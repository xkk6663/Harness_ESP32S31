/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "LED_SVC";

/* ---- command (decoupled from callers) ---- */
typedef enum {
    LED_CMD_STATUS,
    LED_CMD_RGB,
    LED_CMD_ON,
    LED_CMD_MANUAL,
} led_cmd_id_t;

typedef struct {
    led_cmd_id_t id;
    uint8_t a;
    uint8_t b;
    uint8_t c;
} led_cmd_t;

static QueueHandle_t s_q;

/* ---- state owned by the LED task ---- */
static led_indicator_handle_t s_leds[BSP_LED_NUM];
static bool s_manual;
static led_status_t s_status = LED_STATUS_DISCONNECTED;
static uint8_t s_cur_r, s_cur_g, s_cur_b;
static bool s_cur_breathe;
static bool s_led_on = true;

/* Drive the strip with the current color: breathing for status, steady-on for manual. */
static void apply_color(void)
{
    if (!s_leds[0]) {
        return;
    }
    led_indicator_stop(s_leds[0], BSP_LED_BREATHE_SLOW);
    led_indicator_stop(s_leds[0], BSP_LED_OFF);
    if (!s_led_on) {
        led_indicator_start(s_leds[0], BSP_LED_OFF);
        return;
    }
    led_indicator_set_rgb(s_leds[0], SET_IRGB(0, s_cur_r, s_cur_g, s_cur_b));
    led_indicator_start(s_leds[0], s_cur_breathe ? BSP_LED_BREATHE_SLOW : BSP_LED_ON);
}

static void set_color(uint8_t r, uint8_t g, uint8_t b, bool breathe)
{
    s_cur_r = r;
    s_cur_g = g;
    s_cur_b = b;
    s_cur_breathe = breathe;
    apply_color();
}

static void show_status(void)
{
    if (s_status == LED_STATUS_DISCONNECTED) {
        set_color(255, 0, 0, true);
    } else if (s_status == LED_STATUS_CONNECTING) {
        set_color(0, 80, 255, true);
    } else {
        set_color(0, 255, 0, true);
    }
}

static void led_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LED task ready");

    led_cmd_t cmd;
    while (xQueueReceive(s_q, &cmd, portMAX_DELAY) == pdPASS) {
        switch (cmd.id) {
        case LED_CMD_STATUS:
            if (s_manual) {
                break;
            }
            s_status = (led_status_t)cmd.a;
            show_status();
            break;
        case LED_CMD_RGB:
            set_color(cmd.a, cmd.b, cmd.c, false);
            break;
        case LED_CMD_ON:
            s_led_on = cmd.a;
            apply_color();
            break;
        case LED_CMD_MANUAL:
            s_manual = cmd.a;
            if (!s_manual) {
                show_status();
            }
            break;
        default:
            break;
        }
    }
}

static void post(led_cmd_id_t id, uint8_t a, uint8_t b, uint8_t c)
{
    led_cmd_t cmd = { .id = id, .a = a, .b = b, .c = c };
    if (s_q) {
        xQueueSend(s_q, &cmd, 0);
    }
}

void led_service_init(void)
{
    /* create in app_main context (proven to work); task only serializes commands */
    ESP_ERROR_CHECK(bsp_led_indicator_create(s_leds, NULL, BSP_LED_NUM));
    s_q = xQueueCreate(8, sizeof(led_cmd_t));
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);
    /* boot: red breathing = not connected */
    set_color(255, 0, 0, true);
    ESP_LOGI(TAG, "RGB LED init: GPIO37, red breathe=disconnected");
}

void led_service_set_status(led_status_t status)   { post(LED_CMD_STATUS, status, 0, 0); }
void led_service_set_rgb(uint8_t r, uint8_t g, uint8_t b) { post(LED_CMD_RGB, r, g, b); }
void led_service_set_on(bool on)                  { post(LED_CMD_ON, on ? 1 : 0, 0, 0); }
void led_service_set_manual(bool manual)          { post(LED_CMD_MANUAL, manual ? 1 : 0, 0, 0); }
