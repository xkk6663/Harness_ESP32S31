/* avi_mjpeg_muxer.c - AVI(MJPG) 流式封装（纯 C99，可脱离 ESP-IDF 原生单测）
 *
 * 文件结构（本实现写出的**精确**布局，偏移都是常量，close 时按这些偏移回填）：
 *
 *   偏移    内容                                        大小
 *   ------  ------------------------------------------  -----
 *   0       'RIFF' + size(占位) + 'AVI '                12
 *   12      'LIST' + size + 'hdrl'                      12
 *   24        'avih' + 56 + 56B 占位                    64
 *   88        'LIST' + size + 'strl'                    12
 *   100         'strh' + 56 + 56B 占位                  64
 *   164         'strf' + 40 + 40B                      48
 *   212     'LIST' + size(占位) + 'movi'                12
 *   224       '00dc' + size + <jpeg> [+1B pad]          每帧
 *   EOF     'idx1' + size + N×16B                      索引
 *
 * ★ 偏移写成常量而不是"用 tell() 现算"，是为了让 close() 里那十几处回填**不可能算错**；
 *   下面有 _Static_assert 把每个区间的大小锁死，改结构时编译期就会报。
 *
 * ★★ 本文件**不得** include 任何 esp_ 前缀 或 lvgl 的头（见头文件说明）—— 这是"能原生单测"的前提。
 */
#include "avi_mjpeg_muxer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ 布局常量 */
#define OFF_RIFF_SIZE        4u
#define OFF_HDRL_LIST       12u
#define OFF_HDRL_LIST_SIZE  16u
#define OFF_HDRL_FOURCC     20u
#define OFF_AVIH_DATA       32u
#define OFF_AVIH_SIZE       28u
#define OFF_STRL_LIST       88u
#define OFF_STRL_LIST_SIZE  92u
#define OFF_STRL_FOURCC     96u
#define OFF_STRH_DATA      108u
#define OFF_STRH_SIZE      104u
#define OFF_STRF_DATA      172u
#define OFF_STRF_SIZE      168u
#define OFF_MOVI_LIST      212u
#define OFF_MOVI_LIST_SIZE 216u
#define OFF_MOVI_FOURCC    220u

#define MOVI_DATA_START    224u
#define AVIH_PAYLOAD        56u
#define STRH_PAYLOAD        56u
#define STRF_PAYLOAD        40u
#define CHUNK_HDR            8u
#define IDX_ENTRY           16u

/* LIST 的 size 字段**值**（= 4 字节 fourcc + 之后全部子内容），AVI 规范定死。
 * 写成表达式而不是魔数，是为了"改了某个 payload 大小后，下面的断言立刻炸"。 */
#define STRL_LIST_SIZE_VAL  (4u + (CHUNK_HDR + STRH_PAYLOAD) + (CHUNK_HDR + STRF_PAYLOAD))
#define HDRL_LIST_SIZE_VAL  (4u + (CHUNK_HDR + AVIH_PAYLOAD) + (CHUNK_HDR + STRL_LIST_SIZE_VAL))

/* 把布局锁死：任一区间大小对不上都在**编译期**炸，
 * 而不是等到上机"文件打得开但播放器说损坏"再回来查。 */
_Static_assert(OFF_AVIH_DATA  + AVIH_PAYLOAD == OFF_STRL_LIST,    "avih layout drift");
_Static_assert(OFF_STRH_DATA  + STRH_PAYLOAD == OFF_STRF_SIZE - 4u, "strh layout drift");
_Static_assert(OFF_STRF_DATA  + STRF_PAYLOAD == OFF_MOVI_LIST,    "strf layout drift");
_Static_assert(OFF_STRH_SIZE  == OFF_STRH_DATA - 4u,              "strh size field drift");
_Static_assert(OFF_STRF_SIZE  == OFF_STRF_DATA - 4u,              "strf size field drift");
_Static_assert(OFF_STRL_LIST_SIZE == OFF_STRL_LIST + 4u,          "strl size field drift");
_Static_assert(OFF_HDRL_LIST_SIZE == OFF_HDRL_LIST + 4u,          "hdrl size field drift");
_Static_assert(OFF_STRL_LIST  + CHUNK_HDR + STRL_LIST_SIZE_VAL == OFF_MOVI_LIST, "strl end drift");
_Static_assert(OFF_HDRL_LIST  + CHUNK_HDR + HDRL_LIST_SIZE_VAL == OFF_MOVI_LIST, "hdrl end drift");
_Static_assert(OFF_MOVI_LIST  + CHUNK_HDR == OFF_MOVI_FOURCC,     "movi hdr drift");
_Static_assert(OFF_MOVI_FOURCC + 4u == MOVI_DATA_START,           "movi data drift");

