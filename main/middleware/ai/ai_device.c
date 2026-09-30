/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_device.c
 * @brief 小智 MCP 设备工具实现（S7-C / §58，见头文件）。
 *
 * 并发模型：两个工具回调全部即时返回（无阻塞动作）：
 *   - get_status 纯查询，直接返回；
 *   - reboot 走 vTaskDelay(500) 后 safe_reboot_perform（§73.20：给 MCP
 *     response 发送留时间 + 有序收尾，不再裸 esp_restart）。
 *   - wifi.set_enabled 已阉割（§58b 勘误，见下方勘误注释）。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_mcp_engine.h"

#include "ai_device.h"
#include "middleware/net/wifi_manager.h"
#include "middleware/periph/safe_reboot.h"
#include "xiaozhi_service.h"   /* reboot 收尾时尽力停会话（§73.20） */

static const char *TAG = "AI_DEV";

/* 回调返回 JSON/文本的暂存（mcp-c-sdk 串行分发，复用安全） */
static char s_ret[192];

/* ---- 工具回调（协议栈任务上下文） ---- */

/* §58b 勘误：self.wifi.set_enabled 已阉割——"关"= 自杀操作（断网即断会话，
 * 语音不可恢复，只能按键/重启）；"开"逻辑上不可达（能调工具 ⇒ 会话活着 ⇒
 * Wi-Fi 已开）。LLM 误触发代价（设备变砖）远大于收益。保留只读 get_status。 */

static esp_mcp_value_t cb_wifi_get_status(const esp_mcp_property_list_t *props)
{
    (void)props;
    char ip[32] = "0.0.0.0";
    wifi_manager_get_ip(ip, sizeof(ip));
    snprintf(s_ret, sizeof(s_ret),
             "{\"enabled\":%s,\"role\":\"%s\",\"ip\":\"%s\"}",
             wifi_manager_is_enabled() ? "true" : "false",
             wifi_manager_get_role() == WIFI_ROLE_STA ? "sta" : "ap",
             ip);
    ESP_LOGI(TAG, "get_status -> %s", s_ret);
    return esp_mcp_value_create_string(s_ret);
}

/* 延迟重启（独立任务；给 MCP response 发送留 500ms）。
 * §73.20：重启本体从裸 esp_restart 换成 safe_reboot_perform（停相机/卸 SD/
 * 停 Wi-Fi/屏待机）——裸重启已实机复现"软重启后黑屏卡死"，见 §73.20。 */
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    xiaozhi_service_stop();   /* 尽力停会话（异步受理制，不等 teardown） */
    safe_reboot_perform("mcp device.reboot");
}

static esp_mcp_value_t cb_reboot(const esp_mcp_property_list_t *props)
{
    (void)props;
    ESP_LOGW(TAG, "reboot requested via MCP");
    if (xTaskCreate(reboot_task, "ai_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        return esp_mcp_value_create_string("Failed to schedule reboot (out of memory).");
    }
    return esp_mcp_value_create_string("OK - rebooting in 0.5 seconds.");
}

/* ---- 注册 ---- */

static esp_err_t add_tool(esp_mcp_t *mcp, const char *name, const char *desc,
                          esp_mcp_tool_callback_t cb,
                          esp_mcp_property_t *const *props, int n_props)
{
    esp_mcp_tool_t *tool = esp_mcp_tool_create(name, desc, cb);
    ESP_RETURN_ON_FALSE(tool, ESP_FAIL, TAG, "tool create %s failed", name);
    for (int i = 0; i < n_props; i++) {
        ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(tool, props[i]) == ESP_OK,
                            ESP_FAIL, TAG, "add prop to %s failed", name);
    }
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, tool) == ESP_OK,
                        ESP_FAIL, TAG, "add tool %s failed", name);
    ESP_LOGI(TAG, "mcp tool: %s (%d props)", name, n_props);
    return ESP_OK;
}

esp_err_t ai_device_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");

    /* wifi.get_status：无参数（§58b 勘误：set_enabled 已阉割，见 cb 区注释） */
    ESP_RETURN_ON_ERROR(add_tool(mcp, "self.wifi.get_status",
        "查询设备 Wi-Fi 状态（开关、模式、IP 地址）",
        cb_wifi_get_status, NULL, 0), TAG, "wifi_get");

    /* device.reboot：无参数 */
    ESP_RETURN_ON_ERROR(add_tool(mcp, "self.device.reboot",
        "重启设备。仅当用户明确要求重启时调用",
        cb_reboot, NULL, 0), TAG, "reboot");

    return ESP_OK;
}
