/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_sink_service.c
 * @brief A2DP Sink 接收服务实现（bt2codec 管线 + AVRCP 元数据/歌词拆分）。
 *
 * 数据流：io_bt(reader) -> aud_dec(SBC) -> aud_asrc -> io_codec_dev(ES8389)
 *
 * ── 与 bt_push_service（Source 方向）的对称关系 ─────────────────────
 *   Source：io_file -> aud_dec -> aud_asrc -> aud_alc -> aud_enc -> io_bt   （本机推出去）
 *   Sink  ：io_bt   -> aud_dec -> aud_asrc -> io_codec_dev                  （对端推进来）
 * 两者共用同一套「方案 B」纪律（见 bt_push_service.c 顶部注释）：
 *  P1 时机前置 / P2 显式外置 / P3 建一次不毁 / P4 销毁异步 + 阻塞操作走命令队列。
 *
 * ── 为什么必须按「流方向」分流 ───────────────────────────────────
 * esp_bt_audio 对 Source 和 Sink 都用同一个 ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG
 * 事件上报。bt_audio_service.c 里用 esp_bt_audio_stream_get_dir() 判断方向后
 * 才把 Sink 方向的流交给本模块 —— 否则 Source 会去 set_in_uri，方向完全反了。
 *
 * ── 线程纪律（本项目铁律）────────────────────────────────────────
 * on_stream_state / on_playback_status / on_metadata 全部跑在 **BT 线程**：
 *   只做「缓存 + 投队列」这类非阻塞动作，**绝不碰 LVGL**。
 * 真正阻塞的 pipeline 操作（set_stream / loading_jobs / run / stop / reset）
 * 一律投 s_cmd_queue，由 s_proc_task 串行执行。
 * 元数据/歌词只写进 s_meta 结构（s_meta_lock 保护），UI 侧 500ms 轮询抽取。
 *
 * ── ★ 2026-09-22：封面链路整条删除 ──────────────────────────────
 * 原封面 JPEG 解码任务 + 双缓冲（2×240×240×3 ≈ 337KB PSRAM）+ 队列全部移除。
 * 依据见 docs/项目规划/改动记录.md §28.28：AVRCP Cover Art 在安卓侧默认关闭，
 * 实测对端从不声明 TG 能力，设备端收不到任何图 —— 留着只是白占 PSRAM 和任务。
 */

#include "bt_sink_service.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"

#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"
#include "esp_gmf_element.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_asrc.h"
#include "esp_gmf_io_bt.h"
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_err.h"
#include "esp_codec_dev.h"      /* esp_codec_dev_open/set_out_vol（io_codec_dev 不会自己开 codec） */

#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_stream.h"
#include "esp_bt_audio_event.h"
#include "esp_bt_audio_playback.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "media_lib_adapter.h"

static const char *TAG = "BT_SINK";

#define SINK_ASRC_MAX_CH       4
#define SINK_ASRC_MAX_WEIGHT   (SINK_ASRC_MAX_CH * SINK_ASRC_MAX_CH)

#define SINK_CMD_QUEUE_SIZE    8
#define SINK_PROC_STACK        4096
#define SINK_PROC_PRIO         20
#define SINK_PROC_CORE         1

/* 本机 DAC 固定输出参数（与本地播放链路 tab_audio.c 一致，避免反复重配 I2S 时钟） */
#define SINK_DAC_RATE          44100
#define SINK_DAC_CH            2
#define SINK_DAC_BITS          16
#define SINK_DAC_VOL           60

/* ------------------------------------------------------------------ */
/* 命令队列                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    SINK_CMD_PREPARE = 0,   /* 开 codec + loading_jobs */
    SINK_CMD_RUN,
    SINK_CMD_STOP_RESET,
} sink_cmd_t;

typedef struct {
    sink_cmd_t action;
} sink_cmd_pkt_t;

/* ------------------------------------------------------------------ */
/* 常驻对象                                                            */
/* ------------------------------------------------------------------ */

static esp_gmf_pool_handle_t       s_pool  = NULL;
static esp_gmf_pipeline_handle_t   s_pipe  = NULL;
static esp_gmf_task_handle_t       s_task  = NULL;
static esp_bt_audio_stream_handle_t s_stream = NULL;
static esp_codec_dev_handle_t      s_codec = NULL;
static QueueHandle_t               s_cmd_queue = NULL;
static float                      *s_asrc_weight = NULL;
static bool                        s_media_registered = false;
static bool                        s_codec_opened = false;