/* RIFF 标志 */
#define AVIF_HASINDEX        0x00000010u
#define AVIIF_KEYFRAME       0x00000010u

/* idx1 重扫时的批缓冲条数。256 × 16 B = 4 KB，只申请一次、close 时释放。
 * ★ 用它把"每帧 2 次 lseek"降到"每 256 帧 2 次" —— 否则 7200 帧要 14400 次
 *   lseek+fread，在 SD 卡上会让 LVGL 线程卡住几秒（用户看到"按停止后 UI 僵住"）。 */
#define IDX_BATCH_ENTRIES    256u

/* ---- 批缓冲分配器（可插拔；见 .h 里 avi_muxer_set_batch_allocator 的说明）----
 * ★ 默认 malloc/free ⇒ PC 上原生单测（.workbuddy/test_avi_muxer.c）原样能跑；
 *   ESP 侧由 record_video_service 换成 PSRAM 版，避免和只剩 ~4 KB 的内部堆抢。 */
static avi_alloc_fn   s_batch_alloc   = NULL;   /* NULL = 用 malloc */
static avi_release_fn s_batch_release = NULL;   /* NULL = 用 free   */

static uint8_t *batch_alloc(size_t n)
{
    return s_batch_alloc ? (uint8_t *)s_batch_alloc(n) : (uint8_t *)malloc(n);
}

static void batch_release(void *p)
{
    if (s_batch_release) s_batch_release(p);
    else                 free(p);
}

struct avi_muxer {
    FILE     *fp;
    uint32_t  width;
    uint32_t  height;
    uint32_t  nominal_fps;      /* open 时传入 */
    uint32_t  real_fps_x100;    /* 0 = 未设置 ⇒ 用 nominal */
    uint32_t  frames;
    uint64_t  movi_bytes;       /* movi 内全部 chunk 的字节数（含头与 pad） */
    uint32_t  max_frame;
    uint64_t  sum_frame;        /* 所有帧字节数之和，用来算平均（给 avih 建议值） */
    int       closed;
};

/* ------------------------------------------------------------------ 小端工具
 * ★ 显式按字节写，不用结构体直接 fwrite —— 那样会依赖编译器 padding 与主机端序。
 *   代价是几行代码，收益是"在 PC 上跑同一份代码能给出同样的字节"（单测才有意义）。 */
static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int write_bytes(avi_muxer_t *m, const void *buf, size_t n)
{
    return (fwrite(buf, 1, n, m->fp) == n) ? 0 : -1;
}

static int write_u32(avi_muxer_t *m, uint32_t v)
{
    uint8_t b[4];
    put_u32le(b, v);
    return write_bytes(m, b, 4);
}

/* 写一个 chunk 头：fourcc(4) + size(4) */
static int write_chunk_hdr(avi_muxer_t *m, const char fourcc[4], uint32_t size)
{
    if (write_bytes(m, fourcc, 4) != 0) return -1;
    return write_u32(m, size);
}

/* ------------------------------------------------------------------ 头部构造 */

