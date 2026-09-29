/* face_service.h - esp-dl 视觉模型服务（C ABI for C UI layer）
 *
 * ★ §60 起语义升级：不再只有人脸——"模型档位"支持**异族**多档
 *   （FACE 人脸检测 / HAND 手势两段式）。
 *   文件名沿用 face_service（历史沿革），内部已是 vision 服务。
 *
 * 档位与模型（SDCARD 模式，/littlefs/models/，由 littlefs_content/models/ 烧录）：
 *   FACE  HumanFaceDetect(ESPDET_PICO_224_224_FACE)         espdet_pico_224_224_face.espdl
 *   HAND  HandDetect(ESPDET_PICO_224_224_HAND) 两段式       espdet_pico_224_224_hand.espdl
 *         + HandGestureCls(MOBILENETV2_0_5_S8_V1)           mobilenetv2_0_5_128_128_gesture.espdl
 *   （原 416 / MSR/MNP 档已阉割；**POSE 档亦于 §62 阉割** —— 3.0MB 的 yolo11n-pose
 *     需要一块 3.04MB **连续** PSRAM，本机碎片环境下装不下，档位与模型文件一并删除。）
 * ★ 档位数量对外只经 face_service_model_count()/model_label() 暴露，
 *   UI 不要写死档数/档名 —— §62 删档后 UI 未改一行即自动变 2 档。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VISION_MAX_RESULTS 4

typedef struct {
    int   x, y, w, h;
    float score;
    char  label[12];               /* HAND：手势名（one/ok/like...）；其余为空串 */
} vision_result_t;

typedef enum {
    FACE_IDLE     = 0,
    FACE_LOADING  = 1,
    FACE_RUNNING  = 2,
    FACE_FAIL     = -1,
} face_state_t;

/** Request async model load (called from LVGL task; does the real load later). */
void face_service_request_load(void);
/** Must be called periodically from cam_task to perform pending load. */
void face_service_poll(void);
/** 停止检测（**模型常驻**，D6 —— 实际只翻 enabled，不释放任何内存）。
 *  ⚠ 名字里的 "stop" 容易让人以为会卸模型 —— 不会。要真释放请用
 *    face_service_unload()（本文件下方，投递给 cam_task 执行）。 */
esp_err_t face_service_stop(void);
/** Query current state (UI uses this to color the button). */
face_state_t face_service_state(void);
/**
 * Run current model on an RGB565LE frame (240x240 canvas).
 * Returns number of results (<= max). HAND 档的 label/score 为手势类别与置信度
 * （其余档 label 为空串、score 为检测框置信度）。
 */
int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        vision_result_t *out, int max);

/* ===================== 多模型档位（异族三档，§60） =====================
 * ★★ 先读这段再改 UI："模型是否就绪"与"检测是否打开"是**两个独立概念**。
 *    · face_service_state()   = 模型状态（IDLE/LOADING/RUNNING/FAIL）
 *    · face_service_enabled() = 检测开关
 *    ★★ §73.10（2026-09-29 用户定案）：**"模型常驻（D6）"策略已废除** ——
 *    检测关（按钮 OFF / 录入借用归还 / 起流腾地）一律连模型一起卸载，
 *    PSRAM 一分不留；再次打开重新构建（~0.5 s）。这是"按需分配、
 *    即用即释放"总纪律的一部分，不要改回去。
 *    按钮文案必须按 (state, enabled) 两组值决定，不能只看 state。
 *
 * ★★★ 2026-09-28（用户要求"不同按钮功能解耦"）——**职责边界，改动前必读**：
 *    本模块 = **检测**（face / hand）。它回答"有没有脸、在哪"。
 *    它**不负责**识别（"这是谁"）—— 那是 face_recog_service（MSR/MNP + MFN）。
 *    ⇒ FACE 按钮（检测开关）**只**翻这个 bool，绝不去动识别引擎。
 *      识别引擎的加载/释放由**打卡**与**录入**两个会话各自管理。
 * ---------------------------------------------------------------------- */

