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
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps（栈外置 PSRAM，F35） */
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "middleware/audio/audio_service.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/audio/session_manager.h"
#include "udp_voice.h"

static const char *TAG = "UDP_VOICE";
#define FRAME_MAGIC0 0xA5
#define FRAME_MAGIC1 0x5A
#define FRAME_VERSION 1
#define FRAME_HEADER_BYTES 12
#define REORDER_SLOTS 32

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
static volatile bool s_tx_active;
static volatile bool s_rx_active;
static voice_evt_cb_t s_evt_cb;

static void control_task(void *arg);
static void tx_task(void *arg);
static void rx_task(void *arg);
static void notify_evt(voice_evt_t evt);
static void ensure_audio_start(void);
static void maybe_audio_stop(void);

void udp_voice_init(void)
{
    s_cmd_q = xQueueCreate(8, sizeof(voice_cmd_id_t));
    assert(s_cmd_q);
    /* ★ 栈外置 PSRAM（F35）：8KB 栈放内部 RAM 是本项目最大单笔内部占用之一。
       栈只被 CPU 访问、不要求 DMA 可达，属规范 §1.1 里"该外置"的那一类。
       TCB 仍在内部（IDF 强制），可忽略。 */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(control_task, "udp_voice_ctl", 8192, NULL, 5, NULL, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ctl task create failed (stack->PSRAM); check "
                      "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY");
    }
    assert(ok == pdPASS);
    ESP_LOGI(TAG, "init done, tx=%d rx=%d", UDP_VOICE_TX_PORT, UDP_VOICE_RX_PORT);
}

