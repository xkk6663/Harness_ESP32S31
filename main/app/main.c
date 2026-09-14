/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#include "ui/ui_manager/page_manager.h"
#include "ui/page_splash.h"
#include "ui/page_main.h"
#include "ui/overlay_manager.h"
#include "middleware/audio_service.h"
#include "middleware/fs_service.h"
#include "middleware/led_service.h"
#include "middleware/button_service.h"

static const char *TAG = "main";

/* Swipe-up on splash recognized -> push the main page. */
static void on_splash_enter(void)
{
    PageManager_Load(&Page_Main);
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

    /* UI framework */
    PageManager_Init();
    page_splash_set_enter_cb(on_splash_enter);
    PageManager_Load(&Page_Splash);     /* stack root */

    ESP_LOGI(TAG, "boot done, waiting swipe up");
}
