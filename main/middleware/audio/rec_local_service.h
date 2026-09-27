/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rec_local_service.h
 * @brief 本地录音服务（REC tab 后端；2026-09-26 阉割 record_service 后重建）
 *
 * 闭环：REC tab 按下 → 本地录音 → 停止存到 SDCARD_MNT_PATH"/music" 下的
 * rec_XXX.wav → FILES tab 点开本地播放（esp_player WAV 容器 + PCM 解）。
 *
 * ★ 音频链路遵循 S5 ext 协议（与 MP3/小智同一套互斥规则）：
 *   rec_local 走 **ext_claim_exclusive**（独占租借）—— audio_service 是 codec
 *   唯一所有者，本服务只借用 mic 句柄（spk 借而不开），自 open/close；
 *   占用期间 MP3/小智/WAV 播放一律 BUSY，反之亦然。本服务不上云、不传网络。
 *
 * 线程模型（F44 / §39 教训）：所有阻塞 IO（SD fwrite / codec read）都在常驻
 * rec_task（内部 RAM 栈，F35）里做；UI 只投 START 命令 + 轮询查询 API。
 * stop() 置 volatile 标志，≤32ms（一个 chunk）内生效，不走命令队列——
 * 因为录音循环就跑在 START 命令的处理里，STOP 若入队会永远排在后面。
 *
 * ★ is_active() 含 pending 段（play() 同款语义，防 UI 轮询撞建链窗口误杀）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 事件（rec_task 上下文回调；UI 回调里不得碰 LVGL，只置标志/拷贝） */
typedef enum {
    REC_LOC_EVT_STARTED = 0,   /* 已 claim mic + 建文件，开始录 */
    REC_LOC_EVT_DONE,          /* 正常收尾，文件已存（path 有效） */
    REC_LOC_EVT_ERROR,         /* 失败收尾（占用忙/无卡/IO错），path 可能无效 */
} rec_loc_evt_id_t;

typedef void (*rec_loc_evt_cb_t)(rec_loc_evt_id_t evt, const char *path);

/**
 * @brief 初始化：创建命令队列 + 常驻 rec_task（4096，内部 RAM）
 */
void rec_local_service_init(void);

/**
 * @brief 请求开始录音（非阻塞，仅入队；结果经事件/查询 API）
 * @return ESP_OK 已受理；ESP_ERR_INVALID_STATE 正在录/队列满
 */
esp_err_t rec_local_start(void);

/**
 * @brief 请求停止并保存（置标志，≤32ms 生效；未在录时无害）
 */
void rec_local_stop(void);

/**
 * @brief 是否"已请求或正在"录音（含 pending 段；UI 轮询用它）
 */
bool rec_local_service_is_active(void);

/**
 * @brief 已录时长 ms（未在录时返回 0）
 */
uint32_t rec_local_elapsed_ms(void);

/**
 * @brief 最近一次收尾的文件路径（含 ERROR 时最后尝试的路径）
 */
const char *rec_local_last_path(void);

/**
 * @brief 最近一次收尾是否成功（DONE=true / ERROR=false）
 */
bool rec_local_last_ok(void);

/**
 * @brief 注册事件回调（覆盖式；可不注册，UI 纯轮询也能工作）
 */
void rec_local_set_evt_cb(rec_loc_evt_cb_t cb);

#ifdef __cplusplus
}
#endif