void udp_voice_post_cmd(voice_cmd_id_t cmd)
{
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

bool udp_voice_tx_active(void) { return s_tx_active; }
bool udp_voice_rx_active(void) { return s_rx_active; }

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

/* audio stream is shared: start on first TX/RX, stop when both off */
static void ensure_audio_start(void)
{
    if (!s_tx_active && !s_rx_active) {
        if (session_manager_begin(SESSION_UDP_CALL) == ESP_OK && audio_stream_start() == ESP_OK) {
            /* ok */
        } else {
            session_manager_end(SESSION_UDP_CALL);
            ESP_LOGW(TAG, "audio start rejected");
        }
    }
}

static void maybe_audio_stop(void)
{
    if (!s_tx_active && !s_rx_active) {
        audio_stream_stop();
        session_manager_end(SESSION_UDP_CALL);
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
        switch (cmd) {
        case VOICE_CMD_TX_ON:
            if (s_tx_active) break;
            if (!link_manager_is_online()) { notify_evt(VOICE_EVT_NO_PEER); break; }
            ensure_audio_start();
            s_tx_active = true;
            xTaskCreatePinnedToCoreWithCaps(tx_task, "udp_voice_tx", 8192, NULL, 7, &s_tx_task, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ESP_LOGI(TAG, "TX_ON");
            notify_evt(VOICE_EVT_TX_ON);
            break;
        case VOICE_CMD_TX_OFF:
            if (!s_tx_active) break;
            s_tx_active = false;
            maybe_audio_stop();
            ESP_LOGI(TAG, "TX_OFF");
            notify_evt(VOICE_EVT_TX_OFF);
            break;
        case VOICE_CMD_RX_ON:
            if (s_rx_active) break;
            if (!link_manager_is_online()) { notify_evt(VOICE_EVT_NO_PEER); break; }
            ensure_audio_start();
            s_rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(UDP_VOICE_RX_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
            if (s_rx_sock < 0 || bind(s_rx_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                ESP_LOGE(TAG, "rx bind failed errno=%d", errno);
                if (s_rx_sock >= 0) close(s_rx_sock);
                s_rx_sock = -1;
                maybe_audio_stop();
                notify_evt(VOICE_EVT_NO_PEER);
                break;
            }
            s_rx_active = true;
            xTaskCreatePinnedToCoreWithCaps(rx_task, "udp_voice_rx", 8192, NULL, 7, &s_rx_task, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ESP_LOGI(TAG, "RX_ON");
            notify_evt(VOICE_EVT_RX_ON);
            break;
        case VOICE_CMD_RX_OFF:
            if (!s_rx_active) break;
            s_rx_active = false;
            if (s_rx_sock >= 0) { close(s_rx_sock); s_rx_sock = -1; }
            maybe_audio_stop();
            ESP_LOGI(TAG, "RX_OFF");
            notify_evt(VOICE_EVT_RX_OFF);
            break;
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
    while (s_tx_active && sock >= 0) {
        if (!link_manager_is_online()) break;
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
        voice_frame_hdr_t *hdr = (voice_frame_hdr_t *)packet;
        hdr->magic[0] = FRAME_MAGIC0; hdr->magic[1] = FRAME_MAGIC1;
        hdr->version = FRAME_VERSION; hdr->type = 0; hdr->seq = seq++;
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
    uint64_t rms_acc = 0;
    static uint8_t packet[FRAME_HEADER_BYTES + AUDIO_FRAME_BYTES];
    static uint8_t silence[AUDIO_FRAME_BYTES];
    while (s_rx_active && s_rx_sock >= 0) {
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
                    int16_t diff = (int16_t)(hdr->seq - expected);
                    if (!have_expected) {
                        expected = hdr->seq;
                        have_expected = true;
                        frame_slot_t *slot = &slots[hdr->seq % REORDER_SLOTS];
                        slot->used = true; slot->seq = hdr->seq;
                        memcpy(slot->pcm, packet + FRAME_HEADER_BYTES, AUDIO_FRAME_BYTES);
                        ++received;
                    } else if (diff == -1) {
                        ++bad;               /* 单帧迟到/重复：丢弃即可，随流自愈 */
                    } else if (diff < -1) {
                        /* ★ §28.42：设备 expected 超前 PC 生产 ≥2 帧 —— PC 是时钟
                         * 主（上位机 20ms 定步被 OS 调度拖慢是常态），不回跳则
                         * 后续所有包都判旧序号 → 永久静音（实测 bad 批量飙升后
                         * rx 归零）。直接回跳跟随 PC 的真实位置；顺带覆盖 PC
                         * seq 重启/回绕的大后退（旧代码只在 -0x4000 才重同步，
                         * 要漂移 327s 才救回来）。 */
                        expected = hdr->seq;
                        memset(slots, 0, sizeof(slots));
                        frame_slot_t *slot = &slots[hdr->seq % REORDER_SLOTS];
                        slot->used = true; slot->seq = hdr->seq;
                        memcpy(slot->pcm, packet + FRAME_HEADER_BYTES, AUDIO_FRAME_BYTES);
                        ++received;
                    } else if (diff >= REORDER_SLOTS / 2) {
                        /* 前向大跳（会话起步丢了一段）：与其按 60ms/帧爬坑、
                         * 让 32 槽被后续包反复冲掉（reorder 飙升），直接跳到最新 */
                        expected = hdr->seq;
                        memset(slots, 0, sizeof(slots));
                        frame_slot_t *slot = &slots[hdr->seq % REORDER_SLOTS];
                        slot->used = true; slot->seq = hdr->seq;
                        memcpy(slot->pcm, packet + FRAME_HEADER_BYTES, AUDIO_FRAME_BYTES);
                        ++received;
                    } else {
                        frame_slot_t *slot = &slots[hdr->seq % REORDER_SLOTS];
                        if (slot->used && slot->seq != hdr->seq) { ++reordered; }
                        slot->used = true; slot->seq = hdr->seq;
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
                for (int i = 0; i + 1 < AUDIO_FRAME_BYTES; i += 2) {
                    int16_t s = (int16_t)((slot->pcm[i + 1] << 8) | slot->pcm[i]);
                    rms_acc += (uint64_t)s * s;
                }
                audio_stream_write_spk(slot->pcm, AUDIO_FRAME_BYTES);
                slot->used = false;
            } else if (now - last_play_us >= 60000) {
                audio_stream_write_spk(silence, AUDIO_FRAME_BYTES);
                ++lost;
            } else { continue; }
            ++expected; last_play_us = now;
        }
        if (now - stat_us >= 2000000) {
            double rms = received ? sqrt((double)rms_acc / (received * (AUDIO_FRAME_BYTES / 2))) : 0;
            ESP_LOGI(TAG, "STAT: rx=%u lost=%u bad=%u reorder=%u RX_RMS=%.0f", (unsigned)received,
                     (unsigned)lost, (unsigned)bad, (unsigned)reordered, rms);
            received = lost = bad = reordered = 0;
            rms_acc = 0;
            stat_us = now;
        }
    }
    s_rx_task = NULL;
    vTaskDelete(NULL);
}
