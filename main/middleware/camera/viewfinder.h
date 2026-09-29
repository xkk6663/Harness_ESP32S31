/* viewfinder.h - VIDEO 页实时取景器（硬件 JPEG 解码 → 小尺寸 RGB565）
 *
 * ★★ 2026-09-25 新增（用户需求："就跟正常的相机设备一样，原画面可见，
 *    拍摄、录视频的原画面可预览"）。
 *
 * 【为什么必须有这个模块】
 *   传感器同一时刻只能有一种输出格式（ov3660_fmt.c 的硬约束）。录像/拍照用的是
 *   1280x720 JPEG，所以"录像期间也能看到画面"只有一条路：**把那条 JPEG 流解码出来**。
 *   解码器二选一（都已核实，见改动记录 §27）：
 *     - esp_new_jpeg（espressif__esp_new_jpeg）：**纯软件**（nm 确认无任何 ROM/硬件符号），
 *       支持 scale 但 CPU 重（Huffman 熵解码是全分辨率的，缩不放掉多少）；
 *     - IDF esp_driver_jpeg（S31 有硬件 JPEG 解码器，SOC_JPEG_DECODE_SUPPORTED=1）：
 *       CPU≈0，但**不支持降采样** ⇒ 全尺寸出图 1.8 MB，再由我们自己稀疏抽点。
 *   ⇒ 选硬件 + 稀疏抽点：每帧只碰 ~460 KB PSRAM（720 行里只读 180 行）+ 写 115 KB，
 *     约 5 ms；按 150 ms 一帧限速后 CPU 占用 < 5%，不会抢录像的实测 fps。
 *
 * 【怎么拿到帧】
 *   本模块向 camera_service 注册一个"**观察者**"回调（见 camera_service_set_observer）。
 *   观察者在 cam_task 上下文被调用，只做一件事：限速判断 + 把 JPEG 拷进 staging 缓冲 +
 *   举手。**绝不解码、绝不碰 LVGL**（cam_task 里阻塞会让 DVP 连续丢帧）。
 *   真正的解码在独立任务里做。⇒ 无论此刻跑的是取景流、拍照流还是录像流，
 *   取景器都能持续刷新 —— 这正是"录像中也能看到画面"的关键。
 *
 * 【缓冲策略：按需分配（2026-09-28 起），不再开机常驻】
 *   总预算 ≈ 3.0 MB PSRAM（§32 起 staging 扩到驱动上限）：
 *     - 出图缓冲   1,843,200 B（1280x720 RGB565，jpeg_alloc_decoder_mem 分配）
 *     - staging    921,600 B（JPEG 码流中转 = 驱动 JPEG 单缓冲上限）
 *     - 显示缓冲 x2  115,200 B x2（320x180 RGB565 双缓冲，防撕裂）
 *
 *   ★★ 历史：这 2.93 MB 曾是 main.c 开机一次性拿齐、**永不释放**的。旧策略的
 *     顾虑是"1.8 MB 大块一旦被 PSRAM 碎片挡住，就再也拿不回来了"（见
 *     开发文档/内存优化可执行方案.md 手段 B"预分配常驻"）。
 *   ★★ 2026-09-28 用户拍板改为**按需**：
 *     「打卡页加载模型按需分配，取景器的 2.99 MB 也按需分配，功能互不影响，
 *       需要用到的时候再加载，用完再销毁 —— 这样对 PSRAM 的利用率不是最好的嘛」
 *     实现 = `viewfinder_acquire()` / `viewfinder_release()`（见下）。
 *     出发点：这 2.93 MB 是本工程**最大的一笔可让出 PSRAM**（> MFN 加载峰值
 *     2,115,950 B），让出来正好够跑 dl 模型。
 *   ⇒ 现在**没进过 VIDEO/FILES 页**时，本模块占 PSRAM = **0**（只剩一个句柄）。
 *   ★ 换来的一条硬规则：acquire 必须由 **LVGL 线程在动作发起前**调用
 *     （绝不能等 files/vf 任务运行时自己临时要 1.8 MB —— 那正是碎片最严重的时刻，
 *      也正是当初"开机预分配"的理由；现在把它挪到"用户真的要看"的那一刻）。
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 取景画面尺寸 = 720p 的 1/4 稀疏抽点。改这里要同步 tab_video 的画布。
 *  ★ 4 必须是 1280 与 720 的公约数（1/2=640x360、1/4=320x180、1/8=160x90）。 */
