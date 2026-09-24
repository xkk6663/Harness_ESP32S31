/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pcnow_service.c
 * @brief PC 音乐副屏数据服务实现（见 pcnow_service.h 文件头）。
 *
 * 内存账（全 PSRAM）：
 *  - 收包任务栈 8KB（WithCaps 外置，F35）；**只解析 JSON，绝不阻塞**；
 *  - 拉取任务栈 8KB（F44 新增，**内部 RAM**：它跑 lwIP/cJSON/JPEG 解码）；
 *  - JPEG 原始缓冲 ≤ HTTP_BUF_MAX+1 = 96KB+1（临时，解码完释放）；
 *  - 封面前后双缓冲 各 240*240*3 ≈ 172.5KB（解码输出常驻）；
 *  - 歌词数组 ≤512 行：t_ms 2KB + 文本 512*96B=48KB（换曲释放重建）。
 * 内部 RAM 新增 ≈ 8KB（拉取任务栈）。
 *
 * ★ F44（2026-09-24）：**阻塞 IO 必须离开 UDP 收包任务**。
 *   原实现在 handle_meta() 里直接 http_get —— 那是 rx_task 的上下文。抓一张封面在
 *   WiFi 上要几百 ms 到数秒，这期间不 recv，lwIP 收包缓冲溢出 ⇒ 50004 的 META/PROG
 *   成片丢（现象："心跳正常，但进度/歌词/封面时好时坏"）。
 *   ⇒ rx_task 只登记 want；fetch_task 独占阻塞 HTTP；同一 hash 连续失败
 *     FETCH_MAX_TRIES 次即放弃（原为无限重试：PC 每 3s 一拍 META ⇒ 每 3s 重下一张大图）。
 *   配套：PC 侧改成"封面/歌词**就绪才**在 META 里带 hash"，从源头消掉换曲瞬间的
 *   404 竞态；设备端因此每首歌恰好各拉一次。
 */

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"          /* WithCaps 任务族（F35） */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/network/pcnow_service.h"

static const char *TAG = "PCNOW";

/* ---- 常量 ---- */
#define RX_BUF_LEN        1536               /* NOW_* JSON 单行上限 */
#define HTTP_BUF_MAX      (96 * 1024)        /* 单次拉取上限（封面 64KB 内） */
#define COVER_W_MAX       240
#define COVER_H_MAX       240
#define COVER_PIXELS      (COVER_W_MAX * COVER_H_MAX)
#define LRC_MAX_LINES     512
#define LRC_TEXT_BYTES    96                 /* 单行文本容量（含 NUL） */
#define OFFLINE_AS_MS     3 * 1000           /* 进度内插越过曲长就封顶 */

/* ★★ 歌词对齐提前量（F47，2026-09-24）----------------------------------
 * 实机实测：设备端高亮行比 PC 端**晚 1.25s**（拿 PC 自己的副屏预览做基准）。
 * 成因是链路上累积的**固定偏置**，不是抖动：
 *   - PC 的 SMTC/内插位置本身滞后于听感（RTP 式缓冲 + 采样时刻滞后）；
 *   - NOW_PROG 只有 1Hz，PC 算的行号在两拍之间不刷新，最多再叠一个推送周期。
 * 偏置恒定 ⇒ 在设备端一次性补掉最省事、也最可控。
 * ⚠ 这是**实测标定值**，换播放器/换 PC/换输出设备都可能不同。若日后要做成
 *   "每播放器一个值"，正确的位置是**上移到 PC 侧**（pusher 按 player 决定
 *   lead_ms 随 NOW_META 下发），设备端只读字段——那时请删掉这个宏。
 * ⚠ 只调这一个宏即可整体平移歌词；不要再去动 lrc_find_locked() 本身。 */
#define PCNOW_LYR_LEAD_MS 1000

/* ---- 双缓冲封面（解码在服务任务，读取在 LVGL 线程，指针切换在锁内） ---- */
typedef struct {
    uint8_t *rgb;                            /* 240*240*3，RGB888(LVGL 序=BGR) */
    int      w, h;
} cover_buf_t;

/* ---- 服务状态（s_mux 保护） ---- */
static SemaphoreHandle_t s_mux;
static pcnow_state_t     s_state = PCNOW_OFFLINE;

static char     s_title[PCNOW_TITLE_LEN];
static char     s_artist[PCNOW_ARTIST_LEN];
static char     s_album[PCNOW_ALBUM_LEN];
static char     s_player[PCNOW_PLAYER_LEN];
static uint32_t s_duration_ms;
static bool     s_playing;
static bool     s_have_meta;

