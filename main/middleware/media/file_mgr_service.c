/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file file_mgr_service.c
 * @brief S4 文件管理服务实现（设计守约见 .h 与 docs/开发文档/S4_文件管理页方案.md）。
 *
 * 线程模型（F44 / page_wav_player 范式）：
 *   files 任务（core 1, prio 4, 内部 RAM 栈 —— 做文件 IO 的任务栈必须内部 RAM，F35）
 *     - 一切阻塞 IO：readdir / unlink / fread / 解码 / 抽点
 *     - 事件只回调 cb（UI 侧只入队）
 *   LVGL 线程：post_cmd + preview_take + 事件在 lv_timer 里消化
 *
 * 播放节拍：帧间隔 = 1e6*100/fps_x100 us（strh rate/scale 口径，muxer 写实测平均帧率）。
 * 等下一帧用 xQueueReceive 的 timeout —— 顺便就是 STOP/PAUSE 的响应通道，
 * 永远不会出现"按了停止要等一帧才生效"以外的延迟。
 */

#include "file_mgr_service.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>                     /* unlink */
#include <dirent.h>
#include <sys/stat.h>

#include "middleware/periph/sdcard_service.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/capture_service.h"
#include "middleware/camera/viewfinder.h"
#include "middleware/media/record_video_service.h"
#include "middleware/media/avi_mjpeg_demuxer.h"

#define TAG "FMGR"

/* ---- 预览/播放双显示缓冲 640x360（2 x 460,800 = 921,600 B PSRAM）----
 * ★ 2026-09-28：viewfinder 的 2.93 MB 已改为按需（acquire/release），但**本缓冲仍是
 *   开机常驻**（file_mgr_init 分配，全文件无 free 路径）。它是"下一轮按需化"的候选。
 *   本轮不动它的两个理由：
 *     ① 它只有 0.92 MB，而 viewfinder 释放出的 2.93 MB 已足够让 dl 模型跑起来；
 *     ② 释放它必须连带处理 s_pv_canvas 的**零拷贝**指针（tab_files 的画布连占位缓冲
 *        都没设过），改动面与 UAF 风险都明显高于本轮目标。 */
static uint16_t *s_pv[2] = {NULL, NULL};

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

/* ---- 播放/预览运行态 ---- */
typedef enum {
    FMS_IDLE = 0,
    FMS_PLAYING,
    FMS_PAUSED,
} file_mgr_run_t;
static volatile file_mgr_run_t s_run = FMS_IDLE;    /* 只在本任务上下文改写 */

/* ---- 预览/播放发布（双缓冲 + 序号，模式同 viewfinder） ---- */
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

/** 守卫：预览/播放/删除前必须全过（UI 不是唯一防线，S4 方案 §5） */
static bool guards_ok(bool need_idle_media)
{
    if (!sdcard_service_is_mounted())   return false;
    if (need_idle_media) {
        if (camera_service_owner() == CAM_OWNER_MEDIA)            return false;
        if (record_video_service_state() != VIDEO_REC_IDLE)       return false;
        if (capture_service_state() != CAPTURE_IDLE)              return false;
        if (viewfinder_is_enabled())                              return false; /* 共享缓冲归取景器 */
    }
    return true;
}

/**
 * ★ 2026-09-28（§73.11 修正）：任意尺寸 → ≤640x360 **等比**缩放 + 居中 letterbox。
 *  成因一（旧 step2）：720p 专用行寻址，尺寸一变输出错位。
 *  成因二（本函数第一版）：直接把源**拉伸**到 FM_W×FM_H —— 240x240 的方形
 *  打卡照被压成 640x360 的扁矩形（用户报"打卡照片预览是扁的"）。
 *
 *  ★ 新纪律（用户明确要求）：**照片预览统统保留原比例，绝不改变照片比例**。
 *    做法：scale = min(FM_W/sw, FM_H/sh)，缩放结果居中放入目标缓冲，
 *    剩余边缘填黑（letterbox）。宽高比与源严格一致 —— 拉伸只来自显示端
 *    的画布尺寸，与数据无关。
 *  ★ 720p（1280x720 → 640x360）恰好等比 0.5，无黑边，行为与旧版完全一致。
 *  ★ 行寻址用 **stride**（补齐时 > 可见宽），列范围用可见宽（补齐列不进画面）。
 */
static void sub_scale_any(const uint16_t *src, uint32_t sw, uint32_t sh,
                          uint32_t stride, uint16_t *dst)
{
    if (sw == 0 || sh == 0 || stride < sw) return;      /* 守卫：坏尺寸宁可不出图 */

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
        uint32_t sy = (uint32_t)((uint64_t)(y - offy) * sh / ch);
        const uint16_t *s = src + sy * stride;
        for (uint32_t x = 0; x < FM_W; x++) {
            if (x < offx || x >= offx + cw) { d[x] = 0x0000; continue; }   /* 左右黑边 */
            uint32_t sx = (uint32_t)((uint64_t)(x - offx) * sw / cw);
            d[x] = s[sx];
        }
    }
}