static int write_header(avi_muxer_t *m)
{
    uint8_t buf[AVIH_PAYLOAD];
    uint32_t fps = m->nominal_fps;

    /* ---- RIFF + AVI ---- */
    if (write_bytes(m, "RIFF", 4) != 0) return -1;
    if (write_u32(m, 0) != 0) return -1;          /* size 占位，close 回填 */
    if (write_bytes(m, "AVI ", 4) != 0) return -1;

    /* ---- LIST hdrl ---- */
    if (write_bytes(m, "LIST", 4) != 0) return -1;
    if (write_u32(m, HDRL_LIST_SIZE_VAL) != 0) return -1;   /* 常量，可直接写真值 */
    if (write_bytes(m, "hdrl", 4) != 0) return -1;

    /* ---- avih（56B，占位；真值在 close 回填） ---- */
    if (write_chunk_hdr(m, "avih", AVIH_PAYLOAD) != 0) return -1;
    memset(buf, 0, sizeof(buf));
    put_u32le(buf + 0,  fps ? (1000000u / fps) : 0);  /* dwMicroSecPerFrame */
    put_u32le(buf + 4,  0);                            /* dwMaxBytesPerSec   */
    put_u32le(buf + 8,  0);                            /* dwPaddingGranularity */
    put_u32le(buf + 12, AVIF_HASINDEX);                /* dwFlags            */
    put_u32le(buf + 16, 0);                            /* dwTotalFrames      */
    put_u32le(buf + 20, 0);                            /* dwInitialFrames    */
    put_u32le(buf + 24, 1);                            /* dwStreams          */
    put_u32le(buf + 28, 0);                            /* dwSuggestedBufferSize */
    put_u32le(buf + 32, m->width);                     /* dwWidth            */
    put_u32le(buf + 36, m->height);                    /* dwHeight           */
    /* dwReserved[4] 已为 0 */
    if (write_bytes(m, buf, AVIH_PAYLOAD) != 0) return -1;

    /* ---- LIST strl ---- */
    if (write_bytes(m, "LIST", 4) != 0) return -1;
    if (write_u32(m, STRL_LIST_SIZE_VAL) != 0) return -1;
    if (write_bytes(m, "strl", 4) != 0) return -1;

    /* ---- strh（56B，占位） ---- */
    if (write_chunk_hdr(m, "strh", STRH_PAYLOAD) != 0) return -1;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 0, "vids", 4);            /* fccType            */
    memcpy(buf + 4, "MJPG", 4);            /* fccHandler         */
    put_u32le(buf + 8,  0);                /* dwFlags            */
    put_u16le(buf + 12, 0);                /* wPriority          */
    put_u16le(buf + 14, 0);                /* wLanguage          */
    put_u32le(buf + 16, 0);                /* dwInitialFrames    */
    put_u32le(buf + 20, 1);                /* dwScale            */
    put_u32le(buf + 24, fps);              /* dwRate             */
    put_u32le(buf + 28, 0);                /* dwStart            */
    put_u32le(buf + 32, 0);                /* dwLength (帧数)    */
    put_u32le(buf + 36, 0);                /* dwSuggestedBufferSize */
    put_u32le(buf + 40, 0xFFFFFFFFu);      /* dwQuality = -1     */
    put_u32le(buf + 44, 0);                /* dwSampleSize       */
    put_u16le(buf + 48, 0);                /* rcFrame.left       */
    put_u16le(buf + 50, 0);                /* rcFrame.top        */
    put_u16le(buf + 52, (uint16_t)(m->width  & 0xFFFF));  /* rcFrame.right  */
    put_u16le(buf + 54, (uint16_t)(m->height & 0xFFFF));  /* rcFrame.bottom */
    if (write_bytes(m, buf, STRH_PAYLOAD) != 0) return -1;

    /* ---- strf = BITMAPINFOHEADER(40B)，真值可以直接写 ---- */
    if (write_chunk_hdr(m, "strf", STRF_PAYLOAD) != 0) return -1;
    memset(buf, 0, sizeof(buf));
    put_u32le(buf + 0,  40u);                       /* biSize        */
    put_u32le(buf + 4,  m->width);                  /* biWidth       */
    put_u32le(buf + 8,  m->height);                 /* biHeight      */
    put_u16le(buf + 12, 1);                         /* biPlanes      */
    put_u16le(buf + 14, 24);                        /* biBitCount    */
    memcpy(buf + 16, "MJPG", 4);                    /* biCompression */
    /* biSizeImage：JPEG 是变长，这里填"未压缩大小"是惯例（仅供参考） */
    put_u32le(buf + 20, m->width * m->height * 3u);
    /* 其余（X/Y PelsPerMeter、ClrUsed、ClrImportant）为 0 */
    if (write_bytes(m, buf, STRF_PAYLOAD) != 0) return -1;

    /* ---- LIST movi（size 占位） ---- */
    if (write_bytes(m, "LIST", 4) != 0) return -1;
    if (write_u32(m, 0) != 0) return -1;
    if (write_bytes(m, "movi", 4) != 0) return -1;

    return 0;
}

