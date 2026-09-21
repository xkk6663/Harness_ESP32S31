/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_push_service.h
 * @brief A2DP Source 推送服务：把 SD 卡 MP3 通过蓝牙推到耳机。
 *
 * 数据流：io_file(MP3) -> aud_dec -> aud_asrc -> aud_enc(SBC) -> io_bt
 * 由 bt_audio_service 的 STREAM_STATE_CHG 事件驱动：
 *   - stream ALLOCATED (SOURCE/MEDIA)：绑 stream、set uri、run
 *   - stream STOPPED：stop/reset pipeline
 */

#ifndef BT_PUSH_SERVICE_H
#define BT_PUSH_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 local2bt pipeline（建 pool + pipeline + task）。开机调一次 */
esp_err_t bt_push_service_init(void);

/** 选一首 MP3 推送。内部会等 stream ALLOCATED 后自动跑 pipeline */
esp_err_t bt_push_service_play(const char *path);

/** 暂停/停止推送 */
esp_err_t bt_push_service_pause(void);
esp_err_t bt_push_service_stop(void);

/** 当前是否在推流 */
bool bt_push_service_is_playing(void);

/** 由 bt_audio_service 在 STREAM_STATE_CHG 事件里调用 */
void bt_push_service_on_stream_state(void *stream, int state);

#ifdef __cplusplus
}
#endif

#endif /* BT_PUSH_SERVICE_H */
