/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_rgb.c
 * @brief RGB LED control page: on/off switch + R/G/B color sliders.
 */

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "page_rgb.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "middleware/led_service.h"

static lv_obj_t *s_switch;
static lv_obj_t *s_r_slider;
static lv_obj_t *s_g_slider;
static lv_obj_t *s_b_slider;
static lv_obj_t *s_preview;
static lv_obj_t *s_hex_label;

/* apply current slider values to the LED (only when power switch is ON) */
static void apply_color(void)
{
    int r = lv_slider_get_value(s_r_slider);
    int g = lv_slider_get_value(s_g_slider);
    int b = lv_slider_get_value(s_b_slider);
    if (s_preview) {
        lv_obj_set_style_bg_color(s_preview, lv_color_make(r, g, b), 0);
    }
    if (s_hex_label) {
        lv_label_set_text_fmt(s_hex_label, "#%02X%02X%02X", r, g, b);
    }
    /* only push color to the hardware when the LED power switch is on */
    bool on = s_switch && lv_obj_has_state(s_switch, LV_STATE_CHECKED);
    if (on) {
        /* turn on (task keeps s_led_on flag) then push color */
        led_service_set_on(true);
        led_service_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
}

static void slider_event_cb(lv_event_t *e)
{
    (void)e;
    apply_color();
}

static void switch_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (on) {
        /* re-light with the currently selected color */
        apply_color();
    } else {
        led_service_set_on(false);
    }
    /* dim preview when off */
    if (s_preview) {
        lv_obj_set_style_bg_opa(s_preview, on ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

/* one color row: label + slider */
static lv_obj_t *make_channel_row(lv_obj_t *parent, const char *name,
                                  lv_color_t color, lv_obj_t **slider_out,
                                  int32_t value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, 56);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lab = lv_label_create(row);
    lv_label_set_text_static(lab, name);
    lv_obj_set_style_text_color(lab, COL_TEXT, 0);
    lv_obj_set_style_text_font(lab, UI_FONT, 0);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_width(sl, BSP_LCD_H_RES - 180);
    lv_slider_set_range(sl, 0, 255);
    lv_slider_set_value(sl, value, false);
    lv_obj_set_style_bg_color(sl, COL_BORDER, 0);
    lv_obj_set_style_bg_color(sl, color, LV_PART_INDICATOR);
    lv_obj_add_event_cb(sl, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    *slider_out = sl;
    return row;
}

static lv_obj_t *rgb_create(lv_obj_t *parent)
{
    /* take over LED from main-interface status logic; power switch starts OFF */
    led_service_set_manual(true);
    led_service_set_on(false);

    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 16, 0);
    lv_obj_set_style_pad_row(page, 12, 0);

    ui_common_title_bar(page, LV_SYMBOL_IMAGE"  RGB LED");

    /* On/off row */
    lv_obj_t *sw_row = lv_obj_create(page);
    lv_obj_set_size(sw_row, BSP_LCD_H_RES - 40, 56);
    lv_obj_set_style_bg_color(sw_row, COL_CARD, 0);
    lv_obj_set_style_border_color(sw_row, COL_BORDER, 0);
    lv_obj_set_style_border_width(sw_row, 1, 0);
    lv_obj_set_style_radius(sw_row, 12, 0);
    lv_obj_set_style_pad_left(sw_row, 16, 0);
    lv_obj_set_style_pad_right(sw_row, 16, 0);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sw_label = lv_label_create(sw_row);
    lv_label_set_text_static(sw_label, LV_SYMBOL_POWER"  LED POWER");
    lv_obj_set_style_text_color(sw_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(sw_label, UI_FONT, 0);

    s_switch = lv_switch_create(sw_row);
    /* default OFF: user controls manually from here */
    lv_obj_set_style_bg_color(s_switch, COL_BORDER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_switch, COL_CYAN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_switch, switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Preview swatch + hex label */
    s_preview = lv_obj_create(page);
    lv_obj_set_size(s_preview, BSP_LCD_H_RES - 40, 64);
    lv_obj_set_style_radius(s_preview, 12, 0);
    lv_obj_set_style_border_color(s_preview, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_preview, 1, 0);
    lv_obj_set_style_pad_all(s_preview, 0, 0);
    s_hex_label = lv_label_create(s_preview);
    lv_obj_center(s_hex_label);
    lv_obj_set_style_text_color(s_hex_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_hex_label, UI_FONT, 0);

    /* R/G/B channels, default cyan 0x22D3EE = 34/211/238 */
    make_channel_row(page, "R", lv_color_hex(0xF87171), &s_r_slider, 34);
    make_channel_row(page, "G", lv_color_hex(0x34D399), &s_g_slider, 211);
    make_channel_row(page, "B", lv_color_hex(0x22D3EE), &s_b_slider, 238);

    /* push the initial color to the LED */
    apply_color();
    return page;
}

static void rgb_destroy(lv_obj_t *page)
{
    /* hand LED back to main-interface Wi-Fi status logic */
    led_service_set_manual(false);
    lv_obj_del(page);
    s_switch = NULL;
    s_r_slider = s_g_slider = s_b_slider = NULL;
    s_preview = NULL;
    s_hex_label = NULL;
}

const Page_t Page_Rgb = {
    .create = rgb_create,
    .destroy = rgb_destroy,
    .name = "Rgb",
};
