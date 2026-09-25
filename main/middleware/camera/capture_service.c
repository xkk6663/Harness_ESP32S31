/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file capture_service.c
 * @brief 单次拍照事务的实现（S2a）。设计与取舍见 capture_service.h 文件头。
 */

#include "capture_service.h"
#include "camera_service.h"
#include "sdcard_service.h"
#include "net_time_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAG "CAPTURE"

/* ===========================================================================
 *  ★★ 栈策略（先说清楚，改动前先读）
 *
 *  本工程**内部 RAM 只剩 ~4 KB 余量**（见 docs/开发文档/内存管理规划.md），
 *  所以这里**刻意不建常驻任务** —— 常驻 4 KB 内部栈会直接吃掉全部余量。
 *  改成"**一次性任务**"：按下【拍照】才建栈，事务结束（几十~几百 ms）即 vTaskDelete，
 *  栈随之归还。代价是每次拍照有一次 4 KB 分配，可能失败 ⇒ 那就**如实报错**，
 *  不静默降级（本工程一贯口径）。
 *
 *  ★ 栈必须放**内部 RAM**（不能 WithCaps 到 PSRAM）：本任务要做 fopen/fwrite
 *    （FATFS → SDMMC → flash/外设 IO），而 F35 明确：做文件/存储 IO 的任务栈
 *    一旦落在 PSRAM，会在 flash cache 关闭窗口里崩。参见项目记忆 F35/F38/F40。
 *
 *  ★ 优先级 5 与 cam 任务同级、低于 taskLVGL：拍照是用户触发的短事务，
 *    不该长期抢 LVGL 的时间片。
 * =========================================================================== */
#define CAP_TASK_STACK     4096
#define CAP_TASK_PRIO      5

/** 丢弃前 N 帧：OV3660 每次切格式都会重置 AE/AWB，头几帧偏暗/偏色。
 *  3 帧 ≈ 250 ms @12fps，是画质与耗时的折中。 */
#define CAP_WARMUP_SKIP    3
/** 等第一张可用帧的上限。给足余量：切格式（预期数百 ms）+ 预热 3 帧。 */
#define CAP_FRAME_WAIT_MS  6000

/* ---- 路径拼接的容量预算（**和 -Werror=format-truncation 直接相关，别随手改小**）----
 * 最坏输出 = (dir ≤ CAP_DIR_BUF-1) + '/' + (base ≤ CAP_BASE_BUF-1) + "_099.jpg"(8) + NUL。
 * 下面的 _Static_assert 就是把这个不等式**编译期锁死**：哪天有人把 CAPTURE_PATH_MAX
 * 调小，或者把 dir/base 的缓冲放大，会立刻在这里报错，而不是等真机 build 才炸。
 * （历史事故：原本 CAPTURE_PATH_MAX=96 < 112，真机 build 报
 *   `'%s' directive output may be truncated ... region of size between 32 and 95`。） */
#define CAP_DIR_BUF        64
#define CAP_BASE_BUF       40
_Static_assert(CAPTURE_PATH_MAX > (CAP_DIR_BUF - 1) + 1 + (CAP_BASE_BUF - 1) + sizeof("_099.jpg"),
               "CAPTURE_PATH_MAX too small: snprintf may truncate (-Werror=format-truncation=)");

/* ------------------------------------------------------------------------- */

static volatile capture_state_t s_state = CAPTURE_IDLE;
static capture_result_t         s_result;

/** 帧回调上下文。**只在 capture_task 与 cap_frame_cb 之间传递**（前者创建、后者使用）。 */
typedef struct {
    FILE    *fp;          /* 目标文件；NULL 表示"不要再写" */
    int      skip;        /* 还要丢掉的预热帧数 */
    uint32_t seen;        /* 收到的 JPEG 帧总数（含预热帧） */
    uint32_t bytes;       /* 真正写下去的那一帧的长度 */
    bool     got;         /* 已成功写完一帧 */
    int      ferrno;      /* 0 = 无错；否则是 fwrite 的 errno */
} cap_ctx_t;

/* ---------------------------------------------------------------------------
 *  帧回调（cam_task 上下文）
 * ------------------------------------------------------------------------- */
