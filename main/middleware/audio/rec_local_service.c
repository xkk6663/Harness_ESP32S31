/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rec_local_service.c
 * @brief 本地录音服务实现（设计守约见 .h；改动记录 §40）
 *
 * 采集：16k / 2ch / 16bit（板级双 mic，与 xiaozhi 的 XZ_* 三元组一致，
 * mic 布局 MR；增益 open 后 set_in_gain(24.0) 同 xiaozhi 已验证值）。
 * 落盘：WAV(PCM) 44 字节头，停止时回填 data_size / riff_size。
 * 容量：单次上限 300 s（64 KB/s ≈ 19.2 MB），fwrite 短写（盘满）立刻收尾。
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"

#include "audio_service.h"
#include "rec_local_service.h"
#include "middleware/mem_diag.h"
#include "middleware/periph/sdcard_service.h"

#define REC_DIR         SDCARD_MNT_PATH"/music"
#define REC_RATE        16000
#define REC_CHANNELS    2       /* 板级双 mic（同 xiaozhi XZ_BOARD_CHANNELS） */
#define REC_BITS        16
#define REC_CHUNK       2048    /* 32ms @ 64 KB/s */
#define REC_MAX_MS      (300U * 1000U)
#define REC_PATH_MAX    48

static const char *TAG = "REC_LOC";

/* ---- WAV 头（44 B，PCM）---- */
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t riff_size;      /* 36 + data_size，停止时回填 */
    char     wave[4];
    char     fmt_id[4];
    uint32_t fmt_size;       /* 16 */
    uint16_t audio_fmt;      /* 1 = PCM */
    uint16_t channels;
    uint32_t rate;
    uint32_t byte_rate;      /* rate * block_align */
    uint16_t block_align;    /* channels * bits / 8 */
    uint16_t bits;
    char     data_id[4];
    uint32_t data_size;      /* 停止时回填 */
} wav_hdr_t;
_Static_assert(sizeof(wav_hdr_t) == 44, "wav header must be 44 bytes");

/* ---- 状态 ---- */
typedef enum { REC_CMD_START = 0 } rec_cmd_id_t;
typedef struct { rec_cmd_id_t id; } rec_cmd_t;

static QueueHandle_t s_cmd_q;
static rec_loc_evt_cb_t s_evt_cb;

static volatile bool s_active;        /* pending || recording（§39 语义） */
static volatile bool s_recording;     /* 真在采集循环里 */
static volatile bool s_stop_req;      /* stop() 置位，循环每 chunk 查 */
static volatile TickType_t s_start_tick;
static char s_last_path[REC_PATH_MAX];
static bool s_last_ok;

/* 任务私有（单实例任务串行使用，无需保护） */
static uint8_t s_pcm[REC_CHUNK];
static uint8_t s_file_buf[4096];

/* ---- 前向 ---- */
static void rec_task(void *arg);

static inline uint32_t tick_to_ms(TickType_t t)
{
    return (uint32_t)t * (uint32_t)portTICK_PERIOD_MS;
}

/* ============================================================
 * Public API
 * ============================================================ */

