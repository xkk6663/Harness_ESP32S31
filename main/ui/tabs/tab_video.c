/*
 * SPDX-FileCopyright-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_video.c
 * @brief 视频播放：全屏播放 SD 卡 MJPEG AVI，硬件 JPEG 解码 + PPA 缩放
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"

#include "esp_player.h"
#include "esp_audio_render.h"
#include "esp_extractor_defaults.h"
#include "esp_audio_dec_default.h"
#include "esp_video_dec_default.h"
#include "esp_video_render.h"
#include "esp_video_render_backend.h"
#include "media_lib_adapter.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"
#include "esp_gmf_video_color_convert.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "esp_gmf_video_ppa.h"
#endif

#include "tab_video.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "middleware/system/sdcard_service.h"

static const char *TAG = "TAB_VIDEO";

#define MAX_VIDEOS 8
#define PATH_LEN   300

static char s_videos[MAX_VIDEOS][PATH_LEN];
static int  s_video_count = 0;

static lv_obj_t *s_list;
static lv_obj_t *s_parent;
static lv_obj_t *s_title;

static esp_player_handle_t s_player = NULL;
static esp_audio_render_handle_t s_audio_render = NULL;
static esp_gmf_pool_handle_t s_audio_pool = NULL;
static esp_video_render_handle_t s_video_render = NULL;
static esp_gmf_pool_handle_t s_video_pool = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static bool s_media_registered = false;
static bool s_playing = false;
static bool s_lvgl_suspended = false;

/* 在 LVGL 线程执行：强制整屏重绘，把 framebuffer 从视频画面切回 LVGL */
static void restore_ui_cb(void *arg)
{
    (void)arg;
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "LVGL UI invalidate requested");
}

/* 播放器事件回调（运行在 GMF 线程，不可直接调 LVGL 绘制 API） */
static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (!msg) return ESP_PLAYER_ERR_OK;
    switch (msg->event_type) {
    case ESP_PLAYER_EVENT_FINISHED:
        ESP_LOGI(TAG, "playback FINISHED, restoring LVGL");
        s_playing = false;
        if (s_lvgl_suspended) {
            /* 先排队整屏刷新（resume 后由 LVGL 线程执行），再恢复 LVGL 定时器 */
            if (lvgl_port_lock(100)) {
                lv_async_call(restore_ui_cb, NULL);
                lvgl_port_unlock();
            }
            lvgl_port_resume();
            lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
            s_lvgl_suspended = false;
        }
        break;
    case ESP_PLAYER_EVENT_ERROR:
        ESP_LOGE(TAG, "playback ERROR, restoring LVGL");
        s_playing = false;
        if (s_lvgl_suspended) {
            if (lvgl_port_lock(100)) {
                lv_async_call(restore_ui_cb, NULL);
                lvgl_port_unlock();
            }
            lvgl_port_resume();
            lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
            s_lvgl_suspended = false;
        }
        break;
    default:
        break;
    }
    return ESP_PLAYER_ERR_OK;
}

/* 音频写回调 */
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
    esp_video_dec_register_default();
    s_media_registered = true;
}

/* 初始化 GMF 音频处理池 */
static int init_audio_pool(void)
{
    if (esp_gmf_pool_init(&s_audio_pool) != ESP_GMF_ERR_OK) return -1;
    esp_gmf_element_handle_t el = NULL;
    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch_cfg, &el) == ESP_GMF_ERR_OK)
        esp_gmf_pool_register_element(s_audio_pool, el, NULL);
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &el) == ESP_GMF_ERR_OK)
        esp_gmf_pool_register_element(s_audio_pool, el, NULL);
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate_cfg, &el) == ESP_GMF_ERR_OK)
        esp_gmf_pool_register_element(s_audio_pool, el, NULL);
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK)
        esp_gmf_pool_register_element(s_audio_pool, el, NULL);
    return 0;
}

/* 初始化 GMF 视频处理池（PPA 缩放 + 色彩转换） */
static int init_video_pool(void)
{
    if (esp_gmf_pool_init(&s_video_pool) != ESP_GMF_ERR_OK) return -1;
    esp_gmf_element_handle_t el = NULL;
#if CONFIG_IDF_TARGET_ESP32S31
    if (esp_gmf_video_ppa_init(NULL, &el) == ESP_GMF_ERR_OK)
        esp_gmf_pool_register_element(s_video_pool, el, NULL);
#else
    // 其他芯片用软件缩放
#endif
    esp_imgfx_color_convert_cfg_t color_cfg = {
        .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
    };
    if (esp_gmf_video_color_convert_init(&color_cfg, &el) == ESP_GMF_ERR_OK)
        esp_gmf_pool_register_element(s_video_pool, el, NULL);
    return 0;
}

