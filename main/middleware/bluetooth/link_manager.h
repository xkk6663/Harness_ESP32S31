/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file link_manager.h @brief UDP 50003 heartbeat and peer state service. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lwip/sockets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_SIGNAL_PORT 50003
#define LINK_MAX_OBSERVERS 4

typedef enum {
    LINK_EVT_ONLINE = 0,
    LINK_EVT_OFFLINE,
} link_evt_t;

typedef void (*link_evt_cb_t)(link_evt_t evt, const char *peer_ip);

void link_manager_init(void);
bool link_manager_is_online(void);
bool link_manager_get_peer(struct sockaddr_in *peer);
void link_manager_add_evt_cb(link_evt_cb_t cb);
esp_err_t link_manager_send_json(const char *json);

#ifdef __cplusplus
}
#endif
