/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_push_service.c
 * @brief A2DP Source 推送服务（方案 B「延迟常驻」）实现。
 *
 * 数据流：io_file(MP3) -> aud_dec -> aud_asrc -> aud_enc(SBC) -> io_bt
 *
 * ── 为什么要方案 B ────────────────────────────────────────────────
 * 上一版 BTPUSH 的失败链路（详见 docs/项目规划/蓝牙本地推流方案B-延迟常驻.md）：
 *   1) pipeline 懒加载太晚（LCD/WiFi/摄像头已把内部 RAM 切碎）；
 *   2) 在 BT 事件线程上做毫秒级多块分配 + 建 task；
 *   3) bluedroid 的 BTU/BTC 静态队列与 HCI mempool 是开机时按当时堆布局摆的，
 *      之后 heap 一变就把它们的假设踩烂 → assert。
 *
 * ── 方案 B 的四条原则 ───────────────────────────────────────────
 *  P1 时机前置：pool/pipeline/task 全部在 bt_push_service_early_init() 里建，
 *     抢在 bsp_display_start() 之前（与 bt_audio_service_controller_init 同窗口）。
 *  P2 显式外置：GMF task 栈 stack_in_ext=true 进 PSRAM；uri/weight 缓冲用
 *     heap_caps_malloc(MALLOC_CAP_SPIRAM)，不在内部 RAM 上再啃一口。
 *  P3 建一次不毁：run/stop/reset 反复用同一个 pipeline+task；
 *     任何路径都不在 BT 事件回调里 destroy。
 *  P4 销毁异步：request_destroy() 只置标志 + 唤醒 reaper 任务，
 *     真正 destroy 在独立任务上下文里做。
 *
 * ── 关键：所有会阻塞的 pipeline 操作都走命令队列 ─────────────────
 * stream 事件回调（BT/GMF 线程）只做「登记 + reconfig」这类非阻塞动作；
 * set_in_uri / loading_jobs / run / stop / reset 属阻塞语义（内部可能等
 * DEFAULT_TASK_OPT_MAX_TIME_MS），一律投递到 s_cmd_queue 由 s_proc_task
 * 串行执行（与官方 bt_audio 例程 stream_proc_task 的做法一致）。
 */

#include "bt_push_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <math.h>   /* ★ 数字音量换算：log10f / lroundf */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"
#include "esp_gmf_element.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_io_file.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_err.h"
#include "esp_gmf_alc.h"   /* ★ 数字音量：ALC 元素的增益接口 */

#include "esp_bt_audio_media.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_defs.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "media_lib_adapter.h"

static const char *TAG = "BT_PUSH";

#define URI_LEN                  256
#define ASRC_MAX_CH              4
#define ASRC_MAX_WEIGHT_LEN      (ASRC_MAX_CH * ASRC_MAX_CH)

#define CMD_QUEUE_SIZE           8
#define PROC_TASK_STACK          4096
#define PROC_TASK_PRIO           20
#define PROC_TASK_CORE           1
#define REAPER_TASK_STACK        3072
#define REAPER_TASK_PRIO         5
#define REAPER_TASK_CORE         0

/* 命令类型：全部在 s_proc_task 上下文里执行 */
typedef enum {
    CMD_PREPARE = 0,   /* set_in_uri + loading_jobs */
    CMD_RUN,
    CMD_STOP_RESET,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_DESTROY,       /* 由 reaper 语义承载的重活，投给本任务串行做 */
} push_cmd_action_t;

typedef struct {
    push_cmd_action_t action;
} push_cmd_t;

/* ---- 常驻对象（本文件唯一的一组全局对象，开机建一次） ---- */
static esp_gmf_pool_handle_t       s_pool  = NULL;
static esp_gmf_pipeline_handle_t   s_pipe  = NULL;
static esp_gmf_task_handle_t       s_task  = NULL;
static esp_bt_audio_stream_handle_t s_stream = NULL;

static QueueHandle_t s_cmd_queue = NULL;

/* uri 与 asrc 权重放 PSRAM，避免占内部 RAM（P2） */
static char  *s_uri = NULL;
static float *s_asrc_weight = NULL;

