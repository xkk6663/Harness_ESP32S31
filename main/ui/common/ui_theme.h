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

#ifdef __cplusplus
}
#endif
