/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file xiaozhi_service.c
 * @brief 小智对话服务实现（S5 §4.6.3；链路照搬 vocat main/esp_xiaozhi_chat_app.c
 *        的音频编排，裁掉其 UI/板级/相机/点歌）。
 *
 * ★ 2026-09-26 已对齐 av_processor 0.7.0 API（vocat 参考代码按 0.5.x 写）：
 *   - audio_manager 无 play_dev/rec_dev 字段 ⇒ 设备 IO 走 audio_io_ops_t
 *     回调（write_cb/read_cb），内部用 audio_service ext 借出的 codec 句柄；
 *   - recorder/feeder/playback 全部句柄制（open(config, &handle)）；
 *   - AFE 开关 = afe_config.mode.afe.algo_mask 位标志（无 ai_mode_wakeup 字段）；
 *     mask=0 会整体跳过 AFE 走 raw 路径（audio_processor.c:662）⇒ AEC/NS/VAD
 *     必须显式置位；AI_MODE_WAKEUP 不置位 = DIRECT（无唤醒词）；
 *   - feeder open 后停在 IDLE，必须显式 audio_feeder_start()；
 *   - audio_recorder_read_data 返回字节数（实现返回 bytes_to_copy，非 esp_err_t），
 *     内部 esp_gmf_fifo_acquire_read(portMAX_DELAY) 阻塞 ⇒ 上行任务**不自退**，
 *     teardown 先 vTaskDelete 再 close（vocat 已验证顺序）。
 *
 * 数据流（与 vocat 一致）：
 *   上行 mic → audio_recorder(AFE+Opus) → xz_uplink_task read_data
 *             → esp_xiaozhi_chat_send_audio_data
 *   下行 esp_xiaozhi_chat audio_callback(Opus 包) → audio_feeder_feed_data
 *             → 解码 → play_dev（ES8389，经 play_io.write_cb）
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "middleware/mem_diag.h"

#include "audio_processor.h"        /* av_processor: audio_manager/recorder/feeder/playback */
#include "esp_codec_dev.h"

#include "esp_xiaozhi_chat.h"       /* esp_xiaozhi 协议层（vendor 0.1.1） */
#include "esp_xiaozhi_info.h"       /* 激活状态/OTA info（board 默认实现自带 uuid） */

#include "xiaozhi_service.h"
#include "ai_light.h"
#include "ai_speaker.h"
#include "ai_camera.h"
#include "ai_device.h"
#include "audio_service.h"

static const char *TAG = "XZ_SVC";

/* ---- M3.2 裁决项（§53 定案：预案 B）----
 * ★ "MR"（ch0=mic、ch1=DAC 回采作 AEC 参考）已实测否决：截断铁证 =
 *   每轮 STT 只出前 ~5 个字（AEC 自适应滤波 ~1s 收敛后把人声当回声消掉
 *   ⇒ 参考通道实为第二颗麦/非回采，MR 是照抄 vocat 从未实测）。
 * 落地 "MM"：单麦无回采，AFE 关 AEC。TTS 自声污染上行由现有闸门兜住
 *   （TTS 期间 skip + 500ms 静默窗），打断走 §52 abort 路径不需要 AEC。
 * ★ 想回 MR/双麦：先按 S5 §4.6.4 探针实测 ch1 是回采（播正弦看 ch1
 *   能量），再开 AEC_ENABLE，别再盲开。 */
#define XZ_MIC_LAYOUT       "MM"
#define XZ_BOARD_CHANNELS   1
#define XZ_SAMPLE_RATE      16000
#define XZ_BITS             16
#define XZ_FRAME_MS         60
#define XZ_OPUS_BITRATE     24000

/* 上行读块大小：recorder FIFO 按块返回（单块 ≤ 请求值即返回），
 * 此值只是上限；60ms @16k/16bit/mono PCM = 1920B，Opus 帧更小 */
#define XZ_REC_READ_SIZE    (1920 * 2)

/* ---- 命令 ---- */
typedef enum {
    XZ_CMD_START = 0,
    XZ_CMD_STOP,
} xz_cmd_id_t;

typedef struct {
    xz_cmd_id_t id;
} xz_cmd_t;

/* ---- 状态 ---- */
static QueueHandle_t s_cmd_q;
static TaskHandle_t  s_task;
static xiaozhi_evt_cb_t s_evt_cb;
static volatile xiaozhi_state_t s_state = XZ_STATE_IDLE;

