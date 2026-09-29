/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file punch_service.c
 * @brief 打卡事务实现（S8-2 文件名 + CSV / S8-3 像素水印）。设计与取舍见 .h 文件头。
 */

#include "punch_service.h"
#include "jpeg_util.h"

#include "sdcard_service.h"
#include "net_time_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#define TAG "PUNCH"

/* ===========================================================================
 *  ★★ 栈策略（先说清楚，改动前先读）
 *
 *  与 capture_service 完全同款，理由也一样：
 *   · 本工程**内部 RAM 只剩 ~4 KB 余量** ⇒ 刻意**不建常驻任务**；
 *   · 改成"**一次性任务**"：按下/触发才建栈，事务结束（几十~几百 ms）即 vTaskDelete。
 *   · 栈必须在**内部 RAM**（不能 WithCaps 到 PSRAM）：本任务要做
 *     fopen/fwrite/opendir（FATFS → SDMMC → 外设 IO），而 F35 明确：
 *     做文件/存储 IO 的任务栈一旦落在 PSRAM，会在 flash cache 关闭窗口里崩。
 *     （P40 也是这条教训的延续。）
 *  ★ 优先级 4：低于 cam_task(5) 与 taskLVGL —— 打卡是"后台落盘"，
 *    不该和取景器抢时间片。
 * =========================================================================== */
#define PUNCH_TASK_STACK   4096
#define PUNCH_TASK_PRIO    4
#define PUNCH_TASK_CORE    0        /* 与 LVGL/cam 分开核 */

/** JPEG 编码质量（1..100，**越大越好** —— 与 OV3660 的语义相反） */
#define PUNCH_JPEG_Q       80

/* ---- 路径拼接的容量预算（**与 -Werror=format-truncation 直接相关，别随手改小**）----
 * 下面的 _Static_assert 把"最坏输出 < 容量"锁在编译期。 */
#define PUNCH_ROOTBUF      48    /* "/sdcard/attendance" (19)  */
#define PUNCH_SUBBUF       40    /* "01_张三" 最坏 2+1+18 = 21 */
#define PUNCH_BASEBUF      20    /* "20260928_103045" (15) 或 "IMG_0007" (8) */
_Static_assert(PUNCH_PATH_MAX >
                   (PUNCH_ROOTBUF - 1) + 1 +            /* root + '/'                  */
                   (PUNCH_SUBBUF  - 1) + 1 +            /* subdir + '/'                */
                   (PUNCH_BASEBUF - 1) + sizeof("_99.jpg") + 1,
               "PUNCH_PATH_MAX too small: snprintf may truncate (-Werror=format-truncation=)");

/* ★★★ 2026-09-28：`root` 这类**专用小缓冲**的容量，必须用**它们自己**的需求量来锁。
 *
 *  为什么单独列一条：`path_cap_ok(len)` 问的是"这块缓冲够不够装下一条**最长**路径
 *  （PUNCH_PATH_MAX = 160 B）" —— 它**只对 `char x[PUNCH_PATH_MAX]` 有意义**。
 *  曾经在这里写过 `path_cap_ok(sizeof(root))`，于是 `48 >= 160` 恒为 false
 *  ⇒ `!path_cap_ok(...)` 恒为 true ⇒ **每一次打卡都在最早的分支提前返回
 *  ESP_ERR_INVALID_SIZE**（那条路径**故意不打自己的日志**，串口里只看到
 *  `punch FAIL: ESP_ERR_INVALID_SIZE` 与 `punch done in 1 ms`，极难定位）。
 *
 *  ★ 用编译期断言而不是运行时 if：改宏或改挂载点会**编译失败**，比运行时静默返回强。
 *  ★ 需求量 = SDCARD_MNT_PATH + PUNCH_DIR_NAME 两个字面量的展开长度（含 NUL 共 20 B，
 *    实际用 19 B —— 多出的 1 B 是拼接时不需要的那个 NUL，正好当余量）。 */
_Static_assert(PUNCH_ROOTBUF >= sizeof(SDCARD_MNT_PATH) + sizeof(PUNCH_DIR_NAME),
               "PUNCH_ROOTBUF too small to hold SDCARD_MNT_PATH + PUNCH_DIR_NAME");

/* ---------------------------------------------------------------------------
 *  状态
 * ------------------------------------------------------------------------- */
static volatile bool s_busy = false;        /* 有一次性任务在跑（写或清） */
static punch_record_t s_last;               /* 最近一次结论 */

/** 挂起事务（由 begin 填，任务读） */
static bool      s_pend_valid;
static recog_result_t s_pend_who;           /* 只用到 id/name/matched/score */
static net_time_t s_pend_time;

/** 常驻缓冲（PSRAM，第一次真正打卡时分配） */
static uint8_t  *s_frame;                   /* RGB888 240x240x3 = 172,800 B */
static uint8_t  *s_jpeg;                    /* JPEG_UTIL_OUT_MAX = 64 KB */
static int       s_frame_w, s_frame_h;

/** 冷却表：按 person_id 索引（0..FACE_DB_MAX），存**单调时钟**微秒 */
static int64_t   s_cool_us[FACE_DB_MAX + 1];

/** 本次任务是"写一条"还是"清空" */
static volatile bool s_op_clear = false;
/** ★ 2026-09-28：本次任务是"删单条"（值 = 要删的 seq，0 = 不是这个操作）。
 *  与 s_op_clear 互斥使用同一个一次性任务：删单条同样要重写 CSV（阻塞 IO），
 *  复用任务框架就不用再造一套"异步维护操作"的轮子。 */
static volatile int  s_op_del_seq = 0;
/** ★ 2026-09-28：本次任务是"流水-照片对账"（把照片已缺失的行整行删掉）。 */
static volatile bool s_op_sync = false;

/* ---------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------- */

/** 绝对路径（含 NUL）容量是否够。
 *
 *  ★★★ 只对 `char x[PUNCH_PATH_MAX]` 这种**通用路径缓冲**有意义。**不要**拿它去比
 *  `root[PUNCH_ROOTBUF]` / `sub[PUNCH_SUBBUF]` / `base[PUNCH_BASEBUF]` 这类**专用小缓冲**
 *  —— 那些的容量各有各的 `_Static_assert` 担保（见文件头）。语义搞混的后果见 §70：
 *  一次恒真的 `goto finish` 让打卡 100% 失败，而它连一条日志都不留。 */
static bool path_cap_ok(size_t len)
{
    return len >= PUNCH_PATH_MAX;
}

