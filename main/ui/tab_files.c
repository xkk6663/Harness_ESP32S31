/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_files.c
 * @brief FILES tab：SD 卡文件管理（S4，方案见 docs/开发文档/S4_文件管理页方案.md）。
 *
 * 布局（776x408 内容区，绝对定位，F46 教训同 tab_video.c）：
 *   (0,0)    标题               (顶右) 状态行
 *   (0,42)   文件列表 470x306    (486,42) 信息/操作列 290x306
 *   预览/播放态：列表与操作列隐藏，canvas 640x360 居中 (68,24) + zoom 290
 *   （256=1x ⇒ 1.133x，渲染 725x408 = 等比放大到**全高**铺满，中心=页中心 (388,204)；
 *   播放态底部叠进度条 —— 后创建的对象画在上层）。
 *
 * 交互：
 *   - 列表：单击=选中，再点=打开（目录进 / .jpg 预览 / .avi 播放）；
 *   - UP/REFRESH/OPEN/DELETE 四键；DELETE **两步确认**（变红 CONFIRM? 3 秒超时还原）；
 *   - 预览/播放态 tap 画布退出（播放退出 = STOP，等 PLAY_DONE 回浏览态）。
 *
 * 线程纪律（F44）：本文件只发命令、只消费事件队列；一切 IO 在 file_mgr 服务任务。
 */

#include "lvgl.h"

#include "tab_files.h"
#include "tab_video.h"
#include "ui_theme.h"
#include "middleware/media/file_mgr_service.h"
#include "middleware/audio/media_player_service.h"
#include "middleware/periph/sdcard_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define TAG "TAB_FILES"

/* ---- 前向声明（事件排空/回调先于实现使用） ---- */
static void rebuild_list(void);
static void apply_mode_visibility(void);
static void enter_browse_mode(void);
static void post_browse(const char *path);

/* 拷路径：显式判长 + memcpy。不用 snprintf("%s") —— 目的缓冲比源缓冲小时
 * GCC 的 -Werror=format-truncation 会保守报错（实参上界相加必然超），这里
 * "超长路径直接拒" 语义也更诚实。 */
static bool path_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) return false;
    memcpy(dst, src, n + 1);
    return true;
}

/* ---- 布局常量（页内容区 = 776 x 408）----
 * ★ 前缀 FLIST_：`LIST_H` 是 FreeRTOS freertos/list.h 的 include guard 宏，
 *   同名 #define 会把它之后的 FreeRTOS 头全弄坏（-Werror 实锤过一次）。 */
#define FLIST_X         0
#define FLIST_Y         42
#define FLIST_W         470
#define FLIST_H         306
#define CARD_X          486
#define CARD_Y          42
#define CARD_W          290
#define CARD_H          306
#define PV_X            68          /* 640x360 居中：中心=(388,204)=页中心 */
#define PV_Y            24
#define PV_ZOOM         290         /* 256=1x ⇒ 1.133x：渲染 725x408，等比铺满全高 */

/* ---- 控件句柄 ---- */
static lv_obj_t *s_list;
static lv_obj_t *s_card;
static lv_obj_t *s_path_label;      /* 当前目录（DOT 截断） */
static lv_obj_t *s_sel_label;       /* 选中项信息 */
static lv_obj_t *s_up_btn, *s_refresh_btn, *s_open_btn, *s_del_btn;
static lv_obj_t *s_del_label;
static lv_obj_t *s_title, *s_status;
static lv_obj_t *s_pv_canvas;       /* 预览/播放画布 640x360 */
static lv_obj_t *s_pv_hint;         /* "tap to exit"（画在画布上层） */
static lv_obj_t *s_prog;            /* 播放进度条（画布上层叠加） */
static lv_obj_t *s_mp_stop_btn;     /* MP3 播放态的大 STOP 按钮（画布中心） */
static lv_timer_t *s_timer;         /* 事件排空 30ms */
static lv_timer_t *s_tick_timer;    /* 50ms：帧上画布/倒计时/按钮态（必须可删） */
static QueueHandle_t s_evt_q;

