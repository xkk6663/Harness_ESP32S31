/* camera_service.h - OV3660 preview service wrapper
 *
 * ★★ 2026-09-25（S2-0）接口升级，三点变更，改调用方前先读：
 *   ① 帧回调 **加了 size**：RGB565 是定长帧，JPEG 是**变长帧** ⇒ 没有 size 就没法写盘。
 *      size 来源 = V4L2 的 `buf.bytesused`（esp_video 在 DQBUF 时填 element->valid_size，
 *      见 esp_video_ioctl.c:198）。空帧/坏帧**不会**被回调（驱动置 V4L2_BUF_FLAG_ERROR
 *      而非 DONE，cam_task 已经靠这个过滤）。
 *   ② 新增 `camera_service_start_fmt*()`：**格式/尺寸/归属**都由参数指定。
 *      格式切换（VIDIOC_S_SENSOR_FMT）就发生在 start 内部 ⇒
 *      **调用方不需要、也不应该自己切格式**。理由见 ov3660_fmt.h 的文件头。
 *   ③ 新增 `camera_service_owner()`：给"另一个 tab 想知道相机在不在忙"用。
 *      录像可能持续几分钟，期间 CAM 页必须置灰并提示，**不能静默失败**。
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 帧回调（在 cam_task 上下文被调用，**不是 LVGL 线程**）
 *
 * @param data   帧数据。RGB565 模式下是 w*h*2 字节小端；JPEG 模式下是完整 JPEG 码流。
 *               ★ 指针**只在回调期间有效** —— 回调返回后 cam_task 立刻 VIDIOC_QBUF 归还缓冲，
 *                 内容随时可能被 DVP 覆盖。要留数据必须**在回调内拷走/写盘**。
 * @param size   帧字节数（= buf.bytesused）。JPEG 变长，务必用它，不要自己按 w*h 算。
 * @param width  当前流宽度（运行时值，不是编译期常量）
 * @param height 当前流高度
 * @param user   透传
 *
 * ★★ 回调里**不得**做任何长时间阻塞或碰 LVGL 的操作：
 *    - 碰 LVGL：非 LVGL 线程操作 LVGL 是未定义行为（本工程铁律，回调只置 volatile 标志）；
 *    - 长时间阻塞（如写 SD 卡）：DVP 只有 BUF_CNT 个缓冲，阻塞久了会连续丢帧。
 *      真正的落盘请交给独立任务（见 capture_service / record_video_service）。
 */
typedef void (*camera_frame_cb_t)(const uint8_t *data, uint32_t size,
                                  uint32_t width, uint32_t height, void *user);

/** 输出格式档位（本工程只收录这两种，见 ov3660_fmt.c） */
typedef enum {
    CAM_FMT_RGB565 = 0,   /* 240x240，定长帧，给预览 / 模型 / 人脸检测用 */
    CAM_FMT_JPEG,         /* 1280x720，变长帧，给拍照 / 录像用（传感器硬件编码） */
} cam_fmt_t;

/**
 * 相机占用者。**这是两个 tab 和平共处的唯一机制**（录像期间 CAM 页要置灰）。
 * 归属由 `camera_service_start_fmt_owner()` 显式声明；
 * 5 参数的 `camera_service_start_fmt()` 会按格式推断（RGB565⇒PREVIEW，JPEG⇒MEDIA）。
 */
typedef enum {
    CAM_OWNER_NONE = 0,   /* 没在跑 */
    CAM_OWNER_PREVIEW,    /* CAM tab 的 RGB565 预览（含跑模型） */
    CAM_OWNER_MEDIA,      /* VIDEO tab 正在拍照/录像（**独占**） */
} cam_owner_t;

/* 两个档位的尺寸常量（避免调用方到处写魔数） */
#define CAM_PREVIEW_W    240u
#define CAM_PREVIEW_H    240u
#define CAM_MEDIA_W     1280u
#define CAM_MEDIA_H      720u

/* JPEG 质量范围（OV3660 语义：**值越小质量越高、码流越大**）。
 * 注意这与常见"1..100 越大越好"相反，改之前先看 ov3660.c:222 ov3660_set_jpeg_quality。 */
#define CAM_JPEG_Q_BEST      1
#define CAM_JPEG_Q_DEFAULT   8     /* 1280x720@12fps 的稳妥默认档 */
#define CAM_JPEG_Q_WORST    63

esp_err_t camera_service_init(void);

/** 兼容旧调用：等价于 start_fmt(cb, user, CAM_FMT_RGB565, 240, 240)。 */
esp_err_t camera_service_start(camera_frame_cb_t cb, void *user);

/**
 * @brief 按格式/尺寸启动流（对外主入口）
 *
 * 若当前 sensor 档位与请求不符，内部会自动先 `VIDIOC_S_SENSOR_FMT` 切档
 * （随后驱动自己重算流缓冲尺寸）。已是目标档位时**不会**重复切（幂等）。
 *
 * @param owner 归属按格式推断：RGB565⇒CAM_OWNER_PREVIEW，JPEG⇒CAM_OWNER_MEDIA
 *              （与"两个 tab = 两种格式"的职责划分一致，见方案 §2.5.1）。
 * @return ESP_OK / ESP_ERR_INVALID_ARG（档位未收录）/ ESP_ERR_INVALID_STATE（上个任务没退）/ 其他
 */
