/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "middleware/audio_service.h"
#include "middleware/link_manager.h"
#include "middleware/session_manager.h"
#include "udp_voice.h"

static const char *TAG = "UDP_VOICE";
#define FRAME_MAGIC0 0xA5
#define FRAME_MAGIC1 0x5A
#define FRAME_VERSION 1
#define FRAME_HEADER_BYTES 12
#define REORDER_SLOTS 16

typedef struct __attribute__((packed)) {
    uint8_t magic[2];
    uint8_t version;
    uint8_t type;
    uint16_t seq;
    uint32_t timestamp_ms;
    uint16_t len;
} voice_frame_hdr_t;

typedef struct {
    bool used;
    uint16_t seq;
    uint8_t pcm[AUDIO_FRAME_BYTES];
} frame_slot_t;

static QueueHandle_t s_cmd_q;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;
static int s_rx_sock = -1;
static volatile bool s_active;
static voice_evt_cb_t s_evt_cb;

static void control_task(void *arg);
static void tx_task(void *arg);
static void rx_task(void *arg);
static void notify_evt(voice_evt_t evt);
static bool seq_before(uint16_t a, uint16_t b);

void udp_voice_init(void)
{
    s_cmd_q = xQueueCreate(8, sizeof(voice_cmd_id_t));
    assert(s_cmd_q);
    BaseType_t ok = xTaskCreatePinnedToCore(control_task, "udp_voice_ctl", 8192, NULL, 5, NULL, 0);
    assert(ok == pdPASS);
    ESP_LOGI(TAG, "init done, tx=%d rx=%d", UDP_VOICE_TX_PORT, UDP_VOICE_RX_PORT);
}

void udp_voice_post_cmd(voice_cmd_id_t cmd)
{
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

bool udp_voice_is_active(void)
{
    return s_active;
}

void udp_voice_set_evt_cb(voice_evt_cb_t cb)
{
    s_evt_cb = cb;
}

static void notify_evt(voice_evt_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt);
    }
}

