/* avi_mjpeg_muxer.h - 把一串 JPEG 帧封成能播的 AVI(MJPG) 文件
 *
 * ★★ 设计约束（**故意**这样，别改）：
 *   ① **纯 C99，不 include 任何 ESP-IDF / LVGL / esp_* 头**，只用 stdio。
 *      ⇒ 可以脱离 IDF 在 PC 上原样编译并单测（本项目沙箱跑不了 idf.py，
 *        所以"能原生单测"意味着**这个模块的字节级正确性是真的被验证过的**，
 *        而不是靠人眼读结构体）。
 *      ⇒ 也**不要**在这里加 ESP_LOG：日志会引入 esp_log.h，破坏上面这条。
 *        要日志请在调用方（record_video_service）打。
 *   ② 返回值用本文件自己的 `avi_result_t`，不用 `esp_err_t`（同上：保持可移植）。
 *
 * ★ 为什么 idx1 要在 close 时**重扫文件**生成，而不是边录边记：
 *   12 fps × 10 min = 7200 帧。每条 idx1 16 B ⇒ 115 KB。这个工程内部 RAM 只有
 *   ~4 KB 余量（见 docs/开发文档/内存管理规划.md），PSRAM 也该省着用。
 *   重扫一遍的代价：7200 次 (fseek + 读 8 B) —— 在 SD 卡上是几十 ms 级，
 *   而收益是**常驻内存 = 0**。⇒ 这个交换对本工程是明显划算的。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AVI_OK        =  0,
    AVI_ERR_IO    = -1,   /* fopen/fwrite/fseek 失败（卡满、卡拔出、只读……） */
    AVI_ERR_ARG   = -2,   /* 参数非法（尺寸为 0、fps 为 0、帧数据为空……） */
    AVI_ERR_NOMEM = -3,   /* 分配 muxer 结构失败 */
    AVI_ERR_STATE = -4,   /* 状态错（已 close 又 add、重复 close……） */
} avi_result_t;

typedef struct avi_muxer avi_muxer_t;   /* 不透明句柄 */

/* 录制统计（给 UI 显示用；只读快照） */
typedef struct {
    uint32_t frames;         /* 已成功写入的帧数 */
    uint64_t total_bytes;    /* 当前文件字节数 */
    uint32_t max_frame_size; /* 最大单帧字节数（用来估 dwSuggestedBufferSize） */
    uint32_t avg_frame_size; /* 平均单帧字节数 */
} avi_stats_t;

/**
 * 打开并写入 AVI 头（avih/strh/strf 先写占位值，close 时回填真值）。
 *
 * @param path  目标路径（须是 VFS 能解析的，例如 "/sdcard/VID_20260925_213000.avi"）
 * @param width 帧宽（= JPEG 的宽）
 * @param height 帧高
 * @param fps   标称帧率，写进 strh.dwRate。★ 是"标称值"：真实帧率受出帧/写盘速度影响，
 *              close 时会把**实测**平均 fps 也写进 avih，两者不一致时以 avih 为准（见 close）
 * @return 句柄；失败返回 NULL（并已确保不留下半开的 FILE*）
 */
avi_muxer_t *avi_muxer_open(const char *path, uint32_t width, uint32_t height, uint32_t fps);

/**
 * 追加一帧 JPEG。
 *
 * ★ 传入的必须是**完整的一帧 JPEG 码流**（含 SOI FFD8 与 EOI FFD9）。
 *   本函数**不解析、不校验、不改写**内容，只按 '00dc' chunk 原样写入。
 * ★ 帧数据来自 cam_task 回调，生命周期只到回调返回为止 ⇒ 调用方负责"拷走再交给本函数"
 *   或"在本函数内同步写完"（本函数返回时数据已落盘/已进 stdio 缓冲，调用方即可释放）。
 */
avi_result_t avi_muxer_add_jpeg(avi_muxer_t *m, const uint8_t *jpeg, uint32_t size);

/**
 * 正常收尾：回填 avih（总帧数 / 实测平均 fps / 建议缓冲）、回填 movi 与 RIFF 大小、
 * 生成 idx1、关闭文件、释放句柄。
 *
 * ★ 这是**唯一**能让 .avi 被播放器"拖进度条"的路径（idx1 是索引）。
 * ★ 返回 AVI_OK 才算文件完整。返回错误时文件通常仍可播放（只是缺索引/头部信息不准），
 *   但**必须**在 UI 上如实告诉用户，不要假装成功。
 */
avi_result_t avi_muxer_close(avi_muxer_t *m);

/**
 * 异常收尾（拔卡 / 断电前的兜底 / 用户强停）：**只关文件、不回填、不生成 idx1**。
 * ★ 记不住时间戳的话，宁可只 close。调用后句柄失效。
 * ★ 与 close 的区别：close 会 fseek 回文件头写十几处，拔卡时那些写必然失败；
 *   abort 只 flush + fclose，失败面最小。
 */
void avi_muxer_abort(avi_muxer_t *m);

/** 取统计快照（未 close 时也可调，给 UI 显示"已录 xx 帧 / xx MB"）。 */
void avi_muxer_get_stats(const avi_muxer_t *m, avi_stats_t *out);

/**
 * 用**实测**平均帧率覆盖标称值（open 之后、close 之前调用）。
 *
 * ★ 为什么需要它：本工程 JPEG 1280x720 的**标称**是 12 fps，但实际出帧受 AE/AWB 收敛与
 *   SD 写盘速度影响，实测常年低于标称。若把标称 12 写进文件，播放器按 12 fps 播，
 *   结果就是"画面比实际录制快" —— 而"时长与录制时间一致"正是阶段 S2b 的验收判据之一。
 * ⇒ 调用方在停止时把 (总帧数 ÷ 实际秒数) 传进来，文件的时长元数据才与真实一致。
 *
 * @param fps_x100 帧率 ×100（定点），例如 9.63 fps 传 963；0 表示忽略。
 */
void avi_muxer_set_real_fps(avi_muxer_t *m, uint32_t fps_x100);

/**
 * @brief 换掉 close() 用的**批缓冲分配器**（可选；默认 malloc/free）
 *
 * ★ 为什么需要这个钩子（且**只**给这一处开钩子）：
 *   close() 生成 idx1 时要一块 `IDX_BATCH_ENTRIES * IDX_ENTRY` = 4 KB 的临时批缓冲，
 *   这是本模块**唯一**的大块临时分配。而 ESP 侧内部 RAM 只剩 ~4 KB 余量
 *   （见 docs/开发文档/内存管理规划.md）⇒ 内部堆上申请 4 KB 随时可能失败，
 *   一失败就退化成"无索引 AVI"（功能可用但会丢掉 S2b 的验收项）。
 *   ⇒ 由调用方（record_video_service）把它换成 **PSRAM 版**分配器。
 * ★ muxer 自身结构体（一两百字节）仍走 calloc / 内部堆，**不**在此列 —— 不做无谓的扩大。
 * ★ 必须在 avi_muxer_open() 之前设置；两个钩子必须成对配套（同一块内存要由配对的
 *   释放函数归还），中途切换请确保没有存活的 muxer。
 * ★ 传 NULL 可恢复默认（malloc/free）。
 */
typedef void *(*avi_alloc_fn)(size_t size);
typedef void  (*avi_release_fn)(void *ptr);
void avi_muxer_set_batch_allocator(avi_alloc_fn alloc_fn, avi_release_fn release_fn);

#ifdef __cplusplus
}
#endif
