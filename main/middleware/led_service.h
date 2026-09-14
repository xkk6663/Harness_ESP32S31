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
 *  - led_service_init() 初始化 BSP LED indicator，默认慢呼吸；
 *  - led_service_set_mode() 切换 BSP 效果；
 *  - led_service_set_rgb() 自定义颜色（常亮）；
 *  - led_service_set_on() 开关（保持当前颜色）；
 *  - 不依赖 LVGL，不暴露 handle 给上层。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
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

/**
 * @brief 设置自定义 RGB 颜色并常亮显示
 * @param r,g,b 0-255
 */
void led_service_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 开 / 关 LED（on 时保持当前颜色常亮，off 熄灭）
 */
void led_service_set_on(bool on);

#ifdef __cplusplus
}
#endif
