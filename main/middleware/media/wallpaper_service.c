/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wallpaper_service.c
 * @brief SD-card wallpaper pool + 10-min random rotation + NVS persistence
 *        + double-buffered JPEG decode. Design contract: see wallpaper_service.h.
 *
 * ★ main 迁移适配记录（源：支线 b590d31 → 本分支，方案 §75 / V2.1）：
 *   1) include：periph/{fs_service,sdcard_service}.h → system/…（main 分层）；
 *   2) 像素格式定稿 **RGB565**（用户拍板：硬件不支持全屏 RGB888 落屏）——
 *      双缓冲 768KB×2（buf0 借 file_buffer 前 768KB，file_buffer 实为 1.15MB）；
 *   3) 解码走 main `overlay_wp_particles.c: decode_base()` 同款**已实机验证**路线：
 *      esp_new_jpeg RGB888 输出（R,G,B 序，esp_jpeg_common.h:48）+ 软件转 RGB565
 *      （LVGL RGB565 = R 在高位，直接 rgb565(r,g,b) 打包，**无需任何 swap**）。
 *      不用 JPEG_PIXEL_FORMAT_RGB565_LE 直出：本仓无先例、组件头未明示 R/B 位排布，
 *      按用户"保守简单"纪律弃用。代价 = 解码期临时 +1.15MB PSRAM（解完即释放）；
 *   4) mem_diag.h 自支线逐字搬运（header-only，不进 SRCS）。
 */

#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "esp_jpeg_dec.h"

#include "wallpaper_service.h"
#include "middleware/system/fs_service.h"       /* FS_MNT_PATH + file_buffer（buffer0 借用方） */
#include "middleware/system/sdcard_service.h"   /* SDCARD_MNT_PATH + is_mounted */
#include "middleware/mem_diag.h"
#include "middleware/bluetooth/link_manager.h"  /* §76：WP_OFFER 分发槽 + 回包 */

#include "cJSON.h"
#include "esp_http_client.h"

static const char *TAG = "WP_SVC";

/* ================= 配置常量 ================= */

#define WP_DIR              WALLPAPER_POOL_DIR            /* 用户壁纸池（800x480 JPEG） */
#define WP_FALLBACK         FS_MNT_PATH"/xwkkk.jpg"       /* littlefs 兜底图（绝不删） */
#define WP_ROTATE_MS        (10UL * 60UL * 1000UL)        /* 10 分钟随机轮换（queue 超时=到点） */
#define WP_MAX_FILES        32                            /* 池容量上限（超出截断并 WARN） */
#define WP_NAME_MAX         64                            /* FATFS 8.3 关闭后文件名上限够用 */
#define WP_NVS_NS           "wp_svc"
#define WP_NVS_KEY_LAST     "last"
#define WP_COMMIT_TIMEOUT_MS 5000                         /* UI 不在场时的兜底等待 */
#define WP_TASK_STACK       4096                          /* 文件 IO 任务栈必须内部 RAM（F35 铁律） */
#define WP_TASK_PRIO        3
#define WP_REQ_Q_DEPTH      2                             /* 手动切换请求队列 */

/* 壁纸必须整屏（用户已拍板统一做 800x480）；非整屏 JPEG 直接拒收，防花屏。
 * 【V2.1】缓冲格式 RGB565 = 2 B/px，共 768,000 B（不再是 RGB888 的 1.15MB）。 */
#define WP_W    ((uint16_t)BSP_LCD_H_RES)
#define WP_H    ((uint16_t)BSP_LCD_V_RES)
#define WP_BPP  2
#define WP_BYTES  ((size_t)WP_W * (size_t)WP_H * (size_t)WP_BPP)

/* 深蓝兜底色（10,20,40，与粒子层 fallback 同色）的 RGB565 打包 */
static inline uint16_t wp_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ================= 状态 ================= */

static uint8_t *s_buf[2];           /* [0]=file_buffer 前 768KB 借用, [1]=work buffer */
static volatile int s_cur;          /* 当前展示 buffer 下标 */
static volatile uint32_t s_gen;     /* 代际号：每次成功轮换 +1 */
static char s_cur_name[WP_NAME_MAX];/* 当前图文件名（池内相对名；兜底图为纯文件名） */

static SemaphoreHandle_t s_commit_sem;
static QueueHandle_t s_req_q;       /* §75 补充 1：手动切换请求（SET） */