/* 进度内插时钟：NOW_PROG(1Hz) 打点 + esp_timer 外推 */
static uint32_t s_prog_pos_ms;
static int64_t  s_prog_wall_us;
/* ★ ICD V1.3 行号通道（P0-3）：PC 端算好并做过迟滞的行号，-1 = 未提供。
   ★ F47 修正（2026-09-24）：**不再"有值就优先采信"**。PC 的行号是按**未加提前量**
     的位置算的，直接采用会让 PCNOW_LYR_LEAD_MS 在"PC line 生效"时**静默失效**
     （两条取行路各走各的 ⇒ 补偿时灵时不灵）。现在改为与"本机带提前量的二分结果"
     取**更靠后者**，见 pcnow_service_get_info()。 */
static int      s_prog_line = -1;
static char     s_sync_src[PCNOW_SYNC_SRC_LEN];   /* sync_src，UI 显示置信度 */

/* 歌词（v1.1 全量） */
static uint32_t *s_lrc_t;                    /* LRC_MAX_LINES */
static char     (*s_lrc_txt)[LRC_TEXT_BYTES];
static int      s_lrc_count;
static int      s_lrc_index;                 /* 当前高亮行；-1 无 */
static char     s_lrc_pushed[PCNOW_LRC_LEN]; /* v1 回退：最近一次推送行 */
static uint32_t s_lrc_seq;
static char     s_lrc_hash[PCNOW_COVER_HASH_LEN];   /* 已下发的歌词 hash */

/* 封面 */
static cover_buf_t s_cover[2];               /* [0]=front(UI 读) [1]=back(解码写) */
static int      s_cover_front;
static uint32_t s_cover_id;
static bool     s_cover_ready;
static char     s_cover_hash[PCNOW_COVER_HASH_LEN]; /* 已就绪的封面 hash */
static char     s_cover_want[PCNOW_COVER_HASH_LEN]; /* META 里宣告的 hash */

/* ---- F44：异步拉取状态 ----
 * ★ 原实现在 handle_meta() 里**直接** http_get，而 handle_meta 跑在 **UDP 收包任务**
 *   里。抓一张封面在 WiFi 上要几百 ms 到数秒，这期间 rx_task 停在 HTTP 上不 recv，
 *   lwIP 收包缓冲溢出 ⇒ 50004 的 META/PROG 成片丢。
 *   现象正是"心跳正常，但进度/歌词/封面时好时坏 + 丢包率高"。
 *   ⇒ rx_task 只登记"想要什么"，阻塞 IO 全部交给 fetch_task。
 *   顺带解决第二个坑：同一 hash 失败后无限重试（PC 每 3s 一拍 META ⇒ 每 3s 重下
 *   一张大图）—— 这里按 hash 记失败次数，超上限就放弃本曲。
 */
static char     s_lrc_want[PCNOW_COVER_HASH_LEN];   /* rx 写 / fetch 取走 */
static char     s_lrc_try_hash[PCNOW_COVER_HASH_LEN];
static int      s_lrc_tries;
static char     s_cover_try_hash[PCNOW_COVER_HASH_LEN];
static int      s_cover_tries;

#define FETCH_MAX_TRIES  3          /* 同一 hash 连续失败至此即放弃（换曲清零） */
#define FETCH_TICK_MS    100

static TaskHandle_t s_task;
static TaskHandle_t s_fetch_task;
static volatile bool s_task_run;

/* =====================================================================
 * 小工具
 * ===================================================================== */

static uint32_t pos_now_locked(void)
{
    uint32_t pos = s_prog_pos_ms;
    if (s_playing) {
        int64_t dt_us = esp_timer_get_time() - s_prog_wall_us;
        pos += (uint32_t)(dt_us / 1000);
    }
    if (s_duration_ms > 0 && pos > s_duration_ms) {
        pos = s_duration_ms;
    }
    return pos;
}

/* 二分：满足 t<=pos 的最后一行；无则 -1（歌曲开头/间奏） */
static int lrc_find_locked(uint32_t pos)
{
    if (s_lrc_count == 0) {
        return -1;
    }
    int lo = 0, hi = s_lrc_count - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_lrc_t[mid] <= pos) { ans = mid; lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return ans;
}

/* 歌词"取词位置" = 本机内插位置 + 提前量（钳到曲长）。
   ★ 为什么不直接把 out->pos_ms 提前：pos_ms 是**实际播放位置**，进度条与
     时间显示要用真值；歌词要的是"听感提前量"。混用会让进度条跟着虚报 1.25s。 */
static uint32_t lyr_pos_locked(void)
{
    uint32_t pos = pos_now_locked() + PCNOW_LYR_LEAD_MS;
    if (s_duration_ms > 0 && pos > s_duration_ms) {
        pos = s_duration_ms;
    }
    return pos;
}

static void lrc_free_locked(void)
{
    if (s_lrc_t) { heap_caps_free(s_lrc_t); s_lrc_t = NULL; }
    if (s_lrc_txt) { heap_caps_free(s_lrc_txt); s_lrc_txt = NULL; }
    s_lrc_count = 0;
    s_lrc_index = -1;
}

