/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_punch.c
 * @brief 小智 MCP 打卡工具实现（S8-5 / §73.19 / §73.23，见头文件）。
 *
 * 会话编排（全部在 MCP 工具回调 = WS 收包任务上下文，§73.18 同款阻塞契约）：
 *   缓冲 → 起相机(RGB565 240x240) → **实时取景**（右侧浮窗进视频流模式，
 *   连续推帧让用户看到自己，§73.23）→ 取景 VIEWFINDER_MS 后按需加载
 *   识别引擎 → 扫描窗口内 submit/consume（取景推帧全程不断）→
 *   命中且质量合格 → bsp_display_lock 短临界区内（水印离屏画布 +
 *   punch 两阶段事务）→ 等编码落盘 → 结果分发 → **关闭取景浮窗**
 *   （成功/失败/超时任何路径都关，UI 收尾沿立即隐藏）。
 *
 * 重活各在自己的一次性任务里：识别推理在 face_recog 的引擎任务
 * （Core0 10240 栈），JPEG 编码/落盘在 punch_service 的一次性任务；
 * 本回调只做编排与等待（阻塞点：几十 ms 节拍的轮询）。
 *
 * ★ 复刻自 tab_camera.c 的两段逻辑（改动时两边同步看）：
 *   - watermark_draw()：立即模式水印（姓名 CJK 字体 + 时间 ASCII 字体）；
 *   - 打卡准入判据：face_quality_check + 访客不落盘（PUNCH_RECORD_GUEST=0）。
 *   复刻原因：tab 的版本绑定 CAM 页 canvas/armed 状态机，解不出来；
 *   判据源（face_quality / punch_service）本身是共享模块，不会漂移。
 *
 * ★ §73.23 变更：打卡成功后的"静态水印照片卡 3s"（§73.19）被实时取景
 *   取代——用户看到的是直播画面，打卡完成即关浮窗（用户拍板"关闭就行"；
 *   水印底片仍随 punch_service 落盘存证，不丢证据）。s_wm_copy /
 *   make_punch_preview 随之移除（少 172KB 会话期常驻 + 一次全帧转换）。
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "esp_mcp_engine.h"

#include "ai_punch.h"
#include "ai_camera.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/face_recog_service.h"
#include "middleware/camera/face_quality.h"
#include "middleware/camera/punch_service.h"
#include "middleware/net/net_time_service.h"
#include "ui/ui_theme.h"
#include "ui/ui_binfont.h"

LV_FONT_DECLARE(lv_font_cn_20);   /* CJK 兜底字体（bin 字体未就绪时用，同 overlay_ai） */

static const char *TAG = "AI_PUNCH";

/* ---- 几何与预算 ---- */
#define PUNCH_W   CAM_PREVIEW_W         /* 240 */
#define PUNCH_H   CAM_PREVIEW_H         /* 240 */
#define FB_BYTES      (PUNCH_W * PUNCH_H * 2)      /* RGB565 帧 */

/* 会话时间预算（合计最坏 ~33s；§73.19 工具契约允许数秒~30s 级阻塞） */
#define VIEWFINDER_MS      2000    /* 实时取景先行：让用户看到自己再开识别（§73.23） */
#define LOAD_WAIT_MS       10000   /* 识别引擎加载（littlefs 读 + 2.1MB 峰值分配） */
#define SCAN_WINDOW_MS     15000   /* 等人脸出现并认出 */
#define PUNCH_DONE_WAIT_MS  6000   /* JPEG 编码 + 落盘 + CSV */
#define SUBMIT_GAP_MS        120   /* 提交节拍（>单次推理 324ms 的倒数没必要，防叠作业） */
#define VF_PUBLISH_GAP_MS    100   /* 取景推帧限速（~10fps，省 PSRAM 分配churn） */

