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

#ifdef __cplusplus
}
#endif
