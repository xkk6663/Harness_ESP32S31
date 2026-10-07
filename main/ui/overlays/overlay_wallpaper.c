/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wallpaper.c
 * @brief Pull-down wallpaper overlay: drag gestures + clock/weather widget.
 *        §75：壁纸内容不再自解码 —— 借 wallpaper_service 双缓冲（gen 轮询 +
 *        crossfade + commit 归还），两个 canvas 分别绑 service 两个 buffer。
 */

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "overlay_wallpaper.h"
#include "overlay_manager.h"
#include "ui/common/ui_theme.h"
#include "middleware/media/wallpaper_service.h"
#include "middleware/network/net_time_service.h"
#include "middleware/network/wifi_manager.h"

LV_FONT_DECLARE(lv_font_digital_96);
LV_FONT_DECLARE(lv_font_cn_20);
LV_FONT_DECLARE(lv_font_cn_30);
LV_FONT_DECLARE(lv_font_cn_45);
LV_IMG_DECLARE(image_wifi);
LV_IMG_DECLARE(image_sun);
LV_IMG_DECLARE(image_cloud);
LV_IMG_DECLARE(image_temp);
LV_IMG_DECLARE(image_humidity);
LV_IMG_DECLARE(image_wind);
LV_IMG_DECLARE(image_sunrise);
LV_IMG_DECLARE(image_rain);

static const char *TAG = "WP_OVERLAY";

/* §75：壁纸池轮换 + crossfade 动画参数 */
#define WP_FADE_MS       800    /* 渐进换图时长 */
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

/* ---- §75 双缓冲壁纸 canvas：[0]/[1] 分别绑 wallpaper_service 两个 buffer ---- */
static lv_obj_t   *s_wp_cv[2];
static uint32_t    s_wp_gen_seen;
static bool        s_wp_fading;
static lv_obj_t   *s_wp_fading_cv;   /* 正在淡入的 canvas（deinit 时删动画用） */

/* ---- clock/weather widget on pull-down wallpaper ---- */
static lv_obj_t *s_root;
static lv_obj_t *s_clock_frame;     /* §75：crossfade 后把浮窗抬回最顶层用 */
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

/* ---- §75 crossfade：新图 0→255 淡入叠在旧图上，结束隐藏旧图并归还 service ---- */

static void wp_fade_exec_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void wp_fade_done_cb(lv_anim_t *a)
{
    lv_obj_t *old_cv = (lv_obj_t *)lv_anim_get_user_data(a);
    if (old_cv) {
        lv_obj_set_hidden(old_cv, true);
    }
    s_wp_fading = false;
    s_wp_fading_cv = NULL;
    wallpaper_service_commit();     /* 旧 buffer 已无人看，还给 service 当 work buffer */
}

/**
 * 代际号轮询（clock_tick 每秒一拍）：发现 service 换图 → crossfade。
 * 浮层打开时也照常生效（用户拍板：打开状态下能看到壁纸更换）。
 */
static void wallpaper_rotate_check(void)
{
    if (s_wp_fading || !s_wp_cv[0] || !s_wp_cv[1]) {
        return;     /* 降级态（单 canvas）不做 fade：直接等下一轮重绑 */
    }
    uint32_t gen = wallpaper_service_get_gen();
    if (gen == s_wp_gen_seen) {
        return;
    }
    s_wp_gen_seen = gen;

    int cur = wallpaper_service_get_current();
    int old = 1 - cur;
    lv_obj_t *new_cv = s_wp_cv[cur];
    lv_obj_t *old_cv = s_wp_cv[old];

    /* 新图垫底已在屏，先移到最上层再从全透明淡入。
     * ★ move_foreground 会顶到全部兄弟之上（含时钟浮窗 frame——同一父容器），
     *   紧接着必须把浮窗再抬回去，否则第一次 crossfade 后浮窗被新图永久盖住。 */
    lv_obj_set_hidden(new_cv, false);
    lv_obj_set_style_opa(new_cv, LV_OPA_TRANSP, 0);
    lv_obj_move_foreground(new_cv);
    if (s_clock_frame) {
        lv_obj_move_foreground(s_clock_frame);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, new_cv);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, WP_FADE_MS);
    lv_anim_set_exec_cb(&a, wp_fade_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, wp_fade_done_cb);
    lv_anim_set_user_data(&a, old_cv);
    lv_anim_start(&a);

    s_wp_fading = true;
    s_wp_fading_cv = new_cv;
    ESP_LOGI(TAG, "wallpaper crossfade -> buf%d (gen=%u)", cur, (unsigned)gen);
}

