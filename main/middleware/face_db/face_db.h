/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file face_db.h
 * @brief 中间层：人脸库（S8-1）—— 特征索引的落盘与常驻内存视图
 *
 * 方案：docs/项目规划/S8_人脸识别打卡系统方案.md §4（SD 卡数据布局）/ §5.2（模块设计）
 *
 * ===========================================================================
 *  ★ 三条边界（先说清楚，免得后来人把职责揉进来）
 * ===========================================================================
 *  1. **只管库，不管打卡**。库是"系统状态"（掉了要重录），打卡是"业务记录"（可整目录
 *     拷走）；生命周期、备份策略、删除权限全不同 ⇒ 目录分开（本模块 /sdcard/face_db，
 *     打卡 /sdcard/attendance），接口也分开（本文件 vs punch_service）。
 *     FILES 页才能做到"一键清空打卡记录"而**不误删人脸库**。
 *
 *  2. **只管存储，不管识别**。`face_db_enroll()` 接收调用方算好的特征向量，
 *     **不 include 任何 esp-dl 头**。识别引擎（§5.3 face_recog_service）反过来调本模块
 *     查库。⇒ S8-0 的模型结论**不影响本模块**，可独立开发、独立上机验收。
 *
 *  3. **降级语义（母方案 §9 S3 判据，红线）**：
 *     SD 没插 / 挂载失败 ⇒ `face_db_init()` 返回错误、`face_db_is_ready()` = false，
 *     但**人脸检测照常可用**（检测模型在 littlefs）。调用方**不得**把 init 失败当致命错误。
 *     ⇒ 严禁在 init 里用 ESP_ERROR_CHECK —— 那会让"没插卡"变成开不了机。
 *
 * ===========================================================================
 *  ★ 落盘格式（见 .c 的 FDB_HDR_SIZE / FDB_ENT_SIZE 静态断言）
 * ===========================================================================
 *  /sdcard/face_db/
 *    ├── persons.idx.0     ← 索引世代 0
 *    ├── persons.idx.1     ← 索引世代 1
 *    ├── feats.bin         ← 特征拼接区（**追加式**，按索引里的 off/len 定位）
 *    └── thumbs/01.jpg     ← 每人一张底片，供库管理页列表显示
 *
 *  **★ 为什么索引要写两份（双份世代号）**：
 *  FATFS 的 `rename()` **不是原子操作**（无日志），"写临时文件再 rename"这套经典做法
 *  在掉电时可能**两个名字都没了**。所以改成：
 *    读 → 取 `idx.0`/`idx.1` 中 **CRC 通过且 seq 更大**的那份；
 *    写 → **永远写"较旧的那份"**，seq = 当前有效 seq + 1。
 *  任一时刻**至少有一份完整**，最坏只丢最近一次改动。十人规模一次写 ≈ 400 B，
 *  写两份的开销可忽略。
 *
 *  **★ 为什么 feats.bin 追加而不紧凑化**：
 *  删除一条 = 改索引 + 让 feats.bin 里那段落成**孤儿**（不回收）。
 *  10 人 × 512 × 4 B = 20 KB，空洞浪费无所谓；换来的是"删除不重写整个文件"，
 *  掉电窗口从"重写 20 KB"缩到"只改 400 B 索引"。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  容量与尺寸约束
 * ------------------------------------------------------------------------- */
/** ★ 录入人数硬上限（需求 R9：< 10 人）。第 11 人会**明确拒绝**（ESP_ERR_NO_MEM），
 *  不是静默丢弃 —— 验收项之一。 */
#define FACE_DB_MAX         10
/** 姓名缓冲（UTF-8 字节）：6 汉字 = 18 B + NUL + 余量。
 *  ⚠ 是**字节数**不是字符数：`strlen("张三") == 6`。截断按 UTF-8 序列边界处理，
 *  绝不切半个汉字（见 .c 的 fdb_name_sanitize）。 */
#define FACE_NAME_MAX       24
/** 特征维度上限。MFN = 512；留宏是为了换模型时不改结构。 */
#define FACE_DB_DIM_MAX     512
/** 路径缓冲（含 NUL）。最坏 = "/sdcard"(7) + "/face_db"(8) + "/thumbs/01.jpg"(15) = 30，
 *  取 128 留足余量；.c 里有 _Static_assert 把"最坏输出 < 容量"锁在编译期。 */
#define FACE_DB_PATH_MAX    128

/** 访客（未识别）的**保留 id**，不参与分配、永不被 enroll 占用。
 *  打卡落到 `99_访客/`，与真人记录物理隔离，避免"陌生人混进张三的文件夹"。 */
#define FACE_DB_GUEST_ID    0
#define FACE_DB_GUEST_NAME  "访客"

