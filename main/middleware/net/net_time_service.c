/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file net_time_service.c
 * @brief 中间层：SNTP 校时 + 和风天气 7 天（esp_http_client + esp_crt_bundle + cJSON）
 *
 * 数据流：
 *   WiFi 拿到 IP → net_time_service_start() 建常驻任务
 *     └─ 任务循环：等"立即刷新"通知 → SNTP 校时 → 拉天气 → 写快照（加锁）
 *                → 等 QWEATHER_REFRESH_SEC 秒（期间可被新通知提前唤醒）
 *
 * 线程模型：
 *   - 快照读写由 s_lock（xSemaphoreCreateMutex）保护：服务任务写，LVGL task 读；
 *   - 本文件是 middleware 层，禁止 include lvgl.h、禁止调用任何 lv_* API。
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "zlib.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "net_time_service.h"

static const char *TAG = "NET_TIME";

#define NET_TIME_TASK_STACK   8192      /* 业务任务栈不得 < 8192（HTTP + cJSON 会吃栈） */
#define NET_TIME_TASK_PRIO    4
#define NET_TIME_TASK_CORE    0

/* HTTP 一次读取 1KB；和风 7 天 JSON 约 1.5~2KB，缓冲留足 */
#define NET_TIME_HTTP_BUF     8192
#define NET_TIME_JSON_MAX     4096

static SemaphoreHandle_t s_lock;         /* 保护 s_weather，服务任务写 / LVGL task 读 */
static TaskHandle_t      s_task;
static bool              s_time_valid;   /* SNTP 已校时 */
static weather_data_t    s_weather;
static char              s_http_buf[NET_TIME_HTTP_BUF];

/* ==================== 和风天气状况 → 英文 ==================== */

/* Montserrat 字体不含中文字形，天气状况必须映射为英文再上屏 */
typedef struct {
    int         code;       /* 和风 icon 主码 */
    const char *en;
} wx_code_map_t;

static const wx_code_map_t s_wx_map[] = {
    {100, "Sunny"}, {101, "Cloudy"}, {102, "Few Clouds"}, {103, "Partly Cloudy"},
    {104, "Overcast"},
    {150, "Clear Night"}, {151, "Cloudy Night"}, {152, "Few Clouds Night"},
    {153, "Partly Cloudy Night"},
    {300, "Shower"}, {301, "Heavy Shower"},
    {302, "Thunderstorm"}, {303, "Heavy T-Storm"}, {304, "Heavy Hailstorm"},
    {305, "Light Rain"}, {306, "Rain"}, {307, "Heavy Rain"}, {308, "Extreme Rain"},
    {309, "Drizzle"}, {310, "Storm"}, {311, "Heavy Storm"}, {312, "Severe Storm"},
    {313, "Freezing Rain"}, {314, "Light Freezing Rain"}, {315, "Heavy Freezing Rain"},
    {316, "Rainstorm"}, {317, "Downpour"}, {318, "Rainstorm"},
    {350, "Sleet"}, {351, "Heavy Sleet"},
    {399, "Rain"},
    {400, "Light Snow"}, {401, "Snow"}, {402, "Heavy Snow"}, {403, "Blizzard"},
    {404, "Sleet"}, {405, "Heavy Sleet"}, {406, "Rain & Snow"}, {407, "Snow Shower"},
    {408, "Light Snow"}, {409, "Heavy Snow"}, {410, "Snowstorm"},
    {456, "Snow Shower"}, {457, "Snow Shower"},
    {499, "Snow"},
    {500, "Mist"}, {501, "Fog"}, {502, "Haze"}, {503, "Sandstorm"},
    {504, "Dust"}, {507, "Duststorm"}, {508, "Duststorm"}, {509, "Fog"},
    {510, "Light Fog"}, {511, "Haze"}, {512, "Heavy Haze"}, {513, "Severe Haze"},
    {514, "Heavy Fog"}, {515, "Extra Heavy Fog"},
    {900, "Hot"}, {901, "Cold"}, {999, "Unknown"},
};

/**
 * @brief 和风天气状况文本 → 英文短名
 * @note  cJSON 解析出来的 text 是中文；优先按 icon 主码查表，
 *        查不到则回退到中文关键字匹配，最后回退到 "N/A"。
 */
static const char *wx_text_to_en(const char *icon, const char *text)
{
    if (icon && icon[0]) {
        int code = atoi(icon);
        for (size_t i = 0; i < sizeof(s_wx_map) / sizeof(s_wx_map[0]); i++) {
            if (s_wx_map[i].code == code) {
                return s_wx_map[i].en;
            }
        }
    }
    if (text) {
        if (strstr(text, "雷")) return "Thunderstorm";
        if (strstr(text, "雪")) return "Snow";
        if (strstr(text, "雨")) return "Rain";
        if (strstr(text, "雾")) return "Fog";
        if (strstr(text, "霾")) return "Haze";
        if (strstr(text, "沙") || strstr(text, "尘")) return "Dust";
        if (strstr(text, "阴")) return "Overcast";
        if (strstr(text, "多云")) return "Cloudy";
        if (strstr(text, "晴")) return "Sunny";
    }
    return "N/A";
}

