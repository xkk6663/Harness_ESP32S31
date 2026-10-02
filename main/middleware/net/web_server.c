/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_server.c
 * @brief 网页管理端实现（§74）—— 见 web_server.h 总述
 *
 * 实现纪律：
 *  ★ 只读：所有 SD 访问都是 fopen/opendir 只读路径，绝不创建/删除/改写；
 *  ★ 不占大内存：文件与 JSON 一律 4 KB 块流式发送（resp_send_chunk 循环），
 *    唯一的大缓冲是 AVI 单帧读缓冲（256 KB PSRAM，按请求分配用完即还，
 *    绝不静态占用 —— 给 §73 的 face 模型 1.29 MB 连续 PSRAM 让路）；
 *  ★ ★★ 打卡事件用"代际号轮询"而**不是 SSE**（2026-10-01 实现期修正）：
 *    esp_http_server 是**单任务串行**模型 —— handler 不返回，其它请求全部
 *    饿死。阻塞式 SSE handler 是这条铁律的教科书反例。故 punch 完成回调
 *    只更新"代际号 + 快照"槽，前端每 1 s GET /api/punch/last?gen=N 轮询，
 *    gen 变了才带全量记录 —— 无长连接、无 fd 占用、天然支持多客户端。
 *    显示延迟 ≈ 1 s，打卡场景完全够用；
 *  ★ 联调日志：每个请求打 [WEB] 一行（handler + 客户端 IP + 关键参数 +
 *    结果 + 耗时）—— 手机端不通时照日志逐环定位。
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

#include "middleware/periph/sdcard_service.h"
#include "middleware/camera/punch_service.h"
#include "middleware/media/avi_mjpeg_demuxer.h"
#include "middleware/media/wallpaper_service.h"     /* ★ §75 补充 2：壁纸管理 */
#include "middleware/net/web_server.h"

static const char *TAG = "WEB";

/* ---------------------------------------------------------------------------
 *  内嵌单页 HTML（CMake EMBED_FILES "web/index.html"）
 *  ★ 符号命名实证（build/index.html.S）：IDF data_file_embed_asm 只按
 *    **文件名**生成符号（index.html → _binary_index_html_*），不吃
 *    目录前缀 —— 首轮链接 undefined reference 的根因。
 * ------------------------------------------------------------------------- */
extern const unsigned char web_index_start[] asm("_binary_index_html_start");
extern const unsigned char web_index_end[]   asm("_binary_index_html_end");

/* ---------------------------------------------------------------------------
 *  打卡事件槽：代际号 + 快照 + 互斥锁（punch 回调写，轮询 handler 读）
 * ------------------------------------------------------------------------- */
typedef struct {
    SemaphoreHandle_t mtx;
    punch_record_t    rec;
    uint32_t          gen;          /* 每完成一次事务 +1（0 = 还没有事件） */
} evt_slot_t;

static evt_slot_t s_evt;

/* punch 完成回调（punch_task 上下文）：只许快拷 + gen++，绝不阻塞 */
static void on_punch_done(const punch_record_t *rec, void *ctx)
{
    (void)ctx;
    if (rec == NULL || s_evt.mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_evt.mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_evt.rec = *rec;
        s_evt.gen++;
        uint32_t g = s_evt.gen;
        xSemaphoreGive(s_evt.mtx);
        /* ★§74 联调锚点：每次打卡事务上抛必打这行 */
        ESP_LOGI(TAG, "evt gen=%u type=%s err=%s name=%s",
                 (unsigned)g, rec->clear_op ? "clear" : "punch",
                 esp_err_to_name(rec->err), rec->name);
    }
}

/* ---------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------- */

#define IPSTR_LEN 16

/** 请求联调日志：handler 名 + 客户端 IP */
static void log_req(httpd_req_t *req, const char *what)
{
    int fd = httpd_req_to_sockfd(req);
    char ip[IPSTR_LEN] = "?";
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (fd > 0 && getpeername(fd, (struct sockaddr *)&addr, &len) == 0 &&
        addr.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    }
    ESP_LOGI(TAG, "%s <- %s", what, ip);
}

