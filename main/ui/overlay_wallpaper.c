/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wallpaper.c
 * @brief Pull-down wallpaper overlay: JPEG decode + drag gestures.
 *        Extracted from the former ui_splash.c / ui_disp.c.
 *
 * @note 时钟天气卡片只挂在 main 页面的下拉壁纸实例上（overlay_wallpaper_init），
 *       splash 走 overlay_wallpaper_build() 直连，天然隔离、不会带上卡片。
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"

#include "overlay_wallpaper.h"
#include "overlay_manager.h"
#include "ui_theme.h"
#include "middleware/fs_service.h"
#include "middleware/net_time_service.h"

static const char *TAG = "WP_OVERLAY";

#define WALLPAPER_PATH   FS_MNT_PATH"/xwkkk.jpg"
#define PULL_TOP_ZONE    60     /* px from top to arm pull-down   */
#define PULL_DOWN_OPEN   200    /* px travel to fully open        */
#define PULL_ANIM_MS     220

#define CLOCK_TICK_MS    1000   /* 时间每秒走一格              */
#define CLOCK_CARD_W     380
#define CLOCK_CARD_H     110

static lv_obj_t *s_wallpaper;
static lv_obj_t *s_tab_btns;
static int32_t   s_pull_start_y;
static bool      s_pull_armed;

/* ---- 主页面下拉壁纸上的实时时钟天气卡片 ---- */
static lv_obj_t   *s_clock_card;
static lv_obj_t   *s_time_label;
static lv_obj_t   *s_date_label;
static lv_obj_t   *s_wx_label;
static lv_timer_t *s_clock_timer;

/* Decode xwkkk.jpg into the shared PSRAM file_buffer and bind to a canvas. */
static void wallpaper_decode_into(lv_obj_t *canvas)
{
    struct stat st;
    if (stat(WALLPAPER_PATH, &st) != 0) {
        ESP_LOGE(TAG, "wallpaper not found: %s", WALLPAPER_PATH);
        return;
    }
    size_t fsize = st.st_size;
    uint8_t *jpeg_buf = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!jpeg_buf) {
        jpeg_buf = malloc(fsize);
    }
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "no dma mem for jpeg buf");
        return;
    }

    int fd = open(WALLPAPER_PATH, O_RDONLY);
    if (fd < 0) {
        free(jpeg_buf);
        return;
    }
    read(fd, jpeg_buf, fsize);
    close(fd);

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = {
        .inbuf = jpeg_buf,
        .inbuf_len = (int)fsize,
        .outbuf = file_buffer,
    };
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;

    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        goto cleanup;
    }
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg header parse failed");
        goto cleanup;
    }
    int out_len = 0;
    jpeg_dec_get_outbuf_len(dec, &out_len);
    if ((size_t)out_len > file_buffer_size) {
        ESP_LOGE(TAG, "jpeg too large: %d > %u", out_len, (unsigned)file_buffer_size);
        goto cleanup;
    }
    if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
        size_t pixels = (size_t)info.width * info.height;
        uint8_t *p = file_buffer;
        for (size_t i = 0; i < pixels; i++) {
            uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
            p += 3;
        }
        lv_canvas_set_buffer(canvas, file_buffer, info.width, info.height,
                             LV_COLOR_FORMAT_RGB888);
        lv_obj_center(canvas);
        ESP_LOGI(TAG, "wallpaper %dx%d decoded", info.width, info.height);
    } else {
        ESP_LOGE(TAG, "jpeg decode failed");
    }

cleanup:
    if (dec) {
        jpeg_dec_close(dec);
    }
    free(jpeg_buf);
}