/* 访客政策：与 CAM 页打卡同款（2026-09-28 用户要求"识别到符合人脸库才落盘"） */
#ifndef PUNCH_RECORD_GUEST
#define PUNCH_RECORD_GUEST  0
#endif

/* ---- 结果回调（→ XZ_EVT_TEXT → AI 浮窗对话记录） ---- */
static ai_punch_result_cb_t s_result_cb;

static volatile bool s_busy;    /* MCP 回调上下文检查-置位（串行分发 ⇒ 无竞态） */

/* ---- 帧交接（cam_task → 会话任务）：双缓冲 + 临界区换手 ----
 * on_frame（cam_task）：只 memcpy + 换指针（camera_service.h 硬约束）；
 * 会话侧取"最近写完"的那块做 BE→LE 转换。残余竞态窗口（下一帧 memcpy
 * 打到正在读的块）约 0.5ms，远小于帧周期 ~66ms，撕裂帧会被质量门槛拦下重试。 */
static uint8_t *s_fb[2];
static volatile int  s_fb_wi;
static volatile bool s_fb_new;
static volatile bool s_have;
static portMUX_TYPE s_fb_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_fle;          /* LE 工作帧（提交给识别引擎） */

/* ---- 水印离屏画布（会话期临时；LVGL 对象，锁内使用） ---- */
static lv_obj_t *s_wm_canvas;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void report(const char *text)
{
    ESP_LOGI(TAG, "punch result: %s", text);
    if (s_result_cb) {
        s_result_cb(text);
    }
}

/* cam_task 上下文：只 memcpy + 换手（camera_service.h 硬约束） */
static void on_frame(const uint8_t *data, uint32_t size,
                     uint32_t w, uint32_t h, void *user)
{
    (void)w;
    (void)h;
    (void)user;
    if (!s_fb[s_fb_wi] || size != FB_BYTES) {
        return;   /* 缓冲未备好/尺寸不对（档位没对齐）：丢，等下一帧 */
    }
    memcpy(s_fb[s_fb_wi], data, size);
    taskENTER_CRITICAL(&s_fb_mux);
    s_fb_wi ^= 1;
    s_fb_new = true;
    s_have = true;
    taskEXIT_CRITICAL(&s_fb_mux);
}

/* 取最近一帧做 BE→LE 转换（传感器大端 → face_recog_submit 要 rgb565le；
 * 与 tab_camera refresh() 的逐像素换序同源，出处 tab_camera.c:1395 实测注释） */
static void latest_frame_le(void)
{
    taskENTER_CRITICAL(&s_fb_mux);
    int latest = s_fb_wi ^ 1;
    s_fb_new = false;
    taskEXIT_CRITICAL(&s_fb_mux);

    const uint16_t *src = (const uint16_t *)s_fb[latest];
    uint16_t *dst = (uint16_t *)s_fle;
    for (int i = 0; i < PUNCH_W * PUNCH_H; i++) {
        uint16_t v = src[i];
        dst[i] = (uint16_t)((v << 8) | (v >> 8));
    }
}

/* §73.23：把最近一帧推到预览单槽（视频流模式 = 右侧浮窗实时取景）。
 * BE→LE 转到新缓冲，所有权移交槽（UI 取走后 UI free；新帧顶掉旧帧时
 * 由槽释放）。限速 VF_PUBLISH_GAP_MS（~10fps 顺滑且省分配churn）。
 * 不动 s_fb_new（提交节拍用它判新帧，两不干扰）；失败静默（预览是赠品）。 */
static void viewfinder_pump(int64_t *last_pub_us)
{
    bool have;
    int latest;
    taskENTER_CRITICAL(&s_fb_mux);
    have = s_have;
    latest = s_fb_wi ^ 1;
    taskEXIT_CRITICAL(&s_fb_mux);
    if (!have || now_us() - *last_pub_us < VF_PUBLISH_GAP_MS * 1000) {
        return;
    }

    uint8_t *le = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!le) {
        return;
    }
    const uint16_t *src = (const uint16_t *)s_fb[latest];
    uint16_t *dst = (uint16_t *)le;
    for (int i = 0; i < PUNCH_W * PUNCH_H; i++) {
        uint16_t v = src[i];
        dst[i] = (uint16_t)((v << 8) | (v >> 8));
    }
    *last_pub_us = now_us();
    ai_camera_preview_publish_rgb565(le, PUNCH_W, PUNCH_H);
}

