/* face_service.h - Face detection via esp-dl, C ABI for C UI layer */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_SERVICE_MAX_FACES 4

typedef struct {
    int x, y, w, h;
    float score;
} face_box_t;

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
 * Run detection on an RGB565LE frame.
 * Returns number of faces found (<= max).
 */
int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        face_box_t *out, int max);

/* ===================== ★ S1 多模型档位（3 档同族） =====================
 * 设计见 docs/开发文档/多模型加载策略_S1方案.md
 *
 * ★★ 先读这段再改 UI：现在"模型是否就绪"与"检测是否打开"是**两个独立概念**。
 *    · face_service_state()   = 模型状态（IDLE/LOADING/RUNNING/FAIL）
 *    · face_service_enabled() = 检测开关
 *    模型**常驻**：关掉检测不会释放模型（实测常驻只花 264 B 内部 RAM、
 *    largest 零变化，却省掉每次进页面 ~0.9 s 加载，并把"加载期花屏"限制成开机后仅一次）。
 *    所以按钮文案必须按 (state, enabled) 两组值决定，不能只看 state。
 * ---------------------------------------------------------------------- */

/** 档位总数（= 表长，UI 建按钮/算 next 用它，不要写死 3）。 */
int face_service_model_count(void);
/** 档位短文案（ASCII）；id 越界返回 "?"。 */
const char *face_service_model_label(int id);
/** 当前生效档位；模型未就绪时返回"上一次成功的档位"，从未成功过则返回默认档。 */
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
