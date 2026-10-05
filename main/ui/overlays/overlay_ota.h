/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_ota.h
 * @brief M3：OTA 升级浮层（居中卡片 + 进度条 + "请勿断电"警示）。
 *
 * ── 线程纪律（本文件唯一的设计要点）────────────────────────────────
 *  `ota_service` 的事件回调跑在 **ota_task 上下文**（PSRAM 栈的下载任务），
 *  而规范 §4 明令「非 LVGL 线程不得碰 LVGL」。
 *  ⇒ 回调里 **只允许写几个 volatile 标志**；真正的 `lv_*` 调用全部集中在
 *    本模块自己的 `lv_timer`（跑在 taskLVGL 上下文）里。
 *
 *       ota_task ──(evt_cb 置标志)──► s_pending_evt / s_pending_pct
 *                                              │
 *       taskLVGL ──(lv_timer 1Hz 读取)───────┴──► 刷新卡片
 *
 * ── 生命周期 ─────────────────────────────────────────────────────
 *  `overlay_ota_init()` 在 Page_Main 建好后调用（此时 lv_layer_top 可用，
 *  ota_service 也已 init）。它做两件事：
 *    ① 建卡片（初始隐藏）+ 建 lv_timer；
 *    ② 用 `ota_service_set_evt_cb()` 订阅升级事件。
 *  `overlay_ota_deinit()` 在 Page_Main 销毁时调用（解订阅 + 删定时器 + 删卡片）。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建 OTA 浮层（隐藏态）并订阅 ota_service 事件。
 *        必须在 lv_init() 之后、且 ota_service_init() 之后调用。
 */
void overlay_ota_init(void);

/**
 * @brief 解订阅 + 删定时器 + 删卡片。幂等。
 */
void overlay_ota_deinit(void);

/**
 * @brief 主动显示浮层（调试/手动验证用；正常由事件驱动）。
 */
void overlay_ota_show(void);

/** @brief 主动隐藏浮层。 */
void overlay_ota_hide(void);

/** @brief 浮层当前是否可见（供 overlay_manager 互斥判断）。 */
bool overlay_ota_is_visible(void);

#ifdef __cplusplus
}
#endif
