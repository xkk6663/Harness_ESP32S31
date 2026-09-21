/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi_manager.h
 * @brief 中间层：Wi-Fi STA 服务（NVS 多网络记忆 / 上电自动连接 / 扫描 / 手动连接）
 *
 * 设计：
 *  - 不依赖 LVGL；事件通过回调上抛（在 esp event task 上下文执行）；
 *  - 联网成功后把 SSID/密钥写入 NVS（最多 5 个，LRU 置顶）；
 *  - 上电 wifi_manager_auto_connect() 读 NVS，扫描后按 RSSI 依次尝试，
 *    每个最多 WIFI_MAX_RETRY 次尝试后切换下一个，全失败发 WIFI_EVT_ALL_FAILED；
 *  - UI 手动连接复用同一套候选 + 重试逻辑。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_LEN_MAX 32
#define WIFI_PASS_LEN_MAX 64
#define WIFI_SAVED_MAX    5        /* NVS 最多记忆的 Wi-Fi 数量 */
#define WIFI_MAX_RETRY    3        /* 单个 Wi-Fi 的【总尝试次数】（含首次连接）
                                      ★ 2026-09-21 由 5 改为 3（F35）：
                                      实测【每次尝试约吃 10KB 内部 RAM 且不归还】
                                      （t=2363→6778 两次尝试掉 21732 B）。
                                      5 次 ≈ 50KB，把 mbedTLS 需要的 20KB 连续块
                                      挤没了（largest 掉到 15360）→ -0x008D。
                                      降到 3 次 ≈ 省 20KB，不影响"能连上"的场景
                                      （能连上的通常第 1 次就成，重试主要覆盖
                                        AP 刚重启 / 信道切换这类短暂窗口）。
                                      ★ 语义说明：这是**总尝试次数**，不是"额外重试次数"。
                                      日志打印为 (第几次/总数)，见 reconnect_timer_cb。 */

typedef enum {
    WIFI_EVT_DISCONNECTED = 0,
    WIFI_EVT_CONNECTING,
    WIFI_EVT_CONNECTED,
    WIFI_EVT_GOT_IP,
    WIFI_EVT_SCAN_DONE,
    WIFI_EVT_ALL_FAILED,
    WIFI_EVT_OFF,                 /* Wi-Fi 外设被关闭（用户关或达到尝试上限后自动关） */
    WIFI_EVT_AP_UP,              /* AP 热点已开启（板子就绪） */
    WIFI_EVT_STA_JOINED,         /* 有客户端连上 AP */
    WIFI_EVT_STA_LEFT,           /* 有客户端从 AP 断开 */
} wifi_evt_t;

/* 网络角色：默认上电 AP（开发板开热点，PC 直连），可切到 STA（连路由器） */
typedef enum {
    WIFI_ROLE_AP = 0,
    WIFI_ROLE_STA,
} wifi_role_t;

typedef struct {
    char    ssid[WIFI_SSID_LEN_MAX + 1];
    int8_t  rssi;
    uint8_t open;   /* 1 = open network (no password) */
} wifi_ap_info_t;

typedef void (*wifi_evt_cb_t)(wifi_evt_t evt, const char *info);

/**
 * @brief 初始化 Wi-Fi 底层（nvs/netif/event/wifi_start），不自动连接
 */
void wifi_manager_init(void);

/**
 * @brief 上电自动连接：读 NVS 已保存 Wi-Fi，扫描后按信号强度依次尝试，
 *        每个最多尝试 WIFI_MAX_RETRY 次（含首次连接）；无保存记录时直接返回
 */
void wifi_manager_auto_connect(void);

/**
 * @brief 注册事件回调 —— **注意这是两个独立槽位，语义不同，不要混用**
 *
 *  sys_cb（app 层持有）：
 *    - **进程级**，由 `main.c` 在 `wifi_manager_init()` **之前**注册，全生命周期不变；
 *    - 收全部事件（含 WIFI_EVT_GOT_IP），据此拉起校时/天气等后台服务；
 *    - **绝不能被 UI 覆盖**，否则"进一次 WiFi 页面"就会静默掐断后台服务。
 *
 *  evt_cb（UI 页面持有）：
 *    - **页面级**，页面 create 时注册、destroy 时传 NULL 注销，后注册即接管；
 *    - 只用于刷新界面上的状态文字/图标，回调里不许碰 LVGL（只置标志）。
 *
 * 两者独立派发（见 notify()），互不覆盖。历史教训：曾合成一个槽位，
 * page_wifi 进页面就把 app 的回调顶掉，导致天气/校时再也不启动。
 */
void wifi_manager_set_evt_cb(wifi_evt_cb_t cb);
void wifi_manager_set_sys_cb(wifi_evt_cb_t cb);

/** 切换网络角色（AP/STA）。切换后 AP 直接开热点；STA 走自动连接逻辑 */
void wifi_manager_set_role(wifi_role_t role);
wifi_role_t wifi_manager_get_role(void);

/** AP 模式下的热点名（供 UI 显示） */
const char *wifi_manager_get_ap_ssid(void);

/** AP 模式下当前已连接客户端数量 */
int wifi_manager_get_client_count(void);

/**
 * @brief 取已连接客户端 IP 列表（AP 模式）
 * @param out  输出缓冲区，每项至少 16 字节
 * @param max  缓冲区容量
 * @return 实际条数
 */
int wifi_manager_get_client_ips(char (*out)[16], int max);

void wifi_manager_start_scan(void);
int  wifi_manager_get_scan_results(wifi_ap_info_t *out, int max);

/**
 * @brief 手动连接指定 SSID/密钥（成功后自动写入 NVS）
 */
void wifi_manager_connect(const char *ssid, const char *pass);

bool wifi_manager_is_connected(void);
void wifi_manager_get_ip(char *buf, int len);

/**
 * @brief 打开/关闭 Wi-Fi 外设（射频电源）。关 = esp_wifi_stop()，保留 netif/event；
 *        开 = esp_wifi_start() 并按当前角色自动连接/开热点。初始化默认打开。
 */
void wifi_manager_set_enabled(bool on);
bool wifi_manager_is_enabled(void);

/* ---- NVS 凭证管理（可选，供 UI"已保存网络"页面使用）---- */

/** @brief NVS 中已保存的 Wi-Fi 数量 */
int  wifi_manager_get_saved_count(void);

/** @brief 删除指定 SSID 的已保存凭证 */
esp_err_t wifi_manager_forget(const char *ssid);

#ifdef __cplusplus
}
#endif
