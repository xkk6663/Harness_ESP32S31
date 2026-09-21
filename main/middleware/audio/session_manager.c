/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "session_manager.h"

static SemaphoreHandle_t s_mux;
static session_mode_t s_mode = SESSION_IDLE;

void session_manager_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    assert(s_mux);
}

esp_err_t session_manager_begin(session_mode_t mode)
{
    if (mode == SESSION_IDLE || !s_mux) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t err = (s_mode == SESSION_IDLE || s_mode == mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        s_mode = mode;
    }
    xSemaphoreGive(s_mux);
    return err;
}

void session_manager_end(session_mode_t mode)
{
    if (!s_mux) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_mode == mode) {
        s_mode = SESSION_IDLE;
    }
    xSemaphoreGive(s_mux);
}

session_mode_t session_manager_get_mode(void)
{
    if (!s_mux) {
        return SESSION_IDLE;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    session_mode_t mode = s_mode;
    xSemaphoreGive(s_mux);
    return mode;
}
