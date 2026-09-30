/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file safe_reboot.h
 * @brief 有序软重启（§73.20）—— esp_restart 前的统一收尾入口。
 *
 * 背景：ai_device 的 self.device.reboot 工具与 xiaozhi_service 的
 * SYSTEM_CMD "reboot" 原本都是"延迟几百 ms 后裸 esp_restart"（§58 取舍：
 * "不清理，NVS 无在写操作，风险可接受"）。实机复现问题：软重启后黑屏、
 * 无法完成初始化，只有硬件复位能救——"风险可接受"的假设被打脸。
 *
 * 裸 esp_restart 的真实风险（按危害排序）：
 *   1. **SD/FATFS 写盘中断**：重启落在 FAT 写事务中间 = 卷脏
 *      （index.csv 半行 / face_db 索引残缺 / 下次挂载慢）——最大确定性风险；
 *   2. 相机 DVP DMA 还在跑（若 VIDEO/CAM 页活着）；
 *   3. Wi-Fi 射频没干净下电；
 *   4. RGB 屏停留在 display-on 态（本板 **无 LCD_RST、无背光 GPIO、无命令
 *      通道**——BSP_LCD_RST/BACKLIGHT 都是 NC，DISP(GPIO38) 是唯一抓手）。
 *
 * 线程约定：**任务上下文调用**（工具回调起的延迟任务 / 事件任务），本函数
 * 内部阻塞 ~0.5s（相机停流 + SD flush），调用方自己留好"response 发送"
 * 的宽限时间（ai_device 沿用 500ms 原案）。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 执行有序软重启（**不返回**）。
 *
 * 步骤（每步尽力而为，绝不卡死）：
 *   ① 停相机流（DVP DMA 干净停，未跑则跳过）；
 *   ② 卸载 SD 卡（FATFS flush + VFS 注销，未挂载则跳过）；
 *   ③ esp_wifi_stop（未启动返回错误码，忽略）；
 *   ④ RGB 屏 DISP 拉熄屏电平（driver 的 off 极性：disp_en_level 默认 0
 *      ⇒ 熄屏 = 拉高，让屏 IC 尽量回到接近冷启动的状态）；
 *   ⑤ vTaskDelay(100ms) 后 esp_restart()。
 *
 * @param reason 一句人话原因（进日志；可 NULL）
 */
void safe_reboot_perform(const char *reason);

#ifdef __cplusplus
}
#endif