static void bump_lrc_locked(void)
{
    s_lrc_seq++;
}

/* =====================================================================
 * HTTP 拉取（收包任务上下文，阻塞可接受；失败静默降级）
 * ===================================================================== */

static int http_get(const char *url, uint8_t **out, int *out_len)
{
    *out = NULL;
    *out_len = 0;
    /* ★ 多留 1 字节放 NUL（F41）：正文可能是 JSON，调用方按 C 字符串解析。
       原来只分配 HTTP_BUF_MAX 且不写终止符 ⇒ 调用方被迫"用最后一个字节当 NUL"，
       把 json.dumps 的收尾 '}' 直接抹掉 ⇒ cJSON_Parse 必然失败（歌词恒拉不到）。 */
    uint8_t *buf = heap_caps_malloc(HTTP_BUF_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "http buf alloc failed");
        return -1;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 4000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        heap_caps_free(buf);
        return -1;
    }
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        esp_http_client_cleanup(cli);
        heap_caps_free(buf);
        return -1;
    }
    esp_http_client_fetch_headers(cli);
    int total = 0;
    int n;
    while ((n = esp_http_client_read(cli, (char *)buf + total, HTTP_BUF_MAX - total)) > 0) {
        total += n;
        if (total >= HTTP_BUF_MAX) break;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (total <= 0) {
        heap_caps_free(buf);
        return -1;
    }
    buf[total] = '\0';                       /* ★ 终止符由本函数负责，调用方勿再改写 */
    *out = buf;
    *out_len = total;
    return 0;
}

/* =====================================================================
 * 封面：下载 → JPEG 解码 → RGB888(LVGL 序) → 双缓冲换面
 * ===================================================================== */

static bool cover_fetch(const char *hash)
{
    struct sockaddr_in peer;
    if (!link_manager_get_peer(&peer)) {
        return false;
    }
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/cover/%s.jpg", ip, PCNOW_HTTP_PORT, hash);

    uint8_t *jpg = NULL;
    int jpg_len = 0;
    if (http_get(url, &jpg, &jpg_len) != 0) {
        return false;
    }

    /* 解到 back 缓冲。输出格式与 overlay_wallpaper 相同：
     * 解码器 RGB888 内存序 = R,G,B；LVGL 的 LV_COLOR_FORMAT_RGB888 = B,G,R
     * ⇒ 必须 p[0]<->p[2]（教训 §28.30：交换方向注释写清两端约定）。 */
    cover_buf_t *dst = &s_cover[s_cover_front ^ 1];
    if (!dst->rgb) {
        dst->rgb = heap_caps_malloc(COVER_PIXELS * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!dst->rgb) {
            heap_caps_free(jpg);
            ESP_LOGE(TAG, "cover buf alloc failed");
            return false;
        }
    }
    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = {
        .inbuf = jpg,
        .inbuf_len = jpg_len,
        .outbuf = dst->rgb,
    };
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;
    /* 逐级定位失败原因（原来只有一个 ok=false，什么都看不出来） */
    const char *why = "未知";
    if (jpg_len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        why = "不是 JPEG（代理劫持/错误页/JSON）";
    } else if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        why = "jpeg_dec_open 失败";
    } else if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        why = "头解析失败（多半被截断）";
    } else if (info.width > COVER_W_MAX || info.height > COVER_H_MAX) {
        why = "尺寸超限（解码器只收 <=240x240）";
    } else {
        int out_len = 0;
        jpeg_dec_get_outbuf_len(dec, &out_len);
        if (out_len <= 0 || (size_t)out_len > COVER_PIXELS * 3) {
            why = "输出缓冲尺寸异常";
        } else if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
            why = "解码失败（数据不完整/被截断）";
        } else {
            why = NULL;                       /* 成功 */
        }
    }
    if (dec) {
        jpeg_dec_close(dec);
    }

    if (why != NULL) {
        /* ★ 诊断**必须**在 heap_caps_free(jpg) 之前做（F44 修复）：
         *   原实现先 free 再读 jpg[0..7]，读到的是空闲块被写回的链表指针 ⇒
         *   `head=` 恒为垃圾（如 1400005014000050，正是两个被回写的空闲块字段），
         *   把"到底收到了什么"这条最关键线索彻底带偏，白查一整天。 */
        char head_hex[20] = {0};
        for (int i = 0; i < 8 && i < jpg_len; i++) {
            snprintf(&head_hex[i * 2], 3, "%02x", jpg[i]);
        }
        ESP_LOGW(TAG, "cover decode failed (%s): %s | len=%d%s head=%s%s",
                 hash, why, jpg_len,
                 jpg_len >= HTTP_BUF_MAX ? " ★正好顶到 96KB 上限=抓取被截断" : "",
                 head_hex,
                 (info.width || info.height) ? "" : " (未解析出头)");
        if (info.width || info.height) {
            ESP_LOGW(TAG, "  └ 解码器读到的尺寸 = %dx%d（超 240 就是 CDN 没按 param 缩）",
                     info.width, info.height);
        }
        heap_caps_free(jpg);
        return false;
    }

    /* 交换只对成功的这一路做（原实现写在中间，失败时 dst->rgb 被改坏）：
     * 解码器 RGB888 内存序 = R,G,B；LVGL 的 LV_COLOR_FORMAT_RGB888 = B,G,R
     * ⇒ 必须 p[0]<->p[2]（教训 §28.30：交换方向注释写清两端约定）。 */
    size_t px = (size_t)info.width * info.height;
    uint8_t *p = dst->rgb;
    for (size_t i = 0; i < px; i++) {
        uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
        p += 3;
    }
    dst->w = info.width;
    dst->h = info.height;
    heap_caps_free(jpg);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cover_front ^= 1;                      /* front/back 互换 = 原子换面 */
    s_cover_id++;
    s_cover_ready = true;
    strlcpy(s_cover_hash, hash, sizeof(s_cover_hash));   /* 原为死变量：记下已就绪的曲 */
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "cover ready %dx%d (%s) %dB", dst->w, dst->h, hash, jpg_len);
    return true;
}