/* 初始化 speaker codec */
static int init_codec(void)
{
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) return -1;
    esp_codec_dev_set_out_vol(s_codec, 60);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 44100, .channel = 2, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) return -1;
    return 0;
}

/* ★ 2026-09-22 删除 init_lcd()：
 *   本工程走 LVGL backend，面板已由 BSP 初始化，init_player() 里明确注明
 *   "不需要单独初始化 LCD"；init_lcd() 从未被调用 → -Wunused-function。
 *   s_lcd_panel / s_lcd_io 不受影响，它们改由 init_player() 中的
 *   bsp_display_get_panel() / bsp_display_get_io() 取得（见本文件下方）。 */

/* 初始化播放器 */
static int init_player(void)
{
    register_media_defaults();
    if (init_codec() != 0) return -1;
    if (init_audio_pool() != 0) return -1;
    if (init_video_pool() != 0) return -1;
    /* 用 LVGL backend，不需要单独初始化 LCD */

    /* audio render */
    esp_audio_render_cfg_t ar_cfg = {
        .max_stream_num = 1,
        .out_writer = audio_out_write,
        .out_ctx = s_codec,
        .out_sample_info = { .sample_rate = 44100, .channel = 2, .bits_per_sample = 16 },
        .pool = s_audio_pool,
        .process_period = 20,
        .process_buf_align = 16,
    };
    esp_audio_render_handle_t audio_render = NULL;
    if (esp_audio_render_create(&ar_cfg, &audio_render) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "audio_render create failed");
        return -1;
    }
    esp_audio_render_stream_handle_t audio_stream = NULL;
    esp_audio_render_stream_get(audio_render, ESP_AUDIO_RENDER_FIRST_STREAM, &audio_stream);
    s_audio_render = audio_render;

    /* video render */
    esp_video_render_cfg_t vr_cfg = {
        .pool = s_video_pool,
        .fps = 10,
    };
    if (esp_video_render_create(&vr_cfg, &s_video_render) != ESP_VIDEO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "video_render create failed");
        return -1;
    }

    /* LCD backend：直写 framebuffer，硬件直通 */
    s_lcd_panel = bsp_display_get_panel();
    s_lcd_io = bsp_display_get_io();
    if (!s_lcd_panel) {
        ESP_LOGE(TAG, "LCD panel handle is NULL");
        return -1;
    }
    esp_video_render_lcd_cfg_t lcd_cfg = {
        .lcd_type = ESP_VIDEO_RENDER_LCD_TYPE_RGB,
        .fb_num = 2,
        .out_format = ESP_VIDEO_RENDER_FORMAT_RGB565,
        .width = 800,
        .height = 480,
        .lcd_handle = s_lcd_panel,
        .io_handle = s_lcd_io,
    };
    esp_video_render_backend_cfg_t be_cfg = {
        .ops = esp_video_render_get_lcd_backend(),
        .cfg = &lcd_cfg,
        .cfg_size = sizeof(lcd_cfg),
    };
    if (esp_video_render_set_display(s_video_render, &be_cfg) != ESP_VIDEO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "set LCD display failed");
        return -1;
    }

    /* player */
    esp_player_config_t p_cfg = ESP_PLAYER_CONFIG_DEFAULT();
    p_cfg.audio_render_hd = audio_stream;
    p_cfg.video_render_hd = s_video_render;
    if (esp_player_init(&p_cfg, &s_player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "player init failed");
        return -1;
    }
    esp_player_set_event_cb(s_player, player_event_cb, NULL);
    ESP_LOGI(TAG, "video player ready");
    return 0;
}

/* 扫描视频目录 */
static void scan_videos(void)
{
    s_video_count = 0;
    if (!sdcard_service_is_mounted()) return;
    DIR *d = opendir(SDCARD_MNT_PATH "/videos");
    if (!d) {
        ESP_LOGW(TAG, "no /sdcard/videos dir");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) && s_video_count < MAX_VIDEOS) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".avi") != 0 && strcasecmp(dot, ".mjpeg") != 0) continue;
        snprintf(s_videos[s_video_count], PATH_LEN, SDCARD_MNT_PATH "/videos/%s", ent->d_name);
        s_video_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "found %d videos", s_video_count);
}

