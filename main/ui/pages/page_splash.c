/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_splash.c
 * @brief Boot splash page, refactored from the former ui_splash.c.
 */

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "page_splash.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "ui/overlays/overlay_wp_particles.h"

static const char *TAG = "SPLASH";

#define SWIPE_UP_DY   (-80)   /* swipe-up threshold (px), instant enter */

static page_splash_enter_cb_t s_enter_cb;
static lv_obj_t *s_hint;
static lv_obj_t *s_wallpaper;
static int32_t   s_press_start_y;
static bool      s_entered;

static void splash_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_press_start_y = p.y;
    }
}

static void splash_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (s_entered) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_press_start_y;

    if (s_hint) {
        int32_t move = dy / 4;
        if (move < 0) {
            lv_obj_set_y(s_hint, BSP_LCD_V_RES - 90 + move);
        }
    }

    if (dy < SWIPE_UP_DY) {
        s_entered = true;
        overlay_wp_particles_stop();
        ESP_LOGI(TAG, "swipe up detected, entering main UI");
        if (s_enter_cb) {
            s_enter_cb();
        }
    }
}

static lv_obj_t *splash_create(lv_obj_t *parent)
{
    s_entered = false;

    lv_obj_t *page = ui_common_page_root(parent);

    s_wallpaper = overlay_wp_particles_build(page);

    /* ⚠ 事件必须挂在**粒子画布**上，不能挂在 page / s_wallpaper 上（F31）。
     *   画布铺满全屏且 CLICKABLE，LVGL 的命中测试停在它身上，
     *   挂在它祖先上的 PRESSED/PRESSING 永远收不到 ——
     *   表现为"上拉要划两次才对"，第一次的位移全被吞掉。 */
    lv_obj_t *touch_layer = overlay_wp_particles_canvas();
    if (!touch_layer) {
        touch_layer = page;   /* 画布建失败（PSRAM 不足）时兜底 */
    }
    lv_obj_add_flag(touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch_layer, splash_pressed_cb,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(touch_layer, splash_pressing_cb, LV_EVENT_PRESSING, NULL);

    s_hint = lv_label_create(page);
    lv_label_set_text(s_hint, LV_SYMBOL_UP"  SWIPE UP TO ENTER");
    lv_obj_set_style_text_color(s_hint, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT, 0);
    lv_obj_set_style_text_letter_space(s_hint, 2, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    return page;
}

static void splash_destroy(lv_obj_t *page)
{
    overlay_wp_particles_stop();
    lv_obj_del(page);
    s_hint = NULL;
    s_wallpaper = NULL;
}

void page_splash_set_enter_cb(page_splash_enter_cb_t cb)
{
    s_enter_cb = cb;
}

const Page_t Page_Splash = {
    .create = splash_create,
    .destroy = splash_destroy,
    .name = "Splash",
};
