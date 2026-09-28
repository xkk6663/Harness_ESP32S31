/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wallpaper.c
 * @brief Pull-down wallpaper overlay: JPEG decode + drag gestures + clock/weather widget.
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
#include "middleware/periph/fs_service.h"
#include "middleware/net/net_time_service.h"
#include "middleware/net/wifi_manager.h"

LV_FONT_DECLARE(lv_font_digital_96);
LV_FONT_DECLARE(lv_font_cn_20);
LV_FONT_DECLARE(lv_font_cn_30);
LV_FONT_DECLARE(lv_font_cn_45);

/* Icons loaded from LittleFS via LVGL POSIX FS (letter L) */
#define ICON_WIFI     "L:/littlefs/icons/wifi.png"
#define ICON_SUN      "L:/littlefs/icons/sun.png"
#define ICON_CLOUD    "L:/littlefs/icons/cloud.png"
#define ICON_TEMP     "L:/littlefs/icons/temp.png"
#define ICON_HUMIDITY "L:/littlefs/icons/humidity.png"
#define ICON_WIND     "L:/littlefs/icons/wind.png"
#define ICON_SUNRISE  "L:/littlefs/icons/sunrise.png"
#define ICON_RAIN     "L:/littlefs/icons/rain.png"

static const char *TAG = "WP_OVERLAY";

#define WALLPAPER_PATH   FS_MNT_PATH"/xwkkk.jpg"
#define PULL_TOP_ZONE    60
#define PULL_DOWN_OPEN   200
#define PULL_ANIM_MS     220

#define CLOCK_TICK_MS    1000
#define CLOCK_CARD_W     380
#define CLOCK_CARD_H     110

static lv_obj_t *s_wallpaper;
static lv_obj_t *s_tab_btns;
static int32_t   s_pull_start_y;
static bool      s_pull_armed;

/* ---- clock/weather widget on pull-down wallpaper ---- */
static lv_obj_t *s_root;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_ip_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_wx_icon;
static lv_obj_t *s_now_temp_label;
static lv_obj_t *s_now_text_label;
static lv_obj_t *s_hi_lo_label;
static lv_obj_t *s_rh_label;
static lv_obj_t *s_wind_label;
static lv_obj_t *s_aqi_label;
static lv_obj_t *s_sun_label;
static lv_timer_t *s_clock_timer;

/* Decode xwkkk.jpg into PSRAM file_buffer and bind to canvas. */
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
    lv_obj_set_scrollable(layer, false);

    lv_obj_t *canvas = lv_canvas_create(layer);
    lv_obj_center(canvas);
    wallpaper_decode_into(canvas);
    return layer;
}

/* ================= clock/weather widget (pull-down layer only) ================= */

#define CARD_BG_OPA   LV_OPA_TRANSP
#define CARD_RADIUS   14
#define FRAME_OPA     LV_OPA_40

#define ICON_SCALE_SMALL   50
#define ICON_SCALE_MED     70