/**
 * 码流已在 stage 里，解码 → 抽点 → 发布双缓冲。
 * @param stage 调用方从 viewfinder 借的 staging（本次 decode 的输入）
 * @param len   JPEG 字节数
 * @return ESP_OK；其余见 viewfinder_decode_jpeg_mem
 * ★ 2026-09-28：宽高/步长来自解码实测 —— 预览照片（240x240 打卡照）与
 *   播放录像帧（720p）走同一条路，不再假设 720p。
 */
static esp_err_t decode_frame_publish(const uint8_t *stage, size_t len)
{
    esp_err_t err = ESP_OK;
    uint16_t dw = 0, dh = 0, stride = 0;
    const uint16_t *full = viewfinder_decode_jpeg_mem(stage, len, &dw, &dh, &stride, &err);
    if (full == NULL) return err;

    uint32_t idx = s_pv_idx;
    sub_scale_any(full, dw, dh, stride, s_pv[idx]);
    s_pv_idx = idx ^ 1u;
    s_pv_seq++;
    return ESP_OK;
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

    if (!guards_ok(true)) {
        /* 录像/拍照进行中或未挂卡 —— 拒删（UI 已置灰，这里兜底） */
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
    FILE *f = NULL;
    size_t stage_cap = 0;
    uint8_t *stage = viewfinder_borrow_stage(&stage_cap);
    esp_err_t err = ESP_OK;

    if (!guards_ok(true))                  { err = ESP_ERR_INVALID_STATE; goto out; }
    if (stage == NULL) {
        /* ★★ 2026-09-28 按需化：staging 为 NULL 的唯一成因是**调用方没有在 post 命令
         *    之前 `viewfinder_acquire()`**（那一步由 tab_files 的 open_path() 在 LVGL
         *    线程负责）。必须明确报出来 —— 否则 UI 只显示一句 "Preview failed"，
         *    排查时看不出根因（viewfinder_borrow_stage 是纯 getter，不触发惰性 init，
         *    一旦漏了 acquire 就会永久失败且毫无线索）。 */
        ESP_LOGE(TAG, "preview: viewfinder staging NULL - caller must call "
                      "viewfinder_acquire() (in LVGL thread) BEFORE posting FMC_PREVIEW");
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    f = fopen(path, "rb");
    if (f == NULL) { err = ESP_ERR_NOT_FOUND; goto out; }

    /* 照片 ≤ 驱动单缓冲上限（921,600 B）—— 超过即不支持（Q1 极限帧也就这个量级） */
    size_t n = fread(stage, 1, stage_cap, f);
    if (n == 0) { err = ESP_ERR_INVALID_SIZE; goto out; }
    if (n == stage_cap) {
        /* 没读到 EOF ⇒ 文件比 staging 还大，v1 不支持 */
        ESP_LOGW(TAG, "photo %s >= stage %u, too big", path, (unsigned)stage_cap);
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    err = decode_frame_publish(stage, n);
    ESP_LOGI(TAG, "preview %s: %u B, %s", path, (unsigned)n, esp_err_to_name(err));

out:
    if (f) fclose(f);
    emit(FME_PREVIEW_DONE, (uint32_t)err, 0);
}

static void do_play(const char *path)
{
    avi_demux_t *dm = NULL;
    size_t stage_cap = 0;
    uint8_t *stage = viewfinder_borrow_stage(&stage_cap);
    uint32_t total = 0, fps_x100 = 0, w = 0, h = 0;
    int64_t next_us;
    uint32_t played = 0;
    int stop_reason = 0;                    /* 0=播完 1=停止 2=出错 */

    if (!guards_ok(true)) { emit(FME_PLAY_DONE, 2, 0); return; }
    if (stage == NULL) {
        /* 同 do_preview：根因是"调用方忘了 acquire"，不是真的内存不足 —— 明确报出来。 */
        ESP_LOGE(TAG, "play: viewfinder staging NULL - caller must call "
                      "viewfinder_acquire() (in LVGL thread) BEFORE posting FMC_PLAY");
        emit(FME_PLAY_DONE, 2, 0);
        return;
    }

    dm = avi_demux_open(path);
    if (dm == NULL) {
        /* 诊断分流：文件不存在 / 文件过小 vs 头部格式校验失败（两类错修法不同） */
        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "avi not found: %s", path);
        } else if (st.st_size < 224) {
            ESP_LOGW(TAG, "avi too small (%u B): %s", (unsigned)st.st_size, path);
        } else {
            ESP_LOGW(TAG, "avi header format mismatch: %s", path);
        }
        emit(FME_PLAY_DONE, 2, 0);
        return;
    }
    avi_demux_get_info(dm, &fps_x100, &total, &w, &h);
    if (w != VF_SRC_W || h != VF_SRC_H) {
        /* 对账：本工程只录 720p；别的规格 v1 不播（宁可拒绝不错乱） */
        ESP_LOGW(TAG, "avi %ux%u != 720p, refuse", (unsigned)w, (unsigned)h);
        avi_demux_close(dm);
        emit(FME_PLAY_DONE, 2, 0);
        return;
    }

    s_run = FMS_PLAYING;
    emit(FME_PLAY_STARTED, total, fps_x100);
    ESP_LOGI(TAG, "play %s: %u frames @ %u.%02u fps", path,
             (unsigned)total, (unsigned)(fps_x100 / 100u), (unsigned)(fps_x100 % 100u));

    next_us = esp_timer_get_time() + 100000;   /* 首帧前留 100 ms 给 UI 切画面 */

    while (s_run != FMS_IDLE) {
        /* ---- 等"下一帧时刻"或命令（STOP/PAUSE 走同一通道，零额外延迟） ---- */
        int64_t now = esp_timer_get_time();
        TickType_t wait = (next_us > now)
                        ? pdMS_TO_TICKS((next_us - now) / 1000u + 1)
                        : 0;
        file_mgr_cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, wait) == pdPASS) {
            if (cmd.id == FMC_STOP)        { stop_reason = 1; break; }
            if (cmd.id == FMC_PAUSE)       { s_run = (s_run == FMS_PLAYING) ? FMS_PAUSED
                                                                            : FMS_PLAYING; }
            /* 播放中其它命令一律忽略（UI 在播放态本来就禁用） */
            continue;   /* 命令处理不消耗帧预算 */
        }
        if (s_run == FMS_PAUSED) { next_us = esp_timer_get_time() + 100000; continue; }

        /* ---- 读一帧 → 解码 → 发布 ---- */
        uint32_t len = 0;
        avid_result_t r = avi_demux_next_frame(dm, stage, (uint32_t)stage_cap, &len);
        if (r == AVID_EOF) break;
        if (r != AVID_OK)  { stop_reason = 2; break; }

        esp_err_t de = decode_frame_publish(stage, len);
        if (de != ESP_OK) { stop_reason = 2; break; }

        played++;
        if ((played % 10u) == 0 || played == 1) emit(FME_PLAY_PROGRESS, played, total);

        /* 节拍：以 strh 的 fps 为准；落后了就追平（不累积延迟） */
        next_us += (int64_t)(100000000ll / (fps_x100 ? fps_x100 : 1200u));
        now = esp_timer_get_time();
        if (next_us < now) next_us = now;   /* 掉帧追平，不快进补帧 */
    }

    avi_demux_close(dm);
    s_run = FMS_IDLE;
    ESP_LOGI(TAG, "play done: %u/%u frames, reason=%d", (unsigned)played, (unsigned)total, stop_reason);
    emit(FME_PLAY_DONE, (uint32_t)stop_reason, played);
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
            if (s_run != FMS_IDLE) { emit(FME_BUSY, ESP_ERR_INVALID_STATE, 0); break; }
            do_preview(cmd.path);
            break;
        case FMC_PLAY:
            if (s_run != FMS_IDLE) { emit(FME_BUSY, ESP_ERR_INVALID_STATE, 0); break; }
            do_play(cmd.path);
            break;
        case FMC_STOP:
        case FMC_PAUSE:
            /* 不在播放时收到 = 无事可做；播放时由 do_play 自己从队列收 */
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

    for (int i = 0; i < 2; i++) {
        s_pv[i] = heap_caps_aligned_alloc(64, FM_W * FM_H * 2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_pv[i] == NULL) {
            ESP_LOGE(TAG, "alloc preview disp buf %d FAILED (PSRAM tight?)", i);
            return ESP_ERR_NO_MEM;
        }
        memset(s_pv[i], 0, FM_W * FM_H * 2);
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
    ESP_LOGI(TAG, "init ok: preview disp %ux%u x2 (%u KB PSRAM)",
             (unsigned)FM_W, (unsigned)FM_H,
             (unsigned)(2u * FM_W * FM_H * 2u / 1024u));
    return ESP_OK;
}

void file_mgr_set_evt_cb(file_mgr_evt_cb_t cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
}

void file_mgr_get_evt_cb(file_mgr_evt_cb_t *cb, void **user)
{
    if (cb)   *cb   = s_cb;
    if (user) *user = s_cb_user;
}

esp_err_t file_mgr_post_cmd(const file_mgr_cmd_t *cmd)
{
    if (s_cmd_q == NULL || cmd == NULL) return ESP_ERR_INVALID_STATE;
    return (xQueueSend(s_cmd_q, cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool file_mgr_is_playing(void)
{
    return (s_run == FMS_PLAYING || s_run == FMS_PAUSED);
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
