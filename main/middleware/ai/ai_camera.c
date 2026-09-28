/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_camera.c
 * @brief 小智 MCP 相机工具实现（S7-A1 / §58，见头文件）。
 *
 * 并发模型：
 *   - MCP 工具回调（协议栈任务）：置 s_busy + 拷 question + 建任务，立即返回；
 *     mcp-c-sdk 串行分发 ⇒ s_busy 检查-置位无竞态。
 *   - explain 任务（每次现建，PSRAM 栈，自删）：相机事务 + 同步 HTTP。
 *   - 帧回调（cam_task）：只 memcpy + 置事件组（camera_service.h 硬约束）。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"

#include "esp_mcp_engine.h"
#include "esp_xiaozhi_camera.h"
#include "esp_jpeg_dec.h"

#include "ai_camera.h"
#include "xiaozhi_service.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/capture_service.h"

static const char *TAG = "AI_CAM";

/* explain 客户端（init 创建，URL/token 每次任务里从会话现取） */
static esp_xiaozhi_camera_handle_t *s_cli;
static ai_camera_result_cb_t s_result_cb;
static volatile bool s_busy;            /* MCP 回调上下文检查-置位（串行分发） */

/* explain 任务私有（任务独占，创建后只有它自己读写） */
static char     s_question[96];
static uint8_t *s_jpg;                  /* PSRAM 帧缓冲（任务内分配释放） */
static size_t   s_jpg_len;
static EventGroupHandle_t s_frame_evt;  /* BIT0 = 抓到一帧 */

#define JPG_BUF_SIZE   (160 * 1024)     /* 1280x720 q8 实测 ~40-90KB，留余量 */
#define FRAME_WAIT_MS  3000
#define EXPLAIN_OUT_SIZE 1024           /* 视觉回答文本缓冲 */
#define EXPLAIN_STACK_SIZE (16 * 1024)  /* PSRAM 栈（纯 HTTP/TLS，F35 允许外置） */

/* ---- 拍照预览（§59b）：RGB565 缩略图单槽交接 ----
 * 生产者 = 解码所在任务（explain 任务 / capture 预览任务），
 * 消费者 = overlay_ai 50ms lv_timer（LVGL 线程）。
 * 所有权：发布时归槽；UI take 后移交 UI（显示 3s 后 UI free）。
 * 新帧到达而旧帧未被取走 → 直接丢旧（预览是赠品，永远显示最新）。 */
#define PV_W 640
#define PV_H 360                        /* 1/2 scale（两者均 8 的倍数，解码器要求） */

static uint8_t *s_pv_buf;               /* 待取/显示中的 RGB565 帧（NULL=无） */
static int      s_pv_w, s_pv_h;
static volatile bool s_pv_pending;      /* s_pv_buf 有效，等 UI 取 */
static portMUX_TYPE s_pv_mux = portMUX_INITIALIZER_UNLOCKED;

static void preview_publish(uint8_t *rgb, int w, int h)
{
    taskENTER_CRITICAL(&s_pv_mux);
    if (s_pv_pending && s_pv_buf) {
        heap_caps_free(s_pv_buf);       /* UI 没来得及取的旧帧：丢 */
    }
    s_pv_buf = rgb;
    s_pv_w = w;
    s_pv_h = h;
    s_pv_pending = true;
    taskEXIT_CRITICAL(&s_pv_mux);
    ESP_LOGI(TAG, "preview published %dx%d", w, h);
}

bool ai_camera_preview_take(uint8_t **buf, int *w, int *h)
{
    bool got = false;
    taskENTER_CRITICAL(&s_pv_mux);
    if (s_pv_pending && s_pv_buf) {
        *buf = s_pv_buf;
        *w = s_pv_w;
        *h = s_pv_h;
        s_pv_buf = NULL;
        s_pv_pending = false;
        got = true;
    }
    taskEXIT_CRITICAL(&s_pv_mux);
    return got;
}

/* JPEG 码流 → 1/2 scale RGB565 缩略图 → publish。
 * 失败静默（预览是赠品，不抢 explain/capture 主流程的报错通道）。 */
static void preview_decode_publish(const uint8_t *jpg, size_t len)
{
    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    uint8_t *out = NULL;
    int out_len = 0;

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;  /* LVGL16 原生小端，免逐像素换序 */
    cfg.scale.width = PV_W;
    cfg.scale.height = PV_H;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return;
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = (int)len };
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK ||
        jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK) {
        goto out;
    }
    out = heap_caps_malloc((size_t)out_len, MALLOC_CAP_SPIRAM);
    if (!out) {
        goto out;
    }
    io.outbuf = out;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        heap_caps_free(out);
        out = NULL;
        goto out;
    }
    {
        /* 输出尺寸对账：块对齐可能补齐，宽度以 out_len 反推（防画面错乱） */
        int w = out_len / 2 / PV_H;
        if (out_len != PV_W * PV_H * 2) {
            ESP_LOGW(TAG, "preview padded: out_len=%d -> w=%d", out_len, w);
        }
        preview_publish(out, w, PV_H);
        out = NULL;     /* 所有权已移交槽 */
    }
