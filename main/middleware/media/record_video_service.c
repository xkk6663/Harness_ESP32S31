/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file record_video_service.c
 * @brief MJPEG AVI 录像服务实现（S2b）。取舍依据见 record_video_service.h 文件头。
 */

#include "record_video_service.h"
#include "avi_mjpeg_muxer.h"
#include "camera_service.h"
#include "sdcard_service.h"
#include "net_time_service.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAG "RECVID"

/* ★ 栈策略与 capture_service 一致：**一次性任务**（开录才建、收尾完即删），
 *   栈必须放**内部 RAM** —— 本任务要做 fopen/fwrite/fseek（FATFS → SDMMC），
 *   F35 明确"做文件/存储 IO 的任务栈放 PSRAM 会在 flash cache 关闭窗口里崩"。
 *   ★ 收尾的重扫（可能几秒）也跑在这条栈上，所以不能给太小；4096 是实测前的保守值，
 *     上机后按 uxTaskGetStackHighWaterMark 再调（本工程有 `[stack]` 水位总表）。 */
#define REC_TASK_STACK   4096
#define REC_TASK_PRIO    5

/* 主循环节拍：20 ms。它只做"刷新统计 + 查上限 + 查停止请求"，
 * 真正的写盘在帧回调里，所以这个周期不影响录像帧率。 */
#define REC_POLL_MS      20

/* ---- 路径拼接的容量预算（**和 -Werror=format-truncation 直接相关，别随手改小**）----
 * ★ 本工程真实 build 报过：
 *   `record_video_service.c:106:48: error: '%s' directive output may be truncated writing up to
 *    39 bytes into a region of size between 32 and 95 [-Werror=format-truncation=]`
 *   当时 VIDEO_REC_PATH_MAX 还是 96，而最坏输出 = 63 + 1 + 39 + "_099.avi"(8) + NUL = 112 ⇒ 必然截断。
 * ★ 这个诊断**依赖内联**：只有 make_video_path() 被内联进 rec_task() 后，GCC 才知道目标数组
 *   的真实大小（日志里 "inlined from 'rec_task'" 就是它）。⇒ **单文件静态校验看不到这个错**，
 *   只有真机 build 才会炸 —— 这就是它当初能溜过去的原因。
 * ★ _Static_assert 把"最坏输出 < 容量"锁在编译期：谁把容量调小（或把 dir/base 缓冲放大）
 *   都会立刻在这里报错，而不是等 `idf.py build` 才发现。 */
#define REC_DIR_BUF      64
#define REC_BASE_BUF     40
_Static_assert(VIDEO_REC_PATH_MAX > (REC_DIR_BUF - 1) + 1 + (REC_BASE_BUF - 1) + sizeof("_099.avi"),
               "VIDEO_REC_PATH_MAX too small: snprintf may truncate (-Werror=format-truncation=)");

/* ------------------------------------------------------------------------- */

static volatile video_rec_state_t s_state   = VIDEO_REC_IDLE;
static volatile bool              s_stop_req = false;
/* ★ 帧回调与收尾之间的**唯一**闸门：false ⇒ 回调立刻返回，绝不碰 muxer。
 *   置 false 的时机见 rec_task() 的 ⑤/⑥（必须等 cam_task 真的退出）。 */
static volatile bool              s_rec_live = false;
static volatile avi_result_t      s_werr     = AVI_OK;
static TaskHandle_t               s_task     = NULL;
static avi_muxer_t               *s_muxer    = NULL;   /* **只有服务任务**碰它 */

static video_rec_stats_t  s_stats;
static video_rec_result_t s_result;

/* ---------------------------------------------------------------------------
 *  PSRAM 批缓冲分配器（给 muxer 的 close() 用，见其 .h 的钩子说明）
 *
 *  ★ 为什么必须有：muxer 在主 RAM 上申请 4 KB 批缓冲，而本工程内部堆只剩 ~4 KB 余量
 *    ⇒ 大概率失败 ⇒ 退化成"无索引 AVI"，白白丢掉 S2b 的验收项。
 *  ★ PSRAM 也拿不到时**退内部堆**（而不是直接放弃）：能让"这一卷录像是完整的"优先；
 *    真失败时 muxer 自己会走降级路径并清掉 HASINDEX，UI 用 index_ok 如实提示。
 * ------------------------------------------------------------------------- */
