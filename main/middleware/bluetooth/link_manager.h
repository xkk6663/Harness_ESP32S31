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
#define LINK_MAX_JSON_OBSERVERS 4

typedef enum {
    LINK_EVT_ONLINE = 0,
    LINK_EVT_OFFLINE,
} link_evt_t;

typedef void (*link_evt_cb_t)(link_evt_t evt, const char *peer_ip);

/* 通用 JSON 分发槽（**多槽**，供上层服务订阅含 "type" 的包）。
 * ★ 为什么需要它：OTA（OTA_OFFER/OTA_ABORT）与壁纸上传（WP_OFFER）等控制消息
 *   都走同一条 UDP 50003，但消费者分布在 middleware/{network,media}/，
 *   而本文件在 middleware/bluetooth/。若在此直接 #include 具体服务头就构成
 *   **bluetooth → 其他域反向依赖**（规范 01 分层禁令）。
 *   改为上层 init() 里注册回调，本文件只做"有 type 就转发"，
 *   对具体 type 一无所知 —— 消费方各自按前缀过滤（如 OTA_* / WP_*），
 *   不认识的原样忽略 ⇒ 多槽互不干扰。
 * ★ 回调在 link_task 上下文执行 ⇒ 实现方只允许解析/投队列，**禁止阻塞 IO**（F44）。
 * ★ V2.1 变更：单槽 → 多槽（原单槽已被 ota_service 占用，壁纸服务无槽可注册）。 */
typedef void (*link_json_cb_t)(const char *json);
void link_manager_add_json_cb(link_json_cb_t cb);

void link_manager_init(void);
bool link_manager_is_online(void);
bool link_manager_get_peer(struct sockaddr_in *peer);
void link_manager_add_evt_cb(link_evt_cb_t cb);
esp_err_t link_manager_send_json(const char *json);

#ifdef __cplusplus
}
#endif
