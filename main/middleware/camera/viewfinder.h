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
 * 【缓冲策略：开机预分配、常驻、永不 free】
 *   1.8 MB 的出图缓冲一旦因为 PSRAM 被切碎而拿不到，后面就再也拿不回来了
 *   （见 开发文档/内存优化可执行方案.md 手段 B"预分配常驻"）。
 *   所以在 main.c 开机阶段（camera_service_init 之后）就调 viewfinder_init() 拿齐，
 *   顺便进 [mem] 分步账单。总预算 ≈ 3.0 MB PSRAM（§32 起 staging 扩到驱动上限）：
 *     - 出图缓冲   1,843,200 B（1280x720 RGB565，jpeg_alloc_decoder_mem 分配）
 *     - staging    921,600 B（JPEG 码流中转 = 驱动 JPEG 单缓冲上限）
 *     - 显示缓冲 x2  115,200 B x2（320x180 RGB565 双缓冲，防撕裂）
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

/**
 * @brief 开机预分配全部缓冲 + 建硬件解码引擎（幂等；失败可重试）
 *
 * ★ 故意放在 main.c 的开机分步账单里，而不是第一次进 VIDEO 页才做：
 *   避免运行中临时要 1.8 MB 连续 PSRAM（那时最可能拿不到），也让内存账单一眼可见。
 * @return ESP_OK / ESP_ERR_NO_MEM（PSRAM 不够）/ 其他（解码引擎创建失败）
 */
esp_err_t viewfinder_init(void);

/** 开始收帧（注册观察者 + 建解码任务）。幂等；首次调用会懒建任务。 */
esp_err_t viewfinder_enable(void);

/** 停止收帧（摘观察者 + 停任务）。已解码的最后一帧画面**保留**，UI 可以继续显示。 */
void viewfinder_disable(void);

bool viewfinder_is_enabled(void);

/**
 * @brief LVGL 线程取最新帧
 * @return true = 有没看过的新帧（out 已填）；false = 没有新帧（out 不动）
 * ★ 重复取同一帧返回 false —— 调用方不需要自己记 seq。
 */
bool viewfinder_take(viewfinder_frame_t *out);

/* ---- 诊断（进 VIDEO 页仪表盘 / 上机验收用）---- */
uint32_t viewfinder_decoded_count(void);    /**< 成功解码并发布的帧数 */
uint32_t viewfinder_dropped_count(void);    /**< 因限速/解码任务忙而丢掉的帧数 */
uint32_t viewfinder_last_decode_us(void);   /**< 上一次"解码+抽点"耗时（us） */

/* ---- 文件媒体共享解码入口（S4 文件管理页，2026-09-26）----
 * 照片预览 / AVI 播放复用本模块的**单例硬件引擎 + staging + 出图缓冲**，
 * 不再自配 1.8 MB 出图缓冲（PSRAM 扛不起第二份，见 S4 方案 §4.2）。
 *
 * ★★ 使用守约（file_mgr_service 必须遵守）：
 *   1. 仅当 `!viewfinder_is_enabled()` 时使用 —— s_stage/s_dec_out 与 vf_task 互斥；
 *   2. staging 由调用方借去装 JPEG 码流（fread 目标），借期内不得被别人写；
 *   3. decode 返回的指针是**单缓冲**，只在下一次 decode 调用前有效；
 *   4. 典型节奏（播放）：borrow_stage → fread 一帧 → decode_jpeg_mem → 抽点到
 *      自己的显示缓冲 → 下一帧。 */

/** 借出 JPEG 码流 staging（921,600 B，PSRAM）。未 init 返回 NULL。 */
uint8_t *viewfinder_borrow_stage(size_t *cap);

/**
 * @brief 同步解码一段 JPEG（720p）→ 1280x720 RGB565
 * @return 出图指针（VF_SRC_W*VF_SRC_H uint16，**下次调用前有效**）；失败 NULL
 * @param err ESP_ERR_INVALID_STATE = 取景器正在用共享缓冲；其余为解码/尺寸对账错误
 */
const uint16_t *viewfinder_decode_jpeg_mem(const uint8_t *jpg, size_t len, esp_err_t *err);

#ifdef __cplusplus
}
#endif