static void *rec_batch_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = malloc(n);
    }
    return p;
}

static void rec_batch_release(void *p)
{
    heap_caps_free(p);        /* heap_caps_* 分配的块用它对；内部堆的块也能用 */
}

/* ---------------------------------------------------------------------------
 *  路径生成（与 capture_service 同套路：时间戳优先，未校时退化序号）
 * ------------------------------------------------------------------------- */
static int make_video_path(char *out, size_t out_len)
{
    /* ★ 容量检查放在所有 snprintf **之前**：既是真防线（拒绝过小缓冲，避免静默截断），
     *   也让 GCC 能推断出"目标 ≥ VIDEO_REC_PATH_MAX"，从而证明不会截断。 */
    if (out_len < VIDEO_REC_PATH_MAX) {
        ESP_LOGE(TAG, "path buffer too small (%u < %d)", (unsigned)out_len, VIDEO_REC_PATH_MAX);
        return -1;
    }

    char dir[REC_DIR_BUF];
    snprintf(dir, sizeof(dir), "%s%s", SDCARD_MNT_PATH, VIDEO_REC_DIR_NAME);

    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed, errno=%d", dir, errno);
        return -1;
    }

    char base[REC_BASE_BUF];
    time_t now = time(NULL);
    struct tm tmv;
    if (net_time_service_time_valid() && now > 1600000000 &&
        localtime_r(&now, &tmv) != NULL) {
        strftime(base, sizeof(base), "VID_%Y%m%d_%H%M%S", &tmv);
    } else {
        snprintf(base, sizeof(base), "VID_SEQ");   /* 未校时：序号命名，别用 0 秒时间戳 */
    }

    for (int n = 0; n < 100; n++) {
        if (n == 0) snprintf(out, out_len, "%s/%s.avi", dir, base);
        else        snprintf(out, out_len, "%s/%s_%03d.avi", dir, base, n);

        struct stat st;
        if (stat(out, &st) != 0) {
            return 0;
        }
    }
    ESP_LOGE(TAG, "no free name in %s (100 collisions)", dir);
    return -1;
}

/* ---------------------------------------------------------------------------
 *  帧回调（cam_task 上下文）
 *
 *  ★★ 这里**同步写盘**。这是本模块最重要的取舍，完整理由见 .h 文件头。
 *     一句话：方案 §5.1 要的是"宁可降 fps 也不丢帧"，同步写正好满足，
 *     且零额外缓冲（不用赌 720p JPEG 到底多大）。
 *  ★ 绝不能在这里打日志：12 fps × 每条 UART 日志 ~ms 级会真的开始丢帧，
 *     而且会污染"实测 fps"这个验收数据。要日志请让服务任务打。
 * ------------------------------------------------------------------------- */
static void rec_frame_cb(const uint8_t *data, uint32_t size,
                         uint32_t w, uint32_t h, void *user)
{
    avi_muxer_t *m = (avi_muxer_t *)user;

    if (!s_rec_live || m == NULL) return;      /* 已进入收尾 */
    if (s_werr != AVI_OK)         return;      /* 已经写坏过，别再试 */
    if (w != CAM_MEDIA_W || h != CAM_MEDIA_H || size == 0) return;

    avi_result_t r = avi_muxer_add_jpeg(m, data, size);
    if (r != AVI_OK) {
        /* ★ 写失败 = 卡满 / 卡被拔 / 只读。记下来并**请求停止**：
         *   继续录只会不断失败，而且 AVI 尾部会越写越乱。
         *   服务任务看到后走 abort 路径（只 fclose，不再 fseek 回填）。 */
        s_werr     = r;
        s_stop_req = true;
    }
}

