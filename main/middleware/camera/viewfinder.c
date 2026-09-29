/* viewfinder.c - VIDEO 页实时取景器实现（硬件 JPEG 解码 + 稀疏抽点）
 *
 * 设计与证据链见 viewfinder.h 文件头；这里只写实现层的硬约束。
 *
 * 【★ 颜色字节序是钉死的，不要再"调一下试试"】
 *   2D-DMA 的字节重排枚举（esp_hal_dma/include/hal/dma2d_types.h:133-141）明确写着
 *   "This scramble only fits 3 bytes/pixel format. For non-3 bytes/pixel format,
 *    the pixels will be messed up if scrambling is used."
 *   而 esp_driver_jpeg/jpeg_decode.c:563-575 只在 rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB
 *   时才给 RGB565 挂 scramble（BYTE2_0_1）⇒ 对 2 字节/像素的 RGB565 那是**必错**的。
 *   ⇒ RGB565 必须用 JPEG_DEC_RGB_ELEMENT_ORDER_BGR（= 不 scramble）。
 *   BGR 在 jpeg_types.h 里的注释就是 "Output the color component in small endian"，
 *   即小端 —— 正是 LVGL RGB565（小端 CPU 直接按 uint16 读）需要的。
 *   （对照组：tab_camera 收到的 OV3660 原生 RGB565 是**大端**，所以才要逐像素 swap。）
 *
 * 【线程模型】
 *   cam_task（core 1, prio 5）          —— 观察者：限速 + memcpy staging + give 信号量
 *   vf 任务（core 1, prio 4）           —— 等信号量 → 硬件解码 → 稀疏抽点 → 发布
 *   LVGL 线程                           —— viewfinder_take() 读已发布的缓冲指针
 *   两条核心都在 core 1、vf 优先级更低 ⇒ 解码永远不会抢 cam_task 的出帧，
 *   LVGL（affinity -1）也只会在被抢占时晚几 ms，不会饿死。
 */

#include "viewfinder.h"

#include "driver/jpeg_decode.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "middleware/camera/camera_service.h"
#include "middleware/mem_diag.h"       /* ★ 按需化的 acquire/release 各打一条现场快照 */

#include <string.h>

#define TAG "VFD"

/* ---- 源档位（本工程唯一收录的媒体档，见 ov3660_fmt.c / camera_service.h）----
 * 用头文件宏（文件预览/播放也按它算抽点），这里只做一致性锚定。 */
#define SRC_W   VF_SRC_W
#define SRC_H   VF_SRC_H
_Static_assert(SRC_W == 1280u && SRC_H == 720u, "viewfinder source size drift");

/* ---- 稀疏抽点倍率 ----
 * 1280 = 2^8 * 5，720 = 2^4 * 3^2 * 5 ⇒ 公约数 1,2,4,5,8,10,16,20...
 * 1/2 (640x360) 每帧要读 921 KB，1/8 (160x90) 太小 ⇒ 取 1/4 (320x180)。 */
#define VF_STEP 4u

/* 硬件解码器不支持缩放（jpeg_decoder_process 没有 scale/roi 参数）⇒ 全尺寸出图。
 * 4:2:2 的块布局会把输出补齐到 16 的整数倍（jpeg_decode.h note 2）；
 * 1280 与 720 本来就是 16 的倍数 ⇒ 恰好无补齐。用 out_size 与此对账（见 vf_task）。 */
#define DEC_OUT_BYTES   (SRC_W * SRC_H * 2u)        /* 1,843,200 */

/* staging：JPEG 码流中转。
 * ★★ 上限必须是"驱动单缓冲能交付的最大帧"：esp_video.c:2164-2184 对 JPEG 用
 *     w*h*bpp/8（bpp=8，esp_video.c:84）= 1280*720 = 921,600 B，本工程 S_FMT
 *     不设 sizeimage ⇒ 这就是驱动缓冲上限，比它大的帧根本进不了 V4L2。
 *   【教训】192 KB 时代的实现：Q8~Q24 帧才 30~70 KB 绰绰有余，但 Q1（最高质量）
 *     单帧轻松超 192 KB ⇒ vf_observe 每帧都丢 ⇒ 取景黑屏（2026-09-26 用户实报）。
 *   代价：常驻 PSRAM +720 KB（viewfinder 总占用 ~2.2 MB → ~2.97 MB），换来
 *     "任何质量档预览都不可能因 staging 而丢帧"。 */
