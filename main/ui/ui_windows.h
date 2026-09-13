/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_windows.h
 * @brief WAV 播放弹窗 + 录音 tab 按钮事件
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 录音 tab 按钮事件回调 */
void rec_play_event_cb(lv_event_t *e);
void rec_stop_event_cb(lv_event_t *e);
void rec_event_cb(lv_event_t *e);

/**
 * @brief 注册音频事件回调（解锁播放/录音按钮）
 */
void ui_windows_register_audio_events(void);

/**
 * @brief 打开 WAV 播放弹窗（调试用）
 */
void show_window_wav(const char *path);

#ifdef __cplusplus
}
#endif
