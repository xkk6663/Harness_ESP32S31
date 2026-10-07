/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file file_mgr_service.c
 * @brief S4 文件管理服务实现（main 迁移版，设计守约见 .h；源：支线 b590d31）。
 *
 * 线程模型（F44 / page_wav_player 范式）：
 *   files 任务（core 1, prio 4, 内部 RAM 栈 —— 做文件 IO 的任务栈必须内部 RAM，F35）
 *     - 一切阻塞 IO：readdir / unlink / fread / 软解码 / 抽点
 *     - 事件只回调 cb（UI 侧只入队）
 *   LVGL 线程：post_cmd + preview_take + 事件在 lv_timer 里消化
 *
 * ★ main 适配：预览 = esp_new_jpeg 软解码（RGB888 中转 + 抽点直转 RGB565），
 *   不再依赖 camera viewfinder；AVI 播放整线裁掉（.h 有记录）。
 *   解码通道顺序证据同 wallpaper_service.c：esp_new_jpeg RGB888 输出 = R,G,B 序，
 *   直接 rgb565(r,g,b) 打包，无需 swap（overlay_wp_particles.c decode_base 实证）。
 */

#include "file_mgr_service.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>                     /* unlink / read / close */
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_jpeg_dec.h"
#include "middleware/system/sdcard_service.h"
#include "middleware/mem_diag.h"

#define TAG "FMGR"

/* ---- 预览双显示缓冲 640x360（2 x 450,560 = 878 KB PSRAM，开机预分配、永不 free）---- */
static uint16_t *s_pv[2] = {NULL, NULL};

/* ---- 预览解码上限（保守：中转缓冲瞬态分配，超限直接拒） ---- */
#define FM_PREVIEW_MAX_FILE  (2u * 1024u * 1024u)   /* JPEG 文件 ≤ 2MB */
#define FM_PREVIEW_MAX_PX    (1280u * 720u)         /* 像素 ≤ 720p（RGB888 中转 ≤ 2.76MB） */

/* ---- 服务对象 ---- */
static QueueHandle_t s_cmd_q = NULL;
static TaskHandle_t  s_task  = NULL;
static volatile bool s_inited = false;

static volatile file_mgr_evt_cb_t s_cb = NULL;
static void *s_cb_user = NULL;

/* ---- 浏览结果（服务所有；UI 在 BROWSE_DONE 后只读） ---- */
static file_mgr_entry_t s_entries[FM_ENTRY_MAX];
static int   s_entry_count = 0;
static char  s_cur_path[FM_PATH_MAX] = SDCARD_MNT_PATH;

/* ---- 预览发布（双缓冲 + 序号，模式同 viewfinder） ---- */
static volatile uint32_t s_pv_idx = 0;
static volatile uint32_t s_pv_seq = 0;
static uint32_t s_seen_seq = 0;                     /* LVGL 侧 */

/* ---------------------------------------------------------------------------
 *  内部工具
 * ------------------------------------------------------------------------- */

static void emit(file_mgr_evt_id_t id, uint32_t a, uint32_t b)
{
    file_mgr_evt_cb_t cb = s_cb;
    if (cb) {
        file_mgr_evt_t e = { .id = id, .a = a, .b = b };
        cb(&e, s_cb_user);
    }
}

/** RGB565 打包（R 高位，LVGL 同排布） */
static inline uint16_t fm_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** 守卫：预览/删除前必须过（main 无相机/取景器共享资源，只查 SD） */
static bool guards_ok(void)
{
    return sdcard_service_is_mounted();
}

/**
 * ★ 支线 §73.11 同款语义，源改为 RGB888（3B/px）：任意尺寸 → ≤640x360 **等比**
 * 缩放 + 居中 letterbox，边沿填黑。照片预览统统保留原比例，绝不改变照片比例。
 * 行寻址用 stride（**字节数**，= sw*3）。
 */
static void sub_scale_rgb888(const uint8_t *src, uint32_t sw, uint32_t sh,
                             uint32_t stride, uint16_t *dst)
{
    if (sw == 0 || sh == 0 || stride < sw * 3u) return;   /* 守卫：坏尺寸宁可不出图 */

    /* 等比：交叉相乘避免浮点。源相对更宽 ⇒ 宽度受限，否则高度受限。 */
    uint32_t cw, ch;                                    /* 内容区（目标内的实图矩形） */
    if ((uint64_t)sw * FM_H >= (uint64_t)sh * FM_W) {
        cw = FM_W;
        ch = (uint32_t)((uint64_t)FM_W * sh / sw);
    } else {
        ch = FM_H;
        cw = (uint32_t)((uint64_t)FM_H * sw / sh);
    }
    uint32_t offx = (FM_W - cw) / 2u;
    uint32_t offy = (FM_H - ch) / 2u;

    for (uint32_t y = 0; y < FM_H; y++) {
        uint16_t *d = dst + y * FM_W;
        if (y < offy || y >= offy + ch) {
            for (uint32_t x = 0; x < FM_W; x++) d[x] = 0x0000;   /* 上下黑边 */
            continue;
        }
        const uint8_t *s = src + (uint32_t)((uint64_t)(y - offy) * sh / ch) * stride;
        for (uint32_t x = 0; x < FM_W; x++) {
            if (x < offx || x >= offx + cw) { d[x] = 0x0000; continue; }   /* 左右黑边 */
            uint32_t sx = (uint32_t)((uint64_t)(x - offx) * sw / cw);
            const uint8_t *q = &s[sx * 3u];
            d[x] = fm_rgb565(q[0], q[1], q[2]);      /* RGB888(R,G,B) → RGB565，无 swap */
        }
    }
}

