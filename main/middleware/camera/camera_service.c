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
#include "face_recog_service.h"    /* ★ §73：住户报告要报识别引擎是否还占着模型 */
#include "viewfinder.h"            /* ★ §73：住户报告要报共享解码缓冲 */
#include "ov3660_fmt.h"            /* ★ S2-0：运行时格式档位（内含切格式的全部线索） */

#define TAG "CAM"
#define BUF_CNT 2

/* ★★ 2026-09-28 新增：**媒体档（JPEG）单独一份缓冲数**。
 *
 * 为什么要能单独调：媒体档是 1280x720 JPEG，每个 V4L2 缓冲 = w*h*bpp/8 = 921,600 B
 * （JPEG 的 bpp 是 8，见 esp_video.c:84 与 esp_video_config_buffer()）。
 * ★★ §73.15（2026-09-29 晚，本注释第三次定稿）：总需求 = **(MEDIA_BUF_CNT + 1) 块**：
 *      =2 → 2 块流缓冲（REQBUFS 阶段）+ 1 块 **DVP backup buffer**（STREAMON 阶段，
 *           esp_cam_new_dvp_ctlr() 分配一块帧同尺寸的整帧备份，
 *           IDF esp_driver_cam esp_cam_ctlr_dvp_cam.c:866-867）
 *      =1 → 流 1 块 + backup 1 块 = 1,843,200 B
 *   ★ 勘误史：§73.13 曾判 "+1 backup 是旧 BSP 概念、已作废"——**判错了**：
 *     当时只 grep 了 managed_components 的两个组件，backup 分配在 IDF 本体的
 *     esp_driver_cam 组件里，漏搜；当晚现场日志 `no mem for DVP backup buffer`
 *     复现后才定案回滚（判据/重试/文案全部恢复 n+1）。教训：**驱动结论必须
 *     搜遍 IDF 本体 + managed_components 两处**。
 *   ★ 逐块分配没错（esp_video_buffer.c 逐块 aligned_alloc），所以各块**不需要
 *     相邻**；判据按"能切出 n+1 块"（mem_diag_free_capacity）算。
 *   ⚠ backup 分配晚于 REQBUFS ⇒ REQBUFS 成功 ≠ 起流成功 —— §73.15 已把
 *     STREAMON ENOMEM 也纳入腾地重试（goto streambuf 重走一遍）。
 * ★ **=1（2026-09-29 晚 用户拍板，第二次）**：实测 =2 的三块需求（2 流 + 1 backup）
 *   在"三件套在场 / 洞碎"场景反复把起流逼炸（判据警告 → 腾地重试后画面仍卡住无流，
 *   §73.16 现场日志），黑屏的代价远大于丢帧 ⇒ 回到 =1：
 *      总需求 = 1 流 + 1 backup = **2 块 × 921,600 = 1,843,200 B**，free 4.25M 时随便切。
 *   代价（用户已知并接受）：SD 卡写盘瞬间来的帧落到 backup 被丢弃 ⇒ 录像 fps 低于
 *   标称 12、动作略发涩；取景预览不受影响（本来 1 帧就够）。
 *   ⚠ 若录像流畅性将来要求回 =2：必须同时保证"起流时三件套不占洞"
 *     （现 §73.16 时序已满足）+ 判据/腾地兜底在位，且接受贴边运行的偶发失败。
 *
 * ★ 用 `#ifndef` 包住（本工程惯例）：这样 `-DMEDIA_BUF_CNT=2` 也能编译，
 *   离线探针才校验得到"调大"那一侧；否则 `-D` 会撞 'MEDIA_BUF_CNT' redefined [-Werror]。
 */
#ifndef MEDIA_BUF_CNT
#define MEDIA_BUF_CNT 1
#endif

/* ★ §73.13：aligned_alloc(64, per) 实际要从洞里切走的字节数 > per（对齐浪费 +
 *   heap 头），且测量与分配之间堆会波动。判据统一按 per + 本余量算洞容量，
 *   宁可早一步腾地，不给"判据说 fit、驱动说 ENOMEM"留缝。 */
#define CAM_ALLOC_SLACK 128u

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
/* ★★ 缓冲槽数组按 BUF_CNT 开（两个档位共用这块数组）。
 *   MEDIA_BUF_CNT 只能**调小不能调大** —— 调大就会写越界且编译器不一定报错，
 *   所以在这里用 _Static_assert 钉死。 */
