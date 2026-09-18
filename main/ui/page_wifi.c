/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_wifi.c
 * @brief Wi-Fi page: start/scan button + AP list + on-screen keyboard to
 *        enter password and connect.
 *
 * Events from wifi_manager arrive on the esp event task; we only set flags
 * there and refresh LVGL from a periodic lv_timer (runs on the LVGL task).
 */

#include <string.h>

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "page_wifi.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "middleware/net/wifi_manager.h"

static lv_obj_t *s_state_label;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_ap_list;
static lv_obj_t *s_pwd_row;       /* textarea + keyboard container, hidden until AP picked */
static lv_obj_t *s_pwd_ta;
static lv_obj_t *s_kb;
static lv_obj_t *s_connect_btn;
static lv_timer_t *s_poll_timer;
static lv_obj_t *s_mode_btn;
static lv_obj_t *s_mode_lab;
static lv_obj_t *s_ap_info_label;
static lv_obj_t *s_clients_label;
static wifi_role_t s_last_role = (wifi_role_t)-1;
static int s_last_client_count = -1;

static char s_cur_ssid[WIFI_SSID_LEN_MAX + 1];
static char s_ip[16];

static volatile bool s_scan_done;
static volatile bool s_got_ip;
static volatile bool s_state_changed;
static char s_state_txt[40];

/* ---- wifi event -> flags only (do not touch LVGL here) ---- */
static void wifi_evt_cb(wifi_evt_t evt, const char *info)
{
    switch (evt) {
    case WIFI_EVT_SCAN_DONE:
        s_scan_done = true;
        break;
    case WIFI_EVT_GOT_IP:
        if (info) {
            strncpy(s_ip, info, sizeof(s_ip) - 1);
        }
        s_got_ip = true;
        break;
    case WIFI_EVT_CONNECTING:
        snprintf(s_state_txt, sizeof(s_state_txt), "Connecting...");
        s_state_changed = true;
        break;
    case WIFI_EVT_DISCONNECTED:
        snprintf(s_state_txt, sizeof(s_state_txt), "Disconnected");
        s_state_changed = true;
        break;
    case WIFI_EVT_ALL_FAILED:
        snprintf(s_state_txt, sizeof(s_state_txt), "Connect failed");
        s_state_changed = true;
        break;
    default:
        break;
    }
}

static void ap_click_cb(lv_event_t *e);

static void refresh_ap_list(void)
{
    /* clear old rows */
    lv_obj_clean(s_ap_list);

    wifi_ap_info_t buf[16];
    int n = wifi_manager_get_scan_results(buf, 16);
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_button_create(s_ap_list);
        lv_obj_set_size(btn, BSP_LCD_H_RES - 80, 44);
        lv_obj_set_style_bg_color(btn, COL_CARD, 0);
        lv_obj_set_style_bg_color(btn, COL_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 10, 0);

        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text_fmt(lab, "%s  (%ddBm)%s",
                              buf[i].ssid, buf[i].rssi,
                              buf[i].open ? " [OPEN]" : "");
        lv_obj_set_style_text_color(lab, COL_TEXT, 0);
        lv_obj_center(lab);

        /* stash ssid in user_data */
        char *ssid_copy = strdup(buf[i].ssid);
        lv_obj_set_user_data(btn, ssid_copy);
        lv_obj_add_event_cb(btn, ap_click_cb, LV_EVENT_CLICKED, ssid_copy);
    }
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_ap_list);
        lv_label_set_text_static(empty, "No AP found");
        lv_obj_set_style_text_color(empty, COL_SUBTEXT, 0);
    }
}

/* AP row clicked: remember ssid, reveal password + keyboard */
static void ap_click_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid) {
        return;
    }
    strncpy(s_cur_ssid, ssid, sizeof(s_cur_ssid) - 1);
    s_cur_ssid[sizeof(s_cur_ssid) - 1] = 0;

    lv_textarea_set_text(s_pwd_ta, "");
    snprintf(s_state_txt, sizeof(s_state_txt), "Selected: %s", ssid);
    s_state_changed = true;
    lv_obj_remove_flag(s_pwd_row, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_pwd_ta);
}

static void mode_btn_cb(lv_event_t *e)
{
    (void)e;
    wifi_role_t cur = wifi_manager_get_role();
    wifi_manager_set_role(cur == WIFI_ROLE_AP ? WIFI_ROLE_STA : WIFI_ROLE_AP);
}