/* Build the pull-down wallpaper layer: two canvases over the service buffers. */
static lv_obj_t *wallpaper_layer_build(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(layer, COL_BG, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_radius(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_set_scrollable(layer, false);

    for (int i = 0; i < 2; i++) {
        uint8_t *buf = wallpaper_service_get_buffer(i);
        if (!buf) {
            s_wp_cv[i] = NULL;      /* work buffer 降级态：单 canvas 运行 */
            continue;
        }
        lv_obj_t *cv = lv_canvas_create(layer);
        /* ★ canvas 格式必须 == service 缓冲格式（RGB565，V2.1 定稿）。
         *   这是静态画布（只在 crossfade 时重绘），无 wp_particles 那种逐帧
         *   全屏转换的 wdt 风险。 */
        lv_canvas_set_buffer(cv, buf, BSP_LCD_H_RES, BSP_LCD_V_RES,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_center(cv);
        s_wp_cv[i] = cv;
    }

    /* 当前张全透明度显示并置顶；另一张隐藏作 work buffer */
    int cur = wallpaper_service_get_current();
    for (int i = 0; i < 2; i++) {
        if (!s_wp_cv[i]) {
            continue;
        }
        if (i == cur) {
            lv_obj_set_style_opa(s_wp_cv[i], LV_OPA_COVER, 0);
            lv_obj_move_foreground(s_wp_cv[i]);
        } else {
            lv_obj_set_style_opa(s_wp_cv[i], LV_OPA_TRANSP, 0);
            lv_obj_set_hidden(s_wp_cv[i], true);
        }
    }
    s_wp_gen_seen = wallpaper_service_get_gen();
    s_wp_fading = false;
    s_wp_fading_cv = NULL;
    ESP_LOGI(TAG, "wallpaper bound to service buffers (cur=buf%d, gen=%u)",
             cur, (unsigned)s_wp_gen_seen);
    return layer;
}

lv_obj_t *overlay_wallpaper_build(lv_obj_t *parent)
{
    /* §75：原"自解码 JPEG"版本已废弃 —— 现在直接绑 service 双缓冲。
     * 头文件 API 保留（splash 历史入口；现 splash 走粒子浮层）。 */
    return wallpaper_layer_build(parent);
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
    s_clock_frame = frame;      /* §75：crossfade 抬顶用 */
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
    s_wifi_icon = mk_icon(frame, &image_wifi, ICON_SCALE_SMALL,
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

    s_wx_icon = mk_icon(main_card, &image_cloud, ICON_SCALE_MED,
                        LV_ALIGN_LEFT_MID, -20, 5);

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
    mk_icon(rh_card, &image_humidity, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -30, 0);
    s_rh_label = mk_label(rh_card, COL_TEXT, &lv_font_cn_30,
                          LV_ALIGN_RIGHT_MID, 0, 0);

    /* middle-right: wind */
    lv_obj_t *wind_card = card_create(frame, LV_ALIGN_TOP_LEFT, 580, 250, 188, 90);
    mk_icon(wind_card, &image_wind, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -50, 0);
    s_wind_label = mk_label(wind_card, COL_TEXT, &lv_font_cn_20,
                            LV_ALIGN_RIGHT_MID, 0, 0);

    /* bottom-right 1: thermometer (real-time temp) */
    lv_obj_t *t_card = card_create(frame, LV_ALIGN_TOP_LEFT, 384, 350, 188, 90);
    mk_icon(t_card, &image_temp, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -30, 0);
    s_aqi_label = mk_label(t_card, lv_color_hex(0xFFA500), &lv_font_cn_30,
                           LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_aqi_label, "--");

    /* bottom-right 2: sunrise/sunset */
    lv_obj_t *sun_card = card_create(frame, LV_ALIGN_TOP_LEFT, 580, 350, 188, 90);
    mk_icon(sun_card, &image_sunrise, ICON_SCALE_SMALL,
            LV_ALIGN_CENTER, -50, 0);
    s_sun_label = mk_label(sun_card, COL_TEXT, &lv_font_cn_20,
                           LV_ALIGN_RIGHT_MID, 0, 0);

    ESP_LOGI(TAG, "clock widget created");
}

/** Tick: refresh time / date / weather / WiFi status */
static void clock_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_root) return;

    /* §75：代际号轮询（每秒一拍，代价可忽略）——发现新图即 crossfade。
     * 放在时间有效性 return 之前：没校时也要照常换壁纸。 */
    wallpaper_rotate_check();

    static int s_wifi_counter = 0;

    time_t now = 0;
    struct tm tmv = {0};
    char buf[64];

    bool valid = net_time_service_time_valid();
    if (!valid) {
        if ((s_wifi_counter++ % 5) == 0) {
            if (wifi_manager_is_connected()) {
                char ip[16] = {0};
                wifi_manager_get_ip(ip, sizeof(ip));
                lv_label_set_text(s_wifi_label, "WiFi");
                lv_obj_set_style_text_color(s_wifi_label, COL_GREEN, 0);
                lv_label_set_text(s_ip_label, ip);
            } else {
                lv_label_set_text(s_wifi_label, "WiFi");
                lv_obj_set_style_text_color(s_wifi_label, COL_RED, 0);
                lv_label_set_text(s_ip_label, "--");
            }
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
        if (wifi_manager_is_connected()) {
            char ip[16] = {0};
            wifi_manager_get_ip(ip, sizeof(ip));
            lv_obj_set_style_text_color(s_wifi_label, COL_GREEN, 0);
            lv_label_set_text(s_ip_label, ip);
        } else {
            lv_obj_set_style_text_color(s_wifi_label, COL_RED, 0);
            lv_label_set_text(s_ip_label, "--");
        }
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
            lv_img_set_src(s_wx_icon, &image_sun);
        } else if (strstr(txt, "\xe9\x9b\xa8") || strstr(txt, "\xe9\x9b\xb7") || strstr(txt, "Rain")) {
            lv_img_set_src(s_wx_icon, &image_rain);
        } else {
            lv_img_set_src(s_wx_icon, &image_cloud);
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

    /* §75：淡入动画若在播，先删动画并归还 service（否则 service 侧只能靠
     * 5s 超时兜底；显式归还让协议即时闭环）。lv_obj_del 之前做。 */
    if (s_wp_fading && s_wp_fading_cv) {
        lv_anim_del(s_wp_fading_cv, wp_fade_exec_cb);
        s_wp_fading = false;
        s_wp_fading_cv = NULL;
        wallpaper_service_commit();
    }
    s_wp_cv[0] = NULL;
    s_wp_cv[1] = NULL;

    s_root = NULL;
    s_clock_frame = NULL;
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
