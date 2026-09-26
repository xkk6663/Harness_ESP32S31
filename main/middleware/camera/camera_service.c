/* camera_service.c - OV3660 DVP preview via V4L2 */
#include "camera_service.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"       /* ★ VIDIOC_S_DQBUF_TIMEOUT（esp_video 私有扩展） */
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "middleware/mem_diag.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include "linux/videodev2.h"
#include "face_service.h"
#include "ov3660_fmt.h"            /* ★ S2-0：运行时格式档位（内含切格式的全部线索） */

#define TAG "CAM"
#define BUF_CNT 2

/* ★★ S2-0：尺寸不再是编译期常量。原来是 `#define CAM_W 240` / `CAM_H 240`，
 *   被 S_FMT 请求值、回调上报值、以及 tab_camera 的 canvas 共用；
 *   JPEG 档是 1280x720 ⇒ 一律改用运行时值 s_cur_w / s_cur_h。
 *   ★ 请勿在本文件里再引入任何"写死 240/720"的字面量（除下面的默认档常量）。 */

/* stop() 等 cam_task 退出的上限。让 cam_task 醒来的不是 STREAMOFF，而是下面这个
 * DQBUF 超时（原因见 cam_task 内注释）⇒ 上限必须 > 超时值。本函数由 LVGL 线程
 * （按钮回调）调用，不能无限等，等太久 UI 会僵。 */
#define CAM_STOP_WAIT_MS      500
#define CAM_DQBUF_TIMEOUT_MS  200   /* DQBUF 单次等待上限；正常帧间隔 60~100ms */
/* DQBUF 连续超时多少次算"相机真的不出帧了"（200ms × 50 = 10s，故意给足余量，
 * 避免低照度/长曝光时被误判为故障而白白把流掐掉） */
#define CAM_DQ_FAIL_MAX        50

/* ★★ P2-5 定稿（2026-09-25，按实测数据 ⇒ 结论是**不扩**）：
 * 方案 §4-P2-5 担心"`do_load()` 在 cam_task 栈上跑完整 esp-dl C++ 模型构建"，建议 ≥8192。
 * 上机日志给了真值：模型加载完成时 hwm=4136 B、任务退出前 hwm=4004 B（hwm 是**历史最小剩余**）
 * ⇒ 峰值只用了 6144-4004 = 2,140 B，余量 4 KB，占整条栈的 65%，根本不是 F37 那种贴零形态。
 * 原因也写清楚了：**深度推理不在这条栈上** —— `detector->run()` 由 tab_camera::refresh()
 * 在 LVGL 线程里调用，那部分栈深算在 taskLVGL 头上（所以 taskLVGL 才是要盯的那个）。
 * ⇒ 6144 不动，省下本来要花的 2 KB 内部 RAM。
 * 判据留好了：main.c 的 `[stack]` 全任务水位表会在 `hwm ≤ size/8` 时打 "!! 偏紧"；
 * 若以后换成更重的模型（YOLO / 更大输入）或把 run() 挪进来，看到那行再扩。 */
/* P2-5 复核（2026-09-26 按两次水位观测收敛）：P2-5 时代实测峰值 2,140 B（含
 * rec_frame_cb 同步写盘路径），今日预览+拍照会话仅 716 B —— 取大者 ×1.5 向上取
 * 512 ⇒ 3584。回退条件：全路径回归后 [stack] 判"偏紧"就回 6144。 */
#define CAM_TASK_STACK         3584

static int  s_fd = -1;
static void *s_buf[BUF_CNT] = {0};
static size_t s_buf_len[BUF_CNT] = {0};   /* ★ mmap 的真实长度，munmap 必须用它 */
/* s_running 被 LVGL 线程写（stop/start）、被 cam_task 读，标 volatile 以免 O2 下被折叠 */
static volatile bool s_running = false;
/* ★ 已知边界（旧分支 BSP 缺口）：bsp_camera.c 只导出 bsp_camera_start()，**没有**
 *   bsp_camera_stop() ⇒ 一旦 s_bsp_started=true 就再也无法把摄像头硬件关掉（sensor 常供电、
 *   XCLK 常开）。所以这里刻意只在首次 START 时调一次，靠 V4L2 的 STREAMOFF/STREAMON
 *   做"逻辑停/启"。若后续要做真正的低功耗，必须先在 BSP 里补 bsp_camera_stop()。 */