void rec_local_service_init(void)
{
    s_cmd_q = xQueueCreate(2, sizeof(rec_cmd_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(2, rec_cmd_t)", 2 * sizeof(rec_cmd_t));
    /* ★ 栈必须内部 RAM：本任务做 SD 卡 FATFS IO（F35 磁盘 IO 任务栈铁律） */
    BaseType_t ok = xTaskCreate(rec_task, "rec_task", 4096, NULL, 5, NULL);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreate(rec_task, 4096)", 4096);
    ESP_LOGI(TAG, "rec_local_service ready (ext EXCLUSIVE mic lease)");
}

esp_err_t rec_local_start(void)
{
    if (s_cmd_q == NULL || s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 先清 stop 标志**再入队**：xQueueSend 会立刻唤醒 rec_task（双核下另一核
     *   可能抢先跑），若先入队后清标志，task 会看到上一轮 stop 残留的 true 而
     *   误判"pending 期间被 stop"→ 秒取消。实测（2026-09-26）：第二条录不了，
     *   日志特征 `start cancelled (stop during pending)`，就是它。 */
    s_stop_req = false;
    s_last_ok = false;
    s_last_path[0] = '\0';
    rec_cmd_t cmd = { .id = REC_CMD_START };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdPASS) {
        return ESP_ERR_INVALID_STATE;
    }
    s_active = true;                 /* ★ 入队即"已请求"，堵建链窗口 */
    return ESP_OK;
}

void rec_local_stop(void)
{
    s_stop_req = true;
}

bool rec_local_service_is_active(void)
{
    return s_active;
}

uint32_t rec_local_elapsed_ms(void)
{
    if (!s_recording) {
        return 0;
    }
    return tick_to_ms(xTaskGetTickCount() - s_start_tick);
}

const char *rec_local_last_path(void)
{
    return s_last_path;
}

bool rec_local_last_ok(void)
{
    return s_last_ok;
}

void rec_local_set_evt_cb(rec_loc_evt_cb_t cb)
{
    s_evt_cb = cb;
}

/* ============================================================
 * 内部
 * ============================================================ */

static void notify(rec_loc_evt_id_t evt, const char *path)
{
    if (s_evt_cb) {
        s_evt_cb(evt, path);
    }
}

/* 找一个不存在的 rec_XXX.wav（000..999）；失败返回 false */
static bool pick_path(char *buf, size_t len)
{
    for (unsigned i = 0; i < 1000; i++) {
        int n = snprintf(buf, len, REC_DIR"/rec_%03u.wav", i);
        if (n <= 0 || n >= (int)len) {
            return false;
        }
        struct stat st;
        if (stat(buf, &st) != 0) {
            return true;    /* 不存在 → 用它 */
        }
    }
    return false;
}

static void do_record(void)
{
    esp_codec_dev_handle_t spk = NULL, mic = NULL;
    FILE *f = NULL;
    bool f_data_written = false;     /* 头之后是否写过文件（决定 fail 时删不删） */
    uint32_t written = 0;
    bool io_error = false;

    /* 1. ext 独占租借：MP3/小智/WAV 播放占用期间这里 BUSY（反之亦然） */
    if (audio_service_ext_claim_exclusive(&spk, &mic) != ESP_OK) {
        ESP_LOGW(TAG, "mic BUSY (mp3/xiaozhi/wav playing?)");
        notify(REC_LOC_EVT_ERROR, NULL);
        s_active = false;
        return;
    }
    if (mic == NULL) {
        ESP_LOGE(TAG, "no mic on this board");
        goto _fail;
    }
    if (!sdcard_service_is_mounted()) {
        ESP_LOGE(TAG, "SD not mounted");
        goto _fail;
    }
    mkdir(REC_DIR, 0775);            /* 已存在则 EEXIST，无害 */

    /* 2. 文件 + 头 */
    if (!pick_path(s_last_path, sizeof(s_last_path))) {
        ESP_LOGE(TAG, "no free rec_XXX.wav slot (999 files?)");
        goto _fail;
    }
    f = fopen(s_last_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s open for write failed: errno=%d", s_last_path, errno);
        goto _fail;
    }
    setvbuf(f, (char *)s_file_buf, _IOFBF, sizeof(s_file_buf));

    {
        wav_hdr_t hdr = {
            .riff = {'R','I','F','F'},
            .riff_size = 0,              /* 停止时回填 */
            .wave = {'W','A','V','E'},
            .fmt_id = {'f','m','t',' '},
            .fmt_size = 16,
            .audio_fmt = 1,
            .channels = REC_CHANNELS,
            .rate = REC_RATE,
            .block_align = REC_CHANNELS * REC_BITS / 8,
            .bits = REC_BITS,
            .data_id = {'d','a','t','a'},
            .data_size = 0,              /* 停止时回填 */
        };
        hdr.byte_rate = REC_RATE * hdr.block_align;
        if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
            ESP_LOGE(TAG, "wav header write failed");
            goto _fail;
        }
    }

    /* 3. open mic（16k/2ch/16bit，同 xiaozhi）+ 增益 */
    {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = REC_RATE,
            .channel = REC_CHANNELS,
            .bits_per_sample = REC_BITS,
        };
        if (esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic open failed");
            goto _fail;
        }
        esp_codec_dev_set_in_gain(mic, 24.0);   /* 同 xiaozhi 已验证值 */
    }

    s_recording = true;
    s_start_tick = xTaskGetTickCount();
    f_data_written = true;
    ESP_LOGI(TAG, "recording -> %s (16k/%dch/%dbit, cap %us)",
             s_last_path, REC_CHANNELS, REC_BITS, (unsigned)(REC_MAX_MS / 1000));
    notify(REC_LOC_EVT_STARTED, s_last_path);

    /* 4. 采集循环：STOP 置标志 / 300s 上限 / 盘满即收（≤32ms 响应） */
    while (!s_stop_req &&
           tick_to_ms(xTaskGetTickCount() - s_start_tick) < REC_MAX_MS) {
        if (esp_codec_dev_read(mic, s_pcm, sizeof(s_pcm)) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic read failed -> stop");
            io_error = true;
            break;
        }
        size_t w = fwrite(s_pcm, 1, sizeof(s_pcm), f);
        if (w != sizeof(s_pcm)) {
            ESP_LOGE(TAG, "fwrite short: %u/%u (disk full?) -> stop, %u KB total",
                     (unsigned)w, (unsigned)sizeof(s_pcm), (unsigned)(written / 1024));
            if (ferror(f)) clearerr(f);
            io_error = true;
            break;
        }
        written += w;
    }

    /* 5. 收尾：回填 WAV 长度（正常停 / 300s 到 / 盘满 / 读错 都走这） */
    if (written != 0) {
        uint32_t data_size = written;
        uint32_t riff_size = 36 + written;
        bool ok_sz = fseek(f, (long)offsetof(wav_hdr_t, data_size), SEEK_SET) == 0 &&
                     fwrite(&data_size, 1, sizeof(data_size), f) == sizeof(data_size) &&
                     fseek(f, (long)offsetof(wav_hdr_t, riff_size), SEEK_SET) == 0 &&
                     fwrite(&riff_size, 1, sizeof(riff_size), f) == sizeof(riff_size);
        if (!ok_sz) {
            ESP_LOGE(TAG, "wav size fixup FAILED (file may misreport length)");
        }
    }
    ESP_LOGI(TAG, "stop: %u B in %u ms%s",
             (unsigned)written,
             (unsigned)tick_to_ms(xTaskGetTickCount() - s_start_tick),
             io_error ? " [io error]" : "");

    fclose(f);
    esp_codec_dev_close(mic);
    audio_service_ext_release_exclusive();
    s_recording = false;
    s_last_ok = !io_error && written > 0;
    s_active = false;
    notify(io_error ? REC_LOC_EVT_ERROR : REC_LOC_EVT_DONE, s_last_path);
    return;

_fail:
    /* ★ 此路径 mic 尚未 open 成功（各 goto 都在 open 之前），无需 close；
     *   已写过数据头的半截文件直接删掉，不留垃圾。 */
    if (f != NULL) {
        fclose(f);
        if (!f_data_written) {
            remove(s_last_path);
            ESP_LOGW(TAG, "removed partial %s", s_last_path);
        }
    }
    audio_service_ext_release_exclusive();
    s_recording = false;
    s_last_ok = false;
    s_active = false;
    notify(REC_LOC_EVT_ERROR, s_last_path);
}

static void rec_task(void *arg)
{
    (void)arg;
    static rec_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (cmd.id) {
        case REC_CMD_START:
            if (s_stop_req) {
                /* pending 期间被 stop()：直接取消（§39 同款防误杀收口） */
                ESP_LOGI(TAG, "start cancelled (stop during pending)");
                s_active = false;
                break;
            }
            do_record();
            break;
        }
    }
}
