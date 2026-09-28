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
 * 链路：LLM 调 MCP 工具 self.camera.explain → 本模块起独立任务 →
 *   camera_service 抓一帧 JPEG（1280x720，CAM_OWNER_MEDIA 独占事务）→
 *   esp_xiaozhi_camera_explain() 同步上传 explain 服务 → 结果经
 *   ai_camera_set_result_cb() 注册的回调上抛（xiaozhi_service 转成
 *   XZ_EVT_TEXT(role=assistant) 进 AI 浮窗对话记录）。
 *
 * 关键设计（三条硬约束的出处）：
 *  - MCP 工具回调运行在协议栈任务上下文 ⇒ **只置标志+建任务立即返回**，
 *    绝不阻塞（explain 是同步 HTTP，数秒级；F44 纪律）；
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

/** 结果回调（explain 任务上下文调用，勿阻塞勿碰 LVGL）；text 为 UTF-8 回答/错误提示 */
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

#ifdef __cplusplus
}
#endif
