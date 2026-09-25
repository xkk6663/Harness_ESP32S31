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
    /* 运行期启动失败（栈/堆不足、socket bind 失败…）。
     * ★ 与 NO_PEER 分开是刻意的：把"内存不足"报成"对方不在线"是**错的信号**，
     *   会把人引到错误方向（见工程规范：错的日志比没有日志更坏）。 */
    VOICE_EVT_ERROR,
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