/* 手动切换请求（§75 补充 1） */
typedef enum {
    WP_REQ_SET = 1,                 /* 把池内指定名字的图设为当前壁纸 */
} wp_req_type_t;

typedef struct {
    wp_req_type_t type;
    char name[WP_NAME_MAX];
} wp_req_t;

/* ================= 解码（JPEG → 指定 buffer，RGB565 输出） ================= */

/**
 * 解码一张 800x480 JPEG 到 out（必须 ≥ WP_BYTES，RGB565 2B/px）。
 *
 * 通道顺序证据链（照抄 main overlay_wp_particles.c decode_base 的注释结论）：
 *   - esp_new_jpeg `JPEG_PIXEL_FORMAT_RGB888` 输出是 **R,G,B** 依次存放
 *     （esp_jpeg_common.h:48："3-bytes red, green and blue components stored
 *     in memory from low to high address"）；
 *   - LVGL 的 RGB565 位排布 R 在 [15:11]（高位）⇒ 按 q[0],q[1],q[2] 原序入参
 *     rgb565() 打包即可，**不需要任何 R/B 交换**。
 * 非整屏尺寸 → 拒收（用户拍板：池内一律 800x480）。
 */
static bool decode_file_into(const char *path, uint8_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "not a file: %s", path);
        return false;
    }
    size_t fsize = (size_t)st.st_size;
    if (fsize == 0) {
        return false;
    }

    uint8_t *jpeg_buf = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf) {
        jpeg_buf = malloc(fsize);
    }
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "no mem for jpeg buf (%u B): %s", (unsigned)fsize, path);
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(jpeg_buf);
        return false;
    }
    size_t got = 0;
    while (got < fsize) {
        ssize_t r = read(fd, jpeg_buf + got, fsize - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    if (got != fsize) {
        ESP_LOGW(TAG, "short read %u/%u: %s", (unsigned)got, (unsigned)fsize, path);
        free(jpeg_buf);
        return false;
    }

    /* RGB888 中转缓冲（解码期临时，解完即释放，见文件头适配记录 3） */
    uint8_t *rgb = heap_caps_malloc((size_t)WP_W * WP_H * 3, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "no mem for rgb888 stage (%u B)", (unsigned)((size_t)WP_W * WP_H * 3));
        free(jpeg_buf);
        return false;
    }

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = {
        .inbuf = jpeg_buf,
        .inbuf_len = (int)fsize,
        .outbuf = rgb,
    };
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;

    bool ok = false;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        goto cleanup;
    }
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "bad jpeg header: %s", path);
        goto cleanup;
    }
    if (info.width != WP_W || info.height != WP_H) {
        ESP_LOGW(TAG, "reject %ux%u (want %ux%u): %s",
                 (unsigned)info.width, (unsigned)info.height,
                 (unsigned)WP_W, (unsigned)WP_H, path);
        goto cleanup;
    }
    {
        int out_len = 0;
        jpeg_dec_get_outbuf_len(dec, &out_len);
        size_t need = (size_t)WP_W * WP_H * 3;
        if ((size_t)out_len > need) {
            ESP_LOGE(TAG, "decoder wants %d > rgb888 stage %u", out_len, (unsigned)need);
            goto cleanup;
        }
    }
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", path);
        goto cleanup;
    }
    {
        /* RGB888(R,G,B) → RGB565(R 高位)，逐像素软件转换（约 38 万次，任务态几十 ms） */
        const uint8_t *q = rgb;
        uint16_t *dst = (uint16_t *)(void *)out;
        size_t pixels = (size_t)info.width * info.height;
        for (size_t i = 0; i < pixels; i++, q += 3) {
            dst[i] = wp_rgb565(q[0], q[1], q[2]);
        }
    }
    ok = true;
    ESP_LOGI(TAG, "decoded %ux%u -> RGB565 <- %s", (unsigned)info.width,
             (unsigned)info.height, path);

cleanup:
    if (dec) {
        jpeg_dec_close(dec);
    }
    free(rgb);
    free(jpeg_buf);
    return ok;
}

/* ================= SD 池扫描 + 随机挑选 ================= */

typedef struct {
    char name[WP_NAME_MAX];
} wp_name_t;

/**
 * 扫描 /sdcard/wallpaper 下的 .jpg/.jpeg（大小写不敏感），文件名收进 list。
 * 每轮轮换都重扫：PC/FILE 页新落池的图无需重启即可进池。
 * @return 条数（0 = 目录不存在/无图/SD 未挂载）。
 */