_Static_assert(MEDIA_BUF_CNT <= BUF_CNT, "MEDIA_BUF_CNT must not exceed BUF_CNT (s_buf[] size)");
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

/* ---------------------------------------------------------------------------
 *  起流前的 PSRAM **连续块**预检（★★ 2026-09-28，为"拍照偶尔存 SD 失败"而加）
 * ---------------------------------------------------------------------------
 * 【失败现场】全都是驱动内部打的，用户侧只有一句"拍照失败"：
 *     E (108275) dvp_cam: esp_cam_new_dvp_ctlr(867): no mem for DVP backup buffer
 *     E (108281) dvp_video: failed to create DVP
 *     E (108289) CAM: STREAMON fail
 *     I (108296) CAPTURE: capture done: err=ESP_FAIL, 502 ms
 *
 * 【"no mem" 把两种病混成了一句】本工程 §61 的老教训（POSD 加载失败）在这里复发：
 *     (a) PSRAM 总量不够          —— 唯一解是省内存；
 *     (b) 有量但**没有连续块**（碎片）—— 解是让出大块 / 减块数 / 改分配时机。
 *   而 `MEM_DIAG_FMT` 这一轮之前**只打 PSRAM free、不打 largest** ⇒
 *   "free 还有 4.2 MB 却失败"这句话无法当场判定是 (a) 还是 (b)。本轮已补 largest。
 *
 * 【媒体档到底要多少连续 PSRAM】同一个 w*h 要三份，全部从 `MALLOC_CAP_SPIRAM` 拿：
 *   ① REQBUFS：BUF_CNT 个 mmap 缓冲，每块 = w*h*bpp/8
 *      （JPEG bpp=8 ⇒ 1280x720 = 921,600 B；真源 esp_video.c:84 +
 *        esp_video_config_buffer()：JPEG 且 sizeimage==0 时用 w*h*bpp/8）
 *      —— 分配点 esp_video_buffer.c:47 `heap_caps_aligned_alloc(..., info->caps)`
 *   ② STREAMON：IDF DVP 驱动的 backup buffer，尺寸同样 = w*h = 921,600 B
 *      —— esp_cam_ctlr_dvp_cam.c:262-278 算尺寸、:866 分配
 *   ⇒ need = (n + 1) x w x h（JPEG）。RGB565 则每块 x2（见 color depth 分支）。
 *
 * 【★★ 排除 caps 因素（重要，省掉一整类猜测）】
 *   backup buffer 请求的是 `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`，看着像"要一块
 *   能 DMA 的 PSRAM"，容易让人怀疑 S31 上这种内存不存在。但 IDF 在查 heap **之前**
 *   就把 DMA 位摘掉了：
 *     esp_hw_support/heap_align_hw.c:101-108
 *       // Workaround: the heap allocator doesn't have regions marked
 *       // `MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM` so we need to request those
 *       // without the DMA flag.
 *       if (caps & MALLOC_CAP_SPIRAM) { caps &= ~MALLOC_CAP_DMA; }
 *   （S31 的 SPIRAM 内存类型表里确实没有 DMA：heap/port/esp32s31/memory_layout.c:52）
 *   ⇒ 这个失败**只可能是"PSRAM 里找不出这么长的一段空地"**，与 caps / DMA 无关。
 *   ⇒ 也解释了为什么"PSRAM free 有几 MB"和"分配失败"能同时成立。
 * ------------------------------------------------------------------------- */

/** 本次起流用的 V4L2 缓冲数（媒体档可单独调，见 MEDIA_BUF_CNT 注释）。 */
static uint32_t cam_stream_buf_cnt(cam_fmt_t fmt)
{
    return (fmt == CAM_FMT_JPEG) ? (uint32_t)MEDIA_BUF_CNT : (uint32_t)BUF_CNT;
}

/** 单块缓冲字节数（== 驱动眼里的"最坏一帧"）。 */
static size_t cam_stream_frame_bytes(cam_fmt_t fmt, uint32_t w, uint32_t h)
{
    size_t px = (size_t)w * (size_t)h;
    return (fmt == CAM_FMT_JPEG) ? px        /* JPEG：bpp=8 ⇒ 1 B/px */
                                 : px * 2u;  /* RGB565：16 bpp */
}

