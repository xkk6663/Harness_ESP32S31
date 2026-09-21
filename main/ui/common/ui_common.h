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

#ifdef __cplusplus
}
#endif