static int scan_list(wp_name_t *list, int max)
{
    if (!sdcard_service_is_mounted()) {
        return 0;
    }
    DIR *d = opendir(WP_DIR);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        const char *nm = e->d_name;
        size_t len = strlen(nm);
        if (len == 0 || len >= WP_NAME_MAX) {
            continue;
        }
        if (!strcasecmp(nm + len - 4, ".jpg") && len > 4) {
            /* pass */
        } else if (len > 5 && !strcasecmp(nm + len - 5, ".jpeg")) {
            /* pass */
        } else {
            continue;
        }
        snprintf(list[n].name, WP_NAME_MAX, "%s", nm);
        n++;
    }
    closedir(d);
    if (n == max) {
        /* readdir 已到 EOF 才不会进这里；恰好打满时提示可能截断 */
        ESP_LOGW(TAG, "wallpaper pool hit WP_MAX_FILES=%d, list may be truncated", max);
    }
    return n;
}

/**
 * 随机挑一张（esp_random 硬件随机源），避开 avoid（当前张）。
 * @return 下标；-1 = 没得挑（空池 / 只有当前一张）。
 */
static int pick_random(const wp_name_t *list, int n, const char *avoid)
{
    if (n <= 0) {
        return -1;
    }
    if (n == 1) {
        return (avoid[0] && strcmp(list[0].name, avoid) == 0) ? -1 : 0;
    }
    for (int tries = 0; tries < 8; tries++) {
        int k = (int)(esp_random() % (uint32_t)n);
        if (strcmp(list[k].name, avoid) != 0) {
            return k;
        }
    }
    return -1;
}

/* ================= NVS 断电记忆（write-through） ================= */

/**
 * 换图成功立即落 NVS。拔电没有"优雅退出"钩子，只能 write-through：
 * 10min 一次、一条几十字节，nvs(64KB) 擦写寿命完全无压力。
 */
static void save_last(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(WP_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, last-wallpaper not saved");
        return;
    }
    if (nvs_set_str(h, WP_NVS_KEY_LAST, name) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs write failed");
    }
    nvs_close(h);
}

/** 读上次壁纸名。@return true = 读到且非空。 */
static bool load_last(char *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(WP_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = len;
    bool ok = (nvs_get_str(h, WP_NVS_KEY_LAST, out, &sz) == ESP_OK && out[0] != '\0');
    nvs_close(h);
    return ok;
}

/* ================= 池内挑图 + 解码（轮换核心一步） ================= */

/**
 * 从池中随机挑一张（避开当前）解码到 buffer[idle]。最多试 3 张（坏图跳过）。
 * @return true = 成功（s_cur_name/NVS 由本函数维护）。
 */
static bool decode_random_into(int idle)
{
    wp_name_t *list = heap_caps_malloc(sizeof(wp_name_t) * WP_MAX_FILES,
                                       MALLOC_CAP_SPIRAM);
    if (!list) {
        list = malloc(sizeof(wp_name_t) * WP_MAX_FILES);
    }
    if (!list) {
        ESP_LOGE(TAG, "no mem for file list");
        return false;
    }

    bool ok = false;
    int n = scan_list(list, WP_MAX_FILES);
    if (n <= 0) {
        ESP_LOGI(TAG, "pool empty (sd=%d), keep current", (int)sdcard_service_is_mounted());
        goto out;
    }

    for (int attempt = 0; attempt < 3 && attempt < n; attempt++) {
        int k = pick_random(list, n, s_cur_name);
        if (k < 0) {
            break;      /* 池里只有当前一张 */
        }
        char full[WP_NAME_MAX + sizeof(WP_DIR) + 2];
        snprintf(full, sizeof(full), "%s/%s", WP_DIR, list[k].name);
        if (decode_file_into(full, s_buf[idle])) {
            snprintf(s_cur_name, sizeof(s_cur_name), "%s", list[k].name);
            save_last(s_cur_name);
            ok = true;
            break;
        }
        /* 坏图：继续挑下一张（pick_random 会避开失败名吗？不会——已尽力，
         * 3 次尝试 + 8 次去重足够；整池全坏时保持当前图，绝不黑屏） */
    }

out:
    free(list);
    return ok;
}

/**
 * §75 补充 1：把池内指定名字的图解码到 buffer[idle]（手动切换）。
 * 安全校验：名字必须出现在池列表里（防路径注入——UI 传来的名字虽来自
 * readdir，但 API 是公开的，防御性校验一次）。
 * @return true = 成功（s_cur_name/NVS 已更新）。
 */
static bool decode_named_into(const char *name, int idle)
{
    wp_name_t *list = heap_caps_malloc(sizeof(wp_name_t) * WP_MAX_FILES,
                                       MALLOC_CAP_SPIRAM);
    if (!list) {
        list = malloc(sizeof(wp_name_t) * WP_MAX_FILES);
    }
    if (!list) {
        ESP_LOGE(TAG, "no mem for file list");
        return false;
    }

    bool ok = false;
    int n = scan_list(list, WP_MAX_FILES);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, name) != 0) {
            continue;
        }
        char full[WP_NAME_MAX + sizeof(WP_DIR) + 2];
        snprintf(full, sizeof(full), "%s/%s", WP_DIR, name);
        if (decode_file_into(full, s_buf[idle])) {
            snprintf(s_cur_name, sizeof(s_cur_name), "%s", name);
            save_last(s_cur_name);
            ok = true;
        }
        break;      /* 名字在池内即止（解码失败也不再试别的——手动切换语义） */
    }
    if (n > 0 && !ok) {
        ESP_LOGW(TAG, "set request: '%s' not in pool or decode failed", name);
    }
    free(list);
    return ok;
}