/* ---------------------------------------------------------------------------
 *  ★★★ 路径拼接的两个安全原语（2026-09-28 新增，用来消掉 -Werror=format-truncation）
 *
 *  为什么**不能**写 `snprintf(dst, sizeof(dst), "%s/%s", dir, name)`：
 *    `struct dirent::d_name` 在 newlib 里是 `char[256]` ⇒ GCC 按**最坏情况**算
 *    `47 + 1 + 255 = 303 B > PUNCH_PATH_MAX(160)` ⇒ 直接**编译失败**（本工程
 *    `-Werror`）。这不是"GCC 太保守" —— 真的是编译期无法排除溢出；
 *    而且就算侥幸编过，snprintf 也只是把"未定义行为"降级成"静默截断"。
 *
 *  用"strnlen + memcpy + 补 NUL"同时满足两边：
 *    · 语义显式：**总是 NUL 结尾，超长即截断**。截断后的路径指向一个不存在的
 *      文件 ⇒ 后续 stat()/fopen() 自然失败，是**安全失败**，不是 UB；
 *    · GCC 不报 —— 它分析不了 strnlen 的返回值，也就得不出"可能溢出"的结论。
 *      （这是本工程在 `-Werror=format-truncation=` 下的标准出路。）
 * ------------------------------------------------------------------------- */

/** 以 '/' 为分隔符把 `name` 追加到 `path`（原地改写）。总 NUL 结尾、绝不溢出。
 *  @return 0 成功；-1 表示连分隔符都放不下（此时 `path` 保持原样）。 */
static int path_join(char *path, size_t cap, const char *name)
{
    size_t len = strlen(path);
    if (len + 1 >= cap) return -1;             /* 写 '/' 之前就挡住，保证不变 */
    path[len++] = '/';
    size_t room = cap - len - 1;               /* ★ 留 1 B 给结尾 NUL ⇒ room ≥ 1 */
    size_t n    = strnlen(name, room);
    memcpy(path + len, name, n);
    path[len + n] = '\0';
    return 0;
}

/** 把 `s` 直接追加到 `path` 末尾（**不加**分隔符）。总 NUL 结尾、绝不溢出。 */
static void path_append(char *path, size_t cap, const char *s)
{
    size_t len = strlen(path);
    if (len >= cap) return;
    size_t room = cap - len - 1;
    size_t n    = strnlen(s, room);
    memcpy(path + len, s, n);
    path[len + n] = '\0';
}

/** 单调时钟（微秒）。**冷却期一律用它，绝不用 time()** —— 未校时时 time() 会跳。 */
static int64_t mono_us(void)
{
    return esp_timer_get_time();
}

/** 把某个 person_id 的冷却打上（begin 成功的那一刻就扣，见 .h 的注释） */
static void cool_mark(int person_id)
{
    if (person_id >= 0 && person_id <= FACE_DB_MAX) {
        s_cool_us[person_id] = mono_us();
    }
}

bool punch_service_in_cooldown(int person_id)
{
    if (person_id < 0 || person_id > FACE_DB_MAX) return false;
    int64_t last = s_cool_us[person_id];
    if (last == 0) return false;
    return (mono_us() - last) < (int64_t)PUNCH_COOLDOWN_S * 1000000;
}

/* ---------------------------------------------------------------------------
 *  CSV：追加与读取
 * ------------------------------------------------------------------------- */

#define PUNCH_CSV_HDR  "seq,name,person_id,date,time,epoch_s,photo,score\n"

/** 数一下 index.csv 里已有的行数（不含表头）→ 下一个 seq。
 *  ★ 掉电可能留下半行 ⇒ 这里**按 '\n' 计数**，半行不算一条，天然容错。
 *  ★ 扫描上限 512 KB：再大也只是 seq 不准，不值得为它把打卡拖慢。 */
static int csv_count_rows(FILE *fp)
{
    char buf[512];
    int  rows = 0;
    long total = 0;

    if (fseek(fp, 0, SEEK_SET) != 0) return 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') rows++;
        }
        total += (long)n;
        if (total > 512 * 1024) break;
    }
    fseek(fp, 0, SEEK_END);
    if (rows > 0) rows--;          /* 减掉表头那一行 */
    return (rows < 0) ? 0 : rows;
}

/** 追加一行。返回 0 成功。 */
static int csv_append(const char *abs_root, int seq, const recog_result_t *who,
                      const net_time_t *t, const char *rel, float score)
{
    char  path[PUNCH_PATH_MAX];
    char  name[FACE_NAME_MAX];
    FILE *fp;

    if (!path_cap_ok(sizeof(path))) return -1;
    snprintf(path, sizeof(path), "%s/%s", abs_root, PUNCH_CSV_NAME);

    /* ★ 表头只在文件为空时写一次。
     *   用 `"a+"` 而不是 `"a"`：两者写都在末尾，但 `a+` 允许读，
     *   而我们需要 stat 之外的一个可靠判空手段（见下）。
     *   ⚠ 不要用 `ftell` 判空 —— 文本模式下它的返回值在 FATFS 上不可靠。 */
    fp = fopen(path, "a+");
    if (fp == NULL) {
        ESP_LOGE(TAG, "fopen(%s, a+) failed, errno=%d", path, errno);
        return -1;
    }

    {
        struct stat st;
        bool empty = !(stat(path, &st) == 0 && st.st_size > 0);
        if (empty && fputs(PUNCH_CSV_HDR, fp) < 0) {
            ESP_LOGE(TAG, "write csv header failed, errno=%d", errno);
            fclose(fp);
            return -1;
        }
    }

    /* 姓名：写进 CSV 也要安全化 —— 逗号/引号会毁掉列结构。
     * ★ 先走 face_db 的同一套规则（把 \ / : * ? " < > | 与控制字符换成 '_'），
     *   但**逗号不在那个名单里**（它是文件名合法字符），所以下面再补一遍。
     *   两处都用同一份规则，是为了让"目录名"与"CSV 里的名字"永远一致 ——
     *   否则 PC 端按名字做统计会对不上。 */
    face_db_sanitize_name(who->name[0] ? who->name : FACE_DB_GUEST_NAME, name, sizeof(name));
    for (char *c = name; *c; c++) {
        if (*c == ',' || *c == '\"') *c = '_';
    }

    int n;
    if (t->valid) {
        /* ★ 零填充写在格式串里（%04d/%02d），所以列宽稳定 —— Excel 里排序不会乱。 */
        n = fprintf(fp, "%d,%s,%d,%04d-%02d-%02d,%02d:%02d:%02d,%ld,%s,%.3f\n",
                    seq, name, who->person_id,
                    t->year, t->mon, t->day, t->hour, t->min, t->sec,
                    (long)t->epoch, rel, (double)score);
    } else {
        /* ★ R5：未校时写 `--`，**绝不写 1970**。epoch_s 写 0
         *   （0 在 PC 端一眼能看出"这条没有可信时间"，而 1970 看起来像真时间）。 */
        n = fprintf(fp, "%d,%s,%d,--,--,0,%s,%.3f\n",
                    seq, name, who->person_id, rel, (double)score);
    }
    if (n < 0) {
        ESP_LOGE(TAG, "csv write failed, errno=%d", errno);
        fclose(fp);
        return -1;
    }
    /* ★ 必须 fclose 才算落盘（FATFS 的缓冲在 FILE 层）；
     *   打卡是"低频但重要"的写入，宁可多一次 flush。 */
    fclose(fp);
    return 0;
}