static void play_video(int idx)
{
    if (idx < 0 || idx >= s_video_count || !s_player) return;
    ESP_LOGI(TAG, "play: %s", s_videos[idx]);

    /* 挂起 LVGL 渲染，视频期间由 video render 独占 RGB framebuffer，避免抢 buffer 撕裂/闪屏 */
    if (!s_lvgl_suspended) {
        lvgl_port_stop();
        s_lvgl_suspended = true;
        ESP_LOGI(TAG, "LVGL suspended for video playback");
    }

    esp_player_stop(s_player);
    esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(s_videos[idx], ESP_PLAYER_MASK_AV);
    esp_player_set_data_src(s_player, &src);
    esp_player_run(s_player);
    s_playing = true;
}

static void on_item_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    play_video(idx);
}

void tab_video_build(lv_obj_t *parent, lv_group_t *g)
{
    s_parent = parent;
    scan_videos();
    init_player();

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_set_style_pad_top(parent, 14, 0);
    lv_obj_set_style_pad_bottom(parent, 10, 0);

    /* ① 顶栏：标题 + 片源数量（对齐 tab_audio / tab_call 的卡片风格） */
    lv_obj_t *header = ui_common_card(parent, BSP_LCD_H_RES - 40, 48, 12);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 16, 0);

    s_title = lv_label_create(header);
    lv_label_set_text_static(s_title, LV_SYMBOL_VIDEO "  VIDEO LIBRARY");
    lv_obj_set_style_text_color(s_title, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_title, UI_FONT, 0);

    lv_obj_t *cnt = lv_label_create(header);
    lv_label_set_text_fmt(cnt, "%d clip%s", s_video_count,
                          s_video_count == 1 ? "" : "s");
    lv_obj_set_style_text_color(cnt, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(cnt, UI_FONT, 0);

    /* ② 片源列表（占满剩余高度，内部滚动） */
    s_list = ui_common_list(parent);
    lv_obj_set_width(s_list, BSP_LCD_H_RES - 40);
    lv_obj_set_flex_grow(s_list, 1);

    for (int i = 0; i < s_video_count; i++) {
        const char *name = strrchr(s_videos[i], '/');
        name = name ? name + 1 : s_videos[i];
        lv_obj_t *btn = ui_common_list_add_button(s_list, LV_SYMBOL_VIDEO, name);
        lv_obj_add_event_cb(btn, on_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s_video_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text_static(empty, "No clips found in /sdcard/videos");
        lv_obj_set_style_text_color(empty, COL_SUBTEXT, 0);
        lv_obj_set_style_text_font(empty, UI_FONT, 0);
    }

    /* ③ 底部提示 */
    lv_obj_t *foot = lv_label_create(parent);
    lv_label_set_text_static(foot, "Tap a clip - video takes over the panel");
    lv_obj_set_style_text_color(foot, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(foot, UI_FONT, 0);

    (void)g;
}

void tab_video_destroy(void)
{
    if (s_player) {
        esp_player_set_event_cb(s_player, NULL, NULL);
        esp_player_stop(s_player);
        esp_player_deinit(s_player);
        s_player = NULL;
    }
    s_playing = false;
    /* 恢复 LVGL 渲染并强制整屏重绘 */
    if (s_lvgl_suspended) {
        lvgl_port_resume();
        s_lvgl_suspended = false;
        ESP_LOGI(TAG, "LVGL resumed on tab destroy");
    }
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
    if (s_video_render) {
        esp_video_render_destroy(s_video_render);
        s_video_render = NULL;
    }
    if (s_video_pool) {
        esp_gmf_pool_deinit(s_video_pool);
        s_video_pool = NULL;
    }
    if (s_audio_render) {
        esp_audio_render_destroy(s_audio_render);
        s_audio_render = NULL;
    }
    if (s_audio_pool) {
        esp_gmf_pool_deinit(s_audio_pool);
        s_audio_pool = NULL;
    }
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        s_codec = NULL;
    }
    s_list = NULL;
    s_title = NULL;
    s_video_count = 0;
    s_playing = false;
}