/* ------------------------------------------------------------------ 回填 */

static int write_at(avi_muxer_t *m, long off, const void *buf, size_t n)
{
    if (fseek(m->fp, off, SEEK_SET) != 0) return -1;
    return (fwrite(buf, 1, n, m->fp) == n) ? 0 : -1;
}

/* 回填 avih 与 strh 里的真值 */
static int patch_headers(avi_muxer_t *m)
{
    uint8_t b[4];
    uint32_t fps_x100 = m->real_fps_x100 ? m->real_fps_x100 : (m->nominal_fps * 100u);
    uint32_t avg = m->frames ? (uint32_t)(m->sum_frame / m->frames) : 0;

    /* --- avih --- */
    /* dwMicroSecPerFrame = 1e8 / fps_x100 */
    put_u32le(b, fps_x100 ? (uint32_t)(100000000u / fps_x100) : 0);
    if (write_at(m, OFF_AVIH_DATA + 0, b, 4) != 0) return -1;
    /* dwMaxBytesPerSec ≈ 平均帧长 × fps */
    put_u32le(b, (uint32_t)(((uint64_t)avg * fps_x100) / 100u));
    if (write_at(m, OFF_AVIH_DATA + 4, b, 4) != 0) return -1;
    /* dwTotalFrames */
    put_u32le(b, m->frames);
    if (write_at(m, OFF_AVIH_DATA + 16, b, 4) != 0) return -1;
    /* dwSuggestedBufferSize */
    put_u32le(b, m->max_frame);
    if (write_at(m, OFF_AVIH_DATA + 28, b, 4) != 0) return -1;

    /* --- strh ---
     * ★ 用 scale=100 / rate=fps_x100 表达实测平均帧率，避免整数截断
     *   （9.63 fps 直接写 rate=9 会让文件时长差 7%，而时长正是验收判据之一）。 */
    put_u32le(b, 100u);
    if (write_at(m, OFF_STRH_DATA + 20, b, 4) != 0) return -1;   /* dwScale */
    put_u32le(b, fps_x100);
    if (write_at(m, OFF_STRH_DATA + 24, b, 4) != 0) return -1;   /* dwRate  */
    put_u32le(b, m->frames);
    if (write_at(m, OFF_STRH_DATA + 32, b, 4) != 0) return -1;   /* dwLength */
    put_u32le(b, m->max_frame);
    if (write_at(m, OFF_STRH_DATA + 36, b, 4) != 0) return -1;   /* dwSuggestedBufferSize */

    return 0;
}

/**
 * 重扫 movi 生成 idx1，并回填 movi LIST size 与 RIFF size。
 * @param file_end close 时的文件长度（= movi 数据结束位置，也是 idx1 的起点）
 */
