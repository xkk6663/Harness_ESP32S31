/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi_manager.h
 * @brief 中间层：Wi-Fi STA 服务（初始化 / 扫描 / 指定 SSID 连接）
 *
 * 设计：
 *  - 不依赖 LVGL；事件通过回调上抛（在 esp event task 上下文执行）；
 *  - init 只完成底层初始化（nvs/netif/event/wifi start），不自动连接；
 *  - UI 启动按钮 -> wifi_manager_start_scan() 扫描；
 *  - 点选 AP -> 键盘输密码 -> wifi_manager_connect(ssid, pass)。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_LEN_MAX 32

typedef enum {
    WIFI_EVT_DISCONNECTED = 0,
    WIFI_EVT_CONNECTING,
    WIFI_EVT_CONNECTED,
    WIFI_EVT_GOT_IP,
    WIFI_EVT_SCAN_DONE,
} wifi_evt_t;

typedef struct {
    char    ssid[WIFI_SSID_LEN_MAX + 1];
    int8_t  rssi;
    uint8_t open;   /* 1 = open network (no password) */
} wifi_ap_info_t;

/**
 * @brief Wi-Fi 事件回调
 * @param evt  事件类型
 * @param info GOT_IP 时为 IP 字符串；其余为 NULL
 */
typedef void (*wifi_evt_cb_t)(wifi_evt_t evt, const char *info);

/**
 * @brief 初始化 Wi-Fi 底层（不自动连接）
 */
void wifi_manager_init(void);

void wifi_manager_set_evt_cb(wifi_evt_cb_t cb);

/** 系统级回调（如 main.c 的 LED 状态灯），与 UI 回调并存 */
void wifi_manager_set_sys_cb(wifi_evt_cb_t cb);

/**
 * @brief 启动一次扫描（非阻塞）；完成后触发 WIFI_EVT_SCAN_DONE
 */
void wifi_manager_start_scan(void);

/**
 * @brief 取最近一次扫描结果（在 SCAN_DONE 回调后调用）
 * @param out  输出数组
 * @param max  数组容量
 * @return 实际条数
 */
int wifi_manager_get_scan_results(wifi_ap_info_t *out, int max);

/**
 * @brief 连接指定 SSID / 密码
 */
void wifi_manager_connect(const char *ssid, const char *pass);

bool wifi_manager_is_connected(void);

/** 当前 IP 字符串（未连接返回空串） */
void wifi_manager_get_ip(char *buf, int len);

#ifdef __cplusplus
}
#endif