/**
 * 软解码一张 JPEG → 抽点 → 发布双缓冲（全部跑在 files 任务，阻塞无害）。
 * 中转（jpeg 原文 + RGB888）均为瞬态分配，本函数内释放。
 * @return ESP_OK；NO_MEM/INVALID_SIZE/INVALID_STATE = 解码失败（保持当前画面）。
 */
static esp_err_t decode_file_publish(const char *path)
{
    esp_err_t err = ESP_OK;
    uint8_t *jpeg = NULL;
    uint8_t *rgb = NULL;
    jpeg_dec_handle_t dec = NULL;

    /* 1) 整文件读入（transient） */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t fsize = (size_t)st.st_size;
    if (fsize == 0 || fsize > FM_PREVIEW_MAX_FILE) {
        ESP_LOGW(TAG, "preview %s: size %u out of range", path, (unsigned)fsize);
        return ESP_ERR_INVALID_SIZE;
    }
    jpeg = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!jpeg) jpeg = malloc(fsize);
    if (!jpeg) return ESP_ERR_NO_MEM;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { err = ESP_ERR_NOT_FOUND; goto out; }
    size_t got = 0;
    while (got < fsize) {
        ssize_t r = read(fd, jpeg + got, fsize - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != fsize) { err = ESP_ERR_INVALID_STATE; goto out; }

    /* 2) header parse（尺寸闸） */
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = { .inbuf = jpeg, .inbuf_len = (int)fsize, .outbuf = NULL };
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "bad jpeg header: %s", path);
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (info.width == 0 || info.height == 0 ||
        (uint32_t)info.width * (uint32_t)info.height > FM_PREVIEW_MAX_PX) {
        ESP_LOGW(TAG, "preview %ux%u exceeds %u px limit", (unsigned)info.width,
                 (unsigned)info.height, (unsigned)FM_PREVIEW_MAX_PX);
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    /* 3) 解码到 RGB888 中转 */
    size_t rgb_sz = (size_t)info.width * (size_t)info.height * 3u;
    rgb = heap_caps_malloc(rgb_sz, MALLOC_CAP_SPIRAM);
    if (!rgb) { err = ESP_ERR_NO_MEM; goto out; }
    io.outbuf = rgb;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", path);
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    /* 4) 抽点发布（双缓冲翻转 + 序号） */
    {
        uint32_t idx = s_pv_idx;
        sub_scale_rgb888(rgb, (uint32_t)info.width, (uint32_t)info.height,
                         (uint32_t)info.width * 3u, s_pv[idx]);
        s_pv_idx = idx ^ 1u;
        s_pv_seq++;
    }
    ESP_LOGI(TAG, "preview %s: %ux%u, %u B", path,
             (unsigned)info.width, (unsigned)info.height, (unsigned)fsize);

out:
    if (dec) jpeg_dec_close(dec);
    if (rgb) free(rgb);
    if (jpeg) free(jpeg);
    return err;
}

/* ---------------------------------------------------------------------------
 *  动作实现（全部跑在 files 任务上下文）
 * ------------------------------------------------------------------------- */

static void do_browse(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;
    int n = 0;

    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed", path);
        emit(FME_BROWSE_ERR, ESP_ERR_NOT_FOUND, 0);
        return;
    }

    memset(s_entries, 0, sizeof(s_entries));
    while (n < FM_ENTRY_MAX && (ent = readdir(dir)) != NULL) {
        char full[FM_PATH_MAX + FM_NAME_MAX + 2];
        struct stat st;
        int rl = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (rl <= 0 || rl >= (int)sizeof(full)) continue;
        if (stat(full, &st) != 0) continue;

        /* FATFS 的 d_name 上限 255；超 FM_NAME_MAX-1 的名字截断（strncpy 会触发
         * -Werror=stringop-truncation，显式 memcpy + 补 NUL 语义相同且无告警） */
        size_t nl = strlen(ent->d_name);
        if (nl > FM_NAME_MAX - 1) nl = FM_NAME_MAX - 1;
        memcpy(s_entries[n].name, ent->d_name, nl);
        s_entries[n].name[nl] = '\0';
        s_entries[n].is_dir = S_ISDIR(st.st_mode);
        s_entries[n].size   = (uint32_t)st.st_size;
        n++;
    }
    closedir(dir);

    /* 排序：目录在前，其余按名字典序（≤128 项，插入排序足够） */
    for (int i = 1; i < n; i++) {
        file_mgr_entry_t key = s_entries[i];
        int j = i - 1;
        while (j >= 0 &&
               ((s_entries[j].is_dir == key.is_dir &&
                 strcasecmp(s_entries[j].name, key.name) > 0) ||
                (!s_entries[j].is_dir && key.is_dir))) {
            s_entries[j + 1] = s_entries[j];
            j--;
        }
        s_entries[j + 1] = key;
    }

    s_entry_count = n;
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", path);
    emit(FME_BROWSE_DONE, (uint32_t)n, 0);
}

