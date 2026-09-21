/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_music.c
 * @brief 音乐播放 Tab：列出 SD 卡音乐目录，点选播放（esp_player + esp_audio_render）
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

#include "tab_music.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "middleware/system/sdcard_service.h"

static const char *TAG = "TAB_MUSIC";

#define MAX_TRACKS 16
#define NAME_LEN    64
#define PATH_LEN    300

static char s_tracks[MAX_TRACKS][PATH_LEN];
static int  s_track_count = 0;
static int  s_current = -1;

static lv_obj_t *s_list;
static lv_obj_t *s_title;
static lv_obj_t *s_play_btn;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_stop_btn;
static bool s_paused = false;

static esp_player_handle_t s_player = NULL;
static esp_audio_render_handle_t s_render = NULL;
static esp_gmf_pool_handle_t s_pool = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static bool s_media_registered = false;

/* 音频输出写回调：直接写 ES8389 codec，不依赖 CALL tab */
static int audio_out_write(uint8_t *data, uint32_t size, void *ctx)
{
    esp_codec_dev_handle_t dev = (esp_codec_dev_handle_t)ctx;
    if (!dev || !data || !size) return -1;
    return esp_codec_dev_write(dev, data, size);
}

/* 注册 extractor / decoder 默认组件 */
static void register_media_defaults(void)
{
    if (s_media_registered) return;
    media_lib_add_default_adapter();
    esp_extractor_register_default();
    esp_audio_dec_register_default();
    s_media_registered = true;
}

/* 初始化 GMF 处理池（重采样/声道/位宽/音量） */
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

/* 初始化 speaker codec（16kHz 单声道 16bit，后续由 render 自动重采样） */
static int init_codec(void)
{
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return -1;
    }
    esp_codec_dev_set_out_vol(s_codec, 60);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 44100,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return -1;
    }
    return 0;
}

/* 初始化 esp_player 播放管线 */
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

    /* 获取 render stream */
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
    ESP_LOGI(TAG, "player ready");
    return 0;
}

/* ---- 扫描音乐目录 ---- */
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
        if (!dot) continue;
        if (strcasecmp(dot, ".mp3") != 0) continue;

        snprintf(s_tracks[s_track_count], PATH_LEN,
                 SDCARD_MNT_PATH "/music/%s", ent->d_name);
        s_track_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "found %d mp3 files", s_track_count);
}

/* ---- 播放选中曲目 ---- */
static void play_track(int idx)
{
    if (idx < 0 || idx >= s_track_count) return;
    s_current = idx;

    const char *name = strrchr(s_tracks[idx], '/');
    name = name ? name + 1 : s_tracks[idx];

    ESP_LOGI(TAG, "play: %s", s_tracks[idx]);
    if (s_title) {
        lv_label_set_text_fmt(s_title, "%s", name);
    }

    if (!s_player) {
        ESP_LOGE(TAG, "player not inited");
        return;
    }

    /* 停止上一首 */
    esp_player_stop(s_player);

    /* 设置数据源并播放 */
    esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(s_tracks[idx], ESP_PLAYER_MASK_AUDIO);
    esp_player_set_data_src(s_player, &src);
    esp_player_run(s_player);
}

static void on_item_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    play_track(idx);
}

static void on_pause_click(lv_event_t *e)
{
    (void)e;
    if (!s_player) return;
    if (s_paused) {
        esp_player_resume(s_player);
        s_paused = false;
        lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PAUSE" Pause");
        ESP_LOGI(TAG, "resumed");
    } else {
        esp_player_pause(s_player);
        s_paused = true;
        lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PLAY" Resume");
        ESP_LOGI(TAG, "paused");
    }
}

static void on_stop_click(lv_event_t *e)
{
    (void)e;
    if (!s_player) return;
    esp_player_stop(s_player);
    s_paused = false;
    lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PAUSE" Pause");
    if (s_title) lv_label_set_text(s_title, "Stopped");
    ESP_LOGI(TAG, "stopped");
}
void tab_music_build(lv_obj_t *parent, lv_group_t *g)
{
    init_player();

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 标题 */
    s_title = lv_label_create(parent);
    lv_label_set_text(s_title, "SD card music");
    lv_obj_set_style_text_font(s_title, UI_FONT, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 10);

    /* 列表 */
    s_list = lv_list_create(parent);
    lv_obj_set_size(s_list, 700, 300);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 50);

    scan_tracks();
    for (int i = 0; i < s_track_count; i++) {
        const char *name = strrchr(s_tracks[i], '/');
        name = name ? name + 1 : s_tracks[i];
        lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_AUDIO, name);
        lv_obj_add_event_cb(btn, on_item_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* 控制按钮行 */
    lv_obj_t *ctrl_row = lv_obj_create(parent);
    lv_obj_set_size(ctrl_row, 680, 70);
    lv_obj_align(ctrl_row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_t *lbl = NULL;
    s_pause_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(s_pause_btn, 200, 60);
    lbl = lv_label_create(s_pause_btn);
    lv_label_set_text(lbl, LV_SYMBOL_PAUSE" Pause");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(s_pause_btn, on_pause_click, LV_EVENT_CLICKED, NULL);

    s_stop_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(s_stop_btn, 200, 60);
    lbl = lv_label_create(s_stop_btn);
    lv_label_set_text(lbl, LV_SYMBOL_STOP" Stop");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(s_stop_btn, on_stop_click, LV_EVENT_CLICKED, NULL);

    if (g) {
        lv_group_add_obj(g, s_pause_btn);
        lv_group_add_obj(g, s_stop_btn);
    }
}

void tab_music_destroy(void)
{
    if (s_player) {
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
    s_list = NULL;
    s_title = NULL;
    s_play_btn = NULL;
    s_track_count = 0;
    s_current = -1;
}
