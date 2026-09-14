/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_service.h"
#include "esp_log.h"

static const char *TAG = "LED_SVC";

static led_indicator_handle_t s_leds[BSP_LED_NUM];

void led_service_init(void)
{
    ESP_ERROR_CHECK(bsp_led_indicator_create(s_leds, NULL, BSP_LED_NUM));
    led_indicator_start(s_leds[0], BSP_LED_BREATHE_SLOW);
    ESP_LOGI(TAG, "RGB LED init: GPIO37, breathe slow");
}

void led_service_set_mode(bsp_led_effect_t mode)
{
    if (!s_leds[0]) {
        ESP_LOGW(TAG, "led_service not initialized");
        return;
    }
    led_indicator_stop(s_leds[0], BSP_LED_BREATHE_SLOW);
    led_indicator_start(s_leds[0], mode);
    ESP_LOGI(TAG, "LED mode -> %d", mode);
}

void led_service_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_leds[0]) {
        ESP_LOGW(TAG, "led_service not initialized");
        return;
    }
    /* stop any breathing effect, set custom color, hold steady on */
    led_indicator_stop(s_leds[0], BSP_LED_BREATHE_SLOW);
    led_indicator_stop(s_leds[0], BSP_LED_OFF);
    led_indicator_set_rgb(s_leds[0], SET_IRGB(0, r, g, b));
    led_indicator_start(s_leds[0], BSP_LED_ON);
    ESP_LOGI(TAG, "LED rgb -> #%02X%02X%02X", r, g, b);
}

void led_service_set_on(bool on)
{
    if (!s_leds[0]) {
        ESP_LOGW(TAG, "led_service not initialized");
        return;
    }
    led_indicator_stop(s_leds[0], BSP_LED_BREATHE_SLOW);
    led_indicator_start(s_leds[0], on ? BSP_LED_ON : BSP_LED_OFF);
    ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
}
