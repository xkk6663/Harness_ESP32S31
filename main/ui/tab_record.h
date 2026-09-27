/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_record.h
 * @brief REC tab：本地录音（一键录/停，存 /sdcard/music，FILES 页回放）。
 *        2026-09-26 重建版——原版（page_wav_player + record_service 上云链）
 *        已随 call/rec 阉割删除，本版为纯本地闭环。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_record_build(lv_obj_t *screen, lv_group_t *group);
void tab_record_destroy(void);

#ifdef __cplusplus
}
#endif
