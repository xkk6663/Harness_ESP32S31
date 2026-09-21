/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_audio_service.h
 * @brief 蓝牙 A2DP 中间层：封装 esp_bt_audio，不依赖 LVGL。
 *
 * 设计：事件跑在 BT/GMF 线程，仅缓存状态并回调；UI 层用 lv_timer 抽取。
 * 蓝牙默认关，用完 deinit，避免与人脸识别/视频争抢内部 RAM。
 * 配对方式：Just Works（IO_CAP_NONE，无需耳机端确认）。
 */

#ifndef BT_AUDIO_SERVICE_H
#define BT_AUDIO_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A2DP 角色：Source = 本设备推送音乐到蓝牙耳机/音箱；Sink = 手机连本设备播放 */
typedef enum {
    BT_ROLE_SINK = 0,
    BT_ROLE_SOURCE,
} bt_role_t;

/* 对外事件（BT 线程只置标志/缓存数据，UI 层另起 lv_timer 抽取） */
typedef enum {
    BT_EVT_OFF = 0,          /* 蓝牙已关闭/deinit */
    BT_EVT_ON,               /* 蓝牙已初始化，等待扫描/连接 */
    BT_EVT_DISCOVERY_START,  /* 开始扫描 */
    BT_EVT_DISCOVERY_STOP,   /* 扫描结束 */
    BT_EVT_DEVICE_FOUND,     /* 发现新设备，data = const bt_device_t* */
    BT_EVT_CONNECTED,        /* 已连接，data = const uint8_t[6] addr */
    BT_EVT_DISCONNECTED,     /* 已断开 */
    BT_EVT_STREAM_ON,        /* 媒体流开始（开始出声/推送） */
    BT_EVT_STREAM_OFF,       /* 媒体流结束 */
    BT_EVT_MEDIA_CMD,        /* 远端控制命令，data = const esp_bt_audio_media_ctrl_cmd_t*（Sink 用） */
    BT_EVT_VOL_ABSOLUTE,     /* 远端绝对音量，data = const uint8_t* 0-100（Sink 用） */
} bt_evt_t;

#define BT_DEV_NAME_LEN 32

typedef struct {
    char     name[BT_DEV_NAME_LEN];
    uint8_t  addr[6];
    int32_t  rssi;
} bt_device_t;

typedef void (*bt_audio_evt_cb_t)(bt_evt_t evt, const void *data, void *ctx);

/** 初始化中间层（不打开射频，只建回调）。开机调用一次 */
void bt_audio_service_init(bt_audio_evt_cb_t cb, void *ctx);

/** 开机早期（LCD/WiFi/摄像头起来之前）调用一次：init+enable BT controller 并常驻。
 *  此时内部 RAM 最空，controller 的 HCI 内存池能一次性拿到连续块。
 *  之后 set_power 只开关 host（esp_bt_audio_init/deinit），不再碰 controller。
 *  失败只打日志不 abort（蓝牙非必需功能）。 */
void bt_audio_service_controller_init(void);

/** 打开/关闭蓝牙射频与 host（true: esp_bt_audio_init；false: deinit） */
esp_err_t bt_audio_service_set_power(bool on);
bool bt_audio_service_is_on(void);

/** 设置当前角色（仅影响扫描/连接用哪个 role bitmask；两角色在 init 时都已注册） */
void bt_audio_service_set_role(bt_role_t role);
bt_role_t bt_audio_service_get_role(void);

/** Source：开始/停止扫描；点击列表项后 connect */
esp_err_t bt_audio_service_start_discovery(void);
esp_err_t bt_audio_service_stop_discovery(void);
esp_err_t bt_audio_service_connect(const uint8_t *addr);
esp_err_t bt_audio_service_disconnect(void);

/** Source：推送控制（已连接时调用）。path 为 /sdcard/music/xxx.mp3 */
esp_err_t bt_audio_service_play_source(const char *path);
esp_err_t bt_audio_service_pause_source(void);
esp_err_t bt_audio_service_stop_source(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_AUDIO_SERVICE_H */