/** 本次起流要从 PSRAM 拿下的**连续**字节数：n 个 mmap 缓冲 + 1 个驱动 backup。 */
static size_t cam_stream_psram_need(cam_fmt_t fmt, uint32_t w, uint32_t h)
{
    /* ★ §73.15：+1 backup **恢复** —— 它是真的！
     *   实证：IDF esp_driver_cam `esp_cam_ctlr_dvp_cam.c:866-867`
     *   （C:/esp/v6.1/esp-idf/components/esp_driver_cam/dvp/src/）：
     *   `esp_cam_new_dvp_ctlr()` 在 STREAMON 路径创建 DVP 控制器时分配一块
     *   **与帧同尺寸**（JPEG = h_res×v_res = 921,600 B）的 backup buffer，
     *   失败即 `no mem for DVP backup buffer`（§73.15 现场日志复现）。
     *   §73.13/§73.14 把它判为"旧 BSP 概念"是**错的** —— 错因：当时只 grep 了
     *   managed_components 的两个组件，backup 分配在 IDF 本体组件里，漏搜。 */
    return cam_stream_frame_bytes(fmt, w, h) * (size_t)(cam_stream_buf_cnt(fmt) + 1u);
}

/** 起流前把"要多少 / 有多少（含 largest）"打成一行。
 *  ★ 每次起流都打：连续块是**当次现场**的属性，只打一次等于没有。
 *
 *  ★★★ 2026-09-28 修正判据 —— **原版的比较是错的**：
 *     驱动的 n+1 次分配**彼此独立、不要求相邻**（n 个 V4L2 mmap 缓冲 + 1 个 DVP
 *     backup buffer，各自做一次 `heap_caps_aligned_alloc`）⇒ 真正的硬约束是
 *     "**每块** per 字节连续"，**不是**"total = per*(n+1) 字节连续"。
 *     原版拿 `total` 去比 `largest` ⇒ 只要 largest 落在 (per, total) 区间就**误报**。
 *     实测（2026-09-28 上机）：largest=1,835,008 vs total=1,843,200 —— 差 8,192 B
 *     就喊 "NOT ENOUGH CONTIGUOUS PSRAM"，而**同一轮拍照其实成功了**（photo OK）。
 *     ⚠ 误报的代价不只是噪音：它连带触发 heap dump，而那个 dump 持堆锁打上千行
 *     ⇒ 直接 `Interrupt wdt timeout on CPU0` 崩机。所以判据必须按真实约束写。
 *
 *  ★ 不够时**只告警不拒绝**：本估算按驱动最坏尺寸算（JPEG 缓冲按 w*h 而非真实码流长），
 *    保守值会偶发误报；真拒绝会把本来拍得成的场景挡掉。
 *    让它继续试，失败由 STREAMON 分支给出确定的根因说明。 */
