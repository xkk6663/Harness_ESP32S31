/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file face_db.c
 * @brief 中间层：人脸库（S8-1）实现
 *
 * 方案：docs/项目规划/S8_人脸识别打卡系统方案.md §4 / §5.2
 * 接口与设计理由见 face_db.h 的文件头（那里写得比这里全，先读那个）。
 *
 * ===========================================================================
 *  实现要点（按"为什么这么写"排序，不是按代码顺序）
 * ===========================================================================
 *  ① **双份世代号**：FATFS 的 rename 非原子，所以不走"写 tmp 再 rename"，
 *     而是"两份索引交替写、读时取 seq 大的"。任一时刻至少一份完整。
 *
 *  ② **落盘顺序：先特征、后索引**。反过来会出现"索引指向尚不存在的特征"的
 *     悬垂状态，掉电即得到永久读不出特征的库。现在的顺序最坏只丢一条孤儿特征。
 *
 *  ③ **结构体不直接落盘**：`face_person_t` 有编译器 padding（45→44），
 *     跨编译器/版本一变就静静读错。所以手写 40 B 紧凑序列化 + 显式大小断言。
 *
 *  ④ **所有对外输出的路径都先拼进本地 FDB_PATHBUF，再 memcpy 导出**。
 *     原因不是洁癖：本工程开了 `-Werror=format-truncation=`，如果直接
 *     `snprintf(out, out_len, ...)` 而 GCC 内联后能证明 out_len 可能不够，
 *     **整个构建就失败**。本地缓冲容量是编译期已知且够大的，GCC 能证明安全。
 *
 *  ⑤ **锁粒度 = 整个公开 API**。所有对 s_persons / s_count 的读写都在 s_lock 内，
 *     公开查询接口一律"拷贝出去"而不是返回内部指针（见 face_db.h 的说明）。
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdcard_service.h"
#include "face_db.h"

static const char *TAG = "FACE_DB";

/* ---------------------------------------------------------------------------
 *  落盘格式常量（★ 改这里必须同步改 .h 的注释与 §5.2）
 * ------------------------------------------------------------------------- */
#define FDB_MAGIC       "FDB1"
#define FDB_SCHEMA_VER  1u

/** Header 布局（20 B）：
 *    0  magic[4]      "FDB1"
 *    4  schema_ver u16
 *    6  feat_dim   u16
 *    8  count      u8
 *    9  rsv[3]       显式留白（把 seq 顶到 4 对齐，不依赖编译器）
 *   12  seq        u32     世代号，越大越新
 *   16  crc32      u32     计算时本字段先置 0，覆盖 header+entries 全段
 */
#define FDB_HDR_SIZE    20u

/** Entry 布局（40 B，紧凑序列化，非 memcpy 结构体）：
 *    0  id           u16
 *    2  name_len     u8      实际有效字节数（解析时不依赖定长区的 NUL）
 *    3  sample_cnt   u8
 *    4  enroll_epoch u32
 *    8  feat_off     u32
 *   12  feat_len     u32
 *   16  name[24]     定长区，不足补 0
 */
#define FDB_ENT_SIZE    40u

/** 索引文件最大字节数（= 3 个世代都不该超过它；超了说明文件坏了或不是我们的格式） */
#define FDB_IDX_MAX     (FDB_HDR_SIZE + FACE_DB_MAX * FDB_ENT_SIZE)   /* 420 */

/** 内部路径缓冲。★ 必须 ≥ (FACE_DB_PATH_MAX-1) + 最长文件名 + NUL：
 *    127 + "/thumbs/10.jpg"(15) + 1 = 143 ⇒ 取 256 有充足余量，
 *    让 GCC 在编译期就能**证明**所有 snprintf 不截断（见实现要点④）。 */
#define FDB_PATHBUF     256

/* 把"最坏输出 < 容量"锁在编译期（本工程的既有惯例，见 capture_service.c） */
_Static_assert(FDB_ENT_SIZE == 40u, "entry layout drifted");
_Static_assert(FDB_HDR_SIZE == 20u, "header layout drifted");
_Static_assert(FDB_PATHBUF > FACE_DB_PATH_MAX + 32, "path buffer too tight");
_Static_assert(FACE_DB_MAX <= 99, "2-digit seq prefix requires count<=99");

/* ---------------------------------------------------------------------------
 *  模块状态
 * ------------------------------------------------------------------------- */
static bool              s_ready;
static SemaphoreHandle_t s_lock;
static face_person_t     s_persons[FACE_DB_MAX];
static int               s_count;
static int               s_dim;                 /* 库内特征维度（空库时 0） */
static uint16_t          s_next_id;             /* 下一个待分配 id（永不复用） */
static char              s_root[FACE_DB_PATH_MAX];

/* 两个索引世代的状态（读成功后置位；写时挑"较旧"的那个覆盖） */
static bool              s_slot_ok[2];
static uint32_t          s_slot_seq[2];

#define FDB_IDX0_NAME   "persons.idx.0"
#define FDB_IDX1_NAME   "persons.idx.1"
#define FDB_FEATS_NAME  "feats.bin"
#define FDB_THUMB_DIR   "thumbs"

