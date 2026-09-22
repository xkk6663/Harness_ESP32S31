/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_audio.c
 * @brief AUDIO HUB：音乐播放与蓝牙推流合并为一个页面。
 *
 * ★ 2026-09-22 由 tab_music.c + tab_btpush.c 合并而来。
 *   原先两个 tab 各自持有一份 SD 卡扫描结果、一个列表、一套标题与播放控制，
 *   内容 90% 重复却各占一份 LVGL 对象树。现在共用一个列表，顶部放一个
 *   模式切换按钮决定播放出口：
 *     - LOCAL   : esp_player + esp_audio_render → 板载 ES8389 扬声器
 *     - BT PUSH : bt_audio_service → A2DP Source 推流到蓝牙耳机
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "esp_player.h"
#include "esp_audio_render.h"
#include "esp_extractor_defaults.h"
#include "esp_audio_dec_default.h"
#include "media_lib_adapter.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"

#include "tab_audio.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "middleware/system/sdcard_service.h"
#include "middleware/bluetooth/bt_audio_service.h"

LV_FONT_DECLARE(lv_font_cn_20);

static const char *TAG = "TAB_AUDIO";

#define MAX_TRACKS 16
#define PATH_LEN   300
#define CARD_W     (BSP_LCD_H_RES - 40)

/* 播放出口：本地扬声器 / 蓝牙耳机 */
typedef enum {
    AUDIO_MODE_LOCAL = 0,
    AUDIO_MODE_BT,
} audio_mode_t;

static char s_tracks[MAX_TRACKS][PATH_LEN];
static int  s_track_count = 0;
static int  s_current = -1;

static audio_mode_t s_mode = AUDIO_MODE_LOCAL;
static bool s_paused = false;
static bool s_active = false;

static lv_obj_t *s_list;
static lv_obj_t *s_dot;
static lv_obj_t *s_state;
static lv_obj_t *s_track;
static lv_obj_t *s_hint;
static lv_obj_t *s_mode_btn;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_stop_btn;
static lv_timer_t *s_timer;

/* ---- 本地播放链路（原 tab_music 的初始化逻辑，保留不动）---- */
static esp_player_handle_t s_player = NULL;
static esp_audio_render_handle_t s_render = NULL;
static esp_gmf_pool_handle_t s_pool = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static bool s_media_registered = false;

/* ★ 播放器回调跑在播放器任务里，绝不能碰 LVGL —— 只置标志，
   由 status_timer_cb（LVGL 线程）抽取。与 tab_call.c 的做法一致。 */
static volatile bool s_player_finished = false;

static int audio_out_write(uint8_t *data, uint32_t size, void *ctx)
{
    esp_codec_dev_handle_t dev = (esp_codec_dev_handle_t)ctx;
    if (!dev || !data || !size) return -1;
    return esp_codec_dev_write(dev, data, size);
}

static void register_media_defaults(void)
{
    if (s_media_registered) return;
    media_lib_add_default_adapter();
    esp_extractor_register_default();
    esp_audio_dec_register_default();
    s_media_registered = true;
}

static int init_gmf_pool(void)
{
    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "gmf pool init failed");
        return -1;
    }
    esp_gmf_element_handle_t el = NULL;

    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    return 0;
}

static int init_codec(void)
{
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return -1;
    }
    esp_codec_dev_set_out_vol(s_codec, 60);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 44100, .channel = 2, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return -1;
    }
    return 0;
}

static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return ESP_PLAYER_ERR_OK;
    if (msg->event_type == ESP_PLAYER_EVENT_FINISHED ||
        msg->event_type == ESP_PLAYER_EVENT_ERROR) {
        s_player_finished = true;   /* 只置标志，UI 交给 LVGL 定时器 */
    }
    return ESP_PLAYER_ERR_OK;
}

static int init_player(void)
{
    register_media_defaults();
    if (init_codec() != 0) return -1;
    if (init_gmf_pool() != 0) return -1;

    esp_audio_render_cfg_t render_cfg = {
        .max_stream_num = 1,
        .out_writer = audio_out_write,
        .out_ctx = s_codec,
        .out_sample_info = { .sample_rate = 44100, .channel = 2, .bits_per_sample = 16 },
        .pool = s_pool,
        .process_period = 20,
        .process_buf_align = 16,
    };
    if (esp_audio_render_create(&render_cfg, &s_render) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "audio_render create failed");
        return -1;
    }

    esp_audio_render_stream_handle_t stream = NULL;
    if (esp_audio_render_stream_get(s_render, ESP_AUDIO_RENDER_FIRST_STREAM, &stream) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "render stream get failed");
        return -1;
    }

    esp_player_config_t player_cfg = ESP_PLAYER_CONFIG_DEFAULT();
    player_cfg.audio_render_hd = stream;
    player_cfg.video_render_hd = NULL;

    if (esp_player_init(&player_cfg, &s_player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "player init failed");
        return -1;
    }
    esp_player_set_event_cb(s_player, player_event_cb, NULL);
    ESP_LOGI(TAG, "local player ready");
    return 0;
}

