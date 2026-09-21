/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_main.h
 * @brief Main page: CALL / REC / SYS tab-view + pull-down wallpaper overlay.
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const Page_t Page_Main;

/* Rebind the encoder group to the active tab (formerly set_tab_group). */
void page_main_refresh_group(void);

#ifdef __cplusplus
}
#endif
