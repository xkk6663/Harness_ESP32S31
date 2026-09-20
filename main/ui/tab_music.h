/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_music.h
 * @brief 音乐播放 Tab
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_music_build(lv_obj_t *parent, lv_group_t *g);
void tab_music_destroy(void);

#ifdef __cplusplus
}
#endif
