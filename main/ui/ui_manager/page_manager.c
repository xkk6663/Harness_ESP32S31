/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_manager.c
 * @brief Stack page navigator implementation, adapted for bsp_display_lock.
 */

#include "page_manager.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char *TAG = "PAGE_MGR";

typedef struct {
    const Page_t *page;
    lv_obj_t     *obj;
} StackEntry_t;

static StackEntry_t s_stack[PAGE_STACK_MAX];
static uint8_t      s_top = 0;

#define PAGE_ANIM_MS  200

/* one delayed page teardown (runs in LVGL task after a Back event settles) */
static StackEntry_t s_pending_destroy;

static void pending_destroy_cb(lv_timer_t *t)
{
    (void)t;
    if (s_pending_destroy.page && s_pending_destroy.page->destroy) {
        s_pending_destroy.page->destroy(s_pending_destroy.obj);
    }
    s_pending_destroy.page = NULL;
    s_pending_destroy.obj = NULL;
}

void PageManager_Init(void)
{
    s_top = 0;
}

void PageManager_Load(const Page_t *page)
{
    if (!page || !page->create) {
        ESP_LOGE(TAG, "invalid page");
        return;
    }
    if (s_top >= PAGE_STACK_MAX) {
        ESP_LOGW(TAG, "page stack full, drop %s", page->name);
        return;
    }

    bsp_display_lock(0);

    /* A page root is created with NULL parent so it is a real LVGL screen. */
    lv_obj_t *new_obj = page->create(NULL);

    s_stack[s_top].page = page;
    s_stack[s_top].obj  = new_obj;
    s_top++;

    if (s_top == 1) {
        /* First (bottom) page: show immediately, no transition. */
        lv_scr_load(new_obj);
    } else {
        lv_scr_load_anim(new_obj, LV_SCR_LOAD_ANIM_FADE_IN, PAGE_ANIM_MS, 0, false);
    }

    ESP_LOGI(TAG, "load %s (depth=%d)", page->name, s_top);

    bsp_display_unlock();
}

void PageManager_Back(void)
{
    if (s_top <= 1) {
        ESP_LOGW(TAG, "already at root page");
        return;
    }

    bsp_display_lock(0);

    s_top--;
    const Page_t *cur = s_stack[s_top].page;
    lv_obj_t *prev = s_stack[s_top - 1].obj;

    /*
     * We are inside a click event on the page being torn down (its back
     * button). Deleting that screen synchronously would free the very object
     * the LVGL event dispatcher is still using -> crash. Switch screens first,
     * then destroy the popped page from a one-shot timer that runs after the
     * event / fade animation has settled.
     */
    lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_FADE_IN, PAGE_ANIM_MS, 0, false);

    if (cur->destroy) {
        s_pending_destroy.page = cur;
        s_pending_destroy.obj = s_stack[s_top].obj;
        lv_timer_t *tm = lv_timer_create(pending_destroy_cb, PAGE_ANIM_MS + 30, NULL);
        lv_timer_set_repeat_count(tm, 1);
    }

    ESP_LOGI(TAG, "back to %s (depth=%d)", s_stack[s_top - 1].page->name, s_top);

    bsp_display_unlock();
}

void PageManager_BackToMain(void)
{
    if (s_top <= 1) {
        return;
    }

    bsp_display_lock(0);

    while (s_top > 1) {
        s_top--;
        const Page_t *cur = s_stack[s_top].page;
        if (cur->destroy) {
            cur->destroy(s_stack[s_top].obj);
        }
    }
    lv_scr_load(s_stack[0].obj);

    bsp_display_unlock();
}

const Page_t *PageManager_GetCurrent(void)
{
    if (s_top == 0) {
        return NULL;
    }
    return s_stack[s_top - 1].page;
}
