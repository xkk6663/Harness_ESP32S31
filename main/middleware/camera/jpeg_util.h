/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file jpeg_util.h
 * @brief 中间层：JPEG **软编码**（RGB888 → JPEG 码流）—— `espressif__esp_new_jpeg`
 *
 * 方案：docs/项目规划/S8_人脸识别打卡系统方案.md §3 约束②/§5.4
 *
 * ===========================================================================
 *  ★ 为什么需要"软编码"这一层（这是 S8 方案里最反直觉的一处结论）
 * ===========================================================================
 *  OV3660 自带 JPEG 编码器，720p 的 JPEG 码流是 **sensor 直出**的
 *  （`capture_service.c` 拿到 V4L2 buffer 时它**已经是压缩码流**，直接 fwrite 即可）。
 *  ⇒ 想"在照片上画姓名与时间"就晚了 —— 码流外面没法再画任何东西。
 *
 *  要在像素上叠字，只有两条路：
 *    · 拿到**未压缩**帧（240x240 RGB565/RGB888）→ 自己编码 —— 就是本模块；
 *    · 放弃水印，只用文件名 + CSV 记录时间（S8-2 的降级形态）。
 *
 *  ⇒ 结论：`punch_service`（打卡存证）走本模块；`capture_service`（720p 手动拍照）
 *    继续走 sensor 硬编，**两者互不影响、也不互相调用**。
 *
 * ===========================================================================
 *  ★ 依赖与取舍
 * ===========================================================================
 *  - 依赖 `managed_components/espressif__esp_new_jpeg`（本工程已在 PRIV_REQUIRES 里
 *    有 `esp_driver_jpeg`；本文件只需要 esp_jpeg_* 这套头/库）。
 *  - **编码器句柄是常驻缓存的**（`jpeg_enc_open` 内部要建 ~30 KB 的上下文）；
 *    240x240 是定长输入 ⇒ 每次 `close/open` 纯属浪费，且会在 PSRAM 里反复打洞。
 *    代价是必须自己加锁：打卡任务与录入任务可能并发。
 *  - 输出缓冲由**调用方提供**：本模块不持有任何大缓冲
 *    （240x240 Q80 实测典型 15~25 KB，调用方按 `JPEG_UTIL_OUT_MAX` 给就行）。
 *
 *  ★★ 输入缓冲**必须 16 字节对齐**（esp_jpeg 的硬要求，官方注释原话
 *     "The buffer must be aligned 16 byte"）。本工程所有帧缓冲都是
 *     `heap_caps_aligned_alloc(64, ...)` ⇒ 天然满足。若将来有人传栈数组进来，
 *     请自己用 `alignas(16)` 或改用 `jpeg_calloc_align`。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 输出缓冲的建议容量。
 * ★ 240x240 Q80 RGB888 实测典型 15~25 KB；64 KB 是 3 倍以上余量。
 *   （差异极大的图 —— 比如全屏高频噪点 —— 会更大；本模块对"编不下"是
 *     **如实返回错误**而不是静默截断，见 `jpeg_util_encode_rgb888` 的返回码。）
 */
#define JPEG_UTIL_OUT_MAX   (64 * 1024)

/**
 * @brief 把一张 RGB888 图像编码成 JPEG 码流。
 *
 * @param rgb      输入像素，`w * h * 3` 字节，顺序 R,G,B。**必须 16 字节对齐**。
 * @param w,h      尺寸（需 > 0；非 16 的倍数也支持，但某些色度抽样下质量略降）
 * @param quality  1..100，**越大质量越高**（注意：与 OV3660 的语义**相反**，
 *                 编码器这里是常规语义；ov3660_fmt.c 那边是"越小越好"）。
 *                 建议 70~85。
 * @param out      输出缓冲
 * @param out_cap  输出缓冲容量
 * @param out_len  输出：实际写入的字节数（失败时置 0）
 *
 * @return ESP_OK               成功
 *         ESP_ERR_INVALID_ARG  参数非法 / 输入未对齐 / 输出容量不足
 *         ESP_FAIL             编码器报错（细节见日志）
 *
 * @note 线程安全（内部互斥）。**会阻塞**（240x240 典型 20~60 ms），
 *       不得在 LVGL 线程或 cam_task 的帧回调里调。
 */
esp_err_t jpeg_util_encode_rgb888(const uint8_t *rgb, int w, int h, int quality,
                                  uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * @brief 释放常驻编码器（幂等）。一般**不需要**调 —— 它是常驻复用的小对象；
 *        仅在"要彻底归还 PSRAM"的场景（如长时录像前）才有必要。
 */
void jpeg_util_deinit(void);

#ifdef __cplusplus
}
#endif
