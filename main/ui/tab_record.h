/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_record.h
 * @brief REC tab content builder (local 5s test recording).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_record_build(lv_obj_t *tab, lv_group_t *group);

/* Re-enable buttons after an audio task finishes (called by audio evt cb). */
void tab_record_on_play_done(void);
void tab_record_on_record_done(void);

#ifdef __cplusplus
}
#endif
