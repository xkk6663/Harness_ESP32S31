/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file punch_service.h
 * @brief 中间层：打卡事务（S8-2/S8-3）—— 认出人 → 叠水印 → JPEG 软编 → 落盘 → 写流水
 *
 * 方案：docs/项目规划/S8_人脸识别打卡系统方案.md §4（数据布局）/§5.4（模块设计）
 *
 * ===========================================================================
 *  ★ 与 capture_service 的关系（**互不影响、也不互相调用**）
 * ===========================================================================
 *   · `capture_service` = VIDEO tab 的 **720p 手动拍照**（sensor 硬编，零拷贝落盘）；
 *   · `punch_service`   = CAM tab 的 **240x240 自动打卡存证**（软编，能叠水印）。
 *  两者都写 SD，但目录不同（/photos vs /attendance）、相机归属不同
 *  （punch **不申请 owner**，只读 CAM 已经跑着的预览流）。
 *  ⚠ 唯一必须共享的是 **JPEG 编码器**（`jpeg_util`），那里自带互斥。
 *
 * ===========================================================================
 *  ★★ 两阶段 API 的由来（这是本模块唯一"不直观"的地方，先读这段）
 * ===========================================================================
 *  水印要在**照片像素**上画中文姓名与时间，而画中文只能用 LVGL 的字体
 *  （方案 §7-D4 选 W1），**LVGL 只能在 LVGL 线程碰**（本工程铁律，见 §9-R9）。
 *  但 JPEG 编码 + 写盘是几十毫秒的阻塞活，**绝不能**放在 LVGL 线程。
 *
 *  ⇒ 于是把一次打卡拆成"LVGL 线程负责画、独立任务负责写"两拍：
 *
 *      ① `punch_service_begin()`   —— LVGL 线程：守卫 + **同步 memcpy 帧** + 占位
 *                                     （返回一个可写的 RGB888 缓冲给调用方画水印）
 *      ② 调用方在**同一个 LVGL 上下文**里往那个缓冲上画姓名/时间
 *      ③ `punch_service_commit()`  —— LVGL 线程：起一次性任务（编码 → 落盘 → 写 CSV）
 *
 *  为什么 memcpy 放在 ① 而不是让任务自己取：调用方的帧（识别引擎的缓冲）
 *  在下一次识别时就**被覆盖**了，必须在离开 LVGL 线程之前拷走。
 *  172,800 B 的 PSRAM→PSRAM 拷贝约 1 ms，远比"跨线程共享一个会变的指针"便宜。
 *
 * ===========================================================================
 *  ★ 三条不可动摇的纪律
 * ===========================================================================
 *  1. **未校时绝不写 1970**（§9-R5）：`date`/`time` 列写 `--`，`epoch_s` 写 0，
 *     文件名退化成序号（`IMG_0007.jpg`）。判断走 `net_time_service_get()` 的
 *     `valid` 字段 —— 那个字段就是为了"让忘查不可能"才存在的。
 *  2. **冷却期内不重复记录**（§9-R8）：同一 `person_id` 在 `PUNCH_COOLDOWN_S`
 *     内只记一次。计时基准是 `esp_timer_get_time()` 的**单调时钟**，不是 `time()`
 *     （未联网时 `time()` 会跳，跳到过去就等于冷却立刻失效）。
 *  3. **只写 /sdcard/attendance**：本模块**绝不**碰 /sdcard/face_db
 *     （那是系统状态，掉了要重录）。FILES 页"清空打卡记录"能安全执行的前提
 *     就是这条边界。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "face_recog_service.h"     /* recog_result_t */
#include "face_db.h"                /* FACE_NAME_MAX, FACE_DB_GUEST_ID */

