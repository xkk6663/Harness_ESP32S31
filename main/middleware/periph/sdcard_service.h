/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdcard_service.h
 * @brief 中间层：SD 卡挂载服务（SDMMC 4-bit → FATFS）
 *
 * 方案：docs/项目规划/人脸录用系统与SD卡媒体实施方案.md §3（S1）
 *
 * ★ 挂载点分工（红线，勿改）：
 *     /littlefs  ← 内部 flash 的 LittleFS，放**系统资源**（图标 / bin 字体 / 人脸检测模型）
 *     /sdcard    ← SD 卡的 FATFS，放**用户数据**（照片 / 录像 / 人脸特征库）
 *   两套文件系统并存：检测模型必须留在 littlefs，保证"没插卡也能开机可用"。
 *   ⚠ 本分支曾把 `CONFIG_BSP_SD_MOUNT_POINT` 误设为 `/littlefs`（与 LittleFS 撞车），
 *     已在 sdkconfig / sdkconfig.defaults 修正为 `/sdcard`（见方案 §3.1 坑③）。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SD 卡挂载点（与 fs_service.h 的 FS_MNT_PATH 严格区分） */
#define SDCARD_MNT_PATH  "/sdcard"

/**
 * @brief 挂载 SD 卡到 SDCARD_MNT_PATH
 *
 * ★ 语义（与既有中间层服务一致，**必须遵守**）：
 *   - **失败不阻塞启动**：没插卡时只打一条 WARN 并返回错误码，调用方忽略即可
 *     （不要用 ESP_ERROR_CHECK 包它 —— 那会让"没插卡"变成开不了机）。
 *   - **绝不格式化**：`format_if_mount_failed = false`，SD 卡上是用户数据。
 *   - 幂等：已挂载时直接返回 ESP_OK。
 *   - 运行期**不支持热插拔**：拔出后需重启（一期不做 unregister/remount）。
 *
 * @return ESP_OK 已挂载；其他为 esp_vfs_fat_sdmmc_mount 的原始错误码
 */
esp_err_t sdcard_service_mount(void);

/**
 * @brief 卸载 SD 卡（§73.20 有序软重启专用；**平时不要调**）
 *
 * ★ 做两件事：f_unmount（FAT 脏缓冲回写 + 关卷）+ VFS 注销。
 *   esp_restart 前必须走这一步——软重启落在写盘事务中间 = 文件系统脏，
 *   这是"软重启比断电更危险"的最大来源。
 * ★ 失败不拦（调用方马上就重启了）；未挂载时直接返回（幂等）。
 * ★ 调用后 SD 相关 API（照片/录像/人脸库）不可用——只在重启前调用。
 * ★ 任意任务可调；与其他任务并发 SD 访问由调用方保证（重启前的收尾阶段）。
 */
void sdcard_service_unmount(void);

/** SD 卡当前是否已挂载（供 UI 置灰/提示用，任意线程可调） */
bool sdcard_service_is_mounted(void);

#ifdef __cplusplus
}
#endif