/** 安全取 cJSON 子项字符串，缺失返回空串 */
static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return "";
}

/** "25" → 25；非法/缺省返回 INT8_MIN 以便 UI 判空 */
static int8_t json_int8(const cJSON *obj, const char *key, int8_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return (int8_t)atoi(item->valuestring);
    }
    if (cJSON_IsNumber(item)) {
        return (int8_t)item->valuedouble;
    }
    return fallback;
}

/* ==================== 天气请求与解析 ==================== */

/* ==================== 和风返回 gzip 的流式解压 ==================== */

/*
 * 和风 API 会**无视 Accept-Encoding 一律返回 gzip**（实测 identity/gzip/缺省
 * 三种请求头都回 Content-Encoding: gzip，字节头 0x1f 0x8b）。
 * esp_http_client 自身不带解压层，若把压缩数据直接交给 cJSON 会 Parse 失败，
 * 因此这里用 zlib 做**流式 inflate**：边收边解压写进 s_http_buf。
 */
typedef struct {
    z_stream  zs;
    bool      inited;      /* inflateInit2 成功 */
    bool      gz;          /* 本次响应是否为 gzip */
    size_t    out_used;    /* 已解压字节数 */
    bool      overflow;    /* 解压结果超出缓冲 */
} gz_ctx_t;

static gz_ctx_t s_gz;

static esp_err_t http_evt_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {

    case HTTP_EVENT_ON_HEADER: {
        /* gzip / x-gzip 都需要解压（zlib 的 15+16 窗口位自动识别 gzip 头） */
        if (evt->header_key && evt->header_value &&
            strcasecmp(evt->header_key, "Content-Encoding") == 0) {
            s_gz.gz = (strcasecmp(evt->header_value, "gzip") == 0 ||
                       strcasecmp(evt->header_value, "x-gzip") == 0);
            /* inflateInit2: 15=最大窗口, +32=自动识别 gzip/zlib 头 */
            if (s_gz.gz && !s_gz.inited) {
                memset(&s_gz.zs, 0, sizeof(s_gz.zs));
                if (inflateInit2(&s_gz.zs, 15 + 32) == Z_OK) {
                    s_gz.inited = true;
                } else {
                    ESP_LOGE(TAG, "inflateInit2 failed");
                    s_gz.gz = false;   /* 退回：按明文处理 */
                }
            }
        }
        break;
    }

    case HTTP_EVENT_ON_DATA: {
        if (!s_gz.gz) {
            /* 非压缩：直接拷进输出缓冲 */
            if (s_gz.out_used + (size_t)evt->data_len < sizeof(s_http_buf)) {
                memcpy(s_http_buf + s_gz.out_used, evt->data, evt->data_len);
                s_gz.out_used += (size_t)evt->data_len;
            } else {
                s_gz.overflow = true;
            }
            break;
        }

        /* 压缩：流式 inflate，一次调用处理完这段输入 */
        s_gz.zs.next_in  = (Bytef *)evt->data;
        s_gz.zs.avail_in = (uInt)evt->data_len;

        while (s_gz.zs.avail_in > 0) {
            s_gz.zs.next_out  = (Bytef *)(s_http_buf + s_gz.out_used);
            s_gz.zs.avail_out = (uInt)(sizeof(s_http_buf) - 1 - s_gz.out_used);

            int rc = inflate(&s_gz.zs, Z_NO_FLUSH);
            s_gz.out_used = sizeof(s_http_buf) - 1 - s_gz.zs.avail_out;

            if (rc == Z_STREAM_END) {
                break;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                ESP_LOGE(TAG, "inflate failed rc=%d", rc);
                return ESP_OK;
            }
            if (s_gz.zs.avail_out == 0) {
                s_gz.overflow = true;
                break;
            }
            if (rc == Z_BUF_ERROR && s_gz.zs.avail_in == 0) {
                break;   /* 本段输入消化完，等下一段 */
            }
        }
        break;
    }

    case HTTP_EVENT_ON_FINISH: {
        if (s_gz.inited) {
            inflateEnd(&s_gz.zs);
            s_gz.inited = false;
        }
        s_http_buf[s_gz.out_used < sizeof(s_http_buf) ? s_gz.out_used : sizeof(s_http_buf) - 1] = '\0';
        break;
    }

    case HTTP_EVENT_DISCONNECTED:
    case HTTP_EVENT_ERROR: {
        if (s_gz.inited) {
            inflateEnd(&s_gz.zs);
            s_gz.inited = false;
        }
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

/** 通用 GET：url → 解压后 JSON 字符串（写进 s_http_buf），成功返回 true */
static bool http_get_json(const char *url)
{
    memset(s_http_buf, 0, sizeof(s_http_buf));
    memset(&s_gz, 0, sizeof(s_gz));

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_evt_cb,
        .user_data = s_http_buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .user_agent = "esp32-display/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http perform failed: %s", esp_err_to_name(err));
        goto out;
    }
    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGE(TAG, "http status=%d url=%s",
                 esp_http_client_get_status_code(client), url);
        goto out;
    }
    ok = true;