#define VF_W    320u
#define VF_H    180u

/** 解码源尺寸（本工程唯一媒体档：720p JPEG）。文件预览/播放的抽点也按它算。 */
#define VF_SRC_W 1280u
#define VF_SRC_H  720u

/** 一帧取景画面（只读！缓冲归本模块所有，下次 take 到新帧前内容稳定） */
typedef struct {
    const uint16_t *buf;    /* RGB565，小端，LVGL canvas 直接可用 */
    uint32_t w;
    uint32_t h;
    uint32_t seq;           /* 帧序号，从 1 开始；用于 UI 判"是不是新帧" */
} viewfinder_frame_t;

/* ===========================================================================
 *  生命周期：按需 acquire / release（2026-09-28 新增）
 *
 *  ★★ 为什么有这一对：三块缓冲（2,995,200 B）过去只在 viewfinder_init() 里分配、
 *     **全文件搜不到一处 heap_caps_free** ⇒ 只要 main.c 开机调过一次，这 ~2.93 MB
 *     就常驻到关机，**即使从不进 VIDEO 页**。它是本工程最大的一笔可让出 PSRAM：
 *        取景器 2,995,200 B（s_dec_out 单块 1,843,200 B 连续）
 *      > MFN 加载峰值 2,115,950 B
 *      = 媒体档需求 (MEDIA_BUF_CNT+1) x 921,600 B（=1 时正是 1,843,200 B，与 s_dec_out 同尺寸）
 *     ⇒ 让出来正好够跑 dl 模型。用户拍板走"用时申请、用完销毁"。
 *
 *  ★★ 使用契约（违反任一条都会 UAF 或死锁）：
 *    1. **只能在 LVGL 线程调用**。硬原因：`lv_canvas_set_buffer()` 是**零拷贝**
 *       （只把指针写进 canvas 内嵌 draw_buf，无堆分配）——
 *       tab_video 的取景画布 / tab_files 的预览画布都可能正指向本模块的
 *       `s_disp[]` / `s_dec_out`。release 之前**必须**先 `lv_canvas_set_buffer()`
 *       把画布换回调用方自持的缓冲，否则画布直接指向已释放内存。
 *       而 canvas 归 LVGL 线程独占 ⇒ 整套 acquire/release 必须在 LVGL 线程做。
 *       （两个消费方都满足：tab_video 全在 LVGL 线程；tab_files 的预览/播放虽然
 *        在 files 任务里跑，但 acquire 在 post 命令**之前**、release 在
 *        FME_*_DONE 事件**回到 LVGL 线程之后** —— 见 tab_files.c。）
 *    2. **幂等**：重复 acquire（缓冲已在）返回 ESP_OK；重复 release（本来就没占）直接返回。
 *    3. **acquire ≠ enable**：acquire 只保证"缓冲 + 解码引擎"到位（足以借给文件页
 *       做同步解码）；`viewfinder_enable()` 才是"注册观察者、起解码任务、开始收帧"。
 *       两者可独立使用 —— 这正是 FILES 页预览/播放要的形态（只借缓冲，不收帧）。
 *    4. **release 内部先做 `viewfinder_disable()`**（幂等），所以调用方不必再单独 disable。
 *    5. `s_dec`（硬件解码引擎句柄）**只建一次、永不销毁**（它只占内部 RAM 的
 *       DMA 描述符等小结构）。释放的只有三块 PSRAM 数据缓冲。
 * =========================================================================== */

