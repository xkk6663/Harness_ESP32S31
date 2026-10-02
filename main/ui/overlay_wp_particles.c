/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wp_particles.c
 * @brief Wallpaper (wallpaper_service 双缓冲当前图) + floating particles overlay.
 *        RGB888 canvas, PSRAM double-buffer style: copy base frame, draw dots.
 *        §75：底图不再自持拷贝，直接借用 wallpaper_service 当前 buffer（省 1.15MB）。
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "overlay_wp_particles.h"
#include "middleware/periph/fs_service.h"
#include "middleware/media/wallpaper_service.h"

static const char *TAG = "WP_PART";

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
static uint8_t    *s_base;    /* decoded static wallpaper (借用 service 当前 buffer，只读) */
static particle_t s_p[PARTICLE_COUNT];
static uint16_t s_w, s_h;
static size_t   s_pixels;
static int32_t  s_touch_x = -1, s_touch_y = -1;
static uint32_t s_wp_gen_seen;

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
    /* §75：代际号轮询（splash 生命周期短、30ms 一拍代价可忽略）——
     * service 换图后把 s_base 指针切到新 buffer 即可（零拷贝），下一拍
     * memcpy 自然带出新图。splash 粒子层叠在底图上，跳变不可见，不做 crossfade。
     * ★ 只在 LVGL 线程切指针（本回调），与 service 任务写"另一块 buffer"无竞争。 */
    uint32_t gen = wallpaper_service_get_gen();
    if (gen != s_wp_gen_seen) {
        s_wp_gen_seen = gen;
        uint8_t *b = wallpaper_service_get_buffer(wallpaper_service_get_current());
        if (b) {
            s_base = b;
            ESP_LOGI(TAG, "base switched to service buf%d (gen=%u)",
                     wallpaper_service_get_current(), (unsigned)gen);
        }
        wallpaper_service_commit();     /* splash 不播动画，看完立即归还 */
    }

    if (s_base) {
        memcpy(s_buf, s_base, s_pixels * 3);
    }
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

    s_buf  = heap_caps_malloc(buflen, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "psram alloc fail");
        return NULL;
    }

    /* §75：底图借用 wallpaper_service 当前 buffer（开机时 service 已同步解码
     * 上次壁纸/兜底图，此处零解码零拷贝）。service 异常未就绪时退深蓝纯色。 */
    s_base = wallpaper_service_get_buffer(wallpaper_service_get_current());
    s_wp_gen_seen = wallpaper_service_get_gen();
    if (!s_base) {
        ESP_LOGW(TAG, "service buffer unavailable -> solid dark blue base");
        for (size_t i = 0; i < s_pixels; i++) put_px(i, 10, 20, 40);
    }

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_buf, s_w, s_h, LV_COLOR_FORMAT_RGB888);
    lv_obj_center(s_canvas);

    /* touch: repel particles from finger */
    lv_obj_add_event_cb(s_canvas, wp_touch_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_clickable(s_canvas, true);

    for (int i = 0; i < PARTICLE_COUNT; i++) spawn(&s_p[i], true);
    if (s_base) {
        memcpy(s_buf, s_base, buflen);
    }

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
