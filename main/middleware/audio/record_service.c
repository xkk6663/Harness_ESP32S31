/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "middleware/audio/audio_service.h"
#include "middleware/periph/fs_service.h"
#include "middleware/mem_diag.h"
#include "middleware/net/link_manager.h"
#include "middleware/net/session_manager.h"
#include "record_service.h"

static const char *TAG = "RECORD";
#define RECORD_PORT 50002
#define RECORD_MAX_PCM_BYTES (300U * SAMPLE_RATE * 2U)
#define RECORD_FLUSH_BYTES (64U * 1024U)
#define RECORD_CHUNK_PAYLOAD 4090
#define RECORD_PENDING_PATH FS_MNT_PATH "/recordings/pending.wav"
/* 分区名（不是挂载点）：esp_littlefs_info() 要的是 partition label */
#define RECORD_FS_PARTITION "storage"

typedef struct __attribute__((packed)) {
    char riff[4]; uint32_t file_size; char wave[4]; char fmt[4]; uint32_t fmt_size;
    uint16_t format; uint16_t channels; uint32_t sample_rate; uint32_t byte_rate;
    uint16_t block_align; uint16_t bits; char data[4]; uint32_t data_size;
} wav_header_t;

static QueueHandle_t s_cmd_q;
static record_evt_cb_t s_evt_cb;
static volatile bool s_active;
static volatile record_mode_t s_mode;

static void record_task(void *arg);
static void link_cb(link_evt_t evt, const char *peer_ip);
static void notify(record_evt_t event, record_mode_t mode, uint32_t bytes);
static void run_recording(void);
static bool open_tcp(int *sock, uint32_t base_offset, const char *session_id);
static bool send_all(int sock, const void *data, size_t len);
static bool send_chunk(int sock, const uint8_t *pcm, uint16_t len, uint32_t offset);
static uint16_t crc16_ccitt(const uint8_t *data, size_t len);
static int64_t fs_avail_bytes(void);
static bool write_pcm(FILE *file, const uint8_t *pcm, size_t len);
static FILE *open_local(uint32_t *bytes);
static void finalize_local(FILE *file, uint32_t bytes);
static void upload_pending(void);