static volatile bool s_pipe_ready = false;
static volatile bool s_streaming  = false;

/* ---- 元数据 / 播放状态 / 歌词（s_meta_lock 保护）---- */
static bt_sink_meta_t   s_meta;
static uint32_t         s_play_status;          /* esp_bt_audio_playback_status_t */
static bool             s_play_status_valid;
static SemaphoreHandle_t s_meta_lock = NULL;

/* ---- 曲目锁定 + 歌词（见头文件 get_lyric 的说明）----
 *
 * 对端把当前歌词行覆盖写进 AVRCP TITLE(0x01)，所以 TITLE 通道是"曲名/歌词"共用的。
 * 实测一次突发里的到达顺序是 TITLE -> ARTIST -> ALBUM -> GENRE，且 ARTIST 在歌词
 * 模式下会被改写成 "<曲名> (<译名>) - <歌手>"。因此这里的做法是：
 *   ① 攒下本突发的 TITLE/ARTIST/ALBUM 到 s_burst_*；
 *   ② 在 ALBUM 到达时（或下一次 TITLE 到达时补判）一次性 resolve：
 *        换曲 ⇐ 专辑变了，或 歌手不再包含已锁歌手
 *        歌词 ⇐ 其余情况下 TITLE 与已锁曲名不同
 *   ③ resolve 时若判定"换曲"，才提交新的曲名/歌手，并把歌词清空（seq++）。
 * 这样"歌词行"永远不会污染曲名，换曲也能自愈。 */
static char             s_burst_title[BT_SINK_TITLE_LEN];
static char             s_burst_artist[BT_SINK_ARTIST_LEN];
static char             s_burst_album[BT_SINK_ALBUM_LEN];
static bool             s_burst_pending;      /* 本突发攒了还没判定的待定值 */
static bool             s_track_locked;       /* 已锁住本曲目的真曲名/真歌手 */
static bt_sink_lyric_t  s_lyric;

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static void register_media_defaults(void)
{
    if (s_media_registered) {
        return;
    }
    media_lib_add_default_adapter();
    /* 先注册"普通解码器"，再注册"简单解码器"（SBC/ADPCM 这类走 simple 通道） */
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
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

/* src_ch -> dest_ch 的 asrc 混音权重矩阵（与 bt_push_service / 官方例程同构） */
static void fill_asrc_weight(float *weight, uint8_t src_ch, uint8_t dest_ch)
{
    if (!weight || src_ch == 0 || dest_ch == 0 ||
        src_ch > SINK_ASRC_MAX_CH || dest_ch > SINK_ASRC_MAX_CH) {
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
            weight[dest * src_ch + pick] = 1.0f;
        }
    }
}

/* 在管线里按 tag 找 aud_asrc（第 index 个） */
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

static esp_gmf_element_handle_t find_element(const char *name)
{
    esp_gmf_element_handle_t el = NULL;
    if (!s_pipe) {
        return NULL;
    }
    if (esp_gmf_pipeline_get_el_by_name(s_pipe, name, &el) != ESP_GMF_ERR_OK) {
        return NULL;
    }
    return el;
}

