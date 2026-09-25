/* mem_diag.h - 统一的堆诊断宏
 *
 * 目的（对应 docs/开发文档/内存优化可执行方案.md §4-P0）：
 *   1) P0-1 分步账单：每个初始化步骤前后各打一行，diff 就是这一步的净消耗；
 *   2) P0-2/P0-4 失败必说话：任何"资源拿不到"的失败分支都必须打**三元组**。
 *
 * ★ 判读口径（唯一）：只看 largest_free_block，不看 free_size。
 *   "申请量 / free / largest" 三元组才能区分三种假性内存不足：
 *     - free 大、largest 大 ⇒ 真够，失败原因在别处
 *     - free 大、largest 小 ⇒ 碎片化（本工程 NO_MEM 的典型形态）
 *     - free 小                ⇒ 真不够
 *
 * 纯宏、无 .c ⇒ 不需要登记 main/CMakeLists.txt。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>          /* abort() */
#include "esp_heap_caps.h"
#include "esp_log.h"

/* 三元组 + DMA + PSRAM */
#define MEM_DIAG_FMT  "want=%u B | INT free=%u largest=%u | DMA free=%u | PSRAM free=%u"
#define MEM_DIAG_ARGS(want)                                                    \
    (unsigned)(want),                                                          \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),                    \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),           \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),                         \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)

/* ---- P0-1：分步账单 ----
 * 用法：在每一步初始化调用前后各放一行，日志里相邻两行的差值 = 该步净消耗。
 *       收尾用 MEM_STAGE_SNAP 打一次总账。
 * ★ 同时打 largest —— 这是判断"还能不能拿到连续块"的关键。 */
#define MEM_STAGE(tag, stage)                                                  \
    ESP_LOGI(tag, "[mem] %-32s " MEM_DIAG_FMT, (stage), MEM_DIAG_ARGS(0))

/* ---- P0-2 / P0-4：失败必说话 ----
 * ★ 为什么要替掉裸 assert()：assert 在 NDEBUG 下会被编译掉 ⇒ 失败变成**静默**；
 *   而且 abort 现场只会看到 "assert failed"，没有任何内存证据。
 *   本宏永不被裁掉，且先落一条带三元组的日志再 abort（保留初始化期 fail-fast 语义，
 *   符合规范 A1"初始化期可断言 / 运行期禁断言"）。 */
#define MEM_FAIL_FAST(cond, tag, what, want)                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ESP_LOGE(tag, "%s FAILED [" MEM_DIAG_FMT "]", (what),              \
                     MEM_DIAG_ARGS(want));                                     \
            abort();                                                           \
        }                                                                      \
    } while (0)

/* 运行期（非初始化）路径上若只降级不 abort，用这两个：打三元组后继续。 */
#define MEM_ERR_DIAG(tag, what, want)                                          \
    ESP_LOGE(tag, "%s [" MEM_DIAG_FMT "]", (what), MEM_DIAG_ARGS(want))

#define MEM_WARN_DIAG(tag, what, want)                                         \
    ESP_LOGW(tag, "%s [" MEM_DIAG_FMT "]", (what), MEM_DIAG_ARGS(want))