/* ================= 轮换任务 ================= */

static void rotate_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* §75 补充 1：主循环改为 queue 驱动——
         *   · 收到 SET 请求（手动切换 / PC 上传后自动设为当前）→ 立即解码指定图；
         *   · 10min 无请求（超时）→ 随机轮换。
         *   手动事件顺带重置轮换计时（下次轮换从手动切换起算 10min），合理。 */
        wp_req_t req;
        BaseType_t got = xQueueReceive(s_req_q, &req, pdMS_TO_TICKS(WP_ROTATE_MS));

        /* 等上一轮的旧 buffer 被 UI 归还（crossfade 结束）。
         * 5s 超时兜底：UI 不在场（如 splash 已销毁的间隙）也绝不卡死轮换。
         * ★ 语义：计数 1 = "旧 buffer 空闲可覆写"。开机给 1；UI 每次 fade
         *   结束 commit 给 1。超时后 UI 迟到的 commit 只会把计数抬回 1，
         *   等价于"下一轮跳过等待"——此刻 fade 必然早已结束（800ms << 5s），安全。 */
        xSemaphoreTake(s_commit_sem, pdMS_TO_TICKS(WP_COMMIT_TIMEOUT_MS));

        int idle = 1 - s_cur;
        if (!s_buf[idle]) {
            xSemaphoreGive(s_commit_sem);   /* 降级态无解码，把"空闲"还原，防计数漂移 */
            continue;
        }

        bool ok = false;
        if (got == pdTRUE && req.type == WP_REQ_SET) {
            if (strcmp(req.name, s_cur_name) == 0) {
                ESP_LOGI(TAG, "set request: '%s' is already current, skip", req.name);
                xSemaphoreGive(s_commit_sem);
                continue;
            }
            ok = decode_named_into(req.name, idle);
        } else {
            ok = decode_random_into(idle);
        }

        if (ok) {
            int old = s_cur;
            s_cur = idle;           /* 翻转：新图即"当前"（UI 通过 gen 发现） */
            s_gen++;
            ESP_LOGI(TAG, "%s: gen=%u now=buf%d '%s' (old=buf%d fading)",
                     (got == pdTRUE) ? "manual set" : "rotated",
                     (unsigned)s_gen, s_cur, s_cur_name, old);
            /* 成功路径不还信号量：等 UI fade 完 commit（旧 buffer 才真正空闲） */
        } else {
            /* 失败：没有新图、没有 fade，旧 buffer 本来就空闲，计数还原 */
            xSemaphoreGive(s_commit_sem);
        }
    }
}

/* ================= 开机恢复（同步，app_main 串行路径） ================= */

/** 双 buffer 先统一刷成深蓝（RGB565 打包，与粒子层 fallback 同色）——
 *  任何异常路径下 LVGL 读到的都是合法像素，绝不出现未初始化垃圾。 */