/* ---- 状态 ---- */
static bool     s_active;
static int      s_sel_idx = -1;     /* 选中条目（-1 无） */
static lv_obj_t *s_sel_btn;         /* 选中条目按钮（还原高亮用） */
static bool     s_del_armed;        /* 两步删除：第一下已按 */
static int      s_del_ticks;        /* 确认倒计时（refresh 拍数，50ms/拍） */
static bool     s_pv_shown;         /* 预览/播放首帧已上画布（此前画布不显示） */

typedef enum { MODE_BROWSE = 0, MODE_PREVIEW, MODE_PLAY, MODE_MP3 } files_mode_t;
static files_mode_t s_mode = MODE_BROWSE;

/* ===========================================================================
 *  事件队列（file_mgr 服务任务 → LVGL 线程；page_wav_player 范式）
 * =========================================================================== */
#define EVT_Q_DEPTH 8

static void evt_drain_cb(lv_timer_t *t)
{
    (void)t;
    file_mgr_evt_t e;
    while (s_evt_q && xQueueReceive(s_evt_q, &e, 0) == pdTRUE) {
        switch (e.id) {
        case FME_BROWSE_DONE:
            rebuild_list();
            lv_label_set_text_fmt(s_status, "%u items", (unsigned)e.a);
            lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
            break;
        case FME_BROWSE_ERR:
            lv_label_set_text_static(s_status, "Browse failed (SD card mounted?)");
            lv_obj_set_style_text_color(s_status, COL_RED, 0);
            break;
        case FME_DELETE_DONE:
            s_del_armed = false;
            if (e.a == (uint32_t)ESP_OK) {
                s_sel_idx = -1; s_sel_btn = NULL;
                lv_label_set_text_static(s_status, "Deleted");
                lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
                post_browse(NULL);              /* 重列当前目录 */
            } else {
                lv_label_set_text_static(s_status, "Delete refused (busy?)");
                lv_obj_set_style_text_color(s_status, COL_RED, 0);
            }
            break;
        case FME_PREVIEW_DONE:
            if (e.a == (uint32_t)ESP_OK) {
                /* 画面由 refresh 的 take 路径上画布；这里不动模式 */
            } else {
                lv_label_set_text_static(s_status, "Preview failed (unsupported?)");
                lv_obj_set_style_text_color(s_status, COL_RED, 0);
                enter_browse_mode();
            }
            break;
        case FME_PLAY_STARTED:
            lv_bar_set_range(s_prog, 0, (int32_t)e.a);
            lv_bar_set_value(s_prog, 0, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_status, "Playing %u frames @ %u.%02u fps (no audio)",
                                  (unsigned)e.a, (unsigned)(e.b / 100u), (unsigned)(e.b % 100u));
            lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
            break;
        case FME_PLAY_PROGRESS:
            lv_bar_set_value(s_prog, (int32_t)e.a, LV_ANIM_OFF);
            break;
        case FME_PLAY_DONE:
            enter_browse_mode();
            if (e.a == 2) {
                lv_label_set_text_static(s_status, "Play failed");
                lv_obj_set_style_text_color(s_status, COL_RED, 0);
            }
            break;
        case FME_BUSY:
            lv_label_set_text_static(s_status, "Busy - try again");
            lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
            break;
        default:
            break;
        }
    }
}

/* 服务任务上下文回调：严禁 lv_*，只入队 */
static void on_file_evt(const file_mgr_evt_t *evt, void *user)
{
    (void)user;
    if (s_evt_q) xQueueSend(s_evt_q, evt, 0);
}

/* ===========================================================================
 *  动作（LVGL 线程；只发命令）
 * =========================================================================== */
static void post_browse(const char *path)
{
    file_mgr_cmd_t cmd = { .id = FMC_BROWSE };
    path_copy(cmd.path, sizeof(cmd.path), path ? path : file_mgr_get_cur_path());
    file_mgr_post_cmd(&cmd);
}

