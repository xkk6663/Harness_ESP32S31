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

/* 通用 JSON 分发槽（单槽，供上层服务订阅含 "type" 的包）。
 * ★ 为什么需要它：OTA 控制消息（OTA_OFFER/OTA_ABORT）要走同一条 UDP 50003，
 *   但 ota_service 在 middleware/network/，而本文件在 middleware/bluetooth/。
 *   若在此直接 #include "ota_service.h" 就构成 **bluetooth → network 反向依赖**
 *   （规范 01 分层禁令，本工程已有两处倒挂待清，不能再加）。
 *   改为上层在 ota_service_init() 里注册回调，本文件只做"有 type 就转发"，
 *   对具体 type 一无所知 —— 与 pcnow 用 observer 槽的风格一致。
 * ★ 回调在 link_task 上下文执行 ⇒ 实现方只允许解析/投队列，**禁止阻塞 IO**（F44）。 */
typedef void (*link_json_cb_t)(const char *json);
void link_manager_set_json_cb(link_json_cb_t cb);

void link_manager_init(void);
bool link_manager_is_online(void);
bool link_manager_get_peer(struct sockaddr_in *peer);
void link_manager_add_evt_cb(link_evt_cb_t cb);
esp_err_t link_manager_send_json(const char *json);

#ifdef __cplusplus
}
#endif