/* ---------------------------------------------------------------------------
 *  实时统计发布（只有服务任务调用；UI 只读 s_stats，绝不碰 s_muxer）
 * ------------------------------------------------------------------------- */
static void publish_stats(uint64_t t0_us)
{
    if (s_muxer) {
        avi_stats_t st;
        avi_muxer_get_stats(s_muxer, &st);
        s_stats.frames    = st.frames;
        s_stats.bytes     = (uint32_t)st.total_bytes;
        s_stats.max_frame = st.max_frame_size;
    }
    uint64_t us = (uint64_t)(esp_timer_get_time() - (int64_t)t0_us);
    s_stats.seconds = (uint32_t)(us / 1000000ULL);
    /* 实测平均帧率（定点 ×100）。★ 帧数为 0 时保持 0，UI 显示 "--"，
     *   绝不在这里用标称 12 兜底 —— 那会让用户以为"已经跑到 12 fps 了"。 */
    s_stats.fps_x100 = (s_stats.frames > 0 && us > 0)
                     ? (uint32_t)((uint64_t)s_stats.frames * 100ULL * 1000000ULL / us)
                     : 0u;
}

/* ---------------------------------------------------------------------------
 *  服务任务（一次性）：开容器 → 起流 → 守着 → 停流 → 收尾
 * ------------------------------------------------------------------------- */
