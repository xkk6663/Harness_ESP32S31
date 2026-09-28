/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "fs_service.h"
#include "middleware/mem_diag.h"

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

    /* ★ 把"实际挂到的物理分区地址/容量"打进日志（不能只打 total）：
     *   改过 partitions.csv 却只跑 app-flash（不含 0x8000 分区表）时，设备仍按**旧表**
     *   把 label "storage" 解析到旧地址 ⇒ mount 落到错误偏移 ⇒ format_if_mount_failed
     *   静默格式化 ⇒ 表现成"素材全丢"，极易误判为"littlefs 挂错地址/打包漏文件"。
     *   本行的 @地址/size 与 partitions.csv 对不上 = 分区表没烧，一眼定案。 */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, conf.partition_label);
    ESP_LOGI(TAG, "LittleFS mounted at %s, partition '%s' @0x%06x size=%uKB, total=%uKB used=%uKB",
             FS_MNT_PATH, conf.partition_label,
             (unsigned)(part ? part->address : 0),
             (unsigned)(part ? part->size / 1024 : 0),
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
    /* ★ P0-4：这是全工程最大的一笔单点分配（800*480*2 = 768,000 B），
     * 失败时原来只给一句 assert —— 现在先落三元组，能直接看出是 PSRAM 不够还是碎片。 */
    MEM_FAIL_FAST(file_buffer, TAG, "jpeg_calloc_align(JPEG frame buffer)", file_buffer_size);
    MEM_STAGE(TAG, "jpeg frame buffer allocated");
}
