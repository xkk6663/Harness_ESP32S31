/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_splash.h
 * @brief Boot splash page (stack root). Fullscreen wallpaper, swipe up to
 *        enter the main UI via the registered enter callback.
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*page_splash_enter_cb_t)(void);

/* Set the callback fired once when swipe-up is detected. */
void page_splash_set_enter_cb(page_splash_enter_cb_t cb);

extern const Page_t Page_Splash;

#ifdef __cplusplus
}
#endif
