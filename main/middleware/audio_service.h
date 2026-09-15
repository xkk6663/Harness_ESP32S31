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
#include "middleware/fs_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUFFER_SIZE     (1024)
#define SAMPLE_RATE     (16000)
#define DEFAULT_VOLUME  (70)
#define RECORDING_LENGTH (160)   /* 160 * 1024 bytes @16k/16bit mono ≈ 5.12 s */
#define REC_FILENAME    FS_MNT_PATH"/recording.wav"

#define AUDIO_CMD_PATH_MAX  256

/* ---- 命令 ---- */
typedef enum {
    AUDIO_CMD_PLAY_FILE = 0,   /* 播放 path 指定的 WAV */
    AUDIO_CMD_STOP,            /* 停止当前播放 */
    AUDIO_CMD_SET_REPEAT,      /* value: 1=repeat on, 0=off */
    AUDIO_CMD_SET_VOLUME,      /* value: 0..90 */
    AUDIO_CMD_START_RECORD,    /* 录制到 path */
    AUDIO_CMD_STOP_RECORD,     /* 停止录音（当前固定时长，预留） */
} audio_cmd_id_t;

typedef struct {
    audio_cmd_id_t id;
    char path[AUDIO_CMD_PATH_MAX];
    int  value;
} audio_cmd_t;

/* ---- 事件 ---- */
typedef enum {
    AUDIO_EVT_PLAY_DONE = 0,   /* 播放结束（自然播完或被 stop） */
    AUDIO_EVT_RECORD_DONE,     /* 录音结束 */
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

#ifdef __cplusplus
}
#endif
