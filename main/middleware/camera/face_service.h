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
/** Stop and release model / PSRAM. */
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
 *    模型**常驻**：关掉检测不释放模型（切族才卸——先卸后建）。
 *    按钮文案必须按 (state, enabled) 两组值决定，不能只看 state。
 * ---------------------------------------------------------------------- */

/** 档位总数（= 表长，UI 建按钮/算 next 用它，不要写死）。 */
int face_service_model_count(void);
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


#ifdef __cplusplus
}
#endif