/** SD 卡上的库根目录（相对 SDCARD_MNT_PATH） */
#define FACE_DB_DIR_DEF     "/face_db"

/* ---------------------------------------------------------------------------
 *  数据结构
 * ------------------------------------------------------------------------- */
/** 一条人员记录（**内存视图**；落盘是紧凑序列化，不直接 memcpy 本结构 ——
 *  避免对齐/padding 在不同编译器下漂移，见 .c 的 fdb_serialize_entry）。 */
typedef struct {
    uint16_t id;                 /* 1..FACE_DB_MAX；★ 一旦分配**永不复用** */
    char     name[FACE_NAME_MAX];/* UTF-8，已做文件名安全化 */
    uint32_t feat_off;           /* 在 feats.bin 中的字节偏移 */
    uint32_t feat_len;           /* 特征字节数（= dim * sizeof(float)） */
    uint32_t enroll_epoch;       /* 录入时刻（真实 epoch；未校时写 0 而不是 1970） */
    uint8_t  sample_cnt;         /* 录入时采了几张（质量参考，S8-1 为 3） */
    uint8_t  _rsv[3];            /* 显式留白：让结构体尺寸不随编译器变化 */
} face_person_t;

/** 库状态快照（供 UI 显示 "N/10"、置灰按钮） */
typedef struct {
    bool ready;                  /* SD 已挂载 + 索引至少一份解析成功 */
    int  count;                  /* 已录人数（0..FACE_DB_MAX） */
    int  feat_dim;               /* 库内特征的维度（空库时 = 0） */
    int  free_slots;             /* 剩余名额 = FACE_DB_MAX - count */
} face_db_info_t;

/** 查库结果（§5.3 的比对输出） */
typedef struct {
    bool         matched;        /* score >= 阈值 */
    face_person_t person;        /* 命中者；未命中时 id = FACE_DB_GUEST_ID */
    float        score;          /* 余弦相似度 [-1, 1] */
} face_db_match_t;

/* ---------------------------------------------------------------------------
 *  生命周期
 * ------------------------------------------------------------------------- */
/**
 * @brief 初始化：挂载点下建目录 + 读索引到 RAM（10 人 ≈ 400 B）
 * @note  **失败不致命**（红线）：SD 没插就返回错误，调用方照常跑人脸检测。
 *        幂等：重复调用直接返回上次结果。
 */
esp_err_t face_db_init(void);

/**
 * @brief 用**指定根目录**初始化（自检 / 单元测试用）
 *
 * ★ 为什么要有这个入口：S8-1 的验收需要"录入 3 人 → 重初始化 → 数据仍在"这套动作，
 *   而它必须能在**不污染真库**的前提下跑。让根目录可注入，自检就能用
 *   `/sdcard/face_db_selftest` 独立演一遍，跑完连文件一起删干净。
 *   ⚠ 会先 deinit 掉当前实例（同一时刻只允许一个库实例）。
 */
esp_err_t face_db_init_at(const char *root);

/** 卸载：释放 RAM 索引与互斥锁。再次 init 会重新从盘加载。 */
esp_err_t face_db_deinit(void);

/** 库是否可用（SD 已挂载且索引已加载）。任意线程可调。 */
bool face_db_is_ready(void);

/** 取状态快照 */
void face_db_get_info(face_db_info_t *out);

/** 已录人数（未就绪返回 0） */
int face_db_count(void);

/* ---------------------------------------------------------------------------
 *  查询
 * ------------------------------------------------------------------------- */
/**
 * @brief 按槽位读一条（**拷贝到调用方缓冲**）
 *
 * ★ 与方案 §5.2 的一处**有意偏离**：方案原签名返回 `const face_person_t *`（内部指针）。
 *   但那样调用方在锁外持有内部数组的指针，一次 enroll/delete 就可能读到**半更新**的记录
 *   （名字已改、feat_off 还是旧的）。本工程 10 人、一条 ≈ 40 B，拷贝开销可忽略
 *   ⇒ 改成"拷出去"，从 API 形状上消除竞态，而不是要求每个调用方自己加锁。
 *
 * @return ESP_OK；ESP_ERR_NOT_FOUND 槽位空 / 越界；ESP_ERR_INVALID_STATE 库未就绪
 */
esp_err_t face_db_get(int idx, face_person_t *out);

/** 按 id 查（拷贝语义同上） */
esp_err_t face_db_find_by_id(uint16_t id, face_person_t *out);

/** 按姓名查（全等匹配，大小写敏感 —— 中文名不涉及大小写） */
esp_err_t face_db_find_by_name(const char *name, face_person_t *out);

/**
 * @brief 读某人的特征向量
 * @param dim 调用方缓冲的容量（float 个数）；库内维度与之不符返回 ESP_ERR_INVALID_SIZE
 */