/* =====================================================================
 * 歌词 v1.1：GET /lyrics.json?k=<hash> → 行数组
 * ===================================================================== */

/** 拉取全量歌词；**返回是否真的装上了**（F44：fetch_task 靠它记失败次数）。 */
static bool lrc_fetch(const char *hash)
{
    struct sockaddr_in peer;
    if (!link_manager_get_peer(&peer)) {
        return false;
    }
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/lyrics.json?k=%s", ip, PCNOW_HTTP_PORT, hash);

    uint8_t *body = NULL;
    int body_len = 0;
    if (http_get(url, &body, &body_len) != 0) {
        return false;
    }
    /* ★ 不要再写 body[body_len - 1] = '\0'（F41）：那会把 json.dumps 的收尾
       '}' 抹成 NUL，cJSON_Parse 恒失败。http_get 已保证 buf[body_len] == '\0'。 */

    cJSON *root = cJSON_Parse((const char *)body);
    if (!root) {
        /* ★ 诊断：len + 头部原文 —— 7b 开头应是合法 JSON；其余=HTML/文本错误页 */
        ESP_LOGW(TAG, "lyrics parse failed len=%d head=%.48s", body_len, (const char *)body);
        heap_caps_free(body);
        return false;
    }
    cJSON *lines = cJSON_GetObjectItem(root, "lines");
    int n = cJSON_GetArraySize(lines);
    if (n > LRC_MAX_LINES) {
        n = LRC_MAX_LINES;
    }
    if (!cJSON_IsArray(lines) || n == 0) {
        /* PC 侧已改成"歌词就绪才宣告"，走到这里多半是 404 {"error":...} 或真无歌词 */
        ESP_LOGW(TAG, "lyrics 无内容 (%s) len=%d head=%.32s", hash, body_len,
                 (const char *)body);
        cJSON_Delete(root);
        heap_caps_free(body);
        return false;
    }

    uint32_t *t = heap_caps_malloc(sizeof(uint32_t) * n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char (*txt)[LRC_TEXT_BYTES] = heap_caps_malloc((size_t)LRC_TEXT_BYTES * n,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t || !txt) {
        if (t) heap_caps_free(t);
        if (txt) heap_caps_free(txt);
        cJSON_Delete(root);
        heap_caps_free(body);
        ESP_LOGW(TAG, "lyrics alloc failed n=%d", n);
        return false;
    }
    int k = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, lines) {
        cJSON *jt = cJSON_GetObjectItem(it, "t");
        cJSON *jx = cJSON_GetObjectItem(it, "text");
        if (!jt || !jx || !cJSON_IsNumber(jt) || !cJSON_IsString(jx) || k >= n) continue;
        t[k] = (uint32_t)(jt->valuedouble < 0 ? 0 : jt->valuedouble);
        strncpy(txt[k], jx->valuestring ? jx->valuestring : "", LRC_TEXT_BYTES - 1);
        txt[k][LRC_TEXT_BYTES - 1] = '\0';
        k++;
    }
    cJSON_Delete(root);
    heap_caps_free(body);
    if (k == 0) {
        heap_caps_free(t);
        heap_caps_free(txt);
        return false;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    lrc_free_locked();
    /* ★ 单位哨兵（ICD §7.3 规定 t 为**毫秒**）。
     *   历史上 PC(Python) 侧误按**秒**下发，设备端 `(uint32_t)valuedouble` 直接
     *   截断（4.2s → 4ms）⇒ `lrc_find_locked(pos_ms)` 的二分**恒返回最后一行**，
     *   现象是"歌词一直停在最后一句、完全不滚动"。
     *   判据极紧、不可能误伤真毫秒数据：一小时内不可能有"末行时间戳 ≤3.6s 而
     *   全曲时长 ≥60s"的曲子（真毫秒下末行必然 ≥ 10^5 量级）。
     *   只提示不静默 —— 日志点名"t 单位错"，避免再次花一天去猜。 */
    if (k >= 2 && s_duration_ms >= 60000 && t[k - 1] <= 3600) {
        ESP_LOGW(TAG, "歌词时间戳疑似**秒**（末行 t=%u，曲长 %u ms）——契约应为毫秒，"
                      "已按 ×1000 纠正；请检查 PC 端 /lyrics.json 的 t 单位",
                 (unsigned)t[k - 1], (unsigned)s_duration_ms);
        for (int i = 0; i < k; i++) {
            t[i] *= 1000;
        }
    }
    s_lrc_t = t;
    s_lrc_txt = txt;
    s_lrc_count = k;
    s_lrc_index = -1;
    strlcpy(s_lrc_hash, hash, sizeof(s_lrc_hash));
    bump_lrc_locked();
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "lyrics ready: %d lines (%s)", k, hash);
    return true;
}

