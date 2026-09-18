/* color_hsv.c - RGB565 red detection via simple RGB threshold */
#include "color_hsv.h"
#include "esp_log.h"

#define TAG "HSV"

static inline void rgb565_to_rgb888(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (p >> 11) & 0x1F;
    *g = (p >> 5) & 0x3F;
    *b = p & 0x1F;
    /* expand to 8 bit */
    *r = (*r << 3) | (*r >> 2);
    *g = (*g << 2) | (*g >> 4);
    *b = (*b << 3) | (*b >> 2);
}

void color_hsv_detect_red(const uint16_t *rgb565, uint32_t w, uint32_t h, color_bbox_t *out)
{
    out->x = out->y = out->w = out->h = 0;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    int count = 0;
    int total_r = 0, total_g = 0, total_b = 0, samples = 0;
    /* sample every 2x2 pixels for speed */
    for (uint32_t y = 0; y < h; y += 2) {
        for (uint32_t x = 0; x < w; x += 2) {
            uint8_t r, g, b;
            rgb565_to_rgb888(rgb565[y * w + x], &r, &g, &b);
            total_r += r; total_g += g; total_b += b; samples++;
            /* red: high R, low G/B (relaxed threshold) */
            if (r > 80 && r > g + 20 && r > b + 20) {
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
                count++;
            }
        }
    }
    static int s_dbg_skip;
    if (++s_dbg_skip % 15 == 0 && samples > 0) {
        ESP_LOGI(TAG, "avg RGB=(%d,%d,%d) red_pixels=%d/%d",
                 total_r/samples, total_g/samples, total_b/samples, count, samples);
    }
    if (count < 10) return;  /* too few pixels, ignore */
    out->x = minx;
    out->y = miny;
    out->w = maxx - minx;
    out->h = maxy - miny;
}