static volatile bool s_pipe_ready = false;    /* pool+pipe+task 建好 */
static volatile bool s_playing = false;       /* 正在推流 */
static volatile bool s_uri_pending = false;   /* 有待下发的 uri */
static volatile bool s_finished = false;      /* ★ 本曲已自然播完，待上层收尾 */
static void (*s_finished_cb)(void) = NULL;    /* ★ 播完回调（在 GMF 事件线程触发） */
static bool s_media_registered = false;

/* ★ 数字音量（2026-09-22）
 * 背景：本工程推流路径是 SD→解码→ASRC→SBC 编码→蓝牙，**完全不经过本地 codec**，
 *   所以"改本地 codec 音量"对耳机毫无作用；而耳机侧又不接受 AVRCP 音量指令
 *   （vol_ops 整张表从未注册，绝对/相对都返回 ESP_ERR_INVALID_STATE）。
 * 出路：我们是 A2DP 源，**编码前的 PCM 完全由我们掌控** —— 在 aud_asrc 与 aud_enc
 *   之间插一个 ALC 元素做数字衰减，耳机必然听得到，与它支不支持 AVRCP 无关。
 * 代价：低音量时数字衰减使 SBC 有效位数变少，音质略有损失。 */
static esp_gmf_element_handle_t s_alc_el = NULL;   /* pipeline 内的 aud_alc 实例 */
static int                      s_vol    = 100;    /* 本地档位 0..100，默认满幅 */

/* 本地档位(0..100) → dB 增益。对数律贴合听感：
 *   100 -> 0 dB（不衰减）· 50 -> -6 dB · 25 -> -12 dB · 0 -> -64 dB（听不见）
 * ★ 用 -64 而不是"低于 -64"来静音：头文件写明支持范围就是 [-64, 63]，
 *   取下界比依赖"低于下界即 mute"的隐式行为更安全。 */
static int8_t vol_to_gain_db(int vol)
{
    if (vol <= 0) {
        return -64;
    }
    if (vol >= 100) {
        return 0;
    }
    float db = 20.0f * log10f((float)vol / 100.0f);
    if (db < -64.0f) {
        db = -64.0f;
    }
    return (int8_t)lroundf(db);
}

/* 把档位下发到 ALC 元素。
 * ★ 必须**逐声道**下发：ALC 是 per-channel 增益，只设 ch0 会造成左右不平衡。
 *   声道数取元素配置（由上游音频信息协商而来）；首次尚未协商时即初始值 2。
 * ★ 元素句柄常驻（P3：建一次不毁），未推流时调用也安全 ——
 *   ALC 先把增益存着，内部句柄 reopen 时会自动重新套用（见 esp_gmf_alc.c）。 */
