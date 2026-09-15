/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "fs_service.h"

static const char *TAG = "FS_SVC";

uint8_t *file_buffer = NULL;
size_t file_buffer_size = 0;

/* ===== LittleFS 挂载（替代原 bsp_spiffs_mount） ===== */
esp_err_t fs_service_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_MNT_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s, partition '%s', total=%uKB used=%uKB",
             FS_MNT_PATH, conf.partition_label,
             (unsigned)(total / 1024), (unsigned)(used / 1024));
    return ESP_OK;
}

app_file_type_t get_file_type(const char *filepath)
{
    assert(filepath != NULL);
    for (int i = (strlen(filepath) - 1); i >= 0; i--) {
        if (filepath[i] == '.') {
            if (strcmp(&filepath[i + 1], "WAV") == 0 || strcmp(&filepath[i + 1], "wav") == 0) {
                return APP_FILE_TYPE_WAV;
            }
            break;
        }
    }
    return APP_FILE_TYPE_UNKNOWN;
}

void fs_service_init(void)
{
    /* JPEG 解码输出缓冲：按 LCD 全屏大小分配（开屏壁纸 / 图片查看共用） */
    file_buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(lv_color_t);
    file_buffer = jpeg_calloc_align(file_buffer_size, 16);
    assert(file_buffer);
}
