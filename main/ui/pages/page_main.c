/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_main.c
 * @brief Main tab-view page, refactored from the former ui_disp.c.
 *        Wall-paper gesture is delegated to overlay_wallpaper.
 */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "page_main.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "ui/tabs/tab_call.h"
#include "ui/tabs/tab_record.h"
#include "ui/tabs/tab_sys_entry.h"
#include "ui/tabs/tab_audio.h"
#include "ui/tabs/tab_video.h"
#include "ui/overlays/overlay_wallpaper.h"
#include "page_wav_player.h"

static const char *TAG = "PAGE_MAIN";

static lv_obj_t  *s_tabview;
static lv_obj_t  *s_tab_btns;
static lv_group_t *s_group_call;
static lv_group_t *s_group_rec;
static lv_group_t *s_group_sys;

static void tab_changed_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        page_main_refresh_group();
    }
}

/* ★ F35 一次性体检：主页面（含 6 个 tab）创建前后各打一行内部 RAM。
   现象是"切到 SYS tab 开蓝牙时 internal free 只剩 7.7KB"，而这 7.7KB
   与开机时的 193KB 差了 185KB。光看总量分不出是谁吃的，
   所以在"整页 UI 建完"这个天然分界点上量一次：
     - 若此处已掉到很低 → 凶手在**页面/选项卡创建**（LVGL 对象、字体、图片资源）；
     - 若此处还很宽裕、只是开蓝牙时才低 → 凶手在**运行期常驻消费者**
       （WiFi/LWIP 缓冲、TLS、后台任务栈）。
   只做诊断用，定位完可删。 */
static void diag_dump_internal(const char *tag)
{
    ESP_LOGI(TAG, "[diag] %s: internal free=%u largest=%u | PSRAM free=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static lv_obj_t *main_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);

    diag_dump_internal("before page build");

    s_tabview = lv_tabview_create(page);
    lv_tabview_set_tab_bar_size(s_tabview, 48);
    lv_obj_set_size(s_tabview, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(s_tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_tabview, UI_FONT, 0);
    lv_obj_set_style_bg_color(s_tabview, COL_BG, 0);
    lv_obj_add_event_cb(s_tabview, tab_changed_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tab_call = lv_tabview_add_tab(s_tabview, LV_SYMBOL_AUDIO"  CALL");
    lv_obj_t *tab_rec  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SAVE"  REC");
    /* ★ 2026-09-22：原 MUSIC 与 BTPUSH 两页合并为 AUDIO，
       播放出口由页内模式按钮切换（本地扬声器 / 蓝牙推流）。 */
    lv_obj_t *tab_aud  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_LIST"  AUDIO");
    lv_obj_t *tab_vid  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_VIDEO"  VIDEO");
    lv_obj_t *tab_sys  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SETTINGS"  SYS");

    /* Tab bar styling */
    s_tab_btns = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_bg_color(s_tab_btns, COL_TABBAR, 0);
    lv_obj_set_style_text_color(s_tab_btns, COL_TAB_TXT, 0);
    lv_obj_set_style_text_color(s_tab_btns, COL_TAB_ACT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_tab_btns, COL_TAB_ACT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(s_tab_btns, 2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(s_tab_btns, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_tab_btns, COL_TABBAR, LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Bubble touch from tab-bar buttons so the pull-down gesture works. */
    uint32_t n = lv_obj_get_child_cnt(s_tab_btns);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(s_tab_btns, i);
        lv_obj_set_event_bubble(child, true);
    }

    /* Encoder groups (only created when an encoder indev exists) */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        s_group_call = lv_group_create();
        s_group_rec  = lv_group_create();
        s_group_sys  = lv_group_create();
        lv_group_add_obj(s_group_call, s_tab_btns);
        lv_indev_set_group(indev, s_group_call);
    }

    tab_call_build(tab_call, s_group_call);
    tab_record_build(tab_rec, s_group_rec);
    tab_audio_build(tab_aud, NULL);
    tab_video_build(tab_vid, NULL);
    tab_sys_entry_build(tab_sys, s_group_sys);

    /* Pull-down wallpaper overlay on lv_layer_top. */
    overlay_wallpaper_init(s_tab_btns);

    /* Audio completion callbacks (re-enable buttons). */
    page_wav_player_register_audio_events();

    diag_dump_internal("after page build");

    ESP_LOGI(TAG, "main page created");
    return page;
}

static void main_destroy(lv_obj_t *page)
{
    tab_call_destroy();
    tab_record_destroy();
    tab_audio_destroy();
    tab_video_destroy();
    tab_sys_entry_destroy();
    overlay_wallpaper_deinit();

    if (s_group_call) {
        lv_group_del(s_group_call);
        s_group_call = NULL;
    }
    if (s_group_rec) {
        lv_group_del(s_group_rec);
        s_group_rec = NULL;
    }
    if (s_group_sys) {
        lv_group_del(s_group_sys);
        s_group_sys = NULL;
    }

    lv_obj_del(page);
    s_tabview = NULL;
    s_tab_btns = NULL;
}

void page_main_refresh_group(void)
{
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && s_group_call && s_group_rec && s_group_sys && s_tabview) {
        uint16_t tab = lv_tabview_get_tab_act(s_tabview);
        lv_group_set_editing(s_group_call, false);
        lv_group_set_editing(s_group_rec, false);
        lv_group_set_editing(s_group_sys, false);
        switch (tab) {
        case 0:
            lv_group_add_obj(s_group_call, s_tab_btns);
            lv_indev_set_group(indev, s_group_call);
            break;
        case 1:
            lv_group_add_obj(s_group_rec, s_tab_btns);
            lv_indev_set_group(indev, s_group_rec);
            break;
        case 2:
            lv_group_add_obj(s_group_sys, s_tab_btns);
            lv_indev_set_group(indev, s_group_sys);
            break;
        }
        lv_tabview_set_act(s_tabview, tab, false);
    }
}

const Page_t Page_Main = {
    .create = main_create,
    .destroy = main_destroy,
    .name = "Main",
};
