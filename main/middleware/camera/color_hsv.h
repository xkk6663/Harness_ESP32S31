/* color_hsv.h - RGB565 color detection */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x, y, w, h;  /* bounding box in pixels, w=0 => no target */
} color_bbox_t;

/* Detect red pixels in RGB565 frame. Returns bbox. */
void color_hsv_detect_red(const uint16_t *rgb565, uint32_t w, uint32_t h, color_bbox_t *out);

#ifdef __cplusplus
}
#endif