static bool s_bsp_started = false;
static TaskHandle_t s_task = NULL;
static camera_frame_cb_t s_cb = NULL;
static void *s_user = NULL;

/* ---- ★ 第二观察者（2026-09-25，见 camera_service.h 的长注释）----
 * LVGL 线程写、cam_task 读 ⇒ volatile。指向的回调自己保证线程安全
 * （viewfinder 的观察者只做 memcpy + give 信号量，不碰任何共享结构）。 */
static volatile camera_frame_cb_t s_observer      = NULL;
static volatile void             *s_observer_user = NULL;

/* ---- ★ S2-0 运行时流状态 ----
 * 回调、get_stream()、以及 owner 查询都读这里。
 * ★ 初值必须有效（默认预览档），否则"还没 start 就被 UI 读"会拿到 0 宽 0 高，
 *   进而让 canvas 布局算出 0 尺寸（F46 那类"看起来被裁掉"的问题）。 */
static uint32_t    s_cur_w  = CAM_PREVIEW_W;
static uint32_t    s_cur_h  = CAM_PREVIEW_H;
static cam_fmt_t   s_cur_fmt = CAM_FMT_RGB565;
static cam_owner_t s_owner   = CAM_OWNER_NONE;   /* 占用者：见 camera_service.h */
static int         s_jpeg_quality = CAM_JPEG_Q_DEFAULT;

/* ---- ★ S2a/S2b "归还"用：上一次**预览**流的描述 ----
 * 拍照/录像事务结束时要恢复 CAM 页的 RGB565 预览（方案 §2.5.4 步骤⑤），
 * 但事务本身不知道 CAM 页用的是哪个回调（那属于 UI 层）⇒ 在这里记住。
 * ★ 只记 owner == CAM_OWNER_PREVIEW 的那一次；媒体流**不**记
 *   （否则"归还"会把上一个 JPEG 流拉回来，方向反了）。
 * ★ 它跨 stop() 存活：这正是要点 —— 事务的第一步就是把预览停掉。 */
static camera_frame_cb_t s_prev_cb    = NULL;
static void             *s_prev_user  = NULL;
static cam_fmt_t         s_prev_fmt   = CAM_FMT_RGB565;
static uint32_t          s_prev_w     = CAM_PREVIEW_W;
static uint32_t          s_prev_h     = CAM_PREVIEW_H;
static bool              s_has_prev   = false;

