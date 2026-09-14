/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file led_service.h
 * @brief 中间层：RGB LED 服务（WS2812 on GPIO37, RMT 后端）
 *
 * 设计：
 *  - led_service_init() 初始化 BSP LED indicator；
 *  - 提供呼吸/常亮/熄灭等常用模式切换；
 *  - 不依赖 LVGL，不暴露 led_indicator_handle_t 给上层。
 */

#pragma once

#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RGB LED，默认进入慢呼吸
 */
void led_service_init(void);

/**
 * @brief 切换 LED 模式（BSP_LED_* 枚举，如 BSP_LED_BREATHE_SLOW / BSP_LED_ON / BSP_LED_OFF）
 */
void led_service_set_mode(bsp_led_effect_t mode);

#ifdef __cplusplus
}
#endif