/* ---------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------- */
static inline void fdb_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t fdb_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void fdb_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t fdb_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** 把内部拼好的路径拷给调用方；容量不足**明确失败**，不静默截断出一个错路径。 */
static esp_err_t fdb_copy_out(const char *src, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(src);
    if (n + 1 > out_len) {
        out[0] = '\0';
        ESP_LOGE(TAG, "out buffer too small (%u < %u)", (unsigned)out_len, (unsigned)(n + 1));
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, src, n + 1);
    return ESP_OK;
}

/** 拼 `<root>/<name>[-/<sub>]`。root 非空时才加分隔符。 */
static esp_err_t fdb_build_path(const char *name, const char *sub,
                                char *out, size_t out_len)
{
    int n;
    if (sub != NULL) {
        n = snprintf(out, out_len, "%s/%s/%s", s_root, name, sub);
    } else {
        n = snprintf(out, out_len, "%s/%s", s_root, name);
    }
    if (n < 0 || (size_t)n >= out_len) {
        ESP_LOGE(TAG, "path build overflow (need %d, cap %u)", n, (unsigned)out_len);
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/** 确保 `<root>/<name>` 目录存在。EEXIST 是正常情况。 */
static esp_err_t fdb_mkdir_under(const char *name)
{
    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(name, NULL, path, sizeof(path));
    if (e != ESP_OK) {
        return e;
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed, errno=%d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  姓名安全化
 * ------------------------------------------------------------------------- */
void face_db_sanitize_name(const char *in, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }

    size_t oi = 0;
    const unsigned char *p = (const unsigned char *)in;

    while (*p != 0) {
        /* ★ UTF-8 序列长度先判出来，再决定"这个字符能不能整体放下"。
         *   绝不允许在序列中间截断 —— 切半个汉字在 FATFS 里就是一个非法名，
         *   UI 上也显示成乱码方块。宁可少一个字，不要半个字。 */
        size_t seq = 1;
        if ((*p & 0x80u) == 0)          seq = 1;
        else if ((*p & 0xE0u) == 0xC0u) seq = 2;
        else if ((*p & 0xF0u) == 0xE0u) seq = 3;
        else if ((*p & 0xF8u) == 0xF0u) seq = 4;
        else                            seq = 1;   /* 非法首字节：当单字节处理（多半被替换） */

        if (oi + seq + 1 > out_len) {
            break;                                  /* 整体放不下 ⇒ 整字丢弃 */
        }

        if (seq == 1) {
            unsigned char c = *p;
            /* FATFS 非法字符（\ / : * ? " < > |）+ 控制字符 + DEL */
            bool bad = (c < 0x20u) || (c == 0x7Fu) ||
                       c == (unsigned char)'\\' || c == (unsigned char)'/' ||
                       c == (unsigned char)':'  || c == (unsigned char)'*' ||
                       c == (unsigned char)'?'  || c == (unsigned char)'"' ||
                       c == (unsigned char)'<'  || c == (unsigned char)'>' ||
                       c == (unsigned char)'|';
            out[oi++] = bad ? '_' : (char)c;
            p += 1;
        } else {
            memcpy(out + oi, p, seq);
            oi += seq;
            p += seq;
        }
    }
    out[oi] = '\0';
}

esp_err_t face_db_attendance_dir(uint16_t id, const char *name, char *out, size_t out_len)
{
    char nm[FACE_NAME_MAX];
    unsigned seq;

    if (id == FACE_DB_GUEST_ID) {
        face_db_sanitize_name(FACE_DB_GUEST_NAME, nm, sizeof(nm));
        seq = 99u;                                  /* 访客固定 99，不参与 1..10 分配 */
    } else {
        if (name == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        face_db_sanitize_name(name, nm, sizeof(nm));
        /* clamp 到 1..99：既保证 "%02u" 最坏只有 2 字符（GCC format-truncation 友好），
         * 也保证目录排序稳定 */
        if (id > 99u)      seq = 99u;
        else if (id == 0u) seq = 99u;               /* id 0 只有访客，上面已处理；防御 */
        else               seq = id;
    }

    if (nm[0] == '\0') {
        /* 名字全被安全化成空（例如输入全是 "/"）⇒ 用 id 兜底，不要生成 "01_" 这种怪名 */
        snprintf(nm, sizeof(nm), "id%u", (unsigned)seq);
    }

    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%02u_%s", seq, nm);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return ESP_FAIL;
    }
    return fdb_copy_out(tmp, out, out_len);
}

/* ---------------------------------------------------------------------------
 *  索引序列化
 * ------------------------------------------------------------------------- */
static void fdb_serialize_entry(uint8_t *dst, const face_person_t *e, size_t name_len)
{
    fdb_put_u16(dst + 0, e->id);
    dst[2] = (uint8_t)name_len;
    dst[3] = e->sample_cnt;
    fdb_put_u32(dst + 4, e->enroll_epoch);
    fdb_put_u32(dst + 8, e->feat_off);
    fdb_put_u32(dst + 12, e->feat_len);
    memcpy(dst + 16, e->name, FACE_NAME_MAX);       /* 定长区，不足部分是 0 */
}

static void fdb_deserialize_entry(const uint8_t *src, face_person_t *e)
{
    memset(e, 0, sizeof(*e));
    e->id           = fdb_get_u16(src + 0);
    uint8_t nlen    = src[2];
    if (nlen > FACE_NAME_MAX) {
        nlen = FACE_NAME_MAX;                       /* 防御：损坏数据不得越界 */
    }
    e->sample_cnt   = src[3];
    e->enroll_epoch = fdb_get_u32(src + 4);
    e->feat_off     = fdb_get_u32(src + 8);
    e->feat_len     = fdb_get_u32(src + 12);
    memcpy(e->name, src + 16, FACE_NAME_MAX);
    e->name[FACE_NAME_MAX - 1] = '\0';              /* 强制 NUL 结尾，不信任盘上数据 */
}

/** 组一份索引镜像（header + entries），并把 crc32 填好。 */
static size_t fdb_build_image(uint8_t *buf, size_t buf_len, uint32_t seq)
{
    memset(buf, 0, buf_len);
    memcpy(buf, FDB_MAGIC, 4);
    fdb_put_u16(buf + 4, (uint16_t)FDB_SCHEMA_VER);
    fdb_put_u16(buf + 6, (uint16_t)s_dim);
    buf[8] = (uint8_t)s_count;

    for (int i = 0; i < s_count; i++) {
        size_t nlen = strnlen(s_persons[i].name, FACE_NAME_MAX);
        fdb_serialize_entry(buf + FDB_HDR_SIZE + (size_t)i * FDB_ENT_SIZE,
                            &s_persons[i], nlen);
    }

    size_t total = FDB_HDR_SIZE + (size_t)s_count * FDB_ENT_SIZE;
    fdb_put_u32(buf + 12, seq);
    fdb_put_u32(buf + 16, 0);                       /* crc 字段先清零再算 */
    uint32_t crc = esp_rom_crc32_le(0, buf, (uint32_t)total);
    fdb_put_u32(buf + 16, crc);
    return total;
}

static esp_err_t fdb_idx_write(int slot, uint32_t seq)
{
    /* 420 B 走栈：调用链上最深的任务栈是 10 KB 级（lvgl / cam_task），安全。
     * 不用 PSRAM 是因为这点量分配开销比拷贝还大，而且频繁 malloc 正是碎片来源。 */
    uint8_t buf[FDB_IDX_MAX];

    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(slot == 0 ? FDB_IDX0_NAME : FDB_IDX1_NAME,
                                 NULL, path, sizeof(path));
    if (e != ESP_OK) {
        return e;
    }

    size_t total = fdb_build_image(buf, sizeof(buf), seq);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(w) %s failed, errno=%d", path, errno);
        return ESP_FAIL;
    }
    size_t w = fwrite(buf, 1, total, f);
    int fe = ferror(f);
    /* ★ 先 fclose 再判错：不 close 就 return 会漏文件句柄，几十次之后
     *   FATFS 的 FIL 池就满了，表现成"突然所有写盘都失败"。 */
    if (fclose(f) != 0 || w != total || fe) {
        ESP_LOGE(TAG, "write %s failed (wrote %u/%u)", path, (unsigned)w, (unsigned)total);
        return ESP_FAIL;
    }

    s_slot_ok[slot]  = true;
    s_slot_seq[slot] = seq;
    ESP_LOGI(TAG, "idx slot %d written: seq=%u count=%d dim=%d (%u B)",
             slot, (unsigned)seq, s_count, s_dim, (unsigned)total);
    return ESP_OK;
}

static esp_err_t fdb_idx_read(const char *name, uint32_t *seq_out, int *dim_out,
                              int *count_out, face_person_t *arr, int arr_max)
{
    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(name, NULL, path, sizeof(path));
    if (e != ESP_OK) {
        return e;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;                   /* 首次开机很正常，不当错误打日志 */
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < (long)FDB_HDR_SIZE || sz > (long)FDB_IDX_MAX) {
        ESP_LOGW(TAG, "%s: bad size %ld (expect %u..%u)", name, sz,
                 (unsigned)FDB_HDR_SIZE, (unsigned)FDB_IDX_MAX);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    uint8_t buf[FDB_IDX_MAX];
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        ESP_LOGW(TAG, "%s: short read %u/%ld", name, (unsigned)got, sz);
        return ESP_FAIL;
    }

    if (memcmp(buf, FDB_MAGIC, 4) != 0) {
        ESP_LOGW(TAG, "%s: bad magic", name);
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t ver = fdb_get_u16(buf + 4);
    if (ver != FDB_SCHEMA_VER) {
        ESP_LOGW(TAG, "%s: schema %u != %u", name, ver, FDB_SCHEMA_VER);
        return ESP_ERR_INVALID_VERSION;
    }

    uint32_t seq   = fdb_get_u32(buf + 12);
    uint32_t crc   = fdb_get_u32(buf + 16);
    fdb_put_u32(buf + 16, 0);
    uint32_t calc  = esp_rom_crc32_le(0, buf, (uint32_t)sz);
    if (calc != crc) {
        /* ★ CRC 不通过**不是致命错误**：这正是双份世代号存在的意义 ——
         *   另一份大概率是好的，交给上层去挑。这里只警告。 */
        ESP_LOGW(TAG, "%s: crc mismatch (file=%08x calc=%08x)", name, (unsigned)crc, (unsigned)calc);
        return ESP_ERR_INVALID_CRC;
    }

    int dim   = (int)fdb_get_u16(buf + 6);
    int count = (int)buf[8];
    if (count < 0 || count > FACE_DB_MAX || (int)sz != (int)(FDB_HDR_SIZE + (size_t)count * FDB_ENT_SIZE)) {
        ESP_LOGW(TAG, "%s: count/size inconsistent (%d, %ld)", name, count, sz);
        return ESP_ERR_INVALID_SIZE;
    }
    /* ★★★ 2026-09-28 修（BUG：删光人物后重启，人又"复活"）★★★
     *
     * 成因链（每一环都单看都合理，合起来才出事）：
     *   ① face_db_delete() 删到**最后一个人**时把 s_dim 清成 0（空库没有维度可言）；
     *   ② fdb_build_image() 把这个 0 原样写进索引 header 的 dim 字段；
     *   ③ **本函数原来无条件要求 `dim > 0`** ⇒ 于是"空库"那份索引被判定为**坏档**；
     *   ④ fdb_load_best() 见该槽 ok=false，就回落到**另一份世代**（上一个 seq、
     *      还带着刚被删掉的那个人）⇒ 重启后人物原地复活。
     *
     * 正确语义：**count == 0 时空库是合法状态，dim 无意义、不参与校验**；
     * 只有"确实有记录却不知道维度"（count>0 && dim 非法）才是真损坏。
     *
     * ★ 这条判据同时是"删除后**只删了一部分**时不会复活、只有删空才复活"的原因 ——
     *   因为 s_dim 仅在 count 归零那一刻被清（见 face_db_delete()）。 */
    if (count > 0 && (dim <= 0 || dim > FACE_DB_DIM_MAX)) {
        ESP_LOGW(TAG, "%s: bad dim %d (count=%d)", name, dim, count);
        return ESP_ERR_INVALID_SIZE;
    }
    if (count == 0) {
        ESP_LOGI(TAG, "%s: empty library (sq=%u) - legal state, not corruption",
                 name, (unsigned)seq);
    }

    for (int i = 0; i < count; i++) {
        fdb_deserialize_entry(buf + FDB_HDR_SIZE + (size_t)i * FDB_ENT_SIZE, &arr[i]);
        if (arr[i].id == 0 || arr[i].id > FACE_DB_MAX) {
            ESP_LOGW(TAG, "%s: entry %d bad id %u", name, i, arr[i].id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    *seq_out   = seq;
    *dim_out   = dim;
    *count_out = count;
    return ESP_OK;
}

/** 从两份世代里挑最新可用的一份，装进 s_persons。
 *  返回是否有可用数据。 */
static bool fdb_load_best(void)
{
    face_person_t a0[FACE_DB_MAX], a1[FACE_DB_MAX];
    int c0 = 0, c1 = 0, d0 = 0, d1 = 0;
    uint32_t s0 = 0, s1 = 0;

    s_slot_ok[0] = (fdb_idx_read(FDB_IDX0_NAME, &s0, &d0, &c0, a0, FACE_DB_MAX) == ESP_OK);
    s_slot_ok[1] = (fdb_idx_read(FDB_IDX1_NAME, &s1, &d1, &c1, a1, FACE_DB_MAX) == ESP_OK);
    s_slot_seq[0] = s_slot_ok[0] ? s0 : 0;
    s_slot_seq[1] = s_slot_ok[1] ? s1 : 0;

    ESP_LOGI(TAG, "idx scan: slot0=%s(seq=%u) slot1=%s(seq=%u)",
             s_slot_ok[0] ? "ok" : "bad", (unsigned)s_slot_seq[0],
             s_slot_ok[1] ? "ok" : "bad", (unsigned)s_slot_seq[1]);

    if (!s_slot_ok[0] && !s_slot_ok[1]) {
        return false;                               /* 空库：不是错误 */
    }

    bool use0;
    if (s_slot_ok[0] && s_slot_ok[1]) {
        use0 = (s_slot_seq[0] >= s_slot_seq[1]);    /* 相等时任选，取 0 */
    } else {
        use0 = s_slot_ok[0];
    }

    if (use0) {
        s_count = c0; s_dim = d0;
        memcpy(s_persons, a0, sizeof(face_person_t) * (size_t)c0);
    } else {
        s_count = c1; s_dim = d1;
        memcpy(s_persons, a1, sizeof(face_person_t) * (size_t)c1);
    }
    ESP_LOGI(TAG, "loaded from slot %d: count=%d dim=%d", use0 ? 0 : 1, s_count, s_dim);
    return true;
}

/** 选"较旧"的槽位写（两份都有效时）；只有一份有效时写另一份。 */
static int fdb_pick_write_slot(void)
{
    if (!s_slot_ok[0]) {
        return 0;
    }
    if (!s_slot_ok[1]) {
        return 1;
    }
    return (s_slot_seq[0] <= s_slot_seq[1]) ? 0 : 1;
}

static uint32_t fdb_next_seq(void)
{
    uint32_t m = 0;
    if (s_slot_ok[0] && s_slot_seq[0] > m) m = s_slot_seq[0];
    if (s_slot_ok[1] && s_slot_seq[1] > m) m = s_slot_seq[1];
    return m + 1;
}

/** 把当前内存视图写进盘（挑槽位 + 递增 seq）。所有写路径都走它。 */
static esp_err_t fdb_commit(void)
{
    int slot = fdb_pick_write_slot();
    esp_err_t e = fdb_idx_write(slot, fdb_next_seq());
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "commit failed (slot %d)", slot);
        return e;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  特征文件
 * ------------------------------------------------------------------------- */
static esp_err_t fdb_feat_append(const float *feat, int dim, uint32_t *off_out)
{
    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(FDB_FEATS_NAME, NULL, path, sizeof(path));
    if (e != ESP_OK) {
        return e;
    }

    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(a) %s failed, errno=%d", path, errno);
        return ESP_FAIL;
    }

    /* ★ "ab" 模式下不能只靠 ftell() 拿偏移：某些 newlib 实现里 append 模式的
     *   初始位置不保证等于文件长度。显式 fseek(SEEK_END) 一次，行为确定。 */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long off = ftell(f);
    if (off < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    size_t bytes = (size_t)dim * sizeof(float);
    size_t w = fwrite(feat, 1, bytes, f);
    int fe = ferror(f);
    /* 特征先落盘再写索引（实现要点②）：必须确保这里真的写下去了 */
    if (fclose(f) != 0 || w != bytes || fe) {
        ESP_LOGE(TAG, "feat append failed (wrote %u/%u)", (unsigned)w, (unsigned)bytes);
        return ESP_FAIL;
    }

    *off_out = (uint32_t)off;
    return ESP_OK;
}

/** 读第 idx 条记录的特征到 out（out 容量 dim 个 float）。调用方需持锁。 */
static esp_err_t fdb_feat_read_locked(int idx, float *out, int dim)
{
    if (idx < 0 || idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const face_person_t *p = &s_persons[idx];
    if (dim != s_dim || p->feat_len != (uint32_t)s_dim * sizeof(float)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(FDB_FEATS_NAME, NULL, path, sizeof(path));
    if (e != ESP_OK) {
        return e;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(r) %s failed, errno=%d", path, errno);
        return ESP_FAIL;
    }
    if (fseek(f, (long)p->feat_off, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t got = fread(out, sizeof(float), (size_t)dim, f);
    fclose(f);
    if (got != (size_t)dim) {
        ESP_LOGE(TAG, "feat read short (%u/%d) @%u", (unsigned)got, dim, (unsigned)p->feat_off);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 余弦相似度。内部用 double 累加：512 维 float 累加在 32 位上会掉精度。 */
static float fdb_cos_sim(const float *a, const float *b, int dim)
{
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < dim; i++) {
        dot += (double)a[i] * (double)b[i];
        na  += (double)a[i] * (double)a[i];
        nb  += (double)b[i] * (double)b[i];
    }
    if (na <= 0.0 || nb <= 0.0) {
        return 0.0f;                    /* 零向量：定义成不相似，而不是 NaN */
    }
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

/* ---------------------------------------------------------------------------
 *  生命周期
 * ------------------------------------------------------------------------- */
static esp_err_t fdb_ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t face_db_init(void)
{
    return face_db_init_at(SDCARD_MNT_PATH FACE_DB_DIR_DEF);
}

esp_err_t face_db_init_at(const char *root)
{
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(root) + 1 > sizeof(s_root)) {
        ESP_LOGE(TAG, "root too long: %s", root);
        return ESP_ERR_INVALID_ARG;
    }

    /* 已就绪且根目录没变 ⇒ 幂等返回 */
    if (s_ready && strcmp(s_root, root) == 0) {
        return ESP_OK;
    }
    if (s_ready) {
        /* 换根：先卸载，避免两套状态并存 */
        face_db_deinit();
    }

    esp_err_t e = fdb_ensure_lock();
    if (e != ESP_OK) {
        return e;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_persons, 0, sizeof(s_persons));
    s_count   = 0;
    s_dim     = 0;
    s_next_id = 1;
    s_slot_ok[0] = s_slot_ok[1] = false;
    s_slot_seq[0] = s_slot_seq[1] = 0;
    memcpy(s_root, root, strlen(root) + 1);
    xSemaphoreGive(s_lock);

    /* ★ 挂载点必须已经在（由 app 层先调 sdcard_service_mount）。
     *   这里只在挂载点下建库目录；挂载点本身不存在说明卡没插 ⇒ 直接降级，
     *   不尝试创建 "/sdcard"（那会在内部 flash 上建出一个同名目录，后患无穷）。 */
    char mp[FDB_PATHBUF];
    int n = snprintf(mp, sizeof(mp), "%s", SDCARD_MNT_PATH);
    if (n < 0 || (size_t)n >= sizeof(mp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat st;
    if (stat(mp, &st) != 0) {
        ESP_LOGW(TAG, "mount point %s absent (SD not inserted?) -> face db disabled", mp);
        return ESP_ERR_NOT_FOUND;
    }

    /* ① 库根目录（s_root 已经含挂载点前缀，所以直接 mkdir，不走 fdb_mkdir_under）
     * ② 底片子目录 */
    if (mkdir(s_root, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed, errno=%d", s_root, errno);
        return ESP_FAIL;
    }
    if (fdb_mkdir_under(FDB_THUMB_DIR) != ESP_OK) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = fdb_load_best();
    if (have) {
        /* next_id 取 max(id)+1：id 永不复用，删除后也不回收 */
        uint16_t mx = 0;
        for (int i = 0; i < s_count; i++) {
            if (s_persons[i].id > mx) {
                mx = s_persons[i].id;
            }
        }
        s_next_id = (uint16_t)(mx + 1);
    }
    s_ready = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "ready: root=%s count=%d/%d dim=%d next_id=%u",
             s_root, s_count, FACE_DB_MAX, s_dim, (unsigned)s_next_id);
    return ESP_OK;
}

esp_err_t face_db_deinit(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_ready = false;
    s_count = 0;
    s_dim   = 0;
    s_slot_ok[0] = s_slot_ok[1] = false;
    memset(s_persons, 0, sizeof(s_persons));
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    return ESP_OK;
}

bool face_db_is_ready(void)
{
    return s_ready;
}

void face_db_get_info(face_db_info_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->ready      = s_ready;
    out->count      = s_count;
    out->feat_dim   = s_dim;
    out->free_slots = s_ready ? (FACE_DB_MAX - s_count) : 0;
    xSemaphoreGive(s_lock);
}

int face_db_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int c = s_ready ? s_count : 0;
    xSemaphoreGive(s_lock);
    return c;
}

/* ---------------------------------------------------------------------------
 *  查询
 * ------------------------------------------------------------------------- */
esp_err_t face_db_get(int idx, face_person_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e;
    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
    } else if (idx < 0 || idx >= s_count) {
        e = ESP_ERR_NOT_FOUND;
    } else {
        memcpy(out, &s_persons[idx], sizeof(*out));
        e = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t face_db_find_by_id(uint16_t id, face_person_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
    } else {
        for (int i = 0; i < s_count; i++) {
            if (s_persons[i].id == id) {
                memcpy(out, &s_persons[i], sizeof(*out));
                e = ESP_OK;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t face_db_find_by_name(const char *name, face_person_t *out)
{
    if (name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char nm[FACE_NAME_MAX];
    face_db_sanitize_name(name, nm, sizeof(nm));
    if (nm[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
    } else {
        for (int i = 0; i < s_count; i++) {
            if (strncmp(s_persons[i].name, nm, FACE_NAME_MAX) == 0) {
                memcpy(out, &s_persons[i], sizeof(*out));
                e = ESP_OK;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t face_db_get_feat(const face_person_t *p, float *out, int dim)
{
    if (p == NULL || out == NULL || dim <= 0 || dim > FACE_DB_DIM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 按 id 重新定位槽位：调用方给的 face_person_t 可能是**上一次查询的快照**，
     * 期间可能已被删除或下标漂移。用 id 再查一次，避免"读到别人的特征"。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
    } else {
        for (int i = 0; i < s_count; i++) {
            if (s_persons[i].id == p->id) {
                e = fdb_feat_read_locked(i, out, dim);
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return e;
}

int face_db_find_best(const float *feat, int dim, float thr, face_db_match_t *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->person.id = FACE_DB_GUEST_ID;

    if (feat == NULL || dim <= 0 || dim > FACE_DB_DIM_MAX) {
        return -1;
    }
    if (s_lock == NULL) {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready || s_count == 0 || dim != s_dim) {
        xSemaphoreGive(s_lock);
        return -1;
    }

    /* ★ 逐条从 SD 读特征（10 × 2 KB）。S8-1 的调用频率只有"录入时查重"一次，
     *   这点 I/O 无所谓。**S8-2 每帧识别时若实测成为瓶颈，就在这里把
     *   全部特征一次性缓存到 PSRAM（20 KB）** —— 现在不做，避免过早优化。 */
    float buf[FACE_DB_DIM_MAX];
    int   best = -1;
    float best_score = -1.0f;

    for (int i = 0; i < s_count; i++) {
        if (fdb_feat_read_locked(i, buf, dim) != ESP_OK) {
            ESP_LOGW(TAG, "skip entry %d: feat unreadable", i);
            continue;
        }
        float sc = fdb_cos_sim(feat, buf, dim);
        if (sc > best_score) {
            best_score = sc;
            best = i;
        }
    }

    if (best >= 0) {
        out->score = best_score;
        if (best_score >= thr) {
            out->matched = true;
            memcpy(&out->person, &s_persons[best], sizeof(out->person));
        }
    }
    xSemaphoreGive(s_lock);
    return best;
}

int face_db_copy_feats(float *out, int stride, uint16_t *ids, int max_persons)
{
    if (out == NULL || stride <= 0 || max_persons <= 0) {
        return 0;
    }
    if (s_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    if (s_ready) {
        for (int i = 0; i < s_count && n < max_persons; i++) {
            if (stride < s_dim) {
                break;                              /* 调用方给的步长装不下一整条 */
            }
            if (fdb_feat_read_locked(i, out + (size_t)n * (size_t)stride, s_dim) != ESP_OK) {
                continue;
            }
            if (ids != NULL) {
                ids[n] = s_persons[i].id;
            }
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ---------------------------------------------------------------------------
 *  写入
 * ------------------------------------------------------------------------- */
esp_err_t face_db_enroll(const char *name, const float *feat, int dim,
                         uint8_t sample_cnt, uint16_t *out_id)
{
    /* ★ 所有声明放函数顶部：本函数用 goto 做统一收尾（解除锁），
     *   而"跳转跨过带初始化的声明"是经典踩坑点（读未初始化值 / -Wjump-misses-init）。
     *   本工程既有惯例，见 capture_service.c 的同款注释。 */
    char          nm[FACE_NAME_MAX];
    face_person_t rec;
    uint32_t      off = 0;
    esp_err_t     e   = ESP_OK;
    time_t        now = 0;
    int           i;

    if (name == NULL || feat == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dim <= 0 || dim > FACE_DB_DIM_MAX) {
        ESP_LOGE(TAG, "bad dim %d", dim);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    face_db_sanitize_name(name, nm, sizeof(nm));
    if (nm[0] == '\0') {
        ESP_LOGW(TAG, "empty name after sanitize");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (s_count >= FACE_DB_MAX) {
        /* ★ R9 的拒绝路径：10 人已满。**明确报错**，不静默覆盖、不挤掉别人。 */
        ESP_LOGW(TAG, "library full (%d/%d)", s_count, FACE_DB_MAX);
        e = ESP_ERR_NO_MEM;
        goto out;
    }
    if (s_count > 0 && dim != s_dim) {
        /* 同一库里混着不同维度的特征，比对时会静默算错 ⇒ 直接拒绝。
         * 换识别模型（dim 变了）必须清库重录，这是有意的。 */
        ESP_LOGE(TAG, "dim mismatch: lib=%d new=%d", s_dim, dim);
        e = ESP_ERR_INVALID_ARG;
        goto out;
    }
    for (i = 0; i < s_count; i++) {
        if (strncmp(s_persons[i].name, nm, FACE_NAME_MAX) == 0) {
            ESP_LOGW(TAG, "duplicate name: %s", nm);
            e = ESP_ERR_INVALID_STATE;
            goto out;
        }
    }

    e = fdb_feat_append(feat, dim, &off);
    if (e != ESP_OK) {
        goto out;
    }

    memset(&rec, 0, sizeof(rec));
    rec.id           = s_next_id;
    rec.feat_off     = off;
    rec.feat_len     = (uint32_t)dim * sizeof(float);
    rec.sample_cnt   = sample_cnt;
    /* 未校时时写 0（**不写 1970**）：0 在语义上是"时间不可信"，
     * 而 1970 会被下游当成一个真实时刻。 */
    now = time(NULL);
    rec.enroll_epoch = (now > 1600000000) ? (uint32_t)now : 0u;
    memcpy(rec.name, nm, sizeof(rec.name));

    s_persons[s_count] = rec;
    s_count++;
    s_dim = dim;

    e = fdb_commit();
    if (e != ESP_OK) {
        /* 回滚内存视图：盘上还是旧的，内存不能比盘新。
         * ★ 只回滚 count，**不回滚 s_next_id** —— id 永不复用的原则高于"省一个号"。 */
        s_count--;
        s_next_id = rec.id;
        ESP_LOGE(TAG, "commit failed, rolled back in-memory entry");
        goto out;
    }
    s_next_id = (uint16_t)(rec.id + 1);

    if (out_id != NULL) {
        *out_id = rec.id;
    }
    ESP_LOGI(TAG, "enrolled: id=%u name=%s dim=%d slots=%d/%d",
             (unsigned)rec.id, nm, dim, s_count, FACE_DB_MAX);

out:
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t face_db_delete(uint16_t id)
{
    char      thumb[FDB_PATHBUF];
    char      file[FDB_PATHBUF];
    esp_err_t e   = ESP_ERR_NOT_FOUND;
    int       at  = -1;
    int       n   = 0;
    int       i;

    if (id == FACE_DB_GUEST_ID) {
        return ESP_ERR_INVALID_ARG;                 /* 访客是保留 id，不可删 */
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
        goto out;
    }

    for (i = 0; i < s_count; i++) {
        if (s_persons[i].id == id) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        goto out;
    }

    /* 后段前移（保持槽位连续）。feats.bin 里那段特征**留成孤儿不回收** ——
     * 回收要重写整个文件，掉电窗口从 400 B 变成 20 KB，不划算（见 face_db.h）。 */
    if (at < s_count - 1) {
        memmove(&s_persons[at], &s_persons[at + 1],
                sizeof(face_person_t) * (size_t)(s_count - at - 1));
    }
    s_count--;
    memset(&s_persons[s_count], 0, sizeof(face_person_t));
    if (s_count == 0) {
        /* 空库：维度信息一并清掉。
         * ★★ 这个 0 会被原样写进索引 header 的 dim 字段 ⇒ 读侧 fdb_idx_read()
         *    **必须**允许 `count == 0 && dim == 0` 的组合作合法（已修）。
         *    曾经因为读侧无条件要求 dim > 0，导致"删空后重启人物复活"，详见
         *    fdb_idx_read() 里那段 ★★★ 注释。改这里之前先看那里。 */
        s_dim = 0;
    }

    e = fdb_commit();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "commit failed on delete id=%u", (unsigned)id);
        goto out;                                   /* 内存已改，盘上没改：下次重启会"复活" */
    }

    /* 底片顺手删掉。删除失败**不影响结果**（索引才是真相），只记一条日志。 */
    if (fdb_build_path(FDB_THUMB_DIR, NULL, thumb, sizeof(thumb)) == ESP_OK) {
        n = snprintf(file, sizeof(file), "%s/%02u.jpg", thumb, (unsigned)id);
        if (n > 0 && (size_t)n < sizeof(file) && unlink(file) != 0) {
            ESP_LOGD(TAG, "thumb unlink %s skipped, errno=%d", file, errno);
        }
    }
    ESP_LOGI(TAG, "deleted id=%u, now %d/%d", (unsigned)id, s_count, FACE_DB_MAX);

out:
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t face_db_rename(uint16_t id, const char *new_name)
{
    char      nm[FACE_NAME_MAX];
    char      old[FACE_NAME_MAX];
    esp_err_t e  = ESP_ERR_NOT_FOUND;
    int       at = -1;
    int       i;

    if (id == FACE_DB_GUEST_ID || new_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    face_db_sanitize_name(new_name, nm, sizeof(nm));
    if (nm[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready) {
        e = ESP_ERR_INVALID_STATE;
        goto out;
    }

    for (i = 0; i < s_count; i++) {
        if (s_persons[i].id == id) {
            at = i;
        } else if (strncmp(s_persons[i].name, nm, FACE_NAME_MAX) == 0) {
            e = ESP_ERR_INVALID_STATE;              /* 与别人重名 */
            goto out;
        }
    }
    if (at < 0) {
        goto out;
    }

    memcpy(old, s_persons[at].name, sizeof(old));
    memset(s_persons[at].name, 0, sizeof(s_persons[at].name));
    memcpy(s_persons[at].name, nm, sizeof(s_persons[at].name));

    e = fdb_commit();
    if (e != ESP_OK) {
        memcpy(s_persons[at].name, old, sizeof(old));   /* 回滚显示名 */
        goto out;
    }
    /* ★ 打卡目录名**不变**（<序号>_<旧名>）—— 见 face_db.h 的说明：
     *   目录里可能已经有几百张照片，FATFS 的 rename 非原子，收益只有"好看"。 */
    ESP_LOGI(TAG, "renamed id=%u: %s -> %s (attendance dir keeps old name)",
             (unsigned)id, old, nm);

out:
    xSemaphoreGive(s_lock);
    return e;
}

/* ---------------------------------------------------------------------------
 *  底片
 * ------------------------------------------------------------------------- */
esp_err_t face_db_thumb_write(uint16_t id, const void *jpeg, size_t len)
{
    if (jpeg == NULL || len == 0 || id == FACE_DB_GUEST_ID) {
        return ESP_ERR_INVALID_ARG;
    }

    char dir[FDB_PATHBUF];
    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(FDB_THUMB_DIR, NULL, dir, sizeof(dir));
    if (e != ESP_OK) {
        return e;
    }
    int n = snprintf(path, sizeof(path), "%s/%02u.jpg", dir, (unsigned)id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(w) %s failed, errno=%d", path, errno);
        return ESP_FAIL;
    }
    size_t w = fwrite(jpeg, 1, len, f);
    int fe = ferror(f);
    if (fclose(f) != 0 || w != len || fe) {
        ESP_LOGE(TAG, "thumb write failed (%u/%u)", (unsigned)w, (unsigned)len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "thumb saved: %s (%u B)", path, (unsigned)len);
    return ESP_OK;
}

esp_err_t face_db_thumb_path(uint16_t id, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char dir[FDB_PATHBUF];
    char path[FDB_PATHBUF];
    esp_err_t e = fdb_build_path(FDB_THUMB_DIR, NULL, dir, sizeof(dir));
    if (e != ESP_OK) {
        return e;
    }
    int n = snprintf(path, sizeof(path), "%s/%02u.jpg", dir, (unsigned)id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return fdb_copy_out(path, out, out_len);
}