#define STAGE_BYTES     (SRC_W * SRC_H)     /* 921,600 = 驱动 JPEG 缓冲上限 */

/* 取景限速。传感器 12 fps，这里只取 ~6.7 fps：取景够流畅，
 * 又把 PSRAM 带宽（~7 MB/s）与解码 CPU 留给录像/显示。
 * ★ 若上机发现"实测 fps"被拉低，优先调大这个值（如 200000）。 */
#define VF_MIN_INTERVAL_US   150000

/* 解码任务：只跑驱动调用 + 抽点循环，4096 内部栈（同 cam_task 的栈策略：
 * 内部 RAM 才 4 KB，而本任务不做任何文件 IO，放内部最稳）。 */
#define VF_TASK_STACK    4096
#define VF_TASK_PRIO     4
#define VF_TASK_CORE     1        /* 与 cam_task 同核：cam_task(5) > vf(4) ⇒ 永远抢得过 */

/* ---------------------------------------------------------------------------
 *  数据缓冲（2026-09-28 起：acquire 分配 / release 释放，不再开机常驻）
 * ------------------------------------------------------------------------- */
static uint8_t *s_dec_out   = NULL;     /* 硬件解码出图（RGB565 全尺寸） */
static size_t   s_dec_out_sz = 0;
static uint8_t *s_stage     = NULL;     /* JPEG 码流中转 */
static uint16_t *s_disp[2]  = {NULL, NULL};   /* 显示缓冲（320x180 RGB565） */

/* 解码引擎：进程内**只建一次**、常驻复用（见 viewfinder_acquire 的说明）。
 * ★ 它只占内部 RAM 的小结构（DMA 链表描述符 / 事件队列 / codec handle），
 *   数据缓冲全在上面那些 s_* 里 ⇒ 引擎常驻不违背"按需"的目标（目标是 PSRAM）。 */
static jpeg_decoder_handle_t s_dec = NULL;
static bool s_engine_up = false;        /* s_dec 是否已建（防 acquire 循环重复 new） */

/* ---- 状态（均为单写者或多写者但只翻转 bool 的模式，volatile 即可）---- */
static volatile bool s_inited  = false;
static volatile bool s_enabled = false;
static TaskHandle_t  s_task    = NULL;
static SemaphoreHandle_t s_sem = NULL;      /* 二值信号量：staging 就绪 */

/* 观察者 → 解码任务的交接（单生产者 cam_task / 单消费者 vf 任务） */
static volatile bool     s_stage_ready = false;
static volatile uint32_t s_stage_size  = 0;
static int64_t           s_last_accept_us = 0;

/* 解码任务 → LVGL 的发布（双缓冲指针 + 序号） */
static volatile uint32_t s_latest_idx = 0;  /* 0 / 1：哪块显示缓冲是最新 */
static volatile uint32_t s_seq        = 0;  /* 已发布帧数（0 = 还没有） */
static uint32_t          s_seen_seq   = 0;  /* LVGL 侧已看过的序号 */

/* 诊断 */
static volatile uint32_t s_decoded  = 0;
static volatile uint32_t s_dropped  = 0;
static volatile uint32_t s_last_us  = 0;
/* 超限告警只吼一次的旗标 —— 文件级，enable 时清零（否则复测/换档后永远看不到告警） */
static bool s_oversize_warned = false;

/* ---------------------------------------------------------------------------
 *  观察者（cam_task 上下文）—— 本模块唯一会被流回调的入口
 * ------------------------------------------------------------------------- */