/* ---- 列表 ---- */
static const char *base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static void scan_tracks(void)
{
    s_track_count = 0;
    s_current = -1;
    if (!sdcard_service_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }
    DIR *d = opendir(SDCARD_MNT_PATH "/music");
    if (!d) {
        ESP_LOGW(TAG, "no /sdcard/music dir");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) && s_track_count < MAX_TRACKS) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcasecmp(dot, ".mp3") != 0) continue;
        snprintf(s_tracks[s_track_count], PATH_LEN,
                 SDCARD_MNT_PATH "/music/%s", ent->d_name);
        s_track_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "found %d mp3 files", s_track_count);
}

/* ---- 播放控制：按模式路由 ---- */
static void set_pause_label(bool paused)
{
    if (!s_pause_btn) return;
    lv_obj_t *lab = lv_obj_get_child(s_pause_btn, 0);
    if (lab) lv_label_set_text(lab, paused ? (LV_SYMBOL_PLAY "  Resume")
                                           : (LV_SYMBOL_PAUSE "  Pause"));
}

static void set_hint(const char *txt)
{
    if (s_hint) lv_label_set_text(s_hint, txt ? txt : "");
}

static void stop_current(void)
{
    if (s_mode == AUDIO_MODE_BT) {
        bt_audio_service_stop_source();
    } else if (s_player) {
        esp_player_stop(s_player);
    }
    s_paused = false;
    s_active = false;
    set_pause_label(false);
}

static void play_track(int idx)
{
    if (idx < 0 || idx >= s_track_count) return;
    s_current = idx;
    set_hint("");
    if (s_track) lv_label_set_text(s_track, base_name(s_tracks[idx]));

    if (s_mode == AUDIO_MODE_BT) {
        if (!bt_audio_service_is_on()) {
            set_hint("Bluetooth off - turn it on in SYS");
            return;
        }
        ESP_LOGI(TAG, "[bt] push: %s", s_tracks[idx]);
        if (bt_audio_service_play_source(s_tracks[idx]) != ESP_OK) {
            set_hint("Push failed - pair a headset first");
            return;
        }
    } else {
        if (!s_player) {
            set_hint("Local player not ready");
            return;
        }
        ESP_LOGI(TAG, "[local] play: %s", s_tracks[idx]);
        esp_player_stop(s_player);
        esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(s_tracks[idx], ESP_PLAYER_MASK_AUDIO);
        esp_player_set_data_src(s_player, &src);
        esp_player_run(s_player);
        s_player_finished = false;
    }

    s_paused = false;
    s_active = true;
    set_pause_label(false);
}

