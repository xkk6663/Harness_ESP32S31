/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wallpaper_service.c
 * @brief SD-card wallpaper pool + 10-min random rotation + NVS persistence
 *        + double-buffered JPEG decode. Design contract: see wallpaper_service.h.
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
#include "middleware/periph/fs_service.h"       /* FS_MNT_PATH + file_buffer（buffer0 借用方） */
#include "middleware/periph/sdcard_service.h"   /* SDCARD_MNT_PATH + is_mounted */
#include "middleware/mem_diag.h"

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

/* 壁纸必须整屏（用户已拍板统一做 800x480）；非整屏 JPEG 直接拒收，防花屏 */
#define WP_W    ((uint16_t)BSP_LCD_H_RES)
#define WP_H    ((uint16_t)BSP_LCD_V_RES)

/* ================= 状态 ================= */

static uint8_t *s_buf[2];           /* [0]=file_buffer 借用, [1]=work buffer */
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

/* ================= 解码（RGB888 → 指定 buffer） ================= */

/**
 * 解码一张 800x480 JPEG 到 out（必须 ≥ WP_W*WP_H*3）。
 * 输出做 R/B 交换（esp_jpeg_dec RGB888 输出实为 BGR 序，LVGL 按 RGB 读）。
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

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = {
        .inbuf = jpeg_buf,
        .inbuf_len = (int)fsize,
        .outbuf = out,
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
            ESP_LOGE(TAG, "decoder wants %d > buffer %u", out_len, (unsigned)need);
            goto cleanup;
        }
    }
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", path);
        goto cleanup;
    }
    {
        size_t pixels = (size_t)info.width * info.height;
        uint8_t *p = out;
        for (size_t i = 0; i < pixels; i++) {
            uint8_t t = p[0];
            p[0] = p[2];
            p[2] = t;
            p += 3;
        }
    }
    ok = true;
    ESP_LOGI(TAG, "decoded %ux%u <- %s", (unsigned)info.width,
             (unsigned)info.height, path);

cleanup:
    if (dec) {
        jpeg_dec_close(dec);
    }
    free(jpeg_buf);
    return ok;
}

/* ================= SD 池扫描 + 随机挑选 ================= */

typedef struct {
    char name[WP_NAME_MAX];
} wp_name_t;

/**
 * 扫描 /sdcard/wallpaper 下的 .jpg/.jpeg（大小写不敏感），文件名收进 list。
 * 每轮轮换都重扫：二期网页上传的新图无需重启即可进池。
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
         *   · 收到 SET 请求（手动切换）→ 立即解码指定图；
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

/** 双 buffer 先统一刷成深蓝（10,20,40，与粒子层 fallback 同色）——
 *  任何异常路径下 LVGL 读到的都是合法像素，绝不出现未初始化垃圾。 */
static void buffers_fill_fallback(void)
{
    for (int i = 0; i < 2; i++) {
        uint8_t *b = s_buf[i];
        if (!b) {
            continue;
        }
        uint8_t *p = b;
        size_t pixels = (size_t)WP_W * WP_H;
        for (size_t j = 0; j < pixels; j++) {
            p[0] = 10;
            p[1] = 20;
            p[2] = 40;
            p += 3;
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
    s_buf[WP_BUF_0] = file_buffer;

    /* work buffer：与 buffer[0] 同尺寸（w*h*3，见 file_buffer 语义）。
     * ★ 分配失败不致命：s_buf[1]=NULL → 轮换停用，静态壁纸照常（保守降级）。 */
    s_buf[WP_BUF_1] = heap_caps_malloc(file_buffer_size, MALLOC_CAP_SPIRAM);
    if (!s_buf[WP_BUF_1]) {
        MEM_WARN_DIAG(TAG, "wp work buffer alloc failed (degrade: rotation off)",
                      file_buffer_size);
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