#ifdef __cplusplus
extern "C" {
#endif

/** 打卡根目录（相对 SDCARD_MNT_PATH） */
#define PUNCH_DIR_NAME      "/attendance"
/** 流水文件名（PC 端 Excel 直接能打开） */
#define PUNCH_CSV_NAME      "index.csv"

/** 冷却期（秒）：同一人只记一次。跑起来嫌少/嫌多改这一个宏。 */
#define PUNCH_COOLDOWN_S    60

/** 单条记录里"相对路径"的容量（`<NN_姓名>/<YYYYMMDD_HHMMSS>.jpg`）。
 *  最坏 = 2+1+18(6 汉字) + 1 + 13 + 4 = 39 B ⇒ 48 有 9 B 余量。 */
#define PUNCH_REL_MAX       48
/** 绝对路径容量。最坏 = "/sdcard"(7) + "/attendance"(11) + "/"(1) + 39 + NUL = 59。
 *  ★ 取 160 是给 `-Werror=format-truncation=` 留的证明余量（见 capture_service.h
 *    那段血泪注释：缓冲太小不是"截断得难看"，是**编译失败**）。 */
#define PUNCH_PATH_MAX      160

/** 读流水时单次最多返回多少条（调用方给的数组容量上限参考）。
 *  ★ S8-4 从 64 提到 128：记录浏览页一次最多能翻到 128 条历史；
 *    调用方那侧是一块 PSRAM 数组（128 × ~120 B ≈ 15 KB），不占内部 RAM。 */
#define PUNCH_LOG_MAX       128

/** `punch_service_begin()` 的结论（**为什么没发起**，UI 据此给不同提示） */
typedef enum {
    PUNCH_PREP_OK = 0,      /* 已占位，可以画水印了 */
    PUNCH_PREP_COOLDOWN,    /* 冷却期内 —— 不是错误，UI 应提示"刚才已打卡" */
    PUNCH_PREP_BUSY,        /* 上一次还在写 */
    PUNCH_PREP_NO_SD,       /* SD 未挂载 */
    PUNCH_PREP_NO_DB,       /* 人脸库不可用（没法决定目录名的序号） */
    PUNCH_PREP_NO_MEM,      /* 帧缓冲/输出缓冲分配失败 */
    PUNCH_PREP_BAD_ARG,     /* 参数不对 */
} punch_prep_t;

/** 最近一次（或正在进行的）打卡结果快照 */
typedef struct {
    esp_err_t err;                        /* ESP_OK = 照片与流水都写成功了 */
    int       person_id;                  /* 0 = 访客 */
    bool      matched;                    /* 是否认出库内的人 */
    char      name[FACE_NAME_MAX];
    float     score;
    char      path[PUNCH_PATH_MAX];       /* 落盘的 .jpg 绝对路径（失败时可能为空） */
    size_t    jpeg_bytes;
    uint32_t  elapsed_ms;
    bool      time_valid;                 /* R5：本次时间是否可信 */
    /* ★ 这次跑的是"清空"还是"打卡"。
     *  UI 只靠 `elapsed_ms != 0` 判"确实跑过一次"，但一次成功的清空
     *  与一次成功的打卡落在同一个分支里会给出完全不同的提示文案
     *  （"Attendance records cleared" vs "Punched: 张三"），所以必须区分。 */
    bool      clear_op;
    /* ★★ "这一次任务真的跑完了"的**唯一**判据（UI 用它对账是否要弹提示）。
     *
     *  ⚠ 不要再用 `elapsed_ms != 0` 当哨兵：`elapsed_ms` 以 tick 为单位
     *    （10 ms 一跳），而 punch_task 有几条**立刻失败**的早退路径
     *    （参数不对 / SD 未挂载 / 目录拼不出来）—— 它们会在同一个 tick 内
     *    走完 `goto finish`，于是 `elapsed_ms == 0`，UI 就**永远不会提示
     *    "打卡失败"**，用户只看到"按了没反应"。这类"哨兵值被合法数据撞上"
     *    的 bug 在本工程已经出现过一次（§15.2 坑 1），一律改用显式标志位。 */
    bool      has_result;
} punch_record_t;

/** 流水里的一条（供 S8-4 的记录浏览页读） */
typedef struct {
    int   seq;
    int   person_id;
    char  name[FACE_NAME_MAX];
    char  date[12];                 /* "2026-09-28" 或 "--" */
    char  time[10];                 /* "10:30:45"    或 "--" */
    char  rel[PUNCH_REL_MAX];       /* "<NN_姓名>/<file>.jpg" */
    float score;
    /* ★ S8-4：排序用的绝对时间。0 = 该条记录未校时（date/time 为 "--"）。
     *  UI 的"按日期"视图直接按它倒序排 —— **不要**去比较 date/time 字符串：
     *  未校时的那批是 "--"，字典序会插在"2026-…"前面，表现成
     *  "新记录被一堆 -- 顶到下面"，而这种错在数据少的时候根本看不出来。 */
    int64_t epoch_s;
} punch_log_t;

/** 初始化（幂等）。**不分配大缓冲**（帧缓冲与 JPEG 输出都在第一次真正打卡时分配）。 */
void punch_service_init(void);

/**
 * @brief 阶段 ①（**LVGL 线程**）：守卫 + 同步拷帧 + 占位。
 *
 * @param who       识别结果（`person_id`/`name`/`matched`/`score`）
 * @param src_rgb888 待存证的 RGB888 帧（来自 `face_recog_frame()`，**所见即所存**）
 * @param w,h       帧尺寸（必须 ≤ 引擎支持的上限）
 * @param out_frame 输出：**可写的** RGB888 缓冲首地址（与 src 同尺寸、已拷好内容）。
 *                  调用方在这个缓冲上画水印。
 *
 * @return PUNCH_PREP_OK 才是"拿到了缓冲"；其余值都要按原因给用户提示，且
 *         **不要**调 commit()（没有占位）。
 *
 * ★ 冷却期的扣减发生在**本函数返回 OK 的那一刻**（而不是 commit 之后）：
 *   否则"画水印失败 → 不 commit → 冷却没扣 → 下一帧又发起"会变成死循环刷屏。
 */
punch_prep_t punch_service_begin(const recog_result_t *who,
                                 const uint8_t *src_rgb888,
                                 uint16_t w, uint16_t h,
                                 uint8_t **out_frame);

/** 阶段 ③（**LVGL 线程**）：把画好水印的帧交给一次性任务去编码/落盘/写流水。
 *  @return ESP_OK 已受理；ESP_ERR_INVALID_STATE 没有占位（没先 begin）。 */
esp_err_t punch_service_commit(void);

/** 放弃本次占位（画水印失败时回滚；**不会**退还冷却计时的扣减，见 begin 注释）。 */
void punch_service_abort(void);

/** 是否在写（或正在清空记录）。UI 据此把按钮置灰。 */
bool punch_service_busy(void);

/** 是否在冷却期内（UI 用来预判，避免先 begin 再被拒）。 */
bool punch_service_in_cooldown(int person_id);

/** 取结果快照 / 清结果（UI 显示完提示后清，避免重复提示）。 */
void punch_service_get_last(punch_record_t *out);
void punch_service_clear_last(void);

/**
 * @brief 读打卡流水（**最后 max 条**，按文件顺序，最新的在数组末尾）。
 *
 * ★ 只读、不解码、不分配 —— 结果写进调用方给的数组。
 * ★ 行解析是**逐字段 CRC 无关的宽松解析**：任何一行解析失败就跳过那一行，
 *   绝不让一行坏数据毁掉整次浏览（SD 卡在掉电/拔出时出现半行是常态）。
 *
 * @return 实际读到的条数（0 = 没有记录 / 文件不存在；**不区分**这两种，都在 UI 里
 *         显示成"暂无记录"）
 */
int punch_service_log_read(punch_log_t *out, int max);

/**
 * @brief 清空打卡记录（**异步**）：删 /sdcard/attendance 下的全部照片目录，
 *        并把 index.csv 截断。
 *
 * ★★ 红线：**只动 /sdcard/attendance**，绝不碰 /sdcard/face_db
 *   —— 这是 S8-4 的一条验收项（"清空打卡记录不影响人脸库"）。
 * ★ 异步：删除要逐个 unlink（几百个文件），不能让 LVGL 线程等。
 *   发起后用 `punch_service_busy()` 轮询，完成时 `punch_service_get_last()->err` 给结论。
 */
esp_err_t punch_service_log_clear(void);

/**
 * @brief 删除**单条**打卡记录（**异步**）：删照片文件 + 从 index.csv 里去掉那一行。
 *
 * ★ 2026-09-28 新增（用户反馈"face db 页没有删除打卡照的按钮"）。
 * ★ seq 是 CSV 第一列的流水号 —— 它是**跨视图稳定**的行标识（数组下标会随
 *   分组/排序变化，绝不能拿它当删除依据）。
 * ★ 照片不存在不算错误（可能已从 FILES 页单独删过）：CSV 行照删。
 * ★ 异步理由同 log_clear：重写 CSV 是阻塞 IO，不能让 LVGL 线程等。
 *   发起后用 `punch_service_busy()` 轮询。
 * @param seq CSV 第一列的流水号（>0）
 * @return ESP_OK 已受理 / ESP_ERR_INVALID_ARG 参数不对 /
 *         ESP_ERR_INVALID_STATE 正忙或 SD 未挂载 / ESP_ERR_NOT_FOUND CSV 里没有这一行
 *         （NOT_FOUND 在任务里才知道 —— 受理成功不代表行存在，UI 以重读结果为准）
 */
esp_err_t punch_service_log_delete(int seq);

/**
 * @brief 清除**某个人**的全部打卡痕迹（**异步**，§73.25）：删
 *        /sdcard/attendance/<NN_姓名>/ 整个目录（连照片），并把 index.csv
 *        里 photo 列以该目录为前缀的行全部去掉。
 *
 * ★ 2026-09-30 新增（用户反馈"face db 删人后打卡文件夹和照片还留着"）。
 *   面向的调用点是 face db 人物页的删除按钮：face_db_delete 只管库
 *   （索引 + 特征 + 底片），打卡目录是 punch_service 的领地 —— 两边各清
 *   各的，与"清空打卡记录不影响人脸库"红线互为镜像。
 * ★ 目录名用与写入侧同一生成器 face_db_attendance_dir(id, name)（序号 +
 *   安全化姓名），删人后 id 不复用，前缀依旧唯一。
 * ★ CSV 行按 photo 相对路径前缀（"<NN_姓名>/"）圈中，不依赖 name 列。
 * ★ 异步理由同 log_clear；发起后用 `punch_service_busy()` 轮询，
 *   结果槽 clear_op=true（CAM 页/其他 UI 不会弹"打卡成功"）。
 * @param id   人物 id（访客 0 拒绝）
 * @param name 人物姓名（UTF-8；用于生成目录名，须与录入时一致）
 * @return ESP_OK 已受理 / ESP_ERR_INVALID_ARG 参数不对 /
 *         ESP_ERR_INVALID_STATE 正忙或 SD 未挂载
 */
esp_err_t punch_service_purge_person(uint16_t id, const char *name);

/**
 * @brief 同步打卡流水与照片目录（**异步**）：把 index.csv 里"照片文件已经
 *        不存在"的行整行删掉，再清掉 attendance 下的空子目录。
 *
 * ★ 2026-09-28 新增（用户反馈"在 FILES 页删了打卡照，face db 记录页还显示
 *   那条记录"）。成因：CSV 与照片是**两个文件**，从 FILES 页删照片不会改
 *   CSV ⇒ 记录页照单全收。本函数就是两者之间的对账。
 * ★ 只删"照片缺失"的行：姓名/时间还在 CSV 里但图没了的记录，对用户而言
 *   就是"点开预览必失败"的死条目，留着只会制造困惑。
 * ★ 红线同 log_clear：只动 /sdcard/attendance，绝不碰 /sdcard/face_db。
 * ★ 异步理由同上；发起后用 `punch_service_busy()` 轮询。
 * @return ESP_OK 已受理 / ESP_ERR_INVALID_STATE 正忙或 SD 未挂载
 */
esp_err_t punch_service_log_sync(void);

/**
 * @brief 把一张 RGB888 图编码成 JPEG 并作为**底片**写入人脸库
 *        （`/sdcard/face_db/thumbs/NN.jpg`）—— S8-1 录入完成时那一次调用。
 *
 * ★ 放在这里而不是识别/UI 模块：底片与打卡照片是**同一件事**（RGB888 → JPEG），
 *   复用同一个编码器实例与同一块输出缓冲是唯一合理的选择（另开一份等于
 *   多占 64 KB PSRAM + 多一个编码器上下文）。
 * ★ **同步阻塞**（~30~60 ms）。只在"录入成功"那一刻调用一次，可以接受；
 *   但**不要**在 50 ms 的 UI 刷新定时器里调它。
 *
 * @param id      库内 id（决定文件名 NN.jpg）
 * @return ESP_OK / ESP_ERR_INVALID_STATE（正在打卡/清空）/ 编码或写盘错误
 */
esp_err_t punch_service_write_thumb(uint16_t id, const uint8_t *rgb888, int w, int h);

/**
 * @brief 把打卡目录的**真实现状**打到日志（诊断用，卡上到底有没有东西）。
 *
 * ★ 存在的理由：用户侧唯一的现象是"识别成功了但卡上看不到打卡图"，
 *   而这句现象对应**至少**四种完全不同的成因（旧固件打卡 100% 失败 /
 *   JPEG 编码失败 / 写了但 FATFS 没落盘 / 只是没进 attendance 子目录看）。
 *   从设备自己把 `root` 下每个子目录的文件数与字节数打出来，一次就能分辨。
 * ★ 只读、不创建任何东西；目录 / SD 不存在时给**带结论**的警告而不是报错。
 * ★ 同步短操作（几十 ms 级，条目很多时可能上百 ms）⇒ 在**建页**时调，
 *   **不要**放进 50 ms 的刷新定时器。
 */
void punch_service_dump_dir(void);

#ifdef __cplusplus
}
#endif
