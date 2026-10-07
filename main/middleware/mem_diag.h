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
#include <stdbool.h>
#include <stdlib.h>          /* abort() */
#include "esp_heap_caps.h"
#include "esp_log.h"

/* 三元组 + DMA + PSRAM
 *
 * ★★ 2026-09-28 补 `PSRAM largest`：本文件上面刚写下"只看 largest_free_block，
 *   不看 free_size"，但**PSRAM 那一栏只打了 free** —— 判读口径与埋点自相矛盾。
 *   代价已经出现过两次：
 *     ① §61 POSE 加载失败（`Failed to alloc 3044.66KB, largest 1280.00KB`）——
 *        靠 esp-dl 自己报的 largest 才定位，本工程的 [mem] 行给不出这个数；
 *     ② S8 媒体档（拍照/录像 720p JPEG）`dvp_cam: no mem for DVP backup buffer`
 *        —— 日志里 PSRAM free 明明还有几 MB，**但 largest 是多少完全没有记录**
 *        ⇒ 只能靠"caps 被 IDF 归一化过"这类间接推理，无法当场判定碎片还是总量。
 *   ⇒ 加一栏，四个数字一起出现，上面三种假性内存不足才能一次分开。
 *   （INT 已有 largest / free 两栏，PSRAM 补齐后两边口径一致。） */
#define MEM_DIAG_FMT  "want=%u B | INT free=%u largest=%u | DMA free=%u | PSRAM free=%u largest=%u"
#define MEM_DIAG_ARGS(want)                                                    \
    (unsigned)(want),                                                          \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),                    \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),           \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),                         \
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),                      \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)

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

/* ---- 任意时点的"现场快照" ----
 *
 * ★ 与 MEM_STAGE 的分工（别混用）：
 *     MEM_STAGE      = **初始化分步账单** —— 每步前后各一条，相邻两条 diff = 该步净消耗；
 *     mem_diag_snap  = **任意时点的现场** —— 带调用方自定义的 where 标签，回答
 *                      "这一次动作开始前，heap 长什么样"。
 *   为什么非得有它：分步账单只在开机跑一遍，而"偶发失败"发生在**运行期**（§67 那次
 *   拍照偶发失败就是典型）—— 开机那几行数字代表不了起流那一刻的 heap。
 *
 * ★ extra 用来挂"当时谁持有大块"（引擎状态 / 是否 ready / owner）。
 *   只报数字不报持有者，遇到"每次都是同一个模块在抢"就永远说不清 ——
 *   这正是 §67 排查里最缺的一栏。
 *
 * ★ static inline 放头里（而不是新开一个 .c）：
 *   ① 本头既有 C 也有 C++（.cpp）include，inline 两边都能直接用；
 *   ② 不新增源文件 ⇒ 不必改 main/CMakeLists.txt 的 SRCS；
 *   ③ 未被使用的 static inline 不触发 -Wunused-function（GCC 对此豁免）
 *      ⇒ 已 include 本头的 11 个文件零风险、零改动。
 */
static inline void mem_diag_snap(const char *tag, const char *where, const char *extra)
{
    ESP_LOGI(tag, "[%s] INT free=%u largest=%u | PSRAM free=%u largest=%u | %s",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (extra != NULL) ? extra : "-");
}

