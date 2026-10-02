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
#include <stdlib.h>

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "page_wifi.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "middleware/net/wifi_manager.h"
#include "overlay_wallpaper.h"

static lv_obj_t *s_state_label;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_ap_list;
static lv_obj_t *s_pwd_row;       /* full-page connect sub-view, hidden until AP picked */
static lv_obj_t *s_pwd_ta;
static lv_obj_t *s_kb;
static lv_obj_t *s_connect_btn;
static lv_obj_t *s_ssid_lab;      /* target SSID shown in connect sub-view header */
static lv_obj_t *s_cancel_btn;
static lv_timer_t *s_poll_timer;
static lv_obj_t *s_power_btn;
static lv_obj_t *s_power_lab;
static lv_obj_t *s_ap_info_label;
static lv_obj_t *s_clients_label;
/* ★ §74 补充 4：APSTA 常开后"AP↔STA 模式切换"按钮已删 —— 热点常开 +
 * STA 自动连是唯一形态，二选一开关没有存在意义（还是上轮崩机的引爆点）。 */

static char s_cur_ssid[WIFI_SSID_LEN_MAX + 1];
static char s_ip[16];

static volatile bool s_scan_done;
static volatile bool s_got_ip;
static volatile bool s_state_changed;
static char s_state_txt[80];      /* "Connected: <32B ssid> (<15B ip>)" needs > 40 */

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

/* ---- connect sub-view (full-page password entry) ----
 * 点选 AP 后独占整页：隐藏主开关/模式/扫描/列表，pwd_row 放大占满剩余空间，
 * 保证键盘与 CONNECT 按钮完整可见可点（旧版 pwd_row 230px 被页面裁掉，键盘够不着）。 */
static void show_connect_view(bool enter)
{
    lv_obj_set_hidden(s_power_btn, enter);
    lv_obj_set_hidden(s_start_btn, enter);
    lv_obj_set_hidden(s_ap_list, enter);
    lv_obj_set_hidden(s_pwd_row, !enter);
    if (enter) {
        lv_keyboard_set_textarea(s_kb, s_pwd_ta);
    } else {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_textarea_set_text(s_pwd_ta, "");
    }
}

static void do_connect(void)
{
    const char *pass = lv_textarea_get_text(s_pwd_ta);
    wifi_manager_connect(s_cur_ssid, pass);
    show_connect_view(false);     /* 回列表视图，状态栏显示 Connecting... */
}

static void connect_btn_cb(lv_event_t *e)
{
    (void)e;
    do_connect();
}

/* 键盘回车（✓）键 → textarea 收到 LV_EVENT_READY，直接连，省去够按钮 */
static void ta_ready_cb(lv_event_t *e)
{
    (void)e;
    do_connect();
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    show_connect_view(false);
    snprintf(s_state_txt, sizeof(s_state_txt), "Cancelled");
    s_state_changed = true;
}

/* AP 行删除时释放 strdup 的 ssid（lv_obj_clean 只删对象，user_data 会漏） */
static void ap_row_delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

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

        /* stash ssid in user_data (freed on row delete) */
        char *ssid_copy = strdup(buf[i].ssid);
        lv_obj_set_user_data(btn, ssid_copy);
        lv_obj_add_event_cb(btn, ap_click_cb, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(btn, ap_row_delete_cb, LV_EVENT_DELETE, ssid_copy);
    }
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_ap_list);
        lv_label_set_text_static(empty, "No AP found");
        lv_obj_set_style_text_color(empty, COL_SUBTEXT, 0);
    }
}

/* AP row clicked: remember ssid, switch to full-page connect sub-view */
static void ap_click_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid) {
        return;
    }
    strncpy(s_cur_ssid, ssid, sizeof(s_cur_ssid) - 1);
    s_cur_ssid[sizeof(s_cur_ssid) - 1] = 0;

    lv_textarea_set_text(s_pwd_ta, "");
    lv_label_set_text(s_ssid_lab, s_cur_ssid);
    snprintf(s_state_txt, sizeof(s_state_txt), "Selected: %s", ssid);
    s_state_changed = true;
    show_connect_view(true);
}

static void power_btn_cb(lv_event_t *e)
{
    (void)e;
    bool on = !wifi_manager_is_enabled();
    wifi_manager_set_enabled(on);
    s_state_changed = true;
    snprintf(s_state_txt, sizeof(s_state_txt), on ? "WiFi ON" : "WiFi OFF");
}

