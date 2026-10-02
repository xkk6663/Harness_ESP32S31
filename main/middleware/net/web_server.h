/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file web_server.h
 * @brief 中间层：网页管理端（§74）—— APSTA 热点/局域网内浏览器直接管理设备
 *
 * 能力（全部只读，无上传/下载落盘通道）：
 *   1. GET /                    内嵌单页 HTML（手机/PC 通用，响应式）
 *   2. GET /api/punch/last      打卡事件轮询（代际号；esp_http_server 单任务
 *                               串行模型不支持阻塞式 SSE，故前端 1s 一拍）
 *   3. GET /api/attendance/list 打卡流水 JSON（读 punch_service 现役 CSV 解析）
 *   4. GET /api/media/list      SD 卡目录 JSON（白名单 /sdcard/ 之下）
 *   5. GET /api/media/file      照片/文件内联浏览（浏览器直接显示）
 *   6. GET /api/media/info      AVI(MJPG) 元信息 JSON（现役 avi_demux）
 *   7. GET /api/media/frame     AVI 单帧 JPEG（JS 逐帧轮询播放，全平台通用）
 *
 * 安全边界：
 *   - 仅局域网/AP 热点（无公网暴露），AP 侧已有 WPA2 密码；
 *   - 文件路径强校验：必须以 "/sdcard/" 开头且不含 ".."（防路径穿越）；
 *   - 只读打开，绝不写 SD —— 与 punch/capture 的"只动自己领地"红线同款纪律。
 *
 * 线程模型：
 *   - httpd 内部任务串行处理请求（stack 8 KB，handler 必须有界返回）；
 *   - 打卡事件：punch 完成回调只更新"代际号 + 快照"槽（互斥锁保护），
 *     前端 1 s 轮询比对 gen —— 多客户端（手机 + PC）天然支持，无连接占用。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动网页管理端（幂等）：注册 punch 完成回调 + 起 httpd:80。
 *
 * ★ 不挑时机：httpd 绑 0.0.0.0，AP/STA 任一接口拿到 IP 后即可访问；
 *   建议在各 service_init 之后调用（订阅 punch 回调需 punch_service_init 已跑）。
 * @return ESP_OK / 启动失败的 esp_err_t（不打断系统启动，只打 ERROR 日志）
 */
esp_err_t web_server_init(void);

#ifdef __cplusplus
}
#endif
