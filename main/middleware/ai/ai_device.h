/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_device.h
 * @brief 小智 MCP 设备工具：Wi-Fi 状态查询 + 系统重启（S7-C 级，§58b）。
 *
 * 注意：背景调光该开发板不支持（用户确认，不做）。
 *
 * ★ 勘误（§58b）：原设计的 self.wifi.set_enabled 已阉割——
 *   "关"= 自杀操作（断网即断会话，语音不可恢复，只能按键/重启）；
 *   "开"逻辑上不可达（能调工具 ⇒ 会话活着 ⇒ Wi-Fi 已开）。
 *   LLM 误触发代价（设备变砖）远大于收益。只保留只读 get_status。
 */

#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 MCP 设备工具（chat_setup 里与其他 ai_* 并列挂载）：
 *        self.wifi.get_status / self.device.reboot
 *        （self.wifi.set_enabled 已阉割，见文件头勘误）
 */
esp_err_t ai_device_attach(esp_mcp_t *mcp);

#ifdef __cplusplus
}
#endif
