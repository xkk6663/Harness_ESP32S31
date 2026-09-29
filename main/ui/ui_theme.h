/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_theme.h
 * @brief Global AI dark-blue theme: single source of colors / fonts.
 *        Every page/tab/overlay MUST include this instead of re-defining COL_*.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- AI / edge-device dark-blue palette ---- */
#define COL_BG        lv_color_hex(0x0F2547)  /* page background            */
#define COL_DARK      lv_color_hex(0x0B1120)  /* deepest background / glyph */
#define COL_CARD      lv_color_hex(0x1E3A5F)  /* card / panel surface       */
#define COL_TABBAR    lv_color_hex(0x102A4C)  /* main tab bar background    */
#define COL_BORDER    lv_color_hex(0x3B5A82)  /* hairline border            */
#define COL_CYAN      lv_color_hex(0x22D3EE)  /* accent / active            */
#define COL_GREEN     lv_color_hex(0x34D399)  /* ok / standby dot           */
#define COL_RED       lv_color_hex(0xF87171)  /* record / danger            */
#define COL_TEXT      lv_color_hex(0xE2E8F0)  /* primary text               */
#define COL_SUBTEXT   lv_color_hex(0x94A3B8)  /* secondary text             */
#define COL_TAB_TXT   lv_color_hex(0x94A3B8)  /* inactive tab label         */
#define COL_TAB_ACT   lv_color_hex(0x22D3EE)  /* active tab label           */

/* ---- shared fonts ---- */
#define UI_FONT       (&lv_font_montserrat_14)
/* ★ §73.11：CAM 页时间显示用大字号（sdkconfig 已启用 CONFIG_LV_FONT_MONTSERRAT_20）。
 *   ⚠ 用之前先确认 sdkconfig 里 20 号真的开了 —— 没开的话这里是指向空结构的
 *     未定义符号，链接期就会失败（fail loud，不算坏事）。 */
#define UI_FONT_BIG   (&lv_font_montserrat_20)

#ifdef __cplusplus
}
#endif
