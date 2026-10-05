/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_uext.h
 * @brief USB 副屏（uext 固件）切换确认浮层 —— 2s 倒计时确认卡
 *
 * 方案：docs/项目规划/USB副屏集成技术方案.md V1.1 §4.2 / §4.4（D2 拍板：
 * 单击 SET → 弹确认卡 → 2s 内再次按 SET 才切槽重启；超时自动取消）。
 *
 * 线程模型（与 overlay_ota 相同，F44）：
 *   button 任务（overlay_uext_notify_set_click）──► 只置 volatile 标志
 *   taskLVGL（lv_timer 消费）                     ──► 碰 LVGL / 执行切槽
 *
 * 防护：OTA 升级进行中（ota_service_is_active）拒绝切换；uext 分区不存在时
 * 只打日志不动作。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 建卡片 + 起定时器（幂等）。须在 LVGL 就绪后调用（page_main 挂载点）。 */
void overlay_uext_init(void);

/** 删定时器 + 删卡片（幂等）。 */
void overlay_uext_deinit(void);

/**
 * SET 单击入口。可在任意任务上下文调用（按钮任务）；内部只写 volatile。
 * 未 init 时安全（忽略）。
 */
void overlay_uext_notify_set_click(void);

void overlay_uext_show(void);
void overlay_uext_hide(void);
bool overlay_uext_is_visible(void);

#ifdef __cplusplus
}
#endif
