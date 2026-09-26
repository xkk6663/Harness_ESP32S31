/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file avi_mjpeg_demuxer.c
 * @brief AVI(MJPG) 顺序解复用（布局对齐 avi_mjpeg_muxer.c，见 .h 说明）。
 */

#include "avi_mjpeg_demuxer.h"

#include <string.h>
#include <stdlib.h>

/* ---- 与 muxer 对齐的固定偏移（muxer 里有 _Static_assert 锁死，这里只读） ---- */
/* ★ 偏移语义（对照 muxer 的写入流逐字节核算，2026-09-26 修）：
 *   muxer 的 OFF_STRH_SIZE=104 是 **size 字段**的偏移，'strh' fourcc 在它前面
 *   4 字节 = 100 —— 首版把 104 当 fourcc 偏移，所有视频都打不开（§33 补正）。
 *   流：'LIST'@88 + size@92 + 'strl'@96 → 'strh'@100 + size@104 → payload@108。 */
#define OFF_AVIH_FOURCC      24u    /* 'avih'（size@28，payload@32） */
#define OFF_AVIH_USEC        32u    /* avih.dwMicroSecPerFrame */
#define OFF_AVIH_TOTALFRAMES 48u    /* avih.dwTotalFrames */
#define OFF_AVIH_WIDTH       64u    /* avih.dwWidth  */
#define OFF_AVIH_HEIGHT      68u    /* avih.dwHeight */
#define OFF_STRH_FOURCC     100u    /* ★ 'strh' fourcc（size@104，payload@108） */
#define OFF_STRH_SCALE      128u    /* strh.dwScale（payload+20；muxer 回填 100） */
#define OFF_STRH_RATE       132u    /* strh.dwRate （payload+24；muxer 回填 fps_x100） */
#define OFF_MOVI_LIST       212u    /* 'LIST' */
#define OFF_MOVI_FOURCC     220u    /* 'movi' */
#define HEADER_BYTES        224u    /* muxer 布局：movi 数据从 224 开始 */

#define CHUNK_HDR             8u
#define MAX_FRAME_BYTES  (921600u + 64u)   /* 驱动 JPEG 单缓冲上限 + 余量 */

struct avi_demux {
    FILE    *fp;
    uint32_t fps_x100;
    uint32_t total_frames;
    uint32_t width;
    uint32_t height;
    uint32_t frame_idx;
    uint64_t bytes_read;        /* 已从 movi 区读掉的字节（诊断用） */
};

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

avi_demux_t *avi_demux_open(const char *path)
{
    avi_demux_t *d;
    uint8_t hdr[HEADER_BYTES];
    uint32_t scale, rate, usec;

    if (!path) return NULL;

    d = (avi_demux_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->fp = fopen(path, "rb");
    if (!d->fp) { free(d); return NULL; }

    if (fread(hdr, 1, HEADER_BYTES, d->fp) != HEADER_BYTES) goto fail;

    /* ---- 结构校验：不是我们认得的布局就拒绝（宁可拒绝也不乱播） ---- */
    if (memcmp(hdr + 0, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0) goto fail;
    if (memcmp(hdr + OFF_AVIH_FOURCC, "avih", 4) != 0) goto fail;
    if (memcmp(hdr + OFF_STRH_FOURCC, "strh", 4) != 0) goto fail;
    if (memcmp(hdr + OFF_MOVI_LIST, "LIST", 4) != 0 ||
        memcmp(hdr + OFF_MOVI_FOURCC, "movi", 4) != 0) goto fail;

    scale = get_u32le(hdr + OFF_STRH_SCALE);
    rate  = get_u32le(hdr + OFF_STRH_RATE);
    usec  = get_u32le(hdr + OFF_AVIH_USEC);
    if (scale > 0 && rate > 0) {
        d->fps_x100 = rate * 100u / scale;      /* muxer: scale=100, rate=fps_x100 */
    } else if (usec > 0) {
        d->fps_x100 = 100000000u / usec;
    } else {
        d->fps_x100 = 1200u;                    /* 兜底 12 fps（=本工程 nominal） */
    }

    d->total_frames = get_u32le(hdr + OFF_AVIH_TOTALFRAMES);
    d->width        = get_u32le(hdr + OFF_AVIH_WIDTH);
    d->height       = get_u32le(hdr + OFF_AVIH_HEIGHT);

    return d;

fail:
    fclose(d->fp);
    free(d);
    return NULL;
}

void avi_demux_get_info(const avi_demux_t *d,
                        uint32_t *fps_x100, uint32_t *total_frames,
                        uint32_t *width, uint32_t *height)
{
    if (!d) return;
    if (fps_x100)     *fps_x100     = d->fps_x100;
    if (total_frames) *total_frames = d->total_frames;
    if (width)        *width        = d->width;
    if (height)       *height       = d->height;
}

avid_result_t avi_demux_next_frame(avi_demux_t *d, uint8_t *buf, uint32_t buf_len,
                                   uint32_t *out_len)
{
    uint8_t hdr[CHUNK_HDR];
    uint32_t size;

    if (!d || !d->fp || !buf || buf_len == 0) return AVID_ERR_ARG;
    if (d->total_frames && d->frame_idx >= d->total_frames) return AVID_EOF;

    /* ---- 找下一个 '00dc' chunk 头（正常路径一次命中；损坏时向前扫到 EOF） ---- */
    for (;;) {
        if (fread(hdr, 1, CHUNK_HDR, d->fp) != CHUNK_HDR) return AVID_EOF;
        if (memcmp(hdr, "00dc", 4) == 0) break;
        /* 跳过 4 字节（fourcc 已消费），继续找 —— 容忍 00db/'JUNK' 等插入块 */
    }

    size = get_u32le(hdr + 4);
    if (size == 0 || size > buf_len || size > MAX_FRAME_BYTES) return AVID_ERR_FMT;
    if (fread(buf, 1, size, d->fp) != size) return AVID_ERR_IO;

    /* RIFF 奇数长度补 1 字节 pad（muxer 写出时保证） */
    if (size & 1u) {
        uint8_t pad;
        if (fread(&pad, 1, 1, d->fp) != 1) return AVID_ERR_IO;
    }

    d->bytes_read += CHUNK_HDR + size + (size & 1u);
    d->frame_idx++;
    if (out_len) *out_len = size;
    return AVID_OK;
}

void avi_demux_close(avi_demux_t *d)
{
    if (!d) return;
    if (d->fp) fclose(d->fp);
    free(d);
}