/** 就地 URL 解码（%XX；路径里不该有 '+'，不做加号转换以免毁名） */
static void url_decode(char *s)
{
    char *w = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/** 从 query 里取 key 的值并 URL 解码。返回 ESP_OK / ESP_ERR_NOT_FOUND */
static esp_err_t query_get(httpd_req_t *req, const char *key,
                           char *out, size_t outlen)
{
    static char qbuf[512];              /* httpd 单任务串行，static 无竞争 */
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (httpd_query_key_value(qbuf, key, out, (size_t)outlen) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    url_decode(out);
    return ESP_OK;
}

/**
 * 路径安全闸：必须以 "/sdcard/" 开头、不含 ".."、前缀之后非空。
 * ★ 联调期所有 bad_path 都先看这条日志 —— 十有八九是前端没 encode。
 */
static bool path_safe(const char *p)
{
    if (p == NULL || strncmp(p, SDCARD_MNT_PATH "/", 8) != 0) {
        ESP_LOGW(TAG, "path rejected (prefix): %s", p ? p : "(null)");
        return false;
    }
    if (strstr(p, "..") != NULL) {
        ESP_LOGW(TAG, "path rejected (dotdot): %s", p);
        return false;
    }
    if (strlen(p) <= 8) {
        ESP_LOGW(TAG, "path rejected (empty): %s", p);
        return false;
    }
    return true;
}

/** JSON 字符串转义（引号/反斜杠/控制符）——姓名、文件名都过这里 */
static const char *json_esc(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;
    if (dstlen == 0) {
        return dst;
    }
    for (; *src && o + 6 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, dstlen - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;         /* UTF-8 原样透传 */
        }
    }
    dst[o] = 0;
    return dst;
}

/** 发送一个小 JSON 响应（错误语义放 body 里，前端统一按 error 字段处理） */
static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/** 按扩展名给 Content-Type */
static const char *mime_by_ext(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".png"))  return "image/png";
    if (!strcasecmp(ext, ".gif"))  return "image/gif";
    if (!strcasecmp(ext, ".csv"))  return "text/csv";
    if (!strcasecmp(ext, ".txt"))  return "text/plain";
    if (!strcasecmp(ext, ".wav"))  return "audio/wav";
    /* ★ §74 补充 5：浏览器原生解码音频 —— <audio> 直接吃这些 MIME */
    if (!strcasecmp(ext, ".mp3"))  return "audio/mpeg";
    if (!strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".aac")) return "audio/aac";
    if (!strcasecmp(ext, ".flac")) return "audio/flac";
    if (!strcasecmp(ext, ".ogg"))  return "audio/ogg";
    return "application/octet-stream";
}

/* ---------------------------------------------------------------------------
 *  Handler 1：GET /  内嵌单页
 * ------------------------------------------------------------------------- */
static esp_err_t handler_index(httpd_req_t *req)
{
    log_req(req, "index");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = (size_t)(web_index_end - web_index_start);
    esp_err_t e = httpd_resp_send(req, (const char *)web_index_start, (ssize_t)len);
    ESP_LOGI(TAG, "index sent %u B (%s)", (unsigned)len, esp_err_to_name(e));
    return e;
}

