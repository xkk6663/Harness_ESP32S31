/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "lvgl.h"

lv_obj_t *overlay_wp_particles_build(lv_obj_t *parent);
void overlay_wp_particles_stop(void);

/**
 * @brief 取粒子画布对象（可能为 NULL）
 *
 * 用途：调用方需要把触摸事件挂到**真正接收触摸的那一层**时用。
 * 画布铺满全屏且 CLICKABLE，LVGL 命中测试会停在它上面，
 * 挂在父级的 PRESSED/PRESSING 收不到事件（曾导致上拉要划两次，见 F31）。
 */
lv_obj_t *overlay_wp_particles_canvas(void);
