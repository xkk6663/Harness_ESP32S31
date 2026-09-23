/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_btsink.h
 * @brief SINK Tab：A2DP 接收页（专辑封面 + 元数据 + AVRCP 控制）。
 *
 * ★ 本页是"有前置条件"的页：只有 A2DP 角色 = SINK 时才有内容。
 *   角色切换的唯一入口是 Bluetooth 子页面的按钮，**不在本页、也不在
 *   tab 切换回调里**（详见 bt_audio_service_switch_role 的调用契约）。
 *   本页只按当前角色/蓝牙开关状态动态显示或隐藏内容。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 构建 SINK 页。g 可为 NULL（无编码器输入设备时）。 */
void tab_btsink_build(lv_obj_t *parent, lv_group_t *g);

/** 销毁：停轮询定时器并清句柄。LVGL 对象树由父容器统一删。 */
void tab_btsink_destroy(void);

#ifdef __cplusplus
}
#endif
