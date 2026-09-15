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
#include "esp_err.h"
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 统一挂载点：SPIFFS 已迁移为 LittleFS，业务代码一律通过此宏拼接路径 */
#define FS_MNT_PATH     "/littlefs"

typedef enum {
    APP_FILE_TYPE_UNKNOWN,
    APP_FILE_TYPE_WAV,
} app_file_type_t;

/* JPEG 解码输出缓冲（按 LCD 全屏大小分配） */
extern uint8_t *file_buffer;
extern size_t file_buffer_size;

app_file_type_t get_file_type(const char *filepath);

/**
 * @brief 挂载 LittleFS 文件系统到 FS_MNT_PATH
 * @note  在 app_main() 最早期调用，先于任何文件读写；挂载失败自动格式化
 */
esp_err_t fs_service_mount(void);

/**
 * @brief 分配 JPEG 解码帧缓冲
 */
void fs_service_init(void);

#ifdef __cplusplus
}
#endif