static void cap_frame_cb(const uint8_t *data, uint32_t size,
                         uint32_t w, uint32_t h, void *user)
{
    cap_ctx_t *c = (cap_ctx_t *)user;

    if (c == NULL || c->fp == NULL) return;   /* 已收工/已失效 */
    if (c->got || c->ferrno != 0)    return;   /* 已经有结论了，别改 */

    /* 只认媒体档位：别的档位（如 CAM 页 240x240 RGB565）来的帧一律不看 */
    if (w != CAM_MEDIA_W || h != CAM_MEDIA_H) return;
    if (size == 0) return;

    c->seen++;

    if (c->skip > 0) {                        /* 预热阶段：丢 */
        c->skip--;
        return;
    }

    /* ★ 一次性只写这一帧。**必须查返回值** —— record_service.c 的 fwrite 静默冻结
     *   （盘满时返回 0、local_bytes 不再增长、一句日志都没有）就是本工程的前车之鉴。 */
    size_t n = fwrite(data, 1, size, c->fp);
    if (n != size) {
        c->ferrno = errno ? errno : EIO;
        ESP_LOGE(TAG, "fwrite short: %u/%u bytes, errno=%d",
                 (unsigned)n, (unsigned)size, c->ferrno);
        return;
    }
    /* ★ 顺序不能颠倒：先写 bytes，最后才置 got ——
     *   任务侧把 got==true 当作"可以安全 fclose"的信号，提前置位就是提前关文件。 */
    c->bytes = size;
    c->got   = true;
}

/* ---------------------------------------------------------------------------
 *  路径生成
 * ------------------------------------------------------------------------- */

/** 建目录并算出唯一文件名。成功返回 0。 */
static int make_photo_path(char *out, size_t out_len)
{
    /* ★ 两道保险，缺一不可：
     *   ① 这条容量检查是**真防线**（拒绝过小的目标缓冲，避免 snprintf 静默截断），
     *      同时让 GCC 能推断出"目标 ≥ CAPTURE_PATH_MAX"，从而**证明**不会截断 ——
     *      本工程 `-Werror=format-truncation=`，证明不了就是编译错误。
     *   ② 上面的 _Static_assert 把"最坏输出 < 容量"锁在编译期。 */
    if (out_len < CAPTURE_PATH_MAX) {
        ESP_LOGE(TAG, "path buffer too small (%u < %d)", (unsigned)out_len, CAPTURE_PATH_MAX);
        return -1;
    }

    char dir[CAP_DIR_BUF];
    snprintf(dir, sizeof(dir), "%s%s", SDCARD_MNT_PATH, CAPTURE_DIR_NAME);

    /* FATFS 需要逐级建；/sdcard 由挂载提供，这里只补 photos 一级。
     * EEXIST 是正常情况（第二次拍照）。 */
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed, errno=%d", dir, errno);
        return -1;
    }

    char base[CAP_BASE_BUF];
    time_t now = time(NULL);
    struct tm tmv;
    /* 时间可信才用时间戳命名；TZ="CST-8" 已由 net_time_service 设置，localtime 即北京时间。 */
    if (net_time_service_time_valid() && now > 1600000000 &&
        localtime_r(&now, &tmv) != NULL) {
        strftime(base, sizeof(base), "IMG_%Y%m%d_%H%M%S", &tmv);
    } else {
        /* ★ 未校时时**绝不能**用 0 秒时间戳：那会让每张都叫 19700101_000000 而互相覆盖。
         *   退化成序号命名，靠下面的防撞循环递增。 */
        snprintf(base, sizeof(base), "IMG_SEQ");
    }

    /* 防撞名：同秒内连拍、或未校时的固定名，都会撞；撞了就加 _nnn 后缀。
     * ★ 上限 100 次是为了"绝不无限循环"—— 目录里真有 100 个同名前缀时直接失败，
     *   而不是把 UI 挂死。 */
    for (int n = 0; n < 100; n++) {
        if (n == 0) snprintf(out, out_len, "%s/%s.jpg", dir, base);
        else        snprintf(out, out_len, "%s/%s_%03d.jpg", dir, base, n);

        struct stat st;
        if (stat(out, &st) != 0) {
            return 0;                       /* 不存在 ⇒ 这个名字可用 */
        }
    }
    ESP_LOGE(TAG, "no free name in %s (100 collisions)", dir);
    return -1;
}

/* ---------------------------------------------------------------------------
 *  事务（一次性任务里跑，所以允许阻塞）
 * ------------------------------------------------------------------------- */
