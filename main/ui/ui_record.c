/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "ui_state.h"
#include "ui_record.h"
#include "ui_windows.h"
#include "middleware/audio_service.h"

#define COL_BG        lv_color_hex(0x0F2547)
#define COL_CARD      lv_color_hex(0x1E3A5F)
#define COL_BORDER    lv_color_hex(0x3B5A82)
#define COL_CYAN      lv_color_hex(0x22D3EE)
#define COL_RED       lv_color_hex(0xF87171)
#define COL_TEXT      lv_color_hex(0xE2E8F0)
#define COL_SUBTEXT   lv_color_hex(0x94A3B8)

void app_disp_lvgl_show_record(lv_obj_t *screen, lv_group_t *group)
{
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_top(screen, 24, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_SAVE"  LOCAL RECORD");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    /* Card */
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, 280);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 24, 0);

    /* REC button (red) */
    rec_btn = lv_btn_create(card);
    lv_obj_set_size(rec_btn, 100, 100);
    lv_obj_set_style_radius(rec_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_btn, COL_RED, 0);
    lv_obj_set_style_shadow_color(rec_btn, COL_RED, 0);
    lv_obj_set_style_shadow_width(rec_btn, 14, 0);
    lv_obj_t *rl = lv_label_create(rec_btn);
    lv_label_set_text_static(rl, "REC");
    lv_obj_set_style_text_color(rl, lv_color_hex(0x0B1120), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rec_btn, rec_event_cb, LV_EVENT_CLICKED, (char *)REC_FILENAME);

    /* Play button (cyan) */
    play1_btn = lv_btn_create(card);
    lv_obj_set_size(play1_btn, 100, 100);
    lv_obj_set_style_radius(play1_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play1_btn, COL_CYAN, 0);
    lv_obj_set_style_shadow_color(play1_btn, COL_CYAN, 0);
    lv_obj_set_style_shadow_width(play1_btn, 14, 0);
    lv_obj_t *pl = lv_label_create(play1_btn);
    lv_label_set_text_static(pl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(pl, lv_color_hex(0x0B1120), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);
    lv_obj_center(pl);
    lv_obj_add_event_cb(play1_btn, rec_play_event_cb, LV_EVENT_CLICKED, (char *)REC_FILENAME);

    /* Stop button (gray) */
    rec_stop_btn = lv_btn_create(card);
    lv_obj_set_size(rec_stop_btn, 100, 100);
    lv_obj_set_style_radius(rec_stop_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_stop_btn, COL_SUBTEXT, 0);
    lv_obj_t *sl = lv_label_create(rec_stop_btn);
    lv_label_set_text_static(sl, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(sl, lv_color_hex(0x0B1120), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(rec_stop_btn, rec_stop_event_cb, LV_EVENT_CLICKED, NULL);

    /* Footer hint */
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text_static(hint, "5s local test  |  /spiffs/recording.wav");
    lv_obj_set_style_text_color(hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

    if (group) {
        lv_group_add_obj(group, rec_btn);
        lv_group_add_obj(group, play1_btn);
        lv_group_add_obj(group, rec_stop_btn);
    }
}
