/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "button_service.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "iot_button.h"
#include "button_adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "BTN_SVC";

static button_handle_t s_btns[BTN_ID_NUM];
static btn_evt_cb_t    s_evt_cb;

/* Volume state (protected by s_vol_mutex) */
static int             s_volume = 70;
static SemaphoreHandle_t s_vol_mutex;
static QueueHandle_t  s_vol_queue;

#define VOL_STEP   10
#define VOL_MIN    0
#define VOL_MAX    90

/* Actual measured mV on ESP32-S31-Korvo-1 V1.1 (IO42, 10K pull-up):
 *   set  (SW): ~ 260 mV
 *   mode (SW): ~ 759 mV
 *   vol- (SW): ~1237 mV
 *   vol+ (SW): ~1647 mV
 * ±100 mV window for tolerance.
 */
static const uint16_t btn_min[BTN_ID_NUM] = { 180, 660, 1140, 1550 };
static const uint16_t btn_max[BTN_ID_NUM] = { 360, 860, 1340, 1750 };

static const char *button_name(btn_id_t id)
{
    switch (id) {
    case BTN_ID_SET:     return "set";
    case BTN_ID_MODE:    return "mode";
    case BTN_ID_VOLDOWN: return "vol-";
    case BTN_ID_VOLUP:   return "vol+";
    default:             return "UNKNOWN";
    }
}

static const char *button_event_name(btn_evt_t evt)
{
    switch (evt) {
    case BTN_EVT_PRESS_DOWN:   return "PRESS_DOWN";
    case BTN_EVT_PRESS_UP:     return "PRESS_UP";
    case BTN_EVT_SINGLE_CLICK: return "SINGLE_CLICK";
    case BTN_EVT_DOUBLE_CLICK: return "DOUBLE_CLICK";
    case BTN_EVT_PRESS_END:    return "PRESS_END";
    default:                   return "UNKNOWN";
    }
}

static button_event_t to_bsp_evt[5] = {
    [BTN_EVT_PRESS_DOWN]   = BUTTON_PRESS_DOWN,
    [BTN_EVT_PRESS_UP]     = BUTTON_PRESS_UP,
    [BTN_EVT_SINGLE_CLICK] = BUTTON_SINGLE_CLICK,
    [BTN_EVT_DOUBLE_CLICK] = BUTTON_DOUBLE_CLICK,
    [BTN_EVT_PRESS_END]    = BUTTON_PRESS_END,
};

static int get_volume_locked(void)
{
    int v = s_volume;
    xSemaphoreTake(s_vol_mutex, portMAX_DELAY);
    v = s_volume;
    xSemaphoreGive(s_vol_mutex);
    return v;
}

static void publish_volume(void)
{
    btn_vol_msg_t msg = { .volume = get_volume_locked() };
    xQueueSend(s_vol_queue, &msg, 0);
}

static void adjust_volume(int delta)
{
    xSemaphoreTake(s_vol_mutex, portMAX_DELAY);
    s_volume += delta;
    if (s_volume < VOL_MIN) s_volume = VOL_MIN;
    if (s_volume > VOL_MAX) s_volume = VOL_MAX;
    ESP_LOGI(TAG, "volume -> %d", s_volume);
    xSemaphoreGive(s_vol_mutex);
    publish_volume();
}

static void button_event_cb(void *button_handle, void *user_data)
{
    btn_id_t button = (btn_id_t)(intptr_t)user_data;
    button_event_t event = iot_button_get_event(button_handle);

    btn_evt_t evt = BTN_EVT_PRESS_DOWN;
    for (int i = 0; i < 5; i++) {
        if (to_bsp_evt[i] == event) {
            evt = (btn_evt_t)i;
            break;
        }
    }

    char status[64];
    snprintf(status, sizeof(status), "%s: %s", button_name(button), button_event_name(evt));
    ESP_LOGI(TAG, "%s", status);

    /* Volume keys: single click ±10, double click ±20 */
    if (evt == BTN_EVT_SINGLE_CLICK) {
        if (button == BTN_ID_VOLDOWN) {
            adjust_volume(-VOL_STEP);
        } else if (button == BTN_ID_VOLUP) {
            adjust_volume(VOL_STEP);
        }
    } else if (evt == BTN_EVT_DOUBLE_CLICK) {
        if (button == BTN_ID_VOLDOWN) {
            adjust_volume(-VOL_STEP * 2);
        } else if (button == BTN_ID_VOLUP) {
            adjust_volume(VOL_STEP * 2);
        }
    }

    if (s_evt_cb) {
        s_evt_cb(button, evt);
    }
}

void button_service_init(void)
{
    s_vol_mutex = xSemaphoreCreateMutex();
    s_vol_queue = xQueueCreate(4, sizeof(btn_vol_msg_t));

    ESP_ERROR_CHECK(bsp_adc_initialize());
    adc_oneshot_unit_handle_t adc = bsp_adc_get_handle();

    const button_config_t btn_cfg = {0};
    for (int i = 0; i < BTN_ID_NUM; i++) {
        button_adc_config_t adc_cfg = {
            .adc_handle = &adc,
            .unit_id = ADC_UNIT_1,
            .adc_channel = ADC_CHANNEL_0,
            .button_index = (uint8_t)i,
            .min = btn_min[i],
            .max = btn_max[i],
        };
        ESP_ERROR_CHECK(iot_button_new_adc_device(&btn_cfg, &adc_cfg, &s_btns[i]));

        iot_button_register_cb(s_btns[i], BUTTON_PRESS_DOWN,    NULL, button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(s_btns[i], BUTTON_PRESS_UP,      NULL, button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(s_btns[i], BUTTON_SINGLE_CLICK,  NULL, button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(s_btns[i], BUTTON_DOUBLE_CLICK,  NULL, button_event_cb, (void *)(intptr_t)i);
        iot_button_register_cb(s_btns[i], BUTTON_PRESS_END,     NULL, button_event_cb, (void *)(intptr_t)i);
    }
    ESP_LOGI(TAG, "Registered %d ADC buttons, vol=%d", BTN_ID_NUM, s_volume);
}

void button_service_set_evt_cb(btn_evt_cb_t cb)
{
    s_evt_cb = cb;
}

QueueHandle_t button_service_get_vol_queue(void)
{
    return s_vol_queue;
}

int button_service_get_volume(void)
{
    return get_volume_locked();
}

void button_service_set_volume(int vol)
{
    xSemaphoreTake(s_vol_mutex, portMAX_DELAY);
    if (vol < VOL_MIN) vol = VOL_MIN;
    if (vol > VOL_MAX) vol = VOL_MAX;
    s_volume = vol;
    xSemaphoreGive(s_vol_mutex);
}