/** 宽松解析一行 CSV。返回 0 成功。
 *  ★ 列里没有逗号（name 已做安全化）⇒ 用 `%[^,]` 拆分是安全的。
 *  ★ 任何一行解析失败都只跳过它 —— SD 卡掉电留下半行是常态，
 *    不能让一行坏数据毁掉整次浏览（S8-4 验收项"100 条记录滚动不卡"的前提）。 */
static int csv_parse_row(const char *line, punch_log_t *out)
{
    char name[FACE_NAME_MAX];
    char date[12];
    char time[10];
    char rel[PUNCH_REL_MAX];
    int  seq = 0, pid = 0;
    long epoch = 0;
    float score = 0.0f;

    memset(name, 0, sizeof(name));
    memset(date, 0, sizeof(date));
    memset(time, 0, sizeof(time));
    memset(rel, 0, sizeof(rel));

    int got = sscanf(line, "%d,%23[^,],%d,%11[^,],%9[^,],%ld,%47[^,],%f",
                     &seq, name, &pid, date, time, &epoch, rel, &score);
    if (got != 8) return -1;
    if (seq <= 0 || rel[0] == '\0') return -1;

    memset(out, 0, sizeof(*out));
    out->seq       = seq;
    out->person_id = pid;
    out->score     = score;
    out->epoch_s   = (int64_t)epoch;
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->date, sizeof(out->date), "%s", date);
    snprintf(out->time, sizeof(out->time), "%s", time);
    snprintf(out->rel,  sizeof(out->rel),  "%s", rel);
    return 0;
}

/**
 * 把数组**原地**左旋 k 位（`a[k..n-1]` 变成 `a[0..n-1-k]`）。
 *
 * ★ 为什么用"三次翻转"而不是"再开个临时数组"：
 *   本函数的调用点在一个 **4 KB 栈**的一次性任务/UI 线程里，
 *   而一条 `punch_log_t` ≈ 120 B —— `PUNCH_LOG_MAX`(128) 条的临时数组就是
 *   15 KB，直接把栈顶穿。三次翻转是 O(1) 额外空间，代价只是 O(n) 次结构体
 *   赋值（百来次，忽略不计）。
 */
static void rotate_left(punch_log_t *a, int n, int k)
{
    if (n <= 1 || k <= 0 || k >= n) return;
    for (int lo = 0, hi = k - 1; lo < hi; lo++, hi--) {
        punch_log_t t = a[lo]; a[lo] = a[hi]; a[hi] = t;
    }
    for (int lo = k, hi = n - 1; lo < hi; lo++, hi--) {
        punch_log_t t = a[lo]; a[lo] = a[hi]; a[hi] = t;
    }
    for (int lo = 0, hi = n - 1; lo < hi; lo++, hi--) {
        punch_log_t t = a[lo]; a[lo] = a[hi]; a[hi] = t;
    }
}

/* ---------------------------------------------------------------------------
 *  一次性任务（写一条 / 清空）
 * ------------------------------------------------------------------------- */

/** 删掉 root 下的所有**子目录**（连同里面的文件）+ 截断 CSV。
 *  ★★ 红线：只动 root（/sdcard/attendance），绝不碰 /sdcard/face_db。 */
static esp_err_t do_clear(const char *root)
{
    DIR *d = opendir(root);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir(%s) failed, errno=%d (nothing to clear?)", root, errno);
        return ESP_OK;              /* 目录都不存在 = 已经是空的 */
    }

    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strcmp(e->d_name, PUNCH_CSV_NAME) == 0) continue;   /* CSV 单独截断 */

        /* ★ 拼接走 path_append/path_join（原因见那两个函数：snprintf 的
         *   `d_name[256]` 最坏情况会被 GCC 判成溢出 ⇒ -Werror 编译失败）。 */
        char sub[PUNCH_PATH_MAX];
        sub[0] = '\0';
        path_append(sub, sizeof(sub), root);
        if (path_join(sub, sizeof(sub), e->d_name) != 0) break;

        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
            /* 根目录下的散文件也一并删掉（历史上可能有别的命名） */
            if (unlink(sub) == 0) removed++;
            continue;
        }

        DIR *sd = opendir(sub);
        if (sd != NULL) {
            struct dirent *se;
            while ((se = readdir(sd)) != NULL) {
                if (strcmp(se->d_name, ".") == 0 || strcmp(se->d_name, "..") == 0) continue;
                char f[PUNCH_PATH_MAX];
                f[0] = '\0';
                path_append(f, sizeof(f), sub);
                if (path_join(f, sizeof(f), se->d_name) != 0) break;
                if (unlink(f) == 0) removed++;
            }
            closedir(sd);
        }
        if (rmdir(sub) == 0) removed++;
    }
    closedir(d);

    /* CSV：截断后**重新写表头**（保持"PC 端 Excel 能直接打开"的约定） */
    char csv[PUNCH_PATH_MAX];
    if (path_cap_ok(sizeof(csv))) {
        snprintf(csv, sizeof(csv), "%s/%s", root, PUNCH_CSV_NAME);
        FILE *fp = fopen(csv, "wb");
        if (fp) {
            fputs(PUNCH_CSV_HDR, fp);
            fclose(fp);
        } else {
            ESP_LOGW(TAG, "recreate %s failed, errno=%d", csv, errno);
        }
    }

    ESP_LOGI(TAG, "clear done: %d entries removed under %s", removed, root);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  ★★ 2026-09-28 新增：单条删除 / 流水-照片对账（用户反馈的两个 face db 页 BUG）
 * --------------------------------------------------------------------------- */

/**
 * 逐行过滤重写 index.csv —— 删单条与对账共用的唯一实现。
 *
 * ★ 为什么是"重写"而不是"原地删行"：FATFS 没有"删除文件中间一段"这种操作，
 *   重写是唯一正路。做法是经典的"写临时文件 + rename"：
 *     ① 读 index.csv 逐行过筛，保留行写进 index.tmp（先写表头）；
 *     ② fclose 后 unlink(index.csv) + rename(index.tmp, index.csv)。
 *   rename 在 FATFS 上是原子的目录项操作，中途掉电最多留下一个 index.tmp
 *   垃圾文件，index.csv 本身不会出现"半新半旧"。
 *
 * ★ 栈安全：逐行处理（line[192] 栈缓冲），**绝不**把整个 CSV 读进内存 ——
 *   本函数跑在 4 KB 栈的一次性任务里（见 punch_task）。
 *
 * @param root          /sdcard/attendance 绝对路径
 * @param del_seq       >0 = 只删这一行（单条删除模式）；0 = 不按 seq 删
 * @param drop_missing  true = "照片文件不存在"的行也删（对账模式）
 * @param out_dropped   输出：实际删掉的行数（可 NULL）
 * @return ESP_OK / ESP_ERR_NOT_FOUND（del_seq 模式下没找到那一行）
 */