/** 档位总数（= 表长，UI 建按钮/算 next 用它，不要写死）。 */
int face_service_model_count(void);
/**
 * ★ §73.14：**实际已加载**的模型份数（det 1 份 + HAND 档的 gcls 1 份）。
 *   与 model_count()（档位总数常量）语义完全不同 —— 内存判据/住户报告一律用本函数。
 *   ⚠ 历史教训：§73.13 现场日志里 residents 报 "det 模型 ×2 在场" 就是把
 *   model_count() 当"已加载数"用的恒真误报，把排障方向带偏了一轮。
 */
int face_service_loaded_models(void);
/** 档位短文案（ASCII，如 "FACE"/"HAND"）；id 越界返回 "?"。 */
const char *face_service_model_label(int id);
/** 当前生效档位；模型未就绪时返回"上一次成功的档位"，从未成功过则返回 0（FACE）。 */
int face_service_current_model(void);
/**
 * 请求切到指定档位（异步）。LVGL 线程只置请求，真正的"先卸后建"由
 * cam_task 的 face_service_poll() 执行 —— 绝不能在 LVGL 线程里构建模型。
 * 返回值：ESP_OK 已受理；ESP_ERR_INVALID_ARG id 越界；
 *         ESP_ERR_INVALID_STATE 请求的正是当前档位（无需切换）。
 */
esp_err_t face_service_request_model(int id);

/** 检测开关：false = 停止检测但**保留模型**；true = 若模型没好则顺带触发构建。 */
esp_err_t face_service_set_enabled(bool on);
bool face_service_enabled(void);

/**
 * ★★★ 2026-09-28 新增：**停止检测 + 彻底卸载模型 + 释放 scratch**（幂等）。
 *
 * 与 `face_service_stop()` 的区别：
 *   · `face_service_stop()` —— 只把 `s_enabled` 翻成 false，**模型留在内存**；
 *   · `face_service_unload()` —— 真的把 PSRAM 还回去。
 *     以 FACE 档为例：模型 495,840 B + slack 64 KB ≈ 548 KB 连续块，
 *     外加 `s_scratch` 240x240x3 = 172,800 B；HAND 档两段合计 ≈ 1.48 MB。
 *
 * ★ §73.10 起 **det 按钮的 OFF 也走卸载**（face_btn_cb 用的是 unload_sync）
 *   —— 本函数保留给"已知检测不会再用了、但相机还在跑"的会话收尾场景
 *   （录入借用归还）。stop() 目前只剩 set_enabled(false) 内部一个调用者。
 *
 * ★ **只能在 LVGL 线程调用**，而且它只**投递意图**：真正的卸载发生在 cam_task 的
 *   `face_service_poll()` 里 —— ⚠ 相机停着时 poll 不跑，请求会变**死信**；
 *   那种场景必须用下面的 `face_service_unload_sync()`。
 * ★ 卸载后 `face_service_current_model()` 仍返回"上次成功的档位"，
 *   下一次 `face_service_set_enabled(true)` 会**重新构建**它（约 450~900 ms）。
 */
esp_err_t face_service_unload(void);

/**
 * @brief 同步版彻底卸载（**给起流前的"腾地"用**，§73.9）。
 *
 * 与 unload() 的区别：unload() 只投递请求，真正的释放在 face_service_poll()
 * 里 —— 而 poll 只在 cam_task 跑，**相机停着时永远没人消费**，模型就一直占着
 * PSRAM。本函数在"相机不在跑"时**就地**完成卸载（此刻没有帧 ⇒ 帧处理块
 * 进不来 ⇒ scratch 无人写 ⇒ 释放是安全的）；相机在跑则退化为投递。
 *
 * ★ 副作用：det 开关会被关掉（s_enabled=false）——调用方（camera_service
 *   腾地）必须打日志把这件事说清楚，用户回 CAM 页会看到 FACE 按钮是 OFF。
 */
esp_err_t face_service_unload_sync(void);


#ifdef __cplusplus
}
#endif