/* ---------------------------------------------------------------------------
 *  Handler 2：GET /api/punch/last?gen=N  打卡事件轮询（1 s 一拍）
 *
 *   gen == 当前代 → {"gen":G}              （无新事件，前端继续等）
 *   gen != 当前代 → {"gen":G,"has":1,...}  （带全量快照；has=0 表示槽里
 *                                            还没有任何事务完成过）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_punch_last(httpd_req_t *req)
{
    log_req(req, "punch/last");

    char gen_s[16] = "0";
    query_get(req, "gen", gen_s, sizeof(gen_s));
    uint32_t client_gen = (uint32_t)strtoul(gen_s, NULL, 10);

    char body[768];
    char name_e[FACE_NAME_MAX * 2];
    char path_e[PUNCH_PATH_MAX * 2];

    xSemaphoreTake(s_evt.mtx, portMAX_DELAY);
    uint32_t cur_gen = s_evt.gen;
    int has_new = (cur_gen != client_gen);
    punch_record_t rec = s_evt.rec;
    xSemaphoreGive(s_evt.mtx);

    /* ★ body 直接填好交给 send_json（内部按 strlen 发送），不留"设而不读"
     *   的长度变量 —— GCC15 -Wunused-but-set-variable 在 -Werror 下是硬错误 */
    if (!has_new) {
        snprintf(body, sizeof(body), "{\"gen\":%u}", (unsigned)cur_gen);
    } else if (cur_gen == 0) {
        /* 槽还没发生过任何事务：把代号给出去，快照无意义 */
        snprintf(body, sizeof(body), "{\"gen\":0,\"has\":0}");
    } else {
        json_esc(name_e, sizeof(name_e), rec.name);
        json_esc(path_e, sizeof(path_e), rec.path);
        snprintf(body, sizeof(body),
                     "{\"gen\":%u,\"has\":1,\"type\":\"%s\",\"err\":\"%s\","
                     "\"name\":\"%s\",\"person_id\":%d,\"score\":%.3f,"
                     "\"path\":\"%s\",\"time_valid\":%s,\"elapsed_ms\":%u}",
                     (unsigned)cur_gen,
                     rec.clear_op ? "clear" : "punch",
                     esp_err_to_name(rec.err),
                     name_e, rec.person_id, (double)rec.score, path_e,
                     rec.time_valid ? "true" : "false",
                     (unsigned)rec.elapsed_ms);
        ESP_LOGI(TAG, "punch/last served gen=%u %s", (unsigned)cur_gen,
                 rec.clear_op ? "clear" : rec.name);
    }
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  Handler 3：GET /api/attendance/list  打卡流水（最新在前）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_attendance_list(httpd_req_t *req)
{
    log_req(req, "attendance/list");
    int64_t t0 = esp_timer_get_time();

    punch_log_t *logs = heap_caps_malloc(sizeof(punch_log_t) * PUNCH_LOG_MAX,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!logs) {
        return send_json(req, "{\"error\":\"no_mem\"}");
    }
    int n = punch_service_log_read(logs, PUNCH_LOG_MAX);

    /* 流式拼 JSON：大数组不落地（128 条 × ~120 B，落地也就 16 KB，但流式
     * 是本文件"不占大内存"纪律的统一姿势） */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "{\"rows\":[", -1);
    char line[512], name_e[FACE_NAME_MAX * 2], rel_e[PUNCH_REL_MAX * 2];
    for (int i = n - 1; i >= 0; i--) {          /* 倒序：最新的在数组头 */
        json_esc(name_e, sizeof(name_e), logs[i].name);
        json_esc(rel_e, sizeof(rel_e), logs[i].rel);
        int m = snprintf(line, sizeof(line),
                         "%s{\"seq\":%d,\"person_id\":%d,\"name\":\"%s\","
                         "\"date\":\"%s\",\"time\":\"%s\",\"rel\":\"%s\","
                         "\"score\":%.3f,\"epoch_s\":%lld}",
                         (i == n - 1) ? "" : ",",
                         logs[i].seq, logs[i].person_id, name_e,
                         logs[i].date, logs[i].time, rel_e,
                         (double)logs[i].score, (long long)logs[i].epoch_s);
        if (httpd_resp_send_chunk(req, line, m) != ESP_OK) {
            heap_caps_free(logs);
            ESP_LOGW(TAG, "attendance/list send fail");
            return ESP_FAIL;
        }
    }
    heap_caps_free(logs);
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "attendance/list %d rows in %u ms",
             n, (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  Handler 4：GET /api/media/list?path=/sdcard/xxx  目录 JSON
 * ------------------------------------------------------------------------- */
static esp_err_t handler_media_list(httpd_req_t *req)
{
    log_req(req, "media/list");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (query_get(req, "path", path, sizeof(path)) != ESP_OK ||
        !path_safe(path)) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }

    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s failed errno=%d", path, errno);
        return send_json(req, "{\"error\":\"opendir\"}");
    }

    char path_e[PUNCH_PATH_MAX * 2];
    json_esc(path_e, sizeof(path_e), path);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[600];
    int hn = snprintf(head, sizeof(head), "{\"path\":\"%s\",\"entries\":[", path_e);
    httpd_resp_send_chunk(req, head, hn);

    struct dirent *ent;
    int count = 0;
    char line[560], nm_e[256];
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        json_esc(nm_e, sizeof(nm_e), ent->d_name);
        /* FATFS dirent 的 d_type 不可靠 —— 一律 stat 确认 */
        /* ★ GCC15 format-truncation 纪律（同 record_video_service 的
         *   _Static_assert 惯例）：path 最坏 159 + '/' + d_name 最坏 255，
         *   必须 416 才装得下 —— 编译期锁死"最坏输出 < 容量"。 */
        char full[PUNCH_PATH_MAX + 256];    /* 160 + 256 = 416 */
        _Static_assert(sizeof(full) > (PUNCH_PATH_MAX - 1) + 1 + 255,
                       "full must hold worst path + '/' + worst d_name");
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        int is_dir = 0;
        long long size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long long)st.st_size;
        }
        int m = snprintf(line, sizeof(line),
                         "%s{\"name\":\"%s\",\"dir\":%d,\"size\":%lld}",
                         count ? "," : "", nm_e, is_dir, size);
        if (httpd_resp_send_chunk(req, line, m) != ESP_OK) {
            closedir(d);
            ESP_LOGW(TAG, "media/list send fail");
            return ESP_FAIL;
        }
        count++;
    }
    closedir(d);
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "media/list %s -> %d entries in %u ms",
             path, count, (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  Handler 5：GET /api/media/file?path=...  文件内联浏览
 * ------------------------------------------------------------------------- */
static esp_err_t handler_media_file(httpd_req_t *req)
{
    log_req(req, "media/file");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (query_get(req, "path", path, sizeof(path)) != ESP_OK ||
        !path_safe(path)) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "file open fail %s errno=%d", path, errno);
        return send_json(req, "{\"error\":\"open\"}");
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *mime = mime_by_ext(path);
    ESP_LOGI(TAG, "file serve %s (%ld B, %s)", path, size, mime);

    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=60");

    static char fbuf[4096];                 /* httpd 单任务串行，static 安全 */
    long sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < size) {
        size_t want = (size_t)((size - sent) > 4096 ? 4096 : (size - sent));
        size_t got = fread(fbuf, 1, want, f);
        if (got == 0) {
            ESP_LOGW(TAG, "file short read @ %ld/%ld", sent, size);
            ret = ESP_FAIL;
            break;
        }
        if (httpd_resp_send_chunk(req, fbuf, (ssize_t)got) != ESP_OK) {
            ESP_LOGI(TAG, "file client aborted @ %ld/%ld", sent, size);
            ret = ESP_FAIL;                 /* 客户端提前关页也算异常收场 */
            break;
        }
        sent += (long)got;
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    fclose(f);
    ESP_LOGI(TAG, "file done %s %ld/%ld B in %u ms", path, sent, size,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ret;
}

/* ---------------------------------------------------------------------------
 *  Handler 6/7：GET /api/media/info 与 /api/media/frame（AVI 逐帧预览）
 * ------------------------------------------------------------------------- */
#define FRAME_BUF_SIZE  (256 * 1024)        /* 单帧 JPEG 上限（720p MJPG 亦足够） */

/** 取 AVI 路径参数（info/frame 共用） */
static esp_err_t get_avi_path(httpd_req_t *req, char *path, size_t len)
{
    if (query_get(req, "path", path, (size_t)len) != ESP_OK ||
        !path_safe(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".avi") != 0) {
        ESP_LOGW(TAG, "not an avi: %s", path);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t handler_media_info(httpd_req_t *req)
{
    log_req(req, "media/info");
    char path[PUNCH_PATH_MAX];
    if (get_avi_path(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }
    avi_demux_t *d = avi_demux_open(path);
    if (!d) {
        ESP_LOGW(TAG, "avi open fail %s", path);
        return send_json(req, "{\"error\":\"format\"}");
    }
    uint32_t fps_x100 = 0, frames = 0, w = 0, h = 0;
    avi_demux_get_info(d, &fps_x100, &frames, &w, &h);
    avi_demux_close(d);

    char body[160];
    snprintf(body, sizeof(body),
             "{\"fps_x100\":%lu,\"total_frames\":%lu,\"width\":%lu,\"height\":%lu}",
             (unsigned long)fps_x100, (unsigned long)frames,
             (unsigned long)w, (unsigned long)h);
    ESP_LOGI(TAG, "avi info %s: fps=%lu.%02lu frames=%lu %lux%lu", path,
             (unsigned long)(fps_x100 / 100), (unsigned long)(fps_x100 % 100),
             (unsigned long)frames, (unsigned long)w, (unsigned long)h);
    return send_json(req, body);
}

/* ---------------------------------------------------------------------------
 *  ★ §74 补充 5：AVI 顺序会话（单槽）
 *  httpd 单任务串行 ⇒ 一个静态会话槽天然无竞争。顺序播放（请求帧号 ==
 *  会话下一帧）每帧 O(1) 续读，消除"每请求 reopen+跳帧 → 越播越慢"的
 *  线性劣化；回退/换文件才走重开慢路径。后来者抢占会话（慢路径兜底）。
 * ------------------------------------------------------------------------- */
typedef struct {
    char         path[PUNCH_PATH_MAX];
    avi_demux_t *d;
    long         next;      /* 会话中下一次 avi_demux_next_frame 的帧号 */
    uint32_t     total;     /* 总帧数（打开时缓存，夹取用） */
} avi_sess_t;
static avi_sess_t s_avi;            /* 全零初始化 = 无会话 */

static void avi_sess_release(void)
{
    if (s_avi.d) {
        avi_demux_close(s_avi.d);
        s_avi.d = NULL;
    }
    s_avi.path[0] = 0;
    s_avi.next  = 0;
    s_avi.total = 0;
}

/**
 * 确保会话定位到 want 帧（下一次 next_frame 返回第 want 帧）。
 * @param skip_buf 跳帧时丢弃读的缓冲（借用请求的 256KB 帧缓冲）
 * @return ESP_OK / ESP_ERR_NOT_FOUND(打不开) / ESP_FAIL(跳帧中途 EOF/损坏)
 */
static esp_err_t avi_sess_seek(const char *path, long want,
                               uint8_t *skip_buf, uint32_t skip_len)
{
    if (!s_avi.d || strcmp(s_avi.path, path) != 0) {
        avi_sess_release();
        s_avi.d = avi_demux_open(path);
        if (!s_avi.d) {
            return ESP_ERR_NOT_FOUND;
        }
        snprintf(s_avi.path, sizeof(s_avi.path), "%s", path);
        avi_demux_get_info(s_avi.d, NULL, &s_avi.total, NULL, NULL);
        s_avi.next = 0;
    }
    if (s_avi.total > 0 && want >= (long)s_avi.total) {
        want = (long)s_avi.total - 1;   /* 末帧夹取（录像中途总帧数会涨） */
    }
    if (want < s_avi.next) {            /* 回退：重开 */
        avi_demux_close(s_avi.d);
        s_avi.d = avi_demux_open(path);
        if (!s_avi.d) {
            s_avi.path[0] = 0;
            return ESP_ERR_NOT_FOUND;
        }
        avi_demux_get_info(s_avi.d, NULL, &s_avi.total, NULL, NULL);
        s_avi.next = 0;
    }
    while (s_avi.next < want) {         /* 顺序略过中间帧（SD 顺序读，快） */
        uint32_t dlen = 0;
        if (avi_demux_next_frame(s_avi.d, skip_buf, skip_len, &dlen) != AVID_OK) {
            avi_sess_release();
            return ESP_FAIL;
        }
        s_avi.next++;
    }
    return ESP_OK;
}

static esp_err_t handler_media_frame(httpd_req_t *req)
{
    log_req(req, "media/frame");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (get_avi_path(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }
    char fnum[16] = "0";
    query_get(req, "frame", fnum, sizeof(fnum));
    long want = strtol(fnum, NULL, 10);
    if (want < 0) {
        want = 0;
    }

    uint8_t *fbuf = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fbuf) {
        ESP_LOGE(TAG, "frame buf alloc fail (PSRAM tight?)");
        return send_json(req, "{\"error\":\"no_mem\"}");
    }

    esp_err_t se = avi_sess_seek(path, want, fbuf, FRAME_BUF_SIZE);
    if (se != ESP_OK) {
        heap_caps_free(fbuf);
        return send_json(req, se == ESP_ERR_NOT_FOUND ? "{\"error\":\"format\"}"
                                                      : "{\"error\":\"frame\"}");
    }

    uint32_t len = 0;
    esp_err_t ret;
    avid_result_t r = avi_demux_next_frame(s_avi.d, fbuf, FRAME_BUF_SIZE, &len);
    if (r != AVID_OK) {
        ESP_LOGW(TAG, "frame %ld fail (%d) in %s", want, (int)r, path);
        avi_sess_release();
        heap_caps_free(fbuf);
        return send_json(req, "{\"error\":\"frame\"}");
    }
    s_avi.next = want + 1;              /* 会话推进：下一请求从 want+1 续读 */

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)fbuf, (ssize_t)len);
    heap_caps_free(fbuf);

    ESP_LOGI(TAG, "frame %ld/%lu %s %u B in %u ms (%s)", want,
             (unsigned long)s_avi.total, path, (unsigned)len,
             (unsigned)((esp_timer_get_time() - t0) / 1000),
             esp_err_to_name(ret));
    return ret;
}

/* ---------------------------------------------------------------------------
 *  ★ §74 补充 5：GET /api/media/frames?path=&from=&count=  批量帧
 *  一次请求连发 count(≤8) 帧：[u32 大端长度][JPEG 字节] 连排，
 *  application/octet-stream。前端双缓冲预取，把网络延迟彻底移出播放回路。
 * ------------------------------------------------------------------------- */
#define FRAMES_MAX_BATCH 8

static esp_err_t handler_media_frames(httpd_req_t *req)
{
    log_req(req, "media/frames");
    int64_t t0 = esp_timer_get_time();

    char path[PUNCH_PATH_MAX];
    if (get_avi_path(req, path, sizeof(path)) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad_path\"}");
    }
    char fnum[16] = "0", cnum[16] = "8";
    query_get(req, "from", fnum, sizeof(fnum));
    query_get(req, "count", cnum, sizeof(cnum));
    long from = strtol(fnum, NULL, 10);
    if (from < 0) {
        from = 0;
    }
    int cnt = atoi(cnum);
    if (cnt < 1) cnt = 1;
    if (cnt > FRAMES_MAX_BATCH) cnt = FRAMES_MAX_BATCH;

    uint8_t *fbuf = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fbuf) {
        return send_json(req, "{\"error\":\"no_mem\"}");
    }

    esp_err_t se = avi_sess_seek(path, from, fbuf, FRAME_BUF_SIZE);
    if (se != ESP_OK) {
        heap_caps_free(fbuf);
        return send_json(req, se == ESP_ERR_NOT_FOUND ? "{\"error\":\"format\"}"
                                                      : "{\"error\":\"frame\"}");
    }

    /* 先读第一帧成功再发响应头 —— 失败仍可回 JSON 错误 */
    uint32_t len = 0;
    if (avi_demux_next_frame(s_avi.d, fbuf, FRAME_BUF_SIZE, &len) != AVID_OK) {
        avi_sess_release();
        heap_caps_free(fbuf);
        return send_json(req, "{\"error\":\"frame\"}");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int sent = 0;
    esp_err_t ret = ESP_OK;
    for (;;) {
        /* 4 字节大端长度头 + JPEG 字节（与前端 DataView.getUint32 对齐） */
        uint8_t hdr[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                           (uint8_t)(len >> 8),  (uint8_t)len };
        if (httpd_resp_send_chunk(req, (const char *)hdr, 4) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)fbuf, (ssize_t)len) != ESP_OK) {
            ESP_LOGI(TAG, "frames client aborted @ +%d", sent);
            ret = ESP_FAIL;
            break;
        }
        sent++;
        s_avi.next++;
        if (sent >= cnt) {
            break;
        }
        if (avi_demux_next_frame(s_avi.d, fbuf, FRAME_BUF_SIZE, &len) != AVID_OK) {
            break;                      /* EOF/损坏 → 短批收尾，前端按 4B 头解析自然停 */
        }
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    heap_caps_free(fbuf);

    ESP_LOGI(TAG, "frames %ld..%ld/%lu %s (%d sent) in %u ms", from,
             from + sent - 1, (unsigned long)s_avi.total, path, sent,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ret;
}

/* ---------------------------------------------------------------------------
 *  ★ §75 补充 2：壁纸/音乐管理（PC 端上传 / 删除 / 设壁纸）
 *  写路径纪律（覆盖文件头"只读"承诺的扩展——写只允许这两条白名单通道）：
 *   · 目录白名单：wallpaper / music 二选一，其它目录一律拒；
 *   · 文件名字符白名单：禁 / \ : * ? " < > | 与控制符与 ".."；
 *   · 大小上限：壁纸 2MB / 音乐 20MB（请求头预检 + 流中复核双保险）；
 *   · 壁纸上传完成后**立即验图**（800x480 JPEG），不合格当场删除——脏文件绝不进池；
 *   · 全部走 FatFS 文件级 API（fopen/unlink），与 §75 取证结论的写路径纪律一致。
 * ------------------------------------------------------------------------- */
#define WALLPAPER_MAX_BYTES (2LL * 1024 * 1024)
#define MUSIC_MAX_BYTES     (20LL * 1024 * 1024)
#define UPNAME_MAX          80

typedef struct {
    const char *tag;        /* URL 里 dir= 参数的值 */
    const char *dir;        /* 挂载点下的绝对目录 */
    const char *exts;       /* 允许的扩展名（逗号分隔，含点，大小写不敏感） */
    long long   max_bytes;
} upload_dir_t;

static const upload_dir_t s_updirs[] = {
    { "wallpaper", WALLPAPER_POOL_DIR,      ".jpg,.jpeg", WALLPAPER_MAX_BYTES },
    { "music",     SDCARD_MNT_PATH"/music", ".mp3,.wav",  MUSIC_MAX_BYTES },
};

static const upload_dir_t *updir_find(const char *tag)
{
    for (size_t i = 0; i < sizeof(s_updirs) / sizeof(s_updirs[0]); i++) {
        if (strcmp(s_updirs[i].tag, tag) == 0) {
            return &s_updirs[i];
        }
    }
    return NULL;
}

/** 文件名安全闸：非空、< UPNAME_MAX、无 ".."、无路径/通配/控制字符 */
static bool name_safe(const char *n)
{
    if (n == NULL || n[0] == '\0' || strlen(n) >= UPNAME_MAX) {
        return false;
    }
    if (strstr(n, "..") != NULL) {
        return false;
    }
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c) != NULL) {
            return false;
        }
    }
    return true;
}

