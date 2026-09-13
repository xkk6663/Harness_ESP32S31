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
#include "middleware/fs_service.h"

static const char *TAG = "UI";

/* ---- UI 层全局对象定义 ---- */
lv_obj_t *tabview = NULL;
lv_obj_t *tab_btns = NULL;
lv_group_t *filesystem_group = NULL;
lv_group_t *recording_group = NULL;
lv_group_t *settings_group = NULL;

lv_obj_t *play_btn = NULL;
lv_obj_t *play1_btn = NULL;
lv_obj_t *rec_btn = NULL;
lv_obj_t *rec_stop_btn = NULL;

/* 跨文件函数声明 */
extern void app_disp_lvgl_show_record(lv_obj_t *screen, lv_group_t *group);
extern void app_disp_lvgl_show_settings(lv_obj_t *screen, lv_group_t *group);

/* AI dark-blue theme */
#define COL_BG        lv_color_hex(0x0F2547)
#define COL_TABBAR    lv_color_hex(0x102A4C)
#define COL_TAB_TXT   lv_color_hex(0x94A3B8)
#define COL_TAB_ACT   lv_color_hex(0x22D3EE)

static void tab_changed_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        set_tab_group();
    }
}

/* ---- 主界面（上拉后由 splash 回调调用） ---- */

void app_disp_lvgl_show_main(void)
{
    bsp_display_lock(0);

    /* 清掉 splash 残层 */
    lv_obj_clean(lv_scr_act());

    tabview = lv_tabview_create(lv_scr_act());
    lv_tabview_set_tab_bar_size(tabview, 48);
    lv_obj_set_size(tabview, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(tabview, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(tabview, COL_BG, 0);
    lv_obj_add_event_cb(tabview, tab_changed_event, LV_EVENT_VALUE_CHANGED, NULL);

    /* Tab bar 深色 */
    tab_btns = lv_tabview_get_tab_btns(tabview);
    lv_obj_set_style_bg_color(tab_btns, COL_TABBAR, 0);
    lv_obj_set_style_text_color(tab_btns, COL_TAB_TXT, 0);
    lv_obj_set_style_text_color(tab_btns, COL_TAB_ACT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tab_btns, COL_TAB_ACT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tab_btns, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(tab_btns, COL_TABBAR, LV_PART_ITEMS | LV_STATE_CHECKED);

    /* 三个 tab */
    lv_obj_t *tab_call = lv_tabview_add_tab(tabview, LV_SYMBOL_AUDIO"  CALL");
    lv_obj_t *tab_rec  = lv_tabview_add_tab(tabview, LV_SYMBOL_SAVE"  REC");
    lv_obj_t *tab_sys  = lv_tabview_add_tab(tabview, LV_SYMBOL_SETTINGS"  SYS");

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

    /* 注册音频事件回调 */
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
