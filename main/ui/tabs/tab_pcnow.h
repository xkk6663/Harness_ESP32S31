/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_pcnow.h
 * @brief PC 副屏 Tab：实时显示 PC 端推送的曲名/歌手/进度/歌词/封面。
 *        数据由 pcnow_service 提供（本页只读快照，线程纪律同 tab_btsink）。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void tab_pcnow_build(lv_obj_t *parent, lv_group_t *g);

/** 销毁：停轮询/旋转定时器。LVGL 对象树由父容器统一删。 */
void tab_pcnow_destroy(void);

#ifdef __cplusplus
}
#endif