static void open_path(const char *name, bool is_dir)
{
    char full[FM_PATH_MAX + FM_NAME_MAX + 2];
    const char *ext = strrchr(name, '.');
    int rl = snprintf(full, sizeof(full), "%s/%s", file_mgr_get_cur_path(), name);
    if (rl <= 0 || rl >= (int)sizeof(full)) {
        lv_label_set_text_static(s_status, "Path too long");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        return;
    }

    if (is_dir) {
        s_sel_idx = -1; s_sel_btn = NULL;
        post_browse(full);
        return;
    }
    if (ext != NULL && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)) {
        s_mode = MODE_PREVIEW;
        s_pv_shown = false;
        apply_mode_visibility();
        lv_label_set_text_static(s_status, "Decoding...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        file_mgr_cmd_t cmd = { .id = FMC_PREVIEW };
        path_copy(cmd.path, sizeof(cmd.path), full);
        file_mgr_post_cmd(&cmd);
        return;
    }
    if (ext != NULL && strcasecmp(ext, ".avi") == 0) {
        s_mode = MODE_PLAY;
        s_pv_shown = false;
        apply_mode_visibility();
        lv_label_set_text_static(s_status, "Opening...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        file_mgr_cmd_t cmd = { .id = FMC_PLAY };
        path_copy(cmd.path, sizeof(cmd.path), full);
        file_mgr_post_cmd(&cmd);
        return;
    }
    /* S5 M2：MP3/WAV 走 media_player_service（音频域，esp_player 链路：
     * MP3 用 mp3 解，WAV 用 wav 容器 + PCM 解；rec_local 录的 16k WAV
     * 由 render proc 链（rate_cvt）重采样到 44.1k 输出）。
     * 与 AVI 正交：互斥由 audio_service ext 协议（REC/小智独占租借）保证。
     * ★ §43 修：原只认 .mp3，rec_local 录的 .wav 落到 "Unsupported"。 */
    if (ext != NULL && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
        s_mode = MODE_MP3;
        s_pv_shown = false;
        apply_mode_visibility();
        lv_label_set_text_static(s_status, "Opening...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        esp_err_t err = media_player_service_play(full);
        if (err != ESP_OK) {
            enter_browse_mode();
            lv_label_set_text_static(s_status,
                (err == ESP_ERR_INVALID_STATE) ? "Audio busy (rec/call/AI?)"
                                               : "Open failed");
            lv_obj_set_style_text_color(s_status, COL_RED, 0);
        } else {
            lv_label_set_text_static(s_status, "Playing MP3");
            lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
        }
        return;
    }
    lv_label_set_text_static(s_status, "Unsupported file type");
    lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
}

/* ===========================================================================
 *  列表
 * =========================================================================== */
static void list_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    /* ★ F41-A：索引存在**对象**的 user_data 里（lv_obj_get_user_data），与事件
     *   注册时的 event user_data 是两个存储 —— 这里必须用对象那一对。 */
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    const file_mgr_entry_t *entries;
    int count;
    file_mgr_get_entries(&entries, &count);
    if (idx < 0 || idx >= count) return;

    if (s_sel_idx != idx) {
        if (s_sel_btn) lv_obj_set_style_bg_color(s_sel_btn, COL_CARD, 0);
        s_sel_idx  = idx;
        s_sel_btn  = btn;
        lv_obj_set_style_bg_color(btn, COL_CYAN, 0);
        lv_label_set_text_fmt(s_sel_label, "%s\n%u KB%s", entries[idx].name,
                              (unsigned)(entries[idx].size / 1024u),
                              entries[idx].is_dir ? "  (dir)" : "");
        return;
    }
    open_path(entries[idx].name, entries[idx].is_dir);      /* 再点同一条 = 打开 */
}

static void rebuild_list(void)
{
    const file_mgr_entry_t *entries;
    int count;
    file_mgr_get_entries(&entries, &count);

    lv_obj_clean(s_list);                       /* 清掉旧条目 */
    s_sel_idx = -1;
    s_sel_btn  = NULL;

    for (int i = 0; i < count; i++) {
        /* ★ §43：音频类（avi/mp3/wav）用 PLAY 图标，其余图片 IMAGE */
        const char *nm = entries[i].name;
        bool is_media = entries[i].is_dir ? false :
                        strstr(nm, ".avi") || strstr(nm, ".AVI") ||
                        strstr(nm, ".mp3") || strstr(nm, ".MP3") ||
                        strstr(nm, ".wav") || strstr(nm, ".WAV");
        const char *sym = entries[i].is_dir ? LV_SYMBOL_DIRECTORY
                        : is_media ? LV_SYMBOL_PLAY : LV_SYMBOL_IMAGE;
        lv_obj_t *btn = lv_list_add_btn(s_list, sym, entries[i].name);
        lv_obj_set_style_bg_color(btn, COL_CARD, 0);
        lv_obj_set_style_bg_color(btn, COL_CYAN, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, COL_TEXT, 0);
        lv_obj_set_style_text_font(btn, UI_FONT, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, list_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_label_set_text(s_path_label, file_mgr_get_cur_path());
}

/* ===========================================================================
 *  模式切换（浏览 / 预览 / 播放）
 * =========================================================================== */
static void apply_mode_visibility(void)
{
    bool browse = (s_mode == MODE_BROWSE);
    lv_obj_set_hidden(s_list, !browse);
    lv_obj_set_hidden(s_card, !browse);
    lv_obj_set_hidden(s_title, !browse);
    /* ★ 播放态**不用 zoom 变换**（256=1x）：SW 软绘 + 1.13x 全画布逐像素变换
     *   实测把显示帧率拖到 ~3 fps（发布链 16.4 fps 全部到位，2026-09-26 日志裁决）。
     *   640x360 原尺寸直绘像素量少 2.4x 且无变换采样。照片是单张，zoom 无所谓。 */
    lv_obj_set_style_transform_zoom(s_pv_canvas,
                                    (s_mode == MODE_PLAY) ? 256 : PV_ZOOM, 0);
    lv_obj_set_hidden(s_pv_canvas, browse || !s_pv_shown);
    lv_obj_set_hidden(s_pv_hint, browse || !s_pv_shown);
    lv_obj_set_hidden(s_prog, s_mode != MODE_PLAY || !s_pv_shown);
    /* MP3 态：无视频帧，画布保持隐藏，中央给一个大 STOP 按钮 */
    lv_obj_set_hidden(s_mp_stop_btn, s_mode != MODE_MP3);
}

/* MP3 播放态的退出（画布中央 STOP 按钮） */
static void mp_stop_btn_cb(lv_event_t *e)
{
    (void)e;
    media_player_service_stop();
    /* 模式回切在 refresh 轮询里做（is_playing()==false → 回浏览态） */
}

static void enter_browse_mode(void)
{
    if (s_mode == MODE_MP3) {
        media_player_service_stop();    /* 离开 MP3 态必停（幂等） */
    }
    s_mode = MODE_BROWSE;
    s_pv_shown = false;
    apply_mode_visibility();
    lv_label_set_text_static(s_status, "Browse");
    lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
}

static void pv_canvas_cb(lv_event_t *e)
{
    (void)e;
    if (s_mode == MODE_PLAY) {
        file_mgr_cmd_t cmd = { .id = FMC_STOP };
        file_mgr_post_cmd(&cmd);
        /* 模式在 FME_PLAY_DONE 里回浏览态（播放任务退出后） */
    } else if (s_mode == MODE_PREVIEW) {
        enter_browse_mode();
    }
}

/* ===========================================================================
 *  按钮回调
 * =========================================================================== */
static void up_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *cur = file_mgr_get_cur_path();
    if (strcmp(cur, SDCARD_MNT_PATH) == 0) {
        lv_label_set_text_static(s_status, "Already at SD root");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        return;
    }
    char parent[FM_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", cur);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0';
    else if (slash == parent)     slash[1] = '\0';      /* 根目录形态 */
    post_browse(parent);
}

static void refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    post_browse(NULL);
}

static void open_btn_cb(lv_event_t *e)
{
    (void)e;
    const file_mgr_entry_t *entries;
    int count;
    file_mgr_get_entries(&entries, &count);
    if (s_sel_idx < 0 || s_sel_idx >= count) {
        lv_label_set_text_static(s_status, "Select a file first");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        return;
    }
    open_path(entries[s_sel_idx].name, entries[s_sel_idx].is_dir);
}

static void del_btn_cb(lv_event_t *e)
{
    (void)e;
    const file_mgr_entry_t *entries;
    int count;
    file_mgr_get_entries(&entries, &count);

    if (!s_del_armed) {
        if (s_sel_idx < 0 || s_sel_idx >= count) {
            lv_label_set_text_static(s_status, "Select a file first");
            lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
            return;
        }
        s_del_armed = true;
        s_del_ticks = 60;                   /* 3 s 不确认自动还原 */
        lv_label_set_text_static(s_del_label, LV_SYMBOL_WARNING " SURE?");
        lv_obj_set_style_bg_color(s_del_btn, COL_RED, 0);
        return;
    }
    /* 第二下：真删 */
    s_del_armed = false;
    lv_label_set_text_static(s_del_label, "DELETE");
    lv_obj_set_style_bg_color(s_del_btn, COL_BORDER, 0);
    if (s_sel_idx < 0 || s_sel_idx >= count) return;

    char full[FM_PATH_MAX + FM_NAME_MAX + 2];
    int rl = snprintf(full, sizeof(full), "%s/%s",
                      file_mgr_get_cur_path(), entries[s_sel_idx].name);
    if (rl <= 0 || rl >= (int)sizeof(full)) return;
    file_mgr_cmd_t cmd = { .id = FMC_DELETE };
    path_copy(cmd.path, sizeof(cmd.path), full);
    file_mgr_post_cmd(&cmd);
}

/* ===========================================================================
 *  refresh（50 ms）：状态行/按钮可用性唯一写入点 + 预览帧上画布
 * =========================================================================== */
static void refresh(lv_timer_t *t)
{
    (void)t;

    /* ---- 预览/播放帧上画布（有新帧才动） ---- */
    if (s_mode != MODE_BROWSE) {
        if (s_mode == MODE_MP3) {
            /* MP3：无帧上画布；播完（FINISHED/ERROR 后 is_playing=false）自动回浏览 */
            if (!media_player_service_is_playing()) {
                enter_browse_mode();
            }
        } else {
        file_mgr_frame_t f;
        if (file_mgr_preview_take(&f)) {
            lv_canvas_set_buffer(s_pv_canvas, (void *)f.buf, (int32_t)f.w, (int32_t)f.h,
                                 LV_COLOR_FORMAT_RGB565);
            if (!s_pv_shown) {
                s_pv_shown = true;
                apply_mode_visibility();
            }
            lv_obj_invalidate(s_pv_canvas);
        }
        if (s_mode == MODE_PLAY && file_mgr_is_playing()) {
            lv_label_set_text_static(s_status, "Playing - tap picture to stop");
        }
        }
    }

    /* ---- 两步删除确认倒计时 ---- */
    if (s_del_armed && --s_del_ticks <= 0) {
        s_del_armed = false;
        lv_label_set_text_static(s_del_label, "DELETE");
        lv_obj_set_style_bg_color(s_del_btn, COL_BORDER, 0);
    }

    /* ---- 播放态按钮禁用（浏览动作在播放时无意义） ---- */
    if (file_mgr_is_playing() || media_player_service_is_playing()) {
        lv_obj_add_state(s_up_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_open_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_del_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_up_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s_open_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s_del_btn, LV_STATE_DISABLED);
    }
}

/* ===========================================================================
 *  建 / 拆
 * =========================================================================== */
void tab_files_build(lv_obj_t *screen, lv_group_t *group)
{
    (void)group;

    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    s_title = lv_label_create(screen);
    lv_label_set_text_static(s_title, LV_SYMBOL_LIST "  FILES");
    lv_obj_set_pos(s_title, 0, 0);
    lv_obj_set_style_text_color(s_title, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_title, UI_FONT, 0);

    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Idle");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    /* ---- 文件列表 ---- */
    s_list = lv_list_create(screen);
    lv_obj_set_pos(s_list, FLIST_X, FLIST_Y);
    lv_obj_set_size(s_list, FLIST_W, FLIST_H);
    lv_obj_set_style_bg_color(s_list, COL_BG, 0);
    lv_obj_set_style_border_color(s_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 8, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);

    /* ---- 信息/操作列 ---- */
    s_card = lv_obj_create(screen);
    lv_obj_set_pos(s_card, CARD_X, CARD_Y);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(s_card, COL_CARD, 0);
    lv_obj_set_style_border_color(s_card, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_radius(s_card, 16, 0);
    lv_obj_set_style_pad_all(s_card, 10, 0);
    lv_obj_set_scrollable(s_card, false);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_card, 8, 0);

    s_path_label = lv_label_create(s_card);
    lv_label_set_text_static(s_path_label, "/sdcard");
    lv_obj_set_width(s_path_label, CARD_W - 20);
    lv_label_set_long_mode(s_path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_path_label, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_path_label, UI_FONT, 0);

    s_sel_label = lv_label_create(s_card);
    lv_label_set_text_static(s_sel_label, "-");
    lv_obj_set_width(s_sel_label, CARD_W - 20);
    lv_label_set_long_mode(s_sel_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_sel_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_sel_label, UI_FONT, 0);

    /* 2x2 按钮阵：UP REFRESH / OPEN DELETE */
    static const char *labels[] = { "UP", "REFRESH", "OPEN", "DELETE" };
    static lv_event_cb_t cbs[] = { up_btn_cb, refresh_btn_cb, open_btn_cb, del_btn_cb };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(s_card);
        lv_obj_set_size(btn, (CARD_W - 20 - 8) / 2, 44);
        lv_obj_set_style_radius(btn, 22, 0);
        lv_obj_set_style_bg_color(btn, (i == 3) ? COL_BORDER : COL_CYAN, 0);
        lv_obj_add_event_cb(btn, cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text_static(lb, labels[i]);
        lv_obj_set_style_text_color(lb, COL_DARK, 0);
        lv_obj_set_style_text_font(lb, UI_FONT, 0);
        lv_obj_center(lb);
        switch (i) {
        case 0: s_up_btn      = btn; break;
        case 1: s_refresh_btn = btn; break;
        case 2: s_open_btn    = btn; break;
        case 3: s_del_btn     = btn; s_del_label = lb; break;
        }
    }

    /* ---- 预览/播放画布（浏览态隐藏；先建，进度条/hint 后建=画在上层） ---- */
    s_pv_canvas = lv_canvas_create(screen);
    lv_obj_set_pos(s_pv_canvas, PV_X, PV_Y);
    lv_obj_set_hidden(s_pv_canvas, true);
    lv_obj_set_clickable(s_pv_canvas, true);
    lv_obj_add_event_cb(s_pv_canvas, pv_canvas_cb, LV_EVENT_CLICKED, NULL);
    /* ★ 等比放大到全高：360 * (290/256) ≈ 408 = 内容区高；中心=页中心（见文件头） */
    lv_obj_set_style_transform_zoom(s_pv_canvas, PV_ZOOM, 0);
    lv_obj_set_style_transform_pivot_x(s_pv_canvas, FM_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_pv_canvas, FM_H / 2, 0);

    s_pv_hint = lv_label_create(screen);
    lv_label_set_text_static(s_pv_hint, "tap to exit");
    lv_obj_align(s_pv_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_pv_hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_pv_hint, UI_FONT, 0);
    lv_obj_set_hidden(s_pv_hint, true);

    s_prog = lv_bar_create(screen);
    lv_obj_set_size(s_prog, 400, 12);
    lv_obj_align(s_prog, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(s_prog, COL_CARD, 0);
    lv_obj_set_style_bg_color(s_prog, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_prog, 0, 0);
    lv_bar_set_range(s_prog, 0, 100);
    lv_obj_set_hidden(s_prog, true);

    /* ---- MP3 播放态的大 STOP 按钮（画布中心；仅 MODE_MP3 显示） ---- */
    s_mp_stop_btn = lv_btn_create(screen);
    lv_obj_set_size(s_mp_stop_btn, 200, 72);
    lv_obj_set_pos(s_mp_stop_btn, PV_X + (640 - 200) / 2, PV_Y + (360 - 72) / 2);
    lv_obj_set_style_radius(s_mp_stop_btn, 36, 0);
    lv_obj_set_style_bg_color(s_mp_stop_btn, COL_RED, 0);
    lv_obj_set_style_border_width(s_mp_stop_btn, 0, 0);
    lv_obj_add_event_cb(s_mp_stop_btn, mp_stop_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mp_lb = lv_label_create(s_mp_stop_btn);
    lv_label_set_text_static(mp_lb, LV_SYMBOL_STOP "  STOP");
    lv_obj_set_style_text_color(mp_lb, COL_DARK, 0);
    lv_obj_set_style_text_font(mp_lb, UI_FONT, 0);
    lv_obj_center(mp_lb);
    lv_obj_set_hidden(s_mp_stop_btn, true);

    /* ---- 事件队列 + 定时器 ---- */
    s_evt_q = xQueueCreate(EVT_Q_DEPTH, sizeof(file_mgr_evt_t));
    s_timer = lv_timer_create(evt_drain_cb, 30, NULL);
    /* 50 ms 拍：帧上画布 / 删除倒计时 / 按钮态（句柄存住，destroy 要删） */
    s_tick_timer = lv_timer_create(refresh, 50, NULL);

    s_active = false;
    s_mode   = MODE_BROWSE;
    s_del_armed = false;
    s_pv_shown  = false;

    file_mgr_set_evt_cb(on_file_evt, NULL);
}

void tab_files_destroy(void)
{
    file_mgr_cmd_t cmd = { .id = FMC_STOP };
    file_mgr_post_cmd(&cmd);                    /* 播放中退出主页面：请求停止 */
    media_player_service_stop();                /* MP3 同理（幂等，未播放无害） */
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_tick_timer) {
        lv_timer_del(s_tick_timer);
        s_tick_timer = NULL;
    }
    if (s_evt_q) {
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
    }
    s_list = NULL; s_card = NULL;
    s_path_label = NULL; s_sel_label = NULL;
    s_up_btn = NULL; s_refresh_btn = NULL; s_open_btn = NULL;
    s_del_btn = NULL; s_del_label = NULL;
    s_title = NULL; s_status = NULL;
    s_pv_canvas = NULL; s_pv_hint = NULL; s_prog = NULL;
    s_sel_btn = NULL; s_sel_idx = -1;
    s_evt_q = NULL;
    s_active = false; s_mode = MODE_BROWSE; s_del_armed = false; s_pv_shown = false;
}

void tab_files_set_active(bool active)
{
    if (active == s_active) return;
    s_active = active;

    if (active) {
        /* ★ 进 FILES 先关 VIDEO 页取景：解码共享缓冲守卫 + 相机守卫都要求它停
         * （tab_video_set_active(false) 只停 PREVIEW 所有流，MEDIA 不受影响）。 */
        tab_video_set_active(false);
        post_browse(NULL);                      /* BROWSE 最近目录（首次=/sdcard） */
        lv_label_set_text_static(s_status, "Loading...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    } else {
        if (file_mgr_is_playing()) {
            file_mgr_cmd_t cmd = { .id = FMC_STOP };
            file_mgr_post_cmd(&cmd);
        }
        if (media_player_service_is_playing()) {
            media_player_service_stop();
        }
        enter_browse_mode();
    }
}
