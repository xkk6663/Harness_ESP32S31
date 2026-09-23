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

/** 打开/关闭蓝牙射频与 host（true: esp_bt_audio_init；false: deinit）
 *
 *  ★ 关于"为什么 host 不能像 controller 那样开机常驻"（重要，避免误改）：
 *   - `bt_audio_service_controller_init()` 常驻的是 **controller/HCI 内存池**，
 *     那是**真正的一次性分配**，必须在开机空窗期拿;
 *   - 但 **host（bluedroid）无法常驻**：`esp_bt_audio_deinit()` →
 *     `bt_audio_host_bluedroid_stack_deinit()` → `esp_bluedroid_disable()` +
 *     `esp_bluedroid_deinit()` → `btc_deinit()`，会把 BTU/BTC 任务与全部队列
 *     **真的全部释放**（见 `btc_main.c: btc_deinit_bluetooth()` 里的
 *     `vSemaphoreDelete` / `bte_main_shutdown`）。所以下一次打开就是**真正的重新 init**，
 *     照样可能因为内存不足而崩 —— 常驻 host 并不可行；
 *   - 另外 BTA 层（A2DP/AVRCP）必须在 controller 之后初始化，无法提到开机空窗期。
 *   → 结论：**每次 set_power(true) 都是一次有风险的重分配，闸门必须每次都走**。
 *
 *  @return ESP_OK 成功；
 *          ESP_ERR_INVALID_STATE controller 未就绪（早期 init 失败）；
 *          ESP_ERR_NO_MEM 内部 RAM 不足，**主动拒绝打开**。
 *          —— 调用方（UI）应把 ESP_ERR_NO_MEM 提示给用户，而不是当成功处理。 */
esp_err_t bt_audio_service_set_power(bool on);
bool bt_audio_service_is_on(void);

/** 仅"记录"角色（不重建 host）。真正切换请用 bt_audio_service_switch_role()。
 *  未开蓝牙时改这里是有意义的：下次 set_power(true) 就按新角色起。 */
void bt_audio_service_set_role(bt_role_t role);
bt_role_t bt_audio_service_get_role(void);

/**
 * @brief ★ 真正切换 A2DP 角色（Source / Sink）——会**重建整个 host**。
 *
 * ★★ 调用契约（重要）：
 *   唯一合法调用点是 **Bluetooth 子页面（page_bluetooth.c）的角色按钮**。
 *   **不得**在任何 tab 切换回调里调用 —— 本函数会断开当前蓝牙连接
 *   （A2DP 是全局单例，角色位在 init 时一次性传入，只能推倒重建），
 *   必须是用户"明确按按钮"的动作，而不是划 tab 的副作用。
 *
 *   实现：调 set_power(false) → 改角色 → set_power(true)。
 *   未开蓝牙时只记录角色并返回 ESP_OK。
 *
 * @param role BT_ROLE_SOURCE 或 BT_ROLE_SINK
 * @return ESP_OK 成功；
 *         ESP_ERR_INVALID_ARG 角色非法；
 *         ESP_ERR_NO_MEM 重建时内部 RAM 不足（**已自动回滚回原角色**，蓝牙仍可用）；
 *         其它为 esp_bt_audio_init() 的错误码。
 */
esp_err_t bt_audio_service_switch_role(bt_role_t role);

/** Source：开始/停止扫描；点击列表项后 connect */
esp_err_t bt_audio_service_start_discovery(void);
esp_err_t bt_audio_service_stop_discovery(void);
esp_err_t bt_audio_service_connect(const uint8_t *addr);
esp_err_t bt_audio_service_disconnect(void);

/** Source：推送控制（已连接时调用）。path 为 /sdcard/music/xxx.mp3 */
esp_err_t bt_audio_service_play_source(const char *path);
esp_err_t bt_audio_service_pause_source(void);
esp_err_t bt_audio_service_resume_source(void);
esp_err_t bt_audio_service_stop_source(void);

/** 是否已连接对端（音量分发据此判断音量归蓝牙管还是本地 codec 管） */
bool bt_audio_service_is_connected(void);

/**
 * @brief 设置蓝牙耳机音量（0..100，映射为推流数字增益）
 *
 * ★ 2026-09-22 起实现方式改为「推流数字增益」：
 *   推流路径（SD→解码→ASRC→SBC→蓝牙）**不经过本地 codec**，改本地 codec
 *   音量对耳机无效；而耳机侧又不接受 AVRCP 音量指令（AVRCP RC 通道建不起来，
 *   vol_ops 整张表从未注册，绝对/相对都返回 ESP_ERR_INVALID_STATE）。
 *   因此改为在 local2bt pipeline 内的 aud_alc 元素上做编码前 PCM 的数字衰减 ——
 *   我们是 A2DP 源，这段 PCM 完全可控，必然生效且与耳机能力无关。
 *
 * 生效条件：仅在**正在推流**时接管音量；否则返回 ESP_ERR_INVALID_STATE，
 *   调用方应回退到本地 codec（扬声器播放 / 开机提示音的正确出口）。
 *
 * @param volume 目标音量 0..100（0 = 静音，100 = 满幅）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示未推流或 ALC 未就绪，应回退本地 codec
 */
esp_err_t bt_audio_service_set_volume(int volume);

#ifdef __cplusplus
}
#endif

#endif /* BT_AUDIO_SERVICE_H */
