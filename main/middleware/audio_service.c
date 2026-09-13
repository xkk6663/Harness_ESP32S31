/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "audio_service.h"

static const char *TAG = "AUDIO_SVC";

/* ---- 内部状态 ---- */
static esp_codec_dev_handle_t spk_codec_dev = NULL;
#if BSP_CAPS_AUDIO_MIC
static esp_codec_dev_handle_t mic_codec_dev = NULL;
#endif
static SemaphoreHandle_t audio_mux;
static QueueHandle_t    audio_cmd_queue;
static audio_evt_cb_t   audio_evt_cb_fn;

/* 播放工作缓冲（init 时一次性分配） */
static int16_t *play_wav_buf;
static int16_t *rec_buf;

/* 播放内部状态（不再对外暴露全局变量） */
static bool play_repeat = false;

/* 简易 WAV 头 */
typedef struct __attribute__((packed))
{
    uint8_t ignore_0[22];
    uint16_t num_channels;
    uint32_t sample_rate;
    uint8_t ignore_1[6];
    uint16_t bits_per_sample;
    uint8_t ignore_2[4];
    uint32_t data_size;
    uint8_t data[];
} dumb_wav_header_t;

/* ---- 前向声明 ---- */
static void audio_task(void *arg);
static void handle_play_file(const char *path);
static void handle_record(const char *path);

/* ============================================================
 * Public API
 * ============================================================ */

void audio_service_init(void)
{
    /* Speaker */
    spk_codec_dev = bsp_audio_codec_speaker_init();
    assert(spk_codec_dev);
    esp_codec_dev_set_out_vol(spk_codec_dev, DEFAULT_VOLUME);

#if BSP_CAPS_AUDIO_MIC
    mic_codec_dev = bsp_audio_codec_microphone_init();
    assert(mic_codec_dev);
    esp_codec_dev_set_in_gain(mic_codec_dev, 50.0);
#endif

    /* 互斥锁：保护 codec 的并发访问（播放写数据 / UI 调音量） */
    audio_mux = xSemaphoreCreateMutex();
    assert(audio_mux);

    /* 命令队列 */
    audio_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    assert(audio_cmd_queue);

    /* 一次性分配工作缓冲 */
    play_wav_buf = heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_DEFAULT);
    rec_buf      = heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_DEFAULT);
    assert(play_wav_buf && rec_buf);

    /* 常驻音频任务（栈 8KB：WAV 头 + 采样信息 + 命令缓冲都在栈帧里） */
    xTaskCreate(audio_task, "audio_task", 8192, NULL, 6, NULL);
    ESP_LOGI(TAG, "audio_service ready");
}

void audio_service_post_cmd(const audio_cmd_t *cmd)
{
    if (cmd == NULL || audio_cmd_queue == NULL) {
        return;
    }
    /* 非阻塞入队：满了就丢（UI 连点保护） */
    xQueueSend(audio_cmd_queue, cmd, 0);
}

void audio_service_set_evt_cb(audio_evt_cb_t cb)
{
    audio_evt_cb_fn = cb;
}

/* ============================================================
 * 常驻任务
 * ============================================================ */

static void notify_evt(audio_evt_id_t evt)
{
    if (audio_evt_cb_fn) {
        audio_evt_cb_fn(evt);
    }
}

/* 播放一条 WAV；期间通过命令队列响应 STOP/SET_REPEAT/SET_VOLUME */
static void handle_play_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "%s open failed", path);
        notify_evt(AUDIO_EVT_PLAY_DONE);
        return;
    }

    dumb_wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), file) != sizeof(hdr)) {
        ESP_LOGW(TAG, "bad wav header");
        fclose(file);
        notify_evt(AUDIO_EVT_PLAY_DONE);
        return;
    }
    ESP_LOGI(TAG, "ch=%" PRIu16 " bits=%" PRIu16 " rate=%" PRIu32 " size=%" PRIu32,
             hdr.num_channels, hdr.bits_per_sample, hdr.sample_rate, hdr.data_size);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = hdr.sample_rate,
        .channel = hdr.num_channels,
        .bits_per_sample = hdr.bits_per_sample,
