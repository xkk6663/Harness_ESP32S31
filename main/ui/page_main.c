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
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "page_main.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "tab_call.h"
#include "tab_record.h"
#include "tab_sys_entry.h"
#include "tab_camera.h"
#include "tab_video.h"
#include "overlay_wallpaper.h"
#include "page_wav_player.h"

static const char *TAG = "PAGE_MAIN";

static lv_obj_t  *s_tabview;
static lv_obj_t  *s_tab_btns;
static uint32_t   s_tab_vid_idx = 3;    /* VIDEO 页下标：建页时按实际顺序算出（见 main_create） */
static lv_group_t *s_group_call;
static lv_group_t *s_group_rec;
static lv_group_t *s_group_sys;

static void tab_changed_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        /* ★★ 取景流与 tab 绑定（2026-09-25）：只有 VIDEO 页活跃时它才占着相机。
         *   ★ 必须放在 refresh_group() **之前**：那个函数在没有 encoder 输入设备的
         *     触摸方案下整段 early-out（连 s_group_* 判空都不做直接 return），
         *     把通知写进去就永远收不到 —— 这正是本工程反复踩的"静默失效"形态。 */
        tab_video_set_active(lv_tabview_get_tab_active(s_tabview) == s_tab_vid_idx);
        page_main_refresh_group();
    }
}

static lv_obj_t *main_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);

    s_tabview = lv_tabview_create(page);
    lv_tabview_set_tab_bar_size(s_tabview, 48);
    lv_obj_set_size(s_tabview, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(s_tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_tabview, UI_FONT, 0);
    lv_obj_set_style_bg_color(s_tabview, COL_BG, 0);
    lv_obj_add_event_cb(s_tabview, tab_changed_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tab_call = lv_tabview_add_tab(s_tabview, LV_SYMBOL_AUDIO"  CALL");
    lv_obj_t *tab_rec  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SAVE"  REC");
    lv_obj_t *tab_cam  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_VIDEO"  CAM");
    /* ★ S2-0：第 5 个 tab。插在 CAM 与 SYS 之间 —— CAM 是"看/试"（RGB565 预览 + 模型），
     *   VIDEO 是"采"（JPEG 1280x720 拍照/录像），语义相邻；SYS 是设置，天然垫底。
     *   ★ 标签宽度增到 5 个：tab bar 是 F46 的敏感区（页对象自带 pad，子对象越界会被裁），
     *     上机需确认 5 个标签不被挤压/裁字 —— 已列入验收清单。 */
    lv_obj_t *tab_vid  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_IMAGE"  VIDEO");
    lv_obj_t *tab_sys  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SETTINGS"  SYS");

    /* ★ VIDEO 页下标按"第几个加的"反推，不写死 —— 以后再插 tab 也不会错。
     *   （tab_changed_event 用它判断"当前是不是 VIDEO 页"，见上。） */
    s_tab_vid_idx = lv_tabview_get_tab_count(s_tabview) - 1;

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
    tab_camera_build(tab_cam, NULL);
    tab_video_build(tab_vid, NULL);
    tab_sys_entry_build(tab_sys, s_group_sys);

    /* Pull-down wallpaper overlay on lv_layer_top. */
    overlay_wallpaper_init(s_tab_btns);

    /* Audio completion callbacks (re-enable buttons). */
    page_wav_player_register_audio_events();

    ESP_LOGI(TAG, "main page created");
    return page;
}

static void main_destroy(lv_obj_t *page)
{
    tab_call_destroy();
    tab_record_destroy();
    tab_camera_destroy();
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
        /* ★ S2-0：SYS 的下标从 2 挪到 **4** —— CAM 与 SYS 之间插入了 VIDEO tab。
         *   （顺带修掉原有的错位：老代码 case 2 指向的是 CAM，却绑 SYS 的组。）
         *   CAM(2) / VIDEO(3) 在 build 时传的就是 NULL，**没有** encoder 组，
         *   所以刻意不给它们 case，落到 switch 外面保持上一个组的绑定。
         *   ★ 本分支仅在存在 ENCODER 输入设备时才会执行（触摸方案下 s_group_* 全为 NULL，
         *     整个 if 直接跳过），因此这次改动对当前硬件无影响，只是别留错。 */
        case 4:
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