static void cam_task(void *arg)
{
    struct v4l2_buffer buf;
    int dq_fail = 0;

    while (s_running) {
        face_service_poll();  /* async model load if requested, runs on cam_task stack */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
            /* ★★ 这里是"任务能停下来"的唯一关键，改它前先读这段：
             *   ① esp_video 的 DQBUF 默认等 portMAX_DELAY（src/esp_video.c:560）；
             *   ② esp_video_stop_capture() 只会 xSemaphoreTake(stream->ready_sem, 0) 把
             *      信号量**掏空**、**从不 give**（src/esp_video.c:682-720）；
             *   ③ 而 DQBUF 等的正是那个 ready_sem（esp_video_recv_element, src/esp_video.c:1269）。
             *   ⇒ 用默认无限等待时，STREAMOFF 唤不醒本任务，stop() 必然超时、任务永久滞留
             *     （2026-09-25 实测：连续 5 次 500ms 超时，tasks 一直是 17，且人脸模型
             *      永远加载不了 —— 因为 do_load 就挂在本循环的 face_service_poll() 上）。
             *   ⇒ 所以 start() 里用 VIDIOC_S_DQBUF_TIMEOUT 把等待设成 200ms，
             *     超时只是"这一轮没有帧"，回头看 s_running 即可。 */
            if (!s_running) {
                break;                      /* stop() 要求的正常退出路径 */
            }
            if (++dq_fail >= CAM_DQ_FAIL_MAX) {
                ESP_LOGE(TAG, "no frame in %d x %d ms, giving up", CAM_DQ_FAIL_MAX, CAM_DQBUF_TIMEOUT_MS);
                break;
            }
            continue;
        }
        dq_fail = 0;
        /* ★★ S2-0：回调带上**真实帧长** buf.bytesused。
         *   RGB565 是定长（w*h*2），JPEG 是变长 ⇒ 没有它就没法写盘。
         *   ★ 只有 bytesused != 0 时驱动才会置 V4L2_BUF_FLAG_DONE，所以这个 if
         *     天然把"空帧/坏帧"过滤掉了，不需要额外判断（esp_video_ioctl.c:198 附近）。
         * ★★ 2026-09-25：一帧两个消费者 —— 先主回调、再观察者。
         *   注意观察者**必须在这层 if 里**（坏帧不该喂给任何人），
         *   但要在 `s_cb &&` 之外（取景流的主回调是 NULL，观察者照样要收帧）。 */
        if (buf.flags & V4L2_BUF_FLAG_DONE) {
            if (s_cb) {
                s_cb((const uint8_t *)s_buf[buf.index], (uint32_t)buf.bytesused,
                     s_cur_w, s_cur_h, s_user);
            }
            /* ★ 先读进局部变量再调用：volatile 限定符会跟着实参传进去，
             *   回调形参是 `void *user`，直接传 volatile 指针就是 -Werror。
             *   两个 volatile 分开读不是原子的 —— 最坏情形是回调拿到旧 user，
             *   对现役观察者（viewfinder，user 恒为 NULL）无影响；真要换带状态的
             *   观察者时，必须在停流之后换。 */
            camera_frame_cb_t obs   = s_observer;
            void             *ouser = (void *)s_observer_user;   /* 显式摘掉 volatile */
            if (obs) {
                obs((const uint8_t *)s_buf[buf.index], (uint32_t)buf.bytesused,
                    s_cur_w, s_cur_h, ouser);
            }
        }
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF fail");
            break;
        }
    }

    /* ---- 资源释放由本任务独占负责 ----
     * ★ 绝不能在 stop() 里去 munmap/close：那边与还在跑的本任务抢 fd 和缓冲区 = UAF，
     *   而且下次 START 还会因为旧任务没退而建出第二个 cam 任务。stop() 只做
     *   "s_running=false + STREAMOFF + 等本任务收尾"。 */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (s_fd >= 0) {
        ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    }
    for (int i = 0; i < BUF_CNT; i++) {
        if (s_buf[i]) { munmap(s_buf[i], s_buf_len[i]); s_buf[i] = NULL; s_buf_len[i] = 0; }
    }
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    /* ★ P2-5 数据来源：本任务栈 6144，却要跑 face_service_poll() → do_load()
     * （esp-dl C++ 模型构建）。退出前把水位打出来，作为"要不要扩栈 / 挪走 do_load"的判据。 */
    ESP_LOGI(TAG, "cam_task exiting, stack hwm=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_service_init(void)
{
    /* Lazy: bsp_camera_start called on first camera_service_start() */
    ESP_LOGI(TAG, "lazy init (hardware starts on first START)");
    return ESP_OK;
}

/* ===========================================================================
 * ★★★ S2-0 新增：运行时切格式（拍照/录像那条路的**全部关键线索**都在这里）
 *
 * 为什么不能用 VIDIOC_S_FMT 切：见 ov3660_fmt.h 文件头（那里有源码行号版的长注释）。
 * 一句话：`dvp_video_set_format()` 只校验 w/h 是否等于 sensor **当前**尺寸，不改传感器；
 * 真正改格式的是私有 ioctl **VIDIOC_S_SENSOR_FMT**，且它需要一份**带 regs 的完整描述**
 * （ov3660_set_format() 会把 format->regs 强转成 ov3660_reginfo_t* 去写寄存器）。
 * =========================================================================== */

/**
 * 把 sensor 切到目标档位。**幂等**：已是目标档位时什么都不做。
 *
 * ★ 必须早于 VIDIOC_S_FMT：否则 S_FMT 会因尺寸不符直接 EINVAL。
 * ★ 为什么要判"是否已经是"：每次 S_SENSOR_FMT 都会重写整套寄存器并重置 AE/AWB，
 *   画面会黑一下再慢慢亮回来。连拍/连续录像时反复切是**可见的画质抖动**。
 * ★ 判据取 VIDIOC_G_SENSOR_FMT 的**真值**，不依赖我们自己的记忆
 *   （本工程的老教训：错的日志比没有日志更坏）。
 */
static esp_err_t apply_sensor_format(int fd, cam_fmt_t fmt, uint32_t w, uint32_t h)
{
    const esp_cam_sensor_format_t *want = ov3660_fmt_lookup(w, h, fmt == CAM_FMT_JPEG);
    if (want == NULL) {
        return ESP_ERR_INVALID_ARG;      /* 调用方已提前查过，这里只是兜底 */
    }

    esp_cam_sensor_format_t cur;
    memset(&cur, 0, sizeof(cur));
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &cur) == 0) {
        if (cur.width == want->width && cur.height == want->height &&
            cur.format == want->format) {
            return ESP_OK;               /* 已在目标档，不折腾 */
        }
        ESP_LOGI(TAG, "sensor switch: %ux%u(fmt=%d) -> %s (%ux%u)",
                 (unsigned)cur.width, (unsigned)cur.height, (int)cur.format, want->name,
                 (unsigned)want->width, (unsigned)want->height);
    } else {
        ESP_LOGW(TAG, "G_SENSOR_FMT failed, will set sensor format unconditionally");
    }

    if (ioctl(fd, VIDIOC_S_SENSOR_FMT, (void *)want) != 0) {
        ESP_LOGE(TAG, "S_SENSOR_FMT(%s) failed", want->name);
        return ESP_FAIL;
    }
    /* 驱动在 dvp_video_set_sensor_format() 里**紧接着**重跑了 init_config()，
     * 用新尺寸重算并分配了流缓冲（esp_video_config_buffer）⇒
     * ①下一步 S_FMT 的 w/h 必然能匹配；
     * ②方案 §2.5.7 里"JPEG 720p 缓冲够不够大"的担心**由设计化解**。 */
    return ESP_OK;
}