/* =====================================================================
 * NOW_* 消息处理
 * ===================================================================== */

static bool track_changed_locked(const char *title, const char *artist)
{
    return strcmp(s_title, title) != 0 || strcmp(s_artist, artist) != 0;
}

static void handle_meta(const cJSON *root)
{
    const cJSON *jt = cJSON_GetObjectItem(root, "title");
    const cJSON *ja = cJSON_GetObjectItem(root, "artist");
    const cJSON *jal = cJSON_GetObjectItem(root, "album");
    const cJSON *jp = cJSON_GetObjectItem(root, "player");
    const cJSON *jd = cJSON_GetObjectItem(root, "duration_ms");
    const cJSON *jcv = cJSON_GetObjectItem(root, "cover");
    const cJSON *jlr = cJSON_GetObjectItem(root, "lrc");
    if (!cJSON_IsString(jt) || !cJSON_IsString(ja)) {
        return;
    }

    char cover_hash[PCNOW_COVER_HASH_LEN] = "";
    if (cJSON_IsString(jcv) && jcv->valuestring) {
        strlcpy(cover_hash, jcv->valuestring, sizeof(cover_hash));
    }
    char lrc_hash[PCNOW_COVER_HASH_LEN] = "";
    if (cJSON_IsString(jlr) && jlr->valuestring) {
        strlcpy(lrc_hash, jlr->valuestring, sizeof(lrc_hash));
    }

    bool new_track = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    new_track = track_changed_locked(jt->valuestring, ja->valuestring);
    strlcpy(s_title, jt->valuestring, sizeof(s_title));
    strlcpy(s_artist, ja->valuestring, sizeof(s_artist));
    strlcpy(s_album, jal && cJSON_IsString(jal) ? jal->valuestring : "", sizeof(s_album));
    strlcpy(s_player, jp && cJSON_IsString(jp) ? jp->valuestring : "", sizeof(s_player));
    s_duration_ms = jd && cJSON_IsNumber(jd) && jd->valuedouble > 0
                    ? (uint32_t)jd->valuedouble : 0;
    if (new_track) {
        /* 换曲：清歌词时间轴，等拉取；进度未知（等下一拍 NOW_PROG 校正）。
         * ★ 必须连 `s_lrc_hash`/`s_cover_hash` 一起清：它们同时是"已持有"标记和
         *   下面 want 登记的判重键。只 lrc_free_locked() 而不清 hash 的话，
         *   一旦换曲前后 hash 相同（标题抖动/同曲重播），want 登记被
         *   `strcmp(lrc_hash, s_lrc_hash) != 0` 挡掉 ⇒ 数组已释放、又永不重拉
         *   ⇒ 本曲彻底没有歌词（静默失效）。代价只是极偶发的一次重拉。 */
        lrc_free_locked();
        s_lrc_hash[0] = '\0';
        s_cover_hash[0] = '\0';
        s_lrc_pushed[0] = '\0';
        s_prog_line = -1;                /* ★ V1.3：换曲后行号通道作废 */
        bump_lrc_locked();
    }
    /* ★ V1.3：同步置信度（META 才带，PROG 不带） */
    const cJSON *jss = cJSON_GetObjectItem(root, "sync_src");
    if (cJSON_IsString(jss) && jss->valuestring) {
        strlcpy(s_sync_src, jss->valuestring, sizeof(s_sync_src));
    }
    if (s_have_meta == false) {
        s_have_meta = true;
    }
    if (s_state == PCNOW_WAITING && link_manager_is_online()) {
        s_state = PCNOW_LIVE;
    }
    xSemaphoreGive(s_mux);

    /* ★ F44：这里**只登记需求**，绝不阻塞 —— 本函数跑在 UDP 收包任务里。
     *   阻塞 HTTP 由 fetch_task 做（见文件头 F44 注释）。
     *   PC 侧已改成"封面/歌词真正就绪才在 META 里带 hash"，所以登记即有货，
     *   不再是"换曲瞬间凭 hash 去拉、必然 404"的竞态。 */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (new_track) {                 /* 换曲：失败计数先清零，再判是否登记
                                        （同一首重播时 try_hash 会撞上，顺序不能反） */
        s_lrc_try_hash[0] = '\0';
        s_lrc_tries = 0;
        s_cover_try_hash[0] = '\0';
        s_cover_tries = 0;
    }
    if (lrc_hash[0] && strcmp(lrc_hash, s_lrc_hash) != 0
        && !(s_lrc_tries >= FETCH_MAX_TRIES && strcmp(lrc_hash, s_lrc_try_hash) == 0)) {
        strlcpy(s_lrc_want, lrc_hash, sizeof(s_lrc_want));
    }
    if (cover_hash[0] && strcmp(cover_hash, s_cover_hash) != 0
        && !(s_cover_tries >= FETCH_MAX_TRIES && strcmp(cover_hash, s_cover_try_hash) == 0)) {
        strlcpy(s_cover_want, cover_hash, sizeof(s_cover_want));
    }
    xSemaphoreGive(s_mux);
}

