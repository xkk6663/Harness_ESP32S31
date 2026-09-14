/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file button_service.h
 * @brief 中间层：4 个 ADC 分压按键服务（set / mode / vol- / vol+）
 *
 * 设计：
 *  - 基于 BSP bsp_iot_button_create + iot_button 组件；
 *  - 对外暴露逻辑按键名 btn_id_t（命名与官方 factory_demo 一致）；
 *  - 通过事件回调通知上层（在 iot_button 任务上下文执行）；
 *  - 不依赖 LVGL。
 *
 * 硬件：R77=10K 上拉到 3V3，共 ADC1_CH0
 *   set  (SW3, R79=13K) -> 1.87V
 *   mode (SW4, R80=6.8K) -> 1.34V
 *   vol- (SW5, R81=3.3K) -> 0.82V
 *   vol+ (SW6, R82=1.3K) -> 0.38V
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 逻辑按键 ID（与官方 factory_demo 命名一致） ---- */
typedef enum {
    BTN_ID_SET = 0,
    BTN_ID_MODE,
    BTN_ID_VOLDOWN,   /* vol- */
    BTN_ID_VOLUP,     /* vol+ */
    BTN_ID_NUM,
} btn_id_t;

/* ---- 按键事件 ---- */
typedef enum {
    BTN_EVT_PRESS_DOWN = 0,
    BTN_EVT_PRESS_UP,
    BTN_EVT_SINGLE_CLICK,
    BTN_EVT_DOUBLE_CLICK,
    BTN_EVT_PRESS_END,
} btn_evt_t;

/**
 * @brief 按键事件回调类型
 * @param id    逻辑按键 ID
 * @param evt   事件类型
 * @note  在 iot_button 任务上下文执行，不要在此回调里做阻塞操作；
 *        若需操作 LVGL，调用方自行 bsp_display_lock/unlock。
 */
typedef void (*btn_evt_cb_t)(btn_id_t id, btn_evt_t evt);

/**
 * @brief 音量变化消息（发送到音量事件队列）
 */
typedef struct {
    int volume;   /* 新音量 0..90 */
} btn_vol_msg_t;

/**
 * @brief 初始化 4 个 ADC 按键
 */
void button_service_init(void);

/**
 * @brief 注册按键事件回调（覆盖式，后注册者生效）
 */
void button_service_set_evt_cb(btn_evt_cb_t cb);

/**
 * @brief 获取音量事件队列（UI 层 xQueueReceive 用）
 */
QueueHandle_t button_service_get_vol_queue(void);

/**
 * @brief 获取当前音量（线程安全）
 */
int button_service_get_volume(void);

/**
 * @brief 设置当前音量（线程安全，UI 滑块拖动时同步）
 */
void button_service_set_volume(int vol);

#ifdef __cplusplus
}
#endif