esp_err_t camera_service_start_fmt(camera_frame_cb_t cb, void *user,
                                   cam_fmt_t fmt, uint32_t w, uint32_t h);

/**
 * @brief 同上，但**显式声明归属**（需要对外主入口之外的语义时用）。
 *        5 参数的 `camera_service_start_fmt()` 内部就是推断出 owner 后转调本函数。
 */
esp_err_t camera_service_start_fmt_owner(camera_frame_cb_t cb, void *user,
                                         cam_fmt_t fmt, uint32_t w, uint32_t h,
                                         cam_owner_t owner);

esp_err_t camera_service_stop(void);
bool      camera_service_is_running(void);

/**
 * @brief 当前流的实际尺寸/格式（运行时值）。未启动过时返回默认预览档。
 *        任一出参可为 NULL。
 */
void camera_service_get_stream(uint32_t *w, uint32_t *h, cam_fmt_t *fmt);
/**
 * ★ §73.16：当前流是否注册了**帧回调**。
 *   CAM 页预览 on_frame() 非空；VIDEO 页取景流回调传 NULL —— 这是区分
 *   "CAM 页该收到帧却没收到（真故障）"与"VIDEO 页预览（正常）"的唯一可靠判据
 *   （两页共用 CAM_OWNER_PREVEW，owner 区分不了）。
 */
bool camera_service_has_frame_cb(void);

/** 当前占用者。CAM 页在 refresh() 里读它决定是否置灰（读了之后不要再自己"猜状态"）。 */
cam_owner_t camera_service_owner(void);

/**
 * @brief 注册"第二观察者"回调（2026-09-25，为 VIDEO 页实时取景器而加）
 *
 * 与 `camera_service_start*()` 的主回调的关系：**一帧两个消费者**。
 * cam_task 每收到一帧（V4L2_BUF_FLAG_DONE）会先调主回调、再调观察者，
 * 两者互不影响 —— 观察者不存在时开销为零。
 *
 * ★★ 为什么要这个：拍照/录像接管相机后，主回调归了 capture/record 服务，
 *    但取景器**仍然**要看到画面（用户需求："录视频的原画面可预览"）。
 *    观察者就是"旁听席"：不管此刻跑的是预览流、拍照流还是录像流，它都能收到帧。
 *
 * ★★ 观察者的硬约束（违反会直接丢帧，见 camera_service.h 顶部帧回调的说明）：
 *    - 在 **cam_task 上下文**被调用，**不是** LVGL 线程 ⇒ 不得碰任何 LVGL 对象；
 *    - **不得长时间阻塞**（DVP 只有 BUF_CNT 个缓冲）；允许的开销是
 *      "一次 memcpy + 置标志"，毫秒级以内；
 *    - 可以传 NULL 清除；清除后不会再被调用（但不保证"清完的瞬间没有一帧
 *      在半路上"⇒ 实现方要自己容忍多出来的最后一次回调）。
 *
 * ★ 线程安全：内部用 volatile 指针保存，允许在流运行中随时增删。
 */
void camera_service_set_observer(camera_frame_cb_t cb, void *user);

/**
 * @brief 把上一次的**预览流**重新拉起来（拍照/录像收尾时的"归还"）
 *
 * 方案 §2.5.4 的事务模型是"接管 → 干活 → 归还"，其中"归还"= 恢复之前的 RGB565 预览，
 * 这样"拍完自动回预览，人脸检测可继续用"（阶段 S2a 的验收判据）才能成立。
 *
 * ★ 为什么需要这个 API 而不是让调用方自己再 start 一次：
 *   start 需要 `(cb, user, fmt, w, h)`，而拍照/录像事务**并不知道** CAM 页用的是哪个回调
 *   （回调属于 UI 层）。⇒ 由 camera_service 在**每次以 CAM_OWNER_PREVIEW 起流时**
 *   记住这份描述，归还时原样重放。
 * ★ 记的是"**预览**"这一种：以 CAM_OWNER_MEDIA 起的流**不**记录
 *   —— 否则归还会把上一个 JPEG 媒体流原样拉回来，方向就错了。
 *
 * @return ESP_OK 已恢复；ESP_ERR_NOT_FOUND 之前从没有过预览（没什么可归还）；
 *         ESP_ERR_INVALID_STATE 相机此刻仍被占用（应先 stop）；其他为 start 的错误码
 */
esp_err_t camera_service_restore_preview(void);

/**
 * @brief 设置 JPEG 质量（仅 JPEG 模式下有效）
 * @param quality 1..63，**越小质量越高**；越界返回 ESP_ERR_INVALID_ARG。
 *                RGB565 模式下返回 ESP_ERR_INVALID_STATE（传感器会拒绝）。
 */
esp_err_t camera_service_set_jpeg_quality(int quality);

#ifdef __cplusplus
}
#endif