static void alc_apply_gain(int vol)
{
    if (!s_alc_el) {
        return;
    }
    esp_ae_alc_cfg_t *cfg = (esp_ae_alc_cfg_t *)OBJ_GET_CFG(s_alc_el);
    uint8_t nch = (cfg && cfg->channel >= 1) ? cfg->channel : 2;
    if (nch > 2) {
        nch = 2;   /* 本链路最多立体声（ASRC dest_ch ≤ 2） */
    }
    int8_t db = vol_to_gain_db(vol);
    for (uint8_t i = 0; i < nch; i++) {
        esp_gmf_alc_set_gain(s_alc_el, i, db);
    }
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void register_media_defaults(void)
{
    if (s_media_registered) {
        return;
    }
    media_lib_add_default_adapter();
    esp_audio_dec_register_default();
    esp_audio_enc_register_default();
    s_media_registered = true;
}

static esp_audio_type_t codec_to_audio_type(esp_bt_audio_stream_codec_type_t t)
{
    switch (t) {
    case ESP_BT_AUDIO_STREAM_CODEC_SBC: return ESP_AUDIO_TYPE_SBC;
    case ESP_BT_AUDIO_STREAM_CODEC_AAC: return ESP_AUDIO_TYPE_AAC;
    case ESP_BT_AUDIO_STREAM_CODEC_LC3: return ESP_AUDIO_TYPE_LC3;
    default:                            return ESP_AUDIO_TYPE_UNSUPPORT;
    }
}

/* 按 src_ch -> dest_ch 生成 asrc 混音权重矩阵（与官方实现同构） */
static void fill_asrc_weight(float *weight, uint8_t src_ch, uint8_t dest_ch)
{
    if (!weight || src_ch == 0 || dest_ch == 0 ||
        src_ch > ASRC_MAX_CH || dest_ch > ASRC_MAX_CH) {
        return;
    }
    uint32_t len = (uint32_t)src_ch * dest_ch;
    memset(weight, 0, len * sizeof(weight[0]));

    for (uint8_t dest = 0; dest < dest_ch; dest++) {
        if (src_ch == dest_ch) {
            weight[dest * src_ch + dest] = 1.0f;
        } else if (src_ch == 1) {
            weight[dest] = 1.0f;                       /* 单声道 -> 多声道：复制 */
        } else if (dest_ch == 1) {
            for (uint8_t src = 0; src < src_ch; src++) {
                weight[src] = 1.0f / src_ch;           /* 多声道 -> 单声道：平均 */
            }
        } else {
            uint8_t pick = (dest < src_ch) ? dest : (src_ch - 1);
            weight[dest * src_ch + pick] = 1.0f;       /* 取前 N 路 */
        }
    }
}

/* 在 pipeline 里找第 index 个 aud_asrc 元素 */
static esp_gmf_element_handle_t find_asrc(uint8_t index)
{
    if (!s_pipe) {
        return NULL;
    }
    const void *iter = NULL;
    esp_gmf_element_handle_t el = NULL;
    uint8_t cnt = 0;
    while (esp_gmf_pipeline_iterate_element(s_pipe, &iter, &el) == ESP_GMF_ERR_OK) {
        char *tag = NULL;
        esp_gmf_obj_get_tag((esp_gmf_obj_handle_t)el, &tag);
        if (tag && strcasecmp(tag, "aud_asrc") == 0) {
            if (cnt == index) {
                return el;
            }
            cnt++;
        }
    }
    return NULL;
}

/* 配置 asrc 的目标采样率/声道及混音矩阵 */
static void set_asrc_dest(uint8_t index, uint32_t sample_rate, uint8_t src_ch, uint8_t dest_ch)
{
    esp_gmf_element_handle_t asrc = find_asrc(index);
    if (!asrc) {
        ESP_LOGW(TAG, "aud_asrc[%u] not found", index);
        return;
    }
    esp_gmf_asrc_set_dest_rate(asrc, sample_rate);
    esp_gmf_asrc_set_dest_ch(asrc, dest_ch);

    fill_asrc_weight(s_asrc_weight, src_ch, dest_ch);
    esp_asrc_cfg_t *cfg = (esp_asrc_cfg_t *)OBJ_GET_CFG(asrc);
    if (cfg) {
        cfg->weight = s_asrc_weight;
        cfg->weight_len = (uint32_t)src_ch * dest_ch;
    }
}

/* ------------------------------------------------------------------ */
/* 命令队列                                                            */
/* ------------------------------------------------------------------ */

static bool post_cmd(push_cmd_action_t action)
{
    if (!s_cmd_queue) {
        return false;
    }
    push_cmd_t cmd = { .action = action };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, drop action=%d", action);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* pipeline 事件回调                                                   */
/* ------------------------------------------------------------------ */

static esp_gmf_err_t pipe_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (!pkt || pkt->type != ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        return ESP_GMF_ERR_OK;
    }
    if (pkt->sub == ESP_GMF_EVENT_STATE_FINISHED) {
        ESP_LOGI(TAG, "playback finished");
        s_playing = false;
        /* ★ 2026-09-22 修复"播完不退出"：
           此前只置 s_playing=false 就完事，A2DP 流仍保持 STARTED ——
           设备端持续以为在播，session 也不释放。
           现在通知上层收尾（media_stop + session_end + UI 复位）。
           ★ 本函数跑在 GMF 事件线程，绝不能直接调阻塞的 media_stop，
             只置标志 + 回调；上层负责投递到安全上下文执行。 */
        s_finished = true;
        if (s_finished_cb) {
            s_finished_cb();
        }
    } else if (pkt->sub == ESP_GMF_EVENT_STATE_ERROR) {
        ESP_LOGE(TAG, "pipeline error");
        s_playing = false;
        s_finished = true;   /* 出错同样需要收尾，避免卡在"以为在播" */
        if (s_finished_cb) {
            s_finished_cb();
        }
    }
    return ESP_GMF_ERR_OK;
}

/* ------------------------------------------------------------------ */
/* 处理任务：串行执行所有阻塞的 pipeline 操作                          */
/* ------------------------------------------------------------------ */

static void proc_task(void *arg)
{
    (void)arg;
    push_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_pipe) {
            continue;
        }

        switch (cmd.action) {
        case CMD_PREPARE:
            if (s_uri_pending) {
                s_uri_pending = false;
                ESP_LOGI(TAG, "set uri: %s", s_uri);
                esp_gmf_pipeline_set_in_uri(s_pipe, s_uri);
                esp_gmf_pipeline_loading_jobs(s_pipe);
            }
            break;

        case CMD_RUN:
            ESP_LOGI(TAG, "pipeline run");
            esp_gmf_pipeline_run(s_pipe);
            /* ★ run 之后元素才拿到真实音频参数（采样率/声道数）并可能 reopen
               内部句柄，这里再下发一次音量，确保新开的 ALC 实例带上正确增益。 */
            alc_apply_gain(s_vol);
            break;

        case CMD_STOP_RESET:
            ESP_LOGI(TAG, "pipeline stop+reset");
            esp_gmf_pipeline_stop(s_pipe);
            esp_gmf_pipeline_reset(s_pipe);
            break;

        case CMD_PAUSE:
            esp_gmf_pipeline_pause(s_pipe);
            break;

        case CMD_RESUME:
            esp_gmf_pipeline_resume(s_pipe);
            break;

        case CMD_DESTROY:
            /* 只有走到这里才真正销毁（P4） */
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 开机早期：一次性建好 pool / pipeline / task                         */
/* ------------------------------------------------------------------ */

void bt_push_service_early_init(void)
{
    if (s_pipe_ready) {
        return;
    }

    ESP_LOGI(TAG, "early init: free internal=%d largest=%d free PSRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    register_media_defaults();

    /* uri / weight 放 PSRAM（P2） */
    s_uri = heap_caps_malloc(URI_LEN, MALLOC_CAP_SPIRAM);
    s_asrc_weight = heap_caps_calloc(ASRC_MAX_WEIGHT_LEN, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_uri || !s_asrc_weight) {
        ESP_LOGE(TAG, "PSRAM alloc failed (uri=%p weight=%p)", s_uri, s_asrc_weight);
        goto fail;
    }

    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool init failed");
        goto fail;
    }

    /* 注册元素到 pool：aud_dec / aud_asrc / aud_enc */
    esp_gmf_element_handle_t el = NULL;
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
    };
    if (esp_gmf_audio_dec_init(&dec_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_dec init failed");
        goto fail;
    }

    esp_asrc_cfg_t asrc_cfg = DEFAULT_ESP_GMF_ASRC_CONFIG();
    if (esp_gmf_asrc_init(&asrc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_asrc init failed");
        goto fail;
    }

    /* ★ 数字音量元素：aud_alc 插在 asrc 与 enc 之间，
       作用于"已重采样到耳机协商参数"的 PCM，最省算力且不受编码器影响。
       这里的配置只是初值，真实采样率/声道数由上游音频信息协商后自动更新。 */
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_alc init failed");
        goto fail;
    }

    esp_audio_enc_config_t enc_cfg = {
        .type = ESP_AUDIO_TYPE_SBC,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    if (esp_gmf_audio_enc_init(&enc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_enc init failed");
        goto fail;
    }

    /* 注册 IO：io_file（输入） */
    esp_gmf_io_handle_t io = NULL;
    file_io_cfg_t file_cfg = FILE_IO_CFG_DEFAULT();
    file_cfg.dir = ESP_GMF_IO_DIR_READER;
    if (esp_gmf_io_file_init(&file_cfg, &io) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_io(s_pool, io, NULL);
    } else {
        ESP_LOGE(TAG, "io_file init failed");
        goto fail;
    }

    /* 注册 IO：io_bt（输出） */
    bt_io_cfg_t bt_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    if (esp_gmf_io_bt_init(&bt_cfg, &io) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_io(s_pool, io, NULL);
    } else {
        ESP_LOGE(TAG, "io_bt init failed");
        goto fail;
    }

    /* 建 pipeline：io_file -> aud_dec -> aud_asrc -> aud_alc -> aud_enc -> io_bt */
    const char *el_names[] = {"aud_dec", "aud_asrc", "aud_alc", "aud_enc"};
    if (esp_gmf_pool_new_pipeline(s_pool, "io_file", el_names, 4, "io_bt", &s_pipe) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "new_pipeline failed");
        goto fail;
    }
    esp_gmf_pipeline_set_event(s_pipe, pipe_event_cb, NULL);

    /* ★ 取管内 aud_alc 实例句柄（音量控制要用），并把已存档位下发一次。
       ALC 元素随 pipeline 常驻（P3：建一次不毁），故句柄长期有效 ——
       这也意味着**未推流时也能设音量**，增益会被元素保存、推流时自然生效。 */
    if (esp_gmf_pipeline_get_el_by_name(s_pipe, "aud_alc", &s_alc_el) != ESP_GMF_ERR_OK
        || s_alc_el == NULL) {
        s_alc_el = NULL;
        ESP_LOGW(TAG, "aud_alc not in pipeline; digital volume unavailable");
    } else {
        alc_apply_gain(s_vol);
    }

    /* 建 GMF task：栈进 PSRAM（P2），核 1，prio 15 */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.core = 1;
    task_cfg.thread.stack = 5120;
    task_cfg.thread.prio = 15;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "local2bt_task";
    if (esp_gmf_task_init(&task_cfg, &s_task) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "task init failed");
        goto fail;
    }
    esp_gmf_pipeline_bind_task(s_pipe, s_task);

    /* 处理任务：所有阻塞 pipeline 操作都在这里串行跑 */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(push_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "cmd queue create failed");
        goto fail;
    }
    if (xTaskCreatePinnedToCoreWithCaps(proc_task, "bt_push_proc", PROC_TASK_STACK, NULL,
                                        PROC_TASK_PRIO, NULL, PROC_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "proc task create failed");
        goto fail;
    }

    s_pipe_ready = true;
    ESP_LOGI(TAG, "ready (early, resident): internal free=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return;

fail:
    /* 蓝牙非必需功能：失败只打日志，不 abort */
    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    if (s_task) {
        esp_gmf_task_deinit(s_task);
        s_task = NULL;
    }
    if (s_pipe) {
        esp_gmf_pipeline_destroy(s_pipe);
        s_pipe = NULL;
    }
    if (s_pool) {
        esp_gmf_pool_deinit(s_pool);
        s_pool = NULL;
    }
    if (s_uri) {
        heap_caps_free(s_uri);
        s_uri = NULL;
    }
    if (s_asrc_weight) {
        heap_caps_free(s_asrc_weight);
        s_asrc_weight = NULL;
    }
    ESP_LOGE(TAG, "early init FAILED, BT push disabled");
}

/* ------------------------------------------------------------------ */
/* 对外控制接口                                                        */
/* ------------------------------------------------------------------ */

esp_err_t bt_push_service_set_uri(const char *path)
{
    if (!path || !s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* io_file 需要 file:// 前缀 */
    snprintf(s_uri, URI_LEN, "file://%s", path);
    s_uri_pending = true;
    s_finished = false;   /* ★ 新一曲开始：清掉上一曲的播完标志 */
    ESP_LOGI(TAG, "uri queued: %s", s_uri);

    /* 触发 A2DP 流；真正的 pipeline 准备在 stream ALLOCATED 事件里做 */
    esp_err_t ret = esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
    ESP_LOGI(TAG, "media_start ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bt_push_service_pause(void)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_cmd(CMD_PAUSE) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_push_service_resume(void)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_cmd(CMD_RESUME) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_push_service_stop(void)
{
    if (!s_pipe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    s_playing = false;
    esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    return post_cmd(CMD_STOP_RESET) ? ESP_OK : ESP_FAIL;
}

bool bt_push_service_is_playing(void)
{
    return s_playing;
}

/* ★ 2026-09-22：播完（或出错）收尾支持 */
void bt_push_service_set_finished_cb(void (*cb)(void))
{
    s_finished_cb = cb;
}

bool bt_push_service_is_finished(void)
{
    return s_finished;
}

void bt_push_service_clear_finished(void)
{
    s_finished = false;
}

/* ★ 数字音量入口（2026-09-22）：把本地档位(0..100)转成编码前 PCM 的 dB 增益。
 *   未推流时调用同样有效 —— 增益存在常驻的 ALC 元素里，推流时自动生效。 */
esp_err_t bt_push_service_set_volume(int vol)
{
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_vol = vol;
    if (!s_alc_el) {
        /* pipeline 尚未建好（early_init 失败或还没跑）——
           档位已记下，元素就绪时由 early_init 末尾统一下发。 */
        return ESP_ERR_INVALID_STATE;
    }
    alc_apply_gain(vol);
    ESP_LOGI(TAG, "digital vol %d -> %d dB", vol, (int)vol_to_gain_db(vol));
    return ESP_OK;
}

void bt_push_service_request_destroy(void)
{
    /* P4：这里只投递命令，销毁动作在 proc_task 上下文里做 */
    if (s_pipe_ready) {
        post_cmd(CMD_STOP_RESET);
    }
    ESP_LOGI(TAG, "destroy requested (pipeline stays resident by design)");
}

/* ------------------------------------------------------------------ */
/* stream 状态事件（跑在 BT/GMF 线程）                                 */
/* ------------------------------------------------------------------ */

void bt_push_service_on_stream_state(void *stream, int state)
{
    if (!s_pipe_ready || !s_pipe || !stream) {
        return;
    }
    esp_bt_audio_stream_handle_t st = (esp_bt_audio_stream_handle_t)stream;

    switch (state) {
    case ESP_BT_AUDIO_STREAM_STATE_ALLOCATED: {
        s_stream = st;
        ESP_LOGI(TAG, "stream ALLOCATED, prepare pipeline");

        /* 绑 BT stream 到 io_bt（非阻塞，可在此做） */
        esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(s_pipe), st);

        /* 取耳机协商的 codec 参数 */
        esp_bt_audio_stream_codec_info_t ci = {0};
        if (esp_bt_audio_stream_get_codec_info(st, &ci) != ESP_OK) {
            ESP_LOGE(TAG, "get codec info failed");
            return;
        }
        uint8_t ch = (uint8_t)__builtin_popcount(ci.channels);
        if (ch == 0) {
            ch = 2;
        }
        ESP_LOGI(TAG, "codec type=%d sr=%d ch=%d bits=%d",
                 ci.codec_type, ci.sample_rate, ch, ci.bits);

        /* asrc：把源（MP3 解码输出，这里按立体声 44.1k 交底）转到耳机协商参数 */
        set_asrc_dest(0, ci.sample_rate, 2, ch);

        /* ★ 声道数可能随本曲的 codec 协商结果变化，按新声道数补一次增益 */
        alc_apply_gain(s_vol);

        /* encoder：耳机协商的 codec（SBC） */
        esp_audio_enc_config_t enc_cfg = {
            .type = codec_to_audio_type(ci.codec_type),
            .cfg = ci.codec_cfg,
            .cfg_sz = ci.cfg_size,
        };
        esp_gmf_audio_enc_reconfig(ESP_GMF_PIPELINE_GET_LAST_ELEMENT(s_pipe), &enc_cfg);

        /* set_in_uri + loading_jobs 走命令队列（可能阻塞） */
        post_cmd(CMD_PREPARE);
        break;
    }

    case ESP_BT_AUDIO_STREAM_STATE_STARTED:
        ESP_LOGI(TAG, "stream STARTED");
        s_playing = true;
        post_cmd(CMD_RUN);
        break;

    case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
        ESP_LOGI(TAG, "stream STOPPED");
        s_playing = false;
        post_cmd(CMD_STOP_RESET);
        break;

    case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
        ESP_LOGI(TAG, "stream RELEASED");
        s_playing = false;
        if (s_stream == st) {
            s_stream = NULL;
        }
        /* 不 destroy pipeline（P3/P4） */
        break;

    default:
        break;
    }
}
