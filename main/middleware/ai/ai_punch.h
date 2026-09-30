/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ai_punch.h
 * @brief 小智 MCP 打卡工具：语音"打卡/上班了" → 本机人脸识别打卡事务
 *        （S8-5 / §73.19）。
 *
 * 链路：LLM 识别到用户想打卡（"上班了"/"打卡"/"人脸识别"/"签到"等意图，
 *   意图判断由服务端大模型完成，本工具描述里已写明触发词）→ 调
 *   self.punch.clock_in → 本模块接管相机（RGB565 240x240 预览档）→
 *   按需加载识别引擎（MSR+MNP+MFN，用完即卸，§73.10 内存纪律）→
 *   扫描窗口内识别人脸 → 命中且质量合格 → bsp_display_lock 短临界区内
 *   复用 CAM 页同款水印绘制 + punch_service 两阶段事务落盘 → 结果三路分发：
 *   ① tools/call response（服务端模型据此转述"打卡成功/失败"——§73.18
 *      教训：工具结果必须是真实结论，不能回占位符）；
 *   ② 结果回调 → XZ_EVT_TEXT(role=assistant) → AI 浮窗对话记录；
 *   ③ 打卡照片（已叠水印）→ ai_camera 预览单槽 → AI 浮窗照片卡显示 3s。
 *
 * 线程模型（§73.18 同款官方契约）：
 *   - MCP 工具回调（WS 收包任务上下文）**阻塞至会话结束**（最坏 ~30s：
 *     引擎加载 10s + 扫描 15s + 落盘 6s）——与官方 xiaozhi-esp32 固件的
 *     同步工具契约一致；音频走 UDP 不受影响。
 *   - 识别推理 / JPEG 编码 / 落盘各在 face_recog / punch_service 自己的
 *     一次性任务里；本回调只做编排与等待。
 *   - LVGL 操作（水印离屏画布）一律 bsp_display_lock 短临界区（工程惯例，
 *     见 main.c / page_manager.c）。
 *   - 帧回调在 cam_task 上下文：只 memcpy + 置标志（camera_service.h 硬约束）。
 *
 * 与既有页面的和平共处：
 *   - 相机被占（CAM/VIDEO 页在跑、explain 会话进行中）⇒ owner != NONE，
 *     工具直接回 "Camera busy"，不抢断；
 *   - 会话期间用户打开 CAM 页按 START 会失败（上一任务未退）——属已知瞬态，
 *     提示后重开即可；会话最长 ~30s。
 *   - 识别引擎的加载/释放是本会话**独占管理**的（与打卡/录入会话同责，
 *     见 face_service.h 的职责边界注释）；CAM 页若同时在跑 poll 只会有
 *     良性竞态（load 单飞标志去重）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 结果回调（会话任务上下文调用，勿阻塞勿碰 LVGL）；text 为 UTF-8 结论/失败原因 */
typedef void (*ai_punch_result_cb_t)(const char *text);

/**
 * @brief 注册 MCP 打卡工具（chat_setup 里与 ai_camera/ai_device 并列挂载）
 */
esp_err_t ai_punch_attach(esp_mcp_t *mcp);

/**
 * @brief 注册结果回调（xiaozhi_service_init 里注册；未注册时结果只打 LOG）
 */
void ai_punch_set_result_cb(ai_punch_result_cb_t cb);

#ifdef __cplusplus
}
#endif