static int write_index_and_sizes(avi_muxer_t *m, long file_end)
{
    uint8_t *batch = batch_alloc(IDX_BATCH_ENTRIES * IDX_ENTRY);
    uint32_t movi_payload = 4u + (uint32_t)m->movi_bytes;
    long     write_pos;

    /* ---- movi LIST size ---- */
    {
        uint8_t b[4];
        put_u32le(b, movi_payload);
        if (write_at(m, OFF_MOVI_LIST_SIZE, b, 4) != 0) goto fail;
    }

    if (!batch) {
        /* 内存不足 ⇒ **降级**：不写索引条目，并清掉 avih 的 HASINDEX 标志。
         * ★ 宁可"明确声明没有索引"，也不要"声明有索引但内容是垃圾" ——
         *   后者会让播放器按垃圾偏移乱跳，比没有索引糟得多。
         *   （VLC/ffmpeg 对无索引 AVI 都能自行建索引，功能上不致命。） */
        uint8_t b[4];
        put_u32le(b, 0);
        if (write_at(m, OFF_AVIH_DATA + 12, b, 4) != 0) return -1;   /* dwFlags = 0（去掉 HASINDEX） */
        /* 仍然写一个空的 idx1（size=0），保持 RIFF 结构完整 */
        if (fseek(m->fp, file_end, SEEK_SET) != 0) return -1;
        if (write_chunk_hdr(m, "idx1", 0) != 0) return -1;
        goto tail;
    }

    /* ---- idx1 头 ---- */
    if (fseek(m->fp, file_end, SEEK_SET) != 0) goto fail;
    if (write_chunk_hdr(m, "idx1", m->frames * IDX_ENTRY) != 0) goto fail;
    write_pos = file_end + CHUNK_HDR;

    /* ---- 顺序扫 movi，批量写条目 ---- */
    {
        long     read_pos = (long)MOVI_DATA_START;
        uint32_t done = 0;
        while (done < m->frames) {
            uint32_t n = 0;
            while (n < IDX_BATCH_ENTRIES && (done + n) < m->frames) {
                uint8_t hdr[CHUNK_HDR];
                uint32_t sz;
                if (fseek(m->fp, read_pos, SEEK_SET) != 0) goto fail;
                if (fread(hdr, 1, CHUNK_HDR, m->fp) != CHUNK_HDR) goto fail;
                sz = get_u32le(hdr + 4);
                memcpy(batch + n * IDX_ENTRY, hdr, 4);           /* '00dc' */
                put_u32le(batch + n * IDX_ENTRY + 4, AVIIF_KEYFRAME);
                /* ★ 偏移相对 'movi' fourcc 位置（与 ffmpeg 的 avienc 同口径）：
                 *   播放器按 movi_list + offset 定位。 */
                put_u32le(batch + n * IDX_ENTRY + 8,  (uint32_t)(read_pos - (long)OFF_MOVI_FOURCC));
                put_u32le(batch + n * IDX_ENTRY + 12, sz);
                read_pos += (long)(CHUNK_HDR + sz + (sz & 1u));
                n++;
            }
            if (n == 0) break;
            if (fseek(m->fp, write_pos, SEEK_SET) != 0) goto fail;
            if (write_bytes(m, batch, (size_t)n * IDX_ENTRY) != 0) goto fail;
            write_pos += (long)(n * IDX_ENTRY);
            done += n;
        }
    }

tail:
    /* ---- RIFF size = 文件长度 - 8 ---- */
    {
        long end = ftell(m->fp);
        uint8_t b[4];
        if (end < 0) goto fail;
        put_u32le(b, (uint32_t)(end - 8));
        if (write_at(m, OFF_RIFF_SIZE, b, 4) != 0) goto fail;
    }
    /* has_index 已在降级路径里处理；正常路径下 dwFlags 从 write_header 起就是 AVIF_HASINDEX。 */
    batch_release(batch);
    return 0;

fail:
    batch_release(batch);
    return -1;
}

/* ------------------------------------------------------------------ 公开 API */