static void start_btn_cb(lv_event_t *e)
{
    (void)e;
    snprintf(s_state_txt, sizeof(s_state_txt), "Scanning...");
    s_state_changed = true;
    wifi_manager_start_scan();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    /* wifi master switch button */
    bool w_on = wifi_manager_is_enabled();
    lv_obj_set_style_bg_color(s_power_btn, w_on ? COL_GREEN : COL_RED, 0);
    lv_label_set_text_static(s_power_lab, w_on ? LV_SYMBOL_OK"  WiFi: ON" : LV_SYMBOL_CLOSE"  WiFi: OFF");

    /* ★ §74 补充 4：APSTA 常开 —— 角色分支整段删除，AP 信息/客户端列表与
     * 扫描连接控件**同时**可见（热点常开 + 可连路由，本就是并存的两件事）。
     * 热点信息随客户端/IP 变化刷新（放进下方客户端刷新块）。 */

    /* AP 热点信息 + 客户端连接状态（角色恒为 AP/APSTA，始终刷新） */
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

        char ip[16];
        wifi_manager_get_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(s_ap_info_label,
                              "AP hotspot: %s\npassword: %s\nboard IP: %s",
                              wifi_manager_get_ap_ssid(), "800180Xiao",
                              ip[0] ? ip : "starting...");
    }

    if (s_scan_done) {
        s_scan_done = false;
        refresh_ap_list();
    }
    if (s_got_ip) {
        s_got_ip = false;
        snprintf(s_state_txt, sizeof(s_state_txt), "Connected: %s (%s)",
                 wifi_manager_get_sta_ssid(), s_ip);
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

    /* wifi master switch */
    s_power_btn = lv_button_create(page);
    lv_obj_set_size(s_power_btn, BSP_LCD_H_RES - 80, 40);
    lv_obj_set_style_radius(s_power_btn, 10, 0);
    s_power_lab = lv_label_create(s_power_btn);
    lv_obj_center(s_power_lab);
    lv_obj_set_style_text_color(s_power_lab, COL_DARK, 0);
    lv_obj_add_event_cb(s_power_btn, power_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ★ §74 补充 4：APSTA 常开 —— 热点信息/客户端列表与扫描控件常驻同屏，
     * 不再有"模式切换"按钮 */
    s_ap_info_label = lv_label_create(page);
    lv_label_set_text_static(s_ap_info_label, "");
    lv_obj_set_style_text_color(s_ap_info_label, COL_TEXT, 0);

    /* AP 客户端列表 */
    s_clients_label = lv_label_create(page);
    lv_label_set_text_static(s_clients_label, "");
    lv_obj_set_style_text_color(s_clients_label, COL_TEXT, 0);

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

    /* connect sub-view: ssid header + textarea + keyboard + connect button
     * (hidden until an AP is picked; then shown full-page over the list) */
    s_pwd_row = lv_obj_create(page);
    lv_obj_set_size(s_pwd_row, BSP_LCD_H_RES - 80, 340);
    lv_obj_set_style_bg_color(s_pwd_row, COL_CARD, 0);
    lv_obj_set_style_border_color(s_pwd_row, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_pwd_row, 1, 0);
    lv_obj_set_style_radius(s_pwd_row, 10, 0);
    lv_obj_set_style_pad_all(s_pwd_row, 8, 0);
    lv_obj_set_flex_flow(s_pwd_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_pwd_row, 4, 0);
    lv_obj_set_scrollable(s_pwd_row, false);
    lv_obj_set_hidden(s_pwd_row, true);

    /* header: target SSID + cancel */
    lv_obj_t *hdr = lv_obj_create(s_pwd_row);
    lv_obj_set_size(hdr, BSP_LCD_H_RES - 110, 32);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_scrollable(hdr, false);

    s_ssid_lab = lv_label_create(hdr);
    lv_label_set_text_static(s_ssid_lab, "");
    lv_obj_set_style_text_color(s_ssid_lab, COL_TEXT, 0);
    lv_obj_align(s_ssid_lab, LV_ALIGN_LEFT_MID, 0, 0);

    s_cancel_btn = lv_button_create(hdr);
    lv_obj_set_size(s_cancel_btn, 90, 30);
    lv_obj_set_style_bg_color(s_cancel_btn, COL_RED, 0);
    lv_obj_set_style_radius(s_cancel_btn, 8, 0);
    lv_obj_align(s_cancel_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *xl = lv_label_create(s_cancel_btn);
    lv_label_set_text_static(xl, LV_SYMBOL_CLOSE"  Cancel");
    lv_obj_set_style_text_color(xl, COL_DARK, 0);
    lv_obj_center(xl);
    lv_obj_add_event_cb(s_cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    s_pwd_ta = lv_textarea_create(s_pwd_row);
    lv_obj_set_size(s_pwd_ta, BSP_LCD_H_RES - 110, 36);
    lv_textarea_set_placeholder_text(s_pwd_ta, "Password");
    lv_obj_add_event_cb(s_pwd_ta, ta_ready_cb, LV_EVENT_READY, NULL);
    /* password visible (no mask) */

    s_kb = lv_keyboard_create(s_pwd_row);
    lv_obj_set_size(s_kb, BSP_LCD_H_RES - 110, 200);

    s_connect_btn = lv_button_create(s_pwd_row);
    lv_obj_set_size(s_connect_btn, BSP_LCD_H_RES - 110, 38);
    lv_obj_set_style_bg_color(s_connect_btn, COL_GREEN, 0);
    lv_obj_set_style_radius(s_connect_btn, 8, 0);
    lv_obj_t *cl = lv_label_create(s_connect_btn);
    lv_label_set_text_static(cl, LV_SYMBOL_OK"  CONNECT");
    lv_obj_center(cl);
    lv_obj_add_event_cb(s_connect_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    /* register wifi event flags */
    wifi_manager_set_evt_cb(wifi_evt_cb);

    /* reflect current connection state on entry */
    {
        char ip[16];
        wifi_manager_get_ip(ip, sizeof(ip));
        if (wifi_manager_is_connected() && ip[0]) {
            snprintf(s_state_txt, sizeof(s_state_txt), "Connected: %s (%s)",
                     wifi_manager_get_sta_ssid(), ip);
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
    s_ssid_lab = s_cancel_btn = NULL;
    s_ap_info_label = s_clients_label = NULL;
    s_power_btn = s_power_lab = NULL;
}

const Page_t Page_Wifi = {
    .create = wifi_create,
    .destroy = wifi_destroy,
    .name = "Wifi",
};