static void start_btn_cb(lv_event_t *e)
{
    (void)e;
    snprintf(s_state_txt, sizeof(s_state_txt), "Scanning...");
    s_state_changed = true;
    wifi_manager_start_scan();
}

static void connect_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *pass = lv_textarea_get_text(s_pwd_ta);
    wifi_manager_connect(s_cur_ssid, pass);
    /* hide keyboard after connect */
    lv_obj_add_flag(s_pwd_row, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, NULL);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    /* 角色变化：按 AP/STA 显示对应控件 */
    wifi_role_t role = wifi_manager_get_role();
    if (role != s_last_role) {
        s_last_role = role;
        bool is_ap = (role == WIFI_ROLE_AP);
        lv_label_set_text(s_mode_lab, is_ap ? LV_SYMBOL_WIFI"  Mode: AP (tap->STA)"
                                            : LV_SYMBOL_WIFI"  Mode: STA (tap->AP)");
        if (is_ap) {
            lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pwd_row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_ap_info_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_clients_label, LV_OBJ_FLAG_HIDDEN);
            char ip[16];
            wifi_manager_get_ip(ip, sizeof(ip));
            lv_label_set_text_fmt(s_ap_info_label,
                                  "AP hotspot: %s\npassword: %s\nboard IP: %s",
                                  wifi_manager_get_ap_ssid(), "800180Xiao",
                                  ip[0] ? ip : "starting...");
            s_last_client_count = -1;   /* 触发刷新一次 */
        } else {
            lv_obj_clear_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ap_info_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_clients_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* AP 模式下每轮刷新客户端连接状态与 IP（IP 是后到的，不能只看 count） */
    if (wifi_manager_get_role() == WIFI_ROLE_AP) {
        int n = wifi_manager_get_client_count();
        char ips[4][16];
        int got = wifi_manager_get_client_ips(ips, 4);
        char txt[128];
        if (n == 0) {
            snprintf(txt, sizeof(txt), "Clients: 0\nwaiting for device...");
        } else {
            snprintf(txt, sizeof(txt), "Clients: %d", n);
            for (int i = 0; i < got; i++) {
                const char *ip = ips[i][0] ? ips[i] : "(no ip yet)";
                snprintf(txt + strlen(txt), sizeof(txt) - strlen(txt),
                         "\n  %s", ip);
            }
        }
        lv_label_set_text(s_clients_label, txt);
    }

    if (s_scan_done) {
        s_scan_done = false;
        refresh_ap_list();
    }
    if (s_got_ip) {
        s_got_ip = false;
        snprintf(s_state_txt, sizeof(s_state_txt), "Connected: %s", s_ip);
        s_state_changed = true;
    }
    if (s_state_changed) {
        s_state_changed = false;
        lv_label_set_text(s_state_label, s_state_txt);
    }
}

