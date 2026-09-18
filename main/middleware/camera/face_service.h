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

/** Start the AI service (lazy-loads model on first detect). */
esp_err_t face_service_start(void);
/** Stop and release model / PSRAM. */
esp_err_t face_service_stop(void);
/**
 * Run detection on an RGB565LE frame.
 * Returns number of faces found (<= max). Boxes are in original frame coords.
 */
int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        face_box_t *out, int max);
bool face_service_is_running(void);

#ifdef __cplusplus
}
#endif
