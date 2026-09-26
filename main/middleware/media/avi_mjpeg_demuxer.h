/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file avi_mjpeg_demuxer.h
 * @brief AVI(MJPG) 读侧 —— 与 avi_mjpeg_muxer.c 写出的文件严格对齐的顺序解复用器。
 *
 * 设计要点（2026-09-26，S4 文件管理页 M3）：
 *
 * ★ **顺序播放，不建内存索引**。idx1 的用途在播放场景只有"跳转"，而 v1 只有
 *   顺序播放 ⇒ 直接从 movi 起点顺序读 chunk 头（'00dc'+size）即可，每帧 0 次
 *   fseek、总帧数取自 avih.dwTotalFrames（muxer close 时已回填）。省掉
 *   7200 帧 ×16 B 的索引内存与解析时间。
 *   （容错：若途中读到非 '00dc' 的 fourcc，向前扫描直到找到下一个 '00dc' 或 EOF。）
 *
 * ★ 头部字段用**固定偏移**读（muxer 的布局是常量并有 _Static_assert 锁死）：
 *     avih.dwMicroSecPerFrame @32   strh.dwScale @128   strh.dwRate @132
 *     avih.dwTotalFrames     @48
 *   fps 优先取 strh 的 rate/scale（muxer 用 scale=100/rate=fps_x100 表达实测
 *   平均帧率，比 avih 的整数 usec 更准），失败再退 avih。
 *
 * ★ 与 muxer 同款纪律：显式按字节拼小端，不用结构体直接 fread。
 * ★ 纯 C99 + stdio，不 include esp_/lvgl —— 与 muxer 一样保持可脱离 IDF 单测。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avi_demux avi_demux_t;

typedef enum {
    AVID_OK      = 0,
    AVID_EOF     = 1,       /* 正常播完 */
    AVID_ERR_ARG = -1,
    AVID_ERR_IO  = -2,
    AVID_ERR_FMT = -3,      /* 不是本 muxer 认可的 AVI(MJPG) 布局 */
} avid_result_t;

/**
 * 打开并校验头部（RIFF/AVI /strh=MJPG），读出 fps 与总帧数。
 * @return NULL = 打开或格式校验失败
 */
avi_demux_t *avi_demux_open(const char *path);

/** 取流信息（任何一项可为 NULL）。fps_x100 已含实测帧率（scale=100 口径）。 */
void avi_demux_get_info(const avi_demux_t *d,
                        uint32_t *fps_x100, uint32_t *total_frames,
                        uint32_t *width, uint32_t *height);

/**
 * 顺序读下一帧 JPEG 到调用方缓冲。
 * @param buf_len 缓冲容量；单帧超过容量的帧按损坏处理（返回 AVID_ERR_FMT）
 * @param out_len 实际帧长（可为 NULL）
 * @return AVID_OK / AVID_EOF / AVID_ERR_*
 */
avid_result_t avi_demux_next_frame(avi_demux_t *d, uint8_t *buf, uint32_t buf_len,
                                   uint32_t *out_len);

void avi_demux_close(avi_demux_t *d);

#ifdef __cplusplus
}
#endif