static void cam_stream_mem_check(cam_fmt_t fmt, uint32_t w, uint32_t h)
{
    const size_t   per     = cam_stream_frame_bytes(fmt, w, h) + CAM_ALLOC_SLACK;
                                                   /* 单块 = 一帧最坏尺寸 + aligned_alloc 开销余量（§73.13） */
    const uint32_t n       = cam_stream_buf_cnt(fmt);  /* mmap 缓冲数 */
    /* ★ §73.15：+1 = DVP backup buffer（esp_cam_ctlr_dvp_cam.c:866，STREAMON 时
     *   esp_cam_new_dvp_ctlr() 分配一块帧同尺寸的整帧备份）—— 恢复，§73.13 判错。 */
    const size_t   blocks  = (size_t)n + 1u;
    const size_t   total   = cam_stream_frame_bytes(fmt, w, h) * blocks;
    const size_t   free_sz = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* ★★ 判据的核心：**"能不能切出 blocks 个 per"**，而不是"largest ≥ total"。
     *   `cap` = sum( floor(空闲块 / per) )，即乐观口径下能切出几块（见 mem_diag.h）。 */
    size_t   largest = 0;
    uint32_t n_ge    = 0;
    const uint32_t cap = mem_diag_free_capacity(per, &largest, &n_ge);

    ESP_LOGI(TAG, "[streammem] %s %ux%u need per-block=%u B x %u block(s) = %u B total"
                  " | PSRAM free=%u largest=%u | fit: capacity=%u (n_ge=%u)",
             (fmt == CAM_FMT_JPEG) ? "JPEG" : "RGB565", (unsigned)w, (unsigned)h,
             (unsigned)per, (unsigned)blocks, (unsigned)total,
             (unsigned)free_sz, (unsigned)largest, (unsigned)cap, (unsigned)n_ge);

    /* 判据 A（硬约束）：连一块都放不下 ⇒ 本次一定失败。 */
    if (largest < per) {
        ESP_LOGW(TAG, "[streammem] largest=%u < per-block %u ⇒ 第一块缓冲就放不下，"
                      "本次起流**会失败**（free=%u 够但 largest 不够 = 碎片）",
                 (unsigned)largest, (unsigned)per, (unsigned)free_sz);
        mem_diag_dump_top(TAG, "streammem");
        return;
    }
    /* 判据 B（能否凑齐 blocks 块）：每块都放得下，但空闲区切不出这么多块 ⇒ 可能中途失败。 */
    if (cap < blocks || free_sz < total) {
        ESP_LOGW(TAG, "[streammem] 需要 %u 块 x %u B，但空闲区最多只能切出 %u 块"
                      "（free=%u total_need=%u）⇒ 可能中途失败",
                 (unsigned)blocks, (unsigned)per, (unsigned)cap,
                 (unsigned)free_sz, (unsigned)total);
        mem_diag_dump_top(TAG, "streammem");
        return;
    }
    /* ★ 够 —— 安静走开。
     *   注意 "largest < total" 是**正常且常见**的（碎片形态），只要 cap ≥ blocks 就没事：
     *   各块彼此**不需要相邻**（esp_video_buffer.c 逐块 aligned_alloc）。
     *   ⚠ §73.13 现场教训：判据 fit ≠ 驱动一定成功（测量与分配之间堆会波动、
     *   aligned_alloc 的洞开销难精确建模）—— 所以失败兜底在 REQBUFS ENOMEM
     *   的自动腾地重试，判据只做"提前腾地"的预告。 */
}

/**
 * ★★ §73 新增：起流失败时把"**谁还占着大块 PSRAM**"报出来（住户报告）。
 *
 * 背景（用户报"CAM 页停了之后 VIDEO 起流 REQBUFS fail，重新上电就正常"）：
 *   驱动侧证据链（managed_components/espressif__esp_video/src/esp_video.c）：
 *     `REQBUFS → esp_video_setup_buffer() → 先 esp_video_release_stream_buffer()
 *      （**旧缓冲一定被释放了**，不存在"CAM 没还"）→ esp_video_buffer_create()
 *      失败 ⇒ "Failed to create buffer" ⇒ ESP_ERR_NO_MEM`。
 *   所以 REQBUFS fail 的**唯一**含义是：此刻 PSRAM 里切不出一块
 *   `单帧字节`（720p RGB565 = 1,843,200 B；JPEG 720p = 921,600 B）的连续区。
 *   "重新上电正常"就是因为上电时下面这些住户都还没住进来。
 *
 * 报告只陈述事实不给结论 —— 谁该被请走由用户决定（det 模型可以按 FACE 按钮卸载；
 * 识别引擎按 §72 会话结束自动卸载；取景器缓冲归持有它的页面）。
 */