/* ---- 水印（LVGL 线程 = bsp_display_lock 临界区内调用） ---- */

static void wm_canvas_ensure(void)
{
    if (s_wm_canvas) {
        return;
    }
    s_wm_canvas = lv_canvas_create(lv_layer_top());
    lv_obj_set_hidden(s_wm_canvas, true);
    lv_obj_set_ignore_layout(s_wm_canvas, true);
    lv_obj_set_pos(s_wm_canvas, -1000, -1000);
}

static void wm_canvas_teardown(void)
{
    if (s_wm_canvas) {
        lv_obj_delete(s_wm_canvas);
        s_wm_canvas = NULL;
    }
}

/* 复刻 tab_camera::watermark_draw（S8-3）：离屏 canvas + 立即模式。
 * 行高用字体 line_height 动态算（bin 字体 28 / cn_20 ~20，写死必压字）。 */
static void wm_draw(uint8_t *rgb, int w, int h, const char *name, const char *timestr)
{
    if (!rgb || w <= 0 || h <= 0 || !s_wm_canvas) {
        ESP_LOGE(TAG, "watermark: bad frame or canvas missing (%p %dx%d)", rgb, w, h);
        return;
    }

    lv_canvas_set_buffer(s_wm_canvas, rgb, w, h, LV_COLOR_FORMAT_RGB888);

    lv_font_t *cjk = ui_binfont_get();
    const lv_font_t *nf = cjk ? cjk : &lv_font_cn_20;
    int lh_name = nf->line_height;
    int lh_time = UI_FONT->line_height;

    int band_h = lh_name + lh_time + 8;
    if (band_h > h / 2) {
        band_h = h / 2;   /* 极端字体下别把整张图糊掉 */
    }
    int band_y = h - band_h;

    lv_layer_t layer;
    lv_canvas_init_layer(s_wm_canvas, &layer);

    /* 半透明黑带：浅色背景上白字可读的前提 */
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color = lv_color_black();
    rdsc.bg_opa   = LV_OPA_50;
    rdsc.radius   = 0;
    lv_area_t band = { 0, band_y, w - 1, h - 1 };
    lv_draw_rect(&layer, &rdsc, &band);

    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color = lv_color_white();
    ldsc.opa   = LV_OPA_COVER;

    if (name && name[0]) {
        ldsc.font = nf;
        ldsc.text = name;
        lv_area_t a = { 6, band_y + 4, w - 7, band_y + 4 + lh_name - 1 };
        lv_draw_label(&layer, &ldsc, &a);
    }
    if (timestr && timestr[0]) {
        ldsc.font = UI_FONT;
        ldsc.text = timestr;
        lv_area_t a = { 6, band_y + 4 + lh_name, w - 7, band_y + 4 + lh_name + lh_time - 1 };
        lv_draw_label(&layer, &ldsc, &a);
    }

    lv_canvas_finish_layer(s_wm_canvas, &layer);
}

/* ---- 会话本体（WS 收包任务上下文；out 带回结论） ---- */

