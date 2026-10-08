/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps（栈外置 PSRAM，F35） */
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "esp_littlefs.h"
#include "middleware/audio/audio_service.h"
#include "middleware/system/fs_service.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/audio/session_manager.h"
#include "record_service.h"

static const char *TAG = "RECORD";
#define RECORD_PORT 50002
/* ★ §72：LOCAL 上限动态化。旧写法 RECORD_MAX_PCM_BYTES=300s×32000B=9.6MB，
 * 但 storage 分区只有 3MB（去掉字体/壁纸等已用后剩更少），300s 永远到不了，
 * 会先撞 ENOSPC。现在：每次进入 LOCAL 落盘前用 esp_littlefs_info 实时算
 * （总剩余 − 64KB 预留 − WAV 头），再封顶 HARD_MAX。 */
#define RECORD_CAP_HARD_MAX  (300U * SAMPLE_RATE * 2U)   /* 9.6MB 硬顶（跨过此值无意义） */
#define RECORD_CAP_RESERVE   (64U * 1024U)               /* littlefs 元数据 + 安全余量 */
#define RECORD_FLUSH_BYTES (64U * 1024U)
#define RECORD_CHUNK_PAYLOAD 4090
#define RECORD_PENDING_PATH FS_MNT_PATH "/recordings/pending.wav"

typedef struct __attribute__((packed)) {
    char riff[4]; uint32_t file_size; char wave[4]; char fmt[4]; uint32_t fmt_size;
    uint16_t format; uint16_t channels; uint32_t sample_rate; uint32_t byte_rate;
    uint16_t block_align; uint16_t bits; char data[4]; uint32_t data_size;
} wav_header_t;

static QueueHandle_t s_cmd_q;
static record_evt_cb_t s_evt_cb;
static volatile bool s_active;
static volatile record_mode_t s_mode;
static volatile uint32_t s_limit_bytes;   /* §72：当前 LOCAL cap（进度事件 limit_bytes 用） */

static void record_task(void *arg);
static void link_cb(link_evt_t evt, const char *peer_ip);
static void notify(record_evt_t event, record_mode_t mode, uint32_t bytes);
static void run_recording(void);
static bool open_tcp(int *sock, uint32_t base_offset, const char *session_id);
static bool send_all(int sock, const void *data, size_t len);
static bool send_chunk(int sock, const uint8_t *pcm, uint16_t len, uint32_t offset);
static uint16_t crc16_ccitt(const uint8_t *data, size_t len);
static FILE *open_local(uint32_t *bytes);
static void finalize_local(FILE *file, uint32_t bytes);
static void upload_pending(void);

/* §72：LOCAL 可录上限 = littlefs 剩余 − 预留 − WAV 头，封顶 HARD_MAX。
 * 读不到文件系统信息（未挂载）时返回 0 ⇒ 本地录音直接拒绝（保守正确）。 */
static uint32_t record_service_local_cap(void)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK || total <= used) {
        return 0;
    }
    uint32_t avail = (uint32_t)(total - used);
    uint32_t cap = 0;
    if (avail > RECORD_CAP_RESERVE + sizeof(wav_header_t)) {
        cap = avail - RECORD_CAP_RESERVE - (uint32_t)sizeof(wav_header_t);
    }
    if (cap > RECORD_CAP_HARD_MAX) {
        cap = RECORD_CAP_HARD_MAX;
    }
    return cap;
}

uint32_t record_service_get_local_cap(void) { return record_service_local_cap(); }

