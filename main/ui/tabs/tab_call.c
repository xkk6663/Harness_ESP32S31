/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
/** @file tab_call.c @brief CALL tab with independent TX/RX toggle buttons. */
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "tab_call.h"
#include "ui/common/ui_theme.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/audio/udp_voice.h"

typedef struct { uint8_t kind; char ip[16]; } call_ui_evt_t;
enum {
    CALL_UI_LINK_ONLINE, CALL_UI_LINK_OFFLINE,
    CALL_UI_TX_ON, CALL_UI_TX_OFF,
    CALL_UI_RX_ON, CALL_UI_RX_OFF,
    CALL_UI_NO_PEER
};
static QueueHandle_t s_evt_q;
static lv_timer_t *s_timer;
static lv_obj_t *s_status;
static lv_obj_t *s_dot;
static lv_obj_t *s_tx_btn, *s_tx_label;
static lv_obj_t *s_rx_btn, *s_rx_label;
static int64_t s_started_us;

static void post(uint8_t kind, const char *ip) {
    if (!s_evt_q) return;
    call_ui_evt_t evt = {.kind = kind};
    if (ip) snprintf(evt.ip, sizeof(evt.ip), "%s", ip);
    xQueueSend(s_evt_q, &evt, 0);
}
static void on_link(link_evt_t evt, const char *ip) {
    post(evt == LINK_EVT_ONLINE ? CALL_UI_LINK_ONLINE : CALL_UI_LINK_OFFLINE, ip);
}
static void on_voice(voice_evt_t evt) {
    switch (evt) {
    case VOICE_EVT_TX_ON:  post(CALL_UI_TX_ON, NULL); break;
    case VOICE_EVT_TX_OFF: post(CALL_UI_TX_OFF, NULL); break;
    case VOICE_EVT_RX_ON:  post(CALL_UI_RX_ON, NULL); break;
    case VOICE_EVT_RX_OFF: post(CALL_UI_RX_OFF, NULL); break;
    case VOICE_EVT_NO_PEER: post(CALL_UI_NO_PEER, NULL); break;
    }
}

static void refresh_buttons(void) {
    if (s_tx_btn) {
        bool on = udp_voice_tx_active();
        lv_obj_set_style_bg_color(s_tx_btn, on ? COL_RED : COL_CYAN, 0);
        if (s_tx_label) lv_label_set_text_static(s_tx_label, on ? LV_SYMBOL_STOP : LV_SYMBOL_VOLUME_MAX);
    }
    if (s_rx_btn) {
        bool on = udp_voice_rx_active();
        lv_obj_set_style_bg_color(s_rx_btn, on ? COL_GREEN : COL_CYAN, 0);
        if (s_rx_label) lv_label_set_text_static(s_rx_label, on ? LV_SYMBOL_STOP : LV_SYMBOL_AUDIO);
    }
}

static void apply_timer(lv_timer_t *timer) {
    (void)timer; call_ui_evt_t evt;
    while (s_evt_q && xQueueReceive(s_evt_q, &evt, 0) == pdPASS) {
        if (!s_status || !s_dot) continue;
        if (evt.kind == CALL_UI_LINK_ONLINE) {
            lv_obj_set_style_bg_color(s_dot, COL_GREEN, 0);
            lv_label_set_text_fmt(s_status, "READY  |  PC: %s", evt.ip);
        } else if (evt.kind == CALL_UI_LINK_OFFLINE) {
            lv_obj_set_style_bg_color(s_dot, COL_RED, 0);
            if (udp_voice_tx_active()) udp_voice_post_cmd(VOICE_CMD_TX_OFF);
            if (udp_voice_rx_active()) udp_voice_post_cmd(VOICE_CMD_RX_OFF);
            lv_label_set_text_static(s_status, "STANDBY  |  No PC peer");
        } else if (evt.kind == CALL_UI_NO_PEER) {
            lv_label_set_text_static(s_status, "PC peer required");
        }
        refresh_buttons();
    }
    if (s_status && (udp_voice_tx_active() || udp_voice_rx_active())) {
        if (!s_started_us) s_started_us = esp_timer_get_time();
        int seconds = (int)((esp_timer_get_time() - s_started_us) / 1000000);
        lv_label_set_text_fmt(s_status, "ACTIVE  |  TX:%s RX:%s  %02d:%02d",
            udp_voice_tx_active() ? "ON" : "--", udp_voice_rx_active() ? "ON" : "--",
            seconds / 60, seconds % 60);
    } else {
        s_started_us = 0;
    }
}