/* =====================================================================
 * F44：拉取任务 —— 唯一做阻塞 HTTP 的地方（UDP 收包任务绝不阻塞）
 * ===================================================================== */

static void fetch_task(void *arg)
{
    (void)arg;
    while (s_task_run) {
        char lrc_h[PCNOW_COVER_HASH_LEN] = "";
        char cov_h[PCNOW_COVER_HASH_LEN] = "";

        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_lrc_want[0] && strcmp(s_lrc_want, s_lrc_hash) != 0) {
            strlcpy(lrc_h, s_lrc_want, sizeof(lrc_h));
            s_lrc_want[0] = '\0';        /* 一次只试一次；失败由下一拍 META 重新登记 */
        }
        if (s_cover_want[0] && strcmp(s_cover_want, s_cover_hash) != 0) {
            strlcpy(cov_h, s_cover_want, sizeof(cov_h));
            s_cover_want[0] = '\0';
        }
        xSemaphoreGive(s_mux);

        if (lrc_h[0] && !lrc_fetch(lrc_h)) {
            int n;
            xSemaphoreTake(s_mux, portMAX_DELAY);
            if (strcmp(lrc_h, s_lrc_try_hash) != 0) {
                strlcpy(s_lrc_try_hash, lrc_h, sizeof(s_lrc_try_hash));
                s_lrc_tries = 0;
            }
            n = ++s_lrc_tries;
            xSemaphoreGive(s_mux);
            if (n == FETCH_MAX_TRIES) {
                ESP_LOGW(TAG, "lyrics %s 放弃（连续 %d 次失败），本曲不再重试", lrc_h, n);
            }
        }

        if (cov_h[0] && !cover_fetch(cov_h)) {
            int n;
            xSemaphoreTake(s_mux, portMAX_DELAY);
            if (strcmp(cov_h, s_cover_try_hash) != 0) {
                strlcpy(s_cover_try_hash, cov_h, sizeof(s_cover_try_hash));
                s_cover_tries = 0;
            }
            n = ++s_cover_tries;
            xSemaphoreGive(s_mux);
            if (n == FETCH_MAX_TRIES) {
                ESP_LOGW(TAG, "cover %s 放弃（连续 %d 次失败），本曲不再重试", cov_h, n);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FETCH_TICK_MS));
    }
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static void handle_prog(const cJSON *root)
{
    const cJSON *jp = cJSON_GetObjectItem(root, "pos_ms");
    const cJSON *jy = cJSON_GetObjectItem(root, "playing");
    const cJSON *jl = cJSON_GetObjectItem(root, "line");      /* ★ V1.3 */
    if (!cJSON_IsNumber(jp)) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_prog_pos_ms = (uint32_t)(jp->valuedouble < 0 ? 0 : jp->valuedouble);
    s_prog_wall_us = esp_timer_get_time();
    s_playing = cJSON_IsBool(jy) ? cJSON_IsTrue(jy) : s_playing;
    if (cJSON_IsNumber(jl)) {
        int ln = (int)jl->valuedouble;
        /* 越界保护：只接受 [-1, 行数) —— 越界说明两端歌词不同源（换曲竞态），
           一律弃用并在 get_info 里退回本机二分。 */
        s_prog_line = (ln < 0 || ln >= s_lrc_count) ? -1 : ln;
    } else {
        s_prog_line = -1;                 /* 老版 PC：无该字段，退回本机二分 */
    }
    xSemaphoreGive(s_mux);
}

