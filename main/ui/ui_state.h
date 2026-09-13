/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_state.h
 * @brief UI 层全局 LVGL 对象指针集中声明
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tab 框架 */
extern lv_obj_t *tabview;
extern lv_obj_t *tab_btns;
extern lv_group_t *filesystem_group;
extern lv_group_t *recording_group;
extern lv_group_t *settings_group;

/* 音频播放/录音按钮（任务结束时解锁） */
extern lv_obj_t *play_btn;
extern lv_obj_t *play1_btn;
extern lv_obj_t *rec_btn;
extern lv_obj_t *rec_stop_btn;

#ifdef __cplusplus
}
#endif
