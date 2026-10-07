/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_files.h
 * @brief S4 文件管理 tab（page_main tabview 第 8 页，SYS 前）：浏览 / 删除 /
 *        照片预览 / SET WALLPAPER / .wav 跳页回放（main 迁移版）。
 *
 * 激活语义（page_main 的 tab_changed_event 里调，**必须在 page_main_refresh_group()
 * 之前** —— 那个函数在触摸方案下整段 early-out，F44/§29 同款教训）：
 *   - set_active(true)：BROWSE 最近目录（首次 = /sdcard）；
 *   - set_active(false)：预览态复位。
 * ★ main 适配：无 camera viewfinder 共享资源，不再需要 tab_video_set_active(false)。
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