static void handle_lrc_line(const cJSON *root)
{
    const cJSON *jx = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(jx) || !jx->valuestring) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (strcmp(s_lrc_pushed, jx->valuestring) != 0) {
        strlcpy(s_lrc_pushed, jx->valuestring, sizeof(s_lrc_pushed));
        bump_lrc_locked();                   /* v1 回退通道：行推送也推进 seq */
    }
    xSemaphoreGive(s_mux);
}

/* =====================================================================
 * 收包任务
 * ===================================================================== */

static void rx_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(PCNOW_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind %d failed errno=%d", PCNOW_UDP_PORT, errno);
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "rx ready on udp/%d", PCNOW_UDP_PORT);

    static char pkt[RX_BUF_LEN];
    while (s_task_run) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 500 * 1000};
        int ready = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) {
            continue;
        }
        int n = recv(sock, pkt, sizeof(pkt) - 1, 0);
        if (n <= 0) {
            continue;
        }
        pkt[n] = '\0';
        cJSON *root = cJSON_Parse(pkt);
        if (!root) {
            continue;
        }
        const cJSON *jt = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(jt)) {
            if (strcmp(jt->valuestring, "NOW_META") == 0) handle_meta(root);
            else if (strcmp(jt->valuestring, "NOW_PROG") == 0) handle_prog(root);
            else if (strcmp(jt->valuestring, "NOW_LRC") == 0) handle_lrc_line(root);
        }
        cJSON_Delete(root);
    }
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* =====================================================================
 * link_manager observer（第 4 槽位：tab_call / record / tab_sys 之外）
 * ===================================================================== */

static void on_link_evt(link_evt_t evt, const char *peer_ip)
{
    (void)peer_ip;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (evt == LINK_EVT_ONLINE) {
        s_state = s_have_meta ? PCNOW_LIVE : PCNOW_WAITING;
    } else {
        s_state = PCNOW_OFFLINE;
        s_playing = false;                   /* 冻结内插时钟，避免进度乱跑 */
    }
    xSemaphoreGive(s_mux);
}

/* =====================================================================
 * 对外接口
 * ===================================================================== */

void pcnow_service_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    assert(s_mux);
    for (int i = 0; i < 2; i++) {
        s_cover[i].rgb = NULL;
        s_cover[i].w = s_cover[i].h = 0;
    }
    s_task_run = true;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(rx_task, "pcnow_rx", 8192, NULL, 5, &s_task, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed (stack->PSRAM)");
        s_task_run = false;
        return;
    }
    /* ★ F44：拉取任务单独一条，做阻塞 HTTP + JPEG 解码。
     *   栈放**内部 RAM**（不用 WithCaps）：它跑 lwIP + cJSON + JPEG 解码，
     *   F35 的纪律是"碰 flash/NVS/分区 IO 的任务栈必须在内部 RAM"，这里保守起见
     *   不赌 —— 8KB 内部 RAM 换掉一整类偶发 Cache error。 */
    if (xTaskCreatePinnedToCore(fetch_task, "pcnow_fetch", 8192, NULL, 5, &s_fetch_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "fetch task create failed");
        /* 不是致命：没有它封面/歌词不会拉取，但 UDP 推流（曲名/进度）照常 */
    }
    link_manager_add_evt_cb(on_link_evt);
    ESP_LOGI(TAG, "init done (udp/%d, http pull :%d, lyric lead %d ms)",
             PCNOW_UDP_PORT, PCNOW_HTTP_PORT, PCNOW_LYR_LEAD_MS);
}