static void vf_observe(const uint8_t *data, uint32_t size,
                       uint32_t w, uint32_t h, void *user)
{
    (void)user;

    if (!s_enabled)            return;
    if (w != SRC_W || h != SRC_H) return;   /* 只认 720p 媒体档；RGB565 预览流不归我们管 */
    if (s_stage_ready)         return;      /* 上一帧还没解码完 ⇒ 丢（限速的一部分） */
    if (size == 0 || size > STAGE_BYTES) {
        if (!s_oversize_warned) {
            ESP_LOGW(TAG, "frame size %u > staging %u, drop (raise STAGE_BYTES)",
                     (unsigned)size, (unsigned)STAGE_BYTES);
            s_oversize_warned = true;       /* 只吼一次：每帧打日志会拖垮 fps */
        }
        s_dropped++;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_last_accept_us < VF_MIN_INTERVAL_US) {
        s_dropped++;                        /* 限速丢帧是常态，不计入异常 */
        return;
    }
    s_last_accept_us = now;

    /* ★ 唯一的拷贝。~90 KB PSRAM 顺序拷贝 ≈ 1 ms —— 这是 cam_task 里为本模块
     *   花掉的全部时间。绝不能在这里解码（那是 10 ms 级，会让 DVP 连续丢帧）。 */
    memcpy(s_stage, data, size);
    s_stage_size  = size;
    s_stage_ready = true;                   /* 放在 memcpy 之后：消费者看到 ready 即数据完整 */
    xSemaphoreGive(s_sem);
}

/* ---------------------------------------------------------------------------
 *  稀疏抽点：1280x720 -> 320x180
 *  ★ 每帧只读 180/720 行 = 460 KB（跳过的行完全不碰 ⇒ 不产生 PSRAM 事务）。
 * ------------------------------------------------------------------------- */
static void vf_subsample(const uint16_t *src, uint16_t *dst)
{
    for (uint32_t y = 0; y < VF_H; y++) {
        const uint16_t *s = src + (uint32_t)(y * VF_STEP) * SRC_W;
        uint16_t       *d = dst + y * VF_W;
        for (uint32_t x = 0; x < VF_W; x++) {
            d[x] = s[x * VF_STEP];
        }
    }
}

/* ---------------------------------------------------------------------------
 *  解码任务
 * ------------------------------------------------------------------------- */
static void vf_task(void *arg)
{
    (void)arg;
    uint32_t dst_idx = 0;

    while (s_enabled) {
        if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;                       /* 超时只是回头看 s_enabled */
        }
        if (!s_enabled) break;
        if (!s_stage_ready) continue;       /* 理论上不会发生，防呆 */

        const uint32_t sz = s_stage_size;
        int64_t t0 = esp_timer_get_time();

        jpeg_decode_cfg_t cfg = {
            /* ★ 字节序必须 BGR（= 不 scramble），证据见文件头，不要改成 RGB。 */
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        esp_err_t err = jpeg_decoder_process(s_dec, &cfg, s_stage, sz,
                                             s_dec_out, s_dec_out_sz, &out_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "jpeg_decoder_process failed: %s (frame %u B)",
                     esp_err_to_name(err), (unsigned)sz);
            s_dropped++;
            s_stage_ready = false;          /* 解码失败也要放锁，否则观察者从此全丢 */
            continue;
        }
        /* ★ 对账：out_size 与"假设的 1280x720 无补齐"必须一致。这是把
         *   "4:2:2 输出补齐到 16 的倍数"这条文档约束变成运行期硬判据 ——
         *   一旦供应商表换档位/尺寸，这里会立刻响亮失败而不是画面错乱。 */
        if (out_size != DEC_OUT_BYTES) {
            ESP_LOGE(TAG, "decoded size %u != expected %u (1280x720 RGB565) -- "
                          "sensor format changed? stop displaying",
                     (unsigned)out_size, (unsigned)DEC_OUT_BYTES);
            s_dropped++;
            s_stage_ready = false;
            continue;
        }

        vf_subsample((const uint16_t *)s_dec_out, s_disp[dst_idx]);

        s_last_us = (uint32_t)(esp_timer_get_time() - t0);
        s_stage_ready = false;              /* ★ 解码完才放锁：解码期间 staging 不可被覆写 */

        /* 发布：先换指针、后加序号（读侧先看序号再看指针，顺序不能反） */
        s_latest_idx = dst_idx;
        dst_idx ^= 1u;
        s_seq++;
        s_decoded++;
    }

    ESP_LOGI(TAG, "vf task exit (decoded=%u dropped=%u last=%u us)",
             (unsigned)s_decoded, (unsigned)s_dropped, (unsigned)s_last_us);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* 纯缓冲释放（不含停任务 / 不含状态复位）。幂等。
 * ★ acquire 的失败路径与 viewfinder_release() **共用**它 ——
 *   这是"按需分配"必须补上的一环：acquire 中途失败时，已分配的那几块
 *   必须当场回收，否则下次 acquire（s_inited 仍为 false）会再分配一遍 ⇒ 泄漏。
 * ★ s_dec_out 的释放合法性：本工程未开 CONFIG_SPIRAM_ENC_EXEMPT ⇒
 *   JPEG_SPIRAM_ALLOC_CAPS = MALLOC_CAP_SPIRAM（esp_driver_jpeg/jpeg_private.h:41-45）
 *   ⇒ jpeg_alloc_decoder_mem() 内部就是 heap_caps_aligned_calloc，配对 heap_caps_free。
 *   （IDF 未提供 jpeg_free_decoder_mem，官方就是这么用的。） */