static void tx_btn_cb(lv_event_t *e) {
    (void)e;
    udp_voice_post_cmd(udp_voice_tx_active() ? VOICE_CMD_TX_OFF : VOICE_CMD_TX_ON);
}
static void rx_btn_cb(lv_event_t *e) {
    (void)e;
    udp_voice_post_cmd(udp_voice_rx_active() ? VOICE_CMD_RX_OFF : VOICE_CMD_RX_ON);
}

void tab_call_build(lv_obj_t *screen, lv_group_t *group) {
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 14, 0);
    lv_obj_set_style_pad_top(screen, 20, 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, BSP_LCD_H_RES - 40, 48);
    lv_obj_set_style_bg_color(header, COL_CARD, 0);
    lv_obj_set_style_border_color(header, COL_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 12, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, 16, 0);
    lv_obj_set_style_pad_right(header, 16, 0);
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text_static(title, LV_SYMBOL_AUDIO "  VOICE LINK");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);
    s_dot = lv_obj_create(header);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, link_manager_is_online() ? COL_GREEN : COL_RED, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, 280);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_row(card, 18, 0);

    s_status = lv_label_create(card);
    lv_label_set_text_static(s_status, link_manager_is_online() ? "READY  |  PC connected" : "STANDBY  |  No PC peer");
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    /* TX button: device mic -> PC */
    s_tx_btn = lv_btn_create(card);
    lv_obj_set_size(s_tx_btn, 110, 110);
    lv_obj_set_style_radius(s_tx_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_tx_btn, COL_CYAN, 0);
    lv_obj_add_event_cb(s_tx_btn, tx_btn_cb, LV_EVENT_CLICKED, NULL);
    s_tx_label = lv_label_create(s_tx_btn);
    lv_label_set_text_static(s_tx_label, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(s_tx_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_tx_label, UI_FONT, 0);
    lv_obj_center(s_tx_label);

    /* RX button: PC -> device speaker */
    s_rx_btn = lv_btn_create(card);
    lv_obj_set_size(s_rx_btn, 110, 110);
    lv_obj_set_style_radius(s_rx_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rx_btn, COL_CYAN, 0);
    lv_obj_add_event_cb(s_rx_btn, rx_btn_cb, LV_EVENT_CLICKED, NULL);
    s_rx_label = lv_label_create(s_rx_btn);
    lv_label_set_text_static(s_rx_label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(s_rx_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_rx_label, UI_FONT, 0);
    lv_obj_center(s_rx_label);

    lv_obj_t *footer = lv_label_create(screen);
    lv_label_set_text_static(footer, "TX: mic->PC   RX: PC->spk   UDP 50000/50001");
    lv_obj_set_style_text_color(footer, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(footer, UI_FONT, 0);

    if (group) { lv_group_add_obj(group, s_tx_btn); lv_group_add_obj(group, s_rx_btn); }
    if (!s_evt_q) s_evt_q = xQueueCreate(8, sizeof(call_ui_evt_t));
    if (!s_timer) s_timer = lv_timer_create(apply_timer, 100, NULL);
    link_manager_add_evt_cb(on_link);
    udp_voice_set_evt_cb(on_voice);
}

void tab_call_destroy(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_status = s_dot = s_tx_btn = s_tx_label = s_rx_btn = s_rx_label = NULL;
}