static void punch_session(char *out, size_t out_size)
{
    bool cam_started = false;
    bool claimed = false;
    int sw = 0, sh = 0;
    int64_t deadline_us;
    int64_t last_pub_us = 0;    /* 取景推帧限速（§73.23） */

    /* 0a. 引擎会话独占（§73.21）：堵死 CAM 页 recog_pump 的陈旧帧提交与
     *     结果互抢（实测事故：会话读到"路过时陈旧帧"的 guest 结论）。
     *     本会话所有 recog 调用都在本任务内，与 claim 句柄天然一致。 */
    if (!face_recog_session_claim()) {
        snprintf(out, out_size, "Recognition engine is busy - please try again shortly.");
        goto cleanup;
    }
    claimed = true;

    /* 0a-2. 清空共享结果槽（§73.24）：FILES 页维护操作等留下的陈旧
     * has_result 若不清，等待循环会在真正的 punch 完成前读到它而提前
     * 误判（本会话是槽的**唯一**合法读者：claim 已挡住 CAM 页轮询）。 */
    punch_service_clear_last();

    /* 0b. 缓冲（会话期临时，~348KB PSRAM，结束全还） */
    s_fb[0] = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    s_fb[1] = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    s_fle = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb[0] || !s_fb[1] || !s_fle) {
        snprintf(out, out_size, "Out of memory for punch buffers.");
        goto cleanup;
    }
    s_fb_wi = 0;
    s_fb_new = false;
    s_have = false;

    /* 1. 相机（RGB565 240x240 预览档，owner=PREVIEW；进入前已确认 owner==NONE） */
    if (camera_service_start_fmt(on_frame, NULL, CAM_FMT_RGB565,
                                 PUNCH_W, PUNCH_H) != ESP_OK) {
        snprintf(out, out_size, "Failed to start camera for punch.");
        goto cleanup;
    }
    cam_started = true;

    /* 1b. 实时取景（§73.23）：右侧浮窗进视频流模式，先让用户看到自己。
     *     推帧 ~10fps；取景 VIEWFINDER_MS 后才进入识别加载（用户拍板：
     *     "看着镜头 2 秒再识别"）。 */
    ai_camera_preview_video_set(true);
    deadline_us = now_us() + VIEWFINDER_MS * 1000;
    while (now_us() < deadline_us) {
        viewfinder_pump(&last_pub_us);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    /* 2. 识别引擎按需加载（§73.10 纪律：即用即加载，cleanup 里即卸）。
     *    加载期间取景推帧继续（~1.7s 的加载窗口画面不冻结）。 */
    face_recog_request_load();
    deadline_us = now_us() + LOAD_WAIT_MS * 1000;
    while (!face_recog_ready()) {
        viewfinder_pump(&last_pub_us);
        face_recog_poll();   /* 加载派发 + 看门狗的唯一驱动者（本会话独占） */
        if (face_recog_state() == FACE_FAIL) {
            snprintf(out, out_size,
                     "Face engine failed to load (memory busy?) - close the CAMERA/VIDEO page and retry.");
            goto cleanup;
        }
        if (now_us() > deadline_us) {
            snprintf(out, out_size, "Face engine load timed out - please retry.");
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 2b. 排干认领前可能残留的旧结果（不许让它冒充本会话的第一帧结论） */
    while (face_recog_result() != NULL) {
        face_recog_result_consume();
    }

    /* 3. 扫描窗口（取景推帧贯穿全程，用户全程能看到自己） */
    deadline_us = now_us() + SCAN_WINDOW_MS * 1000;
    int64_t last_submit_us = 0;
    for (;;) {
        viewfinder_pump(&last_pub_us);
        if (now_us() > deadline_us) {
            snprintf(out, out_size,
                     "No face recognized in time - please look at the camera and retry.");
            goto cleanup;
        }
        if (s_fb_new && !face_recog_busy() &&
            now_us() - last_submit_us > SUBMIT_GAP_MS * 1000) {
            latest_frame_le();
            if (face_recog_submit((const uint16_t *)s_fle, PUNCH_W, PUNCH_H,
                                  RECOG_JOB_IDENTIFY) == ESP_OK) {
                last_submit_us = now_us();
            }
        }

        const recog_result_t *r = face_recog_result();
        if (!r) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!r->valid) {
            face_recog_result_consume();
            continue;                     /* 没检出脸：窗口内继续等 */
        }

        /* 质量门槛（与 CAM 页共用 face_quality，判据不漂移） */
        vision_result_t vr = { .x = r->x, .y = r->y, .w = r->w, .h = r->h,
                               .score = r->det_score };
        face_quality_t q;
        face_quality_check(&vr, PUNCH_W, PUNCH_H, &q);
        if (!q.ok) {
            face_recog_result_consume();
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;                     /* 姿态/尺寸不合格：窗口内重试 */
        }

#if !PUNCH_RECORD_GUEST
        if (r->person_id == FACE_DB_GUEST_ID) {
            face_recog_result_consume();
            snprintf(out, out_size, "Face not in the face database - punch not recorded.");
            goto cleanup;
        }
#endif

        /* 4. 命中：LVGL 锁内水印 + punch 两阶段事务（tab_camera 同款流程） */
        char who[FACE_NAME_MAX];
        snprintf(who, sizeof(who), "%s", r->name);
        float score = r->score;
        punch_prep_t prep = PUNCH_PREP_BAD_ARG;
        bool committed = false;

        bsp_display_lock(0);
        wm_canvas_ensure();
        const uint8_t *src = face_recog_frame(&sw, &sh);
        uint8_t *dst = NULL;
        if (src) {
            prep = punch_service_begin(r, src, (uint16_t)sw, (uint16_t)sh, &dst);
        }
        if (prep == PUNCH_PREP_OK && dst) {
            net_time_t nt;
            char ts[24];
            net_time_service_get(&nt);
            net_time_service_format(&nt, ts, sizeof(ts));   /* 未校时是长横线占位，不是 1970 */
            wm_draw(dst, sw, sh, who, ts);
            committed = (punch_service_commit() == ESP_OK);
            if (!committed) {
                punch_service_abort();   /* 已 begin 却 commit 失败：回滚占位（冷却不退，见 tab_camera 同注） */
            }
        }
        wm_canvas_teardown();
        bsp_display_unlock();

        face_recog_result_consume();   /* 结果消费（begin 已拷帧，不再需要 r/src） */

        switch (prep) {
        case PUNCH_PREP_OK:
            if (!committed) {
                snprintf(out, out_size, "Failed to write the punch record - please retry.");
                goto cleanup;
            }
            break;
        case PUNCH_PREP_COOLDOWN:
            snprintf(out, out_size, "Already punched: %s (cooldown %ds).", who, PUNCH_COOLDOWN_S);
            goto cleanup;
        case PUNCH_PREP_BUSY:
            snprintf(out, out_size, "Punch service busy (previous write in flight) - try again shortly.");
            goto cleanup;
        case PUNCH_PREP_NO_SD:
            snprintf(out, out_size, "No SD card - punch not saved.");
            goto cleanup;
        case PUNCH_PREP_NO_DB:
            snprintf(out, out_size, "Face database not ready - punch not saved.");
            goto cleanup;
        case PUNCH_PREP_NO_MEM:
            snprintf(out, out_size, "Low memory - punch not saved.");
            goto cleanup;
        default:
            snprintf(out, out_size, "Punch failed (unexpected state) - please retry.");
            goto cleanup;
        }

        /* 5. 等编码/落盘完成（异步任务 + has_result 显式标志，见 punch_service.h）。
         *    等待期间取景推帧继续（画面不冻结，用户全程看着自己）。 */
        punch_record_t rec;
        memset(&rec, 0, sizeof(rec));
        int64_t wdl = now_us() + PUNCH_DONE_WAIT_MS * 1000;
        while (now_us() < wdl) {
            viewfinder_pump(&last_pub_us);
            punch_service_get_last(&rec);
            if (rec.has_result && !punch_service_busy()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (rec.has_result && !rec.clear_op && rec.err == ESP_OK) {
            /* 6. §73.23：打卡成功 = 关闭右侧实时取景浮窗（cleanup 统一收尾沿）。
             *    水印底片已随 punch_service 落盘存证，屏幕上不再弹静态照片卡。 */
            snprintf(out, out_size, "Punched: %s (match score %.2f). Attendance photo saved.",
                     who, (double)score);
        } else {
            snprintf(out, out_size,
                     "Punch failed - photo or attendance record was not saved. Please retry.");
        }
        goto cleanup;
    }

cleanup:
    /* §73.23：视频流收尾——成功/失败/超时任何路径都关实时取景，UI 检测
     * 收尾沿立即隐藏右侧浮窗并释放显示缓冲。 */
    ai_camera_preview_video_set(false);
    if (claimed) {
        face_recog_session_release();   /* §73.21：所有路径必还（先释放再卸引擎亦可） */
    }
    /* 识别引擎即用即卸（§73.10）：被拒（job/load 任务在跑）就重试，最多 ~5s */
    for (int i = 0; i < 25 && face_recog_state() != FACE_IDLE; i++) {
        face_recog_poll();
        if (face_recog_stop() == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (cam_started) {
        camera_service_stop();
    }
    if (s_wm_canvas) {   /* 理论上到不了（锁内必 teardown）；兜底防泄漏 */
        bsp_display_lock(0);
        wm_canvas_teardown();
        bsp_display_unlock();
    }
    if (s_fb[0]) {
        heap_caps_free(s_fb[0]);
        s_fb[0] = NULL;
    }
    if (s_fb[1]) {
        heap_caps_free(s_fb[1]);
        s_fb[1] = NULL;
    }
    if (s_fle) {
        heap_caps_free(s_fle);
        s_fle = NULL;
    }
}

/* ---- MCP 工具回调（WS 收包任务上下文，阻塞至会话结束——§73.18 契约） ---- */

static esp_mcp_value_t cb_punch(const esp_mcp_property_list_t *props)
{
    (void)props;
    char out[160];

    if (s_busy) {
        return esp_mcp_value_create_string("A punch session is already running, please wait.");
    }
    if (camera_service_owner() != CAM_OWNER_NONE) {
        return esp_mcp_value_create_string(
            "Camera is busy - close the CAMERA/VIDEO page first, then ask me to punch in again.");
    }

    s_busy = true;
    punch_session(out, sizeof(out));
    report(out);   /* 分发之二：XZ_EVT_TEXT 进 AI 浮窗对话记录 */
    s_busy = false;
    /* 分发之一：tools/call response（服务端模型转述给用户，§73.18 教训：
     * 必须回真实结论，绝不回占位符）。之三（§73.19 的照片卡）已由 §73.23
     * 的实时取景取代：打卡全程右侧浮窗直播画面，结束即关。 */
    return esp_mcp_value_create_string(out);
}

/* ---- 注册 ---- */

esp_err_t ai_punch_attach(esp_mcp_t *mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "null mcp");

    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "self.punch.clock_in",
        "启动本机人脸识别打卡。当用户说'上班了'、'打卡'、'人脸识别'、'签到'等"
        "想要考勤打卡的话时调用。流程：启动摄像头并把实时画面推到屏幕右侧浮窗"
        "（用户能看到自己）→ 2 秒后启动人脸识别 → 识别到人脸库成员则拍照存证"
        "并记录打卡流水，随后关闭浮窗（耗时数秒到约 30 秒）。无参数。",
        cb_punch);
    ESP_RETURN_ON_FALSE(tool, ESP_FAIL, TAG, "tool create");
    ESP_RETURN_ON_FALSE(esp_mcp_add_tool(mcp, tool) == ESP_OK,
                        ESP_FAIL, TAG, "add tool");
    ESP_LOGI(TAG, "mcp tool: self.punch.clock_in");
    return ESP_OK;
}

void ai_punch_set_result_cb(ai_punch_result_cb_t cb)
{
    s_result_cb = cb;
}
