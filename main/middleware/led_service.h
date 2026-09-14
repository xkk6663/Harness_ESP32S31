/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file led_service.h
 * @brief 中间层：RGB LED 服务（WS2812 on GPIO37, RMT 后端）
 *
 * 并发保护（FreeRTOS）：
 *  - 内部建命令队列 + 专用 LED 任务；所有硬件操作串行执行；
 *  - UI(LVGL task) / WIFI(esp event task) 只 post 命令，不直接碰 LED 驱动，
 *    从根本上消除跨任务并发访问 led_indicator/RMT 的竞争。
 *  - 主界面 set_status() 反映 Wi-Fi 状态；RGB 页 set_manual() 接管为手动。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATUS_DISCONNECTED = 0,
    LED_STATUS_CONNECTING,
    LED_STATUS_CONNECTED,
} led_status_t;

void led_service_init(void);

void led_service_set_status(led_status_t status);
void led_service_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void led_service_set_on(bool on);
void led_service_set_manual(bool manual);

#ifdef __cplusplus
}
#endif
