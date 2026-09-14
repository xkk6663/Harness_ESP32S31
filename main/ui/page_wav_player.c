/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_wav_player.c
 * @brief WAV player page, refactored from the former ui_windows.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "page_wav_player.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "tab_record.h"
#include "middleware/audio_service.h"

static const char *TAG = "WAV_PAGE";

static char      s_file_path[256];
static lv_obj_t *s_play_btn;

/* ---- audio completion event ---- */

static void on_audio_event(audio_evt_id_t evt)
{
    switch (evt) {
    case AUDIO_EVT_PLAY_DONE:
        bsp_display_lock(0);
        if (s_play_btn) {
            lv_obj_clear_state(s_play_btn, LV_STATE_DISABLED);
        }
        tab_record_on_play_done();
        bsp_display_unlock();
        break;
    case AUDIO_EVT_RECORD_DONE:
#if BSP_CAPS_AUDIO_MIC
        bsp_display_lock(0);
        tab_record_on_record_done();
        bsp_display_unlock();
#endif
        break;
    default:
        break;
    }
}

void page_wav_player_register_audio_events(void)
{
    audio_service_set_evt_cb(on_audio_event);
}

/* ---- control callbacks ---- */

static void play_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
        audio_cmd_t cmd = { .id = AUDIO_CMD_PLAY_FILE };
        snprintf(cmd.path, sizeof(cmd.path), "%s", s_file_path);
        audio_service_post_cmd(&cmd);
    }
}

static void stop_event_cb(lv_event_t *e)
{
    (void)e;
    audio_cmd_t cmd = { .id = AUDIO_CMD_STOP };
    audio_service_post_cmd(&cmd);
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

static lv_obj_t *wav_player_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 16, 0);
    lv_obj_set_style_pad_row(page, 14, 0);

    ui_common_title_bar(page, s_file_path);

    audio_cmd_t cmd_reset = { .id = AUDIO_CMD_SET_REPEAT, .value = 0 };
    audio_service_post_cmd(&cmd_reset);

    /* button row */
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, 90);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);

    s_play_btn = lv_btn_create(row);
    lv_obj_set_size(s_play_btn, 70, 70);
    lv_obj_set_style_radius(s_play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_play_btn, COL_CYAN, 0);
    lv_obj_t *label = lv_label_create(s_play_btn);
    lv_label_set_text_static(label, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(label, COL_DARK, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(s_play_btn, play_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_btn = lv_btn_create(row);
    lv_obj_set_size(stop_btn, 70, 70);
    lv_obj_set_style_radius(stop_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stop_btn, COL_SUBTEXT, 0);
    label = lv_label_create(stop_btn);
    lv_label_set_text_static(label, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(label, COL_DARK, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(stop_btn, stop_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *repeat_btn = lv_btn_create(row);
    lv_obj_set_size(repeat_btn, 70, 70);
    lv_obj_set_style_radius(repeat_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(repeat_btn, COL_CARD, 0);
    lv_obj_set_style_border_color(repeat_btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(repeat_btn, 1, 0);
    lv_obj_add_flag(repeat_btn, LV_OBJ_FLAG_CHECKABLE);
    label = lv_label_create(repeat_btn);
    lv_label_set_text_static(label, LV_SYMBOL_LOOP);
    lv_obj_center(label);
    lv_obj_add_event_cb(repeat_btn, repeat_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* volume row */
    lv_obj_t *row2 = lv_obj_create(page);
    lv_obj_set_size(row2, BSP_LCD_H_RES - 40, 70);
    lv_obj_set_style_bg_color(row2, COL_CARD, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_style_radius(row2, 16, 0);
    lv_obj_set_style_pad_left(row2, 16, 0);
    lv_obj_set_style_pad_right(row2, 16, 0);
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

    /* encoder group */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        lv_group_t *g = lv_group_create();
        lv_group_add_obj(g, s_play_btn);
        lv_group_add_obj(g, stop_btn);
        lv_group_add_obj(g, repeat_btn);
        lv_group_add_obj(g, slider);
        lv_indev_set_group(indev, g);
    }

    ESP_LOGI(TAG, "player opened: %s", s_file_path);
    return page;
}

static void wav_player_destroy(lv_obj_t *page)
{
    audio_cmd_t cmd = { .id = AUDIO_CMD_STOP };
    audio_service_post_cmd(&cmd);

    lv_obj_del(page);
    s_play_btn = NULL;
    s_file_path[0] = '\0';
}

void Page_WavPlayer_Open(const char *path)
{
    if (!path) {
        return;
    }
    strncpy(s_file_path, path, sizeof(s_file_path) - 1);
    s_file_path[sizeof(s_file_path) - 1] = '\0';
    PageManager_Load(&Page_WavPlayer);
}

const Page_t Page_WavPlayer = {
    .create = wav_player_create,
    .destroy = wav_player_destroy,
    .name = "WavPlayer",
};