static void rec_task(void *arg)
{
    (void)arg;

    char        path[VIDEO_REC_PATH_MAX] = {0};
    esp_err_t   err      = ESP_OK;
    bool        stream_on = false;
    bool        mux_open  = false;
    bool        stopped   = true;      /* 未起流时即为 true（没有 cam_task 要等） */
    bool        aborted   = false;
    bool        idx_ok    = false;     /* 只有 close() 真返回 AVI_OK 才是 true */
    avi_result_t close_r  = AVI_OK;
    const char *reason    = "user";
    uint64_t    t0        = (uint64_t)esp_timer_get_time();

    memset(&s_stats, 0, sizeof(s_stats));

    /* ---- ① 前置检查 ---- */
    if (!sdcard_service_is_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted, refuse REC");
        err = ESP_ERR_NOT_FOUND;
    } else if (camera_service_owner() == CAM_OWNER_MEDIA) {
        /* 已经有另一路媒体会话在跑 —— 绝不抢（那会掐断别人的录像） */
        ESP_LOGE(TAG, "camera owned by another MEDIA session, refuse REC");
        err = ESP_ERR_INVALID_STATE;
    } else if (make_video_path(path, sizeof(path)) != 0) {
        err = ESP_FAIL;
    }

    /* ---- ①-b 接管：停掉 CAM 页的预览（方案 §2.5.4 步骤①）----
     * ★ 放在**开容器之前**：接管失败就不该留下一个空 .avi。
     * ★ 只有 PREVIEW 才自动停；MEDIA 上面已经拒绝过了。 */
    if (err == ESP_OK && camera_service_owner() != CAM_OWNER_NONE) {
        esp_err_t se = camera_service_stop();
        if (se != ESP_OK) {
            ESP_LOGE(TAG, "take over (stop preview) failed: %s", esp_err_to_name(se));
            err = se;
        }
    }

    /* ---- ② 先开容器（**先开文件**：把 fopen 挡在帧回调之外）---- */
    if (err == ESP_OK) {
        s_muxer = avi_muxer_open(path, CAM_MEDIA_W, CAM_MEDIA_H, VIDEO_REC_NOMINAL_FPS);
        if (s_muxer == NULL) {
            ESP_LOGE(TAG, "avi_muxer_open(%s) failed", path);
            err = ESP_FAIL;
        } else {
            mux_open = true;
            snprintf(s_stats.path, sizeof(s_stats.path), "%s", path);
            ESP_LOGI(TAG, "recording -> %s", path);
        }
    }

    /* ---- ③ 起流（JPEG 1280x720，归属 MEDIA）---- */
    if (err == ESP_OK) {
        err = camera_service_start_fmt_owner(rec_frame_cb, s_muxer,
                                             CAM_FMT_JPEG, CAM_MEDIA_W, CAM_MEDIA_H,
                                             CAM_OWNER_MEDIA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start JPEG stream failed: %s", esp_err_to_name(err));
            reason = "stream start failed";
        } else {
            stream_on = true;
        }
    }

    /* ---- ④ 主循环：只做统计 + 上限 + 停止请求，不参与写盘 ---- */
    while (stream_on && !s_stop_req) {
        publish_stats(t0);

        if (s_werr != AVI_OK) {
            ESP_LOGE(TAG, "muxer write failed (code %d): card full / removed?", (int)s_werr);
            err    = ESP_FAIL;
            reason = "write error";
            break;
        }
        if (s_stats.seconds >= VIDEO_REC_MAX_SECONDS) {
            reason = "max duration";       /* 上限到了也要**正常收尾**，不留坏文件 */
            break;
        }
        if (s_stats.bytes >= VIDEO_REC_MAX_BYTES) {
            reason = "max size";
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(REC_POLL_MS));
    }
    if (!stream_on && err == ESP_OK) {
        reason = "stream not started";
    }

    /* ---- ⑤ 停流：这是"能不能安全收尾"的分水岭 ---- */
    s_state = VIDEO_REC_FINALIZING;
    if (stream_on) {
        esp_err_t se = camera_service_stop();
        stopped = (se == ESP_OK);
        if (!stopped) {
            ESP_LOGE(TAG, "camera_service_stop timeout: %s", esp_err_to_name(se));
            err    = se;
            reason = "camera stop timeout";
        }
    } else {
        stopped = true;      /* 从来没起过流 ⇒ 没有 cam_task，没人碰 muxer */
    }
    /* ★ 无论成败都先关闸：从这一刻起帧回调不会再碰 s_muxer。 */
    s_rec_live = false;

    /* ---- ⑥ 收尾 ---- */
    if (mux_open) {
        if (!stopped) {
            /* ★★ 相机任务没退 ⇒ 帧回调可能**正在** muxer 里写那块内存。
             *    此时 close() / abort() 都是确定的 UAF。宁可泄漏一个 muxer 结构 + 一个 FILE，
             *    也绝不和活着的 cam_task 抢它。这里明确吼出来让用户知道要重启。 */
            ESP_LOGE(TAG, "cam_task still alive -> muxer/FILE intentionally leaked; "
                          "%s is very likely unplayable", path);
            s_muxer = NULL;
        } else if (err == ESP_OK) {
            publish_stats(t0);
            uint32_t fps = s_stats.fps_x100 ? s_stats.fps_x100
                                            : (VIDEO_REC_NOMINAL_FPS * 100u);
            /* ★ 用实测平均 fps 覆盖标称值：否则播放器按 12 fps 播，时长会比真实录制短，
             *   直接违反 S2b 的验收项"时长与录制时间一致"。 */
            avi_muxer_set_real_fps(s_muxer, fps);
            close_r = avi_muxer_close(s_muxer);
            if (close_r != AVI_OK) {
                ESP_LOGE(TAG, "avi_muxer_close failed (code %d)", (int)close_r);
                err    = ESP_FAIL;
                reason = "finalize failed";
            } else {
                idx_ok = true;          /* ★ 只有真的写出 idx1 才算有索引 */
            }
            s_muxer = NULL;
        } else {
            /* ★ 出错（写失败 / 卡没了）时**不要用 close()**：它要 fseek 回文件头写十几处、
             *   还要重扫整个 movi 区生成 idx1 —— 在坏卡上这些必然会再失败一遍，
             *   白白拖时间且毫无收益。abort() 只 flush + fclose，失败面最小
             *   （这正是 avi_mjpeg_muxer.h 里 abort 存在的理由）。 */
            ESP_LOGW(TAG, "abort muxer (reason=%s), file will lack idx1", reason);
            avi_muxer_abort(s_muxer);
            s_muxer = NULL;
            aborted = true;
        }
    }

    /* ---- ⑦ 归还：把 CAM 页的预览流拉回来（方案 §2.5.4 步骤⑤）----
     * ★ 只有相机确实空出来了（stopped）才调；restore 自己也会拒绝"还占着"。
     * ★ 失败不改 err：本次录像的成败已经定了，归还只影响"回预览"的体验，
     *   改 err 就等于谎报"录像失败"。
     * ★ ESP_ERR_NOT_FOUND = 用户从没在 CAM 页按过 START，属于正常，不用打日志。 */
    if (stopped) {
        esp_err_t re = camera_service_restore_preview();
        if (re == ESP_OK) {
            ESP_LOGI(TAG, "preview restored");
        } else if (re != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "restore preview failed: %s", esp_err_to_name(re));
        }
    }

    /* ---- ⑧ 发布结果 ----
     * ★ 先写结果、后置 IDLE，读侧看到 IDLE 即可拿到完整结果。 */
    s_result.err       = err;
    s_result.frames    = s_stats.frames;
    s_result.bytes     = s_stats.bytes;
    s_result.max_frame = s_stats.max_frame;
    s_result.seconds   = s_stats.seconds;
    s_result.fps_x100  = s_stats.fps_x100;
    s_result.index_ok  = idx_ok;
    snprintf(s_result.path, sizeof(s_result.path), "%s", path);

    ESP_LOGI(TAG, "rec done: err=%s reason=%s frames=%u bytes=%u max_frame=%u "
                  "fps=%u.%02u sec=%u%s",
             esp_err_to_name(err), reason,
             (unsigned)s_result.frames, (unsigned)s_result.bytes,
             (unsigned)s_result.max_frame,
             (unsigned)(s_result.fps_x100 / 100u), (unsigned)(s_result.fps_x100 % 100u),
             (unsigned)s_result.seconds,
             aborted ? " [aborted, no idx1]" : "");

    s_state = VIDEO_REC_IDLE;
    s_task  = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 *  对外 API
 * ------------------------------------------------------------------------- */
