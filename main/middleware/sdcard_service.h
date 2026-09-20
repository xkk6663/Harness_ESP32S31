/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdcard_service.h
 * @brief SD 卡挂载服务
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载点路径 */
#define SDCARD_MNT_PATH "/sdcard"

/**
 * @brief 挂载 SD 卡到 /sdcard
 * @return ESP_OK / 错误码
 */
esp_err_t sdcard_service_mount(void);

/** SD 卡是否已挂载 */
bool sdcard_service_is_mounted(void);

#ifdef __cplusplus
}
#endif