static lv_obj_t *card_create(lv_obj_t *parent, lv_align_t align,
                             int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_align(c, align, x, y);
    lv_obj_set_style_bg_color(c, COL_DARK, 0);
    lv_obj_set_style_bg_opa(c, CARD_BG_OPA, 0);
    lv_obj_set_style_radius(c, CARD_RADIUS, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_scrollable(c, false);
    return c;
}

static lv_obj_t *mk_label(lv_obj_t *p, lv_color_t color, const lv_font_t *font,
                          lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *lb = lv_label_create(p);
    lv_label_set_text(lb, "--");
    lv_obj_set_style_text_color(lb, color, 0);
    lv_obj_set_style_text_font(lb, font, 0);
    lv_obj_align(lb, align, x, y);
    return lb;
}

static lv_obj_t *mk_icon(lv_obj_t *parent, const void *src, uint32_t scale,
                         lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *ic = lv_img_create(parent);
    lv_image_set_src(ic, src);
    lv_image_set_scale(ic, scale);
    lv_obj_align(ic, align, x, y);
    return ic;
}

static void clock_widget_create(lv_obj_t *parent)
{
    s_root = parent;

    /* main frame: semi-transparent dark blue rounded box */
    lv_obj_t *frame = lv_obj_create(parent);
    lv_obj_set_size(frame, 780, 460);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(frame, COL_DARK, 0);
    lv_obj_set_style_bg_opa(frame, FRAME_OPA, 0);
    lv_obj_set_style_radius(frame, 20, 0);
    lv_obj_set_style_border_color(frame, COL_CYAN, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_scrollable(frame, false);

    /* top bar */
    s_wifi_icon = mk_icon(frame, ICON_WIFI, ICON_SCALE_SMALL,
                          LV_ALIGN_TOP_LEFT, 12, 8);

    s_wifi_label = mk_label(frame, COL_TEXT, &lv_font_cn_30,
                            LV_ALIGN_TOP_LEFT, 56, 8);
    lv_label_set_text(s_wifi_label, "WiFi");

    s_ip_label = mk_label(frame, lv_color_hex(0xFBBF24), &lv_font_cn_30,
                          LV_ALIGN_TOP_LEFT, 56, 38);
    lv_label_set_text(s_ip_label, "--");

    lv_obj_t *title = mk_label(frame, lv_color_hex(0xFBBF24), &lv_font_cn_30,
                               LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(title, "ESP32-S31");

    /* center big clock */
    s_time_label = lv_label_create(frame);
    lv_label_set_text(s_time_label, "--:--:--");
    lv_obj_set_style_text_color(s_time_label, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_digital_96, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 100);

    /* date */
    s_date_label = mk_label(frame, COL_TEXT, &lv_font_cn_30,
                            LV_ALIGN_TOP_MID, 0, 200);

    /* bottom-left main weather card */
    lv_obj_t *main_card = card_create(frame, LV_ALIGN_BOTTOM_LEFT, 12, -16, 360, 180);
    lv_obj_t *city_lb = mk_label(main_card, COL_TEXT, &lv_font_cn_45,
                                 LV_ALIGN_TOP_LEFT, 0, 0);
    /* Guangzhou (UTF-8) */
    lv_label_set_text(city_lb, "\xe5\xb9\xbf\xe5\xb7\x9e");
    lv_obj_t *today_lb = mk_label(main_card, lv_color_hex(0xFBBF24), &lv_font_cn_30,
                                  LV_ALIGN_TOP_RIGHT, 0, 0);
    /* today weather */
    lv_label_set_text(today_lb, "\xe4\xbb\x8a\xe6\x97\xa5\xe5\xa4\xa9\xe6\xb0\x94");

    s_wx_icon = mk_icon(main_card, ICON_CLOUD, ICON_SCALE_MED,
                        LV_ALIGN_CENTER, -30, 5);

    /* temperature 45px, with C symbol */
    s_now_temp_label = lv_label_create(main_card);
    lv_label_set_text(s_now_temp_label, "--");
    lv_obj_set_style_text_color(s_now_temp_label, lv_color_hex(0xFFA500), 0);
    lv_obj_set_style_text_font(s_now_temp_label, &lv_font_cn_45, 0);
    lv_obj_align(s_now_temp_label, LV_ALIGN_TOP_MID, 40, 40);

    s_now_text_label = mk_label(main_card, COL_CYAN, &lv_font_cn_30,
                                LV_ALIGN_TOP_MID, 50, 90);
    lv_label_set_text(s_now_text_label, "--");

    s_hi_lo_label = mk_label(main_card, COL_TEXT, &lv_font_cn_30,
                              LV_ALIGN_BOTTOM_MID, 0, 0);

    /* right side 2x2 small cards */
    /* top-right: humidity */
    lv_obj_t *rh_card = card_create(frame, LV_ALIGN_TOP_LEFT, 384, 250, 188, 90);
    mk_icon(rh_card, ICON_HUMIDITY, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -30, 0);
    s_rh_label = mk_label(rh_card, COL_TEXT, &lv_font_cn_30,
                          LV_ALIGN_RIGHT_MID, 0, 0);

    /* middle-right: wind */
    lv_obj_t *wind_card = card_create(frame, LV_ALIGN_TOP_LEFT, 580, 250, 188, 90);
    mk_icon(wind_card, ICON_WIND, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -50, 0);
    s_wind_label = mk_label(wind_card, COL_TEXT, &lv_font_cn_20,
                            LV_ALIGN_RIGHT_MID, 0, 0);

    /* bottom-right 1: thermometer (real-time temp) */
    lv_obj_t *t_card = card_create(frame, LV_ALIGN_TOP_LEFT, 384, 350, 188, 90);
    mk_icon(t_card, ICON_TEMP, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -30, 0);
    s_aqi_label = mk_label(t_card, lv_color_hex(0xFFA500), &lv_font_cn_30,
                           LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_aqi_label, "--");

    /* bottom-right 2: sunrise/sunset */
    lv_obj_t *sun_card = card_create(frame, LV_ALIGN_TOP_LEFT, 580, 350, 188, 90);
    mk_icon(sun_card, ICON_SUNRISE, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -50, 0);
    s_sun_label = mk_label(sun_card, COL_TEXT, &lv_font_cn_20,
                           LV_ALIGN_RIGHT_MID, 0, 0);

    ESP_LOGI(TAG, "clock widget created");
}

/** Tick: refresh time / date / weather / WiFi status */

/* 左上角 WiFi 区：已连接 → 绿色 + SSID（截 12 字节防压到中间标题）+ IP；
 * 未连接 → 红色 "WiFi" + "--"。两个 tick 分支（时间无效/有效）共用。 */
static void wifi_status_refresh(void)
{
    if (wifi_manager_is_connected()) {
        char ip[16] = {0};
        wifi_manager_get_ip(ip, sizeof(ip));
        const char *ssid = wifi_manager_get_sta_ssid();
        char txt[40];
        if (ssid[0]) {
            snprintf(txt, sizeof(txt), "%.12s%s",
                     ssid, strlen(ssid) > 12 ? ".." : "");
        } else {
            snprintf(txt, sizeof(txt), "WiFi");
        }
        lv_obj_set_style_text_color(s_wifi_label, COL_GREEN, 0);
        lv_label_set_text(s_wifi_label, txt);
        lv_label_set_text(s_ip_label, ip);
    } else {
        lv_obj_set_style_text_color(s_wifi_label, COL_RED, 0);
        lv_label_set_text(s_wifi_label, "WiFi");
        lv_label_set_text(s_ip_label, "--");
    }
}

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_root) return;

    static int s_wifi_counter = 0;

    time_t now = 0;
    struct tm tmv = {0};
    char buf[64];

    bool valid = net_time_service_time_valid();
    if (!valid) {
        if ((s_wifi_counter++ % 5) == 0) {
            wifi_status_refresh();
        }
        return;
    }

    time(&now);
    localtime_r(&now, &tmv);

    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    lv_label_set_text(s_time_label, buf);

    /* date: YYYY MM DD + weekday in Chinese */
    strftime(buf, sizeof(buf), "%Y %m %d", &tmv);
    static const char *wd_cn[] = {
        "\xe6\x97\xa5",
        "\xe4\xb8\x80",
        "\xe4\xba\x8c",
        "\xe4\xb8\x89",
        "\xe5\x9b\x9b",
        "\xe4\xba\x94",
        "\xe5\x85\xad",
    };
    char wbuf[128];
    int y, mo, d;
    sscanf(buf, "%d %d %d", &y, &mo, &d);
    snprintf(wbuf, sizeof(wbuf), "%d\xe5\xb9\xb4%02d\xe6\x9c\x88%02d\xe6\x97\xa5 \xe6\x98\x9f\xe6\x9c\x9f%s",
             y, mo, d, wd_cn[tmv.tm_wday]);
    lv_label_set_text(s_date_label, wbuf);

    /* refresh WiFi every 5s */
    if ((s_wifi_counter++ % 5) == 0) {
        wifi_status_refresh();
    }

    /* weather */
    weather_data_t wx;
    net_time_service_get_weather(&wx);
    if (wx.ready && wx.day_count > 0) {
        int show_temp = wx.now_temp ? wx.now_temp : wx.days[0].t_max;
        snprintf(buf, sizeof(buf), "%d\xe2\x84\x83", show_temp);
        lv_label_set_text(s_now_temp_label, buf);
        lv_label_set_text(s_now_text_label, wx.now_text[0] ? wx.now_text : wx.days[0].text_en);
        snprintf(buf, sizeof(buf), "\xe6\x9c\x80\xe9\xab\x98%d \xe6\x9c\x80\xe4\xbd\x8e%d",
                 wx.days[0].t_max, wx.days[0].t_min);
        lv_label_set_text(s_hi_lo_label, buf);
        snprintf(buf, sizeof(buf), "%d%%", wx.humidity);
        lv_label_set_text(s_rh_label, buf);
        snprintf(buf, sizeof(buf), "%s%d\xe7\xba\xa7", wx.wind_dir, wx.wind_scale);
        lv_label_set_text(s_wind_label, buf);
        snprintf(buf, sizeof(buf), "%d\xe2\x84\x83", wx.now_temp);
        lv_label_set_text(s_aqi_label, buf);
        if (wx.sunrise[0] && wx.sunset[0]) {
            snprintf(buf, sizeof(buf), "%s/%s", wx.sunrise, wx.sunset);
            lv_label_set_text(s_sun_label, buf);
        }
        /* icon pick by weather text */
        const char *txt = wx.now_text[0] ? wx.now_text : wx.days[0].text_en;
        if (strstr(txt, "\xe6\x99\xb4") || strstr(txt, "Sun") || strstr(txt, "Clear")) {
            lv_img_set_src(s_wx_icon, ICON_SUN);
        } else if (strstr(txt, "\xe9\x9b\xa8") || strstr(txt, "\xe9\x9b\xb7") || strstr(txt, "Rain")) {
            lv_img_set_src(s_wx_icon, ICON_RAIN);
        } else {
            lv_img_set_src(s_wx_icon, ICON_CLOUD);
        }
    }
}

/* ---- animation helper ---- */
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

/* ---- pull-down: drag wallpaper from top edge ---- */
static void main_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = (p.y < PULL_TOP_ZONE);
}

