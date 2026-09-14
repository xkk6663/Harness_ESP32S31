/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_manager.c
 * @brief Mutex-style overlay scheduler.
 */

#include "overlay_manager.h"
#include "overlay_wallpaper.h"
#include "esp_log.h"

static const char *TAG = "OVERLAY";

static overlay_type_t s_active = OVERLAY_NONE;

static void invoke_hide(overlay_type_t type)
{
    switch (type) {
    case OVERLAY_WALLPAPER:
        overlay_wallpaper_hide();
        break;
    default:
        break;
    }
}

static void invoke_show(overlay_type_t type)
{
    switch (type) {
    case OVERLAY_WALLPAPER:
        overlay_wallpaper_show();
        break;
    default:
        break;
    }
}

void overlay_open(overlay_type_t type)
{
    if (type == OVERLAY_NONE) {
        return;
    }
    if (s_active == type) {
        return;
    }
    if (s_active != OVERLAY_NONE) {
        invoke_hide(s_active);
    }
    s_active = type;
    invoke_show(type);
    ESP_LOGI(TAG, "open %d", (int)type);
}

void overlay_close(overlay_type_t type)
{
    if (s_active != type) {
        return;
    }
    invoke_hide(type);
    s_active = OVERLAY_NONE;
    ESP_LOGI(TAG, "close %d", (int)type);
}

bool overlay_any_active(void)
{
    return s_active != OVERLAY_NONE;
}

overlay_type_t overlay_get_active(void)
{
    return s_active;
}