static esp_err_t csv_rewrite_filtered(const char *root, int del_seq,
                                      bool drop_missing, int *out_dropped)
{
    char csv[PUNCH_PATH_MAX];
    char tmp[PUNCH_PATH_MAX];
    if (!path_cap_ok(sizeof(csv)) || !path_cap_ok(sizeof(tmp))) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(csv, sizeof(csv), "%s/%s", root, PUNCH_CSV_NAME);
    snprintf(tmp, sizeof(tmp), "%s/%s", root, "index.tmp");

    FILE *src = fopen(csv, "r");
    if (src == NULL) {
        /* CSV 不存在 = 没有任何记录可删/可对账 */
        return ESP_ERR_NOT_FOUND;
    }

    FILE *dst = fopen(tmp, "wb");
    if (dst == NULL) {
        ESP_LOGE(TAG, "rewrite: fopen(%s, wb) failed, errno=%d", tmp, errno);
        fclose(src);
        return ESP_FAIL;
    }
    fputs(PUNCH_CSV_HDR, dst);

    int  dropped = 0;
    bool found_seq = (del_seq <= 0);        /* 单条删除模式下是否见到过目标行 */
    char line[192];
    bool first = true;

    while (fgets(line, sizeof(line), src) != NULL) {
        if (first) {
            first = false;
            if (strncmp(line, "seq,", 4) == 0) continue;    /* 原表头不进新文件 */
        }

        punch_log_t rec;
        bool keep;
        if (csv_parse_row(line, &rec) != 0) {
            /* 坏行：**保留**。我们读不懂的东西绝不顺手销毁 ——
             * 那可能是掉电留下的半行，也可能只是格式将来变了。 */
            keep = true;
        } else {
            keep = true;
            if (del_seq > 0 && rec.seq == del_seq) {
                keep = false;
                found_seq = true;
                /* 顺手把那张照片也删了（不存在不算错 —— 可能已被单独删过） */
                char photo[PUNCH_PATH_MAX];
                if (path_cap_ok(sizeof(photo))) {
                    photo[0] = '\0';
                    path_append(photo, sizeof(photo), root);
                    if (path_join(photo, sizeof(photo), rec.rel) == 0) {
                        if (unlink(photo) == 0) {
                            ESP_LOGI(TAG, "del: photo removed %s", photo);
                        }
                    }
                }
            } else if (drop_missing) {
                /* 对账：stat 照片，不在卡上的行整行删掉 */
                char photo[PUNCH_PATH_MAX];
                if (path_cap_ok(sizeof(photo))) {
                    photo[0] = '\0';
                    path_append(photo, sizeof(photo), root);
                    path_join(photo, sizeof(photo), rec.rel);
                }
                struct stat st;
                if (photo[0] == '\0' || stat(photo, &st) != 0) {
                    keep = false;
                    ESP_LOGI(TAG, "sync: drop row seq=%d (photo missing: %s)",
                             rec.seq, photo[0] ? photo : "(path too long)");
                }
            }
        }

        if (keep) {
            fputs(line, dst);
        } else {
            dropped++;
        }
    }
    fclose(src);
    fclose(dst);

    if (del_seq > 0 && !found_seq) {
        unlink(tmp);                        /* 没找到目标行：撤销这次重写 */
        ESP_LOGW(TAG, "delete: seq=%d not found in csv", del_seq);
        return ESP_ERR_NOT_FOUND;
    }

    unlink(csv);
    if (rename(tmp, csv) != 0) {
        /* 理论上到不了这（同目录 rename 极少失败）；真发生了也要说清楚后果 */
        ESP_LOGE(TAG, "rename(tmp->csv) failed, errno=%d (rows kept in index.tmp)", errno);
        return ESP_FAIL;
    }

    if (out_dropped) *out_dropped = dropped;
    return ESP_OK;
}

/** 删掉 attendance 下的**空**子目录（对账的收尾：照片被手动删光后目录会剩壳） */
static void rmdir_empty_subdirs(const char *root)
{
    DIR *d = opendir(root);
    if (d == NULL) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strcmp(e->d_name, PUNCH_CSV_NAME) == 0) continue;

        char sub[PUNCH_PATH_MAX];
        sub[0] = '\0';
        path_append(sub, sizeof(sub), root);
        if (path_join(sub, sizeof(sub), e->d_name) != 0) continue;

        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        DIR *sd = opendir(sub);
        if (sd == NULL) continue;
        bool empty = true;
        struct dirent *se;
        while ((se = readdir(sd)) != NULL) {
            /* ★ 只跳过 . / ..：出现任何实体（照片 / 埋伏的散文件）都算非空。
             *   判空走保守方向 —— 宁可留壳，绝不误删还有照片的目录。 */
            if (strcmp(se->d_name, ".") == 0 || strcmp(se->d_name, "..") == 0) continue;
            empty = false;
            break;
        }
        closedir(sd);
        if (empty) {
            if (rmdir(sub) == 0) ESP_LOGI(TAG, "sync: removed empty dir %s", sub);
        }
    }
    closedir(d);
}

