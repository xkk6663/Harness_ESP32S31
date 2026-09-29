/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file face_quality.c
 * @brief 人脸框质量门槛实现（判据出处：S8 方案 §5.6，全部换算为画面比例）
 */

#include "face_quality.h"

/* ---------------------------------------------------------------------------
 *  ★★ 全部门槛集中在这一段 —— 要调就只调这里，别在 UI/业务里另抄一份。
 *
 *  出处：docs/项目规划/S8_人脸识别打卡系统方案.md §5.6（240×240 尺度）
 *
 *  | 门槛       | §5.6 原值              | 这里的表达        | 240 尺度实际值 |
 *  |------------|------------------------|-------------------|----------------|
 *  | 检测置信度 | score ≥ 0.60           | FQ_SCORE_MIN      | 0.60           |
 *  | 人脸占比   | w ≥ 80 px（画面 1/3）  | w ≥ frame_w / 3   | 80 px          |
 *  | 居中度     | 中心距 < 60 px         | dx ≤ frame_w / 4  | 60 px          |
 *
 *  ★ 为什么居中度写成"≤ frame_w/4"而不是 "< frame_w/4"：§5.6 的原措辞是
 *    "距画面中心 < 60 px"，而 60 = 240/4 恰好是整除边界；用 ≤ 让边界值（正好 60）
 *    判为**通过**。差一个像素的判定对业务毫无影响，但**不要**把两边写成
 *    `<` 和 `≤` 混用 —— 那会让"到底哪边宽松"变成一件要读两遍才明白的事。
 * ------------------------------------------------------------------------- */
#define FQ_SCORE_MIN        0.60f
#define FQ_W_DIV            3       /* 人脸框宽 ≥ frame_w / FQ_W_DIV */
#define FQ_CENTER_DIV       4       /* 框中心偏移 ≤ frame_w / FQ_CENTER_DIV */

void face_quality_check(const vision_result_t *r, int frame_w, int frame_h,
                        face_quality_t *out)
{
    if (out == NULL) {
        return;
    }

    /* 默认 = "画面里没脸"。后面每个门槛不过时**就地 return**，
     * 所以 hint 只需在"最后通过"时改写一次，不会有漏赋值分支。 */
    out->has_face  = false;
    out->ok        = false;
    out->score_pct = 0;
    out->hint      = "No face in view";

    if (r == NULL || frame_w <= 0 || frame_h <= 0) {
        return;
    }
    /* ★ 显式挡 w/h <= 0：face_service_detect 在人脸不存在时**是否**会返回
     *   零尺寸条目是它内部的事（当前实现是直接少返回一条），但本函数是公共
     *   判据，不能依赖调用方的这个约定 —— 一个 w=0 的框会让下面所有比较
     *   得出"通过"（0 < frame_w/3 成立 ⇒ 反而被拦），逻辑正确性很脆。 */
    if (r->w <= 0 || r->h <= 0) {
        return;
    }

    out->has_face = true;
    /* +0.5 四舍五入：score 是 float，直接截断会把 0.599 显示成 59%、
     * 看起来"差一点点就过"（其实差 0.001）。显示值要能用来看趋势。 */
    out->score_pct = (int)(r->score * 100.0f + 0.5f);

    if (r->score < FQ_SCORE_MIN) {
        /* 置信度不足的常见成因是"转过脸 / 太远 / 背光"，提示统一说"别动"，
         * 因为对用户可操作的动作只有那一个（把脸正过来）。 */
        out->hint = "Hold still (low conf)";
        return;
    }

    if (r->w < frame_w / FQ_W_DIV) {
        out->hint = "Move closer";
        return;
    }

    {
        int cx = r->x + r->w / 2;
        int dx = cx - frame_w / 2;
        if (dx < 0) {
            dx = -dx;
        }
        if (dx > frame_w / FQ_CENTER_DIV) {
            out->hint = "Center the face";
            return;
        }
    }

    out->ok   = true;
    out->hint = "Ready";
}
