/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_call.c
 * @brief CALL tab (UDP low-latency voice link placeholder).
 */

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_call.h"
#include "ui_theme.h"

static lv_obj_t *s_status_label;

static void call_btn_cb(lv_event_t *e)
{
    (void)e;
    /* TODO: stage 2 hook UDP call command */
    if (s_status_label) {
        lv_label_set_text(s_status_label, "UDP CALL  ->  waiting net");
    }
}

void tab_call_build(lv_obj_t *screen, lv_group_t *group)
{
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 14, 0);
    lv_obj_set_style_pad_top(screen, 20, 0);

    /* Header */
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, BSP_LCD_H_RES - 40, 48);
    lv_obj_set_style_bg_color(header, COL_CARD, 0);
    lv_obj_set_style_border_color(header, COL_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 12, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, 16, 0);
    lv_obj_set_style_pad_right(header, 16, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text_static(title, LV_SYMBOL_AUDIO"  VOICE LINK");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Status breathing dot */
    lv_obj_t *status_dot = lv_obj_create(header);
    lv_obj_set_size(status_dot, 12, 12);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(status_dot, COL_GREEN, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);

    /* Center card */
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, 260);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 18, 0);

    s_status_label = lv_label_create(card);
    lv_label_set_text_static(s_status_label, "STANDBY  |  no peer");
    lv_obj_set_style_text_color(s_status_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status_label, UI_FONT, 0);

    /* Big round call button */
    lv_obj_t *call_btn = lv_btn_create(card);
    lv_obj_set_size(call_btn, 120, 120);
    lv_obj_set_style_radius(call_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(call_btn, COL_CYAN, 0);
    lv_obj_set_style_shadow_color(call_btn, COL_CYAN, 0);
    lv_obj_set_style_shadow_width(call_btn, 18, 0);
    lv_obj_add_event_cb(call_btn, call_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(call_btn);
    lv_label_set_text_static(cl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(cl, COL_DARK, 0);
    lv_obj_set_style_text_font(cl, UI_FONT, 0);
    lv_obj_center(cl);

    /* Footer info bar */
    lv_obj_t *foot = lv_obj_create(screen);
    lv_obj_set_size(foot, BSP_LCD_H_RES - 40, 60);
    lv_obj_set_style_bg_color(foot, COL_CARD, 0);
    lv_obj_set_style_border_color(foot, COL_BORDER, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_radius(foot, 12, 0);
    lv_obj_set_style_pad_left(foot, 16, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *info = lv_label_create(foot);
    lv_label_set_text_static(info, "UDP 50000/50001  |  20ms/frame");
    lv_obj_set_style_text_color(info, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(info, UI_FONT, 0);

    if (group) {
        lv_group_add_obj(group, call_btn);
    }
}