static void capture_task(void *arg)
{
    (void)arg;

    /* ★ 所有声明放函数顶部：本函数用 goto 做统一收尾，而"跳转跨过带初始化的声明"
     *   在 C 里虽然合法，却是典型的踩坑点（-Wjump-misses-init / 读未初始化值）。
     *   统一置零比"逐个判断是否已初始化"更不容易错。 */
    cap_ctx_t        ctx = {0};
    capture_result_t res = {0};
    TickType_t       t0  = xTaskGetTickCount();
    bool             started = false;
    esp_err_t        stop_err = ESP_OK;
    char             path[CAPTURE_PATH_MAX] = {0};

    /* ---- ① 前置检查：SD 卡 ----
     * ★ 先查卡、再建目录、**最后才开流**。顺序反了就会出现"相机已经被拧到 JPEG 档、
     *   却发现写不进去"的尴尬：白白黑一下画面，还得再切回来。 */
    if (!sdcard_service_is_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted, refuse capture");
        res.err = ESP_ERR_NOT_FOUND;
        goto finish;
    }

    /* ---- ② 路径 + 打开文件（**先开文件**，绝不把 fopen 放进帧回调）---- */
    if (make_photo_path(path, sizeof(path)) != 0) {
        res.err = ESP_FAIL;
        goto finish;
    }
    ctx.fp = fopen(path, "wb");
    if (ctx.fp == NULL) {
        ESP_LOGE(TAG, "fopen %s failed, errno=%d", path, errno);
        res.err = ESP_FAIL;
        goto finish;
    }
    ESP_LOGI(TAG, "capture -> %s", path);

    /* ---- ③ 接管：先停掉 CAM 页的预览（方案 §2.5.4 步骤①）----
     * ★ **只有"预览"才自动停**。"媒体"（别人正在录像）一律不抢 —— 那会把另一个
     *   会话的录像掐断。request() 已经挡过一次，这里再挡一次：从 request 返回到
     *   本任务真正跑起来之间有窗口，状态可能已经变了（本工程"别信旧状态"的老教训）。 */
    if (camera_service_owner() == CAM_OWNER_MEDIA) {
        ESP_LOGE(TAG, "camera owned by a MEDIA session, refuse capture");
        res.err = ESP_ERR_INVALID_STATE;
        goto finish;
    }
    if (camera_service_owner() != CAM_OWNER_NONE) {
        esp_err_t se = camera_service_stop();
        if (se != ESP_OK) {
            ESP_LOGE(TAG, "take over (stop preview) failed: %s", esp_err_to_name(se));
            res.err = se;
            goto finish;
        }
    }

    /* ---- ④ 起流（JPEG 1280x720，归属 MEDIA ⇒ CAM 页会看到"被占用"）---- */
    ctx.skip = CAP_WARMUP_SKIP;
    res.err = camera_service_start_fmt_owner(cap_frame_cb, &ctx,
                                             CAM_FMT_JPEG, CAM_MEDIA_W, CAM_MEDIA_H,
                                             CAM_OWNER_MEDIA);
    if (res.err != ESP_OK) {
        ESP_LOGE(TAG, "start JPEG stream failed: %s", esp_err_to_name(res.err));
        goto finish;
    }
    started = true;

    /* ---- ⑤ 等第一张可用帧（预热帧由回调自己丢）---- */
    {
        int waited = 0;
        while (!ctx.got && ctx.ferrno == 0 && waited < CAP_FRAME_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        if (ctx.ferrno != 0) {
            res.err = ESP_FAIL;          /* 写盘出错，回调里已经打过 errno */
        } else if (!ctx.got) {
            ESP_LOGE(TAG, "no usable frame in %d ms (seen %u)",
                     CAP_FRAME_WAIT_MS, (unsigned)ctx.seen);
            res.err    = ESP_ERR_TIMEOUT;
            ctx.skip   = 0;
            ctx.ferrno = ETIMEDOUT;      /* ★ 让回调立刻闭嘴，为下面的 fclose 让路 */
        }
    }

finish:
    /* ---- ⑥ 停流：**必须先停干净再说关文件** ----
     * ★ camera_service_stop() 会等 cam_task 真的退出（见该函数注释）。只有在它返回
     *   ESP_OK 之后，"不会再有帧回调触到那个 FILE" 才是**有保证的**。
     * ★ 若 stop 超时（相机卡死），**就不关这个 FILE**：宁可泄漏一个 FILE 结构，
     *   也不要和还活着的 cam_task 抢它 —— 那是确定的 UAF。下面会把这件事吼出来。 */
    if (started) {
        FILE *fp = ctx.fp;
        stop_err = camera_service_stop();
        ctx.fp = NULL;                   /* 无论成败都先让回调闭嘴 */
        if (stop_err == ESP_OK) {
            fclose(fp);
        } else {
            ESP_LOGE(TAG, "camera_service_stop timeout (%s): FILE intentionally leaked, "
                          "result discarded", esp_err_to_name(stop_err));
            if (res.err == ESP_OK) res.err = stop_err;
        }
    }

    /* ---- ⑦ 结果判定 + 失败时清掉半成品 ---- */
    if (res.err == ESP_OK) {
        res.bytes = ctx.bytes;
        snprintf(res.path, sizeof(res.path), "%s", path);
        ESP_LOGI(TAG, "photo OK: %u bytes, %u frames seen",
                 (unsigned)ctx.bytes, (unsigned)ctx.seen);
    } else if (started && stop_err == ESP_OK && path[0] != '\0') {
        /* 只在"确定没人再写"（stop 成功）时才敢删，否则会删掉正在写的文件。 */
        if (remove(path) != 0) {
            ESP_LOGW(TAG, "remove(%s) failed, errno=%d (leave a junk file)", path, errno);
        }
    }

    /* ---- ⑧ 归还：把 CAM 页的预览流拉回来（方案 §2.5.4 步骤⑤）----
     * ★ 这是"拍完自动回预览、人脸检测可继续用"（阶段 S2a 验收判据）的实现点。
     * ★ 只有在相机确实已经空出来（stop 成功）时才调；restore 自己也会拒绝"还占着"。
     * ★ 归还失败**不改变**本次拍照的结果 —— 照片已经落盘了，归还只影响回预览的体验，
     *   所以这里只 WARN，不把 res.err 改坏（改了就等于谎报"拍照失败"）。
     * ★ ESP_ERR_NOT_FOUND = 之前压根没有预览（用户没在 CAM 页按过 START），
     *   属于正常情况，连日志都不用打。 */
    if (started && stop_err == ESP_OK) {
        esp_err_t re = camera_service_restore_preview();
        if (re == ESP_OK) {
            ESP_LOGI(TAG, "preview restored");
        } else if (re != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "restore preview failed: %s", esp_err_to_name(re));
        }
    }

    res.frames_seen = ctx.seen;
    res.elapsed_ms  = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "capture done: err=%s, %u ms",
             esp_err_to_name(res.err), (unsigned)res.elapsed_ms);

    /* ★ 先写结果、后置 IDLE：读侧看到 IDLE 就能拿到完整结果
     *   （单写者 + volatile，够用；不去为此引入锁）。 */
    s_result = res;
    s_state  = CAPTURE_IDLE;

    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 *  对外 API
 * ------------------------------------------------------------------------- */
