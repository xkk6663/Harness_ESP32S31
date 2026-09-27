/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_ai.h
 * @brief AI 浮窗（S6）：小智对话页从 tabview tab 升级为 lv_layer_top 浮层。
 *
 * 交互（S6 定案）：
 *   - SET 单击：窗关 → 弹出 + xiaozhi_start()；CHATTING → stop()；
 *     IDLE(已结束) → 收窗。三击循环「唤起→结束→收起」，屏幕 X 兜底。
 *   - MODE 按住 = PTT（xiaozhi_service_ptt_start），松开 = ptt_stop；
 *     IDLE 时按下自动 start() 并记 pending，进入 CHATTING 后自动补 PTT。
 *   - 屏幕按钮全部保留，与硬件键驱动同一组请求函数，同一 timer 刷新。
 *
 * 线程纪律（F44 同族）：按键回调（iot_button 任务上下文）只置 volatile 标志
 * + 调 xiaozhi_service_*（内部只置期望态）；LVGL 落笔全部在 50ms lv_timer。
 */

#pragma once

#include <stdbool.h>
#include "middleware/periph/button_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 硬件按键入口（button_service 回调，iot_button 任务上下文）。
 *        只置 volatile 标志 + 调 xiaozhi_service_*（无 LVGL，F44 纪律）。
 */
void overlay_ai_key_feed(btn_id_t id, btn_evt_t evt);

/** 构建浮窗（lv_layer_top，初始在屏幕下方）+ 注册硬件按键回调。建页后调一次。 */
void overlay_ai_init(void);

/** 拆除浮窗与事件通路（Page_Main 销毁时）。 */
void overlay_ai_deinit(void);

/* overlay_manager 钩子：滑入 / 滑出（无动画依赖，管理器保证互斥）。 */
void overlay_ai_show(void);
void overlay_ai_hide(void);

/** 浮窗当前是否在屏上（SET 状态机与按键自动开窗判据）。 */
bool overlay_ai_is_visible(void);

#ifdef __cplusplus
}
#endif