esp_err_t face_db_get_feat(const face_person_t *p, float *out, int dim);

/**
 * @brief 在库内逐条算余弦相似度，取最大值（S8-2 识别的核心一步）
 *
 * @param feat   待查特征（已 L2 归一化；未归一化也能算，函数内部会归一）
 * @param dim    特征维度
 * @param thr    命中阈值；`score < thr` 时 matched=false（person_id 语义上是访客）
 * @param out    结果（不能为 NULL）
 * @return 命中的**槽位号**（0..count-1）；未命中 / 空库 / 库未就绪返回 -1
 */
int face_db_find_best(const float *feat, int dim, float thr, face_db_match_t *out);

/**
 * @brief 批量拷出所有特征（供未来的加速比对 / 导出用）
 * @param out      目标缓冲，按 `stride` 个 float 一条**紧密排列**
 * @param stride   每条占多少个 float（应 ≥ 库内维度）
 * @param ids      输出对应 id（可为 NULL）
 * @return 实际拷出的条数
 */
int face_db_copy_feats(float *out, int stride, uint16_t *ids, int max_persons);

/* ---------------------------------------------------------------------------
 *  写入
 * ------------------------------------------------------------------------- */
/**
 * @brief 录入一个人（特征由调用方算好）
 *
 * @param name        点名（UTF-8）；空串 / 全非法字符返回 ESP_ERR_INVALID_ARG
 * @param feat        特征向量（`dim` 个 float）
 * @param dim         维度（1..FACE_DB_DIM_MAX）
 * @param sample_cnt  本次录了几张（质量参考）
 * @param out_id      输出分配到的 id（可为 NULL）
 *
 * @return ESP_OK                  成功
 *         ESP_ERR_NO_MEM          已满 10 人（★ R9 的拒绝路径，UI 要提示"库已满"）
 *         ESP_ERR_INVALID_STATE   重名 / 库未就绪
 *         ESP_ERR_INVALID_ARG     name 或 feat/dim 非法
 *         ESP_FAIL                落盘失败（内存已回滚，库内容不变）
 *
 * ★ 写入顺序是刻意的：**先把特征追加进 feats.bin，再写索引**。
 *   反过来会出现"索引指向一个还不存在的特征"的悬垂状态 —— 掉电就得到一个
 *   永远读不出特征的库。现在的顺序最坏只丢一条孤儿特征（无害）。
 */
esp_err_t face_db_enroll(const char *name, const float *feat, int dim,
                         uint8_t sample_cnt, uint16_t *out_id);

/** 删除一个人（id 不复用；feats.bin 里那段落成孤儿，不紧凑化） */
esp_err_t face_db_delete(uint16_t id);

/**
 * @brief 改名
 * ★ **不改打卡目录名**（有意为之）：目录名是 `<序号>_<姓名>`，序号才是稳定标识，
 *   姓名只用于显示。重命名一个可能装着几百张照片的目录在 FATFS 上既非原子
 *   （断电风险）又无实际收益 ⇒ 改名只改索引，旧目录名保留。
 */
esp_err_t face_db_rename(uint16_t id, const char *new_name);

/** 写入/覆盖某人的底片（JPEG 原始字节，不解码） */
esp_err_t face_db_thumb_write(uint16_t id, const void *jpeg, size_t len);

/** 取底片路径（供 UI 显示用）。返回 ESP_ERR_NOT_FOUND 表示文件不存在。 */
esp_err_t face_db_thumb_path(uint16_t id, char *out, size_t out_len);

/* ---------------------------------------------------------------------------
 *  打卡目录名（与 punch_service 的唯一耦合点）
 * ------------------------------------------------------------------------- */
/**
 * @brief 生成打卡专属目录名 `<序号:2位>_<姓名安全化>`
 *
 *   face_db_attendance_dir(1, "张三", buf)  -> "01_张三"
 *   face_db_attendance_dir(FACE_DB_GUEST_ID, NULL, buf) -> "99_访客"
 *
 * ★ 序号前缀不是装饰：纯中文名在 FATFS 里按**字节序**排，顺序会乱得没法看；
 *   带 2 位序号后目录顺序与录入顺序一致（方案 §4.1）。
 *
 * @param name 可为 NULL（路人：只用 id 决定序号）
 */
esp_err_t face_db_attendance_dir(uint16_t id, const char *name, char *out, size_t out_len);

/** 文件名安全化：把 FATFS 非法字符与控制字符替换为 '_'（长度按 UTF-8 边界截断）。
 *  暴露出来是因为 punch_service 也可能需要它（例如把 name 直接写进 CSV）。 */
void face_db_sanitize_name(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
