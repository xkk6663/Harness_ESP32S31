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
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 和风天气配置（编译期可用 -D 覆盖，避免把私钥散落到代码逻辑里） ---- */
/* ★ QWEATHER_KEY 的兜底值 = **本地未提交改动**里的真 Key（不是占位符）。
 *   正式做法是 `tools/build.ps1` 经 `-D` 注入（见改动记录 §16），此处只在未注入时兜底。
 * ⚠ 正因为它是真 Key，**这一行绝不能被 commit** —— 提交前请改回占位符 "YOUR_QWEATHER_KEY"。
 *   （原注释写的 "TODO: 填入自己的" 已过期：值早就填了，TODO 反而让人误以为这里还是占位符。） */
#ifndef QWEATHER_KEY
#define QWEATHER_KEY        "a7928653a66a4b82ad2fe36a7a835bf4"
#endif
#ifndef QWEATHER_LOC_ID
#define QWEATHER_LOC_ID     "101280101"           /* 广州 */
#endif
#ifndef QWEATHER_API_HOST
#define QWEATHER_API_HOST   "ke67cgkc3a.re.qweatherapi.com" /* ★ 账号专属 API Host（devapi 已 2026-01-01 停服，见 F17）*/
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
    char         city[24];            /* 城市名 */
    weather_day_t days[WEATHER_DAYS_MAX];
    uint8_t      day_count;
    /* ---- now 实时天气 ---- */
    int8_t       now_temp;            /* 实时温度 ℃ */
    char         now_text[24];        /* 实时天气状况英文 */
    uint8_t      humidity;             /* 湿度 % */
    char         wind_dir[16];         /* 风向英文，如 "North" */
    uint8_t      wind_scale;           /* 风力等级 */
    /* ---- air now ---- */
    int          aqi;                 /* 0=未取到 */
    char         aqi_cat[16];          /* 优/良等英文短词 */
    /* ---- astronomy ---- */
    char         sunrise[8];          /* "HH:MM" */
    char         sunset[8];
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

/* ---------------------------------------------------------------------------
 *  ★ 取"可信时间"的唯一入口（S8-1 新增，方案 §5.1）
 *
 *  为什么要有这对函数（而不是让调用方自己 time(NULL) + localtime_r）：
 *  `capture_service.c:146` 那样的自由组合**漏一次 time_valid 检查就写出 1970 时间戳**，
 *  而 1970 在下游看起来是个"合法时刻"，不会报错、只会静静污染数据（打卡记录尤其致命）。
 *  ⇒ 把"可信判定"和"取值"绑进同一个函数，让"忘了查"在**编译期就不可能发生**：
 *     你拿到 net_time_t 时，valid 字段已经替你查过了。
 *
 *  ★ 纯增量改动，不改任何既有调用点。
 * ------------------------------------------------------------------------- */

/** 时间快照。`valid == false` 时其余字段全 0（**不是 1970**，调用方据此走降级）。 */
typedef struct {
    bool   valid;
    int    year;          /* 2026 */
    int    mon;           /* 1..12（**不是 tm_mon 的 0..11**，这里已经 +1） */
    int    day;           /* 1..31 */
    int    hour;          /* 0..23 */
    int    min;           /* 0..59 */
    int    sec;           /* 0..59 */
    time_t epoch;         /* 真实 epoch 秒；valid=false 时为 0 */
} net_time_t;

/**
 * @brief 取当前本地时间快照（线程安全，任意线程可调）
 * @param out 不能为 NULL；未校时时 out->valid = false，其余字段为 0
 */
void net_time_service_get(net_time_t *out);

/**
 * @brief 格式化：valid 时输出 `"2026-09-28 10:30:45"`（19 B + NUL），
 *        否则输出 `"---- -- -- --:--:--"`（19 B + NUL）
 *
 * ★ 占位符**刻意与真时间同长度**：UI 上排版不跳，人眼也一眼看出"时间不可信"，
 *   而不是显示一个假的 `1970-01-01 08:00:00`。
 *
 * @param t       可为 NULL（等同 valid=false）
 * @param out_len 建议 ≥ 20；不足时输出空串（不静默截断出半截时间）
 */
void net_time_service_format(const net_time_t *t, char *out, size_t out_len);

/** 便捷组合：取时间 + 格式化一步到位（内部就是调上面两个）。 */
void net_time_service_now_str(char *out, size_t out_len);

/**
 * @brief 取最近一次天气快照（线程安全）
 * @param out 输出缓冲，不能为 NULL；未取到数据时 out->ready = false
 */
void net_time_service_get_weather(weather_data_t *out);

#ifdef __cplusplus
}
#endif
