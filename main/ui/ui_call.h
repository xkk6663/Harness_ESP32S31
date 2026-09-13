/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_call.h
 * @brief 通话 tab（UDP 低延迟通话占位页）
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_call_get_screen(void);
void app_disp_lvgl_show_call(lv_obj_t *screen, lv_group_t *group);

#ifdef __cplusplus
}
#endif
