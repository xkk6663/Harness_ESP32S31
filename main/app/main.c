/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "ui/ui_disp.h"
#include "ui/ui_splash.h"
#include "middleware/audio_service.h"
#include "middleware/fs_service.h"

static const char *TAG = "main";

/* 上拉手势识别成功后：进入主界面（此时已在 LVGL 锁上下文） */
static void on_splash_enter(void)
{
    app_disp_lvgl_show_main();
}

void app_main(void)
{
    /* Mount SPIFFS (contains xwkkk.jpg + recording.wav) */
    bsp_spiffs_mount();

    /* I2C: touch + audio codec */
    bsp_i2c_init();

    /* Display + LVGL */
    bsp_display_start();
    bsp_display_brightness_set(50);

    /* Allocate JPEG decode buffer (for splash wallpaper) */
    fs_service_init();

    /* Audio service:常驻 audio_task + command queue */
    audio_service_init();

    /* Splash screen: show xwkkk.jpg, wait for swipe-up gesture */
    bsp_display_lock(0);
    ui_splash_show(on_splash_enter);
    bsp_display_unlock();

    ESP_LOGI(TAG, "boot done, waiting swipe up");
}