static void buffers_fill_fallback(void)
{
    const uint16_t fallback = wp_rgb565(10, 20, 40);
    for (int i = 0; i < 2; i++) {
        uint16_t *p = (uint16_t *)(void *)s_buf[i];
        if (!p) {
            continue;
        }
        size_t pixels = (size_t)WP_W * WP_H;
        for (size_t j = 0; j < pixels; j++) {
            p[j] = fallback;
        }
    }
}

void wallpaper_service_init(void)
{
    /* 依赖闸：file_buffer 必须已就位（fs_service_init 之后调用） */
    if (!file_buffer) {
        ESP_LOGE(TAG, "file_buffer not ready (call after fs_service_init)");
        return;
    }
    /* 借用闸：file_buffer（main 上 = H*V*sizeof(lv_color_t)=1.15MB）必须装得下
     * 全屏 RGB565（768KB）。理论恒成立，防御性校验一次。 */
    if (file_buffer_size < WP_BYTES) {
        ESP_LOGE(TAG, "file_buffer too small: %u < %u",
                 (unsigned)file_buffer_size, (unsigned)WP_BYTES);
        return;
    }
    s_buf[WP_BUF_0] = file_buffer;      /* 只用前 768KB，尾部留白无碍 */

    /* work buffer：全屏 RGB565 = 768,000 B。
     * ★ 分配失败不致命：s_buf[1]=NULL → 轮换停用，静态壁纸照常（保守降级）。 */
    s_buf[WP_BUF_1] = heap_caps_malloc(WP_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_buf[WP_BUF_1]) {
        MEM_WARN_DIAG(TAG, "wp work buffer alloc failed (degrade: rotation off)",
                      WP_BYTES);
    }

    buffers_fill_fallback();
    s_cur = WP_BUF_0;
    s_gen = 0;
    s_cur_name[0] = '\0';

    /* 恢复优先级：NVS 上次壁纸 → 池随机 → littlefs 兜底 → 深蓝（已在 buffer 里） */
    bool restored = false;
    {
        wp_name_t *list = heap_caps_malloc(sizeof(wp_name_t) * WP_MAX_FILES,
                                           MALLOC_CAP_SPIRAM);
        if (!list) {
            list = malloc(sizeof(wp_name_t) * WP_MAX_FILES);
        }
        int n = list ? scan_list(list, WP_MAX_FILES) : 0;
        char last[WP_NAME_MAX] = "";

        if (n > 0 && load_last(last, sizeof(last))) {
            for (int i = 0; i < n; i++) {
                if (strcmp(list[i].name, last) == 0) {
                    char full[(int)sizeof(last) + sizeof(WP_DIR) + 2];
                    snprintf(full, sizeof(full), "%s/%s", WP_DIR, last);
                    if (decode_file_into(full, s_buf[s_cur])) {
                        snprintf(s_cur_name, sizeof(s_cur_name), "%s", last);
                        restored = true;
                        ESP_LOGI(TAG, "restored last wallpaper '%s' (NVS)", last);
                    }
                    break;
                }
            }
        }
        if (!restored && n > 0) {
            int k = pick_random(list, n, s_cur_name);
            if (k >= 0) {
                char full[WP_NAME_MAX + sizeof(WP_DIR) + 2];
                snprintf(full, sizeof(full), "%s/%s", WP_DIR, list[k].name);
                if (decode_file_into(full, s_buf[s_cur])) {
                    snprintf(s_cur_name, sizeof(s_cur_name), "%s", list[k].name);
                    save_last(s_cur_name);
                    restored = true;
                    ESP_LOGI(TAG, "restored random wallpaper '%s'", s_cur_name);
                }
            }
        }
        free(list);
    }
    if (!restored) {
        /* littlefs 兜底图：固件自带，除非文件系统损坏否则必然成功 */
        if (decode_file_into(WP_FALLBACK, s_buf[s_cur])) {
            snprintf(s_cur_name, sizeof(s_cur_name), "xwkkk.jpg");
            ESP_LOGI(TAG, "fallback wallpaper (littlefs)");
        } else {
            ESP_LOGE(TAG, "ALL wallpaper sources failed -> solid dark blue");
        }
    }

    if (s_buf[WP_BUF_1]) {
        s_commit_sem = xSemaphoreCreateBinary();
        s_req_q = xQueueCreate(WP_REQ_Q_DEPTH, sizeof(wp_req_t));
        if (s_commit_sem && s_req_q) {
            xSemaphoreGive(s_commit_sem);   /* 初次轮换前无旧图，直接放行 */
            if (xTaskCreate(rotate_task, "wp_svc", WP_TASK_STACK, NULL,
                            WP_TASK_PRIO, NULL) != pdPASS) {
                ESP_LOGE(TAG, "rotate task create failed -> rotation disabled");
                vSemaphoreDelete(s_commit_sem);
                s_commit_sem = NULL;
                vQueueDelete(s_req_q);
                s_req_q = NULL;
            }
        } else {
            ESP_LOGE(TAG, "sem/queue create failed -> rotation disabled");
            if (s_commit_sem) { vSemaphoreDelete(s_commit_sem); s_commit_sem = NULL; }
            if (s_req_q)      { vQueueDelete(s_req_q);          s_req_q = NULL; }
        }
    }
    MEM_STAGE(TAG, "wallpaper service ready");
    ESP_LOGI(TAG, "ready: cur=buf%d '%s', gen=%u, work=%s",
             s_cur, s_cur_name[0] ? s_cur_name : "(solid)",
             (unsigned)s_gen, s_buf[WP_BUF_1] ? "ok" : "degraded");
}