out:
    if (out) {
        heap_caps_free(out);
    }
    if (dec) {
        jpeg_dec_close(dec);
    }
}

static void report(const char *text)
{
    ESP_LOGI(TAG, "result: %.96s", text);
    if (s_result_cb) {
        s_result_cb(text);
    }
}

/* cam_task 上下文：只 memcpy + 置事件（camera_service.h 硬约束） */
static void on_jpeg_frame(const uint8_t *data, uint32_t size,
                          uint32_t w, uint32_t h, void *user)
{
    (void)w;
    (void)h;
    (void)user;
    if (!s_jpg || s_jpg_len != 0) {
        return;   /* 已有帧/缓冲未备好：直接丢（流会继续送下一帧） */
    }
    if (size == 0 || size > JPG_BUF_SIZE) {
        ESP_LOGW(TAG, "frame too large: %u > %u", (unsigned)size, (unsigned)JPG_BUF_SIZE);
        return;   /* 等下一帧（不该发生：q8 实测远小于缓冲） */
    }
    memcpy(s_jpg, data, size);
    s_jpg_len = size;
    xEventGroupSetBits(s_frame_evt, BIT0);
}

static void finish_with(const char *msg)
{
    report(msg);
}

static void explain_task(void *arg)
{
    (void)arg;
    char out[EXPLAIN_OUT_SIZE];
    size_t out_len = 0;
    bool cam_started = false;
    esp_err_t ret;

    /* 1. vision 端点（MCP initialize 响应里 peek 到的） */
    char url[256], token[256];
    ret = xiaozhi_service_get_vision_endpoint(url, sizeof(url), token, sizeof(token));
    if (ret != ESP_OK) {
        finish_with("Vision service was not configured for this session. "
                    "Please end the chat, start a new one and try again.");
        goto out;
    }
    ESP_RETURN_VOID_ON_ERROR(esp_xiaozhi_camera_set_explain_url(s_cli, url, token),
                             TAG, "set_explain_url");

    /* 2. 相机事务（被占用时不抢断——与 CAM/VIDEO 页和平共处） */
    if (camera_service_owner() != CAM_OWNER_NONE) {
        finish_with("Camera is busy - close the CAMERA/VIDEO page first.");
        goto out;
    }
    s_jpg = heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_jpg) {
        finish_with("Out of memory for photo buffer.");
        goto out;
    }
    s_frame_evt = xEventGroupCreate();
    if (!s_frame_evt) {
        finish_with("Out of memory for frame event.");
        goto out;
    }
    ret = camera_service_start_fmt(on_jpeg_frame, NULL, CAM_FMT_JPEG, CAM_MEDIA_W, CAM_MEDIA_H);
    if (ret != ESP_OK) {
        finish_with("Failed to start camera for photo.");
        goto out;
    }
    cam_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_frame_evt, BIT0, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(FRAME_WAIT_MS));
    camera_service_stop();
    cam_started = false;
    if (!(bits & BIT0) || s_jpg_len == 0) {
        finish_with("No frame from camera - please retry.");
        goto out;
    }
    ESP_LOGI(TAG, "captured %u bytes JPEG, uploading...", (unsigned)s_jpg_len);

    /* 先发预览（解码约百 ms 级；用户"所见"先于 AI "所答"出现） */
    preview_decode_publish(s_jpg, s_jpg_len);

    /* 3. 同步上传 + 视觉问答（独立任务里阻塞是安全的，F44） */
    esp_xiaozhi_camera_frame_t frame = { .data = s_jpg, .len = s_jpg_len };
    ret = esp_xiaozhi_camera_explain(s_cli, &frame,
                                     s_question[0] ? s_question : "请描述这张照片",
                                     out, sizeof(out), &out_len);
    if (ret == ESP_OK && out_len > 0) {
        out[(out_len < sizeof(out)) ? out_len : sizeof(out) - 1] = '\0';
        finish_with(out);
    } else {
        ESP_LOGW(TAG, "explain failed: %s", esp_err_to_name(ret));
        finish_with("Vision service failed - check network and retry.");
    }

out:
    if (cam_started) {
        camera_service_stop();
    }
    if (s_frame_evt) {
        vEventGroupDelete(s_frame_evt);
        s_frame_evt = NULL;
    }
    if (s_jpg) {
        heap_caps_free(s_jpg);
        s_jpg = NULL;
    }
    s_jpg_len = 0;
    s_busy = false;
    ESP_LOGI(TAG, "explain task done");
    vTaskDeleteWithCaps(NULL);
}

/* capture 路径预览：等 capture_service 落盘完成（受理制非阻塞），回读文件解码。
 * SD 卡 FATFS 读写不关 flash cache ⇒ PSRAM 栈安全（F35 只禁 NVS/littlefs/flash IO）。 */
#define CAPV_WAIT_MS  6000
#define CAPV_STACK    (12 * 1024)

