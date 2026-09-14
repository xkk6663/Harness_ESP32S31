/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_system.c
 * @brief System info page.
 */

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_idf_version.h"

#include "page_system.h"
#include "ui_theme.h"
#include "ui_common.h"

static lv_obj_t *system_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 16, 0);
    lv_obj_set_style_pad_row(page, 10, 0);

    ui_common_title_bar(page, LV_SYMBOL_SETTINGS"  System");

    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, 240);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    char buf[96];
    lv_obj_t *l;

    l = lv_label_create(card);
    lv_label_set_text_static(l, "Board: ESP32-S31-Korvo-1 V1.1");
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    snprintf(buf, sizeof(buf), "IDF: %s", esp_get_idf_version());
    l = lv_label_create(card);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    l = lv_label_create(card);
    lv_label_set_text_static(l, "LCD: 800x480");
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    l = lv_label_create(card);
    lv_label_set_text_static(l, "Audio: ES8389 16kHz/16bit");
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    l = lv_label_create(card);
    lv_label_set_text_static(l, "GitHub: xkk6663");
    lv_obj_set_style_text_color(l, COL_CYAN, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    return page;
}

static void system_destroy(lv_obj_t *page)
{
    lv_obj_del(page);
}

const Page_t Page_System = {
    .create = system_create,
    .destroy = system_destroy,
    .name = "System",
};
