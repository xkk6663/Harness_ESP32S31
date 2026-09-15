/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stdio.h>
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
#include "middleware/wifi_manager.h"

static const char *TAG = "main";

/* 本次上电周期内，开机提示音是否已播过（只在第一次上滑进主界面时播一次） */
static bool s_boot_tone_played = false;

/* Swipe-up on splash recognized -> push the main page. */
static void on_splash_enter(void)
{
    PageManager_Load(&Page_Main);

    /* 第一次上滑进主界面：自动播放一次开机提示音 */
    if (!s_boot_tone_played) {
        s_boot_tone_played = true;
        audio_cmd_t cmd = {
            .id = AUDIO_CMD_PLAY_FILE,
            .value = 0,
        };
        snprintf(cmd.path, sizeof(cmd.path), "%s/boot_tone.wav", FS_MNT_PATH);
        audio_service_post_cmd(&cmd);
        ESP_LOGI(TAG, "boot tone requested: %s", cmd.path);
    }
}

/* Wi-Fi status -> main-interface RGB LED (red / blue breathe / green) */
static void on_wifi_evt(wifi_evt_t evt, const char *info)
{
    switch (evt) {
    case WIFI_EVT_DISCONNECTED:
        led_service_set_status(LED_STATUS_DISCONNECTED);
        break;
    case WIFI_EVT_CONNECTING:
        led_service_set_status(LED_STATUS_CONNECTING);
        break;
    case WIFI_EVT_GOT_IP:
        led_service_set_status(LED_STATUS_CONNECTED);
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(fs_service_mount());
    bsp_i2c_init();
    bsp_display_start();
    //bsp_display_brightness_set(50);
    fs_service_init();
    audio_service_init();

    led_service_init();
    button_service_init();
    wifi_manager_init();
    wifi_manager_set_sys_cb(on_wifi_evt);

    /* UI framework */
    PageManager_Init();
    page_splash_set_enter_cb(on_splash_enter);
    PageManager_Load(&Page_Splash);     /* stack root */

    ESP_LOGI(TAG, "boot done, waiting swipe up");
}
