/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file capture_service.h
 * @brief 中间层：单次拍照事务（S2a）—— 切 JPEG 档 → 抓帧 → 落盘 → 归还预览
 *
 * 方案：docs/项目规划/人脸录用系统与SD卡媒体实施方案.md §4
 *
 * ★★ 与方案 §4.2 的**一处有意偏离**（这是好事，不是漏做）：
 *   方案原文让 capture_service 自己 open fd / S_FMT / REQBUFS / mmap。
 *   但方案写于"运行时切格式只能走 VIDIOC_S_FMT"这个**错误认知**之下
 *   （见 ov3660_fmt.h 文件头：S_FMT 在 DVP 上改不了格式）。
 *   如今这套 V4L2 动作已经在 camera_service_start_fmt_owner() 里**正确实现**了
 *   （含 S_SENSOR_FMT、G_FMT 回读真值、每次 start 重设 flip、S_FMT 与 sensor 的先后次序）。
 *   ⇒ 本模块**只做"落盘"这一件事**，取流一律转调 camera_service。
 *     重新实现一遍等于把上面那些坑再踩一遍，且两份实现会静默漂移。
 *
 * ★ 事务全过程（对应方案 §2.5.4 "接管 → 干活 → 归还"）：
 *   ① 检查 SD 卡已挂载 + 相机空闲（owner == NONE）
 *   ② 建目录、开文件（**先开文件**，绝不把 fopen 放进帧回调）
 *   ③ camera_service_start_fmt_owner(JPEG 1280x720, CAM_OWNER_MEDIA)
 *   ④ 帧回调里**丢前 CAP_WARMUP_SKIP 帧**（等 AE/AWB 稳定），第 N+1 帧当场 fwrite
 *   ⑤ camera_service_stop()
 *   ⑥ 关文件；失败则删掉半成品（不留 0 字节垃圾文件）
 *   ⑦ 记录耗时与字节数，供 UI 显示
 *
 * ★ 为什么敢在帧回调里做 fwrite（看似违反"回调内不得阻塞"）：
 *   这是**一次性**动作，全流程只写**一帧**，写完立刻停流 ⇒ 不存在"阻塞久了连续丢帧"
 *   的场景（丢帧也无所谓：我们只要这一张）。换来的是**零额外缓冲**——
 *   不需要为了"把帧交给另一个任务"而多占 200 KB+ PSRAM。
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SD 卡上的照片目录（相对 SDCARD_MNT_PATH） */
#define CAPTURE_DIR_NAME   "/photos"

/**
 * ★★ 路径缓冲长度（含 NUL）。**改小会直接编译失败**，不是风格问题：
 *
 *  本工程开了 `-Werror=format-truncation=`，GCC 会在**目标数组大小可知**时证明
 *  `snprintf` 不会截断。最坏情况 = (dir 最多 63) + '/' + (base 最多 39) + "_099.jpg"(8) + NUL = 112。
 *  历史上这里用的是 96 ⇒ GCC 报
 *  `'%s' directive output may be truncated writing up to 39 bytes into a region of size between 32 and 95`
 *  （注意：这个诊断**依赖内联**，所以单文件静态校验可能看不到，只有真实构建才炸）。
 *  ⇒ 取 128 留余量，并在 .c 里加 `_Static_assert` + 入口容量检查双重兜底。
 */
#define CAPTURE_PATH_MAX   128

typedef enum {
    CAPTURE_IDLE = 0,   /* 空闲，可以发起下一次 */
    CAPTURE_BUSY,       /* 事务进行中（UI 据此把【拍照】按钮置灰） */
} capture_state_t;

/** 最近一次（或正在进行的）拍照结果快照 */
typedef struct {
    esp_err_t err;            /* ESP_OK = 照片可用；否则为失败原因 */
    char      path[CAPTURE_PATH_MAX];   /* 落盘路径（失败时可能为空串） */
    uint32_t  bytes;          /* 文件字节数（= 该帧 JPEG 码流长度） */
    uint32_t  elapsed_ms;     /* 整个事务耗时 ms（切格式→抓帧→落盘→归还） */
    uint32_t  frames_seen;    /* 事务期间收到的 JPEG 帧总数（含被丢掉的预热帧） */
} capture_result_t;

/** 初始化（幂等）。不分配常驻任务 —— 见 .c 里的栈策略说明。 */
void capture_service_init(void);

/**
 * @brief 发起一次拍照（**非阻塞**，立即返回）
 *
 * @return ESP_OK             已受理，稍后轮询 state/result
 *         ESP_ERR_INVALID_STATE 已在拍 / 相机被 CAM 预览占用 / SD 卡没挂载
 */
esp_err_t capture_service_request(void);

/** 当前状态（任意线程可读） */
capture_state_t capture_service_state(void);

/** 取结果快照（tear-free 不可保证，但字段单调推进；UI 只在 IDLE 后读即可） */
void capture_service_get_result(capture_result_t *out);

/** 清掉上一次的结果（UI 显示完提示后调用，避免重复提示） */
void capture_service_clear_result(void);

#ifdef __cplusplus
}
#endif
