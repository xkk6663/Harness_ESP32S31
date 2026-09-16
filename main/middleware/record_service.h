/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file record_service.h @brief TCP recording service with STREAM/LOCAL fallback. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECORD_CMD_START = 0,
    RECORD_CMD_STOP,
    RECORD_CMD_RETRY,
    RECORD_CMD_DELETE_PENDING,
} record_cmd_id_t;

typedef enum {
    RECORD_MODE_NONE = 0,
    RECORD_MODE_STREAM,
    RECORD_MODE_LOCAL,
} record_mode_t;

typedef enum {
    RECORD_EVT_STARTED = 0,
    RECORD_EVT_PROGRESS,
    RECORD_EVT_STOPPED,
    RECORD_EVT_UPLOAD_DONE,
    RECORD_EVT_ERROR,
} record_evt_t;

typedef struct {
    record_evt_t event;
    record_mode_t mode;
    uint32_t bytes;
    uint32_t limit_bytes;
} record_event_t;

typedef void (*record_evt_cb_t)(const record_event_t *event);

void record_service_init(void);
void record_service_post_cmd(record_cmd_id_t cmd);
bool record_service_is_active(void);
record_mode_t record_service_get_mode(void);
void record_service_set_evt_cb(record_evt_cb_t cb);

#ifdef __cplusplus
}
#endif
