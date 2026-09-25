/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file record_video_service.h
 * @brief 中间层：MJPEG AVI 录像服务（S2b）
 *
 * 方案：docs/项目规划/人脸录用系统与SD卡媒体实施方案.md §5
 *
 * 数据通路（全部零软件编码 —— JPEG 由 OV3660 硬件出）：
 *
 *   OV3660(JPEG 1280x720) --DVP--> mmap 缓冲
 *        --> cam_task 回调 rec_frame_cb()
 *        --> avi_muxer_add_jpeg()  ← **就在回调里同步写盘**
 *        --> /sdcard/videos/VID_*.avi
 *
 * ★★ 为什么敢在回调里同步写盘（这是本模块最重要的一个取舍）：
 *   方案 §5.1-④ 的原文要求是"**不丢帧**，宁可降 fps 也不要丢帧"。
 *   回调里同步 fwrite 恰好满足这条：
 *     - 每一帧**只要被回调到就一定完整写下去**（不存在"拷到队列时丢了"）；
 *     - 代价是写盘期间占着 DVP 缓冲 ⇒ **出帧率自然下降**，这正是方案认可的降级方向。
 *   换成"回调里 memcpy 到环形池、另一个任务慢慢写"的做法，需要 3~4 块 200 KB+
 *   的 PSRAM 缓冲（还不知道 720p JPEG 到底多大，见 §2.5.7 风险 1），
 *   且池满时**必须丢帧** —— 与方案明确选定的方向相反。
 *   ⇒ 用"降 fps 换零丢帧 + 零额外缓冲"。
 *
 * ★ 帧率不是 12：实测平均 fps 会在停止时写进 AVI（见 avi_muxer_set_real_fps）。
 *   若硬写标称 12，播放器会按 12 fps 播，时长比真实录制时间短，直接违反 S2b 验收项
 *   "时长与录制时间一致"。
 *
 * ★ 音频：**一期不做**（用户 2026-09-25 20:37 明确"先不录音"），文件里只有一条
 *   vids/MJPG 流，方案 §5.4 有依据。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SD 卡上的录像目录（相对 SDCARD_MNT_PATH） */
#define VIDEO_REC_DIR_NAME   "/videos"

/**
 * ★★ 路径缓冲长度（含 NUL）。**改小会直接编译失败**，不是风格问题：
 *   本工程开了 `-Werror=format-truncation=`，GCC 会在目标数组大小可知时证明 `snprintf`
 *   不会截断。最坏情况 = (dir ≤63) + '/' + (base ≤39) + "_099.avi"(8) + NUL = 112，
 *   而本文件原先用 96 ⇒ 真实构建报
 *   `'%s' directive output may be truncated ... into a region of size between 32 and 95`。
 *   ★ 该诊断**依赖内联**（只有 make_video_path 被内联进 rec_task 后 GCC 才知道 96），
 *     所以单文件静态校验看不到 —— 只有真机 build 才炸。取 128 留余量。
 */
#define VIDEO_REC_PATH_MAX   128

/** 标称帧率（= sensor JPEG 档上限）。只用于 avih 的初始占位，收尾时会被实测值覆盖。 */
#define VIDEO_REC_NOMINAL_FPS   12u

/** 上限保护（方案 §5.3：到达上限立即停止并回填 AVI 头，绝不留"看起来在录其实早停了"的状态） */
#define VIDEO_REC_MAX_SECONDS   300u                        /* 5 分钟 */
#define VIDEO_REC_MAX_BYTES     (200u * 1024u * 1024u)      /* 200 MB */

typedef enum {
    VIDEO_REC_IDLE = 0,        /* 没在录 */
    VIDEO_REC_RECORDING,       /* 正在收帧 */
    /* ★ 为什么要有这一态：收尾要**重扫整个 movi 区生成 idx1**（方案 §5.1 的取舍：
     *   换掉 115 KB 常驻索引内存）。几十 MB 的文件要读几秒 ⇒ 这段时间既不能算"在录"，
     *   也不能当成"结束"（用户可能以为可以拔卡了）。UI 必须显示"正在保存，请勿拔卡"。 */
    VIDEO_REC_FINALIZING,
} video_rec_state_t;

/** 录制中的实时快照（由服务任务刷新；UI 只读，**不持有 muxer 句柄**） */
typedef struct {
    uint32_t frames;        /* 已写入帧数 */
    uint32_t bytes;         /* 当前文件字节数（含容器开销） */
    uint32_t max_frame;     /* 最大单帧字节数 ← §2.5.7 风险 1 的上机真值 */
    uint32_t seconds;       /* 已录秒数 */
    uint32_t fps_x100;      /* 至今实测平均帧率 ×100 */
    char     path[VIDEO_REC_PATH_MAX];   /* 目标路径（未开成时为空串） */
} video_rec_stats_t;

/** 上一次录制的最终结果 */
typedef struct {
    esp_err_t err;          /* ESP_OK = 文件完整（含 idx1） */
    char      path[VIDEO_REC_PATH_MAX];
    uint32_t  frames;
    uint32_t  bytes;
    uint32_t  max_frame;
    uint32_t  seconds;
    uint32_t  fps_x100;
    bool      index_ok;     /* false = 缺 idx1（内存不足降级 或 写失败），仍可能能播 */
} video_rec_result_t;

/** 初始化（幂等）：装上 PSRAM 版批缓冲分配器 —— 见 .c 里的说明。 */
void record_video_service_init(void);

/**
 * @brief 开始录像（**非阻塞**）
 *
 * @return ESP_OK              已受理
 *         ESP_ERR_INVALID_STATE 已在录 / 相机被别人占用 / SD 卡没挂载
 *         ESP_ERR_NO_MEM        任务栈建不起来（内部 RAM 紧）
 */
esp_err_t record_video_service_start(void);

/**
 * @brief 请求停止（**非阻塞**）
 *
 * ★ 立刻返回，真正的收尾（停流 + 回填头 + 生成 idx1）在服务任务里做，
 *   期间 state 会停在 VIDEO_REC_FINALIZING。**绝不能在 LVGL 线程里同步等待** ——
 *   大文件重扫要好几秒，UI 会直接僵住。
 * ★ 也绝不能把这个收尾塞进 cam_task 回调：那是"回调里做长阻塞"的典型反例。
 */
esp_err_t record_video_service_stop(void);

bool              record_video_service_is_recording(void);
video_rec_state_t record_video_service_state(void);

/** 实时快照（任意线程可读） */
void record_video_service_get_stats(video_rec_stats_t *out);

/** 上一次的最终结果（state 回到 IDLE 后才完整） */
void record_video_service_get_result(video_rec_result_t *out);
void record_video_service_clear_result(void);

#ifdef __cplusplus
}
#endif
