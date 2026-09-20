/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdcard_service.c
 * @brief SD 卡挂载服务（SDIO 4-bit，挂载到 /sdcard）
 */

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "bsp/esp-bsp.h"

#include "sdcard_service.h"

static const char *TAG = "SDCARD";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sdcard_service_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD card to /sdcard ...");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s (no card?)", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted OK");
    return ESP_OK;
}

bool sdcard_service_is_mounted(void)
{
    return s_mounted;
}
