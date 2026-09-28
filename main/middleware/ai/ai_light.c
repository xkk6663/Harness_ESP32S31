/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_light.c
 * @brief 小智 MCP 灯控工具实现（§55，见头文件）。
 *
 * 状态机（本模块私有，仅 MCP 回调上下文读写——mcp-c-sdk 串行分发工具调用）：
 *   power × (r,g,b) × brightness ⇒ 实际输出 = power ? rgb×brightness : 灭。
 * set_rgb 语义 = 调色并开灯（智能灯惯例）；brightness 0 = 关灯。
 * 所有落灯动作经 led_service 队列（线程安全），并 set_manual(true) 接管。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"

#include "esp_mcp_engine.h"

#include "ai_light.h"
#include "middleware/periph/led_service.h"

static const char *TAG = "AI_LIGHT";

/* 灯状态（MCP 工具回调上下文私有；mcp-c-sdk 串行调用工具，无并发） */
static bool s_power;
static int  s_r = 255, s_g = 180, s_b = 90;   /* 默认暖白 */
static int  s_brightness = 100;

/* 回调返回 JSON 的暂存（工具串行调用，复用安全） */
static char s_ret[64];

static void light_apply(void)
{
    if (!s_power || s_brightness <= 0) {
        led_service_set_manual(true);
        led_service_set_on(false);
        return;
    }
    led_service_set_manual(true);
    led_service_set_rgb((uint8_t)(s_r * s_brightness / 100),
                        (uint8_t)(s_g * s_brightness / 100),
                        (uint8_t)(s_b * s_brightness / 100));
    led_service_set_on(true);
}

static const char *state_json(void)
{
    snprintf(s_ret, sizeof(s_ret),
             "{\"power\":%s,\"rgb\":[%d,%d,%d],\"brightness\":%d}",
             s_power ? "true" : "false", s_r, s_g, s_b, s_brightness);
    return s_ret;
}

/* ---- 工具回调（协议栈任务上下文，只 post 队列不碰硬件） ---- */

static esp_mcp_value_t cb_get_state(const esp_mcp_property_list_t *props)
{
    (void)props;
    ESP_LOGI(TAG, "get_state -> %s", s_ret);
    return esp_mcp_value_create_string(state_json());
}

static esp_mcp_value_t cb_set_power(const esp_mcp_property_list_t *props)
{
    s_power = esp_mcp_property_list_get_property_bool(props, "power");
    light_apply();
    ESP_LOGI(TAG, "set_power(%d)", (int)s_power);
    return esp_mcp_value_create_string(state_json());
}

static esp_mcp_value_t cb_set_rgb(const esp_mcp_property_list_t *props)
{
    int r = esp_mcp_property_list_get_property_int(props, "r");
    int g = esp_mcp_property_list_get_property_int(props, "g");
    int b = esp_mcp_property_list_get_property_int(props, "b");
    r = (r < 0) ? 0 : (r > 255 ? 255 : r);
    g = (g < 0) ? 0 : (g > 255 ? 255 : g);
    b = (b < 0) ? 0 : (b > 255 ? 255 : b);
    s_r = r; s_g = g; s_b = b;
    s_power = true;             /* 调色即开灯（智能灯惯例，见头文件） */
    light_apply();
    ESP_LOGI(TAG, "set_rgb(%d,%d,%d)", r, g, b);
    return esp_mcp_value_create_string(state_json());
}

static esp_mcp_value_t cb_set_brightness(const esp_mcp_property_list_t *props)
{
    int v = esp_mcp_property_list_get_property_int(props, "brightness");
    v = (v < 0) ? 0 : (v > 100 ? 100 : v);
    s_brightness = v;
    if (v == 0) {
        s_power = false;        /* 亮度 0 = 关灯 */
    } else {
        s_power = true;
    }
    light_apply();
    ESP_LOGI(TAG, "set_brightness(%d)", v);
    return esp_mcp_value_create_string(state_json());
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

esp_err_t ai_light_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");

    /* get_state：无参数 */
    ESP_RETURN_ON_ERROR(add_tool(mcp, "self.light.get_state",
        "获取台灯当前状态（电源开关、RGB 颜色、亮度）",
        cb_get_state, NULL, 0), TAG, "get_state");

    /* set_power：power(bool) */
    esp_mcp_property_t *p_power = esp_mcp_property_create_with_bool("power", true);
    ESP_RETURN_ON_FALSE(p_power, ESP_FAIL, TAG, "prop power");
    ESP_RETURN_ON_ERROR(add_tool(mcp, "self.light.set_power",
        "打开或关闭台灯。power: true=开灯, false=关灯",
        cb_set_power, (esp_mcp_property_t *const[]){ p_power }, 1), TAG, "set_power");

    /* set_rgb：r/g/b(int 0~255) */
    esp_mcp_property_t *p_r = esp_mcp_property_create_with_range("r", 0, 255);
    esp_mcp_property_t *p_g = esp_mcp_property_create_with_range("g", 0, 255);
    esp_mcp_property_t *p_b = esp_mcp_property_create_with_range("b", 0, 255);
    ESP_RETURN_ON_FALSE(p_r && p_g && p_b, ESP_FAIL, TAG, "prop rgb");
    ESP_RETURN_ON_ERROR(add_tool(mcp, "self.light.set_rgb",
        "设置台灯颜色并开灯。r/g/b: 0~255，例如红色是 255,0,0",
        cb_set_rgb, (esp_mcp_property_t *const[]){ p_r, p_g, p_b }, 3), TAG, "set_rgb");

    /* set_brightness：brightness(int 0~100，默认 100) */
    esp_mcp_property_t *p_br = esp_mcp_property_create_with_int_and_range("brightness", 100, 0, 100);
    ESP_RETURN_ON_FALSE(p_br, ESP_FAIL, TAG, "prop brightness");
    ESP_RETURN_ON_ERROR(add_tool(mcp, "self.light.set_brightness",
        "调节台灯亮度（0~100）。0 等于关灯",
        cb_set_brightness, (esp_mcp_property_t *const[]){ p_br }, 1), TAG, "set_brightness");

    return ESP_OK;
}
