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
#include "tab_sys_entry.h"
#include "tab_record.h"
#include "tab_camera.h"
#include "tab_video.h"
#include "tab_files.h"
#include "overlay_wallpaper.h"
#include "overlay_ai.h"

static const char *TAG = "PAGE_MAIN";

static lv_obj_t  *s_tabview;
static lv_obj_t  *s_tab_btns;
static uint32_t   s_tab_vid_idx = 1;    /* VIDEO 页下标：建页时按实际顺序算出（见 main_create） */
static uint32_t   s_tab_files_idx = 2;  /* FILES 页下标（S4 文件管理，同上） */
static lv_group_t *s_group_sys;

static void tab_changed_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        /* ★★ tab 绑定的后台动作（取景流 / 文件管理页）都写在这里。
         *   ★ 必须放在 refresh_group() **之前**：那个函数在没有 encoder 输入设备的
         *     触摸方案下整段 early-out（连 s_group_* 判空都不做直接 return），
         *     把通知写进去就永远收不到 —— 这正是本工程反复踩的"静默失效"形态。
         *   ★ 两个 set_active 天然互斥：同一时刻只有一个 tab 活跃。 */
        bool vid_act   = (lv_tabview_get_tab_active(s_tabview) == s_tab_vid_idx);
        bool files_act = (lv_tabview_get_tab_active(s_tabview) == s_tab_files_idx);
        tab_video_set_active(vid_act);
        tab_files_set_active(files_act);
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

    lv_obj_t *tab_cam  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_VIDEO"  CAM");
    /* ★ S2-0：VIDEO 插在 CAM 之后 —— CAM 是"看/试"（RGB565 预览 + 模型），
     *   VIDEO 是"采"（JPEG 1280x720 拍照/录像），语义相邻。
     * ★ S4：FILES（文件管理）插在 VIDEO 与 SYS 之间；SYS 是设置，天然垫底。
     * ★ 下标**紧跟 add 之后**取 count-1 —— 旧写法"加完全部再取 count-1"只因为
     *   VIDEO 恰好是最后一个才对，插进 FILES 后那样取会指到 SYS（静默错页）。
     * ★ 2026-09-27 call/rec 阉割：CALL/REC 两页删除（main 主线已开发），
     *   页序变为 CAM(0)/VIDEO(1)/FILES(2)/SYS(3)。
     * ★ §40 REC 回归（本地录音版）插在 FILES 与 SYS 之间；
     *   M5 AI tab（小智对话）插在 REC 与 SYS 之间 ⇒
     *   页序 CAM(0)/VIDEO(1)/FILES(2)/REC(3)/AI(4)/SYS(5)。
     * ★ S6：AI tab 移除（升级为 lv_layer_top 浮窗 overlay_ai，SET/MODE
     *   硬件键进出）⇒ 页序 CAM(0)/VIDEO(1)/FILES(2)/REC(3)/SYS(4)。 */
    lv_obj_t *tab_vid  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_IMAGE"  VIDEO");
    s_tab_vid_idx = lv_tabview_get_tab_count(s_tabview) - 1;
    lv_obj_t *tab_files = lv_tabview_add_tab(s_tabview, LV_SYMBOL_LIST"  FILES");
    s_tab_files_idx = lv_tabview_get_tab_count(s_tabview) - 1;
    lv_obj_t *tab_rec  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_AUDIO"  REC");
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
        s_group_sys = lv_group_create();
        lv_group_add_obj(s_group_sys, s_tab_btns);
        lv_indev_set_group(indev, s_group_sys);
    }

    tab_camera_build(tab_cam, NULL);
    tab_video_build(tab_vid, NULL);
    tab_files_build(tab_files, NULL);
    tab_record_build(tab_rec, NULL);
    tab_sys_entry_build(tab_sys, s_group_sys);

    /* Pull-down wallpaper overlay on lv_layer_top. */
    overlay_wallpaper_init(s_tab_btns);

    /* S6：AI 浮窗（lv_layer_top）+ SET/MODE 硬件键绑定。 */
    overlay_ai_init();

    ESP_LOGI(TAG, "main page created");
    return page;
}

static void main_destroy(lv_obj_t *page)
{
    tab_camera_destroy();
    tab_video_destroy();
    tab_files_destroy();
    tab_record_destroy();
    tab_sys_entry_destroy();
    overlay_ai_deinit();
    overlay_wallpaper_deinit();

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
    if (indev && s_group_sys && s_tabview) {
        uint16_t tab = lv_tabview_get_tab_act(s_tabview);
        lv_group_set_editing(s_group_sys, false);
        /* ★ 页序 CAM(0)/VIDEO(1)/FILES(2)/REC(3)/SYS(4)（S6：AI tab 移除，
         *   升级为 overlay_ai 浮窗）。
         *   CAM/VIDEO/FILES/REC 在 build 时传的就是 NULL，**没有** encoder 组，
         *   所以刻意不给它们 case，落到 switch 外面保持上一个组的绑定。
         *   ★ 本分支仅在存在 ENCODER 输入设备时才会执行（触摸方案下 s_group_* 全为 NULL，
         *     整个 if 直接跳过），因此这次改动对当前硬件无影响，只是别留错。 */
        if (tab == 4) {
            lv_group_add_obj(s_group_sys, s_tab_btns);
            lv_indev_set_group(indev, s_group_sys);
        }
        lv_tabview_set_act(s_tabview, tab, false);
    }
}

const Page_t Page_Main = {
    .create = main_create,
    .destroy = main_destroy,
    .name = "Main",
};