/** 一次性任务：写一条打卡 或 清空记录。 */
static void punch_task(void *arg)
{
    (void)arg;

    TickType_t t0 = xTaskGetTickCount();
    punch_record_t res;
    memset(&res, 0, sizeof(res));
    res.err = ESP_FAIL;

    /* ★★★ 这里**不要**再写 `if (!path_cap_ok(sizeof(root)))`。
     *   root 是专用小缓冲（48 B，只需装 "/sdcard/attendance" 19 B），
     *   而 path_cap_ok 把它拿去和 PUNCH_PATH_MAX(160) 比 ⇒ 恒 false
     *   ⇒ 每次打卡都在此 `goto finish` 成 ESP_ERR_INVALID_SIZE。
     *   它的容量由文件头那条 `PUNCH_ROOTBUF >= sizeof(SDCARD_MNT_PATH)+sizeof(PUNCH_DIR_NAME)`
     *   的 _Static_assert 在**编译期**担保，运行时无需再查。
     *
     *   ★★ 注意"恒真的早退"还有一个**静默**的副作用（2026-09-28 实测发现）：
     *      GCC 会把本行之后的整个 `punch_task` 当作**不可达代码**，
     *      于是下面那些 `snprintf` 的 `-Wformat-truncation` **一次都没被检查过**。
     *      修掉本行后它们立刻全部暴露 —— 也就是说"编译通过"从来不代表
     *      这段代码被编译检查过。已按 §70 逐个改为 path_join/path_append。 */
    char root[PUNCH_ROOTBUF];
    snprintf(root, sizeof(root), "%s%s", SDCARD_MNT_PATH, PUNCH_DIR_NAME);

    /* SD 卡必须在（没有它一切免谈）。★ 中途拔卡是"不支持热插拔"的已知限制
     *   （sdcard_service.h），这里只是把失败如实报出来。 */
    if (!sdcard_service_is_mounted()) {
        ESP_LOGE(TAG, "SD not mounted");
        res.err = ESP_ERR_NOT_FOUND;
        goto finish;
    }

    /* ---- 删单条模式（2026-09-28：face db 记录页的行内 DEL 按钮） ---- */
    if (s_op_del_seq > 0) {
        res.clear_op = true;            /* 复用"维护操作"语义：CAM 页不会弹打卡成功提示 */
        int dropped = 0;
        res.err = csv_rewrite_filtered(root, s_op_del_seq, false, &dropped);
        if (res.err == ESP_OK) {
            rmdir_empty_subdirs(root);
            ESP_LOGI(TAG, "delete seq=%d done: %d row(s) dropped", s_op_del_seq, dropped);
        }
        goto finish;
    }

    /* ---- 对账模式（2026-09-28：照片被手动删掉后 CSV 与目录同步） ---- */
    if (s_op_sync) {
        res.clear_op = true;
        int dropped = 0;
        res.err = csv_rewrite_filtered(root, 0, true, &dropped);
        if (res.err == ESP_OK) {
            rmdir_empty_subdirs(root);
            ESP_LOGI(TAG, "sync done: %d stale row(s) dropped", dropped);
        } else if (res.err == ESP_ERR_NOT_FOUND) {
            res.err = ESP_OK;           /* CSV 都不存在 = 天然同步 */
        }
        goto finish;
    }

    /* ---- 清空模式 ---- */
    if (s_op_clear) {
        res.clear_op  = true;          /* ★ UI 靠这个字段把"清空成功"与"打卡成功"分开提示 */
        res.err       = do_clear(root);
        res.person_id = FACE_DB_GUEST_ID;
        goto finish;
    }

    /* ---- 写一条 ---- */
    res.clear_op = false;              /* memset 已归零，写出来是为了让"两条路径"一眼可辨 */
    {
        recog_result_t *who = &s_pend_who;
        net_time_t     *t   = &s_pend_time;
        char  sub[PUNCH_SUBBUF];        /* "01_张三" 或 "99_访客" */
        char  base[PUNCH_BASEBUF];      /* "20260928_103045" */
        char  sub_abs[PUNCH_PATH_MAX];  /* /sdcard/attendance/01_张三 */
        char  rel[PUNCH_REL_MAX];
        char  abs[PUNCH_PATH_MAX];

        /* 目录名（序号 + 安全化姓名）。访客用 NULL ⇒ "99_访客"。
         * ★ 序号前缀不是装饰：纯中文名在 FATFS 里按**字节序**排会乱得没法看。 */
        esp_err_t de = face_db_attendance_dir(
            (uint16_t)((who->person_id < 0) ? FACE_DB_GUEST_ID : who->person_id),
            who->name[0] ? who->name : NULL, sub, sizeof(sub));
        if (de != ESP_OK) {
            ESP_LOGE(TAG, "attendance_dir failed: %s", esp_err_to_name(de));
            res.err = de;
            goto finish;
        }

        if (!path_cap_ok(sizeof(sub_abs))) { res.err = ESP_ERR_INVALID_SIZE; goto finish; }
        snprintf(sub_abs, sizeof(sub_abs), "%s/%s", root, sub);

        /* FATFS 逐级建目录；EEXIST 是常态（第二次打卡）。 */
        if (mkdir(root, 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s failed, errno=%d", root, errno);
            res.err = ESP_FAIL;
            goto finish;
        }
        if (mkdir(sub_abs, 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s failed, errno=%d", sub_abs, errno);
            res.err = ESP_FAIL;
            goto finish;
        }

        /* 文件名 = 时间戳（R5 的技术必然：同秒两人打卡会撞名 ⇒ 必须有防撞后缀）。
         * ★ 未校时**绝不**用 0 秒时间戳 —— 那会让每张都叫 19700101_000000 互相覆盖。 */
        if (t->valid) {
            snprintf(base, sizeof(base), "%04d%02d%02d_%02d%02d%02d",
                     t->year, t->mon, t->day, t->hour, t->min, t->sec);
        } else {
            /* 退化成序号，靠下面的防撞循环递增（与 capture_service 同款） */
            snprintf(base, sizeof(base), "IMG_SEQ");
        }

        int seq = 0;
        {
            char  csv[PUNCH_PATH_MAX];
            if (!path_cap_ok(sizeof(csv))) { res.err = ESP_ERR_INVALID_SIZE; goto finish; }
            snprintf(csv, sizeof(csv), "%s/%s", root, PUNCH_CSV_NAME);
            FILE *fp = fopen(csv, "a+");
            if (fp != NULL) {
                seq = csv_count_rows(fp) + 1;
                fclose(fp);
            }
            /* 打不开也不致命：seq 只是"给人看的行号"，用 0 表示未知即可，
             * 真正重要的是后面的 `photo` 列（它才是数据的唯一标识）。 */
        }

        /* 防撞：同秒连拍 / 未校时的固定名都会撞；撞了加 _NN 后缀。
         * ★ 上限 100 是为了"绝不无限循环"（目录里真有 100 个同名前缀时直接失败）。 */
        int  hit = -1;
        char cand[PUNCH_PATH_MAX];
        for (int n = 0; n < 100; n++) {
            /* ★ 先把它格式化成**足够大的**小缓冲，再用 path_append 拼 ——
             *   直接对一个 48 B 的 `rel` 写 `"%s/%s.jpg"` 会让 GCC 算出
             *   "最坏 39+1+19+4 = 63 B > 48" 而报 format-truncation。
             *   suffix 取 24：数字串最坏 11 字符 ⇒ 11+4("IMG_")+4(".jpg") = 19 < 24。 */
            char suffix[24];
            rel[0] = '\0';
            path_append(rel, sizeof(rel), sub);
            path_append(rel, sizeof(rel), "/");
            if (t->valid) {
                if (n == 0) snprintf(suffix, sizeof(suffix), ".jpg");
                else        snprintf(suffix, sizeof(suffix), "_%02d.jpg", n);
                path_append(rel, sizeof(rel), base);
            } else {
                /* 未校时：文件名用序号（此时 base 只是个占位串 "IMG_SEQ"，不进名字） */
                snprintf(suffix, sizeof(suffix), "IMG_%04d.jpg", (seq > 0 ? seq : 0) + n);
            }
            path_append(rel, sizeof(rel), suffix);
            if (!path_cap_ok(sizeof(cand))) { res.err = ESP_ERR_INVALID_SIZE; goto finish; }
            cand[0] = '\0';
            path_append(cand, sizeof(cand), root);
            if (path_join(cand, sizeof(cand), rel) != 0) { res.err = ESP_ERR_INVALID_SIZE; goto finish; }

            struct stat st;
            if (stat(cand, &st) != 0) { hit = n; break; }
        }
        if (hit < 0) {
            ESP_LOGE(TAG, "no free name in %s (100 collisions)", sub_abs);
            res.err = ESP_FAIL;
            goto finish;
        }
        snprintf(abs, sizeof(abs), "%s", cand);

        /* ---- JPEG 软编（这就是 S8-3 的核心：像素已经被 LVGL 画过水印了） ---- */
        size_t jlen = 0;
        esp_err_t je = jpeg_util_encode_rgb888(s_frame, s_frame_w, s_frame_h,
                                               PUNCH_JPEG_Q, s_jpeg, JPEG_UTIL_OUT_MAX, &jlen);
        if (je != ESP_OK) {
            ESP_LOGE(TAG, "jpeg encode failed: %s", esp_err_to_name(je));
            res.err = je;
            goto finish;
        }

        /* ---- 落盘 ---- */
        {
            FILE *fp = fopen(abs, "wb");
            if (fp == NULL) {
                ESP_LOGE(TAG, "fopen(%s, wb) failed, errno=%d", abs, errno);
                res.err = ESP_FAIL;
                goto finish;
            }
            size_t n = fwrite(s_jpeg, 1, jlen, fp);
            if (n != jlen) {
                /* ★ 必须查返回值：盘满时 fwrite 返回短计数（record_service 的
                 *   "静默冻结"就是没查它，本工程的前车之鉴）。 */
                ESP_LOGE(TAG, "fwrite short: %u/%u, errno=%d", (unsigned)n, (unsigned)jlen, errno);
                fclose(fp);
                unlink(abs);        /* 不留半张坏图 */
                res.err = ESP_FAIL;
                goto finish;
            }
            fclose(fp);
            /* ★★★ 2026-09-28 新增（用户反馈"识别人脸成功了，但 SD 卡里看不到打卡图"）：
             *   写完**立刻回读卡上的真实大小**。
             *
             *   为什么必须有这一步：FATFS 的文件数据在 `fclose()` 才刷出，
             *   而"数据刷出去了"和"目录项更新成功"是两件事。卡写保护、FAT 表所在
             *   扇区坏掉、或根目录项空间耗尽时，`fwrite()` 可以返回**满计数**
             *   而文件在卡上根本不存在 / 长度为 0 —— 那时日志里是一句漂亮的
             *   `punch OK: ...`，用户却在电脑上看不到任何图。
             *   这类"接口说成功、介质上没有"的矛盾只有回读能当场抓住。
             *   （只多一次 stat，相对整条链路可忽略。） */
            struct stat vst;
            long on_card = (stat(abs, &vst) == 0) ? (long)vst.st_size : -1L;
            if (on_card != (long)jlen) {
                ESP_LOGE(TAG, "WRITE VERIFY FAILED: %s on card = %ld B (expect %u) -> "
                              "FATFS write did NOT land (read-only card? no dir entry? bad sector?)",
                         abs, on_card, (unsigned)jlen);
                res.err = ESP_FAIL;
                goto finish;
            }
        }

        /* ---- 写流水（CSV）---- */
        if (csv_append(root, seq, who, t, rel, who->score) != 0) {
            /* 照片已经写好了、CSV 没写：**保留照片**（它是证据），
             * 只把结果标成"部分成功"，别删照片 —— 删了就什么都留不下。 */
            ESP_LOGW(TAG, "photo saved but csv append failed: %s", abs);
            res.err = ESP_FAIL;
        } else {
            res.err = ESP_OK;
        }

        res.person_id  = who->person_id;
        res.matched    = who->matched;
        res.score      = who->score;
        res.time_valid = t->valid;
        res.jpeg_bytes = jlen;
        /* ★ 用 `%.*s` + **编译期**精度：`(int)(sizeof(res.name) - 1)` = 23 是常量，
         *   GCC 于是能证明"最多写 23 B 进 24 B 的缓冲"。
         *   写成裸 `%s` 时它会认为源无界（实测报 "up to 2135 bytes"）——
         *   因为 `who->name` 是**结构体成员**，GCC 不假定它一定含 NUL。 */
        snprintf(res.name, sizeof(res.name), "%.*s",
                 (int)(sizeof(res.name) - 1),
                 who->name[0] ? who->name : FACE_DB_GUEST_NAME);
        snprintf(res.path, sizeof(res.path), "%s", abs);

        ESP_LOGI(TAG, "punch OK: %s | %s (id=%d score=%.3f) | %u B jpeg | time %s",
                 abs, res.name, res.person_id, (double)res.score,
                 (unsigned)res.jpeg_bytes, t->valid ? "valid" : "INVALID (-- )");
    }

finish:
    if (res.err != ESP_OK) {
        ESP_LOGE(TAG, "punch FAIL: %s", esp_err_to_name(res.err));
    }
    res.has_result = true;              /* ★ 唯一判据：本次任务真的跑完了（见头文件） */
    res.elapsed_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);

    /* ★ R7 的验收数据落在这一行：mkdir + 同步写盘 + CSV 的**总耗时**。
     *   它跑在独立任务里（不是 LVGL 线程），所以这个数字只决定"下一拍能不能
     *   立刻再打"，不影响取景器帧率 —— 但明显超过 1 s 就说明 SD 卡很慢，
     *   "连打多张会排队"就能解释了。 */
    ESP_LOGI(TAG, "%s done in %u ms (err=%s)",
             res.clear_op ? "clear" : "punch", (unsigned)res.elapsed_ms,
             esp_err_to_name(res.err));

    /* ★ 先写结果、再清 busy：读侧看到 !busy 就能拿到完整结果。 */
    s_last       = res;
    s_pend_valid = false;
    s_op_clear   = false;
    s_op_del_seq = 0;       /* ★ 2026-09-28：维护操作的标志同样要在退出前清干净 */
    s_op_sync    = false;
    s_busy       = false;

    /* ★★ §73.9（用户验收标准："cam 里面的 PSRAM 按需分配、即用即释放"）：
     *   帧缓冲（172,800 B）+ JPEG 输出（64 KB）旧版一旦分配就**常驻到断电**
     *   —— CAM 会话结束后这两块还挂在账上，挤压 VIDEO/取景器的连续块。
     *   现在随事务结束归还，下次打卡重新分配（~1 ms，相对整条落盘链路可忽略）。
     *   ★ 安全性：此刻任务即将退出，s_frame/s_jpeg 的唯一使用者（水印绘制
     *     在 LVGL begin/commit 之间、编码在任务里）都已结束。 */
    if (s_frame) {
        heap_caps_free(s_frame);
        s_frame = NULL;
        s_frame_w = 0;
        s_frame_h = 0;
        ESP_LOGI(TAG, "punch frame buffer freed (%u B back)", 240u * 240u * 3u);
    }
    if (s_jpeg) {
        heap_caps_free(s_jpeg);
        s_jpeg = NULL;
        ESP_LOGI(TAG, "punch jpeg out buffer freed (%u B back)", (unsigned)JPEG_UTIL_OUT_MAX);
    }

    vTaskDelete(NULL);
}

/** 起一次性任务。返回 ESP_OK 表示已受理。 */
static esp_err_t spawn_task(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(punch_task, "punch",
                                            PUNCH_TASK_STACK, NULL,
                                            PUNCH_TASK_PRIO, NULL, PUNCH_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(punch) FAILED (internal RAM tight)");
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  对外 API
 * ------------------------------------------------------------------------- */
void punch_service_init(void)
{
    /* ★ 幂等 + **只打一次日志**：本函数被 main.c（开机）、tab_camera（每次进 CAM 页）、
     *   page_face（每次进 FACE 页）三处调用，每次都打会让串口里出现"又一次初始化"
     *   的错觉（2026-09-28 上机日志里 PUNCH: init 出现在 118 s 就是这个原因）。 */
    static bool s_logged = false;

    memset(&s_last, 0, sizeof(s_last));
    memset(s_cool_us, 0, sizeof(s_cool_us));
    s_busy       = false;
    s_pend_valid = false;
    s_op_clear   = false;
    if (!s_logged) {
        s_logged = true;
        ESP_LOGI(TAG, "init (one-shot task, stack=%d, cooldown=%ds, q=%d)",
                 PUNCH_TASK_STACK, PUNCH_COOLDOWN_S, PUNCH_JPEG_Q);
    }
}

punch_prep_t punch_service_begin(const recog_result_t *who,
                                 const uint8_t *src_rgb888,
                                 uint16_t w, uint16_t h,
                                 uint8_t **out_frame)
{
    if (out_frame) *out_frame = NULL;
    if (!who || !src_rgb888 || !out_frame || w == 0 || h == 0) return PUNCH_PREP_BAD_ARG;

    if (s_busy) return PUNCH_PREP_BUSY;

    if (!sdcard_service_is_mounted()) return PUNCH_PREP_NO_SD;
    /* ★ 库不可用就没法决定目录名的序号（`01_张三` 里的 01 来自库的 id）
     *   ⇒ 老实拒绝，而不是"先存到临时目录以后再说"（那要写第二套清理逻辑）。 */
    if (!face_db_is_ready()) return PUNCH_PREP_NO_DB;

    /* ★ 冷却：同一人 60 s 内只记一次（§9-R8）。**扣减就发生在这一刻**，
     *   否则"画水印失败 → 不 commit → 冷却没扣 → 下一帧又发起"会死循环刷屏。 */
    if (punch_service_in_cooldown(who->person_id)) return PUNCH_PREP_COOLDOWN;

    /* ---- 常驻缓冲（第一次真正打卡时才分配；没打过的设备一分 PSRAM 都不占） ---- */
    if (s_frame == NULL) {
        s_frame = (uint8_t *)heap_caps_aligned_alloc(64, (size_t)w * h * 3,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_frame == NULL) {
            ESP_LOGE(TAG, "frame alloc FAILED (%u B PSRAM)", (unsigned)(w * h * 3));
            return PUNCH_PREP_NO_MEM;
        }
        s_frame_w = w;
        s_frame_h = h;
        ESP_LOGI(TAG, "frame buffer allocated: %ux%u (%u B)",
                 (unsigned)w, (unsigned)h, (unsigned)(w * h * 3));
    }
    if (s_jpeg == NULL) {
        s_jpeg = (uint8_t *)heap_caps_aligned_alloc(64, JPEG_UTIL_OUT_MAX,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_jpeg == NULL) {
            ESP_LOGE(TAG, "jpeg out alloc FAILED (%u B PSRAM)", (unsigned)JPEG_UTIL_OUT_MAX);
            return PUNCH_PREP_NO_MEM;
        }
    }
    if (s_frame_w != w || s_frame_h != h) {
        /* 尺寸变了（将来支持多分辨率时的显式拒绝，而不是默默用旧的） */
        ESP_LOGE(TAG, "frame size changed %dx%d -> %ux%u, not supported",
                 s_frame_w, s_frame_h, w, h);
        return PUNCH_PREP_BAD_ARG;
    }

    /* ---- 同步拷帧（见 .h 里"为什么 memcpy 放在这里"）---- */
    memcpy(s_frame, src_rgb888, (size_t)w * h * 3);

    /* ---- 占位 + 取时间快照 ----
     * ★ 时间在**这一刻**取（就是"打卡时刻"），而不是任务真正跑起来时。
     *   任务可能晚几十毫秒，跨过整秒边界时两者显示的秒数会不一样 —— 无伤大雅，
     *   但"打卡时间的唯一真相源是 begin 这一刻"这条语义必须固定下来，
     *   否则将来加"补打卡"等功能时会分不清该信哪个。 */
    memset(&s_pend_who, 0, sizeof(s_pend_who));
    s_pend_who.person_id = who->person_id;
    s_pend_who.matched   = who->matched;
    s_pend_who.score     = who->score;
    memcpy(s_pend_who.name, who->name, sizeof(s_pend_who.name));
    s_pend_who.name[sizeof(s_pend_who.name) - 1] = '\0';

    net_time_service_get(&s_pend_time);

    s_pend_valid = true;
    s_busy       = true;          /* 占位即忙：防止并发叠加 */
    cool_mark(who->person_id);

    *out_frame = s_frame;
    return PUNCH_PREP_OK;
}

esp_err_t punch_service_commit(void)
{
    if (!s_pend_valid) {
        ESP_LOGE(TAG, "commit without begin");
        return ESP_ERR_INVALID_STATE;
    }
    s_op_clear = false;
    return spawn_task();
}

void punch_service_abort(void)
{
    if (s_pend_valid) {
        s_pend_valid = false;
        s_busy       = false;
        /* ★ §73.9：begin 分配的帧缓冲此刻必然没人用（任务没起、LVGL 画水印
         *   失败才会走到这）—— 一并归还，别等"下次任务收尾"（可能永远不来）。 */
        if (s_frame) {
            heap_caps_free(s_frame);
            s_frame = NULL;
            s_frame_w = 0;
            s_frame_h = 0;
            ESP_LOGI(TAG, "abort: punch frame buffer freed");
        }
        ESP_LOGW(TAG, "aborted (watermark failed?) - cooldown stays charged");
    }
}

bool punch_service_busy(void)
{
    return s_busy;
}

void punch_service_get_last(punch_record_t *out)
{
    if (out) *out = s_last;
}

void punch_service_clear_last(void)
{
    memset(&s_last, 0, sizeof(s_last));
}

int punch_service_log_read(punch_log_t *out, int max)
{
    if (!out || max <= 0) return 0;
    if (!sdcard_service_is_mounted()) return 0;

    char path[PUNCH_PATH_MAX];
    if (!path_cap_ok(sizeof(path))) return 0;
    snprintf(path, sizeof(path), "%s%s/%s", SDCARD_MNT_PATH, PUNCH_DIR_NAME, PUNCH_CSV_NAME);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return 0;                    /* 文件不存在 = 还没有记录 */

    /* ★ 只保留"最后 max 条"：**环形**写进调用方数组，天然实现 `tail -n`。
     *   这样无论文件多大，只做一遍顺序读、零额外分配、栈占用与条数无关
     *   （下面用三次翻转做**原地**旋转 —— 绝不为了它开一个 PUNCH_LOG_MAX 的栈数组，
     *     那会是 15 KB 栈，直接顶穿这类小任务）。 */
    int   n = 0;          /* 已写入的条数（上限 max） */
    int   idx = 0;        /* 环形写指针 = 最老那条所在的下标（绕回后才有意义） */
    char  line[192];
    bool  first = true;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (first) {
            first = false;
            if (strncmp(line, "seq,", 4) == 0) continue;      /* 跳过表头 */
        }
        punch_log_t rec;
        if (csv_parse_row(line, &rec) != 0) continue;         /* 坏行跳过 */
        out[idx] = rec;
        idx = (idx + 1) % max;
        if (n < max) n++;
    }
    fclose(fp);

    /* ★ 只有"绕回过头"才需要旋转（n == max 且 idx != 0）。
     *   判据写错的话，条数少于 max 时会把整段顺序打乱 —— 那是"没有记录时
     *   显示顺序颠倒"的隐蔽 bug。 */
    if (n == max && idx != 0) {
        rotate_left(out, n, idx);
    }
    return n;
}

esp_err_t punch_service_log_clear(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!sdcard_service_is_mounted()) return ESP_ERR_INVALID_STATE;

    s_op_clear   = true;
    s_pend_valid = false;
    s_busy       = true;
    return spawn_task();
}

esp_err_t punch_service_log_delete(int seq)
{
    if (seq <= 0) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!sdcard_service_is_mounted()) return ESP_ERR_INVALID_STATE;

    s_op_clear   = false;
    s_op_sync    = false;
    s_op_del_seq = seq;         /* punch_task 按 >0 识别本操作 */
    s_pend_valid = false;
    s_busy       = true;
    esp_err_t err = spawn_task();
    if (err != ESP_OK) s_op_del_seq = 0;    /* 起任务失败必须回滚标志 */
    return err;
}

