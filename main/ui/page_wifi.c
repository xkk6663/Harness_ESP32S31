/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_wifi.c
 * @brief WIFI settings page (placeholder until wifi_manager lands).
 */

#include "lvgl.h"

#include "page_wifi.h"
#include "ui_common.h"

static lv_obj_t *wifi_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 16, 0);

    ui_common_title_bar(page, LV_SYMBOL_WIFI"  WIFI");
    ui_common_placeholder(page, "Wi-Fi STA manager\ncoming in net stage");
    return page;
}

static void wifi_destroy(lv_obj_t *page)
{
    lv_obj_del(page);
}

const Page_t Page_Wifi = {
    .create = wifi_create,
    .destroy = wifi_destroy,
    .name = "Wifi",
};