void record_service_init(void)
{
    mkdir(FS_MNT_PATH "/recordings", 0775);
    mkdir(FS_MNT_PATH "/wallpapers", 0775);
    s_cmd_q = xQueueCreate(8, sizeof(record_cmd_id_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(8, record_cmd_id_t)", 8 * sizeof(record_cmd_id_t));
    link_manager_add_evt_cb(link_cb);
    /* 栈收缩（2026-09-26 水位法）：实测峰值 352 B，1536 = 4.4x 余量 */
    BaseType_t ok = xTaskCreatePinnedToCore(record_task, "record_service", 1536, NULL, 5, NULL, 0);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreatePinnedToCore(record_service, 1536)", 1536);
    /* ★ 录音的"代码里写的上限"与"磁盘实际能给的空间"是两个数，且现在不一致：
     * RECORD_MAX_PCM_BYTES 声明 300s ≈ 9.6MB，而 storage 分区只有 6,160,384 B
     * （littlefs_content 已占 768,380）⇒ 实际可用 ≈5.2MB ≈ 166 秒就会先 ENOSPC。
     * 这里把真实剩余空间打出来，免得再靠脑补。
     * ★★ 踩坑：esp_littlefs **没有实现 statvfs/statfs**（2026-09-25 上机实测：
     *    statvfs() 直接失败，日志打成了 "fs avail unknown"）。它给的是自己的
     *    esp_littlefs_info(partition_label, &total, &used)。 */
    int64_t avail = fs_avail_bytes();
    if (avail >= 0) {
        ESP_LOGI(TAG, "init done | pcm cap=%u B (%.0f s @ %u Hz mono16) | fs avail=%.2f MB (%.0f s)",
                 (unsigned)RECORD_MAX_PCM_BYTES,
                 (double)RECORD_MAX_PCM_BYTES / (SAMPLE_RATE * 2.0), (unsigned)SAMPLE_RATE,
                 (double)avail / (1024 * 1024), (double)avail / (SAMPLE_RATE * 2.0));
    } else {
        ESP_LOGW(TAG, "init done | pcm cap=%u B (%.0f s); esp_littlefs_info(%s) failed, fs avail unknown",
                 (unsigned)RECORD_MAX_PCM_BYTES,
                 (double)RECORD_MAX_PCM_BYTES / (SAMPLE_RATE * 2.0), RECORD_FS_PARTITION);
    }
}

void record_service_post_cmd(record_cmd_id_t cmd)
{
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

bool record_service_is_active(void) { return s_active; }
record_mode_t record_service_get_mode(void) { return s_mode; }
void record_service_set_evt_cb(record_evt_cb_t cb) { s_evt_cb = cb; }

static void link_cb(link_evt_t evt, const char *peer_ip)
{
    (void)peer_ip;
    if (evt == LINK_EVT_ONLINE && !s_active) {
        record_service_post_cmd(RECORD_CMD_RETRY);
    }
}

static void notify(record_evt_t event, record_mode_t mode, uint32_t bytes)
{
    if (s_evt_cb) {
        record_event_t message = {.event = event, .mode = mode, .bytes = bytes, .limit_bytes = RECORD_MAX_PCM_BYTES};
        s_evt_cb(&message);
    }
}

static void record_task(void *arg)
{
    (void)arg;
    record_cmd_id_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (cmd == RECORD_CMD_START && !s_active) {
            run_recording();
        } else if (cmd == RECORD_CMD_RETRY && !s_active && link_manager_is_online()) {
            upload_pending();
        } else if (cmd == RECORD_CMD_DELETE_PENDING && !s_active) {
            unlink(RECORD_PENDING_PATH);
        }
    }
}

static void run_recording(void)
{
    if (session_manager_begin(SESSION_TCP_RECORD) != ESP_OK || audio_stream_start() != ESP_OK) {
        session_manager_end(SESSION_TCP_RECORD);
        ESP_LOGW(TAG, "start rejected: audio mode busy");
        notify(RECORD_EVT_ERROR, RECORD_MODE_NONE, 0);
        return;
    }
    char session_id[24];
    snprintf(session_id, sizeof(session_id), "%08" PRIx32 "%08" PRIx32,
             esp_random(), esp_random());
    s_active = true;
    s_mode = RECORD_MODE_LOCAL;
    int sock = -1;
    uint32_t sent_bytes = 0;
    if (link_manager_is_online() && open_tcp(&sock, 0, session_id)) {
        s_mode = RECORD_MODE_STREAM;
    }
    FILE *local = NULL;
    uint32_t local_bytes = 0;
    uint32_t base_offset = 0;
    notify(RECORD_EVT_STARTED, s_mode, 0);
    static uint8_t chunk[RECORD_CHUNK_PAYLOAD];
    uint16_t chunk_used = 0;
    int64_t last_report = esp_timer_get_time();
    uint32_t last_flush = 0;
    bool stop = false;
    bool disk_full = false;
    while (!stop) {
        record_cmd_id_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, 0) == pdPASS && cmd == RECORD_CMD_STOP) {
            stop = true;
        }
        uint8_t frame[AUDIO_FRAME_BYTES];
        if (audio_stream_read_mic(frame, sizeof(frame)) != sizeof(frame)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (s_mode == RECORD_MODE_STREAM) {
            memcpy(chunk + chunk_used, frame, sizeof(frame));
            chunk_used += sizeof(frame);
            if (chunk_used >= RECORD_CHUNK_PAYLOAD - AUDIO_FRAME_BYTES) {
                if (send_chunk(sock, chunk, chunk_used, sent_bytes)) {
                    sent_bytes += chunk_used;
                    chunk_used = 0;
                } else {
                    ESP_LOGW(TAG, "TCP dropped; switching to LOCAL at offset=%u", (unsigned)sent_bytes);
                    close(sock);
                    sock = -1;
                    s_mode = RECORD_MODE_LOCAL;
                    base_offset = sent_bytes;
                    local = open_local(&local_bytes);
                    if (!local) { stop = true; }
                    if (local && chunk_used) {
                        if (write_pcm(local, chunk, chunk_used)) {
                            local_bytes += chunk_used;
                        } else {
                            disk_full = true;
                        }
                        chunk_used = 0;
                    }
                }
            }
        } else {
            if (!local) {
                local = open_local(&local_bytes);
                if (!local) { stop = true; continue; }
            }
            if (local_bytes + sizeof(frame) > RECORD_MAX_PCM_BYTES) {
                ESP_LOGW(TAG, "LOCAL cap reached (300s)");
                stop = true;
                continue;
            }
            if (write_pcm(local, frame, sizeof(frame))) {
                local_bytes += sizeof(frame);
            } else {
                disk_full = true;
            }
            if (local_bytes - last_flush >= RECORD_FLUSH_BYTES) {
                fflush(local);
                last_flush = local_bytes;
            }
        }
        /* ★ 写盘失败 ⇒ 立即收口并**主动告知**（RECORD_EVT_ERROR），
         *   不能再让循环空转（那正是"静默数据丢失"的形态）。 */
        if (disk_full) {
            ESP_LOGE(TAG, "LOCAL write failed => stop early, recorded=%u B (of cap %u)",
                     (unsigned)local_bytes, (unsigned)RECORD_MAX_PCM_BYTES);
            notify(RECORD_EVT_ERROR, s_mode, base_offset + local_bytes);
            stop = true;
            continue;
        }
        uint32_t total = s_mode == RECORD_MODE_STREAM ? sent_bytes + chunk_used : base_offset + local_bytes;
        if (esp_timer_get_time() - last_report > 1000000) {
            notify(RECORD_EVT_PROGRESS, s_mode, total);
            last_report = esp_timer_get_time();
        }
    }
    if (s_mode == RECORD_MODE_STREAM && chunk_used) {
        if (send_chunk(sock, chunk, chunk_used, sent_bytes)) {
            sent_bytes += chunk_used;
        }
    }
    if (s_mode == RECORD_MODE_STREAM && sock >= 0) {
        char end[128];
        snprintf(end, sizeof(end), "{\"type\":\"REC_END\",\"session_id\":\"%s\",\"total_bytes\":%u}\n",
                 session_id, (unsigned)sent_bytes);
        send_all(sock, end, strlen(end));
        close(sock);
    }
    if (local) {
        finalize_local(local, local_bytes);
        if (link_manager_is_online()) {
            upload_pending();
        }
    }
    uint32_t total = s_mode == RECORD_MODE_STREAM ? sent_bytes : base_offset + local_bytes;
    audio_stream_stop();
    session_manager_end(SESSION_TCP_RECORD);
    s_active = false;
    record_mode_t done_mode = s_mode;
    s_mode = RECORD_MODE_NONE;
    ESP_LOGI(TAG, "STOP mode=%d bytes=%u", done_mode, (unsigned)total);
    notify(RECORD_EVT_STOPPED, done_mode, total);
}

/* 分区剩余字节数；失败返回 -1。
 * ★ esp_littlefs 没有 statvfs/statfs（实测失败），只能用 esp_littlefs_info()。 */
static int64_t fs_avail_bytes(void)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info(RECORD_FS_PARTITION, &total, &used) != ESP_OK) {
        return -1;
    }
    return (int64_t)total - (int64_t)used;
}

