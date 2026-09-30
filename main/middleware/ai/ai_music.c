/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_music.c
 * @brief 小智 MCP 在线点歌工具实现（S7-B1 / §73.25 / §73.26 / §73.27 方案 B）。
 *
 * ★★ §73.27 架构反转：**不再抢占会话**——
 *   旧链路（§73.26）：cb_play 投 xiaozhi_service_stop 拆会话 → kicker 等
 *   IDLE → media_player_service 独占 ext STREAM。代价：点歌必断对话、
 *   播完要重新按 PTT 重开（上下文清零）。
 *   新链路（§73.27）：搜到 URL → xiaozhi_service_music_play(url) 投队列 →
 *   xz_task 调 audio_playback_play(s_play, url)——av_processor 的 URL
 *   播放模块（内部 esp_audio_simple_player，vocat 同款引擎），解码 PCM
 *   汇入 xz_play_write_cb 同一条下行流，与 TTS 共用 s_spk 出声。会话
 *   全程活着，LLM 随时可 stop/换曲，播完接着聊。
 *
 *   media_lib http 适配 + mp3/aac 解码器已由 audio_manager_init 自动注册
 *   （av_processor 内置），点歌通路零额外初始化。
 *
 * 上行闸门：音乐出声期间上行 skip（单麦无回采 AEC 关，§53——歌声外放
 * 会污染识别）；按 PTT = 音乐自动让路 + 正常说话（上行任务边沿处理）。
 *
 * ★ URL 来源：端侧音源搜索为主（移植 vocat page_music.c 的 k_music_apis
 *   多端点链：uomg/vvhan/codelife/Meting，逐个 HTTPS 查询，递归扫 JSON
 *   拿第一个可播音频直链）——服务端 LLM 只给歌名。url 参数已从工具上
 *   删除：MCP 引擎 tools/call 强制校验所有注册属性（esp_mcp_tool_args_cb
 *   缺一即拒，默认值不回填，2026-09-30 实锤），单参数工具才允许 LLM
 *   "只报歌名"。
 *
 * 结果回传：搜索/起播失败走 s_result_cb；播完/播失败走 xiaozhi_service
 * 的 xz_music_evt_cb 桥（on_xz_music_evt）→ 统一 XZ_EVT_TEXT 进浮窗。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_mcp_engine.h"

#include "ai_music.h"
#include "xiaozhi_service.h"

static const char *TAG = "AI_MUSIC";

/* URL 最长（https://... 长直链 + 余量；超过这个长度的直链少见，截断保护） */
#define MUSIC_URL_MAX 512
/* 音源搜索（移植 vocat）：每端点 8s、响应体 2KB；4 端点最坏 ~32s（异步不碍事） */
#define MUSIC_SEARCH_TIMEOUT_MS 8000
#define MUSIC_SEARCH_BODY_MAX   2048
#define MUSIC_KICK_STACK        (12 * 1024)   /* HTTPS+cJSON：纯网络任务，F35 允 PSRAM 栈 */

/* ---- 端侧音源搜索（移植 vocat k_music_apis 骨架 + 2026-09-30 端点实测） ---- */

/* 无 key 公开音源端点（歌名拼在 '=' 后）。免费 API 有生有灭，多备几个
 * 轮询回退；下面的递归 JSON 扫描器不挑响应形状，端点活着就能用。
 * ★ 2026-09-30 实测（PC + 真机双端）：
 *   - api.uomg.com / api.vvhan.com：域名解析失败（getaddrinfo 202）——已死，删；
 *   - api.injahow.cn/meting：type=song 需要 id 不是歌名（返回说明页）——删；
 *   - api.codelife.cc：活着但 data:null（源空），保留观察；
 *   - api.qijieya.cn/meting：★活（日调 65 万）。type=search&limit 在前、
 *     id=歌名 在后拼 query；返回数组每项的 url 字段 = 取链端点
 *     （type=url&id=...），播放器 gmf http io 自动跟随 302 到网易云真链
 *     （esp_gmf_io_http.c:216 _stream_redirect），端侧无需二次解析；
 *   - GD Studio：★活（另一家独立站，与 qijieya 互为冗余），两步式专用
 *     管线 music_search_gdstudio（search 拿 url_id → url 直接返回网易云
 *     真 mp3 直链 JSON，连 302 都不用）。
 * ★ 版权性质（实话）：这类源是第三方"解析代理"——mp3 文件本身都在网易云
 *   官方 CDN 上，代理只是代拿直链。免费歌的试听直链本就公开（官方也有
 *   outer/url 外链机制）；"VIP 解析"则是绕过付费墙，属灰色地带，站点
 *   随时可能被版权方投诉关停（uomg/vvhan 疑似即此下场）。个人学习设备
 *   实际风险很低，产品化必须换正版授权。 */
