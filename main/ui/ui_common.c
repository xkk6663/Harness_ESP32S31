/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_common.c
 * @brief Reusable widget factory implementation.
 */

#include "bsp/esp-bsp.h"
#include "ui_common.h"
#include "ui_theme.h"
#include "page_manager.h"

#define TITLE_BAR_H   48
#define ROW_H         56

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    PageManager_Back();
}

lv_obj_t *ui_common_page_root(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(page, COL_BG, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    return page;
}

lv_obj_t *ui_common_title_bar(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, BSP_LCD_H_RES - 40, TITLE_BAR_H);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_radius(bar, 12, 0);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);

    lv_obj_t *back = lv_btn_create(bar);
    lv_obj_set_size(back, 36, 32);
    lv_obj_set_style_bg_color(back, COL_BORDER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text_static(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_center(bl);

    lv_obj_t *txt = lv_label_create(bar);
    lv_label_set_text(txt, title);
    lv_obj_set_style_text_color(txt, COL_CYAN, 0);
    lv_obj_set_style_text_font(txt, UI_FONT, 0);

    return bar;
}

lv_obj_t *ui_common_setting_row(lv_obj_t *parent, const char *icon,
                                const char *text, lv_event_cb_t click_cb,
                                void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, ROW_H);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (click_cb) {
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *left = lv_label_create(row);
    lv_label_set_text_fmt(left, "%s  %s", icon ? icon : "", text);
    lv_obj_set_style_text_color(left, COL_TEXT, 0);
    lv_obj_set_style_text_font(left, UI_FONT, 0);

    lv_obj_t *right = lv_label_create(row);
    lv_label_set_text_static(right, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(right, COL_SUBTEXT, 0);

    return row;
}

lv_obj_t *ui_common_placeholder(lv_obj_t *parent, const char *text)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, 200);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_center(card);

    lv_obj_t *lab = lv_label_create(card);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(lab, UI_FONT, 0);
    lv_obj_center(lab);
    return card;
}
