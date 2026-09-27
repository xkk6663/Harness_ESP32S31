/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file media_player_service.h
 * @brief 中间层：本地音乐播放服务（esp_player + esp_audio_render 链路，S5 §4.6/M2）
 *
 * 设计（与 audio_service 的命令队列模型一致）：
 *  - media_player_service_init() 创建常驻任务与命令队列；esp_player 链路
 *    （media_lib 注册 + gmf pool + render + player）在**首次 PLAY 时惰性建立**，
 *    避免开机时间与常驻内存。
 *  - 输出不自建 codec 句柄：走 audio_service ext 流式协议
 *    （ext_claim(44100/2/16) → ext_write → ext_release），从设计上消灭
 *    "同一 I2S 双句柄" P0（S5 §2.2/2.3）。SET_VOLUME 照常生效（同句柄）。
 *  - 事件回调在服务任务上下文执行（回调内操作 LVGL 前须自行 bsp_display_lock）。
 *
 * 占用语义：PLAY 成功 ⇒ audio_service 处于 ext STREAM 占用；
 * FINISHED/STOP/ERROR 均会 release。播放/录音/通话/小智在此期间被 audio_service 拒绝。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_EVT_STARTED = 0,   /* run 已发出（首次 PLAY 惰性 init 成功后） */
    MP_EVT_FINISHED,      /* 自然播完或被 STOP */
    MP_EVT_ERROR,         /* 链路建立失败 / 播放器错误 */
} mp_evt_id_t;

typedef void (*mp_evt_cb_t)(mp_evt_id_t evt);

/**
 * @brief 初始化服务：创建任务与命令队列（esp_player 链路首次 PLAY 才建）
 */
void media_player_service_init(void);

/**
 * @brief 播放一个音频文件（.mp3/.m4a/.flac 等解码器树内支持的格式）
 * @param path littlefs 绝对路径（如 /storage/music/a.mp3）
 * @return ESP_OK = 已受理（Started 事件稍后上抛）；
 *         ESP_ERR_INVALID_STATE = audio_service 忙（WAV/录音/通话/小智占用中）；
 *         其它 = 链路建立失败
 * @note  任意任务上下文可调；重复 PLAY = 先 STOP 再起新曲。
 */
esp_err_t media_player_service_play(const char *path);

/**
 * @brief 停止播放并归还 codec（未播放时无害）
 */
void media_player_service_stop(void);

/**
 * @brief 是否正在播放（或已请求播放、尚未真正起管线）
 * @note  ★ 语义含 pending 段：play() 入队成功即算"播放中"，直到 STOP/FINISHED/
 *        ERROR 收尾。UI 轮询（tab_files refresh 回浏览态）必须用它——若只看
 *        管线 true 态，50ms 建链窗口里会误判"已结束"而发出误杀 STOP
 *        （2026-09-26 实证：PLAY 后 ~30ms 被 refresh 回浏览打断）。
 */
bool media_player_service_is_playing(void);

/**
 * @brief 注册事件回调（覆盖式；在服务任务上下文调用）
 */
void media_player_service_set_evt_cb(mp_evt_cb_t cb);

#ifdef __cplusplus
}
#endif