static void cam_mem_residents_report(void)
{
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t free_sz = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGE(TAG, "[residents] PSRAM free=%u largest=%u -- who is holding big blocks:",
             (unsigned)free_sz, (unsigned)largest);

    /* det 模型（espdet face / hand + scratch）：§72 起只在 FACE(det) 按钮按下时
     * 加载、再按一次经 unload 归还 —— 它们是 CAM 会话之后最大的一笔常驻。
     * ★ §73.9：起流预判放不下时 start() 会**自动**卸载它们（见 [reclaim] 日志），
     *   这条报告保留是为了让"为什么刚才 FACE 按钮变 OFF 了"有据可查。
     * ★ §73.14：改用 face_service_loaded_models()（真实已加载数）——
     *   旧代码误用 model_count()（档位总数常量），导致这条**恒真误报**，
     *   §73.13 现场日志里"det 模型 ×2 在场"就是它。 */
    int det_models = face_service_loaded_models();
    if (det_models > 0) {
        ESP_LOGE(TAG, "[residents]  - face DET models loaded (%d model(s)): "
                      "auto-unloaded on stream start if tight (FACE btn -> OFF)", det_models);
    }
    /* 识别引擎（MSR+MNP+MFN ~2.3 MB）：§72 语义下会话结束就该为 0；
     * 若这里 >0 说明有一个打卡/录入会话没收干净 —— 那是 BUG，不是常态。 */
    if (face_recog_ready()) {
        ESP_LOGE(TAG, "[residents]  - face RECOG engine still loaded (~2.3 MB): "
                      "a punch/enroll session did NOT release it (BUG?)");
    }
    /* 取景器共享缓冲（§73.13 实测口径：released ≈ 2,925 KB）：
     * acquire 后只要不 release 就一直占着 —— 持有者只有 files/face/video 页。 */
    if (viewfinder_is_enabled()) {
        ESP_LOGE(TAG, "[residents]  - viewfinder buffers held (~2.9 MB): "
                      "leave the page that acquired them");
    }
    if (det_models == 0 && !face_recog_ready() && !viewfinder_is_enabled()) {
        ESP_LOGE(TAG, "[residents]  - none of the known big holders are resident "
                      "-> pure fragmentation (reboot, or reduce MEDIA_BUF_CNT)");
    }
}

/**
 * ★★ §73.15：起流失败的**统一腾地**（幂等，可安全重复调用）。
 *
 * 前提：只在"相机没跑起来"（s_running == false）的起流失败路径里调用 ——
 * 此刻帧处理块进不来，unload_sync / viewfinder_release 都可就地执行。
 *
 * 腾地对象与量级（§73.15 现场日志定案）：
 *   - det 模型（FACE/HAND espdet + scratch，~1.25 MB）；
 *   - 识别引擎（MSR+MNP+MFN ~2.3 MB，正常不该在场——会话泄漏才算 BUG）；
 *   - viewfinder 三件套（~2.93 MB）—— §73.14 起纳入：恢复预览场景它就是最大
 *     住户，先放掉换"起流成功"是稳赚的（起流成功后 VFD 重新渐进 acquire）。
 *
 * 返回是否真的释放了东西 —— 调用方据此决定要不要重试（什么都没腾出来，
 * 重试注定还是 ENOMEM，省一次无用功）。
 */