/* 会话资源（PREPARING 建立，RELEASING 逐层销毁） */
static esp_xiaozhi_chat_handle_t s_chat;        /* 0 = 无会话 */
static volatile bool s_chatting;                /* 上行任务发送闸门（channel 已开） */
static volatile bool s_tts_active;              /* TTS 播放中（MM 预案的上行闸门） */
/* PTT 按住说话（manual 模式：回合边界由按钮显式控制）：
 * want = UI 期望态；on = 协议已生效态。协议消息由上行任务对齐发出
 * （不在 LVGL 任务阻塞发送，也不在 vendor 回调里发——F44 同族纪律） */
static volatile bool s_ptt_want;
static volatile bool s_ptt_on;
/* ★ §52 barge-in：TTS 播放中按 PTT = 打断。此时**不能**直接发 listen start
 * （边播边开票会把服务器回合状态搞死，实测=永久聋），正确路径 = 78 固件
 * Speaking 态按键同款：先 abort_speaking，等服务器 tts stop 复位回合后
 * 再补发 listen start。pending 标记"abort 已发、listen start 欠着" */
static volatile bool s_ptt_start_pending;
/* TTS stop 后的回声静默窗：stop 事件到 ≠ 本地 feeder 队列播空（feeder 无
 * flush API），MM/尾音期间抢上的帧会被服务器转写成垃圾。窗口内不上行 */
#define XZ_ECHO_GUARD_MS    500
static volatile TickType_t s_tts_quiet_until;
/* ★ §53 会话聋化自愈：实测（2026-09-27 日志）第 4 轮起服务器对 listen start/
 * stop 全部 ESP_OK 却不再回任何 stt/tts（连接活着、音频在发）——服务器侧
 * 对话已失效但设备无感。对策 = 本轮发了足量语音（≥10 帧）却 8s 内等不到
 * 任何回音 → 自动 close→open audio channel（重发 hello 换新 session_id）
 * + 重发 wake word，会话自愈，用户无感重试即可。 */
#define XZ_ROUND_STALL_FRAMES   10      /* ~0.6s 语音：防误伤极短误触 */
#define XZ_ROUND_STALL_MS       8000
static volatile bool s_round_armed;         /* 等本轮服务器回音 */
static volatile TickType_t s_round_deadline;
static TaskHandle_t  s_uplink_task;             /* 不自退，teardown 强制删除 */
static uint8_t      *s_uplink_buf;              /* PSRAM */
/* av_processor 0.7.0 句柄制 */
static audio_recorder_handle_t s_recorder;
static audio_feeder_handle_t   s_feeder;
static audio_play_handle_t     s_play;
/* ext 独占租借借出的服务句柄（session_setup 记录，teardown close 后归还） */
static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;

/* ============================================================
 * 事件上抛
 * ============================================================ */

static void notify(xiaozhi_evt_id_t id, const char *text, int role, esp_err_t err)
{
    if (s_evt_cb) {
        xiaozhi_evt_t evt = { .id = id, .text = text, .role = role, .err = err };
        s_evt_cb(&evt);
    }
}

/* §58 A1：ai_camera explain 结果 → XZ_EVT_TEXT(role=assistant) 进浮窗对话记录
 * （ai_camera explain 任务上下文调用，回调链非阻塞） */
static void on_ai_camera_result(const char *text)
{
    notify(XZ_EVT_TEXT, text, 1, ESP_OK);
}

static void set_state(xiaozhi_state_t st)
{
    s_state = st;
    notify(XZ_EVT_STATE, NULL, 0, ESP_OK);
}

/* ============================================================
 * av_processor 0.7.0 设备 IO 适配（audio_io_ops_t 回调，
 * 内部走 ext 独占租借借出的 codec 句柄 —— 从根上避免二次 init）
 * ============================================================ */