/* ★ P0（方案 §8-F-1）：磁盘满时 fwrite 返回 0，而原代码不看返回值 ⇒
 *   local_bytes 冻结 ⇒ 停止条件 local_bytes >= RECORD_MAX_PCM_BYTES 永远不成立
 *   ⇒ 录音"看着还在继续"、文件却不再增长、**一句日志都没有**（静默数据丢失）。
 *   这里统一改成"短写即失败"，并把 errno 与分区剩余空间一起打出来。 */
static bool write_pcm(FILE *file, const uint8_t *pcm, size_t len)
{
    size_t wrote = fwrite(pcm, 1, len, file);
    if (wrote == len) {
        return true;
    }
    int64_t avail = fs_avail_bytes();
    ESP_LOGE(TAG, "fwrite short: want=%u wrote=%u errno=%d (%s) | fs avail=%s",
             (unsigned)len, (unsigned)wrote, errno, strerror(errno),
             avail < 0 ? "unknown" : "see below");
    if (avail >= 0) {
        ESP_LOGE(TAG, "  -> fs avail=%.2f MB", (double)avail / (1024.0 * 1024.0));
    }
    if (ferror(file)) {
        clearerr(file);
    }
    return false;
}

static FILE *open_local(uint32_t *bytes)
{
    FILE *file = fopen(RECORD_PENDING_PATH, "wb");
    if (!file) {
        ESP_LOGE(TAG, "%s open failed, errno=%d", RECORD_PENDING_PATH, errno);
        return NULL;
    }
    wav_header_t blank = {0};
    if (!write_pcm(file, (const uint8_t *)&blank, sizeof(blank))) {
        fclose(file);
        return NULL;   /* 头都写不进去（通常就是空间不足）⇒ 别开这场录音 */
    }
    *bytes = 0;
    return file;
}