static lv_obj_t *wifi_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_style_pad_top(page, 8, 0);

    ui_common_title_bar(page, LV_SYMBOL_WIFI"  WIFI");

    /* status */
    s_state_label = lv_label_create(page);
    lv_label_set_text_static(s_state_label, "Not started");
    lv_obj_set_style_text_color(s_state_label, COL_SUBTEXT, 0);

    /* role toggle: AP <-> STA */
    s_mode_btn = lv_button_create(page);
    lv_obj_set_size(s_mode_btn, BSP_LCD_H_RES - 80, 40);
    lv_obj_set_style_bg_color(s_mode_btn, COL_CYAN, 0);
    lv_obj_set_style_radius(s_mode_btn, 10, 0);
    s_mode_lab = lv_label_create(s_mode_btn);
    lv_obj_center(s_mode_lab);
    lv_obj_set_style_text_color(s_mode_lab, COL_DARK, 0);
    lv_obj_add_event_cb(s_mode_btn, mode_btn_cb, LV_EVENT_CLICKED, NULL);

    /* AP info (only visible in AP mode) */
    s_ap_info_label = lv_label_create(page);
    lv_label_set_text_static(s_ap_info_label, "");
    lv_obj_set_style_text_color(s_ap_info_label, COL_TEXT, 0);
    lv_obj_add_flag(s_ap_info_label, LV_OBJ_FLAG_HIDDEN);

    /* AP 客户端列表（仅 AP 模式可见） */
    s_clients_label = lv_label_create(page);
    lv_label_set_text_static(s_clients_label, "");
    lv_obj_set_style_text_color(s_clients_label, COL_TEXT, 0);
    lv_obj_add_flag(s_clients_label, LV_OBJ_FLAG_HIDDEN);

    /* start / scan button */
    s_start_btn = lv_button_create(page);
    lv_obj_set_size(s_start_btn, BSP_LCD_H_RES - 80, 44);
    lv_obj_set_style_bg_color(s_start_btn, COL_CYAN, 0);
    lv_obj_set_style_radius(s_start_btn, 10, 0);
    lv_obj_t *sl = lv_label_create(s_start_btn);
    lv_label_set_text_static(sl, LV_SYMBOL_WIFI"  START & SCAN");
    lv_obj_set_style_text_color(sl, COL_DARK, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_start_btn, start_btn_cb, LV_EVENT_CLICKED, NULL);

    /* AP list (scrollable, flexible) */
    s_ap_list = lv_obj_create(page);
    lv_obj_set_size(s_ap_list, BSP_LCD_H_RES - 80, 140);
    lv_obj_set_style_bg_color(s_ap_list, COL_DARK, 0);
    lv_obj_set_style_border_color(s_ap_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_ap_list, 1, 0);
    lv_obj_set_style_radius(s_ap_list, 10, 0);
    lv_obj_set_flex_flow(s_ap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ap_list, 4, 0);

    /* password row: textarea + keyboard (hidden until an AP is picked) */
    s_pwd_row = lv_obj_create(page);
    lv_obj_set_size(s_pwd_row, BSP_LCD_H_RES - 80, 230);
    lv_obj_set_style_bg_color(s_pwd_row, COL_CARD, 0);
    lv_obj_set_style_border_color(s_pwd_row, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_pwd_row, 1, 0);
    lv_obj_set_style_radius(s_pwd_row, 10, 0);
    lv_obj_set_flex_flow(s_pwd_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_pwd_row, 4, 0);
    lv_obj_add_flag(s_pwd_row, LV_OBJ_FLAG_HIDDEN);

    s_pwd_ta = lv_textarea_create(s_pwd_row);
    lv_obj_set_size(s_pwd_ta, BSP_LCD_H_RES - 110, 36);
    lv_textarea_set_placeholder_text(s_pwd_ta, "Password");
    /* password visible (no mask) */

    s_kb = lv_keyboard_create(s_pwd_row);
    lv_obj_set_size(s_kb, BSP_LCD_H_RES - 110, 160);

    s_connect_btn = lv_button_create(s_pwd_row);
    lv_obj_set_size(s_connect_btn, BSP_LCD_H_RES - 110, 34);
    lv_obj_set_style_bg_color(s_connect_btn, COL_GREEN, 0);
    lv_obj_set_style_radius(s_connect_btn, 8, 0);
    lv_obj_t *cl = lv_label_create(s_connect_btn);
    lv_label_set_text_static(cl, "CONNECT");
    lv_obj_center(cl);
    lv_obj_add_event_cb(s_connect_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    /* register wifi event flags */
    wifi_manager_set_evt_cb(wifi_evt_cb);

    /* reflect current connection state on entry */
    {
        char ip[16];
        wifi_manager_get_ip(ip, sizeof(ip));
        if (ip[0]) {
            snprintf(s_state_txt, sizeof(s_state_txt), "Connected: %s", ip);
        } else {
            snprintf(s_state_txt, sizeof(s_state_txt), "Not started");
        }
        lv_label_set_text(s_state_label, s_state_txt);
    }

    s_poll_timer = lv_timer_create(poll_cb, 200, NULL);

    return page;
}

static void wifi_destroy(lv_obj_t *page)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    lv_obj_del(page);
    s_state_label = s_start_btn = s_ap_list = s_pwd_row = NULL;
    s_pwd_ta = s_kb = s_connect_btn = NULL;
    s_mode_btn = s_mode_lab = s_ap_info_label = s_clients_label = NULL;
    s_last_role = (wifi_role_t)-1;
    s_last_client_count = -1;
}

const Page_t Page_Wifi = {
    .create = wifi_create,
    .destroy = wifi_destroy,
    .name = "Wifi",
};
