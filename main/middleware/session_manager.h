/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file session_manager.h @brief Audio-mode mutual exclusion gate. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SESSION_IDLE = 0,
    SESSION_UDP_CALL,
    SESSION_TCP_RECORD,
} session_mode_t;

void session_manager_init(void);
esp_err_t session_manager_begin(session_mode_t mode);
void session_manager_end(session_mode_t mode);
session_mode_t session_manager_get_mode(void);

#ifdef __cplusplus
}
#endif