static const char *const k_music_apis[] = {
    "https://api.qijieya.cn/meting/?type=search&limit=5&id=",
    "https://api.codelife.cc/api/music?name=",
    NULL,
};

/* GD Studio 两步式（search → url_id → 直链 JSON） */
#define MUSIC_GD_SEARCH_URL \
    "https://music-api.gdstudio.xyz/api.php?types=search&source=netease&name="
#define MUSIC_GD_URL_API \
    "https://music-api.gdstudio.xyz/api.php?types=url&source=netease&br=320&id="

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} music_http_acc_t;

static esp_err_t music_http_event(esp_http_client_event_t *evt)
{
    music_http_acc_t *a = (music_http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && a) {
        if (a->len + evt->data_len < a->cap) {
            memcpy(a->buf + a->len, evt->data, evt->data_len);
            a->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* 歌名做百分号编码（UTF-8 字节序）拼进查询串 */
static void music_url_encode(const char *src, char *dst, size_t dst_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (j + 3 < dst_cap) {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        } else {
            break;
        }
    }
    dst[j] = '\0';
}

/* 启发式：这串像不像音频直链（区别于封面图/歌词）。
 * "type=url" = Meting 系取链端点的特征子串（qijieya search 结果的 url
 * 字段；GET 它 302 到真 mp3，播放器自动跟随） */
static bool music_is_audio_url(const char *v)
{
    return (strstr(v, ".mp3")  || strstr(v, ".m4a")  || strstr(v, ".aac") ||
            strstr(v, ".flac") || strstr(v, ".wav")  || strstr(v, ".ogg") ||
            strstr(v, ".opus") || strstr(v, "mp3")   || strstr(v, "audio") ||
            strstr(v, "music") || strstr(v, "media") || strstr(v, "play") ||
            strstr(v, "song")  || strstr(v, "type=url"));
}

static bool music_is_image_url(const char *v)
{
    return (strstr(v, ".jpg") || strstr(v, ".jpeg") || strstr(v, ".png") ||
            strstr(v, ".webp") || strstr(v, ".gif"));
}

/* 递归扫 JSON 树取第一个可用直链。mode 0 优先音频；mode 1 收编任何
 * 非图片/歌词的 http(s) 串 —— 不挑端点响应形状（顶层键/嵌套 data/数组通吃） */
static const char *music_find_url(cJSON *node, char *out, size_t out_len, int mode)
{
    if (node == NULL || out == NULL) {
        return NULL;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        const char *v = node->valuestring;
        if (strncmp(v, "http", 4) == 0) {
            if (mode == 0) {
                if (music_is_audio_url(v)) {
                    snprintf(out, out_len, "%s", v);
                    return out;
                }
            } else {
                if (!music_is_image_url(v) && !strstr(v, ".lrc")) {
                    snprintf(out, out_len, "%s", v);
                    return out;
                }
            }
        }
        return NULL;
    }
    if (cJSON_IsArray(node)) {
        cJSON *e;
        cJSON_ArrayForEach(e, node) {
            if (music_find_url(e, out, out_len, mode)) {
                return out;
            }
        }
    } else if (cJSON_IsObject(node)) {
        cJSON *c;
        cJSON_ArrayForEach(c, node) {
            if (music_find_url(c, out, out_len, mode)) {
                return out;
            }
        }
    }
    return NULL;
}

/* 通用 HTTPS GET：收响应体到 body（TLS 走证书 bundle 验证）。
 * 成功返回 true 并填 *out_len。 */
static bool music_http_get(const char *req, char *body, size_t body_cap, size_t *out_len)
{
    music_http_acc_t acc = { .buf = body, .cap = body_cap, .len = 0 };

    esp_http_client_config_t cfg = {
        .url = req,
        .event_handler = music_http_event,
        .user_data = &acc,
        /* ★ §73.27 修复（2026-09-30 实机日志 0x8017）：缺验证选项时
         * esp-tls 直接拒建连接（"No server verification option set" →
         * ESP_ERR_MBEDTLS_SSL_SETUP_FAILED）——vocat 原版带此字段，移植
         * 时遗漏。证书 bundle 工程默认已开（CONFIG_MBEDTLS_CERTIFICATE_
         * BUNDLE=y），零额外内存。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = MUSIC_SEARCH_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    esp_err_t r = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (r != ESP_OK || status < 200 || status >= 300 || acc.len == 0) {
        return false;
    }
    body[acc.len < body_cap ? acc.len : body_cap - 1] = '\0';
    *out_len = acc.len;
    return true;
}

/* 查一个端点：HTTPS GET → 收 2KB 响应体 → 两轮递归扫描出直链。
 * 成功返回 true 并填 out_url。跑在 kicker 任务（PSRAM 栈 12KB）。 */
static bool music_search_one(const char *api, const char *enc_name,
                             char *out_url, size_t out_url_len)
{
    /* 最坏 = 最长端点字面量(~63) + 全编码歌名(≤511) + NUL —— 编译期可证 */
    char req[MUSIC_URL_MAX + 64];
    snprintf(req, sizeof(req), "%s%s", api, enc_name);

    char *body = heap_caps_malloc(MUSIC_SEARCH_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return false;
    }
    bool ok = false;
    size_t n = 0;
    if (music_http_get(req, body, MUSIC_SEARCH_BODY_MAX, &n)) {
        cJSON *root = cJSON_Parse(body);
        if (root != NULL) {
            /* 两轮：先挑"像音频"的直链，没有再收编任何非图片串 */
            if (music_find_url(root, out_url, out_url_len, 0) == NULL) {
                music_find_url(root, out_url, out_url_len, 1);
            }
            cJSON_Delete(root);
            ok = (out_url[0] != '\0');
        }
    }
    heap_caps_free(body);
    ESP_LOGI(TAG, "search %s -> %s", req, ok ? out_url : "(no url)");
    return ok;
}

/* ★ GD Studio 两步式（另一家独立解析站，与 qijieya 互为冗余）：
 *   GET types=search&name=<歌名> → 数组取第一项 url_id
 *   → GET types=url&id=<url_id> → {"url":"...mp3"}（真直链，无 302） */
static bool music_search_gdstudio(const char *enc_name,
                                  char *out_url, size_t out_url_len)
{
    /* 最坏 = GD 搜索前缀(~73) + 全编码歌名(≤511) + "&count=5"(9) + NUL
     * —— 编译期可证（-Werror=format-truncation 实抓过一次，576 不够） */
    char req[MUSIC_URL_MAX + 128];
    char *body = heap_caps_malloc(MUSIC_SEARCH_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return false;
    }
    bool ok = false;
    size_t n = 0;

    snprintf(req, sizeof(req), "%s%s&count=5", MUSIC_GD_SEARCH_URL, enc_name);
    if (music_http_get(req, body, MUSIC_SEARCH_BODY_MAX, &n)) {
        cJSON *root = cJSON_Parse(body);
        const char *url_id = NULL;
        if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
            cJSON *first = cJSON_GetArrayItem(root, 0);
            url_id = cJSON_GetObjectItem(first, "url_id") ?
                     cJSON_GetStringValue(cJSON_GetObjectItem(first, "url_id")) : NULL;
        }
        cJSON_Delete(root);
        if (url_id != NULL && url_id[0] != '\0') {
            snprintf(req, sizeof(req), "%s%s", MUSIC_GD_URL_API, url_id);
            if (music_http_get(req, body, MUSIC_SEARCH_BODY_MAX, &n)) {
                cJSON *r2 = cJSON_Parse(body);
                const char *dl = r2 ? cJSON_GetStringValue(cJSON_GetObjectItem(r2, "url")) : NULL;
                if (dl != NULL && strncmp(dl, "http", 4) == 0 &&
                    strlen(dl) < out_url_len) {
                    snprintf(out_url, out_url_len, "%s", dl);
                    ok = true;
                }
                cJSON_Delete(r2);
            }
        }
    }
    heap_caps_free(body);
    ESP_LOGI(TAG, "gdstudio search -> %s", ok ? out_url : "(no url)");
    return ok;
}

/* 逐端点轮询：第一个给出直链的赢。全灭返回 false。 */
static bool music_search_url(const char *name, char *out_url, size_t out_url_len)
{
    char enc[MUSIC_URL_MAX];
    music_url_encode(name, enc, sizeof(enc));
    for (int i = 0; k_music_apis[i] != NULL; i++) {
        if (music_search_one(k_music_apis[i], enc, out_url, out_url_len)) {
            return true;
        }
    }
    return music_search_gdstudio(enc, out_url, out_url_len);
}

/* 结果回调（kicker 任务 / simple_player 播放任务上下文调用）→ XZ_EVT_TEXT */
static ai_music_result_cb_t s_result_cb;

void ai_music_set_result_cb(ai_music_result_cb_t cb)
{
    s_result_cb = cb;
}

static void report(const char *text)
{
    ESP_LOGI(TAG, "report: %s", text);
    if (s_result_cb) {
        s_result_cb(text);
    }
}

/* ★ §73.27：播放结局回传桥——xiaozhi_service 的 playback 事件
 * （simple_player 播放任务上下文）→ 对话记录。STARTED 不报（tools/call
 * 响应已告知）；STOPPED 不报（用户叫停/换曲，工具响应已答）。 */
static void on_xz_music_evt(xz_music_evt_id_t evt, esp_err_t err)
{
    (void)err;
    switch (evt) {
    case XZ_MUSIC_FINISHED:
        report("Music finished playing.");
        break;
    case XZ_MUSIC_ERROR:
        report("Music playback failed - the link is invalid or the format "
               "is not supported.");
        break;
    default:
        break;
    }
}

/* ---- 一次性 kicker：端侧搜真链 → 投会话内播放（§73.27）----
 * cb_play 跑在 MCP 引擎任务栈，绝不能做阻塞 HTTPS（4 端点最坏 32s）——
 * 搜索留给本任务（PSRAM 栈 12KB，F35 纯网络任务允外置）。
 * ★ §73.27 起不再等会话关（恰恰相反，会话必须活着）：搜到即投
 * xiaozhi_service_music_play，播放与对话并行。 */
typedef struct {
    char name[64];             /* 歌名（搜索用） */
} music_kick_t;

static void music_kick_task(void *arg)
{
    music_kick_t *k = (music_kick_t *)arg;
    char url[MUSIC_URL_MAX];

    if (!music_search_url(k->name, url, sizeof(url))) {
        report("No playable link found for this song - try another title.");
        goto out;
    }

    if (xiaozhi_service_music_play(url) != ESP_OK) {
        /* 队列满/非会话态（用户在搜索期间手动结束了对话） */
        report("Cannot start playback now - the conversation may have "
               "ended. Please ask me to play again.");
    }
out:
    heap_caps_free(k);
    vTaskDeleteWithCaps(NULL);
}

static esp_mcp_value_t cb_play(const esp_mcp_property_list_t *props)
{
    /* ★ 单参数工具（§73.27 勘误）：MCP 引擎对注册的全部属性做"缺一即拒"
     * 校验且不回填默认值（esp_mcp_engine.c esp_mcp_tool_args_cb）——
     * 所以只注册 name，LLM 报歌名即可，无 url 参数可填错。 */
    const char *name = esp_mcp_property_list_get_property_string(props, "name");
    if (name == NULL || name[0] == '\0') {
        return esp_mcp_value_create_string("Please tell me the song name to play.");
    }

    music_kick_t *k = heap_caps_malloc(sizeof(music_kick_t), MALLOC_CAP_SPIRAM);
    if (k == NULL) {
        return esp_mcp_value_create_string("Out of memory - cannot start playback.");
    }
    memset(k, 0, sizeof(*k));
    snprintf(k->name, sizeof(k->name), "%s", name);

    /* 与 ai_camera explain 任务同款：PSRAM 栈一次性网络任务（F35 允外置） */
    TaskHandle_t kick = NULL;
    BaseType_t ok = xTaskCreateWithCaps(music_kick_task, "music_kick",
                                        MUSIC_KICK_STACK, k, 5, &kick,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        heap_caps_free(k);
        return esp_mcp_value_create_string("Out of memory - cannot start playback.");
    }

    ESP_LOGI(TAG, "play scheduled: %s (device search, in-conversation)", name);
    /* 会话不断 ⇒ tools/call 响应必达，LLM 还能接着说话 */
    char ok_msg[160];
    snprintf(ok_msg, sizeof(ok_msg),
             "Okay, looking up '%s' and playing it now. The conversation "
             "stays open - I can switch or stop songs anytime.", name);
    return esp_mcp_value_create_string(ok_msg);
}

static esp_mcp_value_t cb_stop(const esp_mcp_property_list_t *props)
{
    (void)props;
    esp_err_t err = xiaozhi_service_music_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stop scheduled");
        return esp_mcp_value_create_string("Music stopped.");
    }
    return esp_mcp_value_create_string("No music is playing in this "
                                       "conversation right now.");
}

esp_err_t ai_music_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");

    esp_mcp_property_t *p_name =
        esp_mcp_property_create_with_string("name", "");
    ESP_RETURN_ON_FALSE(p_name, ESP_FAIL, TAG, "prop name");

    esp_mcp_tool_t *play = esp_mcp_tool_create(
        "self.music.play",
        "在线点歌：按歌名在公开音源搜索并播放，**边播放边保持对话不断开**。"
        "当用户说'放歌/点歌/来首歌/播放xxx'等想听歌的话时调用：把歌名（如"
        "'晴天'）填进 name 参数即可，设备会自动搜索真实播放直链——**不要自己"
        "编造任何 URL**。播放中用户说'下一首/换一首/换成xxx' = 用新歌名再次"
        "调用本工具（直接换，无需先 stop）；'别放了/停止播放'用 self.music.stop。"
        "查不到可播链接会如实回告，请引导用户换歌名。",
        cb_play);
    ESP_RETURN_ON_FALSE(play, ESP_FAIL, TAG, "tool play");
    ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(play, p_name) == ESP_OK,
                        ESP_FAIL, TAG, "add prop name");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, play) == ESP_OK,
                        ESP_FAIL, TAG, "add tool play");

    esp_mcp_tool_t *stop = esp_mcp_tool_create(
        "self.music.stop",
        "停止当前播放的音乐。当用户说'别放了/停止播放/关掉音乐'时调用。"
        "换歌不必先 stop——直接用新歌名调 self.music.play 即可。无参数。",
        cb_stop);
    ESP_RETURN_ON_FALSE(stop, ESP_FAIL, TAG, "tool stop");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, stop) == ESP_OK,
                        ESP_FAIL, TAG, "add tool stop");

    /* 播放结局回传进对话记录（播完/播失败用户看得见，不再无声） */
    xiaozhi_service_set_music_evt_cb(on_xz_music_evt);

    ESP_LOGI(TAG, "mcp tools: self.music.play(name) / self.music.stop "
                  "(in-conversation playback)");
    return ESP_OK;
}
