/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_manager.h
 * @brief Stack-based page navigator. Each page is a full LVGL screen with
 *        create()/destroy() lifecycle. All APIs are LVGL-thread-safe (they
 *        take/release bsp_display_lock internally).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_STACK_MAX  8   /* maximum page stack depth */

/* Page descriptor: every page exposes one const instance of this. */
typedef struct {
    lv_obj_t *(*create)(lv_obj_t *parent);  /* build page, return root screen object (parent is always NULL) */
    void      (*destroy)(lv_obj_t *page);   /* tear page down (timers/cbs/objects) */
    const char *name;                       /* debug name */
} Page_t;

/* Reset navigator (call once before first load). */
void PageManager_Init(void);

/* Push a page onto the stack and fade it in (auto-locked). */
void PageManager_Load(const Page_t *page);

/* Pop current page and return to the previous one (auto-locked). */
void PageManager_Back(void);

/* Pop all pages above the stack bottom and show it (auto-locked). */
void PageManager_BackToMain(void);

/* Current page descriptor, NULL when stack empty. */
const Page_t *PageManager_GetCurrent(void);

#ifdef __cplusplus
}
#endif
