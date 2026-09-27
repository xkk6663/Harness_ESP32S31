/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file media_player_service.c
 * @brief 本地音乐播放服务实现（S5 M2）
 *
 * 链路照搬 main:main/ui/tabs/tab_audio.c 的 LOCAL 模式（同芯片已验证）：
 *   media_lib 默认适配器 + extractor/audio_dec 注册
 *   → gmf pool（ch_cvt/bit_cvt/rate_cvt/alc 四元素）
 *   → esp_audio_render（out_writer 指向本服务）
 *   → esp_player（audio_render stream + 事件回调）
 *
 * 与 main 的唯一实质差异：out_writer 不自建 codec 句柄，改走
 * audio_service ext 流式协议（ext_claim/ext_write/ext_release）。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "middleware/mem_diag.h"

#include "esp_player.h"
#include "esp_audio_render.h"
#include "esp_extractor_defaults.h"   /* esp_extractor_register_default */
#include "esp_audio_dec_default.h"    /* esp_audio_dec_register_default */
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"
#include "media_lib_adapter.h"

#include "media_player_service.h"
#include "audio_service.h"

static const char *TAG = "MP_SVC";

/* ---- 命令 ---- */
typedef enum {
    MP_CMD_PLAY = 0,     /* value=0；path 为待播文件 */
    MP_CMD_STOP,         /* 停止 + release */
    MP_CMD_FINISHED,     /* 播放器事件回调投递：自然播完/出错 → release + 上抛 */
} mp_cmd_id_t;

typedef struct {
    mp_cmd_id_t id;
    char path[256];
} mp_cmd_t;

/* ---- 状态 ---- */
static QueueHandle_t s_cmd_q;
static TaskHandle_t  s_task;
static mp_evt_cb_t   s_evt_cb;

static esp_player_handle_t            s_player;
static esp_audio_render_handle_t      s_render;
static esp_gmf_pool_handle_t          s_pool;
static bool                           s_media_registered;
static volatile bool                  s_playing;
/* ★ pending 段：play() 入队成功到 mp_task 真正把 s_playing 置位之间有 ~50ms
 *   建链窗口（ext_claim + pool/render/player 惰性重建 + run）。这段窗口里
 *   is_playing() 若只看 s_playing，UI 轮询会误判"已结束"而发出误杀 STOP
 *   （2026-09-26 实证）。is_playing() = s_playing || s_play_pending。 */
static volatile bool                  s_play_pending;
/* 播放器事件回调（播放器任务上下文）只置标志；FINISHED 的收尾
 * （esp_player_stop 后置 + ext_release）统一由服务任务在 MP_CMD_FINISHED 做，
 * 避免在播放器自己的回调里调它的 stop（重入风险）。 */
static volatile bool                  s_player_finished;

static const audio_ext_fmt_t MP_OUT_FMT = {
    .sample_rate = 44100, .channels = 2, .bits = 16,
};

/* ---- 前向 ---- */
static int  mp_link_setup(void);
static void mp_link_teardown(void);

static void notify_evt(mp_evt_id_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt);
    }
}

/* ============================================================
 * esp_player 链路
 * ============================================================ */

/* render 输出回调（播放器管线任务上下文）：统一走 ext 流式协议。
 * ★ 返回值契约是 **esp_err_t 语义（0=成功，非 0=错误码）**，不是"写入字节数"！
 *   实证（2026-09-26 MP3 播放不了）：esp_audio_render 各路径——
 *   audio_render_post_writer 转换层按 ret==0 ? OK : FAIL 判定；
 *   audio_render_proc_write 在 proc->pipeline==NULL 时把 writer 返回值**原样透传**，
 *   player_audio_render_process 再按 !=OK 判错 —— 返回正数字节数（如 17640）会被
 *   当成错误码 → PREPARING->PLAYING 一瞬间就 ERROR。
 *   main 分支 tab_audio.c 的 audio_out_write 之所以能跑，是因为它直接
 *   return esp_codec_dev_write(...)（成功返回 0）。 */
static int mp_out_write(uint8_t *data, uint32_t size, void *ctx)
{
    (void)ctx;
    if (!data || !size) {
        return -1;
    }
    return audio_service_ext_write(data, size);
}

static void register_media_defaults(void)
{
    if (s_media_registered) {
        return;
    }
    media_lib_add_default_adapter();       /* file/http IO 适配 */
    esp_extractor_register_default();      /* mp3/m4a/flac 容器解析 */
    esp_audio_dec_register_default();      /* mp3/aac/flac 解码器 */
    s_media_registered = true;
}

static int init_gmf_pool(void)
{
    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "gmf pool init failed");
        return -1;
    }
    esp_gmf_element_handle_t el = NULL;

    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    }
    return 0;
}

static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (!msg) {
        return ESP_PLAYER_ERR_OK;
    }
    if (msg->event_type == ESP_PLAYER_EVENT_FINISHED ||
        msg->event_type == ESP_PLAYER_EVENT_ERROR) {
        if (msg->event_type == ESP_PLAYER_EVENT_ERROR) {
            ESP_LOGE(TAG, "player error event");
        }
        s_player_finished = true;
        mp_cmd_t cmd = { .id = MP_CMD_FINISHED };
        if (s_cmd_q) {
            xQueueSend(s_cmd_q, &cmd, 0);
        }
    }
    return ESP_PLAYER_ERR_OK;
}