esp_err_t punch_service_log_sync(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!sdcard_service_is_mounted()) return ESP_ERR_INVALID_STATE;

    s_op_clear   = false;
    s_op_del_seq = 0;
    s_op_sync    = true;
    s_pend_valid = false;
    s_busy       = true;
    esp_err_t err = spawn_task();
    if (err != ESP_OK) s_op_sync = false;
    return err;
}

esp_err_t punch_service_write_thumb(uint16_t id, const uint8_t *rgb888, int w, int h)
{
    if (!rgb888 || w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;      /* 与打卡共用编码器/输出缓冲，串行 */

    /* 复用与打卡同一块输出缓冲（lazy 分配；第一次录入时才会真的拿到） */
    if (s_jpeg == NULL) {
        s_jpeg = (uint8_t *)heap_caps_aligned_alloc(64, JPEG_UTIL_OUT_MAX,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_jpeg == NULL) {
            ESP_LOGE(TAG, "thumb: jpeg out alloc FAILED");
            return ESP_ERR_NO_MEM;
        }
    }

    size_t jlen = 0;
    /* 底片用**更高**的质量（Q90）：它是给人看/给将来"以图搜人"用的，
     * 一张几百字节的糊图没有任何价值。 */
    esp_err_t e = jpeg_util_encode_rgb888(rgb888, w, h, 90, s_jpeg, JPEG_UTIL_OUT_MAX, &jlen);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "thumb: encode failed: %s", esp_err_to_name(e));
        /* ★ §73.9：失败也要把输出缓冲还回去（即用即释放不留尾巴） */
        heap_caps_free(s_jpeg);
        s_jpeg = NULL;
        return e;
    }
    e = face_db_thumb_write(id, s_jpeg, jlen);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "thumb: face_db_thumb_write(id=%u) failed: %s",
                 (unsigned)id, esp_err_to_name(e));
        heap_caps_free(s_jpeg);
        s_jpeg = NULL;
        return e;
    }
    ESP_LOGI(TAG, "thumb written: id=%u %u B (q90)", (unsigned)id, (unsigned)jlen);
    /* ★ §73.9：即用即释放 —— 底片写完就把输出缓冲还回去（下次录入重新分配）。 */
    if (s_jpeg) {
        heap_caps_free(s_jpeg);
        s_jpeg = NULL;
    }
    return ESP_OK;
}