/* ================= UI 侧 API ================= */

uint8_t *wallpaper_service_get_buffer(int idx)
{
    if (idx != WP_BUF_1) {
        idx = WP_BUF_0;
    }
    return s_buf[idx];
}

int wallpaper_service_get_current(void)
{
    return s_cur;
}

uint32_t wallpaper_service_get_gen(void)
{
    return s_gen;
}

void wallpaper_service_commit(void)
{
    if (s_commit_sem) {
        xSemaphoreGive(s_commit_sem);
    }
}

esp_err_t wallpaper_service_request_set(const char *name)
{
    if (!s_req_q || !s_buf[WP_BUF_1]) {
        return ESP_ERR_INVALID_STATE;   /* 服务未就绪 / 降级态（work buffer 失败） */
    }
    if (!name || name[0] == '\0' || strlen(name) >= WP_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    wp_req_t req = { .type = WP_REQ_SET };
    snprintf(req.name, sizeof(req.name), "%s", name);
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        return ESP_FAIL;                /* 队列满：服务正忙（上一轮还没处理完） */
    }
    ESP_LOGI(TAG, "set request queued: '%s'", name);
    return ESP_OK;
}

void wallpaper_service_get_current_name(char *out, size_t len)
{
    if (out && len > 0) {
        snprintf(out, len, "%s", s_cur_name);
    }
}

esp_err_t wallpaper_service_validate_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 只读头部 64KB：baseline JPEG 的 DQT/SOF 全在这一段，足够 parse_header */
    uint8_t *buf = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, 64 * 1024, f);
    fclose(f);

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    /* 仅 parse header，outbuf/输出格式均不参与，RGB888 占位即可 */
    jpeg_dec_io_t io = { .inbuf = buf, .inbuf_len = (int)n, .outbuf = NULL };
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;

    esp_err_t ret;
    if (jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK &&
        jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK) {
        ret = (info.width == WP_W && info.height == WP_H)
              ? ESP_OK : ESP_ERR_INVALID_SIZE;
    } else {
        ret = ESP_ERR_INVALID_STATE;    /* 不是合法 JPEG */
    }
    if (dec) {
        jpeg_dec_close(dec);
    }
    heap_caps_free(buf);
    return ret;
}

/* ===========================================================================
 *  §76：PC 上传壁纸（设备拉模型）
 *  契约见 .h。分层说明：本文件 include link_manager.h 与 ota_service 的
 *  "network → bluetooth" 先例同理 —— bluetooth 不 include 本文件，方向合法；
 *  回调槽在 link_manager 是多槽（V2.1 起），OTA / WP 各占一槽互不干扰。
 * =========================================================================== */

#define WP_FETCH_TASK_STACK   6144    /* 内部 RAM：下载要写 SD 卡（F35 铁律） */
#define WP_FETCH_TASK_PRIO    3
#define WP_FETCH_Q_DEPTH      1       /* 同一时刻只容忍一个在途上传 */
#define WP_FETCH_URL_MAX      192
#define WP_FETCH_MAX_FILE     (2u * 1024u * 1024u)   /* JPEG 文件上限 2MB */
#define WP_FETCH_CHUNK        8192
#define WP_TMP_NAME           ".wp_tmp"   /* 下载临时名（校验通过后 rename 落池） */