#if !defined(BSP_BOARD_M5STACK_TAB5)
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
#endif
    };
    esp_codec_dev_open(spk_codec_dev, &fs);

    bool stopped = false;
    do {
        fseek(file, sizeof(hdr), SEEK_SET);
        uint32_t sent = 0;
        while (sent < hdr.data_size && !stopped) {
            xSemaphoreTake(audio_mux, portMAX_DELAY);
            size_t n = fread(play_wav_buf, 1, BUFFER_SIZE, file);
            esp_codec_dev_write(spk_codec_dev, play_wav_buf, n);
            sent += n;
            xSemaphoreGive(audio_mux);

            /* 每读一帧就非阻塞地查一下命令队列（20ms 节拍） */
            static audio_cmd_t cmd;
            if (xQueueReceive(audio_cmd_queue, &cmd, pdMS_TO_TICKS(20)) == pdPASS) {
                switch (cmd.id) {
                case AUDIO_CMD_STOP:
                    stopped = true;
                    break;
                case AUDIO_CMD_SET_REPEAT:
                    play_repeat = cmd.value ? true : false;
                    break;
                case AUDIO_CMD_SET_VOLUME:
                    xSemaphoreTake(audio_mux, portMAX_DELAY);
                    esp_codec_dev_set_out_vol(spk_codec_dev, cmd.value);
                    xSemaphoreGive(audio_mux);
                    break;
                default:
                    /* 其他命令在播放态下暂存：这里直接忽略，IDLE 态再处理 */
                    break;
                }
            }
        }
        if (!stopped) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } while (play_repeat && !stopped);

    esp_codec_dev_close(spk_codec_dev);
    fclose(file);
    notify_evt(AUDIO_EVT_PLAY_DONE);
}

static void handle_record(const char *path)
{
#if BSP_CAPS_AUDIO_MIC
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s open for record failed", path);
        notify_evt(AUDIO_EVT_RECORD_DONE);
        return;
    }

    const dumb_wav_header_t hdr = {
        .bits_per_sample = 16,
        .data_size = RECORDING_LENGTH * BUFFER_SIZE,
        .num_channels = 1,
        .sample_rate = SAMPLE_RATE,
    };
    fwrite(&hdr, 1, sizeof(hdr), f);

    ESP_LOGI(TAG, "recording start");
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    esp_codec_dev_open(mic_codec_dev, &fs);

    size_t written = 0;
    bool stopped = false;
    while (written < RECORDING_LENGTH * BUFFER_SIZE && !stopped) {
        ESP_ERROR_CHECK(esp_codec_dev_read(mic_codec_dev, rec_buf, BUFFER_SIZE));
        written += fwrite(rec_buf, 1, BUFFER_SIZE, f);

        /* 录音态也允许响应 STOP_RECORD / SET_VOLUME */
        static audio_cmd_t cmd;
        if (xQueueReceive(audio_cmd_queue, &cmd, 0) == pdPASS) {
            if (cmd.id == AUDIO_CMD_STOP_RECORD) {
                stopped = true;
            } else if (cmd.id == AUDIO_CMD_SET_VOLUME) {
                /* 录音态不调 speaker 音量，忽略 */
            }
        }
    }
    ESP_LOGI(TAG, "recording stop, %u bytes", (unsigned)written);

    esp_codec_dev_close(mic_codec_dev);
    fclose(f);
#else
    ESP_LOGI(TAG, "recording not supported");
#endif
    notify_evt(AUDIO_EVT_RECORD_DONE);
}

static void audio_task(void *arg)
{
    ESP_LOGI(TAG, "audio_task started");
    static audio_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(audio_cmd_queue, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (cmd.id) {
        case AUDIO_CMD_PLAY_FILE:
            handle_play_file(cmd.path);
            break;
        case AUDIO_CMD_START_RECORD:
            handle_record(cmd.path);
            break;
        case AUDIO_CMD_SET_REPEAT:
            play_repeat = cmd.value ? true : false;
            break;
        case AUDIO_CMD_SET_VOLUME:
            xSemaphoreTake(audio_mux, portMAX_DELAY);
            if (spk_codec_dev) {
                esp_codec_dev_set_out_vol(spk_codec_dev, cmd.value);
            }
            xSemaphoreGive(audio_mux);
            break;
        case AUDIO_CMD_STOP:
        case AUDIO_CMD_STOP_RECORD:
            /* 空闲态下收到 stop 无操作（播放/录音态在循环内消费） */
            break;
        }
    }
}