/** 扩展名是否在该目录的白名单里 */
static bool ext_allowed(const char *name, const upload_dir_t *u)
{
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    /* exts 形如 ".jpg,.jpeg"：逐段比较 */
    const char *p = u->exts;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if ((int)seg == strlen(ext) && strncasecmp(ext, p, seg) == 0) {
            return true;
        }
        p += seg + (comma ? 1 : 0);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 *  GET /api/wallpaper/list —— 池列表 + 当前壁纸名
 * ------------------------------------------------------------------------- */
static esp_err_t handler_wallpaper_list(httpd_req_t *req)
{
    log_req(req, "wallpaper/list");

    char cur[80] = "";
    wallpaper_service_get_current_name(cur, sizeof(cur));
    char cur_e[160];
    json_esc(cur_e, sizeof(cur_e), cur);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[224];
    int hn = snprintf(head, sizeof(head), "{\"current\":\"%s\",\"files\":[", cur_e);
    httpd_resp_send_chunk(req, head, hn);

    char line[384], nm_e[160];
    int count = 0;
    DIR *d = opendir(WALLPAPER_POOL_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *nm = ent->d_name;
            const char *ext = strrchr(nm, '.');
            if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) {
                continue;
            }
            char full[PUNCH_PATH_MAX + 160];
            _Static_assert(sizeof(full) > (PUNCH_PATH_MAX - 1) + 1 + 79,
                           "full must hold worst path + '/' + worst name");
            snprintf(full, sizeof(full), "%s/%s", WALLPAPER_POOL_DIR, nm);
            struct stat st;
            long long size = 0;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                size = (long long)st.st_size;
            }
            json_esc(nm_e, sizeof(nm_e), nm);
            int m = snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"size\":%lld}",
                             count ? "," : "", nm_e, size);
            if (httpd_resp_send_chunk(req, line, m) != ESP_OK) {
                closedir(d);
                return ESP_FAIL;
            }
            count++;
        }
        closedir(d);
    }
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  POST /api/wallpaper/set?name=xxx.jpg —— 设为当前壁纸（异步切换）
 * ------------------------------------------------------------------------- */