static esp_err_t xz_play_write_cb(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* esp_codec_dev_write：成功返回 >=0（字节数/OK），失败负值 */
    return (esp_codec_dev_write(s_spk, data, size) >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t xz_rec_read_cb(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_read(s_mic, data, size) >= 0) ? ESP_OK : ESP_FAIL;
}

/* ============================================================
 * esp_xiaozhi 回调（协议任务上下文，只投事件/置标志）
 * ★ vendor 事件分两路（esp_xiaozhi_chat.c 实证）：
 *   - 连接类（CONNECTED/DISCONNECTED/AUDIO_CHANNEL_OPENED/CLOSED/
 *     SERVER_GOODBYE）走 esp_event 总线 ESP_XIAOZHI_CHAT_EVENTS，
 *     由 xz_esp_event_handler 接（见下）；
 *   - 对话类（CHAT_TEXT/TTS_STATE/ERROR/EMOJI/SYSTEM_CMD/SPEECH_*）
 *     才走 event_callback，参数是 esp_xiaozhi_chat_event_t 枚举。
 *   ★ 头文件里连接类是位掩码 #define（1<<0..5），与枚举值域重叠
 *     （DISCONNECTED=2 == CHAT_ERROR）——绝不能混进同一个 switch。 */
static void chat_audio_cb(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    /* 下行 TTS Opus 包 → feeder 解码出声（vocat :803 同款，0.7.0 句柄制） */
    if (s_feeder) {
        audio_feeder_feed_data(s_feeder, (uint8_t *)data, len);
    }
}

static void chat_event_cb(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    (void)ctx;
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *t = (esp_xiaozhi_chat_text_data_t *)event_data;
        /* 可追溯：stt（role=user）到没到是"服务器听没听见"的唯一证据 */
        ESP_LOGI(TAG, "chat text role=%d: %.48s",
                 t ? (int)t->role : -1, (t && t->text) ? t->text : "(null)");
        s_round_armed = false;      /* 服务器有回音 = 会话活着（§53） */
        notify(XZ_EVT_TEXT, t ? t->text : NULL,
               t ? (int)t->role : 0, ESP_OK);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *t = (esp_xiaozhi_chat_tts_state_t *)event_data;
        bool was = s_tts_active;
        s_tts_active = (t && t->state != ESP_XIAOZHI_CHAT_TTS_STATE_STOP);
        if (was && !s_tts_active) {
            /* stop 事件到 ≠ feeder 播空：开回声静默窗（§52）。
             * ★ 且按 §48 教训：tts stop 后服务器回合必复位——若用户正按住
             *   （含"按住期间 TTS 才开始"的竞态），欠一张 listen start，
             *   由上行任务补票，否则这一按到头都是聋的 */
            s_tts_quiet_until = xTaskGetTickCount() + pdMS_TO_TICKS(XZ_ECHO_GUARD_MS);
            if (s_ptt_on) {
                s_ptt_start_pending = true;
            }
        } else if (!was && s_tts_active) {
            s_tts_quiet_until = 0;
            s_round_armed = false;  /* TTS 能起播 = 会话活着（§53） */
        }
        ESP_LOGI(TAG, "tts state=%d (uplink gate %s)",
                 t ? (int)t->state : -1,
                 s_tts_active ? "PAUSED" : "open");
        notify(XZ_EVT_TTS_STATE, t ? t->text : NULL, 0, ESP_OK);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *e = (esp_xiaozhi_chat_error_info_t *)event_data;
        notify(XZ_EVT_ERROR, e ? e->source : NULL, 0, e ? e->code : ESP_FAIL);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI: {
        /* §58 A2：LLM 情绪标签（vendor chat.c:624 已 emit），转发浮窗对话记录 */
        const char *emo = (const char *)event_data;
        ESP_LOGI(TAG, "emoji: %s", emo ? emo : "(null)");
        notify(XZ_EVT_EMOJI, emo, 0, ESP_OK);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD: {
        /* §58 A3：服务器命令（vendor 只上报）。reboot = 立即重启（78 固件
         * 同款；esp_restart 不做清理——NVS 无在写操作，codec 状态重启后
         * 重新 init，风险可接受）。其他命令忽略。 */
        const char *cmdstr = (const char *)event_data;
        ESP_LOGW(TAG, "system cmd: %s", cmdstr ? cmdstr : "(null)");
        if (cmdstr && strcmp(cmdstr, "reboot") == 0) {
            notify(XZ_EVT_TEXT, "Rebooting device...", 1, ESP_OK);
            vTaskDelay(pdMS_TO_TICKS(200));   /* 给 notify/日志一点落盘时间 */
            esp_restart();
        }
        break;
    }
    default:
        break;   /* SPEECH_STARTED/STOPPED：v1 忽略（EMOJI/SYSTEM_CMD 见上） */
    }
}

/* 连接类事件（esp_event 默认循环上下文，只置标志/上抛） */
static void xz_esp_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "audio channel opened");
        s_chatting = true;      /* 会话就绪，上行任务开始发送 */
        set_state(XZ_STATE_CHATTING);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "audio channel closed");
        s_chatting = false;
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGW(TAG, "disconnected/goodbye (id=%d)", (int)id);
        s_chatting = false;
        break;
    default:
        break;   /* CONNECTED 等：v1 忽略 */
    }
}

