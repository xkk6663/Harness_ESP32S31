/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_call.h
 * @brief CALL tab content builder (low-latency UDP voice link placeholder).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_call_build(lv_obj_t *tab, lv_group_t *group);

#ifdef __cplusplus
}
#endif
