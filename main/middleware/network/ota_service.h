/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file ota_service.h @brief WiFi 局域网 OTA 升级服务（设备端）。
 *
 *  方案见 docs/项目规划/WiFi局域网OTA升级实施方案.md §3.1；
 *  协议见 docs/项目规划/通信接口规范_ICD.md §8（V1.4）。
 *
 *  ── 架构（两张面）────────────────────────────────────────────────
 *  控制面：复用 link_manager 的 UDP 50003 JSON → OTA_OFFER/ACCEPT/REJECT/
 *          PROGRESS/RESULT/ABORT（**零新增端口**）。
 *  数据面：设备作为 **HTTP 客户端**从 PC 的 :50005 流式拉 /firmware.bin
 *          （与副屏封面/歌词拉取同构：设备主动 GET、PC 是服务端）。
 *
 *  ── 状态机 ───────────────────────────────────────────────────────
 *  ```
 *  IDLE → (OFFER 校验通过) OFFERED → DOWNLOADING → VERIFY
 *       → REBOOT_PENDING → [esp_restart] → PENDING_VERIFY
 *       → (30s 自证: WiFi 已连) → VALID
 *       （不自证 ⇒ bootloader 下次启动自动回滚旧槽）
 *  ```
 *
 *  ── M2 能力（本次新增）──────────────────────────────────────────
 *  ① **OTA_ABORT 立即生效**：下载循环每个块边界检查 `s_abort`（原来的
 *     `handle_abort()` 只打日志，形同虚设）。
 *  ② **HTTP Range 断点续传 + 分段哈希校验**：把固件切成 256KB 的"锚点段"，
 *     每段单独算 SHA256 存 nvs；断电重启后从最近锚点续拉，
 *     只需重算该锚点之后的段（≈ 最多 256KB），最终再整包复核。
 *
 *  ── 内存纪律（严格遵循 docs/工程规范/13_内存管理规范.md + 14_堆管理宝贵经验.md）─
 *  - **A 静态池**：4KB 读缓冲 `s_rd_buf` 走 `.bss`（不进堆、零碎片）。
 *  - **B 预分配常驻**：OFFER JSON 拷贝走 PSRAM 堆（一次性、任务内 free）。
 *  - **C 换区域**：任务栈 `WithCaps(MALLOC_CAP_SPIRAM|8BIT)`（F35 统一姿势），
 *    失败分支带 T5 日志（点名 CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY）。
 *  - **A4 三元组**：所有 alloc 失败路径打印 (want / free / largest)。
 *  - **A2 运行期降级**：下载/校验全部失败走 `goto out` + 回 IDLE，
 *    **绝不 assert**（OTA 是"用户可见功能"，崩了等于变砖，见 A3）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_EVT_OFFERED = 0,   /* 收到 OFFER 且校验通过，即将开始拉取 */
    OTA_EVT_DOWNLOADING,   /* 下载中（percent 有效） */
    OTA_EVT_VERIFYING,     /* 下载完成，正在做镜像/SHA256 校验 */
    OTA_EVT_DONE,          /* 校验通过、已切槽，即将重启 */
    OTA_EVT_FAILED,        /* 任一环节失败 */
    OTA_EVT_REJECTED,      /* OFFER 被拒（detail = reason） */
    OTA_EVT_ROLLBACK_ARMED,/* 本固件处于 PENDING_VERIFY（自证窗口内） */
    OTA_EVT_ABORTED,       /* 被 OTA_ABORT 主动中止 */
} ota_evt_t;

typedef struct {
    ota_evt_t event;
    uint8_t   percent;
    char      detail[64];
} ota_event_t;

typedef void (*ota_evt_cb_t)(const ota_event_t *ev);

/** @brief 初始化：注册 link_manager 的 JSON 回调槽 + 建下载任务。
 *         main.c 在 pcnow_service_init() 之后调用（依赖 link_manager 已 init）。 */
void ota_service_init(void);

/** @brief 是否正在升级（OFFER 校验互斥用；也供 UI 判断）。 */
bool ota_service_is_active(void);

/** @brief 当前进度 0-100（UI 浮层用；未在升级时返回 0）。 */
uint8_t ota_service_get_percent(void);

/** @brief 订阅升级事件（UI/自证用）。
 *  ★ 回调在 **ota_task 上下文**，实现方只允许置 volatile 标志，
 *    **禁止碰 LVGL**（规范 §4：非 LVGL 线程不得动 UI）。 */
void ota_service_set_evt_cb(ota_evt_cb_t cb);

/** @brief 自证：若运行槽处于 PENDING_VERIFY 且 WiFi 已连 + link 在线则 mark_valid。
 *         由 main.c 的 30s 一次性定时器调用（方案 §3.1-6）。
 *  @return true = 本次真的执行了 mark_valid 且成功。 */
bool ota_service_mark_valid_if_pending(void);

/** @brief 本固件启动时是否处于 PENDING_VERIFY（自证窗口内）。UI 可据此提示。 */
bool ota_service_is_pending_verify(void);

/** @brief 清掉 nvs 里的断点（调试用；正常流程由成功升级/切槽自动清）。 */
void ota_service_clear_resume(void);

/** @brief 取本固件版本号（esp_app_desc_t.version）。UI 浮层显示 "from" 用。
 *  @param out 输出缓冲，@param len 缓冲长度。无描述符时填 "unknown"。 */
void ota_service_get_running_version(char *out, size_t len);

/** @brief 取下一次升级的 OFFER 版本（最近一次收到的 ver 字段）。
 *         未收到过 OFFER / 已结束则填 "-"。UI 浮层显示 "to" 用。 */
void ota_service_get_offer_version(char *out, size_t len);

#ifdef __cplusplus
}
#endif