/* ---------------------------------------------------------------------------
 *  ★★★ 碎片分布快照（2026-09-28 新增，**必须用它替代 heap_caps_dump**）
 *
 * 【为什么不能用 heap_caps_dump()】
 *   它的实现是（IDF v6.1 components/heap/multi_heap.c:370-378）：
 *       multi_heap_dump() → multi_heap_internal_lock()      ← **持堆锁**
 *                         → tlsf_walk_pool(逐块 STDERR 打印)
 *                         → multi_heap_internal_unlock()
 *   即"**持堆锁 + 逐块 UART 打印**"。本工程实测一次要打 1463 行 ⇒
 *   115200 波特下数秒，全程持堆锁 ⇒ **直接引发
 *   `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`**
 *   （2026-09-28 上机实锤：日志正在打印 `Block 0x50424684 data, size: 108` 就崩了）。
 *   ⇒ 凡是"想看清碎片形态"的地方，一律用本函数，**不要**再调 heap_caps_dump()。
 *
 * 【正确做法：锁内只收集、锁外才打印】
 *   heap_caps_walk() 的**遍历**是微秒级（快），慢的只有打印。
 *   所以：回调里只做 O(N) 的插入排序（不打印），遍历结束后一次性打 2*MEM_DIAG_TOP_N 行。
 *
 * 【为什么要打"最大的已占用块"】
 *   回答"PSRAM 到底被谁占着"。拿到**地址 + 大小**就能与已知缓冲对照，例如：
 *     0x500011F8 处 ≈ 2.75 MB ⇒ ui_binfont 的 CJK 字体副本（ui_binfont.c:107，
 *     请求 2,744,120 = 字体文件大小，常驻，**属正常占用不是泄漏**）。
 *   只有空闲块分布的话，永远看不出"那块最大的占用是谁"。
 * ------------------------------------------------------------------------- */

/** 最多列出几个块（空闲 / 已占用各这么多） */
#define MEM_DIAG_TOP_N 6

typedef struct {
    const void *ptr[MEM_DIAG_TOP_N];
    size_t      sz [MEM_DIAG_TOP_N];
    uint32_t    cnt;
    size_t      total;
} mem_diag_toplist_t;

/** 降序插入，只保留前 MEM_DIAG_TOP_N 个。O(N)，无内存分配。 */
static inline void mem_diag_top_push(mem_diag_toplist_t *l, const void *p, size_t sz)
{
    uint32_t i;
    if (l->cnt < MEM_DIAG_TOP_N) {
        i = l->cnt++;
    } else {
        if (sz <= l->sz[MEM_DIAG_TOP_N - 1]) return;      /* 进不了榜 */
        i = MEM_DIAG_TOP_N - 1;
    }
    while (i > 0 && l->sz[i - 1] < sz) {
        l->sz[i]  = l->sz[i - 1];
        l->ptr[i] = l->ptr[i - 1];
        i--;
    }
    l->sz[i]  = sz;
    l->ptr[i] = p;
}

static inline bool mem_diag_walk_cb(walker_heap_into_t heap_info,
                                    walker_block_info_t block_info, void *user)
{
    (void)heap_info;                       /* 本工程只有一个 PSRAM heap */
    mem_diag_toplist_t *f = &((mem_diag_toplist_t *)user)[0];
    mem_diag_toplist_t *u = &((mem_diag_toplist_t *)user)[1];

    if (block_info.used) {
        u->total += block_info.size;
        mem_diag_top_push(u, block_info.ptr, block_info.size);
    } else {
        f->total += block_info.size;
        mem_diag_top_push(f, block_info.ptr, block_info.size);
    }
    return true;                           /* 继续遍历 */
}

/* ---- 统计"能否分 N 次各拿 size 字节"（判据用，比 largest 准确） ----
 *
 * ★★ 为什么需要它（2026-09-28 第三次修正判据）：
 *   驱动的 n+1 个缓冲是**n+1 次独立的 `heap_caps_aligned_alloc(per)`**，彼此不需相邻
 *   ⇒ 充要条件不是 `largest >= per`（那只是**必要**条件），而是
 *     "把这些空闲块切下去，能切出至少 (n+1) 个 per"。
 *   反例（本工程实测）：largest=1,835,008、per=921,600 ⇒ 切掉一块后只剩 913,408 < per，
 *   第二块就没地方了 —— 单看 largest 会得出"够"的错误结论。
 *   ⇒ 用 sum( floor(block_size / per) ) 作为"能切出几块"的**乐观上界**：
 *     一块 3*per 的大块算 3 块。对"不想被误报打扰"这个诉求来说，乐观方向是对的
 *     （真失败时驱动会自己报 `no mem for DVP backup buffer`）。
 */
