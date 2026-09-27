/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
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
#include "middleware/mem_diag.h"
#include "3rd/ringbuf/ringbuf.h"

static const char *TAG = "AUDIO_SVC";

/* ---- 内部状态 ---- */
static esp_codec_dev_handle_t spk_codec_dev = NULL;
#if BSP_CAPS_AUDIO_MIC
static esp_codec_dev_handle_t mic_codec_dev = NULL;
#endif
static SemaphoreHandle_t audio_mux;
static QueueHandle_t    audio_cmd_queue;
static audio_evt_cb_t   audio_evt_cb_fn;

/* 播放工作缓冲：用第三方 ringbuf 库管理（main/3rd/ringbuf） */
static uint8_t     play_rb_pool[BUFFER_SIZE];
static ringbuf_t   play_rb;

/* 播放内部状态（不再对外暴露全局变量） */
static bool play_repeat = false;

/* ★ P2-3：播放落盘 I/O 的 1 KB 暂存块。
 *   ★ 2026-09-27 call/rec 阉割：原与 handle_record 共用的说明已过时
 *     （handle_record 已删），现仅 handle_play_file 使用。
 *   ★ 注意：语音流路径（audio_stream_task）已随 call/rec 一并删除。 */
static uint8_t audio_task_io[BUFFER_SIZE];

/* ---- ext 客户端协议状态（S5 §2.2）----
 * s_ext_mode：NONE / STREAM（MP3 逐块写，dev 由服务持有并 open）
 *            / EXCLUSIVE（小智独占租借，服务不 open，句柄借出）。
 * s_svc_busy：audio_task 正在同步执行 handle_play_file（命令是
 *   串行消费的，播放中 audio_task 不回队列，外部无从得知"是否空闲"，用此标志）。 */
static volatile audio_ext_mode_t s_ext_mode = AUDIO_EXT_NONE;
static volatile bool s_svc_busy = false;

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

/* ============================================================
 * Public API
 * ============================================================ */

