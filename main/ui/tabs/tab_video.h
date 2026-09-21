/*
 * SPDX-FileCopyright-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_video.h
 * @brief 视频播放 Tab
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_video_build(lv_obj_t *parent, lv_group_t *g);
void tab_video_destroy(void);

#ifdef __cplusplus
}
#endif
