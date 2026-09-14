/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_bluetooth.c
 * @brief Bluetooth settings page (placeholder).
 */

#include "lvgl.h"

#include "page_bluetooth.h"
#include "ui_common.h"

static lv_obj_t *bt_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 16, 0);

    ui_common_title_bar(page, LV_SYMBOL_BLUETOOTH"  Bluetooth");
    ui_common_placeholder(page, "Bluetooth profile\ncoming in net stage");
    return page;
}

static void bt_destroy(lv_obj_t *page)
{
    lv_obj_del(page);
}

const Page_t Page_Bluetooth = {
    .create = bt_create,
    .destroy = bt_destroy,
    .name = "Bluetooth",
};