/**
 * 设 JPEG 质量。走 V4L2_CID_JPEG_COMPRESSION_QUALITY
 * （esp_video_cam.c:71 映射到 ESP_CAM_SENSOR_JPEG_QUALITY → ov3660_set_jpeg_quality()）。
 * ⚠ 传感器侧只在 cur_format == JPEG 时才接受（ov3660.c:495-501），否则 INVALID_STATE。
 */
static esp_err_t do_set_jpeg_quality(int fd, int quality)
{
    struct v4l2_ext_control ctrl = {0};
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CID_JPEG_CLASS,
        .count      = 1,
        .controls   = &ctrl,
    };
    ctrl.id    = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = quality;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t camera_service_start_fmt_owner(camera_frame_cb_t cb, void *user,
                                         cam_fmt_t fmt, uint32_t w, uint32_t h,
                                         cam_owner_t owner)
{
    /* ★ P0-3：上一次 stop() 若没能把 cam_task 等回来（s_task 非 NULL），这里必须拒绝 ——
     * 否则会再建一个同名任务，且旧任务退出时会把新任务的 s_running 清掉 ⇒"按 START 没反应"。 */
    if (s_task != NULL) {
        ESP_LOGE(TAG, "previous cam_task still alive, refuse START");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) return ESP_ERR_INVALID_STATE;

    /* ★ 档位合法性**提前**查：在任何硬件动作（含 open/切格式）之前失败，现场保持干净。 */
    if (ov3660_fmt_lookup(w, h, fmt == CAM_FMT_JPEG) == NULL) {
        ESP_LOGE(TAG, "unsupported profile: %s %ux%u (add it in ov3660_fmt.c)",
                 fmt == CAM_FMT_JPEG ? "JPEG" : "RGB565", (unsigned)w, (unsigned)h);
        return ESP_ERR_INVALID_ARG;
    }

    s_cb = cb; s_user = user;

    if (!s_bsp_started) {
        ESP_LOGI(TAG, "bsp_camera_start...");
        bsp_camera_cfg_t cfg = {0};
        esp_err_t err = bsp_camera_start(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bsp_camera_start fail=%d", err);
            return err;
        }
        s_bsp_started = true;
    }

    MEM_STAGE(TAG, "v4l2 streamon (pre)");

    s_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDONLY);
    if (s_fd < 0) { ESP_LOGE(TAG, "open fail errno=%d", errno); return ESP_FAIL; }

    /* ★★ 必须设 DQBUF 超时，否则 cam_task 一旦阻塞就再也停不下来
     *   （esp_video_stop_capture 不会唤醒它，详见 cam_task 内注释）。 */
    struct timeval dq_tv = {.tv_sec = 0, .tv_usec = CAM_DQBUF_TIMEOUT_MS * 1000};
    if (ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &dq_tv) != 0) {
        ESP_LOGE(TAG, "set DQBUF timeout FAILED, cam_task will be un-stoppable");
    }

    /* ★★ ① 先把 sensor 切到目标档（**必须早于 S_FMT**，理由见 apply_sensor_format）。 */
    if (apply_sensor_format(s_fd, fmt, w, h) != ESP_OK) goto err;

    /* ★ ② 再设 V4L2 侧格式。此时 w/h 已与 sensor 一致 ⇒ 必过。
     *    JPEG 用 V4L2_PIX_FMT_JPEG（'JPEG' fourcc）—— 驱动据此把传感器的
     *    JPEG 码流原样透传（映射见 esp_video_dvp_device.c:94）。 */
    struct v4l2_format vfmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width       = w,
        .fmt.pix.height      = h,
        .fmt.pix.pixelformat = (fmt == CAM_FMT_JPEG) ? V4L2_PIX_FMT_JPEG : V4L2_PIX_FMT_RGB565,
    };
    if (ioctl(s_fd, VIDIOC_S_FMT, &vfmt) != 0) { ESP_LOGE(TAG, "S_FMT fail"); goto err; }

    /* ★ 回读**真值**并记下（不用我们请求的值）—— 驱动可能对尺寸做过调整，
     *   而 UI 布局与落盘都必须按真值走。 */
    {
        struct v4l2_format back = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (ioctl(s_fd, VIDIOC_G_FMT, &back) == 0) {
            s_cur_w = back.fmt.pix.width;
            s_cur_h = back.fmt.pix.height;
        } else {
            s_cur_w = w;
            s_cur_h = h;
        }
    }
    s_cur_fmt = fmt;
    s_owner   = owner;
    ESP_LOGI(TAG, "stream fmt: %s %ux%u owner=%d",
             (fmt == CAM_FMT_JPEG) ? "JPEG" : "RGB565",
             (unsigned)s_cur_w, (unsigned)s_cur_h, (int)owner);

    /* ★★ F48 / CPU 争抢：把预览的 180° 旋转（= hflip + vflip）交给 OV3660 自己做，
     *   寄存器 0x3821[2:1] / 0x3820[2:1]，见 ov3660.c 的 ov3660_set_mirror/set_vflip；
     *   v4l2 CID → sensor param 的映射见 esp_video_cam.c:85-92。
     *
     *   为什么必须从 CPU 挪走（原来的 tab_camera::refresh() 做这件事）：
     *   它在 **LVGL 线程** 里对 240x240 逐像素做 `src_row[CAM_W-1-x]` + 整行倒序 ⇒
     *   逆序访问破坏 cache 预取，一个 64 B cache line 只命中 1 个像素，57,600 像素/帧
     *   全是 cache miss；而源（DVP 帧缓冲 115 KB）和目的（canvas buf 115 KB）**都在 PSRAM**。
     *   与 GDMA 从同一片 PSRAM 持续读帧缓冲正面抢带宽（auto refresh 模式下
     *   = 18 MHz x 2 B = 36 MB/s 持续读；改前还要再叠一次 bounce ISR 的
     *   每 444 us 一次 16,000 B 搬运，那个已随切换消失）。
     *
     *   ★ 组合方向与原代码**完全等价**：原代码 = 整行倒序(vflip) + 行内倒序(hflip) = 旋转 180°，
     *     所以这里 HFLIP+VFLIP 同开。翻转后的像素与原来自算的 s_canvas_buf 逐字节一致 ⇒
     *     face_service_detect() / color_hsv_detect_red() 的坐标系不用动。
     *   ★ 时机：必须在 STREAMON 之前设；S_FMT 之后 DVP 尚未出流，此刻改寄存器最安全。
     *   ★ 用法照抄 esp_video 自带示例 examples/capture_stream/main/capture_stream_main.c:259-276。 */
    {
        struct v4l2_ext_control ctrl = {0};
        struct v4l2_ext_controls ctrls = {
            .ctrl_class = V4L2_CTRL_CLASS_USER,
            .count      = 1,
            .controls   = &ctrl,
        };
        ctrl.id    = V4L2_CID_VFLIP;
        ctrl.value = 1;
        if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
            ESP_LOGE(TAG, "set VFLIP failed -- preview will be upside-down "
                          "(CPU-side flip has been removed)");
        }
        ctrl.id    = V4L2_CID_HFLIP;
        ctrl.value = 1;
        if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
            ESP_LOGE(TAG, "set HFLIP failed -- preview will be mirrored "
                          "(CPU-side flip has been removed)");
        }
        ESP_LOGI(TAG, "sensor flip: HFLIP+VFLIP on (CPU-side rotate removed)");
    }

    /* ★★ F49 真值探针：读回"镜像三件套"的寄存器，确认它们**真的**写进去了。
     *
     *   为什么要读回，而不是"我写了就等于对"：`0x4514` 是**读出顺序补偿**，必须与
     *   HFLIP/VFLIP 同拍；一旦它停在"不镜像"的值，JPEG 会因为读出相位错位而**整体发紫**
     *   （完整证据链见 ov3660_fmt.c 的 F49 长注释）。而"发紫"极易被误判成"JPEG 编码器
     *   有问题"，从而去改一堆无关寄存器 —— 有这一行日志就能**一秒判定**。
     *   ★ 上一次真空档（2026-09-25）就是因为缺这一行，把供应商表的 0x88 误当成
     *     "官方表就这样，没问题"，绕了一大圈。**错的日志比没有日志更坏，但没有日志同样坏。**
     *
     *   路径：esp_video 的 `V4L2_CTRL_CLASS_ESP_CAM_IOCTL` 是**通用转发**
     *   （esp_video_cam.c:183-192：直接把 `ctrl->id` 当传感器 ioctl、`p_u8`/`size` 当参数缓冲）
     *   ⇒ 读用 VIDIOC_G_EXT_CTRLS + ESP_CAM_SENSOR_IOC_G_REG + esp_cam_sensor_reg_val_t。
     *   ★ 读失败**只告警不中断**：这条路坏掉不影响出图，只是失去了"真值"。
     *
     *   ★ 期望值是**推导**来的，不是抄的：
     *       720p JPEG  = 非 binning + V翻转 + H镜像 → 0x4514 应为 0xbb
     *       240x240    =       binning + V翻转 + H镜像 → 0x4514 应为 0xaa
     *     依据同样是 esp32-camera `ov3660.c::set_image_options()` 的查表。
     *   ★ 副产品：本工程**一张日志同时给出两档的真值**（切一次 JPEG 再切回来即可对照），
     *     正好是"预览正常 / 拍照发紫"这个症状的判别实验。 */
    {
        static const uint32_t probe_regs[] = { 0x3820, 0x3821, 0x4514 };
        const uint32_t want_4514 = (fmt == CAM_FMT_JPEG) ? 0xbb : 0xaa;

        for (size_t i = 0; i < sizeof(probe_regs) / sizeof(probe_regs[0]); i++) {
            esp_cam_sensor_reg_val_t rv = {0};
            struct v4l2_ext_control  c  = {0};
            struct v4l2_ext_controls cs = {
                .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
                .count      = 1,
                .controls   = &c,
            };
            rv.regaddr = probe_regs[i];
            c.id   = ESP_CAM_SENSOR_IOC_G_REG;
            c.p_u8 = (uint8_t *)&rv;
            c.size = sizeof(rv);

            if (ioctl(s_fd, VIDIOC_G_EXT_CTRLS, &cs) != 0) {
                ESP_LOGW(TAG, "sensor reg 0x%04x readback failed (errno=%d) -- 无法自证镜像/读出顺序",
                         (unsigned)probe_regs[i], errno);
                continue;
            }
            if (probe_regs[i] == 0x4514) {
                if (rv.value == want_4514) {
                    ESP_LOGI(TAG, "sensor reg 0x4514 = 0x%02x (期望 0x%02x，读出顺序 OK)",
                             (unsigned)rv.value, (unsigned)want_4514);
                } else {
                    ESP_LOGE(TAG, "sensor reg 0x4514 = 0x%02x，期望 0x%02x !!! "
                                  "→ 照片/视频会颜色错位(发紫)，查 ov3660_fmt.c 的 F49 覆盖项",
                             (unsigned)rv.value, (unsigned)want_4514);
                }
            } else {
                ESP_LOGI(TAG, "sensor reg 0x%04x = 0x%02x",
                         (unsigned)rv.regaddr, (unsigned)rv.value);
            }
        }
    }

    /* ★★ S2-0 重要：翻转**必须每次 start 都重设**，不能只做一次。
     *   理由：`ov3660_set_format()` 会先写一整组 ov3660_sensor_default_regs、再写格式组寄存器
     *   （ov3660.c:558-577），而格式组里**不含** 0x3820/0x3821 的翻转位
     *   ⇒ 只要发生过一次 S_SENSOR_FMT，翻转就被"洗掉"了。
     *   ⇒ 顺序固定为：S_SENSOR_FMT → S_FMT → flip → (JPEG quality) → REQBUFS → STREAMON。
     *   ★ 上机判据：切到 JPEG 再切回 RGB565，CAM 页画面方向必须与切之前一致。 */

    /* ★ JPEG 质量：必须在传感器已处于 JPEG 档之后设（ov3660.c:495-501 会检查 cur_format）。 */
    if (fmt == CAM_FMT_JPEG) {
        esp_err_t qe = do_set_jpeg_quality(s_fd, s_jpeg_quality);
        if (qe == ESP_OK) {
            ESP_LOGI(TAG, "jpeg quality = %d (1=best/largest .. 63=worst/smallest)", s_jpeg_quality);
        } else {
            ESP_LOGW(TAG, "set jpeg quality %d failed: %s (用传感器默认质量继续)",
                     s_jpeg_quality, esp_err_to_name(qe));
        }
    }

    struct v4l2_requestbuffers req = {
        .count = BUF_CNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) { ESP_LOGE(TAG, "REQBUFS fail"); goto err; }

    for (int i = 0; i < BUF_CNT; i++) {
        struct v4l2_buffer b = {.type=V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory=V4L2_MEMORY_MMAP, .index=i};
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) goto err;
        s_buf[i] = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        if (s_buf[i] == MAP_FAILED) { s_buf[i] = NULL; goto err; }
        s_buf_len[i] = b.length;          /* ★ 记下真长，munmap 用它 */
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) goto err;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) { ESP_LOGE(TAG, "STREAMON fail"); goto err; }

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(cam_task, "cam", CAM_TASK_STACK, NULL, 5, &s_task, 1);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        MEM_ERR_DIAG(TAG, "xTaskCreatePinnedToCore(cam) FAILED", CAM_TASK_STACK);
        goto err;
    }
    MEM_STAGE(TAG, "v4l2 streamon (post)");
    /* ★ 起流**完全成功**之后**才**记"归还"用的描述（放在所有可能失败的步骤之后：
     *   记了一次没真正起来的流，归还时就会重放一份错的参数）。 */
    if (owner == CAM_OWNER_PREVIEW) {
        s_prev_cb   = cb;
        s_prev_user = user;
        s_prev_fmt  = fmt;
        s_prev_w    = w;
        s_prev_h    = h;
        s_has_prev  = true;
    }
    ESP_LOGI(TAG, "stream on");
    return ESP_OK;
