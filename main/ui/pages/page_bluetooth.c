/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_bluetooth.c
 * @brief Bluetooth 子页面：电源开关 + A2DP Sink/Source 切换 + Source 设备搜索/连接。
 *
 * 事件来自 BT/GMF 线程，回调里只置标志并缓存数据，LVGL 在 200ms lv_timer 里刷新。
 * 蓝牙默认关，打开才 esp_bt_audio_init；关闭即 deinit 释放内存。
 */

#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "page_bluetooth.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "middleware/bluetooth/bt_audio_service.h"

#define MAX_FOUND 12

static const char *TAG = "PAGE_BT";

static lv_obj_t *s_state_label;
static lv_obj_t *s_bt_sw;
static lv_obj_t *s_role_btn;
static lv_obj_t *s_role_lab;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_dev_list;
static lv_timer_t *s_poll_timer;

static volatile bool s_need_refresh;
static volatile bool s_state_changed;
static char s_state_txt[48];

static bt_device_t s_devs[MAX_FOUND];
static int s_dev_cnt = 0;

/* ---- BT 事件 -> 只置标志/缓存，不碰 LVGL ---- */
static void bt_evt_cb(bt_evt_t evt, const void *data, void *ctx)
{
    (void)ctx;
    switch (evt) {
    case BT_EVT_ON:
        snprintf(s_state_txt, sizeof(s_state_txt), "Bluetooth ON");
        s_state_changed = true;
        break;
    case BT_EVT_OFF:
        snprintf(s_state_txt, sizeof(s_state_txt), "Bluetooth OFF");
        s_state_changed = true;
        break;
    case BT_EVT_DISCOVERY_START:
        snprintf(s_state_txt, sizeof(s_state_txt), "Scanning...");
        s_state_changed = true;
        break;
    case BT_EVT_DEVICE_FOUND: {
        if (data && s_dev_cnt < MAX_FOUND) {
            const bt_device_t *d = (const bt_device_t *)data;
            s_devs[s_dev_cnt] = *d;
            s_dev_cnt++;
            s_need_refresh = true;
        }
        break;
    }
    case BT_EVT_CONNECTED:
        snprintf(s_state_txt, sizeof(s_state_txt), "Connected");
        s_state_changed = true;
        break;
    case BT_EVT_DISCONNECTED:
        snprintf(s_state_txt, sizeof(s_state_txt), "Disconnected");
        s_state_changed = true;
        break;
    case BT_EVT_STREAM_ON:
        snprintf(s_state_txt, sizeof(s_state_txt), "Streaming");
        s_state_changed = true;
        break;
    /* ★ 2026-09-22 补：此前只处理 STREAM_ON，播完自动收尾后状态文本会卡在
       "Streaming" —— 连接其实还在，只是推流结束，应回落到 "Connected"。 */
    case BT_EVT_STREAM_OFF:
        snprintf(s_state_txt, sizeof(s_state_txt), "Connected");
        s_state_changed = true;
        break;
    default:
        break;
    }
}

static void dev_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_dev_cnt) return;
    snprintf(s_state_txt, sizeof(s_state_txt), "Connecting...");
    s_state_changed = true;
    bt_audio_service_connect(s_devs[idx].addr);
}

