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
#include "esp_littlefs.h"
#include "esp_heap_caps.h"

#include "page_system.h"
#include "ui_theme.h"
#include "ui_common.h"

/* 实时监视控件（页面销毁时清空，timer 同步删除） */
static lv_obj_t *s_storage_bar;
static lv_obj_t *s_storage_label;
static lv_obj_t *s_heap_label;
static lv_timer_t *s_stat_timer;

/* 跑在 LVGL task：读 littlefs 用量 + 堆空闲，刷新 UI */
static void system_stat_timer_cb(lv_timer_t *t)
{
    (void)t;
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        int pct = total ? (int)((used * 100U) / total) : 0;
        char buf[96];
        snprintf(buf, sizeof(buf), "Storage  %u / %u KB  (%d%%)",
                 (unsigned)(used / 1024), (unsigned)(total / 1024), pct);
        if (s_storage_label) {
            lv_label_set_text(s_storage_label, buf);
        }
        if (s_storage_bar) {
            lv_bar_set_value(s_storage_bar, pct, LV_ANIM_OFF);
        }
    }
    if (s_heap_label) {
        char hb[64];
        snprintf(hb, sizeof(hb), "Heap free  %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        lv_label_set_text(s_heap_label, hb);
    }
}

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

    /* ---- Flash / storage 实时监视卡片 ---- */
    lv_obj_t *scard = lv_obj_create(page);
    lv_obj_set_size(scard, BSP_LCD_H_RES - 40, 150);
    lv_obj_set_style_bg_color(scard, COL_CARD, 0);
    lv_obj_set_style_border_color(scard, COL_BORDER, 0);
    lv_obj_set_style_border_width(scard, 1, 0);
    lv_obj_set_style_radius(scard, 16, 0);
    lv_obj_set_style_pad_all(scard, 16, 0);
    lv_obj_set_flex_flow(scard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scard, 10, 0);

    s_storage_label = lv_label_create(scard);
    lv_label_set_text_static(s_storage_label, "Storage  -- KB");
    lv_obj_set_style_text_color(s_storage_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_storage_label, UI_FONT, 0);

    s_storage_bar = lv_bar_create(scard);
    lv_obj_set_size(s_storage_bar, BSP_LCD_H_RES - 72, 14);
    lv_bar_set_range(s_storage_bar, 0, 100);
    lv_obj_set_style_bg_color(s_storage_bar, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_storage_bar, COL_CYAN, LV_PART_INDICATOR);

    s_heap_label = lv_label_create(scard);
    lv_label_set_text_static(s_heap_label, "Heap free  -- KB");
    lv_obj_set_style_text_color(s_heap_label, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_heap_label, UI_FONT, 0);

    /* 1s 节拍刷新（跑在 LVGL task，安全） */
    s_stat_timer = lv_timer_create(system_stat_timer_cb, 1000, NULL);
    system_stat_timer_cb(s_stat_timer); /* 立刻刷一次 */

    return page;
}

static void system_destroy(lv_obj_t *page)
{
    if (s_stat_timer) {
        lv_timer_del(s_stat_timer);
        s_stat_timer = NULL;
    }
    s_storage_bar = NULL;
    s_storage_label = NULL;
    s_heap_label = NULL;
    lv_obj_del(page);
}

const Page_t Page_System = {
    .create = system_create,
    .destroy = system_destroy,
    .name = "System",
};
