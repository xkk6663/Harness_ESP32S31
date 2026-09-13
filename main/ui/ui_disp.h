/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_disp.h
 * @brief UI 层对外门面
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建并显示主界面（CALL / REC / SYS 三 tab）
 * @note  由开屏上拉后回调调用，内部自行 bsp_display_lock
 */
void app_disp_lvgl_show_main(void);

/**
 * @brief 在 WAV 弹窗关闭后重新绑定输入设备组
 */
void set_tab_group(void);

#ifdef __cplusplus
}
#endif
