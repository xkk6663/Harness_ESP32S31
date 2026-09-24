/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
/* ★ F35：任务栈外置 PSRAM 需要这两个头
   - idf_additions.h → xTaskCreateWithCaps / xTaskCreatePinnedToCoreWithCaps
   - esp_heap_caps.h → MALLOC_CAP_SPIRAM / MALLOC_CAP_8BIT */
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "audio_service.h"
#include "3rd/ringbuf/ringbuf.h"
#include "middleware/bluetooth/bt_sink_service.h"   /* audio_yield（16k 会话仲裁，§28.44） */

static const char *TAG = "AUDIO_SVC";

/* ---- 内部状态 ---- */
static esp_codec_dev_handle_t spk_codec_dev = NULL;
#if BSP_CAPS_AUDIO_MIC
static esp_codec_dev_handle_t mic_codec_dev = NULL;
#endif
static SemaphoreHandle_t audio_mux;
static QueueHandle_t    audio_cmd_queue;
static audio_evt_cb_t   audio_evt_cb_fn;

/* 播放/录音工作缓冲：用第三方 ringbuf 库管理（main/3rd/ringbuf） */
static uint8_t     play_rb_pool[BUFFER_SIZE];
static ringbuf_t   play_rb;
static uint8_t     rec_rb_pool[BUFFER_SIZE];
static ringbuf_t   rec_rb;

/* 播放内部状态（不再对外暴露全局变量） */
static bool play_repeat = false;

/* Stream buffers deliberately live in internal static RAM.  ringbuf has a
 * 14-bit index, so no pool may exceed 16383 bytes. */
