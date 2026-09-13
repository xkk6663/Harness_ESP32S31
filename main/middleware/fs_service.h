/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fs_service.h
 * @brief 中间层：文件类型识别、JPEG 解码帧缓冲
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_MNT_PATH     BSP_SPIFFS_MOUNT_POINT

typedef enum {
    APP_FILE_TYPE_UNKNOWN,
    APP_FILE_TYPE_WAV,
} app_file_type_t;

/* JPEG 解码输出缓冲（按 LCD 全屏大小分配） */
extern uint8_t *file_buffer;
extern size_t file_buffer_size;

app_file_type_t get_file_type(const char *filepath);

/**
 * @brief 分配 JPEG 解码帧缓冲
 */
void fs_service_init(void);

#ifdef __cplusplus
}
#endif
