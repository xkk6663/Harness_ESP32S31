/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_push_service.c
 * @brief A2DP Source local2bt pipeline：MP3 文件 -> SBC -> 蓝牙耳机。
 *
 * 参考官方 bt_audio 例程 stream_proc.c 的 local2bt 部分，只保留 Source 推流。
 */

#include "bt_push_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_err.h"

#include "esp_bt_audio_media.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_defs.h"
#include "media_lib_adapter.h"

static const char *TAG = "BT_PUSH";

#define URI_LEN 256

static esp_gmf_pool_handle_t s_pool = NULL;
static esp_gmf_pipeline_handle_t s_pipe = NULL;
static esp_gmf_task_handle_t s_task = NULL;
static esp_bt_audio_stream_handle_t s_stream = NULL;

static char s_uri[URI_LEN];
static bool s_uri_pending = false;
static bool s_playing = false;
static bool s_media_registered = false;
static bool s_inited = false;

static void register_media_defaults(void)
{
    if (s_media_registered) return;
    media_lib_add_default_adapter();
    s_media_registered = true;
}

static esp_audio_type_t codec_to_audio_type(esp_bt_audio_stream_codec_type_t t)
{
    switch (t) {
    case ESP_BT_AUDIO_STREAM_CODEC_SBC:  return ESP_AUDIO_TYPE_SBC;
    case ESP_BT_AUDIO_STREAM_CODEC_AAC:  return ESP_AUDIO_TYPE_AAC;
    default: return ESP_AUDIO_TYPE_UNSUPPORT;
    }
}

static esp_gmf_err_t pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (pkt && pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        ESP_LOGI(TAG, "pipeline state=%d", pkt->sub);
        if (pkt->sub == ESP_GMF_EVENT_STATE_FINISHED) {
            s_playing = false;
            ESP_LOGI(TAG, "playback finished");
        } else if (pkt->sub == ESP_GMF_EVENT_STATE_ERROR) {
            ESP_LOGE(TAG, "pipeline error");
            s_playing = false;
        }
    }
    return ESP_GMF_ERR_OK;
}

/* 懒加载：第一次 play 时才建 GMF pool/pipeline/task。
   开机就建会占用内部 RAM 并和 bluedroid 连接流程争抢资源导致 assert。 */
static esp_err_t ensure_pipeline(void)
{
    if (s_inited) return ESP_OK;
    register_media_defaults();

    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool init failed");
        return ESP_FAIL;
    }

    const char *el_names[] = {"aud_dec", "aud_asrc", "aud_enc"};
    if (esp_gmf_pool_new_pipeline(s_pool, "io_file", el_names, 3, "io_bt", &s_pipe) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "new_pipeline failed");
        return ESP_FAIL;
    }
    esp_gmf_pipeline_set_event(s_pipe, pipe_event_cb, NULL);

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.thread.core = 1;
    cfg.thread.stack = 5120;
    cfg.thread.prio = 15;
    cfg.thread.stack_in_ext = true;
    cfg.name = "local2bt_task";
    if (esp_gmf_task_init(&cfg, &s_task) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "task init failed");
        return ESP_FAIL;
    }
    esp_gmf_pipeline_bind_task(s_pipe, s_task);
    s_inited = true;
    ESP_LOGI(TAG, "pipeline ready (lazy)");
    return ESP_OK;
}

esp_err_t bt_push_service_init(void)
{
    /* 不在这里建 pipeline，留给第一次 play 时懒加载 */
    ESP_LOGI(TAG, "service init (lazy pipeline)");
    return ESP_OK;
}

esp_err_t bt_push_service_play(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (ensure_pipeline() != ESP_OK || !s_pipe) return ESP_FAIL;

    snprintf(s_uri, sizeof(s_uri), "file://%s", path);
    s_uri_pending = true;
    ESP_LOGI(TAG, "play requested: %s", s_uri);

    /* 触发 A2DP 流；stream ALLOCATED 事件里会绑 io_bt 并 set uri */
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    ESP_LOGI(TAG, "media_start ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bt_push_service_pause(void)
{
    if (!s_pipe) return ESP_ERR_INVALID_STATE;
    esp_gmf_pipeline_pause(s_pipe);
    return ESP_OK;
}

esp_err_t bt_push_service_stop(void)
{
    if (!s_pipe) return ESP_ERR_INVALID_STATE;
    s_playing = false;
    esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    esp_gmf_pipeline_stop(s_pipe);
    esp_gmf_pipeline_reset(s_pipe);
    return ESP_OK;
}

bool bt_push_service_is_playing(void) { return s_playing; }

void bt_push_service_on_stream_state(void *stream, int state)
{
    if (!s_inited || !s_pipe) return;   /* 还没点歌，忽略 */
    esp_bt_audio_stream_handle_t st = (esp_bt_audio_stream_handle_t)stream;
    ESP_LOGI(TAG, "stream state=%d", state);

    switch (state) {
    case ESP_BT_AUDIO_STREAM_STATE_ALLOCATED: {
        s_stream = st;
        /* 绑 BT stream 到 pipeline 输出 io_bt */
        esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(s_pipe), st);

        /* 按耳机协商的 codec 配置 encoder */
        esp_bt_audio_stream_codec_info_t ci = {0};
        esp_bt_audio_stream_get_codec_info(st, &ci);
        ESP_LOGI(TAG, "codec: type=%d sr=%d ch=%d", ci.codec_type, ci.sample_rate, ci.channels);

        /* decoder: MP3 */
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_TYPE_MP3,
            .dec_cfg = NULL,
            .cfg_size = 0,
        };
        esp_gmf_audio_dec_reconfig(s_pipe->head_el, &dec_cfg);

        /* encoder: 耳机协商的 SBC */
        esp_audio_enc_config_t enc_cfg = {
            .type = codec_to_audio_type(ci.codec_type),
            .cfg = ci.codec_cfg,
            .cfg_sz = ci.cfg_size,
        };
        esp_gmf_audio_enc_reconfig(s_pipe->last_el, &enc_cfg);

        if (s_uri_pending) {
            s_uri_pending = false;
            esp_gmf_pipeline_set_in_uri(s_pipe, s_uri);
            esp_gmf_pipeline_loading_jobs(s_pipe);
        }
        break;
    }
    case ESP_BT_AUDIO_STREAM_STATE_STARTED:
        s_playing = true;
        esp_gmf_pipeline_run(s_pipe);
        break;
    case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
        s_playing = false;
        esp_gmf_pipeline_stop(s_pipe);
        esp_gmf_pipeline_reset(s_pipe);
        break;
    case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
        if (s_stream == st) s_stream = NULL;
        break;
    default:
        break;
    }
}