err:
    /* 失败回滚也要把流停掉再拆缓冲：STREAMON 已经发出去了，直接 munmap/close
     * 会让 DVP 继续往已解除映射的地址写。 */
    if (s_fd >= 0) {
        int t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_fd, VIDIOC_STREAMOFF, &t);
    }
    for (int i = 0; i < BUF_CNT; i++) {
        if (s_buf[i]) { munmap(s_buf[i], s_buf_len[i]); s_buf[i] = NULL; s_buf_len[i] = 0; }
    }
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    /* ★ 启动失败 ⇒ 相机没被任何人占用。不置 NONE 的话 CAM 页会一直显示"被录像占用"
     *   并把按钮置灰，而实际上根本没人用 —— 典型的"错的状态比没有状态更坏"。 */
    s_owner = CAM_OWNER_NONE;
    return ESP_FAIL;
}

esp_err_t camera_service_stop(void)
{
    if (!s_running && s_task == NULL) return ESP_OK;
    s_running = false;

    /* ★ 只有两件事：停 DMA + 等任务自己收尾。fd / 映射缓冲一律由 cam_task 释放，
     *   本函数绝不碰 —— 否则就是"一边抽走 fd 一边让任务继续用"的 UAF。
     * ★ STREAMOFF 的真实作用只是**立刻停掉 DVP 写入**；叫醒阻塞在 DQBUF 的任务靠的是
     *   start() 里设的 200ms 超时（见 cam_task 内注释）。 */
    if (s_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    }

    /* ★ P0-3：等 cam_task **真的**退出（它退出前会自己把 s_task 清 NULL）。
     * 不能只 delay 100ms 就假定它退了 —— 那是"猜"。 */
    int waited = 0;
    while (s_task != NULL && waited < CAM_STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (s_task != NULL) {
        ESP_LOGE(TAG, "cam_task not exited after %d ms (stack hwm=%u B); "
                      "fd/buffers stay with the task, refuse further START",
                 waited, (unsigned)uxTaskGetStackHighWaterMark(s_task));
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "stream off (waited %d ms)", waited);
    /* ★ 任务确实退了 ⇒ 相机不再被占用。**故意放在确认退出之后**：
     *   超时那支（上面 return）必须保留 s_owner，因为相机实际上还被旧任务占着。 */
    s_owner = CAM_OWNER_NONE;
    return ESP_OK;
}

/* ★ 把"任务还没退"也算作 running：UI 的 START/STOP 文案才会与真实状态一致
 * （stop() 超时返回时不能显示成"已停"）。 */
bool camera_service_is_running(void) { return s_running || s_task != NULL; }

/* ---- ★ S2-0 新增的薄封装 ---- */

/* 兼容旧调用（CAM tab 原来就是这个语义）。owner 按格式推断成 PREVIEW。 */
esp_err_t camera_service_start(camera_frame_cb_t cb, void *user)
{
    return camera_service_start_fmt_owner(cb, user, CAM_FMT_RGB565,
                                          CAM_PREVIEW_W, CAM_PREVIEW_H, CAM_OWNER_PREVIEW);
}

/* 对外主入口：**归属按格式推断** —— RGB565 ⇒ PREVIEW，JPEG ⇒ MEDIA。
 * 这与方案 §2.5.1 的职责划分一致（CAM tab = RGB565 预览/模型；VIDEO tab = JPEG 媒体）。
 * 需要推断之外的语义时用 camera_service_start_fmt_owner()。 */
esp_err_t camera_service_start_fmt(camera_frame_cb_t cb, void *user,
                                   cam_fmt_t fmt, uint32_t w, uint32_t h)
{
    cam_owner_t owner = (fmt == CAM_FMT_JPEG) ? CAM_OWNER_MEDIA : CAM_OWNER_PREVIEW;
    return camera_service_start_fmt_owner(cb, user, fmt, w, h, owner);
}

void camera_service_get_stream(uint32_t *w, uint32_t *h, cam_fmt_t *fmt)
{
    if (w)   *w = s_cur_w;
    if (h)   *h = s_cur_h;
    if (fmt) *fmt = s_cur_fmt;
}

cam_owner_t camera_service_owner(void) { return s_owner; }

void camera_service_set_observer(camera_frame_cb_t cb, void *user)
{
    s_observer      = cb;
    s_observer_user = user;
}

esp_err_t camera_service_restore_preview(void)
{
    if (!s_has_prev) {
        return ESP_ERR_NOT_FOUND;      /* 从没有过预览：没什么可归还，调用方静默即可 */
    }
    /* ★ 还占着就别抢：调用方必须先 stop()。这里拒绝而不是内部帮它 stop ——
     *   否则"归还"会隐式掐掉一个正在跑的媒体流（方向又反了）。 */
    if (s_owner != CAM_OWNER_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    return camera_service_start_fmt_owner(s_prev_cb, s_prev_user,
                                          s_prev_fmt, s_prev_w, s_prev_h,
                                          CAM_OWNER_PREVIEW);
}

esp_err_t camera_service_set_jpeg_quality(int quality)
{
    if (quality < CAM_JPEG_Q_BEST || quality > CAM_JPEG_Q_WORST) {
        ESP_LOGE(TAG, "jpeg quality %d out of range [%d..%d]",
                 quality, CAM_JPEG_Q_BEST, CAM_JPEG_Q_WORST);
        return ESP_ERR_INVALID_ARG;
    }
    s_jpeg_quality = quality;          /* 记住：下次 start(JPEG) 时自动生效 */

    if (s_fd < 0) {
        return ESP_OK;                 /* 未启动 —— 已记下，届时应用 */
    }
    if (s_cur_fmt != CAM_FMT_JPEG) {
        ESP_LOGW(TAG, "set jpeg quality ignored: current stream is RGB565");
        return ESP_ERR_INVALID_STATE;
    }
    return do_set_jpeg_quality(s_fd, quality);
}