static void cap_preview_task(void *arg)
{
    (void)arg;
    FILE *f = NULL;
    uint8_t *jpg = NULL;
    int waited = 0;

    while (capture_service_state() == CAPTURE_BUSY && waited < CAPV_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    capture_result_t res;
    capture_service_get_result(&res);
    if (res.err == ESP_OK && res.path[0]) {
        f = fopen(res.path, "rb");
        if (f) {
            jpg = heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (jpg) {
                size_t n = fread(jpg, 1, JPG_BUF_SIZE, f);
                if (n > 0) {
                    preview_decode_publish(jpg, n);
                }
            }
        } else {
            ESP_LOGW(TAG, "preview: open %s failed", res.path);
        }
    }
    if (jpg) {
        heap_caps_free(jpg);
    }
    if (f) {
        fclose(f);
    }
    vTaskDeleteWithCaps(NULL);
}

/* ---- MCP 工具回调（协议栈任务上下文：只置标志 + 建任务，立即返回） ---- */

static esp_mcp_value_t cb_explain(const esp_mcp_property_list_t *props)
{
    if (s_busy) {
        return esp_mcp_value_create_string("A photo is already being analyzed, please wait.");
    }
    if (!s_cli) {
        return esp_mcp_value_create_string("Camera service is not initialized.");
    }
    const char *q = esp_mcp_property_list_get_property_string(props, "question");
    snprintf(s_question, sizeof(s_question), "%s",
             (q && q[0]) ? q : "请用一句话描述这张照片里有什么");

    s_busy = true;
    s_jpg_len = 0;
    BaseType_t ok = xTaskCreateWithCaps(explain_task, "ai_cam", EXPLAIN_STACK_SIZE,
                                        NULL, 5, NULL, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_busy = false;
        ESP_LOGE(TAG, "create explain task failed");
        return esp_mcp_value_create_string("Failed to start the camera task (out of memory).");
    }
    ESP_LOGI(TAG, "explain requested: %.64s", s_question);
    return esp_mcp_value_create_string("Photo taken. I am analyzing it now, one moment please.");
}

/* ---- 注册 ---- */

/* §58 C 级：拍照存 /sdcard/photos（FILES 页可看）。capture_service 受理制
 * 非阻塞（切档→抓帧→落盘→归还，约 1~3s 自行完成），回调直接受理即回。
 * 与 explain 的互斥天然成立：capture 在相机被占（含 explain 事务中）时
 * 返回 INVALID_STATE。 */
static esp_mcp_value_t cb_capture(const esp_mcp_property_list_t *props)
{
    (void)props;
    esp_err_t r = capture_service_request();
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "capture requested (-> /sdcard/photos)");
        /* 预览任务（后置：等落盘完成回读解码；失败不影响照片本身） */
        if (xTaskCreateWithCaps(cap_preview_task, "ai_cappv", CAPV_STACK,
                                NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGW(TAG, "preview task create failed (photo still saved)");
        }
        return esp_mcp_value_create_string(
            "Photo taken and saved to the SD card (photos folder). "
            "It can be viewed in the FILES page.");
    }
    if (r == ESP_ERR_INVALID_STATE) {
        return esp_mcp_value_create_string(
            "Cannot take a photo now - camera is busy or SD card is not mounted.");
    }
    return esp_mcp_value_create_string("Failed to request a photo.");
}

esp_err_t ai_camera_init(void)
{
    if (s_cli) {
        return ESP_OK;   /* 幂等 */
    }
    /* 无 config：URL 等会话建立后每次现取（token 会刷新） */
    return esp_xiaozhi_camera_create(NULL, &s_cli);
}

void ai_camera_set_result_cb(ai_camera_result_cb_t cb)
{
    s_result_cb = cb;
}

esp_err_t ai_camera_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");
    ESP_RETURN_ON_FALSE(s_cli, ESP_ERR_INVALID_STATE, TAG, "ai_camera_init first");

    esp_mcp_property_t *p_q = esp_mcp_property_create_with_string(
        "question", "请用一句话描述这张照片里有什么");
    ESP_RETURN_ON_FALSE(p_q, ESP_FAIL, TAG, "prop question");

    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "self.camera.explain",
        "拍一张照片并用视觉模型回答。当用户想让你\"看看\"眼前的东西、描述看到的场景时调用。"
        "question: 想针对照片问的问题（可选）",
        cb_explain);
    ESP_RETURN_ON_FALSE(tool, ESP_FAIL, TAG, "tool create");
    ESP_RETURN_ON_FALSE(esp_mcp_tool_add_property(tool, p_q) == ESP_OK,
                        ESP_FAIL, TAG, "add prop");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, tool) == ESP_OK,
                        ESP_FAIL, TAG, "add tool");
    ESP_LOGI(TAG, "mcp tool: self.camera.explain");

    /* self.camera.capture：无参数 */
    esp_mcp_tool_t *t2 = esp_mcp_tool_create(
        "self.camera.capture",
        "拍一张照片保存到 SD 卡（photos 目录），供用户在 FILES 页查看。"
        "不需要视觉回答、只想留存照片时调用",
        cb_capture);
    ESP_RETURN_ON_FALSE(t2, ESP_FAIL, TAG, "tool create capture");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, t2) == ESP_OK,
                        ESP_FAIL, TAG, "add tool capture");
    ESP_LOGI(TAG, "mcp tool: self.camera.capture");
    return ESP_OK;
}
