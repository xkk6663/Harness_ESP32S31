/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file audio_service.h
 * @brief 中间层：音频服务（FreeRTOS 常驻任务 + 命令队列模型）
 *
 * 设计：
 *  - audio_service_init() 创建常驻 audio_task 和命令队列；
 *  - UI 层不直接操作 codec、不直接 xTaskCreate，而是通过
 *    audio_service_post_cmd() 下发命令；
 *  - 播放/录音完成后，通过注册的事件回调通知 UI（在音频任务上下文调用，
 *    回调内部需自行 bsp_display_lock 操作 LVGL）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "middleware/periph/fs_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUFFER_SIZE     (1024)
#define DEFAULT_VOLUME  (70)

#define AUDIO_CMD_PATH_MAX  256

/* ---- 命令 ---- */
typedef enum {
    AUDIO_CMD_PLAY_FILE = 0,   /* 播放 path 指定的 WAV */
    AUDIO_CMD_STOP,            /* 停止当前播放 */
    AUDIO_CMD_SET_REPEAT,      /* value: 1=repeat on, 0=off */
    AUDIO_CMD_SET_VOLUME,      /* value: 0..90 */
} audio_cmd_id_t;

typedef struct {
    audio_cmd_id_t id;
    char path[AUDIO_CMD_PATH_MAX];
    int  value;
} audio_cmd_t;

/* ---- 事件 ---- */
typedef enum {
    AUDIO_EVT_PLAY_DONE = 0,   /* 播放结束（自然播完或被 stop） */
} audio_evt_id_t;

/**
 * @brief 事件回调类型（在 audio_task 上下文执行）
 * @note  回调内操作 LVGL 对象前必须自行 bsp_display_lock/unlock。
 */
typedef void (*audio_evt_cb_t)(audio_evt_id_t evt);

/**
 * @brief 初始化音频服务：创建 codec、互斥锁、命令队列与常驻任务
 * @note  在 bsp_display_start() 之后、首次 UI 事件之前调用一次。
 */
void audio_service_init(void);

/**
 * @brief 下发一条音频命令（线程安全，从中断或任意任务上下文均可调用）
 */
void audio_service_post_cmd(const audio_cmd_t *cmd);

/**
 * @brief 注册事件回调（覆盖式，后注册者生效）
 */
void audio_service_set_evt_cb(audio_evt_cb_t cb);

/* ============================================================
 * ext 客户端协议（S5 §2.2/2.3）：audio_service 是 codec 的唯一所有者，
 * 其它音频客户端（MP3 播放、小智对话）以两种模式借用：
 *
 *  ① 流式（MP3 等逐块 PCM）：
 *       ext_claim(&fmt) → ext_write()… → ext_release()
 *     服务按 fmt open 自己的 spk dev；占用期间 WAV 播放被拒（BUSY），
 *     SET_VOLUME 照常生效（同句柄）。
 *
 *  ② 独占租借（小智 AFE 全双工管线）：
 *       ext_claim_exclusive(&spk, &mic) → 调用方自行 open/close 这两个
 *       服务句柄（16k 全双工、mic 布局自定）→ close 后 ext_release_exclusive()
 *     服务保持 IDLE 不 open；复用服务句柄 = 不再 BSP 二次 init，
 *     从根上避开"同一 I2S 双句柄"P0。归还得先 close 再 release。
 *
 * 任何占用期间：PLAY_FILE 命令被拒（BUSY 日志）；SET_VOLUME 仅 STREAM 态生效。
 * v1 无看门狗：调用方异常不 release 会永久占住——调用方须保证
 * STOP/错误路径必然走到 release（file_mgr/xiaozhi_service 单状态机兜底）。
 *
 * ★ 2026-09-27 call/rec 阉割：原 audio_stream_* 全双工路径（供 udp_voice/
 *   record_service 用的 16k 直通流）与 START/STOP_RECORD 录音命令已随
 *   call/rec 功能一并删除。mic 现在只经 ext_claim_exclusive 借出使用。
 */
typedef struct {
    int sample_rate;   /* 例 44100（MP3）/ 16000 */
    int channels;      /* 1/2 */
    int bits;          /* 16 */
} audio_ext_fmt_t;

typedef enum {
    AUDIO_EXT_NONE = 0,   /* 空闲 */
    AUDIO_EXT_STREAM,     /* ① 流式占用中（服务持有并 open dev） */
    AUDIO_EXT_EXCLUSIVE,  /* ② 独占租借中（句柄借出，服务不 open） */
} audio_ext_mode_t;

/**
 * @brief 流式占用 spk：成功后可 ext_write()；失败返回 ESP_ERR_INVALID_STATE（忙）或 open 错误
 */
esp_err_t audio_service_ext_claim(const audio_ext_fmt_t *fmt);

/**
 * @brief 流式写 PCM（仅 claim 后有效；内部拿锁，任意任务上下文可调）
 */
esp_err_t audio_service_ext_write(const void *pcm, size_t bytes);

/**
 * @brief 归还（close dev）；未占用时无害
 */
void audio_service_ext_release(void);

/**
 * @brief 独占租借：返回服务持有的底层 dev（mic 可为 NULL，视 BSP_CAPS_AUDIO_MIC）。
 *        成功后调用方 open/close 全权负责；失败返回 ESP_ERR_INVALID_STATE（忙）
 */
esp_err_t audio_service_ext_claim_exclusive(esp_codec_dev_handle_t *spk,
                                            esp_codec_dev_handle_t *mic);

/**
 * @brief 归还独占（调用方必须已 close 两个句柄）；未占用时无害
 */
void audio_service_ext_release_exclusive(void);

/**
 * @brief mic 是否空闲（录音/通话入口闸门；ext 任何占用期间为 false）
 */
bool audio_service_mic_available(void);

#ifdef __cplusplus
}
#endif