static bool cam_reclaim_residents(void)
{
    bool did = false;
    if (face_service_loaded_models() > 0) {
        (void)face_service_unload_sync();
        did = true;
    }
    if (face_recog_ready()) {
        (void)face_recog_stop();
        did = true;
    }
    if (viewfinder_is_enabled()) {
        viewfinder_release();
        did = true;
    }
    if (did) {
        ESP_LOGW(TAG, "[reclaim] CAM residents unloaded (det/recog/viewfinder), "
                      "PSRAM largest=%u now",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    return did;
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

    /* ★★ 同样在任何硬件动作之前，把"这次要多少连续 PSRAM / 现在有多少"记下来。
     *   ⚠ 位置关键：此刻上一次的流已经拆干净（上面 s_running == false），而本次的
     *     缓冲还没分配 ⇒ 这一行就是驱动将要面对的真实 heap，测量值可直接当预判。 */
    cam_stream_mem_check(fmt, w, h);

    /* ★★ §73.9（2026-09-28 深夜日志定案）：起流前的**礼貌腾地**。
     *
     *   现场证据：det 开着进 VIDEO，REQBUFS 吃掉 2 块后 backup 那块放不下
     *  （`no mem for DVP backup buffer` → STREAMON fail）。住户报告点名
     *   det 模型 —— 只要它常驻，720p 的 3 块需求**数学上**就装不下，
     *   用户必须手动回 CAM 页关 FACE 才能录，这是不可接受的体验。
     *
     *   策略：**预判放不下**（largest < 单块，或切不出 blocks 块）且有大块
     *   住户时，先卸载再继续 —— det 模型（~1.25 MB）与识别引擎（~2.3 MB）
     *   都是 CAM 页的功能，起流这一刻相机必然是停的 ⇒ unload_sync 可以
     *   **就地**执行（异步版在相机停着时没人消费，见 face_service.h）。
     *
     *   ★ 副作用必须说清楚：det 开关被关 —— 用户回 CAM 页会看到 FACE 按钮
     *     是 OFF（宁可按钮变灰，也不让 VIDEO 莫名起不来；这与"打卡/录入
     *     结束即彻底释放"是同一条内存纪律）。
     *   ★ 回退开关：不接受这个语义就删掉本段，行为回到"失败 + 住户报告"。 */
    {
        const size_t per    = cam_stream_frame_bytes(fmt, w, h) + CAM_ALLOC_SLACK;
        /* §73.15：+1 backup 恢复（esp_cam_ctlr_dvp_cam.c:866，见 psram_need 注释） */
        const size_t blocks = (size_t)cam_stream_buf_cnt(fmt) + 1u;
        size_t   largest = 0;
        uint32_t n_ge    = 0;
        const uint32_t cap = mem_diag_free_capacity(per, &largest, &n_ge);
        const bool   tight = (largest < per) || (cap < blocks);

        if (tight && (face_service_loaded_models() > 0 || face_recog_ready() ||
                      viewfinder_is_enabled())) {   /* §73.14/§73.15：腾地列表含 viewfinder */
            ESP_LOGW(TAG, "[reclaim] largest=%u < need per=%u x %u block(s) -> "
                          "unloading CAM-page residents first (FACE button will show OFF)",
                     (unsigned)largest, (unsigned)per, (unsigned)blocks);
            (void)cam_reclaim_residents();          /* §73.15：统一 helper（幂等） */
        }
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
     *     face_service_detect() 的坐标系不用动（§73.12 后 color_hsv 已下线）。
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

    /* ★ 本次实际要几个缓冲：媒体档用 MEDIA_BUF_CNT（可单独调小以省连续 PSRAM），
     *   预览档用 BUF_CNT。两者都必须 <= BUF_CNT —— s_buf[] 是按 BUF_CNT 开的
     *   （下面加了 _Static_assert 钉住）。 */
    const uint32_t nbuf = cam_stream_buf_cnt(fmt);

    bool streamon_retried = false;      /* ★ §73.15：STREAMON ENOMEM 腾地重试，只试一次 */
streambuf:                              /* ← 重试入口：重新 REQBUFS → mmap/QBUF → STREAMON */
    struct v4l2_requestbuffers req = {
        .count = nbuf,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        /* ★★ §73.13/§73.15：REQBUFS ENOMEM → 自动腾地重试一次。
         *   腾地列表 = det 模型 + 识别引擎 + viewfinder 三件套
         *   （cam_reclaim_residents，幂等；前提是此刻相机没跑起来 ⇒ 就地卸载安全）。
         *   REQBUFS 可在同一 fd 上原地重发（驱动侧旧缓冲在失败路径已被释放）。 */
        bool reclaimed = false;
        if (errno == ENOMEM) {
            reclaimed = cam_reclaim_residents() &&
                        (ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0);
            if (reclaimed) {
                ESP_LOGI(TAG, "REQBUFS retry OK after reclaim");
            }
        }
        if (!reclaimed) {
            /* ★★ §73 新增：REQBUFS fail 的根因翻译。
             *   驱动日志 "esp_video: Failed to create buffer" 的唯一含义：
             *   esp_video_buffer_create() 逐块 aligned_alloc 切不出
             *   per-block 大小的连续 PSRAM（esp_video_setup_buffer() 在此
             *   **之前**已经把旧流缓冲释放掉了 —— "CAM 没还"不成立）。
             *   ★ §73.15 文案修正：REQBUFS 只占 nbuf 块；backup 是 **STREAMON 阶段**
             *     esp_cam_new_dvp_ctlr() 分配的（esp_cam_ctlr_dvp_cam.c:866-867，
             *     §73.13 判其"不存在"是 grep 漏了 IDF 本体组件，详见 psram_need 注释）。 */
            const size_t per     = cam_stream_frame_bytes(fmt, w, h);
            const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
            ESP_LOGE(TAG, "REQBUFS fail (errno=%d): driver allocates %u buffer(s) "
                          "of %u B each (backup comes later at STREAMON); PSRAM largest=%u -> %s",
                     errno, (unsigned)nbuf, (unsigned)per, (unsigned)largest,
                     (largest < per) ? "LARGEST TOO SMALL (fragmentation / big holder resident)"
                                     : "largest fits - read the driver log above");
            cam_mem_residents_report();
            goto err;
        }
    }

    for (uint32_t i = 0; i < nbuf; i++) {
        struct v4l2_buffer b = {.type=V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory=V4L2_MEMORY_MMAP, .index=i};
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) goto err;
        s_buf[i] = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        if (s_buf[i] == MAP_FAILED) { s_buf[i] = NULL; goto err; }
        s_buf_len[i] = b.length;          /* ★ 记下真长，munmap 用它 */
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) goto err;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        /* ★★ §73.15（本轮现场日志定案）：STREAMON 阶段的 ENOMEM = **DVP backup
         *   buffer 分配失败** —— `esp_cam_new_dvp_ctlr()`（esp_cam_ctlr_dvp_cam.c:866-867）
         *   在此刻创建 DVP 控制器，要多切**一块帧同尺寸**（JPEG 720p = 921,600 B）的
         *   backup。REQBUFS 的 nbuf 块已把洞吃光 ⇒ backup 切不出（现场 largest=917,504
         *   < 921,600，差 4KB）。与 REQBUFS 一样：腾地（det/recog/viewfinder）后
         *   **重走 REQBUFS→QBUF→STREAMON** 一次。
         *   §73.13 的教训：判据/重试必须覆盖到 STREAMON —— 只兜 REQBUFS 救不了
         *   "REQBUFS 成功、backup 失败"这一形态。 */
        if (errno == ENOMEM && !streamon_retried && cam_reclaim_residents()) {
            streamon_retried = true;
            ESP_LOGW(TAG, "STREAMON ENOMEM (DVP backup?) -> reclaimed residents, "
                          "retrying REQBUFS..STREAMON once (FACE button will show OFF)");
            /* 先拆干净：munmap 已映射的缓冲 + REQBUFS(count=0) 释放驱动侧缓冲 */
            for (uint32_t i = 0; i < nbuf; i++) {
                if (s_buf[i]) { munmap(s_buf[i], s_buf_len[i]); s_buf[i] = NULL; s_buf_len[i] = 0; }
            }
            struct v4l2_requestbuffers req0 = req;
            req0.count = 0;
            ioctl(s_fd, VIDIOC_REQBUFS, &req0);
            goto streambuf;
        }
        /* ★★ 2026-09-28：把驱动那句含糊的 "no mem" 翻译成可行动的根因。
         *   现场原始日志见 cam_stream_mem_check() 的注释。这一段的价值是：
         *   以后只要看到本行，就知道该往"省内存/减块数"走，而不是去查 caps、
         *   去查 SD 卡、去查 sensor。 */
        /* ★★ 2026-09-28 判据修正：真正的硬约束是"**单块** per 字节连续"，
         *   不是 "total = per*(n+1) 字节连续"（n+1 次分配彼此独立、不需要相邻）。
         *   原版拿 total 比 largest ⇒ largest 落在 (per, total) 时会把"够"误判成碎片。
         *   ⚠ 这一段原本的文案（"PSRAM free 几 MB 却失败"）就是 §67 那条误导的源头之一。 */
        const size_t per     = cam_stream_frame_bytes(fmt, w, h);
        const size_t total   = cam_stream_psram_need(fmt, w, h);
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "STREAMON fail (errno=%d). If the driver log above says "
                      "`no mem for DVP backup buffer` or `failed to create DVP`: "
                      "PSRAM largest=%u B vs per-block需要=%u B (%u bufs + 1 backup, %u B total) -> %s",
                 errno, (unsigned)largest, (unsigned)per,
                 (unsigned)(cam_stream_buf_cnt(fmt) + 1u), (unsigned)total,
                 (largest < per) ? "FRAGMENTATION: free 大概率够，但没有一块这么大的连续区 "
                                   "（减 MEDIA_BUF_CNT 或先让出大块）"
                                 : "largest 已够放单块 —— 去读上面驱动的原始日志");
        cam_mem_residents_report();     /* ★ §73：把"谁占着大块"一并报出来 */
        goto err;
    }

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

bool camera_service_has_frame_cb(void) { return s_cb != NULL; }   /* §73.16：区分 CAM/VIDEO 预览 */

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
