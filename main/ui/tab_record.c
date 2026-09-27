/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_record.c
 * @brief REC tab：本地录音一键录/停。后端 rec_local_service（ext 独占租借
 *        mic），存 /sdcard/music/rec_XXX.wav；回放在 FILES 页点开（本地闭环）。
 *
 * 线程纪律（F44）：本文件不做任何阻塞 IO，全靠 100ms lv_timer 轮询
 * rec_local_service 的查询 API 刷 UI（§39：is_active 含 pending 段，不会撞
 * 建链窗口误判）。
 */

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_record.h"
#include "ui_theme.h"
#include "middleware/audio/rec_local_service.h"

static lv_obj_t   *s_btn;
static lv_obj_t   *s_btn_label;
static lv_obj_t   *s_status;
static lv_obj_t   *s_time_label;
static lv_obj_t   *s_hint;
static lv_timer_t *s_timer;
static bool        s_prev_active;

/* ============================================================
 * 交互
 * ============================================================ */

static void rec_btn_cb(lv_event_t *e)
{
    (void)e;
    if (rec_local_service_is_active()) {
        rec_local_stop();
        lv_label_set_text_static(s_status, "Saving...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
        return;
    }
    esp_err_t err = rec_local_start();
    if (err != ESP_OK) {
        lv_label_set_text_static(s_status, "Start failed (busy?)");
        lv_obj_set_style_text_color(s_status, COL_RED, 0);
    }
    /* 成功态由 timer 轮询接管（pending → Recording...） */
}

/* ============================================================
 * 轮询刷新（100ms）：唯一的状态行/按钮落笔点
 * ============================================================ */

static void rec_timer_cb(lv_timer_t *t)
{
    (void)t;
    bool act = rec_local_service_is_active();

    if (act != s_prev_active) {
        if (act) {
            /* pending / recording */
            lv_label_set_text_static(s_btn_label, LV_SYMBOL_STOP "  STOP & SAVE");
            lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
            lv_label_set_text_static(s_status, "Recording...");
            lv_obj_set_style_text_color(s_status, COL_RED, 0);
        } else {
            lv_label_set_text_static(s_btn_label, LV_SYMBOL_AUDIO "  REC");
            lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
            lv_label_set_text_static(s_time_label, "00:00");
            if (s_prev_active) {
                /* 刚收尾：报结果 */
                if (rec_local_last_ok()) {
                    lv_label_set_text_fmt(s_status, "Saved: %s",
                                          rec_local_last_path());
                    lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
                } else {
                    lv_label_set_text_static(s_status, "Record FAILED");
                    lv_obj_set_style_text_color(s_status, COL_RED, 0);
                }
            }
        }
        s_prev_active = act;
    }

    if (act) {
        uint32_t ms = rec_local_elapsed_ms();
        lv_label_set_text_fmt(s_time_label, "%02u:%02u",
                              (unsigned)(ms / 60000U), (unsigned)((ms / 1000U) % 60U));
    }
}

/* ============================================================
 * 建 / 拆
 * ============================================================ */

void tab_record_build(lv_obj_t *screen, lv_group_t *group)
{
    (void)group;    /* 触摸方案：与 CAM/VIDEO/FILES 一致，不占 encoder 组 */

    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    /* Title（左上） */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_AUDIO "  REC");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Status（右上） */
    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Idle");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    /* 大按钮（中央偏上）：待机 RED"REC" / 录音中 RED"STOP & SAVE" */
    s_btn = lv_btn_create(screen);
    lv_obj_set_size(s_btn, 260, 88);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_radius(s_btn, 20, 0);
    lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
    lv_obj_set_style_border_width(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_darken(COL_RED, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn, rec_btn_cb, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text_static(s_btn_label, LV_SYMBOL_AUDIO "  REC");
    lv_obj_set_style_text_color(s_btn_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_label, UI_FONT, 0);
    lv_obj_center(s_btn_label);

    /* 时长（按钮下方） */
    s_time_label = lv_label_create(screen);
    lv_label_set_text_static(s_time_label, "00:00");
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, 36);
    lv_obj_set_style_text_color(s_time_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, UI_FONT, 0);

    /* 提示（底部） */
    s_hint = lv_label_create(screen);
    lv_label_set_text_static(s_hint,
        "Saved to /sdcard/music  -  play in FILES tab");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT, 0);

    /* 恢复态：build 发生在页面切换回来时，若服务还在录（跨 tab 录音合法），
     * timer 首拍会自动把按钮翻到 STOP & SAVE 并续显时长 */
    s_prev_active = !rec_local_service_is_active();  /* 强制首拍走一次分支 */

    if (s_timer == NULL) {
        s_timer = lv_timer_create(rec_timer_cb, 100, NULL);
    }
}

void tab_record_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_btn = NULL;
    s_btn_label = NULL;
    s_status = NULL;
    s_time_label = NULL;
    s_hint = NULL;
    /* ★ 故意不 stop：录音是后台服务，切页/回主页不断录（状态在服务侧，
     *   重进 tab 由 timer 恢复显示）。要停请按 STOP & SAVE。 */
}
