/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
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
    /* JPEG 解码输出缓冲：按 LCD 全屏大小分配（开屏壁纸 / 图片查看共用）。
       尺寸 = 800 * 480 * sizeof(lv_color_t)。

       ⚠ 注意 sizeof(lv_color_t) 取决于 CONFIG_LV_COLOR_DEPTH，不是固定 2：
         本工程 sdkconfig 里 CONFIG_LV_COLOR_DEPTH=24 → sizeof = 3 字节，
         所以实际是 800*480*3 = 1152000 字节 ≈ 1.10MB，**不是 750KB**。
         （LV_COLOR_DEPTH_16 时才是 768000 ≈ 750KB。早期注释按 16 位写，
           与灯光/UI 改 24 位后已不符 —— 体积差 1.5 倍，核算内存时别照抄。）

       ★ 必须显式指定 PSRAM，不能用 jpeg_calloc_align()：
       jpeg_calloc_align() 内部走的是 heap_caps_calloc_prefer()，即
       "**优先内部 RAM**、内部不够才退到 PSRAM"。本工程开屏壁纸在
       bsp_display_start() 之后立刻解码，此时内部 RAM 还有 ~750KB 连续空间，
       于是这 750KB 被**吃在内部 RAM 里**——直接导致后续开蓝牙时
       internal free 只剩 25KB 而 ASSERT 崩溃（F28）。

       显式给 MALLOC_CAP_SPIRAM 后：优先 PSRAM，失败才允许退内部 RAM，
       把内部 RAM 留给 bluedroid / WiFi / DMA 这些**必须在内部**的用途。
       16 字节对齐要求：heap_caps_aligned_calloc 原生支持。 */
    file_buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(lv_color_t);

    file_buffer = heap_caps_aligned_calloc(16, 1, file_buffer_size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file_buffer) {
        /* PSRAM 也拿不到时的兜底：退回默认分配（可能落内部 RAM）。
           此时至少壁纸能显示，但请留意后续开蓝牙的内存门槛告警。 */
        ESP_LOGW(TAG, "PSRAM alloc %u bytes failed, fallback to prefer-internal",
                 (unsigned)file_buffer_size);
        file_buffer = jpeg_calloc_align(file_buffer_size, 16);
    }
    assert(file_buffer);
    ESP_LOGI(TAG, "jpeg out buffer %u bytes @ %p",
             (unsigned)file_buffer_size, file_buffer);
}