static int mp_link_setup(void)
{
    register_media_defaults();
    if (init_gmf_pool() != 0) {
        return -1;
    }

    esp_audio_render_cfg_t render_cfg = {
        .max_stream_num = 1,
        .out_writer = mp_out_write,
        .out_ctx = NULL,                    /* 写入目标由 ext 协议决定 */
        .out_sample_info = {
            .sample_rate = MP_OUT_FMT.sample_rate,
            .channel = MP_OUT_FMT.channels,
            .bits_per_sample = MP_OUT_FMT.bits,
        },
        .pool = s_pool,
        .process_period = 20,
        .process_buf_align = 16,
    };
    if (esp_audio_render_create(&render_cfg, &s_render) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "audio_render create failed");
        return -1;
    }

    esp_audio_render_stream_handle_t stream = NULL;
    if (esp_audio_render_stream_get(s_render, ESP_AUDIO_RENDER_FIRST_STREAM,
                                    &stream) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "render stream get failed");
        return -1;
    }

    esp_player_config_t player_cfg = ESP_PLAYER_CONFIG_DEFAULT();
    player_cfg.audio_render_hd = stream;
    player_cfg.video_render_hd = NULL;

    if (esp_player_init(&player_cfg, &s_player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "player init failed");
        return -1;
    }
    esp_player_set_event_cb(s_player, player_event_cb, NULL);
    ESP_LOGI(TAG, "local player link ready");
    return 0;
}

static void mp_link_teardown(void)
{
    if (s_player) {
        esp_player_stop(s_player);
        esp_player_deinit(s_player);
        s_player = NULL;
    }
    if (s_render) {
        esp_audio_render_destroy(s_render);
        s_render = NULL;
    }
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
}

/* ============================================================
 * 服务任务
 * ============================================================ */

static void mp_task(void *arg)
{
    static mp_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (cmd.id) {
        case MP_CMD_PLAY:
            if (s_playing) {
                /* 重复 PLAY：先停当前（不 release，马上复用 ext 占用） */
                esp_player_stop(s_player);
                s_playing = false;
            }
            /* ext 流式占用：失败 = audio_service 忙（WAV/录音/通话/小智） */
            if (!s_playing &&
                audio_service_ext_claim(&MP_OUT_FMT) != ESP_OK) {
                ESP_LOGW(TAG, "play refused: audio_service busy");
                s_play_pending = false;
                notify_evt(MP_EVT_ERROR);
                break;
            }
            if (!s_player && mp_link_setup() != 0) {
                audio_service_ext_release();
                s_play_pending = false;
                notify_evt(MP_EVT_ERROR);
                break;
            }
            s_player_finished = false;
            {
                esp_player_data_src_t src =
                    ESP_PLAYER_DATA_SRC(cmd.path, ESP_PLAYER_MASK_AUDIO);
                esp_player_set_data_src(s_player, &src);
            }
            if (esp_player_run(s_player) != ESP_PLAYER_ERR_OK) {
                ESP_LOGE(TAG, "player run failed: %s", cmd.path);
                esp_player_stop(s_player);
                audio_service_ext_release();
                s_play_pending = false;
                notify_evt(MP_EVT_ERROR);
                break;
            }
            s_playing = true;
            s_play_pending = false;
            ESP_LOGI(TAG, "playing: %s", cmd.path);
            notify_evt(MP_EVT_STARTED);
            break;

        case MP_CMD_STOP:
            if (s_playing) {
                esp_player_stop(s_player);
                s_playing = false;
                audio_service_ext_release();
                ESP_LOGI(TAG, "stopped by request");
            }
            s_play_pending = false;
            /* ★ 全拆 GMF 链路（pool/render/player）：链路在 PLAY 时惰性重建。
             *   不拆的话整条 esp_player 管线（pool + 任务 + 缓冲）在 STOP 后
             *   一直常驻，MP3 是低频操作，没必要占着内存。 */
            mp_link_teardown();
            notify_evt(MP_EVT_FINISHED);
            break;

        case MP_CMD_FINISHED:
            if (s_playing) {
                /* 播放器已自行停止（FINISHED/ERROR 事件），收尾归还 */
                esp_player_stop(s_player);
                s_playing = false;
                audio_service_ext_release();
                ESP_LOGI(TAG, "finished");
            }
            s_play_pending = false;
            mp_link_teardown();
            notify_evt(MP_EVT_FINISHED);
            break;
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void media_player_service_init(void)
{
    s_cmd_q = xQueueCreate(4, sizeof(mp_cmd_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(4, mp_cmd_t)", 4 * sizeof(mp_cmd_t));
    BaseType_t ok = xTaskCreate(mp_task, "mp_task", 3072, NULL, 5, &s_task);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreate(mp_task, 3072)", 3072);
    ESP_LOGI(TAG, "media_player_service ready (link lazy-init on first play)");
}

esp_err_t media_player_service_play(const char *path)
{
    if (!path || !path[0] || !s_cmd_q) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_cmd_t cmd = { .id = MP_CMD_PLAY };
    if (strlen(path) >= sizeof(cmd.path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(cmd.path, path);
    /* 非阻塞投递；结果通过事件回调（STARTED/ERROR）上抛 */
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdPASS) {
        return ESP_ERR_INVALID_STATE;
    }
    s_play_pending = true;   /* ★ 入队即算"播放中"，堵死建链窗口的误杀竞态 */
    return ESP_OK;
}

void media_player_service_stop(void)
{
    mp_cmd_t cmd = { .id = MP_CMD_STOP };
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

bool media_player_service_is_playing(void)
{
    /* ★ 含 pending 段：见 s_play_pending 注释（UI 轮询防误杀竞态） */
    return s_playing || s_play_pending;
}

void media_player_service_set_evt_cb(mp_evt_cb_t cb)
{
    s_evt_cb = cb;
}
