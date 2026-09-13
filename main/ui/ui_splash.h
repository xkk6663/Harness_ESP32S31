/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_splash.h
 * @brief 开屏壁纸：全屏显示 xwkkk.jpg，上拉后进入主界面
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 进入主界面的回调（在 LVGL 任务上下文调用）
 */
typedef void (*ui_splash_enter_cb_t)(void);

/**
 * @brief 显示开屏壁纸，监听上拉手势
 * @param cb 上拉手势识别成功后回调（内部已持 bsp_display_lock）
 */
void ui_splash_show(ui_splash_enter_cb_t cb);

#ifdef __cplusplus
}
#endif