bool pcnow_service_get_info(pcnow_info_t *out)
{
    if (!out || !s_mux) {
        return false;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_have_meta) {
        xSemaphoreGive(s_mux);
        return false;
    }
    strlcpy(out->title, s_title, sizeof(out->title));
    strlcpy(out->artist, s_artist, sizeof(out->artist));
    strlcpy(out->album, s_album, sizeof(out->album));
    strlcpy(out->player, s_player, sizeof(out->player));
    out->duration_ms = s_duration_ms;
    out->pos_ms = pos_now_locked();
    out->playing = s_playing;

    /* 行来源（F47 改动，替代原先的"PC line 优先"）：
         本机按**带提前量的位置**二分   vs   PC 的 line（按**未补偿**的位置算）
       取**更靠后者**。三种情形都自洽：
       - 正常播放：本机值恒 ≥ PC 值（领先 PCNOW_LYR_LEAD_MS，且 PC 行号只有 1Hz
         刷新、天然落后本机的连续内插）⇒ 生效的是本机值 ⇒ **提前量真正落地**；
       - 播放器位置抖动（网易云那种死轴 + CDP 重锚）：本机值可能突然回退，此时
         取 PC 那个做过迟滞的值 ⇒ 仍然不会"来回跳"，PC 侧 _stable_line 的价值保住；
       - seek：两条路一起回退，最多保持 1 拍，与改动前行为一致。
       ★ 关键：不能只给本机那条路加提前量、却仍优先用 PC 的 line —— 那样补偿会在
         "PC line 生效"时**静默失效**（两条路各走各的，现象是"时灵时不灵"）。 */
    out->cover_id = s_cover_id;
    out->cover_ready = s_cover_ready;
    out->lrc_seq = s_lrc_seq;
    strlcpy(out->sync_src, s_sync_src, sizeof(out->sync_src));
    if (s_lrc_count > 0) {
        const int idx_local = lrc_find_locked(lyr_pos_locked());       /* 带提前量 */
        const int idx_pc    = (s_prog_line >= 0 && s_prog_line < s_lrc_count)
                              ? s_prog_line : -1;                      /* 未补偿 */
        const int idx       = (idx_pc > idx_local) ? idx_pc : idx_local;
        out->lrc_line_from_pc = (idx_pc > idx_local);                  /* 诊断：本拍谁赢 */
        if (idx != s_lrc_index) {
            s_lrc_index = idx;
            bump_lrc_locked();
            out->lrc_seq = s_lrc_seq;        /* 本拍就带新 seq，UI 免一轮等待 */
        }
        out->lrc_index = s_lrc_index;
        if (idx >= 0) {
            strlcpy(out->lrc_text, s_lrc_txt[idx], sizeof(out->lrc_text));
        } else {
            out->lrc_text[0] = '\0';         /* 间奏/前奏 */
        }
        /* 三行窗的上下行（v1.1 全量数组才有） */
        if (idx > 0) {
            strlcpy(out->lrc_prev, s_lrc_txt[idx - 1], sizeof(out->lrc_prev));
        } else {
            out->lrc_prev[0] = '\0';
        }
        if (idx >= 0 && idx + 1 < s_lrc_count) {
            strlcpy(out->lrc_next, s_lrc_txt[idx + 1], sizeof(out->lrc_next));
        } else {
            out->lrc_next[0] = '\0';
        }
    } else {
        out->lrc_index = -1;
        out->lrc_line_from_pc = false;
        out->lrc_prev[0] = '\0';
        out->lrc_next[0] = '\0';
        if (s_lrc_pushed[0]) {
            strlcpy(out->lrc_text, s_lrc_pushed, sizeof(out->lrc_text));
        } else {
            out->lrc_text[0] = '\0';
        }
    }
    xSemaphoreGive(s_mux);
    return true;
}

pcnow_state_t pcnow_service_get_state(void)
{
    pcnow_state_t st;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    st = s_state;
    xSemaphoreGive(s_mux);
    return st;
}

void pcnow_service_send_cmd(const char *cmd)
{
    if (!cmd || !cmd[0]) {
        return;
    }
    char json[80];
    snprintf(json, sizeof(json), "{\"type\":\"NOW_CMD\",\"cmd\":\"%s\"}", cmd);
    esp_err_t err = link_manager_send_json(json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send_cmd %s failed: %s", cmd, esp_err_to_name(err));
    }
}

void pcnow_service_send_seek(uint32_t pos_ms)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"type\":\"NOW_CMD\",\"cmd\":\"seek\",\"pos_ms\":%lu}",
             (unsigned long)pos_ms);
    esp_err_t err = link_manager_send_json(json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send_seek failed: %s", esp_err_to_name(err));
        return;
    }
    /* 本地内插基准立即跟随：滑条拖完 UI 马上到位，不等 1Hz 的 NOW_PROG；
     * PC 端收到后也会把内插跳过去，两边同源收敛。 */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_prog_pos_ms = pos_ms;
    s_prog_wall_us = esp_timer_get_time();
    /* ★ ICD V1.3：本地拖动期间先作废 PC 的行号，让 get_info 立刻按新 pos 二分
       （否则要等下一拍 PROG 才换行，手感是"拖了但歌词不动"）。
       下一拍 PROG 会带着 PC 的 line 重新建立通道。 */
    s_prog_line = -1;
    xSemaphoreGive(s_mux);
}

/** UI 取封面：返回 front 缓冲与尺寸（LVGL 线程，锁内拷指针，不拷像素）。 */
const uint8_t *pcnow_service_get_cover(int *w, int *h)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const uint8_t *p = NULL;
    if (s_cover_ready) {
        cover_buf_t *f = &s_cover[s_cover_front];
        p = f->rgb;
        *w = f->w;
        *h = f->h;
    }
    xSemaphoreGive(s_mux);
    return p;
}