static void control_task(void *arg)
{
    (void)arg;
    voice_cmd_id_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (cmd == VOICE_CMD_START) {
            if (s_active) {
                continue;
            }
            if (!link_manager_is_online()) {
                ESP_LOGW(TAG, "start rejected: no PC peer");
                notify_evt(VOICE_EVT_NO_PEER);
                continue;
            }
            if (session_manager_begin(SESSION_UDP_CALL) != ESP_OK || audio_stream_start() != ESP_OK) {
                session_manager_end(SESSION_UDP_CALL);
                ESP_LOGW(TAG, "start rejected: audio mode busy");
                notify_evt(VOICE_EVT_NO_PEER);
                continue;
            }
            s_rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(UDP_VOICE_RX_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
            if (s_rx_sock < 0 || bind(s_rx_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                ESP_LOGE(TAG, "rx socket bind failed, errno=%d", errno);
                if (s_rx_sock >= 0) {
                    close(s_rx_sock);
                    s_rx_sock = -1;
                }
                audio_stream_stop();
                session_manager_end(SESSION_UDP_CALL);
                notify_evt(VOICE_EVT_NO_PEER);
                continue;
            }
            s_active = true;
            xTaskCreatePinnedToCore(tx_task, "udp_voice_tx", 8192, NULL, 7, &s_tx_task, 0);
            xTaskCreatePinnedToCore(rx_task, "udp_voice_rx", 8192, NULL, 7, &s_rx_task, 0);
            ESP_LOGI(TAG, "START");
            notify_evt(VOICE_EVT_STARTED);
        } else if (cmd == VOICE_CMD_STOP && s_active) {
            s_active = false;
            if (s_rx_sock >= 0) {
                close(s_rx_sock);
                s_rx_sock = -1;
            }
            audio_stream_stop();
            session_manager_end(SESSION_UDP_CALL);
            ESP_LOGI(TAG, "STOP");
            notify_evt(VOICE_EVT_STOPPED);
        }
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    uint16_t seq = 0;
    static uint8_t packet[FRAME_HEADER_BYTES + AUDIO_FRAME_BYTES];
    uint32_t frames = 0;
    int64_t stat_us = esp_timer_get_time();
    uint64_t rms_acc = 0;
    while (s_active && sock >= 0) {
        if (!link_manager_is_online()) {
            udp_voice_post_cmd(VOICE_CMD_STOP);
            break;
        }
        uint8_t *pcm = packet + FRAME_HEADER_BYTES;
        if (audio_stream_read_mic(pcm, AUDIO_FRAME_BYTES) != AUDIO_FRAME_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        for (int i = 0; i + 1 < AUDIO_FRAME_BYTES; i += 2) {
            int16_t s = (int16_t)((pcm[i + 1] << 8) | pcm[i]);
            rms_acc += (uint64_t)s * s;
        }
        frames++;
        voice_frame_hdr_t *hdr = (voice_frame_hdr_t *)packet;        hdr->magic[0] = FRAME_MAGIC0;
        hdr->magic[1] = FRAME_MAGIC1;
        hdr->version = FRAME_VERSION;
        hdr->type = 0;
        hdr->seq = seq++;
        hdr->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        hdr->len = AUDIO_FRAME_BYTES;
        struct sockaddr_in peer;
        if (link_manager_get_peer(&peer)) {
            peer.sin_port = htons(UDP_VOICE_TX_PORT);
            sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&peer, sizeof(peer));
        }
        if (esp_timer_get_time() - stat_us >= 2000000) {
            double rms = frames ? sqrt((double)rms_acc / (frames * (AUDIO_FRAME_BYTES / 2))) : 0;
            ESP_LOGI(TAG, "TX_RMS=%.0f frames=%u", rms, frames);
            rms_acc = 0; frames = 0; stat_us = esp_timer_get_time();
        }
    }
    close(sock);
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

static void rx_task(void *arg)
{
    (void)arg;
    static frame_slot_t slots[REORDER_SLOTS] = {0};
    uint16_t expected = 0;
    bool have_expected = false;
    int64_t last_play_us = esp_timer_get_time();
    int64_t stat_us = last_play_us;
    uint32_t received = 0, lost = 0, bad = 0, reordered = 0;
    static uint8_t packet[FRAME_HEADER_BYTES + AUDIO_FRAME_BYTES];
    static uint8_t silence[AUDIO_FRAME_BYTES];
    while (s_active && s_rx_sock >= 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_rx_sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        int ready = select(s_rx_sock + 1, &rfds, NULL, NULL, &tv);
        if (ready > 0) {
            int n = recv(s_rx_sock, packet, sizeof(packet), 0);
            if (n != sizeof(packet)) {
                ++bad;
            } else {
                voice_frame_hdr_t *hdr = (voice_frame_hdr_t *)packet;
                if (hdr->magic[0] != FRAME_MAGIC0 || hdr->magic[1] != FRAME_MAGIC1 ||
                    hdr->version != FRAME_VERSION || hdr->type != 0 || hdr->len != AUDIO_FRAME_BYTES) {
                    ++bad;
                } else {
                    if (!have_expected) {
                        expected = hdr->seq;
                        have_expected = true;
                    }
                    if (seq_before(hdr->seq, expected)) {
                        ++bad;
                    } else {
                        frame_slot_t *slot = &slots[hdr->seq % REORDER_SLOTS];
                        if (slot->used && slot->seq != hdr->seq) {
                            ++reordered;
                        }
                        slot->used = true;
                        slot->seq = hdr->seq;
                        memcpy(slot->pcm, packet + FRAME_HEADER_BYTES, AUDIO_FRAME_BYTES);
                        ++received;
                    }
                }
            }
        }
        int64_t now = esp_timer_get_time();
        if (have_expected && now - last_play_us >= 20000) {
            frame_slot_t *slot = &slots[expected % REORDER_SLOTS];
            if (slot->used && slot->seq == expected) {
                audio_stream_write_spk(slot->pcm, AUDIO_FRAME_BYTES);
                slot->used = false;
            } else if (now - last_play_us >= 40000) {
                audio_stream_write_spk(silence, AUDIO_FRAME_BYTES);
                ++lost;
            } else {
                continue;
            }
            ++expected;
            last_play_us = now;
        }
        if (now - stat_us >= 2000000) {
            ESP_LOGI(TAG, "STAT: rx=%u lost=%u bad=%u reorder=%u", (unsigned)received,
                     (unsigned)lost, (unsigned)bad, (unsigned)reordered);
            received = lost = bad = reordered = 0;
            stat_us = now;
        }
    }
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static bool seq_before(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) < 0;
}
