/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file file_mgr_service.h
 * @brief S4 文件管理服务：SD 卡目录浏览 / 删除 / 照片预览 / MJPEG AVI 播放。
 *
 * 范式照抄 page_wav_player.c（本地音频播放页）：**服务任务 + cmd 投递 +
 * 事件回调 + LVGL 侧队列排空** —— 服务任务做一切阻塞 IO（F44 线程纪律），
 * 事件只投给 UI 的回调（UI 侧必须只入队，在 lv_timer 里落笔）。
 *
 * 【解码资源共享】预览/播放复用 viewfinder 的单例硬件引擎 + staging + 出图缓冲
 * （viewfinder_borrow_stage / viewfinder_decode_jpeg_mem，守约见 viewfinder.h）。
 * 本服务自配的只有 640x360 **双显示缓冲**（2x460.8 KB，开机预分配、永不 free）。
 *
 * 【互斥矩阵（S4 方案 §5）】进预览/播放前的守卫检查（服务侧再查一遍，UI 不是唯一防线）：
 *   - SD 已挂载；
 *   - camera_service_owner() != MEDIA（录像/拍照进行中整页只读）；
 *   - record_video_service / capture_service 均 IDLE；
 *   - !viewfinder_is_enabled()（取景器开着时共享缓冲归它）。
 * 录像/拍照进行中 BROWSE 仍允许（只读浏览无害），DELETE/preview/play 拒绝。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 预览/播放显示尺寸 = 720p 的 1/2 稀疏抽点（2 是公约数；640x360 + 1.25x zoom 铺满） */
#define FM_W    640u
#define FM_H    360u

#define FM_ENTRY_MAX    128     /**< 单目录条目上限（photos/videos 规模远小于此） */
#define FM_NAME_MAX      64     /**< 文件名缓冲 */
#define FM_PATH_MAX     128     /**< 路径缓冲 */

/** 目录条目（服务所有，BROWSE_DONE 后 UI 只读访问，直到下一次 BROWSE） */
typedef struct {
    char     name[FM_NAME_MAX];
    uint32_t size;              /**< 字节（目录恒 0） */
    bool     is_dir;
} file_mgr_entry_t;

/** 预览/播放画面（只读！双缓冲，下次 take 到新帧前内容稳定） */
typedef struct {
    const uint16_t *buf;        /**< RGB565 小端，FM_W x FM_H */
    uint32_t w, h;
    uint32_t seq;               /**< 从 1 开始；重复 take 同帧返回 false */
} file_mgr_frame_t;

/* ---- 事件（回调在**服务任务**上下文触发；UI 必须只入队） ---- */
typedef enum {
    FME_BROWSE_DONE = 0,    /**< a = 条目数 */
    FME_BROWSE_ERR,         /**< a = esp_err（多为未挂卡/路径不存在） */
    FME_DELETE_DONE,        /**< a = esp_err（ESP_ERR_INVALID_STATE = 有媒体事务在跑，拒删） */
    FME_PREVIEW_DONE,       /**< a = esp_err（OK 时画面已发布，用 file_mgr_preview_take 取） */
    FME_PLAY_STARTED,       /**< a = 总帧数, b = fps_x100 */
    FME_PLAY_PROGRESS,      /**< a = 当前帧(1-based), b = 总帧数 */
    FME_PLAY_DONE,          /**< a = 结束原因：0=播完 1=用户停止 2=出错 */
    FME_BUSY,               /**< 请求被拒（有别的操作在跑 / 守卫不过），a = esp_err */
} file_mgr_evt_id_t;

typedef struct {
    file_mgr_evt_id_t id;
    uint32_t a, b;
} file_mgr_evt_t;

typedef void (*file_mgr_evt_cb_t)(const file_mgr_evt_t *evt, void *user);

/* ---- cmd ---- */
typedef enum {
    FMC_BROWSE = 0,         /**< path = 目录 */
    FMC_DELETE,             /**< path = 文件 */
    FMC_PREVIEW,            /**< path = .jpg */
    FMC_PLAY,               /**< path = .avi */
    FMC_STOP,               /**< 停止播放（PLAY_DONE(reason=1)） */
    FMC_PAUSE,              /**< 播放中暂停/继续切换 */
} file_mgr_cmd_id_t;

typedef struct {
    file_mgr_cmd_id_t id;
    char path[FM_PATH_MAX];
} file_mgr_cmd_t;

/* ---- API（除 take/get_* 外都要在 LVGL 线程调） ---- */

/** 开机预分配显示双缓冲 + 建服务任务（幂等）。main.c 的 [mem] 分步账单里调。 */
esp_err_t file_mgr_init(void);

void file_mgr_set_evt_cb(file_mgr_evt_cb_t cb, void *user);

/** 投递命令（拷贝语义；队列满返回 ESP_ERR_INVALID_STATE，UI 提示稍后再试）。 */
esp_err_t file_mgr_post_cmd(const file_mgr_cmd_t *cmd);

/** 服务当前是否在播（UI 据此画按钮/禁删） */
bool file_mgr_is_playing(void);

/** 浏览结果访问（BROWSE_DONE 后只读；count/entries 可为 NULL） */
void file_mgr_get_entries(const file_mgr_entry_t **entries, int *count);

/** 最近一次浏览的目录（服务所有，只读） */
const char *file_mgr_get_cur_path(void);

/** LVGL 线程取最新预览/播放帧（语义同 viewfinder_take：重复取同帧返回 false） */
bool file_mgr_preview_take(file_mgr_frame_t *out);

#ifdef __cplusplus
}
#endif
