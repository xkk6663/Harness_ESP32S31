/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
/** @file tab_record.c @brief Recording tab that only posts record-service commands. */
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "tab_record.h"
#include "ui_theme.h"
#include "middleware/audio/record_service.h"

static QueueHandle_t s_evt_q; static lv_timer_t *s_timer; static lv_obj_t *s_btn; static lv_obj_t *s_btn_label; static lv_obj_t *s_status; static lv_obj_t *s_bar;
static void on_record(const record_event_t *evt) { if (s_evt_q) xQueueSend(s_evt_q, evt, 0); }
static void drain(lv_timer_t *timer)
{
    (void)timer; record_event_t evt;
    while (s_evt_q && xQueueReceive(s_evt_q, &evt, 0) == pdPASS) {
        if (!s_btn || !s_status) continue;
        if (evt.event == RECORD_EVT_STARTED) { lv_label_set_text_static(s_btn_label, "STOP"); lv_label_set_text_static(s_status, evt.mode == RECORD_MODE_STREAM ? "STREAM  |  PC recording" : "LOCAL  |  offline cache"); }
        else if (evt.event == RECORD_EVT_PROGRESS) { lv_bar_set_value(s_bar, (int)(evt.bytes * 100U / evt.limit_bytes), LV_ANIM_ON); lv_label_set_text_fmt(s_status, "%s  |  %u s", evt.mode == RECORD_MODE_STREAM ? "STREAM" : "LOCAL", (unsigned)(evt.bytes / 32000U)); }
        else if (evt.event == RECORD_EVT_STOPPED) { lv_label_set_text_static(s_btn_label, "REC"); lv_label_set_text_static(s_status, "SAVED / synced"); lv_bar_set_value(s_bar, 0, LV_ANIM_ON); }
        else if (evt.event == RECORD_EVT_UPLOAD_DONE) lv_label_set_text_static(s_status, "LOCAL recording uploaded");
        else if (evt.event == RECORD_EVT_ERROR) lv_label_set_text_static(s_status, "Audio busy: stop CALL first");
    }
}
static void record_btn_cb(lv_event_t *e) { (void)e; record_service_post_cmd(record_service_is_active() ? RECORD_CMD_STOP : RECORD_CMD_START); }
void tab_record_build(lv_obj_t *screen, lv_group_t *group)
{
    lv_obj_set_scrollable(screen, false); lv_obj_set_style_bg_color(screen, COL_BG, 0); lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); lv_obj_set_style_pad_row(screen, 16, 0); lv_obj_set_style_pad_top(screen, 24, 0);
    lv_obj_t *title = lv_label_create(screen); lv_label_set_text_static(title, LV_SYMBOL_SAVE "  RECORD & SYNC"); lv_obj_set_style_text_color(title, COL_CYAN, 0); lv_obj_set_style_text_font(title, UI_FONT, 0);
    lv_obj_t *card = lv_obj_create(screen); lv_obj_set_size(card, BSP_LCD_H_RES - 40, 280); lv_obj_set_style_bg_color(card, COL_CARD, 0); lv_obj_set_style_border_color(card, COL_BORDER, 0); lv_obj_set_style_border_width(card, 1, 0); lv_obj_set_style_radius(card, 16, 0); lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); lv_obj_set_style_pad_row(card, 18, 0);
    s_status = lv_label_create(card); lv_label_set_text_static(s_status, "Ready  |  PC online = STREAM"); lv_obj_set_style_text_color(s_status, COL_TEXT, 0); lv_obj_set_style_text_font(s_status, UI_FONT, 0);
    s_btn = lv_btn_create(card); lv_obj_set_size(s_btn, 130, 100); lv_obj_set_style_radius(s_btn, 50, 0); lv_obj_set_style_bg_color(s_btn, COL_RED, 0); lv_obj_add_event_cb(s_btn, record_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(s_btn); lv_label_set_text_static(s_btn_label, "REC"); lv_obj_set_style_text_color(s_btn_label, COL_DARK, 0); lv_obj_set_style_text_font(s_btn_label, UI_FONT, 0); lv_obj_center(s_btn_label);
    s_bar = lv_bar_create(card); lv_obj_set_width(s_bar, BSP_LCD_H_RES - 130); lv_bar_set_range(s_bar, 0, 100); lv_bar_set_value(s_bar, 0, LV_ANIM_OFF); lv_obj_set_style_bg_color(s_bar, COL_BORDER, 0); lv_obj_set_style_bg_color(s_bar, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_t *hint = lv_label_create(screen); lv_label_set_text_static(hint, "STREAM unlimited  |  LOCAL max 5 min"); lv_obj_set_style_text_color(hint, COL_SUBTEXT, 0); lv_obj_set_style_text_font(hint, UI_FONT, 0);
    if (group) {
        lv_group_add_obj(group, s_btn);
    }
    if (!s_evt_q) {
        s_evt_q = xQueueCreate(8, sizeof(record_event_t));
    }
    if (!s_timer) {
        s_timer = lv_timer_create(drain, 100, NULL);
    }
    record_service_set_evt_cb(on_record);
}
void tab_record_destroy(void) { if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; } s_btn = s_btn_label = s_status = s_bar = NULL; }
void tab_record_on_play_done(void) {}
void tab_record_on_record_done(void) {}
