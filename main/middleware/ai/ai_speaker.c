/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_speaker.c
 * @brief 小智 MCP 音量工具实现（§56，见头文件）。
 *
 * 工具回调在协议栈任务上下文；三个落点（button_service 状态源、
 * audio_service 命令队列、xiaozhi 借出 spk 句柄）均线程安全。
 */

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"

#include "esp_mcp_engine.h"

#include "ai_speaker.h"
#include "middleware/periph/button_service.h"
#include "middleware/audio/audio_service.h"
#include "xiaozhi_service.h"

static const char *TAG = "AI_SPK";

/* 板子上限与 button_service 一致（AUDIO_CMD_SET_VOLUME value: 0..90） */
#define SPK_VOL_MAX   90

static esp_mcp_value_t cb_set_volume(const esp_mcp_property_list_t *props)
{
    int v = esp_mcp_property_list_get_property_int(props, "volume");
    v = (v < 0) ? 0 : (v > 100 ? 100 : v);
    int board = (v > SPK_VOL_MAX) ? SPK_VOL_MAX : v;

    button_service_set_volume(board);          /* ① 全局状态源 */
    audio_cmd_t cmd = {
        .id = AUDIO_CMD_SET_VOLUME,
        .value = board,
    };
    audio_service_post_cmd(&cmd);              /* ② 播放态生效 */
    xiaozhi_service_set_volume(board);         /* ③ 小智对话态生效 */

    ESP_LOGI(TAG, "set_volume(%d) -> board %d", v, board);
    return esp_mcp_value_create_int(board);
}

static esp_mcp_value_t cb_get_volume(const esp_mcp_property_list_t *props)
{
    (void)props;
    int v = button_service_get_volume();
    ESP_LOGI(TAG, "get_volume -> %d", v);
    return esp_mcp_value_create_int(v);
}

esp_err_t ai_speaker_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");

    esp_mcp_property_t *p_vol =
        esp_mcp_property_create_with_int_and_range("volume", 70, 0, 100);
    ESP_RETURN_ON_FALSE(p_vol, ESP_FAIL, TAG, "prop volume");

    esp_mcp_tool_t *set_tool = esp_mcp_tool_create(
        "self.speaker.set_volume",
        "设置扬声器音量。volume: 0~100，0 是静音",
        cb_set_volume);
    ESP_RETURN_ON_FALSE(set_tool, ESP_FAIL, TAG, "tool set_volume");
    ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(set_tool, p_vol) == ESP_OK,
                        ESP_FAIL, TAG, "add prop");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, set_tool) == ESP_OK,
                        ESP_FAIL, TAG, "add tool set_volume");

    esp_mcp_tool_t *get_tool = esp_mcp_tool_create(
        "self.speaker.get_volume",
        "查询当前扬声器音量",
        cb_get_volume);
    ESP_RETURN_ON_FALSE(get_tool, ESP_FAIL, TAG, "tool get_volume");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, get_tool) == ESP_OK,
                        ESP_FAIL, TAG, "add tool get_volume");

    ESP_LOGI(TAG, "mcp tools: self.speaker.set_volume / get_volume");
    return ESP_OK;
}
