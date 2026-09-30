/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file xiaozhi_service.h
 * @brief 中间层：小智对话服务（esp_xiaozhi + av_processor 编排，S5 §4.6/M3）
 *
 * 设计（S5 §4.6.3 状态机）：
 *   IDLE --start()--> PREPARING（ext_claim_exclusive 借 codec → av_processor
 *   音频管线 → esp_xiaozhi 会话）--> CHATTING --stop()--> RELEASING --> IDLE。
 *
 * 音频所有权：对话期通过 audio_service ext 独占租借持有 spk/mic 两个
 * esp_codec_dev 句柄（服务自行 open/close）；期间 WAV/录音/通话/MP3 全部
 * 被 audio_service 拒绝。退出必经 ext_release_exclusive 归还。
 *
 * v1 约定（2026-09-26 拍板）：
 *  - 无唤醒词：按键触发（ai_mode_wakeup=false，不需要 srmodels）；
 *  - 无 MCP 点歌（mcp_engine=NULL）；
 *  - AEC 预案 M3.2 裁决（XZ_MIC_LAYOUT 宏：MR=codec 内部回采 / MM=单麦降级）。
 *
 * 线程纪律（P4）：所有回调只投队列/置 volatile 快照，绝不碰 LVGL；
 * UI 用 lv_timer 轮询 get_state() 与事件队列。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XZ_STATE_IDLE = 0,
    XZ_STATE_PREPARING,   /* 起管线 + 连服务器（1~2s，事件通知到位） */
    XZ_STATE_CHATTING,
    XZ_STATE_RELEASING,
} xiaozhi_state_t;

typedef enum {
    XZ_EVT_STATE = 0,     /* 状态机变化 */
    XZ_EVT_TEXT,          /* 对话文本；text/role 有效（指针回调期有效） */
    XZ_EVT_TTS_STATE,     /* TTS start/stop/sentence_start；text 仅 sentence_start */
    XZ_EVT_ERROR,         /* err + text(=source 提示) */
    XZ_EVT_ACT_CODE,      /* 设备未激活；text=激活码（去 xiaozhi.me 输入） */
    XZ_EVT_EMOJI,         /* §58 A2：LLM 情绪标签；text = "happy"/"sad"/... */
} xiaozhi_evt_id_t;

typedef struct {
    xiaozhi_evt_id_t id;
    const char *text;     /* XZ_EVT_TEXT/XZ_EVT_TTS_STATE/XZ_EVT_ERROR 时有效 */
    int role;             /* 0=user 1=assistant（XZ_EVT_TEXT） */
    esp_err_t err;        /* XZ_EVT_ERROR */
} xiaozhi_evt_t;

typedef void (*xiaozhi_evt_cb_t)(const xiaozhi_evt_t *evt);

/* ★★ §73.27 方案 B：会话内在线点歌（音乐不再抢占会话）----
 * 点歌走 av_processor audio_playback（闲置句柄 s_play 的"URL 播放入口"），
 * 内部 = esp_audio_simple_player 解码 → 汇入 xz_play_write_cb 同一条下行流
 * → 与 TTS 同一 s_spk 出声。会话全程活着，LLM 可随时 stop/换曲。
 * 优先级策略：TTS feeder > 音乐 playback —— 小智说话时音乐被音频策略
 * 自动压住（输出 gate 静音、进度照走），说完自动恢复。 */
typedef enum {
    XZ_MUSIC_STARTED = 0,   /* 播放器实际起播（simple_player 任务上下文回调） */
    XZ_MUSIC_FINISHED,      /* 自然播完 */
    XZ_MUSIC_ERROR,         /* 链路失效/解码失败等 */
} xz_music_evt_id_t;

typedef void (*xz_music_evt_cb_t)(xz_music_evt_id_t evt, esp_err_t err);

/**
 * @brief 初始化：建任务/队列，不占音频资源
 */
void xiaozhi_service_init(void);

/**
 * @brief 进入对话（异步）。MP3/WAV 播放中调用会被拒（先停播放再试）
 * @return ESP_OK=已受理（结果经 XZ_EVT_STATE/XZ_EVT_ERROR 上抛）；
 *         ESP_ERR_INVALID_STATE=已在对话中或 audio/audio_service 忙
 */
esp_err_t xiaozhi_service_start(void);

/**
 * @brief 退出对话（异步；RELEASING 完成后回 XZ_STATE_IDLE）
 */
esp_err_t xiaozhi_service_stop(void);

xiaozhi_state_t xiaozhi_service_get_state(void);

/**
 * @brief PTT 按住说话开始（manual 模式 listen start）。
 *        只置期望态，协议消息由上行任务发出；非会话期返回 INVALID_STATE。
 *        ★ §52：TTS 播放中调用 = **打断**（barge-in）——上行任务改发
 *        abort_speaking，待服务器 tts stop 复位回合后再补 listen start。
 *        用户无需等它说完，按住即插话
 */
esp_err_t xiaozhi_service_ptt_start(void);

/**
 * @brief PTT 松开（manual 模式 listen stop）。任意时刻可调，幂等
 */
void xiaozhi_service_ptt_stop(void);

/**
 * @brief 对话态设置扬声器音量（作用于会话借出的 spk 句柄，0~90）。
 *        非会话期返回 INVALID_STATE（播放态音量走 audio_service 通道）。
 *        §56：由 ai_speaker MCP 工具三通道之一调用
 */
esp_err_t xiaozhi_service_set_volume(int vol);

/**
 * @brief 注册事件回调（覆盖式；在服务任务上下文调用，勿阻塞勿碰 LVGL）
 */
void xiaozhi_service_set_evt_cb(xiaozhi_evt_cb_t cb);

/**
 * @brief §58 A1：透出 MCP initialize 响应缓存的 vision explain 端点
 *        （vendor esp_xiaozhi_chat_get_vision_endpoint 的服务层包装）。
 *        非会话期 INVALID_STATE；端点未知 NOT_FOUND。
 *        供 ai_camera 的 explain 任务在上传前现取（token 会随会话刷新）
 */
esp_err_t xiaozhi_service_get_vision_endpoint(char *url, size_t url_size,
                                              char *token, size_t token_size);

/* ---- §73.27 会话内在线点歌（方案 B）---- */

/**
 * @brief 会话内播放一首 URL 音频（异步投队列，xz_task 执行）。
 *        只在 XZ_STATE_CHATTING 有效（点歌是会话内动作）。
 *        换曲 = 直接再次调用（av_processor 内部先停旧流再起新流）。
 *        起播/播完/失败经 xz_music_evt_cb 上抛；播放期间上行被闸
 *        （歌声不再回传服务器），按 PTT = 停音乐 + 正常说话。
 * @return ESP_OK=已受理；INVALID_ARG=空 url；INVALID_STATE=非会话期/队列满
 */
esp_err_t xiaozhi_service_music_play(const char *url);

/**
 * @brief 停止当前音乐（异步投队列）。幂等：没在播也返回 OK
 */
esp_err_t xiaozhi_service_music_stop(void);

/**
 * @brief 注册音乐播放事件回调（覆盖式）。回调跑在 simple_player 播放
 *        任务上下文——只置标志/转投，勿阻塞勿碰 LVGL
 */
void xiaozhi_service_set_music_evt_cb(xz_music_evt_cb_t cb);

/**
 * @brief 音乐是否在播（含起播前的建链窗口：以播放器事件为准，有轻微滞后）
 */
bool xiaozhi_service_music_playing(void);

#ifdef __cplusplus
}
#endif