typedef struct {
    size_t   min;        /* 单次分配要多少字节 */
    uint32_t n_ge;       /* size >= min 的块数（保守口径） */
    uint32_t capacity;   /* sum(size / min)：能切出几块（乐观上界） */
    size_t   largest;
} mem_diag_free_ctx_t;

static inline bool mem_diag_free_walk_cb(walker_heap_into_t heap_info,
                                         walker_block_info_t block_info, void *user)
{
    (void)heap_info;
    mem_diag_free_ctx_t *c = (mem_diag_free_ctx_t *)user;
    if (block_info.used) return true;                 /* 只看空闲块 */
    if (block_info.size > c->largest) c->largest = block_info.size;
    if (block_info.size >= c->min) {
        c->n_ge++;
        c->capacity += (uint32_t)(block_info.size / c->min);
    }
    return true;
}

/**
 * @brief 统计 PSRAM 里"分 N 次、每次 size 字节"能否成功。
 * @param size        单次分配的字节数
 * @param largest_out 输出最大空闲块（可 NULL）
 * @param n_ge_out    输出 "≥ size 的块数"（可 NULL，保守口径）
 * @return 能切出多少个 size 大小的块（sum(floor(block/size))，乐观上界）
 * ★ 用法：`cap >= 需要的块数` + `largest >= size` 两条一起看，才既不误报也不漏判。
 */
static inline uint32_t mem_diag_free_capacity(size_t size, size_t *largest_out, uint32_t *n_ge_out)
{
    mem_diag_free_ctx_t c = { size, 0, 0, 0 };   /* 顺序初始化：C / C++ 都合法 */
    heap_caps_walk(MALLOC_CAP_SPIRAM, mem_diag_free_walk_cb, &c);
    if (largest_out) *largest_out = c.largest;
    if (n_ge_out)    *n_ge_out    = c.n_ge;
    return c.capacity;
}

/**
 * @brief 打一份轻量碎片分布（PSRAM）：总计 + 最大 N 个空闲块 + 最大 N 个已占用块。
 * @param tag   日志 TAG（通常传模块自己的 TAG）
 * @param why   为什么打（会拼进日志，例如 "streammem" / "recog peak line not met"）
 * ★ 安全：内部用 heap_caps_walk 只收集，打印全部发生在遍历（及堆锁）结束之后。
 */
static inline void mem_diag_dump_top(const char *tag, const char *why)
{
    /* ★ 用 `= {0}` 而不是 C99 复合字面量 `(mem_diag_toplist_t){0}` ——
     *   本头既被 .c 也被 .cpp include，而**复合字面量是 C99 特性，C++ 不支持**。 */
    mem_diag_toplist_t l[2] = { { {0}, {0}, 0, 0 }, { {0}, {0}, 0, 0 } };

    heap_caps_walk(MALLOC_CAP_SPIRAM, mem_diag_walk_cb, l);

    ESP_LOGW(tag, "[%s] PSRAM blocks: free %u B in %u chunk(s) | used %u B in %u chunk(s)",
             why, (unsigned)l[0].total, (unsigned)l[0].cnt,
             (unsigned)l[1].total, (unsigned)l[1].cnt);
    for (uint32_t i = 0; i < l[0].cnt; i++) {
        ESP_LOGW(tag, "[%s]   free#%u  %8u B @ %p", why, (unsigned)(i + 1),
                 (unsigned)l[0].sz[i], l[0].ptr[i]);
    }
    for (uint32_t i = 0; i < l[1].cnt; i++) {
        ESP_LOGW(tag, "[%s]   USED#%u  %8u B @ %p   <-- 与其他模块的已知缓冲对照",
                 why, (unsigned)(i + 1), (unsigned)l[1].sz[i], l[1].ptr[i]);
    }
}