static void set_asrc_dest(uint8_t index, uint32_t sample_rate, uint8_t src_ch, uint8_t dest_ch)
{
    esp_gmf_element_handle_t asrc = find_asrc(index);
    if (!asrc) {
        ESP_LOGW(TAG, "aud_asrc[%u] not found", index);
        return;
    }
    if (src_ch == 0 || dest_ch == 0 ||
        src_ch > SINK_ASRC_MAX_CH || dest_ch > SINK_ASRC_MAX_CH) {
        ESP_LOGW(TAG, "invalid asrc ch: src=%u dest=%u", src_ch, dest_ch);
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

static bool post_cmd(sink_cmd_t action)
{
    if (!s_cmd_queue) {
        return false;
    }
    sink_cmd_pkt_t cmd = { .action = action };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, drop action=%d", action);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 元数据 / 歌词：突发判定                                             */
/* ------------------------------------------------------------------ */

/* 把一段「不一定 NUL 结尾」的 UTF-8 抄进 dst（保证 NUL 结尾，超长截断）。
 * ★ 刻意不用 snprintf("%s")：本工程 -Werror=all 下 GCC 的 -Wformat-truncation
 *   会对"源与目标同尺寸"的 snprintf 直接报错，而这里的截断本就是设计意图。 */
static void copy_text(char *dst, size_t cap, const void *src, uint32_t len)
{
    if (cap == 0) {
        return;
    }
    size_t n = len;
    if (n > cap - 1) {
        n = cap - 1;
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

/* 同上，但源是 NUL 结尾的 C 串。 */
static void str_copy(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

/**
 * @brief 结算一次元数据突发，决定它是「换曲」还是「歌词行」。
 * @note  必须已持有 s_meta_lock。
 */
static void burst_resolve(void)
{
    if (!s_burst_pending) {
        return;
    }
    s_burst_pending = false;

    bool new_track = !s_track_locked;
    if (s_track_locked) {
        if (s_burst_album[0] && strcmp(s_burst_album, s_meta.album) != 0) {
            /* 专辑变了 ⇒ 换曲 */
            new_track = true;
        } else if (s_burst_artist[0] && s_meta.artist[0] &&
                   strstr(s_burst_artist, s_meta.artist) == NULL) {
            /* 歌手不再包含已锁歌手 ⇒ 换曲。
             * ★ 用"包含"而不是"相等"：歌词模式下对端把 ARTIST 改写成
             *   "<曲名> (<译名>) - <歌手>"，原歌手仍是它的子串。 */
            new_track = true;
        }
    }

    if (new_track) {
        if (s_burst_title[0])  {
            str_copy(s_meta.title, sizeof(s_meta.title), s_burst_title);
        }
        if (s_burst_artist[0]) {
            str_copy(s_meta.artist, sizeof(s_meta.artist), s_burst_artist);
        }
        s_track_locked = true;
        s_lyric.text[0] = '\0';
        s_lyric.seq++;                 /* 通知 UI：上一曲的歌词作废 */
        ESP_LOGI(TAG, "track locked: \"%s\" / \"%s\"", s_meta.title, s_meta.artist);
    } else if (s_burst_title[0] && strcmp(s_burst_title, s_meta.title) != 0) {
        str_copy(s_lyric.text, sizeof(s_lyric.text), s_burst_title);
        s_lyric.seq++;
        ESP_LOGI(TAG, "lyric[%u] = %s", (unsigned)s_lyric.seq, s_lyric.text);
    }

    /* album 延后到这里才提交：上面判"变没变"必须拿上一曲的值比。 */
    if (s_burst_album[0]) {
        str_copy(s_meta.album, sizeof(s_meta.album), s_burst_album);
    }

    s_burst_title[0] = '\0';
    s_burst_artist[0] = '\0';
    s_burst_album[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* 处理任务：串行执行所有阻塞的 pipeline 操作                          */
/* ------------------------------------------------------------------ */

static void proc_task(void *arg)
{
    (void)arg;
    sink_cmd_pkt_t cmd;
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_pipe) {
            continue;
        }

        switch (cmd.action) {
        case SINK_CMD_PREPARE:
            /* codec 只开一次：反复 open/close 会有功放开关的爆音 */
            if (s_codec && !s_codec_opened) {
                esp_codec_dev_sample_info_t fs = {
                    .sample_rate = SINK_DAC_RATE,
                    .channel = SINK_DAC_CH,
                    .bits_per_sample = SINK_DAC_BITS,
                };
                esp_err_t ret = esp_codec_dev_open(s_codec, &fs);
                if (ret == ESP_CODEC_DEV_OK) {
                    esp_codec_dev_set_out_vol(s_codec, SINK_DAC_VOL);
                    s_codec_opened = true;
                    ESP_LOGI(TAG, "codec opened @%d/%dch/%dbits", SINK_DAC_RATE, SINK_DAC_CH, SINK_DAC_BITS);
                } else {
                    ESP_LOGE(TAG, "codec open failed: %d", (int)ret);
                }
            }
            ESP_LOGI(TAG, "pipeline loading jobs");
            esp_gmf_pipeline_loading_jobs(s_pipe);
            break;

        case SINK_CMD_RUN:
            ESP_LOGI(TAG, "pipeline run");
            esp_gmf_pipeline_run(s_pipe);
            break;

        case SINK_CMD_STOP_RESET:
            ESP_LOGI(TAG, "pipeline stop+reset");
            esp_gmf_pipeline_stop(s_pipe);
            esp_gmf_pipeline_reset(s_pipe);
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 开机早期：一次性建好 pool / pipeline / task                          */
/* ------------------------------------------------------------------ */

void bt_sink_service_early_init(void)
{
    if (s_pipe_ready) {
        return;
    }

    ESP_LOGI(TAG, "early init: internal free=%d largest=%d | PSRAM free=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    register_media_defaults();

    s_meta_lock = xSemaphoreCreateMutex();
    if (!s_meta_lock) {
        ESP_LOGE(TAG, "meta mutex create failed");
        goto fail;
    }

    /* asrc 权重矩阵：显式 PSRAM（P2） */
    s_asrc_weight = heap_caps_calloc(SINK_ASRC_MAX_WEIGHT, sizeof(float),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_asrc_weight) {
        ESP_LOGE(TAG, "asrc weight PSRAM alloc failed");
        goto fail;
    }

    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pool init failed");
        goto fail;
    }

    esp_gmf_element_handle_t el = NULL;
    esp_gmf_io_handle_t io = NULL;

    /* 1) 解码器：初值随便给 SBC，真正参数由 ALLOCATED 时按对端协商结果重配 */
    esp_audio_simple_dec_cfg_t dec_cfg = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
    dec_cfg.dec_type = ESP_AUDIO_TYPE_SBC;
    dec_cfg.dec_cfg = NULL;
    dec_cfg.cfg_size = 0;
    if (esp_gmf_audio_dec_init(&dec_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_dec init failed");
        goto fail;
    }

    /* 2) 采样率/声道适配：对端可能给 48k/单声道，本机 DAC 固定 44.1k 立体声 */
    esp_asrc_cfg_t asrc_cfg = DEFAULT_ESP_GMF_ASRC_CONFIG();
    if (esp_gmf_asrc_init(&asrc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_pool, el, NULL);
    } else {
        ESP_LOGE(TAG, "aud_asrc init failed");
        goto fail;
    }

    /* 3) 输入 IO：io_bt（READER）—— 音频由对端推来 */
    bt_io_cfg_t bt_cfg = ESP_GMF_BT_IO_CFG_DEFAULT();
    bt_cfg.dir = ESP_GMF_IO_DIR_READER;
    bt_cfg.stream = NULL;
    if (esp_gmf_io_bt_init(&bt_cfg, &io) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_io(s_pool, io, NULL);
    } else {
        ESP_LOGE(TAG, "io_bt(reader) init failed");
        goto fail;
    }

    /* 4) 输出 IO：io_codec_dev（WRITER）—— 写板载 ES8389。
          ★ 注意：io_codec_dev 自己**不** open codec，必须我们在 PREPARE 里 open。 */
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "speaker codec init failed");
        goto fail;
    }
    codec_dev_io_cfg_t codec_cfg = ESP_GMF_IO_CODEC_DEV_CFG_DEFAULT();
    codec_cfg.dir = ESP_GMF_IO_DIR_WRITER;
    codec_cfg.dev = s_codec;
    if (esp_gmf_io_codec_dev_init(&codec_cfg, &io) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_io(s_pool, io, NULL);
    } else {
        ESP_LOGE(TAG, "io_codec_dev(writer) init failed");
        goto fail;
    }

    /* 5) 建管线：io_bt -> aud_dec -> aud_asrc -> io_codec_dev */
    const char *el_names[] = {"aud_dec", "aud_asrc"};
    if (esp_gmf_pool_new_pipeline(s_pool, "io_bt", el_names, 2, "io_codec_dev", &s_pipe) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "new_pipeline failed");
        goto fail;
    }

    /* 6) GMF task：栈进 PSRAM，核 1 */
    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.core = 1;
    task_cfg.thread.stack = 5120;
    task_cfg.thread.prio = 15;
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "bt2codec_task";
    if (esp_gmf_task_init(&task_cfg, &s_task) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "task init failed");
        goto fail;
    }
    esp_gmf_pipeline_bind_task(s_pipe, s_task);

    /* 7) 命令队列 + 处理任务 */
    s_cmd_queue = xQueueCreate(SINK_CMD_QUEUE_SIZE, sizeof(sink_cmd_pkt_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "cmd queue create failed");
        goto fail;
    }
    if (xTaskCreatePinnedToCoreWithCaps(proc_task, "bt_sink_proc", SINK_PROC_STACK, NULL,
                                        SINK_PROC_PRIO, NULL, SINK_PROC_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "proc task create failed");
        goto fail;
    }

    s_pipe_ready = true;
    ESP_LOGI(TAG, "ready (early, resident): internal free=%d | PSRAM free=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return;

fail:
    /* 蓝牙非必需功能：失败只打日志，不让整机起不来。
       ★ 封面链路已删除（见文件头），收尾不再需要"先让解码任务退出再删队列"那套同步。 */
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
    if (s_asrc_weight) { heap_caps_free(s_asrc_weight); s_asrc_weight = NULL; }
    if (s_meta_lock) { vSemaphoreDelete(s_meta_lock); s_meta_lock = NULL; }
    ESP_LOGE(TAG, "early init FAILED, BT sink disabled");
}

/* ------------------------------------------------------------------ */
/* 事件入口（全部跑在 BT 线程）                                        */
/* ------------------------------------------------------------------ */

void bt_sink_service_on_stream_state(void *stream, int state)
{
    if (!s_pipe_ready || !s_pipe || !stream) {
        return;
    }
    esp_bt_audio_stream_handle_t st = (esp_bt_audio_stream_handle_t)stream;

    switch (state) {
    case ESP_BT_AUDIO_STREAM_STATE_ALLOCATED: {
        s_stream = st;
        ESP_LOGI(TAG, "stream ALLOCATED, prepare bt2codec pipeline");

        /* 绑 BT stream 到 io_bt 的**输入**实例（Sink 是读，不是写） */
        esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_IN_INSTANCE(s_pipe), st);

        esp_bt_audio_stream_codec_info_t ci = {0};
        if (esp_bt_audio_stream_get_codec_info(st, &ci) != ESP_OK) {
            ESP_LOGE(TAG, "get codec info failed");
            return;
        }
        uint8_t src_ch = (uint8_t)__builtin_popcount(ci.channels);
        if (src_ch == 0) {
            src_ch = 2;
        }
        ESP_LOGI(TAG, "peer codec: type=%d sr=%d ch=%d bits=%d",
                 ci.codec_type, ci.sample_rate, src_ch, ci.bits);

        /* 解码器按对端协商结果重配（SBC/AAC/LC3 由 codec_to_audio_type 映射） */
        esp_gmf_element_handle_t dec = find_element("aud_dec");
        if (dec) {
            esp_audio_simple_dec_cfg_t dec_cfg = {
                .dec_type = codec_to_audio_type(ci.codec_type),
                .dec_cfg = ci.codec_cfg,
                .cfg_size = ci.cfg_size,
                .use_frame_dec = true,
            };
            if (esp_gmf_audio_dec_reconfig(dec, &dec_cfg) != ESP_GMF_ERR_OK) {
                ESP_LOGW(TAG, "aud_dec reconfig failed");
            }
        } else {
            ESP_LOGW(TAG, "aud_dec not found in pipeline");
        }

        /* ASRC：对端采样率/声道 -> 本机 DAC 固定 44.1k/2ch */
        set_asrc_dest(0, SINK_DAC_RATE, src_ch, SINK_DAC_CH);

        /* ★ 注册远端通知（只有注册了才会收到 PLAYBACK_STATUS_CHG / TRACK_CHANGE），
           并主动拉一次元数据 —— 有些对端不会先发 TRACK_CHANGE。
           ★ 刻意**不**注册 PLAY_POS_CHANGED：本工程没有进度条，
             注册它只会让对端每隔约 1s 往 BT 线程塞一次无用事件。 */
        esp_err_t nr = esp_bt_audio_playback_reg_notifications(
            ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE |
            ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE |
            ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_END |
            ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_REACHED_START |
            ESP_BT_AUDIO_PLAYBACK_EVENT_NOW_PLAYING_CHANGE);
        ESP_LOGI(TAG, "reg_notifications ret=%s", esp_err_to_name(nr));

        esp_err_t mr = esp_bt_audio_playback_request_metadata(
            ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE |
            ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST |
            ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM |
            ESP_BT_AUDIO_PLAYBACK_METADATA_GENRE);
        ESP_LOGI(TAG, "request_metadata ret=%s", esp_err_to_name(mr));

        /* loading_jobs 可能阻塞 → 走命令队列 */
        post_cmd(SINK_CMD_PREPARE);
        break;
    }

    case ESP_BT_AUDIO_STREAM_STATE_STARTED:
        ESP_LOGI(TAG, "stream STARTED");
        s_streaming = true;
        post_cmd(SINK_CMD_RUN);
        break;

    case ESP_BT_AUDIO_STREAM_STATE_STOPPED:
        ESP_LOGI(TAG, "stream STOPPED");
        s_streaming = false;
        post_cmd(SINK_CMD_STOP_RESET);
        break;

    case ESP_BT_AUDIO_STREAM_STATE_RELEASED:
        ESP_LOGI(TAG, "stream RELEASED");
        s_streaming = false;
        if (s_stream == st) {
            s_stream = NULL;
        }
        /* 不 destroy pipeline（P3）：下次 ALLOCATED 复用同一个 */
        break;

    default:
        break;
    }
}

void bt_sink_service_on_playback_status(const void *evt)
{
    if (!evt) {
        return;
    }
    const esp_bt_audio_event_playback_st_t *ps = (const esp_bt_audio_event_playback_st_t *)evt;

    if (s_meta_lock) xSemaphoreTake(s_meta_lock, portMAX_DELAY);
    switch (ps->event) {
    case ESP_BT_AUDIO_PLAYBACK_EVENT_PLAY_STATUS_CHANGE:
        s_play_status = ps->evt_param.play_status;
        s_play_status_valid = true;
        ESP_LOGI(TAG, "play status -> %u", (unsigned)ps->evt_param.play_status);
        break;
    case ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE:
        /* ★ 这里**不能**清元数据（2026-09-22 修正）。
         *   实测对端每推进一句歌词都会发一次 TRACK_CHANGE（见 §28.29 日志：
         *   56035/56769/59353/63851/67004/70120 各对应一句歌词），
         *   若在这里 memset(s_meta)，曲名/歌手会被反复擦掉。
         *   真正的换曲判定交给 burst_resolve() 的「专辑变 / 歌手不再包含」两条判据。 */
        ESP_LOGI(TAG, "track changed -> request metadata");
        break;
    default:
        break;
    }
    if (s_meta_lock) xSemaphoreGive(s_meta_lock);

    if (ps->event == ESP_BT_AUDIO_PLAYBACK_EVENT_TRACK_CHANGE) {
        esp_bt_audio_playback_request_metadata(
            ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE |
            ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST |
            ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM |
            ESP_BT_AUDIO_PLAYBACK_METADATA_GENRE);
    }
}

void bt_sink_service_on_metadata(const void *evt)
{
    if (!evt) {
        return;
    }
    const esp_bt_audio_event_playback_metadata_t *md =
        (const esp_bt_audio_event_playback_metadata_t *)evt;

    if (!md->value || md->length == 0) {
        /* ★ 空值**不能**当成"这块字段没了"就丢掉：实测对端每次突发都会带一条
         *   `type=0x20 len=0`（空流派）。但 TITLE/ARTIST/ALBUM 的空值确实该丢弃。 */
        return;
    }

    char tmp[BT_SINK_TITLE_LEN];   /* 三个字段里最长的容量 */
    copy_text(tmp, sizeof(tmp), md->value, md->length);

    if (s_meta_lock) xSemaphoreTake(s_meta_lock, portMAX_DELAY);

    switch (md->type) {
    case ESP_BT_AUDIO_PLAYBACK_METADATA_TITLE:
        /* ★ TITLE 是"曲名 / 歌词行"共用通道：先攒着，等突发结算时再决定去向。
         *   结算前若上一突发还没等到 ALBUM（对端不带专辑信息），在这里补判 ——
         *   代价是那种对端会有一句歌词的滞后，可接受。 */
        burst_resolve();
        str_copy(s_burst_title, sizeof(s_burst_title), tmp);
        s_burst_pending = true;
        break;

    case ESP_BT_AUDIO_PLAYBACK_METADATA_ARTIST:
        str_copy(s_burst_artist, sizeof(s_burst_artist), tmp);
        s_burst_pending = true;
        break;

    case ESP_BT_AUDIO_PLAYBACK_METADATA_ALBUM:
        str_copy(s_burst_album, sizeof(s_burst_album), tmp);
        s_burst_pending = true;
        burst_resolve();               /* ALBUM 通常是一次突发的最后一个字段 → 就地结算 */
        break;

    case ESP_BT_AUDIO_PLAYBACK_METADATA_GENRE:
        /* 流派不参与"换曲/歌词"判定，直接提交（UI 目前也不显示，留着以备后用） */
        str_copy(s_meta.genre, sizeof(s_meta.genre), tmp);
        break;

    default:
        break;
    }

    if (s_meta_lock) xSemaphoreGive(s_meta_lock);

    ESP_LOGI(TAG, "meta type=0x%02x = %s", (unsigned)md->type, tmp);
}

void bt_sink_service_request_stop(void)
{
    if (!s_pipe_ready) {
        return;
    }
    s_streaming = false;
    s_stream = NULL;
    post_cmd(SINK_CMD_STOP_RESET);
    ESP_LOGI(TAG, "stop requested (pipeline stays resident)");
}

/* ------------------------------------------------------------------ */
/* UI 轮询接口                                                         */
/* ------------------------------------------------------------------ */

bool bt_sink_service_get_meta(bt_sink_meta_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!s_meta_lock) {
        return false;
    }
    xSemaphoreTake(s_meta_lock, portMAX_DELAY);
    *out = s_meta;
    xSemaphoreGive(s_meta_lock);
    return (out->title[0] || out->artist[0] || out->album[0] || out->genre[0]);
}

bt_sink_state_t bt_sink_service_get_state(void)
{
    if (!s_play_status_valid) {
        return BT_SINK_STATE_IDLE;
    }
    switch (s_play_status) {
    case ESP_BT_AUDIO_PLAYBACK_STATUS_PLAYING: return BT_SINK_STATE_PLAYING;
    case ESP_BT_AUDIO_PLAYBACK_STATUS_PAUSED:  return BT_SINK_STATE_PAUSED;
    case ESP_BT_AUDIO_PLAYBACK_STATUS_STOPPED: return BT_SINK_STATE_STOPPED;
    default:                                   return BT_SINK_STATE_IDLE;
    }
}

bool bt_sink_service_is_streaming(void)
{
    return s_streaming;
}

bool bt_sink_service_get_lyric(bt_sink_lyric_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!s_meta_lock) {
        return false;
    }
    xSemaphoreTake(s_meta_lock, portMAX_DELAY);
    *out = s_lyric;
    xSemaphoreGive(s_meta_lock);
    /* ★ 返回 false 只代表"从没收到过歌词"；UI 靠 seq 变化推进动画，
     *   换曲时 s_lyric.text 会被清空但 seq 前进 —— 那也是一次有效通知。 */
    return (out->seq != 0);
}

void bt_sink_service_clear_media(void)
{
    if (s_meta_lock) xSemaphoreTake(s_meta_lock, portMAX_DELAY);
    memset(&s_meta, 0, sizeof(s_meta));
    s_play_status_valid = false;

    /* 曲目锁定 + 待定突发也要一起复位，否则重连后会把上一轮的曲名当成本轮已锁值，
     * 导致本轮第一句歌词被误判成"换曲"。 */
    s_track_locked = false;
    s_burst_pending = false;
    s_burst_title[0] = '\0';
    s_burst_artist[0] = '\0';
    s_burst_album[0] = '\0';
    s_lyric.text[0] = '\0';
    s_lyric.seq++;

    if (s_meta_lock) xSemaphoreGive(s_meta_lock);
}

/* ------------------------------------------------------------------ */
/* AVRCP 控制（CT 方向）                                                */
/* ------------------------------------------------------------------ */

esp_err_t bt_sink_service_play(void)  { return esp_bt_audio_playback_play();  }
esp_err_t bt_sink_service_pause(void) { return esp_bt_audio_playback_pause(); }
esp_err_t bt_sink_service_next(void)  { return esp_bt_audio_playback_next();  }
esp_err_t bt_sink_service_prev(void)  { return esp_bt_audio_playback_prev();  }
