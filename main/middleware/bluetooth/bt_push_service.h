/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_push_service.h
 * @brief A2DP Source 推送服务（方案 B「延迟常驻」）：把 SD 卡 MP3 推到蓝牙耳机。
 *
 * 数据流：io_file(MP3) -> aud_dec -> aud_asrc -> aud_enc(SBC) -> io_bt
 *
 * 方案 B 设计要点（对应 docs/项目规划/蓝牙本地推流方案B-延迟常驻.md §2）：
 *  - P1 时机前置：pool 在 bt_push_service_early_init() 建，抢开机内部 RAM 最空的窗口
 *    （与 bt_audio_service_controller_init 同一个窗口，在 LCD/WiFi/摄像头之前）；
 *  - P2 显式外置：GMF task 栈 stack_in_ext=true 进 PSRAM；pool 元素/IO 在外部 RAM；
 *  - P3 建一次不毁：pipeline 首次 ALLOCATED 才建，STOPPED/RELEASED 只停不毁；
 *  - P4 销毁异步：真正的 destroy 只在 reaper 任务上下文执行，绝不在 BT 事件回调里做。
 *
 * 事件驱动：由 bt_audio_service 在 STREAM_STATE_CHG 里调用
 * bt_push_service_on_stream_state(stream, state)。
 */

#ifndef BT_PUSH_SERVICE_H
#define BT_PUSH_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开机早期预建 GMF pool（抢内部 RAM 最空窗口）。
 * @note  必须在 bsp_display_start() 之前调用；失败只打日志不 abort（蓝牙非必需功能）。
 */
void bt_push_service_early_init(void);

/**
 * @brief 登记待推送的本地文件路径。
 * @note  uri 在 STREAM ALLOCATED 事件到来时才真正下发给 pipeline，
 *        因为那一刻才拿得到 stream handle。
 * @param path SD 卡上的 MP3 绝对路径，如 /sdcard/music/a.mp3
 */
esp_err_t bt_push_service_set_uri(const char *path);

/** 暂停推流（已连接且正在推流时有效） */
esp_err_t bt_push_service_pause(void);

/** 从暂停恢复推流 */
esp_err_t bt_push_service_resume(void);

/** 停止推流并复位 pipeline（不销毁 pipeline/task） */
esp_err_t bt_push_service_stop(void);

/** 当前是否在推流 */
bool bt_push_service_is_playing(void);

/** 请求异步销毁 pipeline（蓝牙关闭时调用；实际销毁在 reaper 任务里做） */
void bt_push_service_request_destroy(void);

/**
 * @brief 由 bt_audio_service 在 STREAM_STATE_CHG 事件里调用。
 * @param stream  stream handle（透传自 esp_bt_audio_event_stream_st_t）
 * @param state   ESP_BT_AUDIO_STREAM_STATE_* 四态
 * @note  本函数跑在 BT/GMF 事件线程上下文，只做轻量登记与 pipeline 启动，
 *        不做任何 destroy。
 */
void bt_push_service_on_stream_state(void *stream, int state);

/**
 * @brief 注册"播放自然结束"回调（★ 2026-09-22 新增）。
 *
 * 背景：GMF pipeline 把文件读尽后会发 FINISHED，但 **A2DP 流不会自动停** ——
 * 设备端持续以为在播，session 也不释放，表现为"播完了不退出、要手动 stop"。
 * 修复方式：push 层在收到 FINISHED/ERROR 时回调本函数，
 * 由 bt_audio_service 统一收尾（media_stop + session_manager_end + 通知 UI）。
 *
 * @note 回调在 GMF 事件线程上下文执行，**不得**直接调用阻塞的
 *       esp_bt_audio_media_stop()，应只投递命令/置标志。
 * @param cb 回调函数，可为 NULL 取消注册
 */
void bt_push_service_set_finished_cb(void (*cb)(void));

/**
 * @brief 该曲是否已自然播完（供 UI 轮询/收尾判断）。
 */
bool bt_push_service_is_finished(void);

/** 清除"播完"标志（上层收尾完成后调用） */
void bt_push_service_clear_finished(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_PUSH_SERVICE_H */
