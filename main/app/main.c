/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "ui/ui_disp.h"
#include "ui/ui_splash.h"
#include "middleware/audio_service.h"
#include "middleware/fs_service.h"
#include "middleware/led_service.h"
#include "middleware/button_service.h"

static const char *TAG = "main";

/* Swipe-up gesture recognized -> enter main UI */
static void on_splash_enter(void)
{
    app_disp_lvgl_show_main();
}

void app_main(void)
{
    bsp_spiffs_mount();
    bsp_i2c_init();
    bsp_display_start();
    bsp_display_brightness_set(50);
    fs_service_init();
    audio_service_init();

    led_service_init();
    button_service_init();

    bsp_display_lock(0);
    ui_splash_show(on_splash_enter);
    bsp_display_unlock();

    ESP_LOGI(TAG, "boot done, waiting swipe up");
}
