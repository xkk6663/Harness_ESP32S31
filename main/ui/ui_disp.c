/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "ui_disp.h"
#include "ui_state.h"
#include "ui_windows.h"
#include "ui_call.h"
#include "ui_splash.h"
#include "middleware/fs_service.h"

static const char *TAG = "UI";

/* ---- UI layer global object definitions ---- */
lv_obj_t *tabview = NULL;
lv_obj_t *tab_btns = NULL;
lv_group_t *filesystem_group = NULL;
lv_group_t *recording_group = NULL;
lv_group_t *settings_group = NULL;

lv_obj_t *play_btn = NULL;
lv_obj_t *play1_btn = NULL;
lv_obj_t *rec_btn = NULL;
lv_obj_t *rec_stop_btn = NULL;

/* Re-declare record/settings builders (defined in their own TUs) */
extern void app_disp_lvgl_show_record(lv_obj_t *screen, lv_group_t *group);
extern void app_disp_lvgl_show_settings(lv_obj_t *screen, lv_group_t *group);

/* AI dark-blue theme */
#define COL_BG        lv_color_hex(0x0F2547)
#define COL_TABBAR    lv_color_hex(0x102A4C)
#define COL_TAB_TXT   lv_color_hex(0x94A3B8)
#define COL_TAB_ACT   lv_color_hex(0x22D3EE)

/* Pull-down wallpaper thresholds */
#define PULL_TOP_ZONE     60     /* px from top to trigger pull-down */
#define PULL_DOWN_OPEN   200    /* px travel to fully open */
#define PULL_ANIM_MS      220

static lv_obj_t *s_wallpaper;
static int32_t  s_pull_start_y;
static bool     s_pull_armed;

/* ---- anim helper: move object y to target ---- */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void wallpaper_animate_to(lv_obj_t *wp, int32_t target_y)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wp);
    lv_anim_set_values(&a, lv_obj_get_y(wp), target_y);
    lv_anim_set_time(&a, PULL_ANIM_MS);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ---- pull-down on main UI: drag wallpaper from top ---- */
static void main_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    /* Only arm if press starts near the top tab-bar zone. */
    s_pull_armed = (p.y < PULL_TOP_ZONE);
}

static void main_pressing_cb(lv_event_t *e)
{
    if (!s_pull_armed || !s_wallpaper) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy > 0) {
        /* wallpaper starts at y = -BSP_LCD_V_RES, follows finger */
        int32_t y = -BSP_LCD_V_RES + dy;
        if (y > 0) y = 0;
        lv_obj_set_y(s_wallpaper, y);
    }
}

static void main_released_cb(lv_event_t *e)
{
    if (!s_pull_armed || !s_wallpaper) {
        s_pull_armed = false;
        return;
    }
    int32_t y = lv_obj_get_y(s_wallpaper);
    /* More than PULL_DOWN_DOWN open threshold -> snap open; else snap shut. */
    if (y > -BSP_LCD_V_RES + PULL_DOWN_OPEN) {
        wallpaper_animate_to(s_wallpaper, 0);
        ESP_LOGI(TAG, "wallpaper pulled down");
    } else {
        wallpaper_animate_to(s_wallpaper, -BSP_LCD_V_RES);
    }
    s_pull_armed = false;
}

/* Swipe-up on the open wallpaper to dismiss it. */
static void wp_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = true;
}

static void wp_pressing_cb(lv_event_t *e)
{
    if (!s_pull_armed || !s_wallpaper) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy < 0) {
        /* dragging up: move wallpaper up toward off-screen */
        int32_t y = dy;
        if (y < -BSP_LCD_V_RES) y = -BSP_LCD_V_RES;
        lv_obj_set_y(s_wallpaper, y);
    }
}