/**
 * @brief 申请取景器缓冲（幂等）。失败可重试，不影响其它功能。
 * @return ESP_OK / ESP_ERR_NO_MEM（PSRAM 连续块不够）/ 其他（解码引擎创建失败）
 * ★ 只在 LVGL 线程调用（见上方契约 1）。
 */
esp_err_t viewfinder_acquire(void);

/**
 * @brief 归还取景器缓冲（幂等）：先停收帧，再释放 s_dec_out / s_stage / s_disp[]。
 * ★ 只在 LVGL 线程调用；调用方**必须已让所有 canvas 松开指向本模块缓冲的指针**。
 * ★ 若解码任务在超时窗口内没能退出（内部有 100 ms 等待），本次**拒绝释放**并告警
 *   —— 宁可不省这点内存，也不能在任务还在写缓冲时把它 free 掉。
 */
void viewfinder_release(void);

/** @brief `viewfinder_acquire()` 的兼容别名（历史调用方/兜底路径用）。 */
esp_err_t viewfinder_init(void);

/** 开始收帧（注册观察者 + 建解码任务）。幂等；首次调用会懒建任务。
 *  ★ 若缓冲尚未 acquire，本函数会先 `viewfinder_acquire()` 兜底 —— 但**不要在
 *    运行期依赖这条兜底**：那等于在碎片最严重的时刻临时要 1.8 MB 连续 PSRAM。
 *    正确做法是调用方先在 LVGL 线程 acquire 成功，再 enable。 */
esp_err_t viewfinder_enable(void);

/** 停止收帧（摘观察者 + 停任务）。已解码的最后一帧画面**保留**，UI 可以继续显示。
 *  ⚠ **本函数不释放任何缓冲**（只停任务）—— 要真正把 2.93 MB 还给系统，调
 *    `viewfinder_release()`。反过来说：只 disable 不 release，内存账单**不会**变小。 */
void viewfinder_disable(void);

bool viewfinder_is_enabled(void);

/**
 * @brief LVGL 线程取最新帧
 * @return true = 有没看过的新帧（out 已填）；false = 没有新帧（out 不动）
 * ★ 重复取同一帧返回 false —— 调用方不需要自己记 seq。
 */
bool viewfinder_take(viewfinder_frame_t *out);

/**
 * @brief 取"当前已发布的最新一帧"的只读指针（**不看序号**，重复调用返回同一帧）。
 *
 * ★ 与 `viewfinder_take()` 的区别：take 是"只给新帧"（同一帧给第二次返回 false）；
 *   本函数是"给我最后一帧"，不关心看过没看过。
 * ★ 唯一用途：调用方在 `viewfinder_release()` **之前**把最后一帧拷进自持缓冲，
 *   这样"释放双缓冲"与"画面停在最后一帧"可以同时成立（否则画布只能变黑占位）。
 *   参考实现：tab_video.c 的 `vf_release_all()`。
 * @return 只读指针（VF_W*VF_H uint16，小端 RGB565）；未 acquire 或从未发布过帧时 NULL。
 *  ⚠ 指向的缓冲随下一次 `viewfinder_release()` 失效 —— 拷完再用。
 */
const uint16_t *viewfinder_last_frame(void);

/* ---- 诊断（进 VIDEO 页仪表盘 / 上机验收用）---- */
uint32_t viewfinder_decoded_count(void);    /**< 成功解码并发布的帧数 */
uint32_t viewfinder_dropped_count(void);    /**< 因限速/解码任务忙而丢掉的帧数 */
uint32_t viewfinder_last_decode_us(void);   /**< 上一次"解码+抽点"耗时（us） */

