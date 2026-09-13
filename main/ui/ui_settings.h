/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建设置 tab 页面（亮度滑块）
 */
void app_disp_lvgl_show_settings(lv_obj_t *screen, lv_group_t *group);

#ifdef __cplusplus
}
#endif