static void wp_released_cb(lv_event_t *e)
{
    if (!s_pull_armed || !s_wallpaper) { s_pull_armed = false; return; }
    int32_t y = lv_obj_get_y(s_wallpaper);
    if (y < -120) {
        wallpaper_animate_to(s_wallpaper, -BSP_LCD_V_RES);
        ESP_LOGI(TAG, "wallpaper swiped up, dismissed");
    } else {
        wallpaper_animate_to(s_wallpaper, 0);
    }
    s_pull_armed = false;
}

static void tab_changed_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        set_tab_group();
    }
}

/* ---- main UI (called by splash enter callback) ---- */

void app_disp_lvgl_show_main(void)
{
    bsp_display_lock(0);

    lv_obj_clean(lv_scr_act());

    tabview = lv_tabview_create(lv_scr_act());
    lv_tabview_set_tab_bar_size(tabview, 48);
    lv_obj_set_size(tabview, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(tabview, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(tabview, COL_BG, 0);
    lv_obj_add_event_cb(tabview, tab_changed_event, LV_EVENT_VALUE_CHANGED, NULL);

    /* Three tabs */
    lv_obj_t *tab_call = lv_tabview_add_tab(tabview, LV_SYMBOL_AUDIO"  CALL");
    lv_obj_t *tab_rec  = lv_tabview_add_tab(tabview, LV_SYMBOL_SAVE"  REC");
    lv_obj_t *tab_sys  = lv_tabview_add_tab(tabview, LV_SYMBOL_SETTINGS"  SYS");

    /* Tab bar dark */
    tab_btns = lv_tabview_get_tab_btns(tabview);
    lv_obj_set_style_bg_color(tab_btns, COL_TABBAR, 0);
    lv_obj_set_style_text_color(tab_btns, COL_TAB_TXT, 0);
    lv_obj_set_style_text_color(tab_btns, COL_TAB_ACT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_btns, COL_TAB_ACT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tab_btns, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(tab_btns, COL_TABBAR, LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Let tab-bar buttons bubble touch events up to tab_btns so pull-down works. */
    uint32_t n = lv_obj_get_child_cnt(tab_btns);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(tab_btns, i);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* Pull-down gesture on the TOP TAB BAR: drag wallpaper from top. */
    lv_obj_add_event_cb(tab_btns, main_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(tab_btns, main_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(tab_btns, main_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_flag(tab_btns, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Input device group */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        filesystem_group = lv_group_create();
        recording_group = lv_group_create();
        settings_group = lv_group_create();
        lv_group_add_obj(filesystem_group, tab_btns);
        lv_indev_set_group(indev, filesystem_group);
    }

    app_disp_lvgl_show_call(tab_call, filesystem_group);
    app_disp_lvgl_show_record(tab_rec, recording_group);
    app_disp_lvgl_show_settings(tab_sys, settings_group);

    /* Wallpaper layer on top of tabview, initially off-screen above. */
    s_wallpaper = ui_wallpaper_attach(lv_layer_top());
    lv_obj_set_y(s_wallpaper, -BSP_LCD_V_RES);
    lv_obj_add_flag(s_wallpaper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wallpaper, wp_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_released_cb, LV_EVENT_RELEASED, NULL);

    /* Register audio event callbacks */
    ui_windows_register_audio_events();

    bsp_display_unlock();
}

void set_tab_group(void)
{
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && filesystem_group && recording_group && settings_group) {
        uint16_t tab = lv_tabview_get_tab_act(tabview);
        lv_group_set_editing(filesystem_group, false);
        lv_group_set_editing(recording_group, false);
        lv_group_set_editing(settings_group, false);
        switch (tab) {
        case 0:
            lv_group_add_obj(filesystem_group, tab_btns);
            lv_indev_set_group(indev, filesystem_group);
            break;
        case 1:
            lv_group_add_obj(recording_group, tab_btns);
            lv_indev_set_group(indev, recording_group);
            break;
        case 2:
            lv_group_add_obj(settings_group, tab_btns);
            lv_indev_set_group(indev, settings_group);
            break;
        }
        lv_tabview_set_act(tabview, tab, false);
    }
}
