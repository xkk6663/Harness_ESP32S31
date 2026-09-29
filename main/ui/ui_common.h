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

/* ---------------------------------------------------------------------------
 *  ★ S8-1 新增：只在文本**真的变了**才写标签
 *
 *  为什么需要它：本工程的 UI 惯例是"50 ms/1 s 定时器刷新一切状态"，
 *  而 `lv_label_set_text()` 每次都 malloc（LCD 刷屏 + 堆碎片），
 *  `lv_label_set_text_fmt()` 更是每次都格式化 + malloc。定时器里无脑调用
 *  等于每秒几十次堆分配 —— 只有"变了才写"才能让它变回零代价。
 *
 *  ★★ 语义陷阱（照抄时最容易踩的一条）：
 *     text 是**动态缓冲**（栈上的 char buf[]）时，必须走 `lv_label_set_text()`
 *     的**拷贝**语义。若误用 `lv_label_set_text_static()`，标签会永久持有一个
 *     **已失效的栈指针** —— 表现是"过一会儿文字变成乱码/整页崩"，
 *     而且堆栈一复位就再也复现不出来。本函数内部固定用拷贝语义。
 * ------------------------------------------------------------------------- */
void ui_common_set_label_text_if_changed(lv_obj_t *label, const char *text);

#ifdef __cplusplus
}
#endif
