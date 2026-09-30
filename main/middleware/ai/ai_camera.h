/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_camera.h
 * @brief 小智 MCP 相机工具：拍一张照走云端视觉问答（S7-A1，见 docs/开发文档/
 *        S7_小智待发掘能力考古.md；改动记录 §58）。
 *
 * 链路（§73.18 同步桥改造后）：LLM 调 MCP 工具 self.camera.explain →
 *   本模块建独立 explain 任务 → camera_service 抓一帧 JPEG（1280x720，
 *   CAM_OWNER_MEDIA 独占事务）→ esp_xiaozhi_camera_explain() 同步上传
 *   explain 服务 → 结果双路分发：
 *   ① 同步写回 tools/call response（服务端大模型"拿到"识别文本——
 *      §73.18 根因修复点：旧版返回占位符，模型永远拿不到结果）；
 *   ② ai_camera_set_result_cb() 注册的回调上抛（xiaozhi_service 转成
 *      XZ_EVT_TEXT(role=assistant) 进 AI 浮窗对话记录）。
 *
 * 关键设计（三条硬约束的出处）：
 *  - MCP 工具回调运行在 WS 收包任务上下文 ⇒ **重活（相机+TLS/HTTP）仍
 *    在独立 explain 任务（PSRAM 栈）里做**，回调本身只阻塞在信号量上
 *    等结果（§73.18 评估：整段同步进回调会有 WS 任务栈溢出风险；而
 *    "工具回调数秒级阻塞"本就是官方 xiaozhi-esp32 固件的既定契约，
 *    服务端按异步工具等待 tools/call 响应）；
 *  - explain 任务用 xTaskCreateWithCaps 外置 PSRAM 栈（F35：纯网络任务
 *    可外置；任务自删 vTaskDeleteWithCaps）——内部 RAM 刚性底盘不留常驻；
 *  - 相机事务：起流前查 camera_service_owner()，被 PREVIEW/MEDIA 占用时
 *    直接报"相机忙"（不抢断，与 CAM/VIDEO 页和平共处）。
 *
 * 依赖：vision URL/token 由 vendor 从 MCP initialize 响应 peek 缓存
 * （esp_xiaozhi_chat_get_vision_endpoint），经 xiaozhi_service_get_vision_endpoint()
 * 透出。会话未建立时工具返回提示语（LLM 会转告用户）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 结果回调（explain 任务上下文调用，勿阻塞勿碰 LVGL）；text 为 UTF-8 回答/错误提示。
 *  §73.18 起该回调只服务 UI 浮窗记录；服务端模型的结果走 tools/call response。 */
typedef void (*ai_camera_result_cb_t)(const char *text);

/**
 * @brief 创建 explain 客户端句柄（幂等；重复调用直接返回 ESP_OK）
 */
esp_err_t ai_camera_init(void);

/**
 * @brief 注册 MCP 相机工具（chat_setup 里与 ai_light/ai_speaker 并列挂载）
 */
esp_err_t ai_camera_attach(esp_mcp_t *mcp);

/**
 * @brief 注册结果回调（xiaozhi_service_init 里注册；未注册时结果只打 LOG）
 */
void ai_camera_set_result_cb(ai_camera_result_cb_t cb);

/**
 * @brief 取走最新一张拍照预览缩略图（§59b，overlay_ai 50ms 轮询调用）。
 *
 * 两个 MCP 摄像头工具（explain / capture）各拍一帧后，本模块解码出
 * RGB565 小图放进单槽；UI 轮询命中即**移交所有权**（返回 true 后 *buf
 * 由 UI free，展示 3s 后释放）。未命中返回 false。
 * 线程安全（临界区保护单槽）；可在 LVGL 任务调用。
 */
bool ai_camera_preview_take(uint8_t **buf, int *w, int *h);

/**
 * @brief 直接发布一帧 RGB565 预览进单槽（§73.19：打卡照片显示用）。
 *
 * 与 take 对偶的"生产者"入口：buf 为 RGB565（LVGL 原生小端）w*h*2 字节，
 * **所有权移交单槽**（UI 取走后由 UI 释放；被新帧顶掉时由本模块释放）。
 * buf 为 NULL 或尺寸非法时释放 buf（容错）。
 * 线程安全（临界区保护单槽）；任意任务可调。
 */
void ai_camera_preview_publish_rgb565(uint8_t *buf, int w, int h);

/**
 * @brief 预览单槽"视频流模式"开关（§73.23：打卡实时取景）。
 *
 * on=true：UI 轮询到新帧就连续换帧显示（实时视频），**不启动** 3s 隐藏
 * 定时器，并清掉残留的静态卡定时器——调用方必须在会话结束时置 false，
 * UI 检测到收尾沿会立即隐藏浮窗并释放显示缓冲。
 * on=false：恢复静态卡语义（一帧 + 3s 自动消失），不影响 explain/capture。
 * 默认 false。任意任务可调；开关本身不带帧，帧照常走 publish/take 单槽。
 */
void ai_camera_preview_video_set(bool on);

/** 读当前视频流模式状态（overlay_ai 轮询用，检测收尾沿）。 */
bool ai_camera_preview_video_get(void);

#ifdef __cplusplus
}
#endif
