/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file face_quality.h
 * @brief 中间层：人脸框质量门槛（S8-1）—— 录入/打卡的**唯一**判据来源
 *
 * 方案：docs/项目规划/S8_人脸识别打卡系统方案.md §5.6
 *
 * ===========================================================================
 *  ★ 为什么值得单独一个文件（它只有 3 个条件）
 * ===========================================================================
 *  这 3 个条件会被**两处**使用，而且两处的角色完全不同：
 *    · UI（CAM 页 ENROLL 态）：拿它当**引导文案**（"Move closer" / "Center the face"）；
 *    · 业务（S8-2 录入采样 / 打卡抓帧）：拿它当**准入闸门**（不过就不采）。
 *  如果各写一份，只要有人改了 "80 px" 而忘了改另一处，就会出现
 *  **"界面显示 Ready、按下去却说质量不合格"** —— 这正是本工程反复踩过的那类
 *  "两套状态机/两份真相源早晚不一致"的坑（见 tab_camera.c 的 FACE/MODEL 注释）。
 *  ⇒ 收敛成一个函数：引导和闸门**不可能**漂移。
 *
 *  ★ 为什么门槛按"画面尺寸的比例"表达（而不是 §5.6 表里的 80 px / 60 px）：
 *  §5.6 那两个数是**在 240×240 尺度上**定的。本工程同一份人脸框会经过两条路径
 *  （CAM 预览 240×240 / 打卡帧 240×240 / 将来录像 1280×720），写死像素值会在
 *  换分辨率时**静默失效**（比如 720p 下"人脸 ≥80 px"变成人人都过）。
 *  ⇒ 这里换算成比例：80/240 = 1/3、60/240 = 1/4。换分辨率时判据语义不变。
 *
 *  ★ 提示文案一律 ASCII：本工程的 UI_FONT（montserrat_14）**不含中文字形**，
 *  中文要额外挂 CJK 字体（见 ui_binfont.h 的使用禁区）。这一行是状态栏小字，
 *  不值得为它引一套字体；且与既有状态文案（"Start camera first" 等）风格一致。
 */

#pragma once

#include <stdbool.h>

#include "face_service.h"        /* vision_result_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 质量判定结果（含一句可直接上屏的提示） */
typedef struct {
    bool  has_face;      /* 传进来的框是否有效（无人脸 / 空框 ⇒ false） */
    bool  ok;            /* ★ 全部门槛通过 —— 这就是录入/打卡的准入判据 */
    int   score_pct;     /* 检测置信度 ×100（供状态栏显示 "hold still (52%)"） */
    const char *hint;    /* 一句人话提示；ok == true 时为 "Ready" */
} face_quality_t;

/**
 * @brief 对**一个**检测结果做质量判定（通常是画面里面积最大的那个框）。
 *
 * @param r        检测框（face_service_detect 的输出之一）；NULL 视为"无人脸"
 * @param frame_w  所在画面的宽（像素）；用于把门槛换算成比例
 * @param frame_h  所在画面的高（像素）
 * @param out      结果，不能为 NULL
 *
 * @note 纯函数、无状态、任意线程可调（不锁、不分配）。
 */
void face_quality_check(const vision_result_t *r, int frame_w, int frame_h,
                        face_quality_t *out);

#ifdef __cplusplus
}
#endif
