/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_sys_entry.c
 * @brief SYS tab: volume slider + direct entry rows to the four resource
 *        sub-pages (WIFI / Bluetooth / RGB / System).
 */

#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_sys_entry.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "page_manager.h"
#include "page_wifi.h"
#include "page_bluetooth.h"
#include "page_rgb.h"
#include "page_system.h"
#include "middleware/audio_service.h"
#include "middleware/button_service.h"
#include "freertos/queue.h"

static lv_obj_t  *s_vol_slider;
static lv_timer_t *s_vol_timer;

/* sub-page row ids */
enum {
    ROW_WIFI = 0,
    ROW_BLUETOOTH,
    ROW_RGB,
    ROW_SYSTEM,
};

static void volume_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int vol = lv_slider_get_value(slider);
    button_service_set_volume(vol);
    audio_cmd_t cmd = {
        .id = AUDIO_CMD_SET_VOLUME,
        .value = vol,
    };
    audio_service_post_cmd(&cmd);
}

static void vol_timer_cb(lv_timer_t *t)
{
    (void)t;
    QueueHandle_t q = button_service_get_vol_queue();
    btn_vol_msg_t msg;
    while (q && xQueueReceive(q, &msg, 0) == pdPASS) {
        if (s_vol_slider) {
            lv_slider_set_value(s_vol_slider, msg.volume, true);
        }
        audio_cmd_t cmd = {
            .id = AUDIO_CMD_SET_VOLUME,
            .value = msg.volume,
        };
        audio_service_post_cmd(&cmd);
    }
}

static void row_click_cb(lv_event_t *e)
{
    uintptr_t id = (uintptr_t)lv_event_get_user_data(e);
    switch (id) {
    case ROW_WIFI:
        PageManager_Load(&Page_Wifi);
        break;
    case ROW_BLUETOOTH:
        PageManager_Load(&Page_Bluetooth);
        break;
    case ROW_RGB:
        PageManager_Load(&Page_Rgb);
        break;
    case ROW_SYSTEM:
        PageManager_Load(&Page_System);
        break;
    default:
        break;
    }
}

void tab_sys_entry_build(lv_obj_t *screen, lv_group_t *group)
{
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_top(screen, 16, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_SETTINGS"  SYSTEM");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Volume card */
    lv_obj_t *vol = lv_obj_create(screen);
    lv_obj_set_size(vol, BSP_LCD_H_RES - 40, 80);
    lv_obj_set_style_bg_color(vol, COL_CARD, 0);
    lv_obj_set_style_border_color(vol, COL_BORDER, 0);
    lv_obj_set_style_border_width(vol, 1, 0);
    lv_obj_set_style_radius(vol, 16, 0);
    lv_obj_set_style_pad_left(vol, 16, 0);
    lv_obj_set_style_pad_right(vol, 16, 0);
    lv_obj_set_flex_flow(vol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *l = lv_label_create(vol);
    lv_label_set_text_static(l, LV_SYMBOL_VOLUME_MAX"  VOL");
    lv_obj_set_style_text_color(l, COL_TEXT, 0);

    s_vol_slider = lv_slider_create(vol);
    lv_obj_set_width(s_vol_slider, BSP_LCD_H_RES - 220);
    lv_slider_set_range(s_vol_slider, 0, 90);
    lv_slider_set_value(s_vol_slider, button_service_get_volume(), false);
    lv_obj_set_style_bg_color(s_vol_slider, COL_BORDER, 0);
    lv_obj_set_style_bg_color(s_vol_slider, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_vol_slider, volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (group) {
        lv_group_add_obj(group, s_vol_slider);
    }

    /* Four resource sub-page entries */
    ui_common_setting_row(screen, LV_SYMBOL_WIFI, "WIFI", row_click_cb, (void *)(uintptr_t)ROW_WIFI);
    ui_common_setting_row(screen, LV_SYMBOL_BLUETOOTH, "Bluetooth", row_click_cb, (void *)(uintptr_t)ROW_BLUETOOTH);
    ui_common_setting_row(screen, LV_SYMBOL_IMAGE, "RGB LED", row_click_cb, (void *)(uintptr_t)ROW_RGB);
    ui_common_setting_row(screen, LV_SYMBOL_SETTINGS, "System", row_click_cb, (void *)(uintptr_t)ROW_SYSTEM);

    if (s_vol_timer == NULL) {
        s_vol_timer = lv_timer_create(vol_timer_cb, 100, NULL);
    }
}

void tab_sys_entry_destroy(void)
{
    if (s_vol_timer) {
        lv_timer_del(s_vol_timer);
        s_vol_timer = NULL;
    }
    s_vol_slider = NULL;
}
