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
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_sys_entry.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "ui/ui_manager/page_manager.h"
#include "ui/pages/page_wifi.h"
#include "ui/pages/page_bluetooth.h"
#include "ui/pages/page_rgb.h"
#include "ui/pages/page_system.h"
#include "middleware/system/button_service.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/network/wifi_manager.h"
#include "middleware/system/sdcard_service.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "freertos/queue.h"

static lv_obj_t  *s_vol_slider;
static lv_timer_t *s_vol_timer;
static lv_obj_t *s_link_label;
static QueueHandle_t s_link_evt_q;

typedef struct {
    link_evt_t evt;
    char ip[16];
} link_ui_evt_t;

static void link_event_cb(link_evt_t evt, const char *ip)
{
    if (s_link_evt_q) {
        link_ui_evt_t message = {.evt = evt};
        snprintf(message.ip, sizeof(message.ip), "%s", ip ? ip : "");
        xQueueSend(s_link_evt_q, &message, 0);
    }
}

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
    /* 只写中央音量状态；出口由 button_service 的音量回调统一分发
       （蓝牙已连 → AVRCP 给耳机，否则 → 本地 codec） */
    button_service_set_volume(vol);
}

static void vol_timer_cb(lv_timer_t *t)
{
    (void)t;
    QueueHandle_t q = button_service_get_vol_queue();
    btn_vol_msg_t msg;
    while (q && xQueueReceive(q, &msg, 0) == pdPASS) {
        /* 只同步 UI 显示；音量出口已由 button_service 的音量回调分发，
           这里再发一次本地 codec 会在蓝牙模式下误调本地音量 */
        if (s_vol_slider) {
            lv_slider_set_value(s_vol_slider, msg.volume, true);
        }
    }
    link_ui_evt_t link_evt;
    while (s_link_evt_q && xQueueReceive(s_link_evt_q, &link_evt, 0) == pdPASS) {
        if (s_link_label) {
            if (link_evt.evt == LINK_EVT_ONLINE) {
                lv_label_set_text_fmt(s_link_label, LV_SYMBOL_OK "  PC online: %s", link_evt.ip);
                lv_obj_set_style_text_color(s_link_label, COL_GREEN, 0);
            } else {
                lv_label_set_text_static(s_link_label, LV_SYMBOL_CLOSE "  No PC peer");
                lv_obj_set_style_text_color(s_link_label, COL_RED, 0);
            }
        }
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
    lv_obj_set_scrollable(screen, false);
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

    s_link_label = lv_label_create(screen);
    lv_label_set_text_static(s_link_label, link_manager_is_online() ? LV_SYMBOL_OK "  PC online" : LV_SYMBOL_CLOSE "  No PC peer");
    lv_obj_set_style_text_color(s_link_label, link_manager_is_online() ? COL_GREEN : COL_RED, 0);
    lv_obj_set_style_text_font(s_link_label, UI_FONT, 0);

    /* 设备信息卡片（§69）：版本 / 编译 / 网络 / 存储 —— 数据全部来自
       esp_app_desc + wifi_manager + sdcard_service，构建时一次读取。 */
    lv_obj_t *info = lv_obj_create(screen);
    lv_obj_set_size(info, BSP_LCD_H_RES - 40, 150);
    lv_obj_set_style_bg_color(info, COL_CARD, 0);
    lv_obj_set_style_border_color(info, COL_BORDER, 0);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_radius(info, 16, 0);
    lv_obj_set_style_pad_left(info, 16, 0);
    lv_obj_set_style_pad_right(info, 16, 0);
    lv_obj_set_style_pad_top(info, 10, 0);
    lv_obj_set_style_pad_bottom(info, 10, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info, 6, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ip[16] = "-";
    wifi_manager_get_ip(ip, sizeof(ip));

    lv_obj_t *row;
    row = lv_label_create(info);
    lv_label_set_text_fmt(row, "固件  %s  ·  IDF %s", app->version, app->idf_ver);
    lv_obj_set_style_text_color(row, COL_CYAN, 0);
    lv_obj_set_style_text_font(row, UI_FONT, 0);

    row = lv_label_create(info);
    lv_label_set_text_fmt(row, "编译  %s %s", app->date, app->time);
    lv_obj_set_style_text_color(row, COL_TEXT, 0);
    lv_obj_set_style_text_font(row, UI_FONT, 0);

    row = lv_label_create(info);
    lv_label_set_text_fmt(row, "IP    %s  ·  SD %s", ip,
                          sdcard_service_is_mounted() ? "OK" : "NO");
    lv_obj_set_style_text_color(row, COL_TEXT, 0);
    lv_obj_set_style_text_font(row, UI_FONT, 0);

    row = lv_label_create(info);
    lv_label_set_text_fmt(row, "MAC   %02X:%02X:%02X:%02X:%02X:%02X",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_obj_set_style_text_color(row, COL_TEXT, 0);
    lv_obj_set_style_text_font(row, UI_FONT, 0);

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
    if (s_link_evt_q == NULL) {
        s_link_evt_q = xQueueCreate(8, sizeof(link_ui_evt_t));
    }
    link_manager_add_evt_cb(link_event_cb);
}

void tab_sys_entry_destroy(void)
{
    if (s_vol_timer) {
        lv_timer_del(s_vol_timer);
        s_vol_timer = NULL;
    }
    s_vol_slider = NULL;
    s_link_label = NULL;
}
