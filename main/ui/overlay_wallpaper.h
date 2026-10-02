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

/* §75 注：原 overlay_wallpaper_build() 已收回本模块内部（static）——
 * 图片内容由 wallpaper_service 双缓冲提供，本文件只做 canvas 装配与 crossfade，
 * 不再有独立的外部构建入口。 */

/**
 * @brief Create the pull-down wallpaper on lv_layer_top() and bind the main
 *        tab-bar pull-down / wallpaper swipe-up gestures. Called once when
 *        Page_Main is created.
 * @param tab_btns Main tab-view tab-bar object (gesture source).
 */
void overlay_wallpaper_init(lv_obj_t *tab_btns);

/** Remove the wallpaper layer (called when Page_Main is destroyed). */
void overlay_wallpaper_deinit(void);

/* overlay_manager hooks: snap fully open / snap shut. */
void overlay_wallpaper_show(void);
void overlay_wallpaper_hide(void);

#ifdef __cplusplus
}
#endif