void record_video_service_init(void)
{
    avi_muxer_set_batch_allocator(rec_batch_alloc, rec_batch_release);
    ESP_LOGI(TAG, "init (muxer idx-batch buffer -> PSRAM; finalize runs in service task)");
}

esp_err_t record_video_service_start(void)
{
    /* ★ 只查两条：任务残留 / SD 卡。
     *   相机占用**不在这里拦** —— CAM 页预览是允许被接管的（方案 §2.5.4 步骤①），
     *   真正的判断放在 rec_task 里（那里才能安全地停别人的流）。 */
    if (s_state != VIDEO_REC_IDLE || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sdcard_service_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_result, 0, sizeof(s_result));
    s_werr     = AVI_OK;
    s_stop_req = false;
    s_rec_live = true;                  /* 先开闸，再建任务（任务里就会起流） */
    s_state    = VIDEO_REC_RECORDING;

    if (xTaskCreate(rec_task, "recvid", REC_TASK_STACK, NULL, REC_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(recvid) FAILED (internal RAM tight)");
        s_rec_live = false;
        s_state    = VIDEO_REC_IDLE;
        s_task     = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t record_video_service_stop(void)
{
    if (s_state == VIDEO_REC_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 只举手，不等待：等由服务任务做（大文件重扫要好几秒）。
     *   在 LVGL 线程里同步等 = UI 直接僵住，这是本工程反复强调的红线。 */
    s_stop_req = true;
    return ESP_OK;
}

bool record_video_service_is_recording(void)
{
    return (s_state == VIDEO_REC_RECORDING);
}

video_rec_state_t record_video_service_state(void)
{
    return s_state;
}

void record_video_service_get_stats(video_rec_stats_t *out)
{
    if (out) *out = s_stats;
}

void record_video_service_get_result(video_rec_result_t *out)
{
    if (out) *out = s_result;
}

void record_video_service_clear_result(void)
{
    memset(&s_result, 0, sizeof(s_result));
}
