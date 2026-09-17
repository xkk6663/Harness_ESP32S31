/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wp_particles.c
 * @brief Static wallpaper (xwkkk.jpg) + floating particles overlay.
 *        RGB888 canvas, PSRAM double-buffer style: copy base frame, draw dots.
 */

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "overlay_wp_particles.h"
#include "middleware/fs_service.h"

static const char *TAG = "WP_PART";

#define WALLPAPER_PATH   FS_MNT_PATH"/xwkkk.jpg"
#define PARTICLE_COUNT   24
#define TICK_MS           30

typedef struct {
    float x, y;
    float vx, vy;
    uint8_t r, g, b;
    uint8_t rsz;
} particle_t;

static lv_obj_t   *s_canvas;
static lv_timer_t *s_timer;
static uint8_t    *s_buf;     /* displayed canvas (RGB888) */
static uint8_t    *s_base;    /* decoded static wallpaper copy */
static particle_t s_p[PARTICLE_COUNT];
static uint16_t s_w, s_h;
static size_t   s_pixels;
static int32_t  s_touch_x = -1, s_touch_y = -1;

static uint32_t rnd_state = 20240912;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static void spawn(particle_t *p, bool random_y)
{
    p->x = (float)(rnd() % s_w);
    p->y = random_y ? (float)(rnd() % s_h) : -10.0f;
    p->vx = ((float)(rnd() % 100) / 100.0f - 0.5f) * 1.2f;
    p->vy = 0.8f + (float)(rnd() % 80) / 100.0f;
    p->rsz = 2 + (rnd() % 3);
    p->r = 180 + (rnd() % 60);
    p->g = 210 + (rnd() % 40);
    p->b = 255;
}

/* Decode xwkkk.jpg into s_base (RGB888) */
static bool decode_base(void)
{
    struct stat st;
    if (stat(WALLPAPER_PATH, &st) != 0) {
        ESP_LOGE(TAG, "wallpaper not found: %s", WALLPAPER_PATH);
        return false;
    }
    size_t fsize = st.st_size;
    uint8_t *jpeg_buf = heap_caps_malloc(fsize, MALLOC_CAP_DMA);
    if (!jpeg_buf) return false;

    int fd = open(WALLPAPER_PATH, O_RDONLY);
    if (fd < 0) { free(jpeg_buf); return false; }
    read(fd, jpeg_buf, fsize);
    close(fd);

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = { .inbuf = jpeg_buf, .inbuf_len = (int)fsize, .outbuf = s_base };
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;

    bool ok = false;
    if (jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK &&
        jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK) {
        if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
            s_w = info.width; s_h = info.height;
            s_pixels = (size_t)s_w * s_h;
            /* JPEG RGB888 output is BGR; swap R/B to match LVGL */
            for (size_t i = 0; i < s_pixels; i++) {
                uint8_t *p = &s_base[i * 3];
                uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
            }
            ESP_LOGI(TAG, "wallpaper decoded %dx%d RGB888", s_w, s_h);
            ok = true;
        }
    } else {
        ESP_LOGE(TAG, "jpeg decode failed");
    }
    if (dec) jpeg_dec_close(dec);
    free(jpeg_buf);
    return ok;
}

static inline void put_px(size_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = &s_buf[idx * 3];
    p[0] = r; p[1] = g; p[2] = b;
}

static void draw_dot(particle_t *p)
{
    int32_t cx = (int32_t)p->x, cy = (int32_t)p->y, r = p->rsz;
    for (int32_t dy = -r; dy <= r; dy++) {
        for (int32_t dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy > r*r) continue;
            int32_t x = cx + dx, y = cy + dy;
            if (x >= 0 && x < (int32_t)s_w && y >= 0 && y < (int32_t)s_h) {
                put_px((size_t)y * s_w + x, p->r, p->g, p->b);
            }
        }
    }
}

static void wp_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) {
        s_touch_x = p.x; s_touch_y = p.y;
    } else if (code == LV_EVENT_RELEASED) {
        s_touch_x = -1; s_touch_y = -1;
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    memcpy(s_buf, s_base, s_pixels * 3);
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        particle_t *p = &s_p[i];
        /* repel from touch point */
        if (s_touch_x >= 0) {
            float dx = p->x - s_touch_x;
            float dy = p->y - s_touch_y;
            float d2 = dx*dx + dy*dy;
            if (d2 < 120*120 && d2 > 1) {
                float d = __builtin_sqrtf(d2);
                float force = (120.0f - d) / 120.0f * 3.0f;
                p->vx += dx / d * force;
                p->vy += dy / d * force;
            }
        }
        /* damping + gravity upward */
        p->vx *= 0.92f;
        p->vy = p->vy * 0.92f + 0.5f;
        p->x += p->vx;
        p->y += p->vy;
        if (p->y > s_h + 10 || p->x < -10 || p->x > s_w + 10) {
            spawn(p, false);
        }
        draw_dot(p);
    }
    lv_obj_invalidate(s_canvas);
}

lv_obj_t *overlay_wp_particles_build(lv_obj_t *parent)
{
    s_w = BSP_LCD_H_RES;
    s_h = BSP_LCD_V_RES;
    s_pixels = (size_t)s_w * s_h;
    size_t buflen = s_pixels * 3;

    s_base = heap_caps_malloc(buflen, MALLOC_CAP_SPIRAM);
    s_buf  = heap_caps_malloc(buflen, MALLOC_CAP_SPIRAM);
    if (!s_base || !s_buf) {
        ESP_LOGE(TAG, "psram alloc fail");
        return NULL;
    }
    memset(s_base, 0, buflen);

    if (!decode_base()) {
        /* fallback: dark blue base */
        for (size_t i = 0; i < s_pixels; i++) put_px(i, 10, 20, 40);
    }

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_buf, s_w, s_h, LV_COLOR_FORMAT_RGB888);
    lv_obj_center(s_canvas);

    /* touch: repel particles from finger */
    lv_obj_add_event_cb(s_canvas, wp_touch_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < PARTICLE_COUNT; i++) spawn(&s_p[i], true);
    memcpy(s_buf, s_base, buflen);

    s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    ESP_LOGI(TAG, "particles running %dx%d", s_w, s_h);
    return s_canvas;
}

void overlay_wp_particles_stop(void)
{
    if (s_timer) {
        ESP_LOGI(TAG, "particles timer stopped");
        lv_timer_del(s_timer); s_timer = NULL;
    }
}
