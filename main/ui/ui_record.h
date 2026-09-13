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
 * @brief 创建录音 tab 页面（REC / 播放 / 停止）
 */
void app_disp_lvgl_show_record(lv_obj_t *screen, lv_group_t *group);

#ifdef __cplusplus
}
#endif