static void finalize_local(FILE *file, uint32_t bytes)
{
    wav_header_t hdr = {.riff = {'R','I','F','F'}, .file_size = bytes + 36, .wave = {'W','A','V','E'},
                        .fmt = {'f','m','t',' '}, .fmt_size = 16, .format = 1, .channels = 1,
                        .sample_rate = SAMPLE_RATE, .byte_rate = SAMPLE_RATE * 2, .block_align = 2,
                        .bits = 16, .data = {'d','a','t','a'}, .data_size = bytes};
    fseek(file, 0, SEEK_SET);
    if (!write_pcm(file, (const uint8_t *)&hdr, sizeof(hdr))) {
        /* 头写不回去 ⇒ 这个 wav 不可播（upload_pending 会靠 memcmp("RIFF") 拦住它，
         * 不会把坏文件传上去）。 */
        ESP_LOGE(TAG, "wav header rewrite FAILED, %u B pcm stays header-less", (unsigned)bytes);
    }
    fflush(file);
    fclose(file);
}

static bool open_tcp(int *sock, uint32_t base_offset, const char *session_id)
{
    struct sockaddr_in peer;
    if (!link_manager_get_peer(&peer)) return false;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return false;
    peer.sin_port = htons(RECORD_PORT);
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        close(fd);
        return false;
    }
    char begin[256];
    snprintf(begin, sizeof(begin),
             "{\"type\":\"REC_BEGIN\",\"session_id\":\"%s\",\"name\":\"recording.wav\",\"sample_rate\":16000,\"channels\":1,\"bits\":16,\"base_offset\":%u}\n",
             session_id, (unsigned)base_offset);
    if (!send_all(fd, begin, strlen(begin))) { close(fd); return false; }
    *sock = fd;
    return true;
}

static bool send_all(int sock, const void *data, size_t len)
{
    const uint8_t *ptr = data;
    while (len) {
        int n = send(sock, ptr, len, 0);
        if (n <= 0) return false;
        ptr += n;
        len -= n;
    }
    return true;
}

static bool send_chunk(int sock, const uint8_t *pcm, uint16_t len, uint32_t offset)
{
    uint8_t head[10];
    uint32_t chunk_len = len + 6;
    uint16_t crc = crc16_ccitt(pcm, len);
    memcpy(head, &chunk_len, 4); memcpy(head + 4, &offset, 4); memcpy(head + 8, &crc, 2);
    return send_all(sock, head, sizeof(head)) && send_all(sock, pcm, len);
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static void upload_pending(void)
{
    FILE *file = fopen(RECORD_PENDING_PATH, "rb");
    if (!file) return;
    wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), file) != sizeof(hdr) || memcmp(hdr.riff, "RIFF", 4)) { fclose(file); return; }
    char session_id[24];
    snprintf(session_id, sizeof(session_id), "local%08" PRIx32, esp_random());
    int sock = -1;
    if (!open_tcp(&sock, 0, session_id)) { fclose(file); return; }
    static uint8_t chunk[RECORD_CHUNK_PAYLOAD];
    uint32_t offset = 0;
    size_t n;
    bool ok = true;
    while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (!send_chunk(sock, chunk, (uint16_t)n, offset)) { ok = false; break; }
        offset += n;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    char end[128];
    snprintf(end, sizeof(end), "{\"type\":\"REC_END\",\"session_id\":\"%s\",\"total_bytes\":%u}\n",
             session_id, (unsigned)offset);
    ok = ok && send_all(sock, end, strlen(end));
    close(sock);
    fclose(file);
    if (ok) {
        unlink(RECORD_PENDING_PATH);
        ESP_LOGI(TAG, "pending upload done, %u bytes", (unsigned)offset);
        notify(RECORD_EVT_UPLOAD_DONE, RECORD_MODE_LOCAL, offset);
    }
}
