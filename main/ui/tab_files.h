/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_files.h
 * @brief S4 文件管理 tab（page_main tabview 第 6 页）：浏览 / 删除 / 照片预览 / AVI 播放。
 *
 * 激活语义（page_main 的 tab_changed_event 里调，**必须在 page_main_refresh_group()
 * 之前** —— 那个函数在触摸方案下整段 early-out，F44/§29 同款教训）：
 *   - set_active(true)：先 `tab_video_set_active(false)` 关掉 VIDEO 页取景
 *     （解码共享缓冲与相机守卫都要求），再 BROWSE 最近目录；
 *   - set_active(false)：若在播则 STOP；预览态复位。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_files_build(lv_obj_t *screen, lv_group_t *group);
void tab_files_destroy(void);
void tab_files_set_active(bool active);

#ifdef __cplusplus
}
#endif