void audio_service_init(void)
{
    /* Speaker */
    spk_codec_dev = bsp_audio_codec_speaker_init();
    /* ★ P0-4：初始化期 fail-fast 保留（规范 A1 允许），但先落一条带三元组的日志再 abort。
     * 裸 assert 有两个问题：NDEBUG 下被编译掉（失败变**静默**）、现场没有任何内存证据。 */
    MEM_FAIL_FAST(spk_codec_dev, TAG, "bsp_audio_codec_speaker_init()", 0);
    esp_codec_dev_set_out_vol(spk_codec_dev, DEFAULT_VOLUME);

#if BSP_CAPS_AUDIO_MIC
    mic_codec_dev = bsp_audio_codec_microphone_init();
    MEM_FAIL_FAST(mic_codec_dev, TAG, "bsp_audio_codec_microphone_init()", 0);
    esp_codec_dev_set_in_gain(mic_codec_dev, 50.0);
#endif

    /* 互斥锁：保护 codec 的并发访问（播放写数据 / UI 调音量） */
    audio_mux = xSemaphoreCreateMutex();
    MEM_FAIL_FAST(audio_mux, TAG, "xSemaphoreCreateMutex(audio_mux)", 0);

    /* 命令队列 */
    audio_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    MEM_FAIL_FAST(audio_cmd_queue, TAG, "xQueueCreate(4, audio_cmd_t)",
                  4 * sizeof(audio_cmd_t));

    /* 初始化第三方 ringbuf（pool 为静态数组，无需动态分配） */
    ringbuffer_init(&play_rb, play_rb_pool, BUFFER_SIZE);

    /* 常驻音频任务（栈 8KB：WAV 头 + 采样信息 + 命令缓冲都在栈帧里） */
    /* 栈收缩（2026-09-26 水位法）：实测峰值 756 B，2048 = 2.7x 余量 */
    BaseType_t ok = xTaskCreate(audio_task, "audio_task", 2048, NULL, 6, NULL);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreate(audio_task, 2048)", 2048);
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
 * ext 客户端协议（S5 §2.2/2.3）
 * ============================================================ */

/* 两个互斥条件的统一检查：任何 ext/服务内播放 已占用即忙。
 * 调用方须持有 audio_mux。
 * ★ 2026-09-27 call/rec 阉割：原 stream_active 条件已随全双工流路径删除。 */
static bool ext_slot_busy_locked(void)
{
    return s_ext_mode != AUDIO_EXT_NONE || s_svc_busy;
}

esp_err_t audio_service_ext_claim(const audio_ext_fmt_t *fmt)
{
    if (fmt == NULL || fmt->sample_rate <= 0 ||
        (fmt->channels != 1 && fmt->channels != 2) ||
        (fmt->bits != 16 && fmt->bits != 32)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    if (ext_slot_busy_locked()) {
        xSemaphoreGive(audio_mux);
        ESP_LOGW(TAG, "ext_claim BUSY (mode=%d busy=%d)",
                 s_ext_mode, s_svc_busy);
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = fmt->sample_rate,
        .channel = fmt->channels,
        .bits_per_sample = fmt->bits,
#if !defined(BSP_BOARD_M5STACK_TAB5)
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
#endif
    };
    err = esp_codec_dev_open(spk_codec_dev, &fs);
    if (err == ESP_OK) {
        s_ext_mode = AUDIO_EXT_STREAM;
        ESP_LOGI(TAG, "ext STREAM claimed: %d Hz / %d ch / %d bit",
                 fmt->sample_rate, fmt->channels, fmt->bits);
    } else {
        ESP_LOGE(TAG, "ext_claim open failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(audio_mux);
    return err;
}

esp_err_t audio_service_ext_write(const void *pcm, size_t bytes)
{
    if (!pcm || !bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ext_mode != AUDIO_EXT_STREAM) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    err = esp_codec_dev_write(spk_codec_dev, (void *)pcm, bytes);
    xSemaphoreGive(audio_mux);
    return err;
}

void audio_service_ext_release(void)
{
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    if (s_ext_mode == AUDIO_EXT_STREAM) {
        esp_codec_dev_close(spk_codec_dev);
        s_ext_mode = AUDIO_EXT_NONE;
        ESP_LOGI(TAG, "ext STREAM released");
    }
    xSemaphoreGive(audio_mux);
}

esp_err_t audio_service_ext_claim_exclusive(esp_codec_dev_handle_t *spk,
                                            esp_codec_dev_handle_t *mic)
{
    if (spk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    if (ext_slot_busy_locked()) {
        xSemaphoreGive(audio_mux);
        ESP_LOGW(TAG, "ext_claim_exclusive BUSY (mode=%d busy=%d)",
                 s_ext_mode, s_svc_busy);
        return ESP_ERR_INVALID_STATE;
    }
    /* 复用服务句柄借出：调用方自行 open/close（16k 全双工 + mic 布局自定）。
     * 服务自身保持 IDLE——不 open、不写、不 close，归还前绝不碰这两个句柄。 */
    s_ext_mode = AUDIO_EXT_EXCLUSIVE;
    *spk = spk_codec_dev;
#if BSP_CAPS_AUDIO_MIC
    if (mic) {
        *mic = mic_codec_dev;
    }
#else
    if (mic) {
        *mic = NULL;
    }
#endif
    xSemaphoreGive(audio_mux);
    ESP_LOGI(TAG, "ext EXCLUSIVE claimed (handles lent out)");
    return ESP_OK;
}

void audio_service_ext_release_exclusive(void)
{
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    if (s_ext_mode == AUDIO_EXT_EXCLUSIVE) {
        /* 约定：调用方归还前必须已 close 两个句柄（服务在此只清状态） */
        s_ext_mode = AUDIO_EXT_NONE;
        ESP_LOGI(TAG, "ext EXCLUSIVE released");
    }
    xSemaphoreGive(audio_mux);
}

bool audio_service_mic_available(void)
{
    return s_ext_mode == AUDIO_EXT_NONE;
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
    s_svc_busy = true;   /* ext 协议：服务内播放期间外部 claim 一律 BUSY */
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "%s open failed", path);
        s_svc_busy = false;
        notify_evt(AUDIO_EVT_PLAY_DONE);
        return;
    }

    dumb_wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), file) != sizeof(hdr)) {
        ESP_LOGW(TAG, "bad wav header");
        fclose(file);
        s_svc_busy = false;
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
    uint8_t *io_tmp = audio_task_io;   /* ★ P2-3：与 handle_record 共用（同一任务、互斥） */
    do {
        fseek(file, sizeof(hdr), SEEK_SET);
        uint32_t sent = 0;
        while (sent < hdr.data_size && !stopped) {
            xSemaphoreTake(audio_mux, portMAX_DELAY);
            size_t n = fread(io_tmp, 1, BUFFER_SIZE, file);
            /* 写入 ringbuf，再读出给 codec（统一走第三方 ringbuf 接口） */
            ringbuffer_put(&play_rb, io_tmp, (uint16_t)n);
            uint16_t got = ringbuffer_get(&play_rb, io_tmp, (uint16_t)n);
            esp_codec_dev_write(spk_codec_dev, io_tmp, got);
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
    s_svc_busy = false;
    notify_evt(AUDIO_EVT_PLAY_DONE);
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
            if (s_ext_mode != AUDIO_EXT_NONE) {
                /* ext 占用期间（MP3/小智）拒绝 WAV 播放：同一 codec */
                ESP_LOGW(TAG, "WAV playback refused: ext mode=%d", s_ext_mode);
            } else {
                handle_play_file(cmd.path);
            }
            break;
        case AUDIO_CMD_SET_REPEAT:
            play_repeat = cmd.value ? true : false;
            break;
        case AUDIO_CMD_SET_VOLUME:
            xSemaphoreTake(audio_mux, portMAX_DELAY);
            if (spk_codec_dev && s_ext_mode != AUDIO_EXT_EXCLUSIVE) {
                /* EXCLUSIVE 期间音量归调用方管线管（服务 dev 未 open）；
                 * STREAM 期间照常生效（同句柄，MP3 音量可用） */
                esp_codec_dev_set_out_vol(spk_codec_dev, cmd.value);
            }
            xSemaphoreGive(audio_mux);
            break;
        case AUDIO_CMD_STOP:
            /* 空闲态下收到 stop 无操作（播放态在循环内消费） */
            break;
        }
    }
}