lv_obj_t *overlay_wallpaper_build(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(layer, COL_BG, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_radius(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *canvas = lv_canvas_create(layer);
    lv_obj_center(canvas);
    wallpaper_decode_into(canvas);
    return layer;
}

/* ================= 实时时钟天气卡片（仅主页面下拉壁纸） ================= */

/**
 * @brief 在壁纸 layer 上创建时钟天气卡片（默认 HIDDEN，联网取到数据后浮现）
 * @param parent 主页面下拉壁纸 layer（build 的返回值）
 * @note  卡片是 layer 的子对象，壁纸整体位移时自动跟随，无需额外跟随代码。
 */
static void clock_widget_create(lv_obj_t *parent)
{
    s_clock_card = lv_obj_create(parent);
    lv_obj_set_size(s_clock_card, CLOCK_CARD_W, CLOCK_CARD_H);
    lv_obj_align(s_clock_card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(s_clock_card, COL_DARK, 0);
    lv_obj_set_style_bg_opa(s_clock_card, LV_OPA_70, 0);   /* 半透明，压在壁纸上仍可读 */
    lv_obj_set_style_radius(s_clock_card, 14, 0);
    lv_obj_set_style_border_color(s_clock_card, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_clock_card, 1, 0);
    lv_obj_set_style_pad_all(s_clock_card, 0, 0);
    lv_obj_remove_flag(s_clock_card, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(s_clock_card);
    lv_label_set_text_static(s_time_label, "--:--:--");
    lv_obj_set_style_text_color(s_time_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_28, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 8);

    s_date_label = lv_label_create(s_clock_card);
    lv_label_set_text_static(s_date_label, "----/--/--");
    lv_obj_set_style_text_color(s_date_label, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_date_label, UI_FONT, 0);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 46);

    s_wx_label = lv_label_create(s_clock_card);
    lv_label_set_text_static(s_wx_label, "--");
    lv_obj_set_style_text_color(s_wx_label, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_wx_label, UI_FONT, 0);
    lv_obj_align(s_wx_label, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_add_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN);   /* 默认隐藏，取到数据才显示 */
    ESP_LOGI(TAG, "clock card created (hidden until online)");
}

/**
 * @brief 每秒刷新：时间/日期/当天天气；未校时成功则整卡隐藏
 * @note  跑在 LVGL task（lv_timer 回调），可安全操作控件。
 */
static void clock_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_clock_card) {
        return;
    }

    /* 没联网 / 没校时 → 整卡隐藏，壁纸保持干净 */
    if (!net_time_service_time_valid()) {
        if (!lv_obj_has_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "time invalid, clock card hidden");
        }
        return;
    }

    time_t now = 0;
    struct tm tmv = {0};
    /* 40 容不下 "最高/最低 + 24B 英文天气状况"，留足余量避免 -Werror=format-truncation */
    char buf[64];

    time(&now);
    localtime_r(&now, &tmv);

    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    lv_label_set_text(s_time_label, buf);

    strftime(buf, sizeof(buf), "%Y-%m-%d %a", &tmv);
    lv_label_set_text(s_date_label, buf);

    weather_data_t wx;
    net_time_service_get_weather(&wx);
    if (wx.ready && wx.day_count > 0) {
        snprintf(buf, sizeof(buf), "%d C / %d C   %s",
                 wx.days[0].t_max, wx.days[0].t_min, wx.days[0].text_en);
        lv_label_set_text(s_wx_label, buf);
    } else {
        /* 时间到了但天气还没回来：先只显示时间，天气位留占位 */
        lv_label_set_text(s_wx_label, "--");
    }

    if (lv_obj_has_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_clock_card, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "clock card visible: %s %s", lv_label_get_text(s_time_label),
                 lv_label_get_text(s_date_label));
    }
}

/* ---- anim helper ---- */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}
static void wallpaper_animate_to(lv_obj_t *wp, int32_t target_y)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wp);
    lv_anim_set_values(&a, lv_obj_get_y(wp), target_y);
    lv_anim_set_time(&a, PULL_ANIM_MS);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ---- pull-down: drag wallpaper out of the top edge ---- */
static void main_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = (p.y < PULL_TOP_ZONE);
}

static void main_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy > 0) {
        int32_t y = -BSP_LCD_V_RES + dy;
        if (y > 0) {
            y = 0;
        }
        lv_obj_set_y(s_wallpaper, y);
    }
}

static void main_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        s_pull_armed = false;
        return;
    }
    int32_t y = lv_obj_get_y(s_wallpaper);
    if (y > -BSP_LCD_V_RES + PULL_DOWN_OPEN) {
        overlay_open(OVERLAY_WALLPAPER);   /* snaps to y=0 via show() */
    } else {
        wallpaper_animate_to(s_wallpaper, -BSP_LCD_V_RES);
    }
    s_pull_armed = false;
}

/* ---- swipe-up on open wallpaper to dismiss ---- */
static void wp_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = true;
}

static void wp_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy < 0) {
        int32_t y = dy;
        if (y < -BSP_LCD_V_RES) {
            y = -BSP_LCD_V_RES;
        }
        lv_obj_set_y(s_wallpaper, y);
    }
}

static void wp_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        s_pull_armed = false;
        return;
    }
    int32_t y = lv_obj_get_y(s_wallpaper);
    if (y < -120) {
        overlay_close(OVERLAY_WALLPAPER);  /* snaps shut via hide() */
    } else {
        wallpaper_animate_to(s_wallpaper, 0);
    }
    s_pull_armed = false;
}

void overlay_wallpaper_init(lv_obj_t *tab_btns)
{
    s_tab_btns = tab_btns;

    s_wallpaper = overlay_wallpaper_build(lv_layer_top());
    /* 只给主页面这个实例加时钟卡片：splash 直接调 build()，走不到这里 */
    clock_widget_create(s_wallpaper);
    lv_obj_set_y(s_wallpaper, -BSP_LCD_V_RES);
    lv_obj_add_flag(s_wallpaper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wallpaper, wp_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_released_cb, LV_EVENT_RELEASED, NULL);

    if (s_tab_btns) {
        lv_obj_add_event_cb(s_tab_btns, main_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_tab_btns, main_pressing_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_tab_btns, main_released_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_flag(s_tab_btns, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    if (!s_clock_timer) {
        s_clock_timer = lv_timer_create(clock_tick, CLOCK_TICK_MS, NULL);
    }
}

void overlay_wallpaper_deinit(void)
{
    if (s_clock_timer) {
        lv_timer_del(s_clock_timer);
        s_clock_timer = NULL;
    }
    s_clock_card = NULL;
    s_time_label = NULL;
    s_date_label = NULL;
    s_wx_label = NULL;

    if (s_wallpaper) {
        lv_obj_del(s_wallpaper);
        s_wallpaper = NULL;
    }
    s_tab_btns = NULL;
    s_pull_armed = false;
}

void overlay_wallpaper_show(void)
{
    if (s_wallpaper) {
        wallpaper_animate_to(s_wallpaper, 0);
    }
}

void overlay_wallpaper_hide(void)
{
    if (s_wallpaper) {
        wallpaper_animate_to(s_wallpaper, -BSP_LCD_V_RES);
    }
}