out:
    esp_http_client_cleanup(client);
    return ok;
}

/** 解析和风 7 天 JSON 到 out（不加锁，由调用方加锁写入 s_weather） */
static bool parse_weather_json(const char *json, weather_data_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "cJSON_Parse failed");
        return false;
    }

    const char *code = json_str(root, "code");
    if (strcmp(code, "200") != 0) {
        ESP_LOGE(TAG, "qweather api code=%s (expect 200)", code[0] ? code : "?");
        cJSON_Delete(root);
        return false;
    }

    weather_data_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsArray(daily)) {
        ESP_LOGE(TAG, "no \"daily\" array in response");
        cJSON_Delete(root);
        return false;
    }

    const cJSON *day = NULL;
    cJSON_ArrayForEach(day, daily) {
        if (tmp.day_count >= WEATHER_DAYS_MAX) {
            break;
        }
        weather_day_t *d = &tmp.days[tmp.day_count];
        d->t_max = json_int8(day, "tempMax", 0);
        d->t_min = json_int8(day, "tempMin", 0);
        snprintf(d->date, sizeof(d->date), "%s", json_str(day, "fxDate"));
        snprintf(d->text_en, sizeof(d->text_en), "%s",
                 json_str(day, "textDay"));
        tmp.day_count++;
    }

    cJSON_Delete(root);

    if (tmp.day_count == 0) {
        ESP_LOGE(TAG, "daily array empty");
        return false;
    }

    tmp.ready = true;
    tmp.updated_s = (uint32_t)time(NULL);
    snprintf(tmp.city, sizeof(tmp.city), "%s", "Guangzhou");

    *out = tmp;
    return true;
}

/** 解析 /v7/weather/now：填 now_temp/now_text(中文)/humidity/wind_dir(中文)/wind_scale */
static void parse_weather_now(weather_data_t *out)
{
    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return;
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    if (now) {
        out->now_temp = json_int8(now, "temp", 0);
        /* UI 要中文，直接存和风返回的中文 text（如 "晴"/"多云"） */
        snprintf(out->now_text, sizeof(out->now_text), "%s", json_str(now, "text"));
        out->humidity = (uint8_t)json_int8(now, "humidity", 0);
        snprintf(out->wind_dir, sizeof(out->wind_dir), "%s", json_str(now, "windDir"));
        out->wind_scale = (uint8_t)json_int8(now, "windScale", 0);
    }
    cJSON_Delete(root);
}

/** 解析 /v7/air/now：填 aqi/aqi_cat */
static void parse_air_now(weather_data_t *out)
{
    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return;
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    if (now) {
        out->aqi = json_int8(now, "aqi", 0);
        snprintf(out->aqi_cat, sizeof(out->aqi_cat), "%s", json_str(now, "category"));
    }
    cJSON_Delete(root);
}

/** Parse /v7/astronomy/sun: fill sunrise/sunset (HH:MM only) */
static void parse_sun(weather_data_t *out)
{
    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        ESP_LOGW(TAG, "sun: cJSON parse failed, body: %.200s", s_http_buf);
        return;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (code) {
        ESP_LOGI(TAG, "sun raw body: %.300s", s_http_buf);
    }
    const cJSON *sr = cJSON_GetObjectItemCaseSensitive(root, "sunrise");
    const cJSON *ss = cJSON_GetObjectItemCaseSensitive(root, "sunset");
    if (cJSON_IsString(sr) && sr->valuestring) {
        const char *p = sr->valuestring;
        const char *t = strchr(p, 'T');
        snprintf(out->sunrise, sizeof(out->sunrise), "%.5s", t ? t + 1 : p);
    }
    if (cJSON_IsString(ss) && ss->valuestring) {
        const char *p = ss->valuestring;
        const char *t = strchr(p, 'T');
        snprintf(out->sunset, sizeof(out->sunset), "%.5s", t ? t + 1 : p);
    }
    ESP_LOGI(TAG, "sun: sunrise=%s sunset=%s", out->sunrise, out->sunset);
    cJSON_Delete(root);
}

