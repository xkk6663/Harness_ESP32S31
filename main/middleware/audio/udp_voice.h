/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file udp_voice.h @brief Low-latency UDP audio transport, independent of LVGL. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDP_VOICE_TX_PORT 50000
#define UDP_VOICE_RX_PORT 50001

typedef enum {
    VOICE_CMD_TX_ON = 0,   /* uplink: device mic -> PC */
    VOICE_CMD_TX_OFF,
    VOICE_CMD_RX_ON,       /* downlink: PC -> device speaker */
    VOICE_CMD_RX_OFF,
} voice_cmd_id_t;

typedef enum {
    VOICE_EVT_TX_ON = 0,
    VOICE_EVT_TX_OFF,
    VOICE_EVT_RX_ON,
    VOICE_EVT_RX_OFF,
    VOICE_EVT_NO_PEER,
} voice_evt_t;

typedef void (*voice_evt_cb_t)(voice_evt_t evt);

void udp_voice_init(void);
void udp_voice_post_cmd(voice_cmd_id_t cmd);
bool udp_voice_tx_active(void);
bool udp_voice_rx_active(void);
void udp_voice_set_evt_cb(voice_evt_cb_t cb);

#ifdef __cplusplus
}
#endif