static void do_delete(const char *path)
{
    esp_err_t err = ESP_OK;

    if (!guards_ok()) {
        /* 未挂卡拒删（UI 已置灰，这里兜底） */
        err = ESP_ERR_INVALID_STATE;
    } else if (unlink(path) != 0) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGI(TAG, "deleted %s", path);
        /* 当前目录少了一项 ⇒ 重列（事件先发，UI 收到 BROWSE_DONE 刷新） */
    }
    emit(FME_DELETE_DONE, (uint32_t)err, 0);
}

static void do_preview(const char *path)
{
    esp_err_t err;

    if (!s_pv[0] || !s_pv[1]) {
        err = ESP_ERR_NO_MEM;           /* 预览缓冲降级态 */
    } else if (!guards_ok()) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = decode_file_publish(path);
    }
    emit(FME_PREVIEW_DONE, (uint32_t)err, 0);
}

/* ---------------------------------------------------------------------------
 *  服务任务
 * ------------------------------------------------------------------------- */

static void files_task(void *arg)
{
    (void)arg;
    file_mgr_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) continue;

        switch (cmd.id) {
        case FMC_BROWSE:
            do_browse(cmd.path);
            break;
        case FMC_DELETE:
            do_delete(cmd.path);
            break;
        case FMC_PREVIEW:
            do_preview(cmd.path);
            break;
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 *  公开 API
 * ------------------------------------------------------------------------- */

esp_err_t file_mgr_init(void)
{
    if (s_inited) return ESP_OK;

    /* 预览双缓冲：分配失败**只降级预览**，浏览/删除照常（服务任务照建）。
     * main 的 PSRAM 比"大头缓冲全在此刻拿"的支线更紧张，不做整服务连坐。 */
    bool pv_ok = true;
    for (int i = 0; i < 2; i++) {
        s_pv[i] = heap_caps_aligned_alloc(64, FM_W * FM_H * 2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_pv[i] == NULL) {
            ESP_LOGE(TAG, "alloc preview disp buf %d FAILED (PSRAM tight?)", i);
            pv_ok = false;
        } else {
            memset(s_pv[i], 0, FM_W * FM_H * 2);
        }
    }

    s_cmd_q = xQueueCreate(4, sizeof(file_mgr_cmd_t));
    if (s_cmd_q == NULL) {
        ESP_LOGE(TAG, "create cmd queue FAILED (internal RAM tight)");
        return ESP_ERR_NO_MEM;
    }

    /* 内部 RAM 栈：本任务做文件 IO（F35 铁律） */
    if (xTaskCreatePinnedToCore(files_task, "fmgr", 4608, NULL,
                                4, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(fmgr) FAILED (internal RAM tight)");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    MEM_STAGE(TAG, "file_mgr ready");
    ESP_LOGI(TAG, "init ok: preview disp %ux%u x2 (%u KB PSRAM, %s)",
             (unsigned)FM_W, (unsigned)FM_H,
             (unsigned)(2u * FM_W * FM_H * 2u / 1024u),
             pv_ok ? "ok" : "degraded (preview off)");
    return ESP_OK;
}

void file_mgr_set_evt_cb(file_mgr_evt_cb_t cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
}

esp_err_t file_mgr_post_cmd(const file_mgr_cmd_t *cmd)
{
    if (s_cmd_q == NULL || cmd == NULL) return ESP_ERR_INVALID_STATE;
    return (xQueueSend(s_cmd_q, cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void file_mgr_get_entries(const file_mgr_entry_t **entries, int *count)
{
    if (entries) *entries = s_entries;
    if (count)   *count   = s_entry_count;
}

const char *file_mgr_get_cur_path(void)
{
    return s_cur_path;
}

bool file_mgr_preview_take(file_mgr_frame_t *out)
{
    uint32_t seq = s_pv_seq;
    if (seq == 0 || seq == s_seen_seq) return false;
    if (out == NULL) { s_seen_seq = seq; return true; }

    out->buf = s_pv[s_pv_idx ^ 1u];     /* 最新发布的那块（idx 已翻转为"下一块"） */
    out->w   = FM_W;
    out->h   = FM_H;
    out->seq = seq;
    s_seen_seq = seq;
    return true;
}
