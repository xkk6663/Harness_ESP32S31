/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file jpeg_util.c
 * @brief JPEG 软编码实现。设计与取舍见 jpeg_util.h 文件头。
 */

#include "jpeg_util.h"

#include "esp_jpeg_common.h"
#include "esp_jpeg_enc.h"

#include "esp_log.h"
#include "esp_timer.h"          /* ★ 编码耗时（R4 的验收数据） */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#define TAG "JPEG_UTIL"

/* ===========================================================================
 *  ★ 为什么这里要自己加锁（而不是让调用方保证串行）
 * ---------------------------------------------------------------------------
 *  两个调用方天然可能并发：
 *    · punch_service 的一次性落盘任务（打卡存证）；
 *    · 录入流程里写"底片"的那一次（走的是识别引擎的任务）。
 *  两者都是"低频、短促"，用一把互斥锁把它们串起来，代价为零；
 *  而如果改成"约定两个任务不许同时进来"，那就是把一个**容易违反的约定**
 *  写进了注释里 —— 本工程在这类地方一贯选择"从 API 形状上消除竞态"。
 *
 *  ★ 句柄缓存策略：只在 (w,h,quality) 变化时才 close/open。
 *    240x240 是定长输入，所以正常路径上**只 open 一次**。
 * =========================================================================== */
static SemaphoreHandle_t s_lock;
static jpeg_enc_handle_t s_enc;
static int               s_w, s_h, s_q;

/* 惰性建锁：不能在静态初始化里建（调度器未必就绪），也不能用 ESP_ERROR_CHECK
 * （那是"资源紧张就重启"的写法，本工程中间层一律拒绝）。 */
static bool util_lock_init(void)
{
    if (s_lock) return true;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex FAILED (internal RAM tight)");
        return false;
    }
    return true;
}

/* 关掉当前句柄（把 (w,h,q) 一起复位，让下次必然重开） */
static void enc_close_locked(void)
{
    if (s_enc) {
        jpeg_enc_close(s_enc);
        s_enc = NULL;
    }
    s_w = s_h = s_q = 0;
}

void jpeg_util_deinit(void)
{
    if (!util_lock_init()) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    enc_close_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "encoder released");
}

esp_err_t jpeg_util_encode_rgb888(const uint8_t *rgb, int w, int h, int quality,
                                  uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len) *out_len = 0;

    if (!rgb || !out || w <= 0 || h <= 0 || quality < 1 || quality > 100) {
        ESP_LOGE(TAG, "bad args: rgb=%p out=%p w=%d h=%d q=%d",
                 (const void *)rgb, (void *)out, w, h, quality);
        return ESP_ERR_INVALID_ARG;
    }
    /* ★ 16 字节对齐是 esp_jpeg 的硬要求（见 .h）。先拦下来说清楚，
     *   而不是让它在编码器内部按"数据损坏"报一个看不懂的错。 */
    if (((uintptr_t)rgb & 0xF) != 0) {
        ESP_LOGE(TAG, "input buffer not 16-byte aligned (%p) - use heap_caps_aligned_alloc",
                 (const void *)rgb);
        return ESP_ERR_INVALID_ARG;
    }

    if (!util_lock_init()) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    int64_t   t0  = esp_timer_get_time();

    /* ---- 句柄：几何/质量不变则复用 ---- */
    if (s_enc == NULL || s_w != w || s_h != h || s_q != quality) {
        enc_close_locked();

        jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
        cfg.width     = w;
        cfg.height    = h;
        cfg.src_type  = JPEG_PIXEL_FORMAT_RGB888;
        /* ★ 4:2:0 抽样：与传感器硬件档一致，体积最小。
         *   本工程用途是"存证/底片"，肉眼能认出人即可，不需要 4:4:4。 */
        cfg.subsampling = JPEG_SUBSAMPLE_420;
        cfg.quality   = (uint8_t)quality;
        cfg.rotate    = JPEG_ROTATE_0D;
        /* ★ 不建编码线程：本模块的调用点都已经是"独立任务里的一次性动作"，
         *   再开一个任务只会多占内部栈（本工程内部 RAM 只有 ~4 KB 余量）。 */
        cfg.task_enable = false;

        jpeg_error_t e = jpeg_enc_open(&cfg, &s_enc);
        if (e != JPEG_ERR_OK || s_enc == NULL) {
            ESP_LOGE(TAG, "jpeg_enc_open failed: %d (%dx%d q=%d)", (int)e, w, h, quality);
            s_enc = NULL;
            ret = ESP_FAIL;
            goto out;
        }
        s_w = w; s_h = h; s_q = quality;
        ESP_LOGI(TAG, "encoder opened: %dx%d q=%d", w, h, quality);
    }

    {
        int in_bytes = w * h * 3;
        int out_size = 0;
        jpeg_error_t e = jpeg_enc_process(s_enc, rgb, in_bytes, out, (int)out_cap, &out_size);
        if (e != JPEG_ERR_OK) {
            /* ★ 唯一需要区分的一条：缓冲不够。其余都是编码器内部错误。
             *   （`out_cap` 是调用方给的，写不下一律如实报错，绝不静默截断
             *     —— 截断的 JPEG 在 PC 上会变成"半张图"，比失败还难查。） */
            ESP_LOGE(TAG, "jpeg_enc_process failed: e=%d out_size=%d cap=%u (%dx%d)",
                     (int)e, out_size, (unsigned)out_cap, w, h);
            ret = ESP_FAIL;
            goto out;
        }
        if (out_size <= 0 || (size_t)out_size > out_cap) {
            ESP_LOGE(TAG, "bad encoded size %d (cap %u)", out_size, (unsigned)out_cap);
            ret = ESP_ERR_INVALID_SIZE;
            goto out;
        }
        if (out_len) *out_len = (size_t)out_size;

        /* ★ R4 的验收数据就在这一行：软编单张的**实测**耗时。
         *   预期 20~60 ms（240×240 q80）；> 200 ms 才需要降 Q 或降分辨率。
         *   频率很低（每次打卡/每次录入底片各一次），不需要限频。 */
        ESP_LOGI(TAG, "enc %dx%d q=%d -> %d B in %u ms",
                 w, h, quality, out_size,
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
    }

out:
    xSemaphoreGive(s_lock);
    return ret;
}