static esp_err_t handler_wallpaper_set(httpd_req_t *req)
{
    log_req(req, "wallpaper/set");

    char name[UPNAME_MAX] = "";
    if (query_get(req, "name", name, sizeof(name)) != ESP_OK || !name_safe(name)) {
        return send_json(req, "{\"error\":\"bad_name\"}");
    }
    const char *ext = strrchr(name, '.');
    if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg"))) {
        return send_json(req, "{\"error\":\"bad_ext\"}");
    }
    /* 提前给明确错误：名字必须在池内（request_set 侧还会再校验一次） */
    char full[PUNCH_PATH_MAX + UPNAME_MAX];
    _Static_assert(sizeof(full) > (PUNCH_PATH_MAX - 1) + 1 + (UPNAME_MAX - 1),
                   "full must hold worst path + '/' + worst name");
    snprintf(full, sizeof(full), "%s/%s", WALLPAPER_POOL_DIR, name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_json(req, "{\"error\":\"not_in_pool\"}");
    }

    esp_err_t e = wallpaper_service_request_set(name);
    if (e != ESP_OK) {
        char body[96];
        snprintf(body, sizeof(body), "{\"error\":\"busy\",\"rc\":\"%s\"}",
                 esp_err_to_name(e));
        return send_json(req, body);
    }
    ESP_LOGI(TAG, "wallpaper set -> '%s'", name);
    return send_json(req, "{\"ok\":1}");
}

