/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_common.h
 * @brief Reusable widget factory: full-screen page root, title bar with a
 *        back arrow, and a settings list row. Keeps visual style consistent.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a full-screen, non-scrollable page root with COL_BG background. */
lv_obj_t *ui_common_page_root(lv_obj_t *parent);

/* Create a title bar: back arrow (PageManager_Back) on the left + title text. */
lv_obj_t *ui_common_title_bar(lv_obj_t *parent, const char *title);

/* Create a settings row "icon  text ........ >" with a click callback. */
lv_obj_t *ui_common_setting_row(lv_obj_t *parent, const char *icon,
                                const char *text, lv_event_cb_t click_cb,
                                void *user_data);

/* Create a centered placeholder info card (used by not-yet-built pages). */
lv_obj_t *ui_common_placeholder(lv_obj_t *parent, const char *text);

/* Create a vertical list container.
 * ★ 替代已弃用的 lv_list_create()：lv_list 本质就是 flex-column 的 lv_obj。 */
lv_obj_t *ui_common_list(lv_obj_t *parent);

/* Append a full-width "symbol + text" row button to a list created by
 * ui_common_list(). Returns the button (attach LV_EVENT_CLICKED to it).
 * ★ 替代已弃用的 lv_list_add_button()，逐行复刻其官方实现。 */
lv_obj_t *ui_common_list_add_button(lv_obj_t *list, const void *symbol,
                                    const char *text);

/* ---- 统一视觉语言：卡片与按钮（对齐 tab_call.c 的既有风格）---- */

/* A raised card surface: COL_CARD background + hairline border + radius. */
lv_obj_t *ui_common_card(lv_obj_t *parent, int w, int h, int radius);

/* Filled cyan pill button for the PRIMARY action (text is set as its label). */
lv_obj_t *ui_common_accent_button(lv_obj_t *parent, int w, int h,
                                  const char *text);

/* Outlined cyan pill button for SECONDARY actions / mode toggles. */
lv_obj_t *ui_common_ghost_button(lv_obj_t *parent, int w, int h,
                                 const char *text);

#ifdef __cplusplus
}
#endif
