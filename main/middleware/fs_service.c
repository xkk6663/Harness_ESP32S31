/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "fs_service.h"

uint8_t *file_buffer = NULL;
size_t file_buffer_size = 0;

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
