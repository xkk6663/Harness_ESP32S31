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
#include "middleware/audio/audio_service.h"
#include "middleware/net/link_manager.h"
#include "middleware/net/session_manager.h"
#include "middleware/mem_diag.h"
#include "udp_voice.h"

static const char *TAG = "UDP_VOICE";
#define FRAME_MAGIC0 0xA5
#define FRAME_MAGIC1 0x5A
#define FRAME_VERSION 1
#define FRAME_HEADER_BYTES 12

/* ★ P2-1：重排池容量。frame_slot_t = 644 B（bool+pad+uint16+640B PCM）⇒
 *   32 → 20,608 B；8 → 5,152 B，**省 15,456 B 内部 .bss**。
 *
 * ★ 先确认过 S5 规范（"池容量与丢弃阈值不得相等"）：
 *   本文件把"等不到就先补静音"的阈值**硬编码在播放分支里**
 *   （见 rx_task：`now - last_play_us >= 60000` ⇒ 60 ms），与 REORDER_SLOTS 无关
 *   ⇒ 缩池**不会**改丢弃行为。这也是能安全缩的前提。
 *
 * ★★ 但"窗口"这个说法只讲了一半 —— 真正的约束是**发送端超前量的上限**：
 *   slot 下标是 `seq % REORDER_SLOTS`。若收到的 seq 比 expected 超前 **≥ N 帧**，
 *   就命中 `hdr->seq % N == expected % N`，**直接覆盖掉还没播的那一帧** ⇒ 该帧永久丢失。
 *   所以 N 不是"抖动窗口"，而是"容忍 PC 侧一次 burst 连发多少帧"。
 *   - 纯抖动：LAN 抖动 ≪ 50 ms ⇒ N=8（160 ms）绰绰有余（这正是原方案的论证，成立）。
 *   - **burst 场景（方案漏了）**：WiFi 卡顿 200 ms 后，PC 侧把这十几帧一次性补发，
 *     超前量可瞬间 >8 ⇒ 丢帧。本工程 WiFi 抖动是已知高发（见 F48 / reason=200）。
 *   ⇒ 现取 8（按方案 §4-P2-1）；**验收判据 = 现成的 STAT 行 `reorder=` 与 `lost=`**
 *     （`udp_voice.c` 每 2 s 打一次）：只在 WiFi 抖动期观察，两者不上升才算通过。
 *     若 `reorder=` 上升 ⇒ 就是 burst 溢出，把本值改 12（240 ms，仍省 12,880 B）
 *     或 16（320 ms，省 10,304 B）即可，**不需要改任何其它代码**。 */
#define REORDER_SLOTS 8

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
static bool seq_before(uint16_t a, uint16_t b);
static void ensure_audio_start(void);
static void maybe_audio_stop(void);

void udp_voice_init(void)
{
    s_cmd_q = xQueueCreate(8, sizeof(voice_cmd_id_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(8, voice_cmd_id_t)", 8 * sizeof(voice_cmd_id_t));
    /* 栈收缩（2026-09-26 水位法）：实测峰值 428 B，1536 = 3.6x 余量。
     * ★ tx/rx 两个任务保持 8192 不动 —— 没跑过通话，没有水位数据。 */
    BaseType_t ok = xTaskCreatePinnedToCore(control_task, "udp_voice_ctl", 1536, NULL, 5, NULL, 0);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreatePinnedToCore(udp_voice_ctl, 1536)", 1536);
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
            /* ★ 任务创建结果原来没人看：失败时 s_tx_active 已经是 true 而任务不存在，
             * 上层却收到 TX_ON ⇒ 卡死在"TX_ON 但没声音"且无任何日志。
             * 运行期不 abort（规范 A2），但必须说话 + 回滚状态。 */
            if (xTaskCreatePinnedToCore(tx_task, "udp_voice_tx", 8192, NULL, 7, &s_tx_task, 0) != pdPASS) {
                s_tx_task = NULL;
                s_tx_active = false;
                MEM_ERR_DIAG(TAG, "xTaskCreatePinnedToCore(udp_voice_tx, 8192) FAILED", 8192);
                maybe_audio_stop();
                notify_evt(VOICE_EVT_ERROR);
                break;
            }
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
                notify_evt(VOICE_EVT_ERROR);
                break;
            }
            s_rx_active = true;
            if (xTaskCreatePinnedToCore(rx_task, "udp_voice_rx", 8192, NULL, 7, &s_rx_task, 0) != pdPASS) {
                s_rx_task = NULL;
                s_rx_active = false;
                MEM_ERR_DIAG(TAG, "xTaskCreatePinnedToCore(udp_voice_rx, 8192) FAILED", 8192);
                close(s_rx_sock);
                s_rx_sock = -1;
                maybe_audio_stop();
                notify_evt(VOICE_EVT_ERROR);
                break;
            }
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
                    if (!have_expected) { expected = hdr->seq; have_expected = true; }
                    else if ((int16_t)(hdr->seq - expected) < -0x4000) {
                        /* PC restarted seq: re-sync */
                        expected = hdr->seq;
                        memset(slots, 0, sizeof(slots));
                    } else if (seq_before(hdr->seq, expected)) { ++bad; }
                    else {
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

static bool seq_before(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) < 0;
}
