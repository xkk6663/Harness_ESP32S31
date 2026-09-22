/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_audio.h
 * @brief AUDIO HUB Tab：本地播放 / 蓝牙推流 合并页（原 tab_music + tab_btpush）
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_audio_build(lv_obj_t *parent, lv_group_t *g);
void tab_audio_destroy(void);

#ifdef __cplusplus
}
#endif