void capture_service_init(void)
{
    s_state = CAPTURE_IDLE;
    memset(&s_result, 0, sizeof(s_result));
    ESP_LOGI(TAG, "init (one-shot task, stack=%d, no resident cost)", CAP_TASK_STACK);
}

esp_err_t capture_service_request(void)
{
    if (s_state != CAPTURE_IDLE) {
        return ESP_ERR_INVALID_STATE;      /* 上一次还没跑完 */
    }
    if (!sdcard_service_is_mounted()) {
        return ESP_ERR_INVALID_STATE;      /* 没卡：让 UI 直接提示，别起任务白跑一趟 */
    }
    /* ★ 相机被**媒体会话**（别人在录像）持有时**拒绝**，而不是去抢：
     *   抢会把另一路录像掐断（而它可能已经录了几分钟）。
     * ★ 但被 **CAM 页预览**持有时**允许**："接管"正是方案 §2.5.4 步骤①的本意 ——
     *   用户切到 VIDEO 页按拍照，本来就期望把预览让出来，拍完再还回去。
     *   ⇒ 这里只拦 MEDIA，PREVIEW 交给任务里的接管逻辑处理。 */
    if (camera_service_owner() == CAM_OWNER_MEDIA) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state = CAPTURE_BUSY;

    BaseType_t ok = xTaskCreate(capture_task, "capture", CAP_TASK_STACK, NULL,
                                CAP_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        /* ★ 栈分配失败要如实说：本工程内部 RAM 余量本就只有 ~4 KB。 */
        ESP_LOGE(TAG, "xTaskCreate(capture) FAILED (internal RAM tight)");
        s_state = CAPTURE_IDLE;
        s_result.err = ESP_ERR_NO_MEM;
        s_result.elapsed_ms = 0;
        s_result.bytes = 0;
        s_result.path[0] = '\0';
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

capture_state_t capture_service_state(void) { return s_state; }

void capture_service_get_result(capture_result_t *out)
{
    if (out) *out = s_result;
}

void capture_service_clear_result(void)
{
    memset(&s_result, 0, sizeof(s_result));
}