typedef struct {
    char name[WP_NAME_MAX];
    char url[WP_FETCH_URL_MAX];
    uint32_t size;                /* PC 报的文件大小（0 = 未知，仅作对账参考） */
} wp_fetch_req_t;

static QueueHandle_t s_fetch_q;
static uint8_t *s_fetch_chunk;    /* 下载块缓冲（8KB SPIRAM，init 一次） */

/* 回包（UDP 50003 → PC:50003，端口覆写由 link_manager 负责） */
static void wp_result(bool ok, const char *name, const char *errmsg)
{
    char buf[192];
    /* name 已过"纯文件名"校验（无引号/反斜杠），errmsg 全部为本文件固定串 —— 直拼安全 */
    snprintf(buf, sizeof(buf),
             "{\"type\":\"WP_RESULT\",\"ok\":%d,\"name\":\"%s\",\"err\":\"%s\"}",
             ok ? 1 : 0, name ? name : "", errmsg ? errmsg : "");
    link_manager_send_json(buf);
}

/** link_task 上下文：只允许 cJSON 解析 + 入队（F44），拒绝路径直接回包 */
static void wp_on_json(const char *json)
{
    if (!s_fetch_q || !json) {
        return;
    }
    if (!strstr(json, "\"type\"") || !strstr(json, "WP_OFFER")) {
        return;                             /* 只认 WP_OFFER（与 ota_on_json 同风格） */
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        wp_result(false, "", "bad json");
        return;
    }
    const cJSON *j_name = cJSON_GetObjectItem(root, "name");
    const cJSON *j_url  = cJSON_GetObjectItem(root, "url");
    const cJSON *j_size = cJSON_GetObjectItem(root, "size");
    const char *name = cJSON_GetStringValue(j_name);
    const char *url  = cJSON_GetStringValue(j_url);
    uint32_t size = cJSON_IsNumber(j_size) ? (uint32_t)cJSON_GetNumberValue(j_size) : 0;

    wp_fetch_req_t req = {0};
    bool valid = (name && url && name[0] && url[0] &&
                  strlen(name) < WP_NAME_MAX &&
                  strlen(url) < WP_FETCH_URL_MAX &&
                  strstr(url, "http") == url);
    if (valid) {
        /* 名字必须是"纯文件名"：拒绝路径注入（服务端池校验的另一道前闸） */
        if (strchr(name, '/') || strchr(name, '\\') || strchr(name, '"')) {
            valid = false;
        }
        /* 后缀必须 .jpg/.jpeg（大小写不敏感）——池扫描只认这两类 */
        size_t nl = strlen(name);
        if (nl > 4 && strcasecmp(name + nl - 4, ".jpg") == 0) {
            /* pass */
        } else if (nl > 5 && strcasecmp(name + nl - 5, ".jpeg") == 0) {
            /* pass */
        } else {
            valid = false;
        }
    }
    if (valid) {
        snprintf(req.name, sizeof(req.name), "%s", name);
        snprintf(req.url, sizeof(req.url), "%s", url);
        req.size = size;
    }
    cJSON_Delete(root);

    if (!valid) {
        ESP_LOGW(TAG, "[WP] offer rejected: bad name/url");
        wp_result(false, name ? name : "", "bad offer");
        return;
    }
    if (xQueueSend(s_fetch_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[WP] fetch queue full -> busy");
        wp_result(false, req.name, "busy");
    }
}

/** 下载 → 校验 → 落池 → 设为当前（wp_fetch 任务上下文，阻塞无害） */
static void do_fetch(const wp_fetch_req_t *req)
{
    char tmp[WP_NAME_MAX + sizeof(WP_DIR) + 2];
    char final[WP_NAME_MAX + sizeof(WP_DIR) + 2];
    snprintf(tmp, sizeof(tmp), "%s/%s", WP_DIR, WP_TMP_NAME);
    snprintf(final, sizeof(final), "%s/%s", WP_DIR, req->name);

    /* 0) 前闸：SD 在位 + 服务非降级（轮换任务可用才谈得上"设为当前"） */
    if (!sdcard_service_is_mounted())      { wp_result(false, req->name, "no sd"); return; }
    if (!s_req_q || !s_buf[WP_BUF_1])      { wp_result(false, req->name, "degraded"); return; }

    /* 1) HTTP GET 流式落 SD（先写临时名） */
    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli)                             { wp_result(false, req->name, "http init"); return; }

    bool ok = false;
    const char *errmsg = "http";
    size_t total = 0;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        if (status != 200) {
            ESP_LOGW(TAG, "[WP] GET status=%d url=%s", status, req->url);
            errmsg = "http status";
        } else {
            FILE *f = fopen(tmp, "wb");
            if (!f) {
                errmsg = "open tmp";
            } else {
                ok = true;
                int n;
                while ((n = esp_http_client_read(cli, (char *)s_fetch_chunk,
                                                 WP_FETCH_CHUNK)) > 0) {
                    total += (size_t)n;
                    if (total > WP_FETCH_MAX_FILE) {    /* 超限硬闸 */
                        ok = false;
                        errmsg = "too big";
                        break;
                    }
                    if (fwrite(s_fetch_chunk, 1, (size_t)n, f) != (size_t)n) {
                        ok = false;
                        errmsg = "sd write";
                        break;
                    }
                }
                if (fclose(f) != 0) { ok = false; errmsg = "sd close"; }
                /* PC 报了 size 就对账（差一字节都算坏——F17 介质回读纪律的发送端） */
                if (ok && req->size != 0 && total != (size_t)req->size) {
                    ok = false;
                    errmsg = "size mismatch";
                }
            }
        }
        esp_http_client_close(cli);
    } else {
        errmsg = "http open";
    }
    esp_http_client_cleanup(cli);

    /* 2) 介质回读对账（F17：fwrite 返回满计数 ≠ 落盘成功） */
    if (ok) {
        struct stat vst;
        long on_card = (stat(tmp, &vst) == 0) ? (long)vst.st_size : -1L;
        if (on_card != (long)total) {
            ok = false;
            errmsg = "verify fail";
        }
    }

    /* 3) 校验（800x480 JPEG 闸）→ 落池 → 设为当前 */
    if (ok) {
        esp_err_t vret = wallpaper_service_validate_file(tmp);
        if (vret != ESP_OK) {
            ok = false;
            errmsg = (vret == ESP_ERR_INVALID_SIZE) ? "not 800x480" : "bad jpeg";
        }
    }
    if (ok) {
        unlink(final);                      /* FATFS rename 不允许覆盖已有目标 */
        if (rename(tmp, final) != 0) {
            ok = false;
            errmsg = "rename";
        }
    }
    if (!ok) {
        unlink(tmp);                        /* 失败清场：不留 .wp_tmp 垃圾 */
        ESP_LOGW(TAG, "[WP] fetch failed: %s (%u B got)", errmsg, (unsigned)total);
        wp_result(false, req->name, errmsg);
        return;
    }

    ESP_LOGI(TAG, "[WP] fetched %s (%u B) -> pool", req->name, (unsigned)total);
    esp_err_t serr = wallpaper_service_request_set(req->name);
    if (serr == ESP_OK) {
        wp_result(true, req->name, "");
    } else {
        /* 图已在池内（下次轮换/手动可选中），只是"立即设为当前"没排上 */
        wp_result(false, req->name,
                  (serr == ESP_ERR_INVALID_STATE) ? "degraded" : "set busy");
    }
}