static void main_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy > 0) {
        int32_t y = -BSP_LCD_V_RES + dy;
        if (y > 0) y = 0;
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
            overlay_open(OVERLAY_WALLPAPER);
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
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = true;
}

static void wp_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy < 0) {
        int32_t y = dy;
        if (y < -BSP_LCD_V_RES) y = -BSP_LCD_V_RES;
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
        overlay_close(OVERLAY_WALLPAPER);
    } else {
        wallpaper_animate_to(s_wallpaper, 0);
    }
    s_pull_armed = false;
}

void overlay_wallpaper_init(lv_obj_t *tab_btns)
{
    s_tab_btns = tab_btns;

    s_wallpaper = overlay_wallpaper_build(lv_layer_top());
    clock_widget_create(s_wallpaper);
    lv_obj_set_y(s_wallpaper, -BSP_LCD_V_RES);
    lv_obj_set_clickable(s_wallpaper, true);
    lv_obj_add_event_cb(s_wallpaper, wp_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_released_cb, LV_EVENT_RELEASED, NULL);

    if (s_tab_btns) {
        lv_obj_add_event_cb(s_tab_btns, main_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_tab_btns, main_pressing_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_tab_btns, main_released_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_set_gesture_bubble(s_tab_btns, true);
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
    s_root = NULL;
    s_wifi_icon = NULL;
    s_wifi_label = NULL;
    s_ip_label = NULL;
    s_time_label = NULL;
    s_date_label = NULL;
    s_wx_icon = NULL;
    s_now_temp_label = NULL;
    s_now_text_label = NULL;
    s_hi_lo_label = NULL;
    s_rh_label = NULL;
    s_wind_label = NULL;
    s_aqi_label = NULL;
    s_sun_label = NULL;

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