/* ===========================================================================
 *  ★★★ 2026-09-28 新增：打卡目录体检（用户反馈"卡上看不到打卡图"）
 *
 *  为什么放在这里而不是让用户拔卡插电脑：
 *   · "打卡到底有没有落盘"是**设备侧**的事实，插到电脑上看还多一层 SD 读卡器/
 *     电脑缓存/分区不识别（本卡是 FAT，但电脑把它当移动盘时可能只显示第一个分区）
 *     的干扰 —— 从设备自己打印，才是唯一可信的来源；
 *   · 它和 `punch_task` 用**同一套** SDCARD_MNT_PATH / PUNCH_DIR_NAME 常量，
 *     不存在"电脑上看到的路径和设备写的路径不是一回事"这种错觉。
 *
 *  调用时机：CAM 页建页时调一次（那时 SD 早已挂载完成，且用户正要做打卡）。
 * =========================================================================== */
void punch_service_dump_dir(void)
{
    char root[PUNCH_ROOTBUF];
    snprintf(root, sizeof(root), "%s%s", SDCARD_MNT_PATH, PUNCH_DIR_NAME);

    if (!sdcard_service_is_mounted()) {
        ESP_LOGW(TAG, "dir dump: SD NOT mounted -> nothing can be on the card");
        return;
    }

    DIR *d = opendir(root);
    if (d == NULL) {
        /* 这是**最有用**的一句：目录都不存在 ⇒ 从来没有一次打卡走到落盘这步。
         * 于是排查方向立刻从"文件系统"转到"打卡链路本身"（见 §70/§71）。 */
        ESP_LOGW(TAG, "dir dump: %s does NOT exist -> no punch has ever reached the write step",
                 root);
        return;
    }

    int  subdirs = 0, files_total = 0;
    long bytes_total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;          /* 跳过 . / .. */

        char sub[PUNCH_PATH_MAX];
        sub[0] = '\0';
        path_append(sub, sizeof(sub), root);
        if (path_join(sub, sizeof(sub), e->d_name) != 0) continue;

        struct stat sst;
        if (stat(sub, &sst) != 0 || !S_ISDIR(sst.st_mode)) {
            continue;                               /* index.csv 之类的文件单独不列 */
        }
        subdirs++;

        DIR *sd = opendir(sub);
        if (sd == NULL) continue;
        int  nf = 0;
        long nb = 0;
        struct dirent *se;
        while ((se = readdir(sd)) != NULL) {
            if (se->d_name[0] == '.') continue;
            char f[PUNCH_PATH_MAX];
            f[0] = '\0';
            path_append(f, sizeof(f), sub);
            if (path_join(f, sizeof(f), se->d_name) != 0) continue;
            struct stat fst;
            if (stat(f, &fst) != 0 || S_ISDIR(fst.st_mode)) continue;
            nf++;
            nb += (long)fst.st_size;
        }
        closedir(sd);
        ESP_LOGI(TAG, "dir dump: %s -> %d file(s), %ld B", sub, nf, nb);
        files_total += nf;
        bytes_total += nb;
    }
    closedir(d);

    ESP_LOGI(TAG, "dir dump: %s = %d subdir(s) / %d file(s) / %ld B total",
             root, subdirs, files_total, bytes_total);
    if (files_total == 0) {
        ESP_LOGW(TAG, "dir dump: dir exists but holds ZERO file -> a punch logged OK "
                      "without landing, or the only records were deleted");
    }
}
