/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file net_time_service.h
 * @brief 中间层：网络校时（SNTP）+ 和风天气 7 天（HTTP + cJSON）服务
 *
 * 详细说明：
 *  - 本文件属于 middleware 层，**不依赖 LVGL**（红线：中间层不得 include lvgl.h）；
 *  - WiFi 拿到 IP 后由 app 层调用 net_time_service_start() 拉起，SNTP 校时 +
 *    每 1 小时拉取一次和风 7 天预报，结果以快照形式持有，UI 层定时读取；
 *  - 快照读写用互斥锁保护：写入发生在服务任务，读取发生在 LVGL task；
 *  - 未联网 / 未取到数据时 time_valid() 返回 false，UI 侧自行隐藏控件；
 *  - 配置宏 QWEATHER_KEY / QWEATHER_LOC_ID 可用编译期 -D 覆盖。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 和风天气配置（编译期可用 -D 覆盖，避免把私钥散落到代码逻辑里） ---- */
#ifndef QWEATHER_KEY
#define QWEATHER_KEY        "YOUR_QWEATHER_KEY"   /* TODO: 填入自己的和风 API Key */
#endif
#ifndef QWEATHER_LOC_ID
#define QWEATHER_LOC_ID     "101280101"           /* 广州 */
#endif
#ifndef QWEATHER_API_HOST
#define QWEATHER_API_HOST   "ke67cgkc3a.re.qweatherapi.com" /* 免费版 / 老版订阅地址 */
#endif
#ifndef QWEATHER_REFRESH_SEC
#define QWEATHER_REFRESH_SEC 3600                 /* 天气刷新周期：1 小时 */
#endif

/* NTP 服务器与北京时区（CST-8 表示东八区，POSIX TZ 符号与时区偏移相反） */
#define NET_TIME_NTP_SERVER  "ntp.aliyun.com"
#define NET_TIME_NTP_TZ      "CST-8"

#define WEATHER_DAYS_MAX     7

/** 单日天气（英文文案，Montserrat 字体不含中文字形） */
typedef struct {
    int8_t  t_max;                    /* 最高温 ℃ */
    int8_t  t_min;                    /* 最低温 ℃ */
    char    text_en[24];              /* 英文天气状况，如 "Sunny" */
    char    date[16];                 /* 和风返回的 fxDate，如 "2026-09-17" */
} weather_day_t;

/** 天气快照：ready=false 表示尚未成功取到数据，UI 应隐藏卡片 */
typedef struct {
    bool         ready;
    uint32_t     updated_s;           /* 快照刷新时刻（真实 epoch 秒） */
    char         city[24];            /* 城市名（和风返回为中文，仅留档） */
    weather_day_t days[WEATHER_DAYS_MAX];
    uint8_t      day_count;
} weather_data_t;

/**
 * @brief 启动校时与天气服务（WiFi 拿到 IP 后调用）
 * @note  非阻塞：仅创建常驻任务，实际联网请求由服务任务完成；
 *        重复调用安全（已在运行则直接返回）。
 */
void net_time_service_start(void);

/**
 * @brief SNTP 是否已完成校时（系统时间是否可信）
 * @return true 表示时间已同步过，UI 可以显示时钟
 */
bool net_time_service_time_valid(void);

/**
 * @brief 取最近一次天气快照（线程安全）
 * @param out 输出缓冲，不能为 NULL；未取到数据时 out->ready = false
 */
void net_time_service_get_weather(weather_data_t *out);

#ifdef __cplusplus
}
#endif
