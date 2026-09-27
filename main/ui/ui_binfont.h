/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_binfont.h
 * @brief 全 CJK 预渲染 bin 字体：LittleFS 读取 → PSRAM → LVGL 运行时字体。
 *
 * ── 为什么不用编译期内置字体（本项目已有 lv_font_cn_20/30/45）──
 * 内置那几套是**按固定字表**生成的（只覆盖界面上的死字），字号也只有几档。
 * 但 SINK 页显示的曲名/艺术家来自对端 AVRCP，字符集**完全不可预测** ——
 * 今天推「说好不哭」，明天推「両方の気持ち」，内置字表必然出方块。
 * 所以这里复现参考仓库的做法：把 **0x20-0x7E + 0x3000-0x303F +
 * 0x4E00-0x9FFF + 0xFF00-0xFFEF**（ASCII / CJK 标点 / 全部 CJK 基本区 /
 * 全角字符）共约 2.14 万字形预渲染成一份 **1bpp bin 字体**。
 *
 * ── 为什么放 LittleFS 而不是编译进来 ──────────────────────────────
 * 同样的字表编译成 .c 是 ~12.7MB 的源文件、进 rodata 会撑爆 flash 并拉长
 * 每次构建；做成 bin 只有 ~1.78MB，且**改动字表不必重编 app**
 * （只重烧 storage 分区即可）。
 *
 * ── 为什么整体读进 PSRAM（而不是 lazy 按需读文件）───────────────
 * lazy 需要在渲染路径上同步读 flash，中文标签一多就会把 LVGL 任务卡住；
 * 而 `lv_binfont_create_from_buffer()`（dynamic_glyph_load=false）是
 * **一次性**把全部字形位图解析进堆里，之后渲染零 IO。
 * 本板 PSRAM 空闲 ~11.5MB，本字体常驻 ~1.9MB，完全负担得起。
 *
 * ── 使用禁区：本字体**没有 fallback**，别拿去套"图标+英文"的标签 ──────
 * `lv_binfont_create_ex()` 里的 lv_font_t 是 lv_zalloc 出来的，`fallback == NULL`，
 * 而本 bin 的字形区间**不含 FontAwesome 私用区 0xF000-0xF8FF**。所以：
 *   ✅ 适合套：来自外部的动态文本（歌名/歌手/专辑/流派…），可能是中文。
 *   ❌ 不要套：写死的英文 + LV_SYMBOL_* 的标签 ——
 *        (1) 图标（0xF293 蓝牙、0xF04B 播放…）会变方块；
 *        (2) 字号是 28px，会把原本 14px 的英文撑大，整行高度跟着变。
 * 需要"中英混排且带图标"的标签，正确做法是给字体手工挂 fallback
 * （`font->fallback = UI_FONT`），但那样图标与正文会不同号，观感更差 ——
 * 所以本项目的取舍是：**图标/英文标签一律保持 UI_FONT，CJK 只给动态文本**。
 *
 * ── 依赖（改 Kconfig 前务必看这里）────────────────────────────────
 * 需要 `CONFIG_LV_USE_FS_MEMFS=y` + 盘符 letter：`lv_binfont_create_from_buffer`
 * 是把内存缓冲伪装成一个 memfs 文件喂给 `lv_fs_open`，**不是** FS_POSIX。
 * 见 sdkconfig.defaults 里 "btsink Phase 5" 那一段。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 阶段 1／2：把 bin 文件从 LittleFS 读进 PSRAM（**不碰 LVGL**）。
 *
 * ★★ 调用位置有硬约束：必须在**任何 PSRAM 栈任务创建之前**（F40）。
 *   本函数做一次 ~2.7MB 的 LittleFS 读；`esp_flash_read()` 会按 16KB 分块
 *   （`MAX_READ_CHUNK`），每块一对 `spiflash_start_read/end`
 *   ＝ 关一次全局 cache。而 IDF 的
 *   `spi_flash_disable_interrupts_caches_and_other_cpu()`（cache_utils.c）
 *   **只关 cache、并不停住另一核**，所以这些窗口内任何核上的任何 PSRAM
 *   访问（包括别人任务栈的压栈/弹栈）都会直接 Cache error（F38 家族）。
 *   ⇒ 放在 app_main 后续任务创建之前（本分支：fs_service_mount 之后、
 *     bsp_display_start 之前，此刻系统里还没有任何任务），
 *     此刻系统里只有内部栈任务，天然零风险。
 *
 * @note 与 `ui_binfont_attach()` 的分工：本函数只做 POSIX
 *       stat/malloc/open/read，**不依赖 lv_init()**，因此可以提前调用；
 *       `lv_binfont_create_ex()` 那段（需要 LVGL）留给 attach。
 *       幂等：重复调用只跑一次。
 */
void ui_binfont_load(void);

/**
 * @brief 阶段 2／2：把已读入的缓冲解析成 LVGL 运行时字体（纯 RAM，无 flash IO）。
 *
 * @note 必须在 `bsp_display_start()`（内部 `lv_init()`）之后调用。
 *       幂等；若阶段 1 未成功（或文件缺失）则什么都不做，
 *       `ui_binfont_get()` 保持返回 NULL，调用方照常回退内置字体。
 */
void ui_binfont_attach(void);

/**
 * @brief 便捷包装：等价于 `ui_binfont_load()` + `ui_binfont_attach()`。
 *
 * @note 因为 attach 需要 lv_init()，本包装只能在 `bsp_display_start()` 之后调用
 *       ⇒ 它**无法**满足 F40 的"读必须早于 PSRAM 栈任务"约束。
 *       新代码请显式分别调用两个阶段（见 main.c app_main 的 F40 注释）。
 */
void ui_binfont_init(void);

/**
 * @brief 取字体指针。
 * @return 已加载的 lv_font_t；未就绪/失败返回 NULL。
 *         ★ 调用方拿到 NULL **必须**回退到编译期内置字体
 *         （如 UI_FONT / lv_font_cn_20），否则中文会变方块。
 */
lv_font_t *ui_binfont_get(void);

/** @brief 释放字体与文件缓冲（正常流程不需要调用；仅回收/调试用）。 */
void ui_binfont_deinit(void);

#ifdef __cplusplus
}
#endif