static void refresh_dev_list(void)
{
    lv_obj_clean(s_dev_list);
    for (int i = 0; i < s_dev_cnt; i++) {
        lv_obj_t *btn = lv_button_create(s_dev_list);
        lv_obj_set_size(btn, BSP_LCD_H_RES - 110, 40);
        lv_obj_set_style_bg_color(btn, COL_CARD, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_t *lab = lv_label_create(btn);
        const char *nm = s_devs[i].name[0] ? s_devs[i].name : "(unknown)";
        lv_label_set_text_fmt(lab, "%s  (%ld dBm)", nm, (long)s_devs[i].rssi);
        lv_obj_set_style_text_color(lab, COL_TEXT, 0);
        lv_obj_center(lab);
        lv_obj_add_event_cb(btn, dev_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s_dev_cnt == 0) {
        lv_obj_t *empty = lv_label_create(s_dev_list);
        lv_label_set_text_static(empty, "No device yet");
        lv_obj_set_style_text_color(empty, COL_SUBTEXT, 0);
    }
}

static void bt_sw_cb(lv_event_t *e)
{
    (void)e;
    bool on = lv_obj_has_state(s_bt_sw, LV_STATE_CHECKED);
    esp_err_t ret = bt_audio_service_set_power(on);

    if (on && ret != ESP_OK) {
        /* 打开失败（多半是内部 RAM 不足，被主动拒绝）：把开关弹回 OFF，
           否则 UI 显示"已开"而实际没起来，用户会一直等扫描结果。 */
        ESP_LOGW(TAG, "BT power on failed: %s", esp_err_to_name(ret));
        lv_obj_remove_event_cb(s_bt_sw, bt_sw_cb);
        lv_obj_remove_state(s_bt_sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_bt_sw, bt_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
        snprintf(s_state_txt, sizeof(s_state_txt),
                 ret == ESP_ERR_NO_MEM ? "Not enough RAM" : "Power on failed");
        s_state_changed = true;
        return;
    }

    if (!on) s_dev_cnt = 0;
    snprintf(s_state_txt, sizeof(s_state_txt), on ? "Powering on..." : "Powering off...");
    s_state_changed = true;
}

static void role_btn_cb(lv_event_t *e)
{
    (void)e;
    bt_role_t cur = bt_audio_service_get_role();
    bt_audio_service_set_role(cur == BT_ROLE_SOURCE ? BT_ROLE_SINK : BT_ROLE_SOURCE);
    s_state_changed = true;
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    s_dev_cnt = 0;
    bt_audio_service_start_discovery();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    bool on = bt_audio_service_is_on();
    bool sw_on = lv_obj_has_state(s_bt_sw, LV_STATE_CHECKED);
    if (sw_on != on) {
        lv_obj_remove_event_cb(s_bt_sw, bt_sw_cb);
        if (on) lv_obj_add_state(s_bt_sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_bt_sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_bt_sw, bt_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* 控件可见性：关蓝牙时只留开关 */
    if (on) {
        lv_obj_clear_flag(s_role_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_role_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dev_list, LV_OBJ_FLAG_HIDDEN);
    }

    /* 角色按钮文案 */
    if (on) {
        bt_role_t r = bt_audio_service_get_role();
        lv_label_set_text_fmt(s_role_lab, "Role: %s (tap)",
                              r == BT_ROLE_SOURCE ? "Source" : "Sink");
    }

    if (s_need_refresh) {
        s_need_refresh = false;
        refresh_dev_list();
    }
    if (s_state_changed) {
        s_state_changed = false;
        lv_label_set_text(s_state_label, s_state_txt);
    }
}

static lv_obj_t *bt_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_style_pad_top(page, 8, 0);

    ui_common_title_bar(page, LV_SYMBOL_BLUETOOTH"  Bluetooth");

    /* 状态 */
    s_state_label = lv_label_create(page);
    lv_label_set_text_static(s_state_label, "OFF");
    lv_obj_set_style_text_color(s_state_label, COL_SUBTEXT, 0);

    /* 电源开关 */
    lv_obj_t *pw_row = lv_obj_create(page);
    lv_obj_set_size(pw_row, BSP_LCD_H_RES - 80, 44);
    lv_obj_set_style_bg_color(pw_row, COL_DARK, 0);
    lv_obj_set_style_border_color(pw_row, COL_BORDER, 0);
    lv_obj_set_style_border_width(pw_row, 1, 0);
    lv_obj_set_style_radius(pw_row, 10, 0);
    lv_obj_set_flex_flow(pw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(pw_row, 12, 0);
    lv_obj_set_style_pad_right(pw_row, 12, 0);
    lv_obj_remove_flag(pw_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pw_lab = lv_label_create(pw_row);
    lv_label_set_text_static(pw_lab, LV_SYMBOL_BLUETOOTH"  Bluetooth Power");
    lv_obj_set_style_text_color(pw_lab, COL_TEXT, 0);
    s_bt_sw = lv_switch_create(pw_row);
    lv_obj_add_event_cb(s_bt_sw, bt_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 角色切换 */
    s_role_btn = lv_button_create(page);
    lv_obj_set_size(s_role_btn, BSP_LCD_H_RES - 80, 40);
    lv_obj_set_style_bg_color(s_role_btn, COL_CYAN, 0);
    lv_obj_set_style_radius(s_role_btn, 10, 0);
    s_role_lab = lv_label_create(s_role_btn);
    lv_obj_center(s_role_lab);
    lv_obj_set_style_text_color(s_role_lab, COL_DARK, 0);
    lv_obj_add_event_cb(s_role_btn, role_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 扫描 */
    s_scan_btn = lv_button_create(page);
    lv_obj_set_size(s_scan_btn, BSP_LCD_H_RES - 80, 44);
    lv_obj_set_style_bg_color(s_scan_btn, COL_CYAN, 0);
    lv_obj_set_style_radius(s_scan_btn, 10, 0);
    lv_obj_t *sl = lv_label_create(s_scan_btn);
    lv_label_set_text_static(sl, LV_SYMBOL_BLUETOOTH"  SEARCH DEVICE");
    lv_obj_set_style_text_color(sl, COL_DARK, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 设备列表 */
    s_dev_list = lv_obj_create(page);
    lv_obj_set_size(s_dev_list, BSP_LCD_H_RES - 80, 180);
    lv_obj_set_style_bg_color(s_dev_list, COL_DARK, 0);
    lv_obj_set_style_border_color(s_dev_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_dev_list, 1, 0);
    lv_obj_set_style_radius(s_dev_list, 10, 0);
    lv_obj_set_flex_flow(s_dev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dev_list, 4, 0);

    /* 注册回调（init 中间层，不打开射频） */
    bt_audio_service_init(bt_evt_cb, NULL);

    s_poll_timer = lv_timer_create(poll_cb, 200, NULL);
    return page;
}

static void bt_destroy(lv_obj_t *page)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    lv_obj_del(page);
    s_state_label = s_bt_sw = s_role_btn = s_role_lab = s_scan_btn = s_dev_list = NULL;
    s_dev_cnt = 0;
}

const Page_t Page_Bluetooth = {
    .create = bt_create,
    .destroy = bt_destroy,
    .name = "Bluetooth",
};