/**
 * @brief 拉取一次和风天气（7d + now + air + sun）并写入快照
 * @return true 表示成功刷新
 */
static bool fetch_weather_once(void)
{
    if (strcmp(QWEATHER_KEY, "YOUR_QWEATHER_KEY") == 0) {
        ESP_LOGW(TAG, "QWEATHER_KEY not configured, skip weather (fill it and rebuild)");
        return false;
    }

    char url[320];
    weather_data_t tmp;
    bool ok = false;

    /* 1) 7d 预报（必须成功） */
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/7d?location=%s&key=%s&lang=zh",
             QWEATHER_API_HOST, QWEATHER_LOC_ID, QWEATHER_KEY);
    if (!http_get_json(url) || !parse_weather_json(s_http_buf, &tmp)) {
        ESP_LOGE(TAG, "7d failed");
        return false;
    }

    /* 2) now 实时 */
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/now?location=%s&key=%s&lang=zh",
             QWEATHER_API_HOST, QWEATHER_LOC_ID, QWEATHER_KEY);
    if (http_get_json(url)) parse_weather_now(&tmp);

    /* 3) 空气质量 */
    snprintf(url, sizeof(url),
             "https://%s/v7/air/now?location=%s&key=%s&lang=zh",
             QWEATHER_API_HOST, QWEATHER_LOC_ID, QWEATHER_KEY);
    if (http_get_json(url)) parse_air_now(&tmp);

    /* 4) 日出日落 */
    time_t now_t = time(NULL);
    struct tm tmv = {0};
    localtime_r(&now_t, &tmv);
    char date[16];
    strftime(date, sizeof(date), "%Y%m%d", &tmv);
    ESP_LOGI(TAG, "sun req date=%s now_t=%ld", date, (long)now_t);
    snprintf(url, sizeof(url),
             "https://%s/v7/astronomy/sun?location=%s&date=%s&key=%s&lang=zh",
             QWEATHER_API_HOST, QWEATHER_LOC_ID, date, QWEATHER_KEY);
    if (http_get_json(url)) parse_sun(&tmp);

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_weather = tmp;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "weather ok: %s %d/%d hum=%d aqi=%d %s/%s",
             tmp.now_text, tmp.days[0].t_max, tmp.days[0].t_min,
             tmp.humidity, tmp.aqi, tmp.sunrise, tmp.sunset);
    ok = true;
    return ok;
}

/* ==================== SNTP 校时 ==================== */

static bool sntp_sync_once(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NET_TIME_NTP_SERVER);
    cfg.start = true;
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "sntp init failed, err=%s", esp_err_to_name(err));
        return false;
    }

    /* 最多等 15s；超时不算致命，下一轮刷新会重试 */
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp sync timeout (%s), will retry next cycle", esp_err_to_name(err));
        return false;
    }

    time_t now = 0;
    struct tm tmv = {0};
    time(&now);
    localtime_r(&now, &tmv);
    ESP_LOGI(TAG, "sntp synced: %04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    s_time_valid = true;
    return true;
}

/* ==================== 服务任务 ==================== */

static void net_time_task(void *arg)
{
    (void)arg;

    /* IPv4 且只取 SNTP 结果，TZ 决定 localtime 的偏移（必须早于首次校时） */
    setenv("TZ", NET_TIME_NTP_TZ, 1);
    tzset();

    sntp_sync_once();
    fetch_weather_once();

    for (;;) {
        /* 定时等待：被 net_time_service_start() 的通知提前唤醒则立即重试 */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(QWEATHER_REFRESH_SEC * 1000)) > 0) {
            ESP_LOGI(TAG, "refresh requested by notify");
        }
        if (!s_time_valid) {
            sntp_sync_once();
        }
        fetch_weather_once();
    }
}

/* ==================== 对外 API ==================== */

void net_time_service_start(void)
{
    if (s_task) {
        /* 已在运行：给个通知让它立刻刷新一轮（重新联网场景） */
        xTaskNotifyGive(s_task);
        ESP_LOGI(TAG, "already running, refresh notified");
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }

    if (xTaskCreatePinnedToCore(net_time_task, "net_time", NET_TIME_TASK_STACK,
                                NULL, NET_TIME_TASK_PRIO, &s_task,
                                NET_TIME_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
        return;
    }

    ESP_LOGI(TAG, "net_time service started, ntp=%s loc=%s",
             NET_TIME_NTP_SERVER, QWEATHER_LOC_ID);
}

bool net_time_service_time_valid(void)
{
    return s_time_valid;
}

void net_time_service_get_weather(weather_data_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_weather;
        xSemaphoreGive(s_lock);
    }
}
