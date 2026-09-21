/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_manager.h
 * @brief Mutex-style manager for lv_layer_top overlays (wallpaper, volume
 *        popup). At most one overlay is active at a time; opening a new one
 *        dismisses the current one to avoid touch conflicts.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_WALLPAPER,   /* pull-down wallpaper layer  */
} overlay_type_t;

/* Open an overlay (auto-closes the active one). */
void overlay_open(overlay_type_t type);

/* Close a specific overlay (no-op if it is not the active one). */
void overlay_close(overlay_type_t type);

/* True when any overlay is active. */
bool overlay_any_active(void);

/* Currently active overlay type. */
overlay_type_t overlay_get_active(void);

#ifdef __cplusplus
}
#endif