static void free_buffers(void)
{
    if (s_dec_out != NULL) { heap_caps_free(s_dec_out); s_dec_out = NULL; }
    s_dec_out_sz = 0;
    if (s_stage != NULL) { heap_caps_free(s_stage); s_stage = NULL; }
    for (int i = 0; i < 2; i++) {
        if (s_disp[i] != NULL) { heap_caps_free(s_disp[i]); s_disp[i] = NULL; }
    }
}

/* ---------------------------------------------------------------------------
 *  对外 API —— 生命周期（acquire / release，2026-09-28 按需化）
 * ------------------------------------------------------------------------- */
esp_err_t viewfinder_acquire(void)
{
    if (s_inited) return ESP_OK;        /* 幂等：缓冲已在 */

    mem_diag_snap(TAG, "vfmem acquire", "viewfinder buffers");

    /* ---- ① 出图缓冲：必须用 jpeg_alloc_decoder_mem（它做 cache/2D-DMA 对齐，
     *        并保证落在**未加密 PSRAM** —— jpeg_decoder_process 入口会硬校验，
     *        自己 heap_caps_alloc 出来的指针可能过不了那个校验）。 ---- */
    jpeg_decode_memory_alloc_cfg_t mcfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    s_dec_out = jpeg_alloc_decoder_mem(DEC_OUT_BYTES, &mcfg, &s_dec_out_sz);
    if (s_dec_out == NULL) {
        ESP_LOGE(TAG, "alloc %u B decode outbuf FAILED (PSRAM tight?)",
                 (unsigned)DEC_OUT_BYTES);
        free_buffers();                 /* 幂等：此刻只可能没有任何残留，但保持一致 */
        return ESP_ERR_NO_MEM;
    }

    /* ---- ② staging 与显示缓冲：普通 PSRAM 即可（staging 只被 CPU 拷贝；
     *        显示缓冲只被 CPU 读）。64 字节对齐与 cache 行对齐，习惯保持一致。 ---- */
    s_stage = heap_caps_aligned_alloc(64, STAGE_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    for (int i = 0; i < 2; i++) {
        s_disp[i] = heap_caps_aligned_alloc(64, VF_W * VF_H * 2,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    }
    if (s_stage == NULL || s_disp[0] == NULL || s_disp[1] == NULL) {
        ESP_LOGE(TAG, "alloc staging/disp FAILED (staging=%p disp=%p,%p)",
                 s_stage, s_disp[0], s_disp[1]);
        free_buffers();                 /* ★ 必须：回收已分配的那几块，否则下次 acquire 泄漏 */
        return ESP_ERR_NO_MEM;
    }
    memset(s_stage, 0, STAGE_BYTES);
    memset(s_disp[0], 0, VF_W * VF_H * 2);
    memset(s_disp[1], 0, VF_W * VF_H * 2);

    /* ---- ③ 解码引擎：进程内**只建一次**、常驻复用（不要每帧 open/close）----
     *    timeout_ms 必须大于最坏解码耗时；硬件解码 720p 通常 < 30 ms，给 200 稳妥。
     *  ★ 为什么引擎不跟着 release 销毁：它只占内部 RAM 的小结构（rxlink/txlink DMA
     *    描述符、header_info、evt_queue、codec handle、dma2d pool），而"按需"要省的是
     *    PSRAM（2.93 MB）。acquire/release 的频率是"用户进/出页面"级，与其反复
     *    create/delete codec handle，不如让它常驻。
     *    若将来确实要连引擎一起回收：IDF v6.1 有配对 API `jpeg_del_decoder_engine()`
     *    （include/driver/jpeg_decode.h:128；实现 jpeg_decode.c:403 完整 free 上述全部
     *     资源 + jpeg_release_codec_handle），把 s_engine_up 复位即可。
     *  ★ s_engine_up 防的是"release → 再 acquire"循环里重复 new 造成泄漏。 */
    if (!s_engine_up) {
        jpeg_decode_engine_cfg_t ecfg = {
            .intr_priority = 0,
            .timeout_ms    = 200,
            .flags         = { .allow_pd = 0 },
        };
        esp_err_t err = jpeg_new_decoder_engine(&ecfg, &s_dec);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
            free_buffers();
            return err;
        }
        s_engine_up = true;
    }

    s_inited = true;
    ESP_LOGI(TAG, "acquire ok: dec_out=%u B stage=%u B disp=%u B x2 (total ~%u KB PSRAM)",
             (unsigned)s_dec_out_sz, (unsigned)STAGE_BYTES, (unsigned)(VF_W * VF_H * 2),
             (unsigned)((s_dec_out_sz + STAGE_BYTES + 2 * VF_W * VF_H * 2) / 1024u));

    /* ★★ 2026-09-28 按需化：这笔账现在是**可收回**的。
     *   - 旧行为：main.c 开机无条件 init，且全文件无一处 free ⇒ ~2.93 MB 常驻到关机；
     *   - 新行为：本函数只经 viewfinder_acquire() 进入，配对 viewfinder_release()
     *     释放 ⇒ 离开 VIDEO / FILES 页后，这块 PSRAM 回归系统。
     *   ⚠ 调用方义务（违反即 UAF）：release 之前必须让所有 lv_canvas 松开指向
     *     s_disp[] / s_dec_out 的指针 —— lv_canvas_set_buffer() 是零拷贝，只存指针。
     *   ⇒ 上机核对：离开页面后 PSRAM free 应回升 ~2.93 MB
     *     （会看到 `VFD: vfmem acquire` / `VFD: vfmem released` 两条现场快照）。 */
    ESP_LOGW(TAG, "acquired %u KB PSRAM (dec_out %u B contiguous + stage %u B + disp x2 %u B) "
                  "- RELEASABLE: call viewfinder_release() before leaving the page",
             (unsigned)((s_dec_out_sz + STAGE_BYTES + 2 * VF_W * VF_H * 2) / 1024u),
             (unsigned)s_dec_out_sz, (unsigned)STAGE_BYTES,
             (unsigned)(2 * VF_W * VF_H * 2));
    return ESP_OK;
}

/* 兼容别名：历史调用方与"兜底懒初始化"路径（enable / decode_jpeg_mem）都用它。 */
esp_err_t viewfinder_init(void)
{
    return viewfinder_acquire();
}

/* ---------------------------------------------------------------------------
 *  归还：停收帧 + 释放三块 PSRAM 数据缓冲（LVGL 线程专用，幂等）
 * ------------------------------------------------------------------------- */
void viewfinder_release(void)
{
    /* ① 先停收帧（幂等）。必须先停 —— vf_task 正在写 s_disp / 读 s_stage 时释放 = UAF。 */
    viewfinder_disable();

    if (!s_inited) return;              /* 幂等：本来就没占 */

    /* ② 任务必须已退出。disable() 内部最多等 100 ms；超时说明它还在跑 ——
     *    这时**拒绝释放**（宁可留着这点内存，也不能在任务还在写缓冲时 free）。 */
    if (s_task != NULL) {
        ESP_LOGW(TAG, "release SKIPPED: vf task still alive after disable - "
                      "buffers NOT freed (would be use-after-free)");
        return;
    }

    /* ③ 先记账（free 之后 s_dec_out_sz 会被清零） */
    const uint32_t freed_kb =
        (uint32_t)((s_dec_out_sz + STAGE_BYTES + 2u * VF_W * VF_H * 2u) / 1024u);

    free_buffers();

    /* ④ 复位发布状态。s_seq 不复位的话，下次 acquire 后 viewfinder_take() 会把
     *    "上一轮的序号"当成新帧（UI 侧 s_seen_seq 语义错乱，可能一直不刷新）。 */
    s_latest_idx  = 0;
    s_seq         = 0;
    s_seen_seq    = 0;
    s_stage_ready = false;
    s_stage_size  = 0;

    s_inited = false;                   /* ★ 引擎 s_dec / s_engine_up 保持不动（常驻） */

    ESP_LOGI(TAG, "released ~%u KB PSRAM (engine kept; decoded=%u dropped=%u)",
             (unsigned)freed_kb, (unsigned)s_decoded, (unsigned)s_dropped);
    mem_diag_snap(TAG, "vfmem released", "viewfinder buffers freed");
}

esp_err_t viewfinder_enable(void)
{
    if (s_enabled) return ESP_OK;

    if (!s_inited) {
        /* 兜底：调用方忘了先 acquire 时也能用。
         * ★ 别在运行期依赖这条 —— 那等于在碎片最严重的时刻临时要 1.8 MB 连续 PSRAM。
         *   正确做法：LVGL 线程先 viewfinder_acquire() 成功，再 enable。 */
        esp_err_t err = viewfinder_acquire();
        if (err != ESP_OK) return err;
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "previous vf task still alive, refuse enable");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sem == NULL) {
        s_sem = xSemaphoreCreateBinary();
        if (s_sem == NULL) {
            ESP_LOGE(TAG, "create semaphore FAILED (internal RAM tight)");
            return ESP_ERR_NO_MEM;
        }
    }

    s_stage_ready   = false;
    s_last_accept_us = 0;                       /* 立刻收第一帧，别等 150 ms */
    s_oversize_warned = false;                  /* 本轮还没吼过，超限要重新告警 */
    s_enabled = true;

    if (xTaskCreatePinnedToCore(vf_task, "vfind", VF_TASK_STACK, NULL,
                                VF_TASK_PRIO, &s_task, VF_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(vfind) FAILED (internal RAM tight)");
        s_enabled = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    camera_service_set_observer(vf_observe, NULL);
    ESP_LOGI(TAG, "enabled (observer attached, %ux%u @ ~%u ms)",
             (unsigned)VF_W, (unsigned)VF_H, (unsigned)(VF_MIN_INTERVAL_US / 1000u));
    return ESP_OK;
}

void viewfinder_disable(void)
{
    if (!s_enabled) return;

    /* ★ 顺序很重要：先摘观察者（之后不会再有新帧），再停任务。
     *   反过来会有一个窗口：任务已停、观察者还在 give 一个没人接的信号量。 */
    camera_service_set_observer(NULL, NULL);
    s_enabled = false;
    xSemaphoreGive(s_sem);                      /* 唤醒，让它走到 while 判断退出 */

    /* 等 vf 任务退出。它最多再做一次 10 ms 级的解码 + 5 ms 抽点就退出。
     * 由 LVGL 线程（tab 切换）调用 ⇒ 不能等太久。 */
    int waited = 0;
    while (s_task != NULL && waited < 100) {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "vf task still alive after %d ms (will self-exit)", waited);
    }
    ESP_LOGI(TAG, "disabled (decoded=%u dropped=%u; buffers NOT touched here - "
                  "call viewfinder_release() to return the ~2.93 MB)",
             (unsigned)s_decoded, (unsigned)s_dropped);
}

bool viewfinder_is_enabled(void) { return s_enabled; }

bool viewfinder_take(viewfinder_frame_t *out)
{
    uint32_t seq = s_seq;
    if (seq == 0 || seq == s_seen_seq) return false;   /* 没帧 / 已看过 */
    if (out == NULL) { s_seen_seq = seq; return true; }

    out->buf = (const uint16_t *)s_disp[s_latest_idx];
    out->w   = VF_W;
    out->h   = VF_H;
    out->seq = seq;
    s_seen_seq = seq;
    return true;
}

const uint16_t *viewfinder_last_frame(void)
{
    if (!s_inited || s_seq == 0) return NULL;   /* 没 acquire / 还没发布过帧 */
    return (const uint16_t *)s_disp[s_latest_idx];
}

uint32_t viewfinder_decoded_count(void)  { return s_decoded; }
uint32_t viewfinder_dropped_count(void)  { return s_dropped; }
uint32_t viewfinder_last_decode_us(void) { return s_last_us; }

/* ---------------------------------------------------------------------------
 *  文件媒体共享解码入口（S4 文件管理页；守约见 viewfinder.h）
 * ------------------------------------------------------------------------- */
uint8_t *viewfinder_borrow_stage(size_t *cap)
{
    if (cap) *cap = STAGE_BYTES;
    return s_stage;         /* 未 init 时为 NULL，调用方自检 */
}

const uint16_t *viewfinder_decode_jpeg_mem(const uint8_t *jpg, size_t len,
                                           uint16_t *out_w, uint16_t *out_h,
                                           uint16_t *out_stride,
                                           esp_err_t *err)
{
    jpeg_decode_cfg_t cfg = {
        /* ★ 字节序必须 BGR（= 不 scramble），证据见文件头，不要改成 RGB。 */
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t e = ESP_OK;
    jpeg_decode_picture_info_t info;        /* 两个块都要用 ⇒ 函数作用域 */

    if (err) *err = ESP_OK;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_stride) *out_stride = 0;

    if (!s_inited) {
        e = viewfinder_init();              /* 兜底懒初始化（同 enable 路径） */
    } else if (s_enabled) {
        e = ESP_ERR_INVALID_STATE;          /* ★ 共享缓冲正被 vf_task 用，拒绝 */
    }
    if (e == ESP_OK && (jpg == NULL || len == 0)) e = ESP_ERR_INVALID_ARG;

    if (e == ESP_OK) {
        /* ★ 2026-09-28（§73）：先解析头拿真实宽高 —— 打卡照片是 240x240，
         *   旧版"out_size 必须等于 1280*720*2"的硬对账把它们全拒了。 */
        e = jpeg_decoder_get_info(jpg, (uint32_t)len, &info);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "decode: parse info failed: %s", esp_err_to_name(e));
        } else if (info.width == 0 || info.height == 0 ||
                   info.width > 4096 || info.height > 4096) {
            ESP_LOGE(TAG, "decode: bogus dims %ux%u", (unsigned)info.width, (unsigned)info.height);
            e = ESP_ERR_INVALID_SIZE;
        } else if ((uint32_t)info.width * info.height * 2u > s_dec_out_sz) {
            /* 超出共享出图缓冲：如实说，调用方还能拿尺寸给用户看 */
            ESP_LOGE(TAG, "decode: %ux%u needs %u B > out buf %u B",
                     (unsigned)info.width, (unsigned)info.height,
                     (unsigned)(info.width * info.height * 2u), (unsigned)s_dec_out_sz);
            e = ESP_ERR_INVALID_SIZE;
        }
    }

    if (e == ESP_OK) {
        e = jpeg_decoder_process(s_dec, &cfg, jpg, len, s_dec_out, s_dec_out_sz, &out_size);
        if (e == ESP_OK) {
            /* ★ 对账放宽：接受"可见尺寸"或"16 像素补齐尺寸"两种出图大小
             *  （YUV420/422 的 JPEG 引擎会补齐，见 jpeg_decode.h 的 note）。
             *   硬上限仍是 s_dec_out_sz —— 出图比缓冲还大宁可失败也不越界。 */
            uint32_t vis  = (uint32_t)info.width  * info.height  * 2u;
            uint32_t pw   = ((uint32_t)info.width  + 15u) & ~15u;
            uint32_t ph   = ((uint32_t)info.height + 15u) & ~15u;
            uint32_t pad  = pw * ph * 2u;
            if (out_size != vis && out_size != pad) {
                ESP_LOGE(TAG, "decoded %u B != expect vis=%u / pad=%u (dims %ux%u)",
                         (unsigned)out_size, (unsigned)vis, (unsigned)pad,
                         (unsigned)info.width, (unsigned)info.height);
                e = ESP_ERR_INVALID_SIZE;
            } else {
                if (out_w) *out_w = (uint16_t)info.width;
                if (out_h) *out_h = (uint16_t)info.height;
                /* ★ 步长：补齐发生时行里多出 (pw - w) 列，逐行寻址必须用 pw。
                 *   本工程自己的文件（240x240 打卡照 / 720p 录像帧）宽都是 16 的
                 *   倍数，永远走 stride == width 的快路；这一步只为外来 JPEG 兜底。 */
                if (out_stride) *out_stride = (uint16_t)((out_size == pad) ? pw : info.width);
            }
        }
    }

    if (e != ESP_OK) {
        if (err) *err = e;
        return NULL;
    }
    return (const uint16_t *)s_dec_out;     /* 单缓冲：下次调用前有效 */
}
