/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_splash.h
 * @brief Splash wallpaper: show xwkkk.jpg, swipe up to enter main UI
 *        Also exposes a reusable wallpaper layer for pull-down reveal.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter-main-UI callback (called in LVGL task context).
 */
typedef void (*ui_splash_enter_cb_t)(void);

/**
 * @brief Show splash wallpaper, listen for swipe-up gesture.
 * @param cb Called when swipe-up detected (already under bsp_display_lock).
 */
void ui_splash_show(ui_splash_enter_cb_t cb);

/**
 * @brief Attach a fullscreen wallpaper layer to parent, initially off-screen above.
 *        Decodes xwkkk.jpg once into file_buffer (PSRAM) and reuses it.
 * @param parent Parent object (usually lv_layer_top()).
 * @return The wallpaper container; caller can lv_anim its y position.
 */
lv_obj_t *ui_wallpaper_attach(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