avi_muxer_t *avi_muxer_open(const char *path, uint32_t width, uint32_t height, uint32_t fps)
{
    avi_muxer_t *m;

    if (!path || width == 0 || height == 0 || fps == 0) return NULL;

    m = (avi_muxer_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->width       = width;
    m->height      = height;
    m->nominal_fps = fps;

    /* ★★★ 必须是 "w+b"（**读写**），不能是 "wb"。
     *   因为 close() 生成 idx1 时要**回扫 movi 区的 chunk 头**（fseek+fread）——
     *   而 "wb" 是只写流，fread 会直接失败 ⇒ close 返回 IO 错、文件缺索引。
     *   ★ 这个 bug 是**原生单测抓到的**（本文件不依赖 IDF，所以能真跑一遍），
     *     靠读代码很难发现。改这里前请先跑 .workbuddy/test_avi_muxer.c。 */
    m->fp = fopen(path, "w+b");
    if (!m->fp) { free(m); return NULL; }

    if (write_header(m) != 0) {
        fclose(m->fp);
        free(m);
        return NULL;
    }
    return m;
}

avi_result_t avi_muxer_add_jpeg(avi_muxer_t *m, const uint8_t *jpeg, uint32_t size)
{
    uint8_t hdr[CHUNK_HDR];
    uint32_t pad;

    if (!m || !jpeg || size == 0) return AVI_ERR_ARG;
    if (m->closed || !m->fp)      return AVI_ERR_STATE;

    /* '00dc' = 第 0 号流的压缩视频数据 */
    memcpy(hdr, "00dc", 4);
    put_u32le(hdr + 4, size);

    if (write_bytes(m, hdr, CHUNK_HDR) != 0) return AVI_ERR_IO;
    if (write_bytes(m, jpeg, size) != 0)     return AVI_ERR_IO;

    /* RIFF 规定 chunk 长度为偶数字节，奇数补 1 字节 0 */
    pad = size & 1u;
    if (pad) {
        uint8_t z = 0;
        if (write_bytes(m, &z, 1) != 0) return AVI_ERR_IO;
    }

    m->movi_bytes += (uint64_t)CHUNK_HDR + size + pad;
    m->sum_frame  += size;
    if (size > m->max_frame) m->max_frame = size;
    m->frames++;

    return AVI_OK;
}

avi_result_t avi_muxer_close(avi_muxer_t *m)
{
    long file_end;
    avi_result_t r = AVI_OK;

    if (!m) return AVI_ERR_ARG;
    if (m->closed || !m->fp) return AVI_ERR_STATE;

    if (fflush(m->fp) != 0) r = AVI_ERR_IO;

    file_end = ftell(m->fp);
    if (file_end < 0) r = AVI_ERR_IO;

    if (r == AVI_OK) {
        if (patch_headers(m) != 0)                       r = AVI_ERR_IO;
        else if (write_index_and_sizes(m, file_end) != 0) r = AVI_ERR_IO;
    }
    /* 收尾 write_at 把文件指针挪到了别处 —— 不影响内容，fclose 会 flush。 */

    if (fclose(m->fp) != 0) r = AVI_ERR_IO;

    m->fp     = NULL;
    m->closed = 1;
    free(m);
    return r;
}

void avi_muxer_abort(avi_muxer_t *m)
{
    if (!m || m->closed) return;
    if (m->fp) {
        fflush(m->fp);
        fclose(m->fp);
        m->fp = NULL;
    }
    m->closed = 1;
    free(m);
}

void avi_muxer_get_stats(const avi_muxer_t *m, avi_stats_t *out)
{
    if (!out) return;
    if (!m) { memset(out, 0, sizeof(*out)); return; }

    out->frames         = m->frames;
    out->max_frame_size = m->max_frame;
    out->avg_frame_size = m->frames ? (uint32_t)(m->sum_frame / m->frames) : 0;
    out->total_bytes    = (uint64_t)MOVI_DATA_START + m->movi_bytes;   /* 不含 idx1 */
}

void avi_muxer_set_real_fps(avi_muxer_t *m, uint32_t fps_x100)
{
    if (!m || m->closed || fps_x100 == 0) return;
    m->real_fps_x100 = fps_x100;
}

void avi_muxer_set_batch_allocator(avi_alloc_fn alloc_fn, avi_release_fn release_fn)
{
    /* ★ 两个钩子要么都设、要么都清成 NULL（= 默认 malloc/free）。
     *   只设一个会造成"用 A 分配、用 free 归还"的错配 —— 静默的堆破坏，
     *   比直接失败难查得多，所以这里直接忽略这种调用。 */
    if ((alloc_fn == NULL) != (release_fn == NULL)) return;
    s_batch_alloc   = alloc_fn;
    s_batch_release = release_fn;
}