static esp_err_t chat_setup(void)
{
    esp_xiaozhi_chat_config_t cfg = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    cfg.audio_callback = chat_audio_cb;
    cfg.event_callback = chat_event_cb;
    cfg.audio_callback_ctx = NULL;
    cfg.event_callback_ctx = NULL;
    /* ★ MCP 引擎是 vendor 的**会话载体**（esp_xiaozhi_chat.c:1400 chat_start
     *   硬性必填，实测 2026-09-27 "MCP engine not initialized" 直接拒启），
     *   不是可选项。§44 曾建空引擎（零工具）；§55 起挂**灯控工具**
     *   （ai_light：开/关/调色/亮度，服务器 tools/list 自动发现）。
     *   owns=true 交给 chat_deinit 统一释放（destroy_owned_resources 在
     *   deinit 内部调用，无双删风险；工具随引擎一起析构）。 */
    esp_mcp_t *mcp = NULL;
    esp_err_t ret = esp_mcp_create(&mcp);
    if (ret != ESP_OK || mcp == NULL) {
        ESP_LOGE(TAG, "mcp engine create failed: %s", esp_err_to_name(ret));
        return ret;
    }
    cfg.mcp_engine = mcp;
    cfg.owns_mcp_engine = true;
    ret = ai_light_attach(mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ai_light_attach failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ai_speaker_attach(mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ai_speaker_attach failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ai_camera_attach(mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ai_camera_attach failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ai_device_attach(mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ai_device_attach failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return esp_xiaozhi_chat_init(&cfg, &s_chat);
}

/* ============================================================
 * 上行任务：recorder(AFE+Opus) → chat
 * ★ 生命周期：**不自退**（audio_recorder_read_data 在 fifo 上
 *   portMAX_DELAY 阻塞，自退会让 s_uplink_task 句柄悬空）；
 *   统一由 session_teardown 强制 vTaskDelete（vocat 已验证顺序）。
 * ============================================================ */

static void xz_uplink_task(void *arg)
{
    (void)arg;
    int consec_err = 0;
    uint32_t frames = 0, n_sent = 0, n_skip_tts = 0, n_skip_ptt = 0,
             n_skip_echo = 0, total_b = 0;
    uint32_t round_sent = 0;    /* 本轮（按住期间）已发帧数，§53 自愈判据 */
    for (;;) {
        int ret = audio_recorder_read_data(s_recorder, s_uplink_buf, XZ_REC_READ_SIZE);
        if (ret <= 0) {
            /* recorder 已关/未开等：退避重试，日志节流 */
            if (++consec_err == 1 || (consec_err % 50) == 0) {
                ESP_LOGW(TAG, "recorder read failed: %d (consec=%d)", ret, consec_err);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        consec_err = 0;
        frames++;

        /* PTT 期望态对齐：UI 只置 s_ptt_want，协议消息在这里发。
         * manual 模式：按住 = listen start，松开 = listen stop（官方协议
         * 的设备端控回合边界）。
         * ★ §52 barge-in：TTS 播放中按住 → abort_speaking 打断（78 固件
         *   Speaking 态按键同款路径），listen start **延后**到 tts stop
         *   复位回合后再补发。TTS 中直接发 listen start 实测把会话搞死。 */
        bool want = s_ptt_want;
        if (want != s_ptt_on && s_chatting && s_chat) {
            esp_err_t r;
            if (want && s_tts_active) {
                r = esp_xiaozhi_chat_send_abort_speaking(
                        s_chat,
                        ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
                if (r == ESP_OK) {
                    s_ptt_on = true;
                    s_ptt_start_pending = true;   /* listen start 欠着 */
                    round_sent = 0;
                }
                ESP_LOGI(TAG, "ptt BARGE-IN abort_speaking: %s",
                         esp_err_to_name(r));
            } else {
                r = want
                    ? esp_xiaozhi_chat_send_start_listening(
                          s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL)
                    : esp_xiaozhi_chat_send_stop_listening(s_chat);
                if (r == ESP_OK) {
                    s_ptt_on = want;
                    if (!want) {
                        s_ptt_start_pending = false;   /* 没按住就松了：撤欠票 */
                        /* §53：本轮结束。发了足量语音却没等到任何回音的
                         * 话，-arm- 8s 停摆检测（由本任务底部自愈） */
                        if (round_sent >= XZ_ROUND_STALL_FRAMES) {
                            s_round_armed = true;
                            s_round_deadline = xTaskGetTickCount() +
                                               pdMS_TO_TICKS(XZ_ROUND_STALL_MS);
                            ESP_LOGI(TAG, "round closed: %u frames sent, "
                                          "awaiting stt/tts (arm %dms)",
                                     (unsigned)round_sent,
                                     (int)XZ_ROUND_STALL_MS);
                        }
                        round_sent = 0;
                    } else {
                        round_sent = 0;   /* 新一轮从零计 */
                    }
                }
                ESP_LOGI(TAG, "ptt %s: %s", want ? "START" : "STOP",
                         esp_err_to_name(r));
            }
        }

        /* 打断补票：abort 后服务器回 tts stop（回合已复位），此时才真正
         * 开票——否则 s_ptt_on 已 true 而服务器早收票回空闲 = 永久聋 */
        if (s_ptt_on && s_ptt_start_pending && !s_tts_active &&
            s_chatting && s_chat) {
            esp_err_t r = esp_xiaozhi_chat_send_start_listening(
                    s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
            if (r == ESP_OK) {
                s_ptt_start_pending = false;
            }
            ESP_LOGI(TAG, "ptt RESUME (post-abort listen start): %s",
                     esp_err_to_name(r));
        }

        /* 闸门：会话在 + PTT 已生效 + TTS 未在播 + 回声静默窗已过。 */
        bool in_quiet = (s_tts_quiet_until != 0 &&
                         xTaskGetTickCount() < s_tts_quiet_until);
        bool may_send = (s_chatting && s_chat && s_ptt_on &&
                         !s_tts_active && !in_quiet);
        if (may_send) {
            esp_err_t err = esp_xiaozhi_chat_send_audio_data(
                s_chat, (const char *)s_uplink_buf, ret);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "send audio failed: %s", esp_err_to_name(err));
            } else {
                n_sent++;
                total_b += (uint32_t)ret;
            }
        } else if (s_tts_active && s_ptt_on) {
            n_skip_tts++;      /* TTS 播放中暂停上行 */
        } else if (in_quiet) {
            n_skip_echo++;     /* TTS 刚停：尾音静默窗 */
        } else {
            n_skip_ptt++;      /* 没按住语音按钮 */
        }

        /* ★ 可追溯水位：每 25 帧（~1.5s）一条，闸门状态全部留痕 */
        if ((frames % 25) == 0) {
            ESP_LOGI(TAG, "uplink: frames=%u sent=%u skip_tts=%u skip_ptt=%u "
                          "skip_echo=%u last=%dB total=%uKB tts=%d ptt=%d pend=%d",
                     (unsigned)frames, (unsigned)n_sent, (unsigned)n_skip_tts,
                     (unsigned)n_skip_ptt, (unsigned)n_skip_echo, ret,
                     (unsigned)(total_b / 1024),
                     (int)s_tts_active, (int)s_ptt_on,
                     (int)s_ptt_start_pending);
        }
    }
}

/* ============================================================
 * PREPARING / RELEASING（服务任务上下文串行执行）
 * ============================================================ */

static void session_teardown(void)
{
    set_state(XZ_STATE_RELEASING);
    s_chatting = false;
    s_tts_active = false;
    s_ptt_want = false;
    s_ptt_on = false;
    s_ptt_start_pending = false;    /* §52 barge-in 欠票一并撤 */
    s_tts_quiet_until = 0;

    /* 先摘 esp_event 订阅（防 teardown 期间连接事件再置 s_chatting） */
    esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                 xz_esp_event_handler);
    if (s_chat) {
        /* 协议礼貌收尾：先告知服务器停止收音再 goodbye（best-effort，
         * WS 已断时发送失败不阻 teardown，close_audio_channel 会继续） */
        (void)esp_xiaozhi_chat_send_stop_listening(s_chat);
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        esp_xiaozhi_chat_stop(s_chat);
        esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
    /* ★ 先强制删上行任务（大概率阻塞在 recorder fifo portMAX_DELAY；
     *   vocat 同款顺序：thread → close），再逐层关管线 */
    if (s_uplink_task) {
        vTaskDelete(s_uplink_task);
        s_uplink_task = NULL;
    }
    if (s_feeder) {
        audio_feeder_stop(s_feeder);
    }
    if (s_recorder) {
        audio_recorder_close(s_recorder);
        s_recorder = NULL;
    }
    if (s_feeder) {
        audio_feeder_close(s_feeder);
        s_feeder = NULL;
    }
    if (s_play) {
        audio_playback_close(s_play);
        s_play = NULL;
    }
    audio_manager_deinit();

    /* 归还 codec：必须先 close 两个 dev（约定见 audio_service.h） */
    if (s_spk) {
        esp_codec_dev_close(s_spk);
        s_spk = NULL;
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        s_mic = NULL;
    }
    audio_service_ext_release_exclusive();

    if (s_uplink_buf) {
        heap_caps_free(s_uplink_buf);
        s_uplink_buf = NULL;
    }
    ESP_LOGI(TAG, "session released");
    set_state(XZ_STATE_IDLE);
}

/* 激活检查（非致命，session_setup 步骤 8.5 调用）：
 * HTTP POST 到 OTA 端点拉设备信息；未激活的设备响应里带 activation_code，
 * 经 XZ_EVT_ACT_CODE 上抛给 UI 大字展示（用户去 xiaozhi.me 输入后即激活）。
 * 已激活设备响应无 code，静默通过。失败只告警不阻断（chat_start 还可以试）。
 * ★ 必须在服务任务上下文调（阻塞 HTTP 数秒）；vendor board.c 自带 uuid/keystore
 *   默认实现，无需板级胶水。 */
static void check_activation(void)
{
    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t err = esp_xiaozhi_chat_get_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_info failed: %s (activation check skipped)",
                 esp_err_to_name(err));
        return;
    }
    if (info.has_activation_code && info.activation_code) {
        ESP_LOGW(TAG, "device NOT activated, code=%s (%s)", info.activation_code,
                 info.activation_message ? info.activation_message : "");
        notify(XZ_EVT_ACT_CODE, info.activation_code, 0, ESP_OK);
    } else {
        ESP_LOGI(TAG, "activation check: activated");
    }
    esp_xiaozhi_chat_free_info(&info);
}

/* 返回 ESP_OK = 进入 CHATTING 前 CANDIDATE OK（channel 事件再置 CHATTING）；
 * 失败 = 已就地收尾回 IDLE 并上抛 ERROR。 */
static esp_err_t session_setup(void)
{
    esp_err_t ret = ESP_OK;
    set_state(XZ_STATE_PREPARING);

    /* 1. ext 独占租借：借出服务句柄（WAV/录音/通话/MP3 全部被拒） */
    esp_codec_dev_handle_t spk = NULL, mic = NULL;
    ESP_RETURN_ON_ERROR(audio_service_ext_claim_exclusive(&spk, &mic),
                        TAG, "codec busy");
    if (mic == NULL) {
        ESP_LOGE(TAG, "BSP has no mic, xiaozhi unsupported");
        audio_service_ext_release_exclusive();
        notify(XZ_EVT_ERROR, "no mic", 0, ESP_ERR_NOT_SUPPORTED);
        set_state(XZ_STATE_IDLE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_spk = spk;
    s_mic = mic;

    /* 2. av_processor audio_manager（0.7.0：设备 IO 走回调，喂借出句柄的
     *    读写实现；格式经 audio_manager_config_set_same_io_format 设置） */
    audio_manager_config_t acfg = DEFAULT_AUDIO_MANAGER_CONFIG();
    acfg.play_io.write_cb = xz_play_write_cb;
    acfg.rec_io.read_cb   = xz_rec_read_cb;
    audio_manager_config_set_same_io_format(&acfg, XZ_SAMPLE_RATE, XZ_BITS,
                                            XZ_BOARD_CHANNELS);
    snprintf(acfg.mic_layout, sizeof(acfg.mic_layout), "%s", XZ_MIC_LAYOUT);
    ESP_GOTO_ON_ERROR(audio_manager_init(&acfg), fail, TAG, "audio_manager_init");

    /* 3. open 两个 dev（16k 全双工）+ 增益在 open 之后（ES8389 语义） */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = XZ_SAMPLE_RATE,
        .channel = XZ_BOARD_CHANNELS,
        .bits_per_sample = XZ_BITS,
    };
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(spk, &fs) == ESP_CODEC_DEV_OK
                        ? ESP_OK : ESP_FAIL, fail, TAG, "open spk");
    ESP_GOTO_ON_ERROR(esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK
                        ? ESP_OK : ESP_FAIL, fail, TAG, "open mic");
    esp_codec_dev_set_out_vol(spk, 70);
    esp_codec_dev_set_in_gain(mic, 24.0);

    /* 4. 下行 playback（URL 播放入口，v1 不主动 play；0.7.0 句柄制） */
    audio_play_config_t pcfg = DEFAULT_AUDIO_PLAY_CONFIG();
    ESP_GOTO_ON_ERROR(audio_playback_open(&pcfg, &s_play), fail, TAG, "playback_open");

    /* 5. 上行 recorder：Opus 编码 + AFE（DIRECT 模式无唤醒词）
     *    ★ 0.7.0：无 ai_mode_wakeup 字段 ⇒ algo_mask 不置 AI_MODE_WAKEUP 即
     *      DIRECT；mask 必须非 0 才会走 AFE 路径（=0 是 raw 直通）⇒
     *      NS/VAD 显式置位（AEC 已关，见下方 §53 注）。
     *    ★ DEFAULT 宏是纯初始化列表，只能用于声明处赋值，不能作表达式赋值。
     *    ★ M3.3 上机验证点：model 分区未烧 srmodels，DIRECT 模式不需要
     *      wakenet；若 esp_srmodel_init("model") 在空分区上非良性失败，
     *      在此加自定义 afe_config 规避。 */
    audio_recorder_config_t rcfg = DEFAULT_AUDIO_RECORDER_CONFIG();
    av_processor_afe_config_t afe = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    /* ★ §53：AEC 必须关（MM 布局无参考通道；MR 参考错配实测把人声消掉）。
     * NS/VAD 保留。若未来实测 ch1=真回采再恢复 AEC_ENABLE。 */
    afe.mode.afe.algo_mask = AV_PROCESSOR_AFE_FLAG_NS_ENABLE
                           | AV_PROCESSOR_AFE_FLAG_VAD_ENABLE;
    rcfg.afe_config = afe;
    av_processor_encoder_config_t *enc = &rcfg.encoder_cfg;
    enc->format = AV_PROCESSOR_FORMAT_ID_OPUS;
    enc->params.opus.audio_info.sample_rate = XZ_SAMPLE_RATE;
    enc->params.opus.audio_info.sample_bits = XZ_BITS;
    enc->params.opus.audio_info.channels = 1;
    enc->params.opus.audio_info.frame_duration = XZ_FRAME_MS;
    enc->params.opus.enable_vbr = false;
    enc->params.opus.bitrate = XZ_OPUS_BITRATE;
    rcfg.recorder_event_cb = NULL;               /* v1 不消费 AFE 事件 */
    rcfg.recorder_ctx = NULL;
    rcfg.recorder_task_config.task_core = 1;
    ESP_GOTO_ON_ERROR(audio_recorder_open(&rcfg, &s_recorder), fail, TAG, "recorder_open");

    /* 6. 下行 feeder：Opus 解码（16k/1ch，与协议 hello 参数一致）
     *    ★ 0.7.0：open 后停在 IDLE，必须显式 start。 */
    audio_feeder_config_t fcfg = DEFAULT_AUDIO_FEEDER_CONFIG();
    av_processor_decoder_config_t *dec = &fcfg.decoder_cfg;
    dec->format = AV_PROCESSOR_FORMAT_ID_OPUS;
    dec->params.opus.audio_info.sample_rate = XZ_SAMPLE_RATE;
    dec->params.opus.audio_info.sample_bits = XZ_BITS;
    dec->params.opus.audio_info.channels = 1;
    dec->params.opus.audio_info.frame_duration = XZ_FRAME_MS;
    fcfg.feeder_task_config.task_core = 1;
    ESP_GOTO_ON_ERROR(audio_feeder_open(&fcfg, &s_feeder), fail, TAG, "feeder_open");
    ESP_GOTO_ON_ERROR(audio_feeder_start(s_feeder), fail, TAG, "feeder_start");

    /* 7. 上行任务 + PSRAM 读缓冲 */
    s_uplink_buf = heap_caps_calloc(1, XZ_REC_READ_SIZE, MALLOC_CAP_SPIRAM);
    ESP_GOTO_ON_FALSE(s_uplink_buf, ESP_ERR_NO_MEM, fail, TAG, "uplink buf");
    if (xTaskCreate(xz_uplink_task, "xz_uplink", 3072, NULL, 5, &s_uplink_task)
        != pdPASS) {
        s_uplink_task = NULL;
        ESP_GOTO_ON_ERROR(ESP_ERR_NO_MEM, fail, TAG, "uplink task");
    }

    /* 8. esp_xiaozhi 会话：init → 订阅连接类事件 → 激活检查 → start → open channel
     *    （hello 默认 opus/16k/1ch/60ms；连接事件走 esp_event 总线，见回调注释） */
    ESP_GOTO_ON_ERROR(chat_setup(), fail, TAG, "chat_init");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS,
                                                 ESP_EVENT_ANY_ID,
                                                 xz_esp_event_handler, NULL),
                      fail, TAG, "register events");
    /* 8.5 激活检查（非致命）：未激活设备 HTTP info 会带 activation_code，
     *     UI 收 XZ_EVT_ACT_CODE 大字展示，用户去 xiaozhi.me 输入 */
    check_activation();
    ESP_GOTO_ON_ERROR(esp_xiaozhi_chat_start(s_chat), fail, TAG, "chat_start");
    ESP_GOTO_ON_ERROR(esp_xiaozhi_chat_open_audio_channel(s_chat, NULL, NULL, 0),
                      fail, TAG, "open_audio_channel");

    /* ★ wake word detect：服务器开启对话回合的扳机（实测缺它 = 全聋）。
     *   listen start/stop 改由 PTT 按钮在 manual 模式下显式控制
     *   （每轮边界可追溯；realtime 自由讲 + tts-stop 重开票实测不可靠：
     *   只有紧跟着的第一轮能被听见，见 §47/§48）。 */
    ESP_GOTO_ON_ERROR(
        esp_xiaozhi_chat_send_wake_word(s_chat, "你好小智"),
        fail, TAG, "send wake word");
    ESP_LOGI(TAG, "session ready (wake detect); hold PTT to talk (manual)");

    /* CHATTING 由 AUDIO_CHANNEL_OPENED 事件置位 */
    ESP_LOGI(TAG, "session setup done, waiting audio channel");
    return ESP_OK;

fail:
    esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                 xz_esp_event_handler);
    notify(XZ_EVT_ERROR, "session setup failed", 0, ESP_FAIL);
    session_teardown();
    return ret;
}

/* ============================================================
 * 服务任务
 * ============================================================ */

static void xz_task(void *arg)
{
    static xz_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (cmd.id == XZ_CMD_START && s_state == XZ_STATE_IDLE) {
            session_setup();
        } else if (cmd.id == XZ_CMD_STOP &&
                   (s_state == XZ_STATE_PREPARING || s_state == XZ_STATE_CHATTING)) {
            session_teardown();
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void xiaozhi_service_init(void)
{
    s_cmd_q = xQueueCreate(4, sizeof(xz_cmd_t));
    MEM_FAIL_FAST(s_cmd_q, TAG, "xQueueCreate(4, xz_cmd_t)", 4 * sizeof(xz_cmd_t));
    BaseType_t ok = xTaskCreate(xz_task, "xz_task", 4096, NULL, 5, &s_task);
    MEM_FAIL_FAST(ok == pdPASS, TAG, "xTaskCreate(xz_task, 4096)", 4096);
    /* §58 A1：相机 explain 客户端 + 结果上桥（explain 任务上下文 → 本服务
     * 的 XZ_EVT_TEXT 通道 → AI 浮窗对话记录）。失败不拦启动（无相机也能对话） */
    if (ai_camera_init() == ESP_OK) {
        ai_camera_set_result_cb(on_ai_camera_result);
    } else {
        ESP_LOGW(TAG, "ai_camera_init failed (vision tools disabled)");
    }
    ESP_LOGI(TAG, "xiaozhi_service ready (mic_layout=%s ch=%d, no wake word)",
             XZ_MIC_LAYOUT, XZ_BOARD_CHANNELS);
}

esp_err_t xiaozhi_service_start(void)
{
    if (s_state != XZ_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* audio_service 忙（WAV 播放/录音/通话/MP3 ext 占用）⇒ 拒；
     * UI 层负责先停播放再 start（S5 §2.4：链路串行不并发） */
    if (!audio_service_mic_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    xz_cmd_t cmd = { .id = XZ_CMD_START };
    return (xQueueSend(s_cmd_q, &cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t xiaozhi_service_stop(void)
{
    if (s_state != XZ_STATE_PREPARING && s_state != XZ_STATE_CHATTING) {
        return ESP_ERR_INVALID_STATE;
    }
    xz_cmd_t cmd = { .id = XZ_CMD_STOP };
    return (xQueueSend(s_cmd_q, &cmd, 0) == pdPASS) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

xiaozhi_state_t xiaozhi_service_get_state(void)
{
    return s_state;
}

/* ---- PTT 按住说话（manual 模式，回合边界由 UI 显式控制）----
 * 这里只置期望态；协议消息（listen start/stop）由上行任务对齐发出。
 * 非会话期 ptt_start 返回 INVALID_STATE（UI 提示先按 TALK）。 */
esp_err_t xiaozhi_service_ptt_start(void)
{
    if (s_state != XZ_STATE_CHATTING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ptt_want = true;
    return ESP_OK;
}

void xiaozhi_service_ptt_stop(void)
{
    s_ptt_want = false;
}

/* ---- §56 音量：对话态作用于借出的 spk 句柄（非会话期为 no-op——
 *      播放态由 audio_service SET_VOLUME 通道负责，见 ai_speaker）---- */
esp_err_t xiaozhi_service_set_volume(int vol)
{
    if (s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_set_out_vol(s_spk, vol) == ESP_CODEC_DEV_OK)
               ? ESP_OK : ESP_FAIL;
}

/* ---- §58 A1 Vision：透出 MCP initialize 响应缓存的 explain 端点。
 *      非会话期 s_chat 为 0 → INVALID_STATE（工具只在会话中被调用） ---- */
esp_err_t xiaozhi_service_get_vision_endpoint(char *url, size_t url_size,
                                              char *token, size_t token_size)
{
    if (s_chat == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_xiaozhi_chat_get_vision_endpoint(s_chat, url, url_size, token, token_size);
}

void xiaozhi_service_set_evt_cb(xiaozhi_evt_cb_t cb)
{
    s_evt_cb = cb;
}