/* ---- 文件媒体共享解码入口（S4 文件管理页，2026-09-26）----
 * 照片预览 / AVI 播放复用本模块的**单例硬件引擎 + staging + 出图缓冲**，
 * 不再自配 1.8 MB 出图缓冲（PSRAM 扛不起第二份，见 S4 方案 §4.2）。
 *
 * ★★ 使用守约（file_mgr_service 必须遵守）：
 *   0. **调用方（tab_files）必须先在 LVGL 线程 `viewfinder_acquire()`**，
 *      并且在整个预览/播放事务结束（FME_PREVIEW_DONE / FME_PLAY_DONE 回到
 *      LVGL 线程）之前**不得 release** —— 否则 files 任务手里的 stage/出图缓冲
 *      会在使用中被 free；
 *   1. 仅当 `!viewfinder_is_enabled()` 时使用 —— s_stage/s_dec_out 与 vf_task 互斥；
 *   2. staging 由调用方借去装 JPEG 码流（fread 目标），借期内不得被别人写；
 *   3. decode 返回的指针是**单缓冲**，只在下一次 decode 调用前有效；
 *   4. 典型节奏（播放）：borrow_stage → fread 一帧 → decode_jpeg_mem → 抽点到
 *      自己的显示缓冲 → 下一帧。 */

/** 借出 JPEG 码流 staging（921,600 B，PSRAM）。**未 acquire 时返回 NULL**。
 *
 *  ★★ 契约：调用方必须**先在 LVGL 线程 `viewfinder_acquire()` 成功**，再发命令。
 *    本函数是**纯 getter，不会触发惰性 init**（对比 `viewfinder_decode_jpeg_mem()`
 *    里有 `if (!s_inited) viewfinder_init();`）。
 *    ⚠ 为什么故意不在这里兜底 init：本函数的调用者（file_mgr 的 `do_preview()` /
 *      `do_play()`）跑在 **files 任务**上下文，在那里临时要 1.8 MB 连续 PSRAM
 *      风险最高（碎片）—— 正是"按需"要避免的时刻。所以让它在未就绪时**干脆返回
 *      NULL**，由调用方把错误报上去（`file_mgr_service.c` 会打明确日志指明原因）。
 *  ⇒ 实现侧对应：`tab_files.c` 的 `open_path()` 在 post FMC_PREVIEW / FMC_PLAY
 *    **之前**调 `viewfinder_acquire()`，保证 files 任务被调度时 s_stage 已就位。 */
uint8_t *viewfinder_borrow_stage(size_t *cap);

/**
 * @brief 同步解码一段 JPEG → RGB565（**任意 ≤720p 尺寸**，不只 1280x720）
 *
 * ★ 2026-09-28 放宽（用户反馈"face db 页预览打卡照报 NO_MEM/打不开"）：
 *   旧版硬性要求 out_size == 1280*720*2（720p 专用对账），而打卡照片是
 *   240x240 —— 115,200 B ≠ 1,843,200 B，直接被拒。现在改为：
 *     ① 先 `jpeg_decoder_get_info()` 拿真实宽高（YUV420/422 的 JPEG 会被
 *        引擎按 16 像素边界补齐，因此同时接受"可见尺寸"与"补齐尺寸"两种
 *        出图大小，以 s_dec_out_sz 为硬上限）；
 *     ② 宽高通过 out_w/out_h **带回给调用方**，由它决定怎么缩放
 *       （file_mgr 的 sub_step2 已同步改成任意尺寸的最近邻抽点）。
 *   vf_task 自己的 720p 取景路径**不经过本函数**（对账铁律原样保留在那边）。
 *
 * @return 出图指针（**下次调用前有效**）；失败 NULL
 * @param out_w,out_h 输出：解码出的实际**可见**宽/高（可为 NULL = 不关心）
 * @param out_stride  输出：**行步长**（uint16 数）。YUV420/422 的 JPEG 被引擎按
 *                    16 像素补齐时，步长 = 补齐后的宽 > 可见宽 —— 调用方逐行
 *                    寻址**必须**用它，否则画面会错位（可为 NULL = 不关心）
 * @param err ESP_ERR_INVALID_STATE = 取景器正在用共享缓冲；其余为解码/尺寸错误
 */
const uint16_t *viewfinder_decode_jpeg_mem(const uint8_t *jpg, size_t len,
                                           uint16_t *out_w, uint16_t *out_h,
                                           uint16_t *out_stride,
                                           esp_err_t *err);

#ifdef __cplusplus
}
#endif
