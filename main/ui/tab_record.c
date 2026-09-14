/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_record.c
 * @brief REC tab (local test record / play / stop). Buttons are file-static;
 *        audio completion re-enables them through tab_record_on_*().
 */

#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_record.h"
#include "ui_theme.h"
#include "middleware/audio_service.h"

static lv_obj_t *s_rec_btn;
static lv_obj_t *s_play_btn;
static lv_obj_t *s_stop_btn;

/* ---- button click handlers (formerly in ui_windows.c) ---- */

static void rec_play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
        audio_cmd_t cmd = { .id = AUDIO_CMD_PLAY_FILE };
        strncpy(cmd.path, lv_event_get_user_data(e), sizeof(cmd.path) - 1);
        audio_service_post_cmd(&cmd);
    }
}

static void rec_stop_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        audio_cmd_t cmd = { .id = AUDIO_CMD_STOP };
        audio_service_post_cmd(&cmd);
    }
}

static void rec_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
        if (s_stop_btn && s_play_btn) {
            lv_obj_add_state(s_play_btn, LV_STATE_DISABLED);
            lv_obj_add_state(s_stop_btn, LV_STATE_DISABLED);
        }
        audio_cmd_t cmd = { .id = AUDIO_CMD_START_RECORD };
        strncpy(cmd.path, lv_event_get_user_data(e), sizeof(cmd.path) - 1);
        audio_service_post_cmd(&cmd);
    }
}

void tab_record_build(lv_obj_t *screen, lv_group_t *group)
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
    lv_obj_set_style_text_font(title, UI_FONT, 0);

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
    s_rec_btn = lv_btn_create(card);
    lv_obj_set_size(s_rec_btn, 100, 100);
    lv_obj_set_style_radius(s_rec_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rec_btn, COL_RED, 0);
    lv_obj_set_style_shadow_color(s_rec_btn, COL_RED, 0);
    lv_obj_set_style_shadow_width(s_rec_btn, 14, 0);
    lv_obj_t *rl = lv_label_create(s_rec_btn);
    lv_label_set_text_static(rl, "REC");
    lv_obj_set_style_text_color(rl, COL_DARK, 0);
    lv_obj_set_style_text_font(rl, UI_FONT, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(s_rec_btn, rec_event_cb, LV_EVENT_CLICKED, (char *)REC_FILENAME);

    /* Play button (cyan) */
    s_play_btn = lv_btn_create(card);
    lv_obj_set_size(s_play_btn, 100, 100);
    lv_obj_set_style_radius(s_play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_play_btn, COL_CYAN, 0);
    lv_obj_set_style_shadow_color(s_play_btn, COL_CYAN, 0);
    lv_obj_set_style_shadow_width(s_play_btn, 14, 0);
    lv_obj_t *pl = lv_label_create(s_play_btn);
    lv_label_set_text_static(pl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(pl, COL_DARK, 0);
    lv_obj_set_style_text_font(pl, UI_FONT, 0);
    lv_obj_center(pl);
    lv_obj_add_event_cb(s_play_btn, rec_play_event_cb, LV_EVENT_CLICKED, (char *)REC_FILENAME);

    /* Stop button (gray) */
    s_stop_btn = lv_btn_create(card);
    lv_obj_set_size(s_stop_btn, 100, 100);
    lv_obj_set_style_radius(s_stop_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_stop_btn, COL_SUBTEXT, 0);
    lv_obj_t *sl = lv_label_create(s_stop_btn);
    lv_label_set_text_static(sl, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(sl, COL_DARK, 0);
    lv_obj_set_style_text_font(sl, UI_FONT, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_stop_btn, rec_stop_event_cb, LV_EVENT_CLICKED, NULL);

    /* Footer hint */
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text_static(hint, "5s local test  |  /spiffs/recording.wav");
    lv_obj_set_style_text_color(hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(hint, UI_FONT, 0);

    if (group) {
        lv_group_add_obj(group, s_rec_btn);
        lv_group_add_obj(group, s_play_btn);
        lv_group_add_obj(group, s_stop_btn);
    }
}

void tab_record_on_play_done(void)
{
    if (s_play_btn) {
        lv_obj_clear_state(s_play_btn, LV_STATE_DISABLED);
    }
}

void tab_record_on_record_done(void)
{
    if (s_rec_btn) {
        lv_obj_clear_state(s_rec_btn, LV_STATE_DISABLED);
    }
    if (s_play_btn) {
        lv_obj_clear_state(s_play_btn, LV_STATE_DISABLED);
    }
    if (s_stop_btn) {
        lv_obj_clear_state(s_stop_btn, LV_STATE_DISABLED);
    }
}
