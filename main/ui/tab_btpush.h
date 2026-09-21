/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_btpush.h
 * @brief BTPUSH tab：把 SD 卡 MP3 通过 A2DP Source 推到蓝牙耳机
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_btpush_build(lv_obj_t *parent, lv_group_t *g);
void tab_btpush_destroy(void);

#ifdef __cplusplus
}
#endif
