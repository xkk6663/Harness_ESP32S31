/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_binfont.c
 * @brief 全 CJK bin 字体加载实现（LittleFS → PSRAM → lv_binfont_create_ex 懒加载字形）。
 *
 * 加载链路：
 *   /littlefs/fonts/lv_font_cjk_28.bin
 *     --(stat + read, 显式 PSRAM)-->  s_file_buf (~1.9MB)
 *     --(lv_binfont_create_ex{ .buffer, .dynamic_glyph_load = true })-->  s_font
 *
 * ★ 为什么必须开 dynamic_glyph_load（字面省 ~1.62MB RAM）：
 *   lv_binfont 的字形其实是"双轴"加载，两件事互不相干：
 *     A 轴 = 字体文件副本是否常驻内存；
 *     B 轴 = 全部字形位图是否在 create 时一次性展开。
 *   `lv_binfont_create_from_buffer()` 只是 `lv_binfont_create_ex()` 的语法糖，
 *   它把 dsc **零初始化** → `dynamic_glyph_load` 默认 false → 于是
 *   `load_glyph()`（lv_binfont_loader.c:650）会 `lv_malloc(glyf 全表展开尺寸)`
 *   再把 21360 个字形位图整块拷进 RAM。
 *   本字体的 glyf 表就有 1,774,476 B，展开后 ≈ 1.71MB —— 而一屏最多用几十个字形，
 *   这 1.7MB 纯属白占（本板 PSRAM 一共 16MB，但 BT 协议栈 + 音频管线 + 双 framebuffer
 *   已经把 PSRAM 压得很紧，见 docs/工程规范/13_内存管理规范）。
 *
 *   改成 `dynamic_glyph_load = true` 后的常驻开销：
 *     · glyph 描述符表  21360 × 12B  ≈ 250KB   （无论哪种模式都省不掉）
 *     · loca 偏移表     21360 × 4B   ≈  85KB   （本字体 index_to_loc_format=1）
 *     · "最大单字形"暂存缓冲         ≈ 200B    （渲染谁读谁，覆盖复用）
 *   净省 ≈ 1.62MB，并且 create 时不再有那一次 1.7MB 的整块 memcpy，
 *   开机 splash 阶段也更短。
 *
 * ★ 为什么 s_file_buf 必须常驻，绝不能 free（旧注释是错的，已作废）：
 *   dynamic_glyph_load=true 时，`lv_binfont_create_ex()` 里的
 *   `file_owned_by_loader` 为真 → LVGL **不会** close 那个 memfs 文件句柄，
 *   而是把它交给 glyph_loader 长期持有；之后每次渲染都按 loca 偏移从
 *   本能缓冲里现读字形位图（见 load_glyph_bitmap_cb，lv_binfont_loader.c:452）。
 *   所以释放 s_file_buf = 悬空指针，必然 HardFault。
 *   旧版本注释里"理论上创建完就能 free 掉源缓冲"的说法只在
 *   dynamic_glyph_load=false 时成立，切勿照做。
 */

#include "ui_binfont.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "middleware/system/fs_service.h"

#if !LV_USE_FS_MEMFS
#error "ui_binfont 依赖 LV_USE_FS_MEMFS：请在 sdkconfig.defaults 打开 \
CONFIG_LV_USE_FS_MEMFS=y 与 CONFIG_LV_FS_MEMFS_LETTER（见 'btsink Phase 5' 段）"
#endif

static const char *TAG = "UI_BINFONT";

/** bin 字体路径（随 storage 分区镜像一起烧录，见 littlefs_content/fonts/） */
#define BINFONT_PATH   FS_MNT_PATH "/fonts/lv_font_cjk_28.bin"

static lv_font_t *s_font     = NULL;   /* 加载好的 LVGL 字体 */
static uint8_t   *s_file_buf = NULL;   /* bin 文件副本，常驻 PSRAM */
static bool       s_tried    = false;  /* 只尝试一次：失败不再重试刷日志 */

lv_font_t *ui_binfont_get(void)
{
    return s_font;
}

void ui_binfont_init(void)
{
    if (s_tried) {
        return;
    }
    s_tried = true;

    struct stat st;
    if (stat(BINFONT_PATH, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "font missing: %s -- CJK will fall back to built-in font", BINFONT_PATH);
        return;
    }

    /* ★ 显式 PSRAM：本字体 ~1.9MB，而此刻内部 RAM 只剩几十 KB
       （见 docs/工程规范/13_内存管理规范）。绝不能落内部。 */
    s_file_buf = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_file_buf) {
        ESP_LOGE(TAG, "PSRAM alloc %d failed (PSRAM free=%u)",
                 (int)st.st_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return;
    }

    int fd = open(BINFONT_PATH, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open failed: %s", BINFONT_PATH);
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
        return;
    }
    ssize_t got = read(fd, s_file_buf, (size_t)st.st_size);
    close(fd);
    if (got != (ssize_t)st.st_size) {
        ESP_LOGE(TAG, "short read: %d/%d", (int)got, (int)st.st_size);
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
        return;
    }

    /* ★ 走 lv_binfont_create_ex + dynamic_glyph_load=true：
       只展开 glyph 描述符 + loca，位图按需现读，省下整块 glyf 展开（≈1.62MB）。
       注意 s_file_buf 因此必须常驻（LVGL 会长期持有 memfs 句柄），见文件头。 */
    const lv_binfont_dsc_t dsc = {
        .path               = NULL,      /* 用 buffer 源，需 LV_USE_FS_MEMFS */
        .buffer             = s_file_buf,
        .buffer_size        = (uint32_t)st.st_size,
        .dynamic_glyph_load = true,
    };
    s_font = lv_binfont_create_ex(&dsc);
    if (!s_font) {
        /* 常见原因：bin 与当前 LVGL 版本的字表格式不匹配，或字表生成参数被改过 */
        ESP_LOGE(TAG, "lv_binfont_create_ex failed (bad bin or version mismatch)");
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
        return;
    }

    ESP_LOGI(TAG, "CJK font ready: %d bytes in PSRAM, dynamic_glyph_load=ON, "
                  "line_h=%d base_line=%d | PSRAM free=%u",
             (int)st.st_size, s_font->line_height, s_font->base_line,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void ui_binfont_deinit(void)
{
    if (s_font) {
        lv_binfont_destroy(s_font);
        s_font = NULL;
    }
    if (s_file_buf) {
        heap_caps_free(s_file_buf);
        s_file_buf = NULL;
    }
    s_tried = false;
}