void record_service_init(void)
{
    mkdir(FS_MNT_PATH "/recordings", 0775);
    mkdir(FS_MNT_PATH "/wallpapers", 0775);
    s_cmd_q = xQueueCreate(8, sizeof(record_cmd_id_t));
    assert(s_cmd_q);
    link_manager_add_evt_cb(link_cb);
    /* ★ §28.40：本任务栈必须留在内部 RAM，不能像 F35 其他任务那样外置 PSRAM！
     * record_task 会做 littlefs 文件操作（RETRY→upload_pending 的 fopen /
     * LOCAL 落盘 fwrite / unlink），flash 操作期间 SPI 缓存被关、PSRAM 不可
     * 访问；IDF v6.1 在 spi_flash_disable_interrupts_caches_and_other_cpu() 里
     * assert(esp_task_stack_is_sane_cache_disabled())（esp_memory_utils.c：
     * 仅检查"当前 SP 不在 PSRAM"，无自动栈切换）——栈在 PSRAM 的任务发起
     * flash 操作直接 panic。实测：LINK ONLINE → link_cb 投 RETRY →
     * fopen 待传录音 → assert（cache_utils.c:114）。
     * F35 的"栈外置"只适用于纯 CPU/网络/GPIO 任务（全项目已排查，
     * link/udp_voice/pcnow_rx/net_time/led/bt_*_proc 均不碰 flash，唯此违例）。
     * 代价：+8KB 内部 RAM（F35 共外置 ~60KB，F37 LVGL 已 +9KB 验证过余量；
     * 若蓝牙开机再现 F35 症状，再考虑"内部栈文件工作线程"方案 B）。 */
    BaseType_t ok = xTaskCreatePinnedToCore(record_task, "record_service", 8192, NULL, 5, NULL, 0);
    assert(ok == pdPASS);
    ESP_LOGI(TAG, "init done, local cap=dynamic (littlefs free based, hard max %u B)",
             (unsigned)RECORD_CAP_HARD_MAX);
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
        record_event_t message = {.event = event, .mode = mode, .bytes = bytes,
                                  .limit_bytes = s_limit_bytes};
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
    /* §72：进入录音时算一次 LOCAL cap；STREAM→LOCAL 降级时再刷新（时间差内
       剩余空间可能又变了）。cap=0 且需要本地落盘 ⇒ 立即收场，防 ENOSPC 死循环。 */
    uint32_t cap = record_service_local_cap();
    s_limit_bytes = cap;
    notify(RECORD_EVT_STARTED, s_mode, 0);
    static uint8_t chunk[RECORD_CHUNK_PAYLOAD];
    uint16_t chunk_used = 0;
    int64_t last_report = esp_timer_get_time();
    uint32_t last_flush = 0;
    bool stop = false;
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
                    /* §72：降级瞬间重算 cap（录音期间空间可能被其它模块吃掉） */
                    cap = record_service_local_cap();
                    s_limit_bytes = cap;
                    local = open_local(&local_bytes);
                    if (!local) { stop = true; }
                    if (local && chunk_used) {
                        fwrite(chunk, 1, chunk_used, local);
                        local_bytes += chunk_used;
                        chunk_used = 0;
                    }
                }
            }
        } else {
            if (!local) {
                local = open_local(&local_bytes);
                if (!local) { stop = true; continue; }
            }
            if (cap < sizeof(frame)) {
                ESP_LOGE(TAG, "LOCAL no space: cap=%u B (littlefs free too small)", (unsigned)cap);
                notify(RECORD_EVT_ERROR, RECORD_MODE_LOCAL, 0);
                stop = true;
                continue;
            }
            if (local_bytes + sizeof(frame) > cap) {
                ESP_LOGW(TAG, "LOCAL cap reached (%u s, littlefs free based)",
                         (unsigned)(cap / (SAMPLE_RATE * 2U)));
                stop = true;
                continue;
            }
            local_bytes += fwrite(frame, 1, sizeof(frame), local);
            if (local_bytes - last_flush >= RECORD_FLUSH_BYTES) {
                fflush(local);
                last_flush = local_bytes;
            }
        }
        uint32_t total = s_mode == RECORD_MODE_STREAM ? sent_bytes + chunk_used : base_offset + local_bytes;
        if (esp_timer_get_time() - last_report > 200000) {
            /* §28.41：进度上报 1s → 200ms。1s 粒度时设备 UI 的读数最多滞后
               1s，用户停下后与 PC 端 wav 时长差"一点点"，被当成对不上。 */
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
    /* §28.41：尾部块结算后补报一次最终进度 —— 保证设备 UI 最后读到的秒数
       与 PC 收到的 wav 时长（total/32000）严格一致，消除"录制时长对不上"。 */
    notify(RECORD_EVT_PROGRESS, s_mode, total);
    audio_stream_stop();
    session_manager_end(SESSION_TCP_RECORD);
    s_active = false;
    record_mode_t done_mode = s_mode;
    s_mode = RECORD_MODE_NONE;
    ESP_LOGI(TAG, "STOP mode=%d bytes=%u", done_mode, (unsigned)total);
    notify(RECORD_EVT_STOPPED, done_mode, total);
}

static FILE *open_local(uint32_t *bytes)
{
    FILE *file = fopen(RECORD_PENDING_PATH, "wb");
    if (!file) {
        ESP_LOGE(TAG, "%s open failed, errno=%d", RECORD_PENDING_PATH, errno);
        return NULL;
    }
    wav_header_t blank = {0};
    fwrite(&blank, 1, sizeof(blank), file);
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
    fwrite(&hdr, 1, sizeof(hdr), file);
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
