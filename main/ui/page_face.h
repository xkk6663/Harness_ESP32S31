/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 人脸库管理页（S8-1）：N/10 名额 + 姓名列表 + 删除。入口在 SYS 页。 */
extern const Page_t Page_Face;

#ifdef __cplusplus
}
#endif