/* ---------------------------------------------------------------------------
 *  POST /api/upload?dir=wallpaper|music&name=xxx —— 原始 body 流式落盘
 *  前端用 XHR 直接发文件二进制（不搞 multipart，文件名走 query）。
 * ------------------------------------------------------------------------- */
static esp_err_t handler_upload(httpd_req_t *req)
{
    log_req(req, "upload");

    char dir[16] = "";
    if (query_get(req, "dir", dir, sizeof(dir)) != ESP_OK) {
        return send_json(req, "{\"error\":\"no_dir\"}");
    }
    const upload_dir_t *u = updir_find(dir);
    if (!u) {
        return send_json(req, "{\"error\":\"bad_dir\"}");
    }
    char name[UPNAME_MAX] = "";
    if (query_get(req, "name", name, sizeof(name)) != ESP_OK || !name_safe(name)) {
        return send_json(req, "{\"error\":\"bad_name\"}");
    }
    if (!ext_allowed(name, u)) {
        return send_json(req, "{\"error\":\"bad_ext\"}");
    }
    if (req->content_len <= 0 || (long long)req->content_len > u->max_bytes) {
        return send_json(req, "{\"error\":\"too_large\"}");
    }

    char full[PUNCH_PATH_MAX + UPNAME_MAX];
    snprintf(full, sizeof(full), "%s/%s", u->dir, name);
    /* ★ §75 补充 2 修订：空卡上目录不存在时先补建（EEXIST 无害）——
     *   music 由"设备录一次音"顺带创建、wallpaper 此前没人建，纯格式化空卡
     *   直接网页上传会 fopen 失败。写入目标已受目录白名单约束，这里 mkdir 无风险。 */
    mkdir(u->dir, 0775);
    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGW(TAG, "upload open fail %s errno=%d", full, errno);
        return send_json(req, "{\"error\":\"open\"}");
    }

    static char rbuf[4096];                 /* httpd 单任务串行，static 安全 */
    long long total = 0;
    int remaining = req->content_len;
    esp_err_t fail = ESP_OK;
    while (remaining > 0) {
        int want = remaining > (int)sizeof(rbuf) ? (int)sizeof(rbuf) : remaining;
        int got = httpd_req_recv(req, rbuf, (size_t)want);
        if (got <= 0) {
            fail = ESP_FAIL;                /* 断连 / 超时 */
            break;
        }
        if (fwrite(rbuf, 1, (size_t)got, f) != (size_t)got) {
            fail = ESP_FAIL;                /* 卡满 / IO 错 */
            break;
        }
        total += got;
        remaining -= got;
        if (total > u->max_bytes) {         /* 流中复核（防谎报 Content-Length） */
            fail = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    fclose(f);
    if (fail != ESP_OK) {
        unlink(full);
        ESP_LOGW(TAG, "upload aborted (%s), removed %s",
                 esp_err_to_name(fail), full);
        return send_json(req, fail == ESP_ERR_INVALID_SIZE
                              ? "{\"error\":\"too_large\"}" : "{\"error\":\"recv\"}");
    }

    /* 壁纸特检：800x480 JPEG，不合格当场删——脏文件绝不进池 */
    if (strcmp(u->tag, "wallpaper") == 0) {
        esp_err_t v = wallpaper_service_validate_file(full);
        if (v != ESP_OK) {
            unlink(full);
            ESP_LOGW(TAG, "upload rejected: '%s' not a 800x480 JPEG (%s), removed",
                     name, esp_err_to_name(v));
            return send_json(req, "{\"error\":\"not_800x480_jpeg\"}");
        }
    }
    ESP_LOGI(TAG, "upload ok: %s (%lld B)", full, total);
    return send_json(req, "{\"ok\":1}");
}

/* ---------------------------------------------------------------------------
 *  DELETE /api/file?dir=wallpaper|music&name=xxx —— 白名单目录内删除
 * ------------------------------------------------------------------------- */
static esp_err_t handler_file_delete(httpd_req_t *req)
{
    log_req(req, "file/delete");

    char dir[16] = "";
    if (query_get(req, "dir", dir, sizeof(dir)) != ESP_OK) {
        return send_json(req, "{\"error\":\"no_dir\"}");
    }
    const upload_dir_t *u = updir_find(dir);
    if (!u) {
        return send_json(req, "{\"error\":\"bad_dir\"}");
    }
    char name[UPNAME_MAX] = "";
    if (query_get(req, "name", name, sizeof(name)) != ESP_OK || !name_safe(name)) {
        return send_json(req, "{\"error\":\"bad_name\"}");
    }
    char full[PUNCH_PATH_MAX + UPNAME_MAX];
    snprintf(full, sizeof(full), "%s/%s", u->dir, name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_json(req, "{\"error\":\"not_found\"}");
    }
    if (unlink(full) != 0) {
        return send_json(req, "{\"error\":\"unlink\"}");
    }
    ESP_LOGI(TAG, "deleted %s", full);
    return send_json(req, "{\"ok\":1}");
}

/* ---------------------------------------------------------------------------
 *  启动
 * ------------------------------------------------------------------------- */
static const httpd_uri_t s_uris[] = {
    { .uri = "/",                    .method = HTTP_GET,    .handler = handler_index,             .user_ctx = NULL },
    { .uri = "/api/punch/last",      .method = HTTP_GET,    .handler = handler_punch_last,        .user_ctx = NULL },
    { .uri = "/api/attendance/list", .method = HTTP_GET,    .handler = handler_attendance_list,   .user_ctx = NULL },
    { .uri = "/api/media/list",      .method = HTTP_GET,    .handler = handler_media_list,        .user_ctx = NULL },
    { .uri = "/api/media/file",      .method = HTTP_GET,    .handler = handler_media_file,        .user_ctx = NULL },
    { .uri = "/api/media/info",      .method = HTTP_GET,    .handler = handler_media_info,        .user_ctx = NULL },
    { .uri = "/api/media/frame",     .method = HTTP_GET,    .handler = handler_media_frame,       .user_ctx = NULL },
    { .uri = "/api/media/frames",    .method = HTTP_GET,    .handler = handler_media_frames,      .user_ctx = NULL },
    { .uri = "/api/wallpaper/list",  .method = HTTP_GET,    .handler = handler_wallpaper_list,    .user_ctx = NULL },
    { .uri = "/api/wallpaper/set",   .method = HTTP_POST,   .handler = handler_wallpaper_set,     .user_ctx = NULL },
    { .uri = "/api/upload",          .method = HTTP_POST,   .handler = handler_upload,            .user_ctx = NULL },
    { .uri = "/api/file",            .method = HTTP_DELETE, .handler = handler_file_delete,       .user_ctx = NULL },
};

esp_err_t web_server_init(void)
{
    static bool s_started = false;
    if (s_started) {
        return ESP_OK;
    }

    memset(&s_evt, 0, sizeof(s_evt));
    s_evt.mtx = xSemaphoreCreateMutex();
    if (!s_evt.mtx) {
        return ESP_ERR_NO_MEM;
    }
    punch_service_set_done_cb(on_punch_done, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;                  /* 流式 handler + snprintf 需要 */
    cfg.max_uri_handlers = 16;              /* §75 补充 2：8 只读 + 4 管理端点 */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 30;
    cfg.lru_purge_enable = true;            /* fd 紧张时挤掉最久未用的连接 */

    httpd_handle_t server = NULL;
    esp_err_t e = httpd_start(&server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(e));
        return e;
    }
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &s_uris[i]));
    }
    s_started = true;
    ESP_LOGI(TAG, "web server ready: %u endpoints, port %d "
             "(AP: http://192.168.4.1/ / STA: http://<device-ip>/)",
             (unsigned)(sizeof(s_uris) / sizeof(s_uris[0])), cfg.server_port);
    return ESP_OK;
}