/* ---- 事件回调 ---- */
static void on_item_click(lv_event_t *e)
{
    play_track((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_pause_click(lv_event_t *e)
{
    (void)e;
    if (s_mode == AUDIO_MODE_BT) {
        if (s_paused) bt_audio_service_resume_source();
        else          bt_audio_service_pause_source();
    } else {
        if (!s_player) return;
        if (s_paused) esp_player_resume(s_player);
        else          esp_player_pause(s_player);
    }
    s_paused = !s_paused;
    set_pause_label(s_paused);
}

static void on_stop_click(lv_event_t *e)
{
    (void)e;
    stop_current();
    if (s_track) lv_label_set_text(s_track, "-- no track --");
    set_hint("");
}

/* 模式按钮：ghost(本地) 与 accent(蓝牙) 两种外观来回切 */
static void apply_mode_style(void)
{
    if (!s_mode_btn) return;
    lv_obj_t *lab = lv_obj_get_child(s_mode_btn, 0);
    if (s_mode == AUDIO_MODE_BT) {
        lv_obj_set_style_bg_color(s_mode_btn, COL_CYAN, 0);
        lv_obj_set_style_bg_opa(s_mode_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_mode_btn, COL_CYAN, 0);
        if (lab) {
            lv_label_set_text(lab, LV_SYMBOL_BLUETOOTH "  BT PUSH");
            lv_obj_set_style_text_color(lab, COL_DARK, 0);
        }
    } else {
        lv_obj_set_style_bg_opa(s_mode_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_mode_btn, COL_CYAN, 0);
        if (lab) {
            lv_label_set_text(lab, LV_SYMBOL_AUDIO "  LOCAL");
            lv_obj_set_style_text_color(lab, COL_CYAN, 0);
        }
    }
}

static void on_mode_click(lv_event_t *e)
{
    (void)e;
    /* 切出口前先停播：否则本地与推流两条链路可能同时出声 */
    stop_current();
    s_mode = (s_mode == AUDIO_MODE_LOCAL) ? AUDIO_MODE_BT : AUDIO_MODE_LOCAL;
    apply_mode_style();
    if (s_track) lv_label_set_text(s_track, "-- no track --");
    set_hint(s_mode == AUDIO_MODE_BT ? "Headset must be paired in SYS > Bluetooth" : "");
    ESP_LOGI(TAG, "mode -> %s", s_mode == AUDIO_MODE_BT ? "BT PUSH" : "LOCAL");
}

/* ---- 状态刷新（LVGL 线程）---- */
static void update_status(void)
{
    if (!s_dot || !s_state) return;
    const char *txt;
    lv_color_t col;

    if (s_mode == AUDIO_MODE_BT) {
        if (!bt_audio_service_is_on()) {
            txt = "BT OFF  |  turn on in SYS";
            col = COL_RED;
        } else if (bt_audio_service_is_connected()) {
            txt = s_active ? "PUSHING  |  headset" : "CONNECTED  |  headset";
            col = COL_GREEN;
        } else {
            txt = "BT ON  |  no headset";
            col = COL_CYAN;
        }
    } else {
        txt = s_active ? "PLAYING  |  local speaker" : "READY  |  local speaker";
        col = s_active ? COL_GREEN : COL_SUBTEXT;
    }
    lv_obj_set_style_bg_color(s_dot, col, 0);
    lv_label_set_text_static(s_state, txt);
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_player_finished) {
        s_player_finished = false;
        s_active = false;
        set_pause_label(false);
    }
    update_status();
}

/* ---- 构建 ---- */
void tab_audio_build(lv_obj_t *parent, lv_group_t *g)
{
    init_player();

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_set_style_pad_top(parent, 14, 0);
    lv_obj_set_style_pad_bottom(parent, 10, 0);

    /* ① 顶栏：标题 + 播放出口切换 */
    lv_obj_t *header = ui_common_card(parent, CARD_W, 48, 12);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 16, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text_static(title, LV_SYMBOL_AUDIO "  AUDIO HUB");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    s_mode_btn = ui_common_ghost_button(header, 168, 34, LV_SYMBOL_AUDIO "  LOCAL");
    lv_obj_add_event_cb(s_mode_btn, on_mode_click, LV_EVENT_CLICKED, NULL);
    apply_mode_style();

    /* ② 正在播放卡片：状态灯 + 状态文本 + 曲名 + 提示 */
    lv_obj_t *np = ui_common_card(parent, CARD_W, 92, 14);
    lv_obj_set_flex_flow(np, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(np, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(np, 12, 0);
    lv_obj_set_style_pad_column(np, 12, 0);

    s_dot = lv_obj_create(np);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_scrollbar_mode(s_dot, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *col = lv_obj_create(np);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    s_state = lv_label_create(col);
    lv_obj_set_style_text_color(s_state, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_state, UI_FONT, 0);

    s_track = lv_label_create(col);
    lv_label_set_text_static(s_track, "-- no track --");
    lv_obj_set_width(s_track, LV_PCT(100));
    lv_label_set_long_mode(s_track, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_track, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_track, &lv_font_cn_20, 0);

    s_hint = lv_label_create(col);
    lv_obj_set_width(s_hint, LV_PCT(100));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(s_hint, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT, 0);

    /* ③ 曲目列表（本地 / 蓝牙共用同一份 SD 卡扫描结果）*/
    s_list = ui_common_list(parent);
    lv_obj_set_width(s_list, CARD_W);
    lv_obj_set_flex_grow(s_list, 1);

    scan_tracks();
    for (int i = 0; i < s_track_count; i++) {
        lv_obj_t *btn = ui_common_list_add_button(s_list, LV_SYMBOL_AUDIO,
                                                  base_name(s_tracks[i]));
        lv_obj_add_event_cb(btn, on_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s_track_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text_static(empty, "No MP3 found in /sdcard/music");
        lv_obj_set_style_text_color(empty, COL_SUBTEXT, 0);
        lv_obj_set_style_text_font(empty, UI_FONT, 0);
    }

    /* ④ 控制行 */
    lv_obj_t *ctrl = lv_obj_create(parent);
    lv_obj_set_size(ctrl, CARD_W, 56);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 0, 0);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_scrollbar_mode(ctrl, LV_SCROLLBAR_MODE_OFF);

    s_pause_btn = ui_common_accent_button(ctrl, 220, 52, LV_SYMBOL_PAUSE "  Pause");
    lv_obj_add_event_cb(s_pause_btn, on_pause_click, LV_EVENT_CLICKED, NULL);

    s_stop_btn = ui_common_ghost_button(ctrl, 180, 52, LV_SYMBOL_STOP "  Stop");
    lv_obj_add_event_cb(s_stop_btn, on_stop_click, LV_EVENT_CLICKED, NULL);

    if (g) {
        lv_group_add_obj(g, s_mode_btn);
        lv_group_add_obj(g, s_pause_btn);
        lv_group_add_obj(g, s_stop_btn);
    }

    update_status();
    if (!s_timer) s_timer = lv_timer_create(status_timer_cb, 500, NULL);
}

void tab_audio_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_player) {
        esp_player_set_event_cb(s_player, NULL, NULL);
        esp_player_stop(s_player);
        esp_player_deinit(s_player);
        s_player = NULL;
    }
    if (s_render) {
        esp_audio_render_destroy(s_render);
        s_render = NULL;
    }
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        s_codec = NULL;
    }
    s_list = s_dot = s_state = s_track = s_hint = NULL;
    s_mode_btn = s_pause_btn = s_stop_btn = NULL;
    s_track_count = 0;
    s_current = -1;
    s_active = false;
    s_paused = false;
    s_player_finished = false;
}