static uint8_t mic_tx_rb_pool[5120];
static uint8_t spk_rx_rb_pool[3840];
static ringbuf_t mic_tx_rb;
static ringbuf_t spk_rx_rb;
static SemaphoreHandle_t mic_rb_mux;
static SemaphoreHandle_t spk_rb_mux;
static TaskHandle_t stream_task_handle;
static volatile bool stream_active;

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
static void audio_stream_task(void *arg);

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

    /* 初始化第三方 ringbuf（pool 为静态数组，无需动态分配） */
    ringbuffer_init(&play_rb, play_rb_pool, BUFFER_SIZE);
    ringbuffer_init(&rec_rb,  rec_rb_pool,  BUFFER_SIZE);
    ringbuffer_init(&mic_tx_rb, mic_tx_rb_pool, sizeof(mic_tx_rb_pool));
    ringbuffer_init(&spk_rx_rb, spk_rx_rb_pool, sizeof(spk_rx_rb_pool));
    mic_rb_mux = xSemaphoreCreateMutex();
    spk_rb_mux = xSemaphoreCreateMutex();
    assert(mic_rb_mux && spk_rb_mux);

    /* 常驻音频任务（栈 8KB：WAV 头 + 采样信息 + 命令缓冲都在栈帧里）
       ★ 栈外置 PSRAM（F35）：只在 CPU 上压栈，不需要 DMA 可达 */
    xTaskCreateWithCaps(audio_task, "audio_task", 8192, NULL, 6, NULL,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

/* ------------------------------------------------------------------ */
/* ★ F42：共享 I2S data 接口的 enable 位强制复位                        */
/* ------------------------------------------------------------------ */

/* 背景（2026-09-24 实机日志）：BT_SINK 让位成功后，16k 会话的 set_fmt 仍然报
 *     E I2S_IF: Current mode record   conflict 16000 with peer mode 44100
 *     E I2S_IF: Current mode playback conflict 16000 with peer mode 44100
 * ⇒ I2S 停在 44.1k，16k 的 PCM 灌进 44.1k 管道 ⇒ 通话下行（扬声器）听不到。
 *
 * 根因不止在 BT：BSP 的 bsp_audio_codec_speaker_init() / microphone_init()
 * **每调用一次就新建一个** esp_codec_dev 对象，而本工程有 5 个这样的对象
 * （audio_service 的 spk/mic、bt_sink_service、tab_audio、tab_video）**共用 BSP 里
 * 同一个 static i2s_data_if**。于是：
 *     in_enable / out_enable / fs   →  共享（在 i2s_data_t 里）
 *     input_opened / output_opened  →  每个 esp_codec_dev 对象私有
 * 只要出现一次非对称的 open/close（A 对象 open 了 44.1k，B 对象关闭时因自己
 * output_opened==false 被 esp_codec_dev_close() 提前返回、没走到 data_if->enable(false)），
 * 共享的 enable 位就会与对象状态**永久脱节**。
 * 而 check_fs_compatible() 的"快速分支"（audio_codec_data_i2s.c:433）要求
 * **对端通道 enable==false** 才把新 fs 直接写进去；脱节的 enable 位会让它走冲突分支
 * 返回 NOT_SUPPORT，且 esp_codec_dev_open() **不检查 set_fmt 的返回值**
 * （esp_codec_dev.c:178）⇒ 冲突被完全静默，open 照样打印 "Open codec device OK"。
 *
 * 解法：跨采样率会话开始前，不依赖任何 esp_codec_dev 对象的状态，直接对共享 data_if
 * 把两路都 disable（幂等），让接下来的 open 必定走快速分支。
 * ⚠ 结构性隐患仍在（5 个对象共享一份 enable 状态）：后续应统一为单一 codec 实例
 *   （codec_bus 单例）。详见 docs/工程规范/11_遇到的问题.md F42。 */
static void force_reset_shared_i2s(void)
{
    const audio_codec_data_if_t *itf = bsp_audio_get_codec_itf();
    if (itf == NULL || itf->enable == NULL) {
        return;
    }
    /* 顺序要紧：先 OUT 再 IN。S3 系列 I2S 在"RX 仍开着"时 disable TX 会被挂成
       out_disable_pending（audio_codec_data_i2s.c:565+），先关 OUT 可避开这条分支。 */
    int r_out = itf->enable(itf, ESP_CODEC_DEV_TYPE_OUT, false);
    int r_in = itf->enable(itf, ESP_CODEC_DEV_TYPE_IN, false);
    ESP_LOGI(TAG, "shared I2S force-released (out=%d in=%d)", r_out, r_in);
}

esp_err_t audio_stream_start(void)
{
#if !BSP_CAPS_AUDIO_MIC
    return ESP_ERR_NOT_SUPPORTED;
#else
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    if (stream_active) {
        xSemaphoreGive(audio_mux);
        return ESP_OK;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    /* ★ 会话仲裁（§28.44）：BT SINK 以 44.1k 打开 codec 且"只开一次"不关，
     *   本函数的 16k open 会走 check_fs_compatible 冲突分支、且 esp_codec_dev
     *   静默忽略 set_fmt 失败（open 仍返 OK）→ I2S 停在 44.1k → 16k 单声道
     *   数据灌进 44.1k 立体声管道 → 扬声器无声/变调。先让 BT 交出 codec。 */
    bt_sink_service_audio_yield();

    /* ★ F42：BT 交还 codec 只保证它自己那条路。共享 I2S 的 enable 位仍可能被别的
       OUT 对象（audio_service.spk / tab_audio / tab_video）留成 true。这里做两步归位：
       ① close 本会话要用的两个句柄 —— esp_codec_dev_open() 在
          (input_opened||output_opened) 时会**提前返回**：既不 set_fmt 也不 enable；
          若上次的关闭被 esp_codec_dev_close() 的提前返回吃掉，句柄就会带着
          opened=true 进来，导致本次 16k 配置根本不生效。先 close（幂等，无害）保证
          下面两个 open 一定走完整路径。
       ② 强制复位共享 I2S 两路的 enable 位，让 check_fs_compatible 快分支必定命中。 */
    esp_codec_dev_close(mic_codec_dev);
    esp_codec_dev_close(spk_codec_dev);
    force_reset_shared_i2s();

    esp_err_t err = esp_codec_dev_open(mic_codec_dev, &fs);
    if (err == ESP_OK) {
        err = esp_codec_dev_open(spk_codec_dev, &fs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream codec open failed, err=%s", esp_err_to_name(err));
        esp_codec_dev_close(mic_codec_dev);
        xSemaphoreGive(audio_mux);
        return err;
    }
    stream_active = true;
    ringbuffer_reset(&mic_tx_rb);
    ringbuffer_reset(&spk_rx_rb);
    /* ★ 栈外置 PSRAM（F35）：stream 任务栈里是 20ms 采样的临时帧，
       纯 CPU 访问，不需要 DMA 可达。TCB 仍强制落内部（IDF 行为）。 */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(audio_stream_task, "audio_stream", 8192,
                                                    NULL, 8, &stream_task_handle, 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "stream task create failed (stack->PSRAM); check "
                      "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY");
        stream_active = false;
        esp_codec_dev_close(mic_codec_dev);
        esp_codec_dev_close(spk_codec_dev);
        xSemaphoreGive(audio_mux);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(audio_mux);
    ESP_LOGI(TAG, "stream started, 16k/mono/20ms core=1 (rev F42: shared I2S reset)");
    return ESP_OK;
#endif
}

void audio_stream_stop(void)
{
    if (!stream_active) {
        return;
    }
    stream_active = false;
    if (stream_task_handle) {
        xTaskNotifyGive(stream_task_handle);
    }
}

bool audio_stream_is_active(void)
{
    return stream_active;
}

size_t audio_stream_read_mic(uint8_t *buf, size_t len)
{
    if (!buf || !len || !mic_rb_mux) {
        return 0;
    }
    xSemaphoreTake(mic_rb_mux, portMAX_DELAY);
    uint16_t n = ringbuffer_get(&mic_tx_rb, buf, (uint16_t)MIN(len, UINT16_MAX));
    xSemaphoreGive(mic_rb_mux);
    return n;
}

size_t audio_stream_write_spk(const uint8_t *buf, size_t len)
{
    if (!buf || !len || !spk_rb_mux) {
        return 0;
    }
    xSemaphoreTake(spk_rb_mux, portMAX_DELAY);
    /* More than six queued frames means latency is growing: retain newest. */
    while (ringbuffer_data_len(&spk_rx_rb) + len > 6 * AUDIO_FRAME_BYTES) {
        static uint8_t discard[AUDIO_FRAME_BYTES];
        ringbuffer_get(&spk_rx_rb, discard, AUDIO_FRAME_BYTES);
    }
    uint16_t n = ringbuffer_put_force(&spk_rx_rb, buf, (uint16_t)MIN(len, UINT16_MAX));
    xSemaphoreGive(spk_rb_mux);
    return n;
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
    static uint8_t io_tmp[BUFFER_SIZE];
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
    static uint8_t rec_tmp[BUFFER_SIZE];
    while (written < RECORDING_LENGTH * BUFFER_SIZE && !stopped) {
        ESP_ERROR_CHECK(esp_codec_dev_read(mic_codec_dev, rec_tmp, BUFFER_SIZE));
        /* codec 读进 ringbuf，再读出落盘 */
        ringbuffer_put(&rec_rb, rec_tmp, BUFFER_SIZE);
        uint16_t got = ringbuffer_get(&rec_rb, rec_tmp, BUFFER_SIZE);
        written += fwrite(rec_tmp, 1, got, f);

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
            if (!stream_active) {
                handle_play_file(cmd.path);
            } else {
                ESP_LOGW(TAG, "ignore playback while stream is active");
            }
            break;
        case AUDIO_CMD_START_RECORD:
            if (!stream_active) {
                handle_record(cmd.path);
            } else {
                ESP_LOGW(TAG, "ignore legacy record while stream is active");
            }
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

/* One Core1 task owns codec I/O while networking only accesses the two
 * ringbuffers.  This keeps UDP/TCP code independent of ES8389/I2S. */
static void audio_stream_task(void *arg)
{
    (void)arg;
#if BSP_CAPS_AUDIO_MIC
    static uint8_t mic_frame[AUDIO_FRAME_BYTES];
    static uint8_t spk_frame[AUDIO_FRAME_BYTES];
    int64_t stat_us = esp_timer_get_time();
    uint32_t frames = 0;
    int64_t rms_acc = 0;
    while (stream_active) {
        esp_err_t err = esp_codec_dev_read(mic_codec_dev, mic_frame, sizeof(mic_frame));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stream mic read failed, err=%s", esp_err_to_name(err));
            memset(mic_frame, 0, sizeof(mic_frame));
        }
        /* mic RMS diagnostics */
        for (int i = 0; i + 1 < (int)sizeof(mic_frame); i += 2) {
            int16_t s = (int16_t)((mic_frame[i + 1] << 8) | mic_frame[i]);
            rms_acc += (int64_t)s * s;
        }
        frames++;
        if (esp_timer_get_time() - stat_us >= 2000000) {
            double rms = frames ? sqrt((double)rms_acc / (frames * (AUDIO_FRAME_BYTES / 2))) : 0;
            ESP_LOGI(TAG, "MIC_RMS=%.0f frames=%u", rms, frames);
            rms_acc = 0; frames = 0; stat_us = esp_timer_get_time();
        }
        xSemaphoreTake(mic_rb_mux, portMAX_DELAY);
        ringbuffer_put_force(&mic_tx_rb, mic_frame, sizeof(mic_frame));
        xSemaphoreGive(mic_rb_mux);

        xSemaphoreTake(spk_rb_mux, portMAX_DELAY);
        uint16_t got = ringbuffer_get(&spk_rx_rb, spk_frame, sizeof(spk_frame));
        xSemaphoreGive(spk_rb_mux);
        if (got < sizeof(spk_frame)) {
            memset(spk_frame + got, 0, sizeof(spk_frame) - got);
        }
        err = esp_codec_dev_write(spk_codec_dev, spk_frame, sizeof(spk_frame));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stream speaker write failed, err=%s", esp_err_to_name(err));
        }
    }
    xSemaphoreTake(audio_mux, portMAX_DELAY);
    esp_codec_dev_close(mic_codec_dev);
    esp_codec_dev_close(spk_codec_dev);
    stream_task_handle = NULL;
    xSemaphoreGive(audio_mux);
    ESP_LOGI(TAG, "stream stopped");
#endif
    vTaskDelete(NULL);
}
