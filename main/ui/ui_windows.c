/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "ui_state.h"
#include "ui_disp.h"
#include "ui_windows.h"
#include "middleware/audio_service.h"

static const char *TAG = "UI_WIN";

/* 播放路径暂存 */
static char usb_drive_play_file[250];

#define COL_CARD      lv_color_hex(0x1E293B)
#define COL_BORDER    lv_color_hex(0x334155)
#define COL_CYAN      lv_color_hex(0x22D3EE)
#define COL_TEXT      lv_color_hex(0xE2E8F0)
#define COL_SUBTEXT   lv_color_hex(0x64748B)

/* ---- 音频事件回调 ---- */

static void on_audio_event(audio_evt_id_t evt)
{
    switch (evt) {
    case AUDIO_EVT_PLAY_DONE:
        bsp_display_lock(0);
        if (play_btn)    lv_obj_clear_state(play_btn, LV_STATE_DISABLED);
        if (play1_btn)   lv_obj_clear_state(play1_btn, LV_STATE_DISABLED);
        bsp_display_unlock();
        break;
    case AUDIO_EVT_RECORD_DONE:
#if BSP_CAPS_AUDIO_MIC
        bsp_display_lock(0);
        if (rec_btn)     lv_obj_clear_state(rec_btn, LV_STATE_DISABLED);
        if (play1_btn)   lv_obj_clear_state(play1_btn, LV_STATE_DISABLED);
        if (rec_stop_btn) lv_obj_clear_state(rec_stop_btn, LV_STATE_DISABLED);
        bsp_display_unlock();
#endif
        break;
    }
}

void ui_windows_register_audio_events(void)
{
    audio_service_set_evt_cb(on_audio_event);
}

/* ---- WAV 播放事件回调 ---- */

static void play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
        audio_cmd_t cmd = { .id = AUDIO_CMD_PLAY_FILE };
        strncpy(cmd.path, lv_event_get_user_data(e), sizeof(cmd.path) - 1);
        audio_service_post_cmd(&cmd);
    }
}

static void stop_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        audio_cmd_t cmd = { .id = AUDIO_CMD_STOP };
        audio_service_post_cmd(&cmd);
    }
}

static void repeat_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *obj = lv_event_get_target(e);
        audio_cmd_t cmd = {
            .id = AUDIO_CMD_SET_REPEAT,
            .value = (lv_obj_get_state(obj) & LV_STATE_CHECKED) ? 1 : 0,
        };
        audio_service_post_cmd(&cmd);
    }
}

static void volume_event_cb(lv_event_t *e)
{
    audio_cmd_t cmd = {
        .id = AUDIO_CMD_SET_VOLUME,
        .value = lv_slider_get_value(lv_event_get_target(e)),
    };
    audio_service_post_cmd(&cmd);
}

static void close_window_wav_handler(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_del(lv_event_get_user_data(e));
        audio_cmd_t cmd = { .id = AUDIO_CMD_STOP };
        audio_service_post_cmd(&cmd);
        set_tab_group();
    }
}

void show_window_wav(const char *path)
{
    lv_obj_t *btn, *stop_btn, *repeat_btn, *label;

    lv_obj_t *win = lv_win_create(lv_scr_act());
    lv_win_add_title(win, path);
    lv_obj_set_style_bg_color(win, COL_CARD, 0);

    strcpy(usb_drive_play_file, path);

    audio_cmd_t cmd_reset = { .id = AUDIO_CMD_SET_REPEAT, .value = 0 };
    audio_service_post_cmd(&cmd_reset);

    btn = lv_win_add_button(win, LV_SYMBOL_CLOSE, 60);
    lv_obj_add_event_cb(btn, close_window_wav_handler, LV_EVENT_CLICKED, win);

    lv_obj_t *cont = lv_win_get_content(win);
    lv_obj_set_style_bg_color(cont, COL_CARD, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 按钮行 */
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, 80);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);

    play_btn = lv_btn_create(row);
    lv_obj_set_size(play_btn, 70, 70);
    lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play_btn, COL_CYAN, 0);
    label = lv_label_create(play_btn);
    lv_label_set_text_static(label, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0B1120), 0);
    lv_obj_add_event_cb(play_btn, play_event_cb, LV_EVENT_CLICKED, (char *)usb_drive_play_file);

    stop_btn = lv_btn_create(row);
    lv_obj_set_size(stop_btn, 70, 70);
    lv_obj_set_style_radius(stop_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stop_btn, COL_SUBTEXT, 0);
    label = lv_label_create(stop_btn);
    lv_label_set_text_static(label, LV_SYMBOL_STOP);
    lv_obj_add_event_cb(stop_btn, stop_event_cb, LV_EVENT_CLICKED, NULL);

    repeat_btn = lv_btn_create(row);
    lv_obj_set_size(repeat_btn, 70, 70);
    lv_obj_set_style_radius(repeat_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(repeat_btn, COL_CARD, 0);
    lv_obj_set_style_border_color(repeat_btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(repeat_btn, 1, 0);
    lv_obj_add_flag(repeat_btn, LV_OBJ_FLAG_CHECKABLE);
    label = lv_label_create(repeat_btn);
    lv_label_set_text_static(label, LV_SYMBOL_LOOP);
    lv_obj_add_event_cb(repeat_btn, repeat_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 音量行 */
    lv_obj_t *row2 = lv_obj_create(cont);
    lv_obj_set_size(row2, BSP_LCD_H_RES - 40, 70);
    lv_obj_set_style_bg_color(row2, COL_CARD, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    label = lv_label_create(row2);
    lv_label_set_text_static(label, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(label, COL_TEXT, 0);

    lv_obj_t *slider = lv_slider_create(row2);
    lv_obj_set_width(slider, BSP_LCD_H_RES - 220);
    lv_slider_set_range(slider, 0, 90);
    lv_slider_set_value(slider, DEFAULT_VOLUME, false);
    lv_obj_set_style_bg_color(slider, COL_BORDER, 0);
    lv_obj_set_style_bg_color(slider, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Encoder group */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        lv_group_t *g = lv_group_create();
        lv_group_add_obj(g, btn);
        lv_group_add_obj(g, play_btn);
        lv_group_add_obj(g, stop_btn);
        lv_group_add_obj(g, repeat_btn);
        lv_group_add_obj(g, slider);
        lv_indev_set_group(indev, g);
    }
}

/* ---- 录音 tab 按钮事件 ---- */

void rec_play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
        audio_cmd_t cmd = { .id = AUDIO_CMD_PLAY_FILE };
        strncpy(cmd.path, lv_event_get_user_data(e), sizeof(cmd.path) - 1);
        audio_service_post_cmd(&cmd);
    }
}

void rec_stop_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        audio_cmd_t cmd = { .id = AUDIO_CMD_STOP };
        audio_service_post_cmd(&cmd);
    }
}

void rec_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
        if (rec_stop_btn && play1_btn) {
            lv_obj_add_state(play1_btn, LV_STATE_DISABLED);
            lv_obj_add_state(rec_stop_btn, LV_STATE_DISABLED);
        }
        audio_cmd_t cmd = { .id = AUDIO_CMD_START_RECORD };
        strncpy(cmd.path, lv_event_get_user_data(e), sizeof(cmd.path) - 1);
        audio_service_post_cmd(&cmd);
    }
}