static void wp_fetch_task(void *arg)
{
    (void)arg;
    wp_fetch_req_t req;
    for (;;) {
        if (xQueueReceive(s_fetch_q, &req, portMAX_DELAY) == pdPASS) {
            do_fetch(&req);
        }
    }
}

void wallpaper_service_fetch_init(void)
{
    if (s_fetch_q) {
        return;                             /* 幂等 */
    }
    s_fetch_chunk = heap_caps_malloc(WP_FETCH_CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_fetch_chunk) {
        ESP_LOGE(TAG, "[WP] fetch chunk alloc failed -> upload disabled");
        return;
    }
    s_fetch_q = xQueueCreate(WP_FETCH_Q_DEPTH, sizeof(wp_fetch_req_t));
    if (!s_fetch_q) {
        ESP_LOGE(TAG, "[WP] fetch queue create failed -> upload disabled");
        return;
    }
    if (xTaskCreate(wp_fetch_task, "wp_fetch", WP_FETCH_TASK_STACK, NULL,
                    WP_FETCH_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "[WP] fetch task create failed -> upload disabled");
        vQueueDelete(s_fetch_q);
        s_fetch_q = NULL;
        return;
    }
    link_manager_add_json_cb(wp_on_json);
    ESP_LOGI(TAG, "[WP] fetch ready (offer via link, data via http)");
}
