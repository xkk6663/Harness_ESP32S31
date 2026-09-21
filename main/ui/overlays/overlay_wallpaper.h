/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wallpaper.h
 * @brief Pull-down wallpaper overlay. The JPEG canvas builder is shared with
 *        the splash page; gesture handling lives on the main tab bar.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build a fullscreen wallpaper container + decoded JPEG canvas.
 *        Shared by page_splash and the main pull-down overlay.
 * @param parent Container parent.
 * @return Wallpaper container object.
 */
lv_obj_t *overlay_wallpaper_build(lv_obj_t *parent);

/**
 * @brief Create the pull-down wallpaper on lv_layer_top() and bind the main
 *        tab-bar pull-down / wallpaper swipe-up gestures. Called once when
 *        Page_Main is created.
 * @param tab_btns Main tab-view tab-bar object (gesture source).
 */
void overlay_wallpaper_init(lv_obj_t *tab_btns);

/**
 * @brief Remove the wallpaper layer (called when Page_Main is destroyed).
 */
void overlay_wallpaper_deinit(void);

/* overlay_manager hooks: snap fully open / snap shut. */
void overlay_wallpaper_show(void);
void overlay_wallpaper_hide(void);

#ifdef __cplusplus
}
#endif
