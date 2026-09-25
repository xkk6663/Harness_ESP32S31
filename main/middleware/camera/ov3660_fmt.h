/* ov3660_fmt.h - OV3660 运行时格式档位表（供 VIDIOC_S_SENSOR_FMT 使用）
 *
 * ★★★ 为什么必须有这个文件（2026-09-25 逐行核实，**推翻了本工程原方案 §2.2 的一处说法**）
 *
 *   原说法是"esp_video 的 DVP 设备支持运行时切格式：VIDIOC_S_FMT"。**不准确**：
 *
 *   ① `VIDIOC_S_FMT` 在 DVP 上**改不了分辨率/像素格式**。
 *      `esp_video_dvp_device.c:381 dvp_video_set_format()` 里写得很死：
 *        if (pix->width != CAPTURE_VIDEO_GET_FORMAT_WIDTH(video) ||
 *            pix->height != CAPTURE_VIDEO_GET_FORMAT_HEIGHT(video))  → 直接 EINVAL
 *      它只**校验** pixelformat（且只在 sensor 是 RGB565 时校验），不碰传感器。
 *
 *   ② 真正改格式的入口是 esp_video 的**私有 ioctl**：
 *        VIDIOC_S_SENSOR_FMT   （esp_video_ioctl.h:24）
 *          → esp_video_ioctl_set_sensor_format()（esp_video_ioctl.c:355）
 *          → dvp_video_set_sensor_format()（esp_video_dvp_device.c:453）
 *          → esp_cam_sensor_set_format() → ov3660_set_format()
 *      切完之后 `dvp_video_set_sensor_format()` 会**立刻重跑 init_config()**，
 *      而 init_config() 会用**新的 sensor 尺寸**重算并分配流缓冲
 *      （`esp_video_config_buffer(video, &format, DVP_MEM_CAPS)`）
 *      ⇒ 所以"JPEG 720p 的 mmap 缓冲够不够大"这个担心（方案 §2.5.7 风险 1）**由设计化解**，
 *        前提是：**只能从 S_SENSOR_FMT 进去**，不能用 S_FMT。
 *
 *   ③ 而 `ov3660_set_format()` 会把 `format->regs` **强转成 `ov3660_reginfo_t *`** 去写寄存器
 *      （`ov3660.c:558-577`：`ov3660_write_array(handle, (ov3660_reginfo_t *)format->regs)`）
 *      ⇒ **regs 必须指向一份真实的寄存器表，传 NULL 必炸**。
 *
 *   ④ 那份寄存器表在 esp_cam_sensor 的 `private_include` 目录里（不对外导出），
 *      而 BSP（`bsp_camera.c` 只有 `bsp_camera_start()`）和 esp_video 都**不暴露 sensor 句柄**
 *      ⇒ userspace 拿不到 `esp_cam_sensor_device_t *`，
 *        也就无法用 `esp_cam_sensor_query_format()` 去枚举出带 regs 的条目。
 *      ⇒ **只能由本工程自己提供一份带 regs 的 `esp_cam_sensor_format_t`**，就是这个文件。
 *
 * ★★ 本文件的设计选择：**引用**供应商私有头，而不是**复制**寄存器值
 *
 *   - 复制（把 ov3660_settings.h + 两个表头搬进本目录，约 10.5 KB）的坏处是**静默过期**：
 *     供应商改了时序/寄存器，我们那份副本不会跟着变 —— 画面悄悄变差、且没有任何日志。
 *   - 引用私有头的坏处是**耦合**：`main/CMakeLists.txt` 必须把
 *       managed_components/espressif__esp_cam_sensor/sensors/ov3660/private_include
 *     加进 `PRIV_INCLUDE_DIRS`。但一旦供应商重构目录，**编译期就大声报错**（找不到宏），
 *     逼我们同步 —— 这是**可以接受的失败方式**（响亮 > 静默）。
 *   - ⇒ 选引用。**若将来该路径失效导致构建失败（这是期望行为），再退回"复制"方案**
 *     （复制清单：ov3660_types.h + ov3660_regs.h + ov3660_settings.h + 两个 *_12fps.h /
 *      *_24fps.h 表头，以及删掉 CMakeLists 里那条 PRIV_INCLUDE_DIRS）。
 *
 * ★ 另：`ov3660_settings.h` 里除了宏，还定义了 `static const ov3660_sensor_default_regs[]`
 *   （供应商驱动自己用的"通用寄存器组"，本文件用不到）⇒ 会触发 `-Wunused-const-variable`。
 *   在 .c 里用 pragma 局部关掉即可，**不要**为了让告警消失去"引用"它。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_cam_sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按 (宽, 高, 是否 JPEG) 查本工程收录的 OV3660 档位
 *
 * @param width      目标宽度
 * @param height     目标高度
 * @param want_jpeg  true = JPEG 档，false = RGB565 档
 * @return 指向**静态**格式描述的指针（内含 valid regs，可直接喂 VIDIOC_S_SENSOR_FMT）；
 *         未收录则返回 NULL —— **调用方必须报错，不要猜、不要兜底成别的档**。
 */
const esp_cam_sensor_format_t *ov3660_fmt_lookup(uint32_t width, uint32_t height, bool want_jpeg);

#ifdef __cplusplus
}
#endif
