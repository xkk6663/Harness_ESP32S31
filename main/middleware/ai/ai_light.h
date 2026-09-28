/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_light.h
 * @brief 小智 MCP 灯控工具（§55）：把板载 WS2812（led_service）挂成小智的
 *        "台灯"设备，语音即可 开灯/关灯/调色/调亮度。
 *
 * 工具集（78 官方 self.light 命名规范，服务器 tools/list 自动发现）：
 *   self.light.get_state     查询电源/颜色/亮度
 *   self.light.set_power     开关灯（power: bool）
 *   self.light.set_rgb       调色并开灯（r/g/b: 0~255）
 *   self.light.set_brightness调亮度（0~100，0 等于关灯）
 *
 * 线程模型：MCP 工具回调跑在协议栈任务上下文；led_service 是命令队列 +
 * 专用任务，post 即线程安全。AI 任何设置都会 set_manual(true) 接管
 * （WiFi 状态指示灯让位），恢复状态模式走 RGB 页。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_mcp_s esp_mcp_t;

/**
 * @brief 把灯控工具注册进小智 MCP 引擎（chat_setup 里、chat_init 前调用）。
 * @param mcp 已创建的空 MCP 引擎（所有权仍在调用方/chat_deinit）。
 */
esp_err_t ai_light_attach(esp_mcp_t *mcp);

#ifdef __cplusplus
}
#endif
