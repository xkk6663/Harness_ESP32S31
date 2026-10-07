/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
/* ★ F35：任务栈外置 PSRAM（idf_additions.h 提供 WithCaps 系列；esp_heap_caps.h 提供 caps 宏） */
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"   /* esp_app_get_description()：fw_ver 真实化（组件 esp_app_format） */
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "link_manager.h"
#include "middleware/audio/udp_voice.h"

static const char *TAG = "LINK";
static int s_sock = -1;
static SemaphoreHandle_t s_mux;
static struct sockaddr_in s_peer;
static bool s_online;
static int64_t s_last_heartbeat_us;
static link_evt_cb_t s_observers[LINK_MAX_OBSERVERS];
static link_json_cb_t s_json_cbs[LINK_MAX_JSON_OBSERVERS];   /* 多槽 JSON 分发，见 .h 说明 */

static void link_task(void *arg);
static void notify_observers(link_evt_t evt, const char *ip);

void link_manager_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    assert(s_mux);
    /* ★ 栈外置 PSRAM（F35）：链路任务栈只被 CPU 使用，无 DMA 要求。
       失败即报错 —— 若这里返回 pdFAIL，说明 PSRAM 外置栈路径被配置关掉了，
       不是"内存不够"（此时 PSRAM 还富余十几 MB）。 */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(link_task, "link_manager", 8192, NULL, 4, NULL, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "link_manager task create failed (stack->PSRAM); check "
                      "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY");
    }
    assert(ok == pdPASS);
    ESP_LOGI(TAG, "init done, UDP %d", LINK_SIGNAL_PORT);
}

void link_manager_add_evt_cb(link_evt_cb_t cb)
{
    if (!cb || !s_mux) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (uint8_t i = 0; i < LINK_MAX_OBSERVERS; ++i) {
        if (s_observers[i] == cb) {
            xSemaphoreGive(s_mux);
            return;
        }
        if (!s_observers[i]) {
            s_observers[i] = cb;
            break;
        }
    }
    xSemaphoreGive(s_mux);
}

void link_manager_add_json_cb(link_json_cb_t cb)
{
    if (!cb) {
        return;
    }
    for (int i = 0; i < LINK_MAX_JSON_OBSERVERS; i++) {
        if (s_json_cbs[i] == cb) {
            return;     /* 幂等 */
        }
    }
    for (int i = 0; i < LINK_MAX_JSON_OBSERVERS; i++) {
        if (!s_json_cbs[i]) {
            s_json_cbs[i] = cb;   /* init 期注册一次，之后只读（与 observers 同风格） */
            return;
        }
    }
    ESP_LOGE(TAG, "json cb slots full (%d), reject", LINK_MAX_JSON_OBSERVERS);
}

bool link_manager_is_online(void)
{
    bool online = false;
    if (s_mux) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        online = s_online;
        xSemaphoreGive(s_mux);
    }
    return online;
}

bool link_manager_get_peer(struct sockaddr_in *peer)
{
    if (!peer || !s_mux) {
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool online = s_online;
    if (online) {
        *peer = s_peer;
    }
    xSemaphoreGive(s_mux);
    return online;
}

esp_err_t link_manager_send_json(const char *json)
{
    struct sockaddr_in peer;
    if (!json || s_sock < 0 || !link_manager_get_peer(&peer)) {
        return ESP_ERR_INVALID_STATE;
    }
    peer.sin_port = htons(LINK_SIGNAL_PORT);
    ssize_t sent = sendto(s_sock, json, strlen(json), 0, (struct sockaddr *)&peer, sizeof(peer));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

static void notify_observers(link_evt_t evt, const char *ip)
{
    link_evt_cb_t callbacks[LINK_MAX_OBSERVERS] = {0};
    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(callbacks, s_observers, sizeof(callbacks));
    xSemaphoreGive(s_mux);
    for (uint8_t i = 0; i < LINK_MAX_OBSERVERS; ++i) {
        if (callbacks[i]) {
            callbacks[i](evt, ip);
        }
    }
}

static void link_task(void *arg)
{
    (void)arg;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket create failed, errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(LINK_SIGNAL_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind %d failed, errno=%d", LINK_SIGNAL_PORT, errno);
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    char packet[384];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
        int ready = select(s_sock + 1, &rfds, NULL, NULL, &tv);
        if (ready > 0 && FD_ISSET(s_sock, &rfds)) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(s_sock, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&from, &from_len);
            if (n > 0) {
                packet[n] = '\0';
                if (strstr(packet, "\"type\"") && strstr(packet, "HEARTBEAT")) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
                    bool was_online;
                    xSemaphoreTake(s_mux, portMAX_DELAY);
                    was_online = s_online;
                    s_peer = from;
                    s_online = true;
                    s_last_heartbeat_us = esp_timer_get_time();
                    xSemaphoreGive(s_mux);
                    char ack[192];
                    /* ★ fw_ver 真实化：PC 的版本比对与 OTA"升级成功/已回滚"判定
                       全靠这个字段。原来硬编码 "phase2" ⇒ 升完级 PC 看不出变化，
                       会误判成"回滚了"。取编译期烧进镜像的描述符（esp_app_format）。 */
                    const esp_app_desc_t *desc = esp_app_get_description();
                    snprintf(ack, sizeof(ack),
                             "{\"type\":\"HEARTBEAT_ACK\",\"ver\":1,\"proto_ver\":1,"
                             "\"sample_rate\":16000,\"fw_ver\":\"%s\"}",
                             desc ? desc->version : "unknown");
                    sendto(s_sock, ack, strlen(ack), 0, (struct sockaddr *)&from, from_len);
                    if (!was_online) {
                        ESP_LOGI(TAG, "ONLINE peer=%s", ip);
                        notify_observers(LINK_EVT_ONLINE, ip);
                    }
                }
                /* 信令分发：SESSION_START/STOP -> udp_voice */
                if (strstr(packet, "\"type\"") && strstr(packet, "SESSION_START")) {
                    ESP_LOGI(TAG, "sig SESSION_START -> voice tx+rx on");
                    udp_voice_post_cmd(VOICE_CMD_TX_ON);
                    udp_voice_post_cmd(VOICE_CMD_RX_ON);
                } else if (strstr(packet, "\"type\"") && strstr(packet, "SESSION_STOP")) {
                    ESP_LOGI(TAG, "sig SESSION_STOP -> voice tx+rx off");
                    udp_voice_post_cmd(VOICE_CMD_TX_OFF);
                    udp_voice_post_cmd(VOICE_CMD_RX_OFF);
                }
                /* ★ 通用 JSON 分发（末端）：OTA_* / WP_* 等上层订阅的消息。
                   放在 HEARTBEAT/SESSION_* 这些"本文件认识的"分支之后，
                   不认识的和认识的都能到达订阅者。订阅方自行 strstr 判 type，
                   多槽各过滤各的，互不干扰。 */
                for (int i = 0; i < LINK_MAX_JSON_OBSERVERS; i++) {
                    if (s_json_cbs[i]) {
                        s_json_cbs[i](packet);
                    }
                }
            }
        }
        bool offline = false;
        char ip[INET_ADDRSTRLEN] = "";
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_online && esp_timer_get_time() - s_last_heartbeat_us > 5000000) {
            inet_ntoa_r(s_peer.sin_addr, ip, sizeof(ip));
            s_online = false;
            offline = true;
        }
        xSemaphoreGive(s_mux);
        if (offline) {
            ESP_LOGW(TAG, "OFFLINE peer=%s (heartbeat timeout)", ip);
            notify_observers(LINK_EVT_OFFLINE, ip);
        }
    }
}
