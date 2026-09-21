/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_btpush.c
 * @brief BTPUSH：A2DP Source 把 SD 卡 MP3 推到蓝牙耳机。
 *
 * 前置：先在 SYS > Bluetooth 打开蓝牙、连上耳机，再回此 tab 点歌。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_btpush.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "middleware/sdcard_service.h"
#include "middleware/bt_audio_service.h"

static const char *TAG = "TAB_BTPUSH";

#define MAX_TRACKS 16
#define NAME_LEN   64
#define PATH_LEN   300

static char s_tracks[MAX_TRACKS][PATH_LEN];
static int  s_track_count = 0;
static int  s_current = -1;

static lv_obj_t *s_list;
static lv_obj_t *s_title;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_stop_btn;
static bool s_paused = false;

static void scan_tracks(void)
{
    s_track_count = 0;
    s_current = -1;
    if (!sdcard_service_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }
    DIR *d = opendir(SDCARD_MNT_PATH "/music");
    if (!d) {
        ESP_LOGW(TAG, "no /sdcard/music dir");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) && s_track_count < MAX_TRACKS) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcasecmp(dot, ".mp3") != 0) continue;
        snprintf(s_tracks[s_track_count], PATH_LEN,
                 SDCARD_MNT_PATH "/music/%s", ent->d_name);
        s_track_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "found %d mp3 files", s_track_count);
}

static void play_track(int idx)
{
    if (idx < 0 || idx >= s_track_count) return;
    s_current = idx;

    const char *name = strrchr(s_tracks[idx], '/');
    name = name ? name + 1 : s_tracks[idx];
    ESP_LOGI(TAG, "push: %s", s_tracks[idx]);
    if (s_title) lv_label_set_text_fmt(s_title, "%s", name);

    if (!bt_audio_service_is_on()) {
        if (s_title) lv_label_set_text(s_title, "BT off - open it in SYS");
        return;
    }
    esp_err_t ret = bt_audio_service_play_source(s_tracks[idx]);
    if (ret != ESP_OK && s_title) {
        lv_label_set_text(s_title, "Push failed (pair a headset?)");
    }
    s_paused = false;
    if (s_pause_btn) {
        lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PAUSE" Pause");
    }
}

static void on_item_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    play_track(idx);
}

static void on_pause_click(lv_event_t *e)
{
    (void)e;
    if (s_paused) {
        bt_audio_service_play_source(s_current >= 0 ? s_tracks[s_current] : NULL);
        s_paused = false;
        lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PAUSE" Pause");
    } else {
        bt_audio_service_pause_source();
        s_paused = true;
        lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PLAY" Resume");
    }
}

static void on_stop_click(lv_event_t *e)
{
    (void)e;
    bt_audio_service_stop_source();
    s_paused = false;
    lv_label_set_text(lv_obj_get_child(s_pause_btn, 0), LV_SYMBOL_PAUSE" Pause");
    if (s_title) lv_label_set_text(s_title, "Stopped");
}

void tab_btpush_build(lv_obj_t *parent, lv_group_t *g)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_title = lv_label_create(parent);
    lv_label_set_text(s_title, "BT Push to headset");
    lv_obj_set_style_text_font(s_title, UI_FONT, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 10);

    s_list = lv_list_create(parent);
    lv_obj_set_size(s_list, 700, 300);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 50);

    scan_tracks();
    for (int i = 0; i < s_track_count; i++) {
        const char *name = strrchr(s_tracks[i], '/');
        name = name ? name + 1 : s_tracks[i];
        lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_AUDIO, name);
        lv_obj_add_event_cb(btn, on_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_t *ctrl_row = lv_obj_create(parent);
    lv_obj_set_size(ctrl_row, 680, 70);
    lv_obj_align(ctrl_row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);

    s_pause_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(s_pause_btn, 200, 60);
    lv_obj_t *lbl = lv_label_create(s_pause_btn);
    lv_label_set_text(lbl, LV_SYMBOL_PAUSE" Pause");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(s_pause_btn, on_pause_click, LV_EVENT_CLICKED, NULL);

    s_stop_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(s_stop_btn, 200, 60);
    lbl = lv_label_create(s_stop_btn);
    lv_label_set_text(lbl, LV_SYMBOL_STOP" Stop");
    lv_obj_center(lbl);
    lv_obj_add_event_cb(s_stop_btn, on_stop_click, LV_EVENT_CLICKED, NULL);

    if (g) {
        lv_group_add_obj(g, s_pause_btn);
        lv_group_add_obj(g, s_stop_btn);
    }
}

void tab_btpush_destroy(void)
{
    s_list = NULL;
    s_title = NULL;
    s_pause_btn = NULL;
    s_stop_btn = NULL;
    s_track_count = 0;
    s_current = -1;
}
