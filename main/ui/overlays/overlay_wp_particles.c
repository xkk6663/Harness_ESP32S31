/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wp_particles.c
 * @brief Wallpaper (wallpaper_service 双缓冲当前图) + floating particles overlay.
 *        §75：底图 s_base 不再自持/自解码 —— 借 service 当前 buffer（零拷贝），
 *        gen 轮询发现新图后只切指针并 commit 归还，省 768KB PSRAM。
 *        本文件的性能红线（RGB565 画布 / 局部失效 / 失效矩形上限）原样保留。
 *
 * ⚠⚠ 性能红线（改这个文件前务必读完，否则会踩 task_wdt 崩溃）⚠⚠
 *
 *  1) 画布格式必须 == 屏幕帧缓冲格式。
 *     屏幕是 RGB565（bsp/display.h: BSP_LCD_COLOR_FORMAT = ESP_LCD_COLOR_FORMAT_RGB565），
 *     所以 canvas 必须是 LV_COLOR_FORMAT_RGB565。
 *     历史 bug：这里曾用 LV_COLOR_FORMAT_RGB888，LVGL 每帧要走
 *     `rgb888_image_blend()`（lv_draw_sw_blend_to_rgb565.c）把 800x480 逐像素转成
 *     RGB565 —— 38.4 万次/帧。加上每帧 1.15MB 全屏 memcpy，单帧耗时超过 30ms 定时周期，
 *     于是 taskLVGL 永远处于"就绪"状态、永不阻塞，IDLE0 拿不到 CPU → task_wdt 报
 *     "taskLVGL starved IDLE0"，PC 落在 rgb888_image_blend。
 *     改成 RGB565 后 LVGL 走 `rgb565_image_blend()` 的 lv_memcpy 快路径，这条慢路径彻底消失。
 *
 *  2) 禁止全屏重绘。静态壁纸只在初始化时拷一次；
 *     每 tick 只从 s_base 回填上一帧 24 个粒子的"脚印"（几个 7x7 小矩形），
 *     再画新位置。
 *     历史 bug：曾用 `lv_obj_invalidate(s_canvas)` —— 那是**整屏** 800x480 重绘。
 *     现在改用 `lv_obj_invalidate_area()` 只失效粒子所在小矩形。
 *
 *  3) 失效矩形数量有硬上限。LVGL 的 `disp->inv_areas[]` 只有 LV_INV_BUF_SIZE(=32) 个槽；
 *     一旦某次刷新前累计的失效区超过 32 个，lv_refr.c 会**丢弃整个列表并换成全屏失效**，
 *     优化直接失效。所以这里用 dirty_push() 把 24 个粒子框合并、硬顶到 DIRTY_MAX(=16) 个。
 *
 *  关于 DIRECT 双缓冲：本工程走 esp_lvgl_port 的 avoid_tearing 分支
 *  （BSP_LCD_RGB_BUFFER_NUMS=2 + CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE），
 *  LVGL 用 LV_DISPLAY_RENDER_MODE_DIRECT 直接渲染进 RGB 帧缓冲。
 *  该模式下局部失效是**被支持**的：lv_refr.c 会把本次失效区记到 sync_areas，
 *  下一帧 refr_sync_areas() 再把这些小矩形同步到另一块帧缓冲，不会闪烁。
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "overlay_wp_particles.h"
#include "middleware/media/wallpaper_service.h"

static const char *TAG = "WP_PART";

#define PARTICLE_COUNT   24
#define TICK_MS          30

/* 单次刷新允许提交给 LVGL 的失效矩形上限（必须 < LV_INV_BUF_SIZE=32，留余量给其它控件） */
#define DIRTY_MAX        16
/* 两个粒子框间距小于该值时合并成一个矩形，避免失效区数量爆炸 */
#define DIRTY_MERGE_GAP  6

typedef struct {
    float x, y;
    float vx, vy;
    uint8_t r, g, b;
    uint8_t rsz;
    int32_t px, py;   /* 上一帧实际画下的位置/半径，用于回填脚印 */
    int32_t pr;
} particle_t;

static lv_obj_t   *s_canvas;
static lv_timer_t *s_timer;
static uint16_t   *s_buf;     /* 显示用画布（RGB565，和屏幕同格式） */
static uint16_t   *s_base;    /* 静态壁纸（RGB565；§75：借 service 当前 buffer，只读） */
static particle_t s_p[PARTICLE_COUNT];
static uint16_t s_w, s_h;
static size_t   s_pixels;
static int32_t  s_touch_x = -1, s_touch_y = -1;
static uint32_t s_wp_gen_seen;

static lv_area_t s_dirty[DIRTY_MAX];
static int32_t   s_dirty_cnt;

static uint32_t rnd_state = 20240912;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
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
    p->px = -1000; p->py = -1000; p->pr = 0;   /* 还没画过 */
}

/* ---------------------------------------------------------------- 失效矩形管理 */

static void dirty_reset(void)
{
    s_dirty_cnt = 0;
}

static inline bool rects_near(const lv_area_t *a, const lv_area_t *b, int32_t gap)
{
    return !(a->x2 + gap < b->x1 || b->x2 + gap < a->x1 ||
             a->y2 + gap < b->y1 || b->y2 + gap < a->y1);
}

static inline void rect_union(lv_area_t *a, const lv_area_t *b)
{
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->y2 > a->y2) a->y2 = b->y2;
}

static inline int32_t rect_size(const lv_area_t *a)
{
    return (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
}

/**
 * @brief 登记一个待失效矩形。会尽量与已有矩形合并，并保证数量 <= DIRTY_MAX。
 */
static void dirty_push(const lv_area_t *box)
{
    for (int32_t i = 0; i < s_dirty_cnt; i++) {
        if (!rects_near(&s_dirty[i], box, DIRTY_MERGE_GAP)) continue;

        rect_union(&s_dirty[i], box);
        /* 合并后 i 可能又和后面的矩形挨上了，收敛一遍 */
        for (int32_t j = i + 1; j < s_dirty_cnt; ) {
            if (rects_near(&s_dirty[i], &s_dirty[j], DIRTY_MERGE_GAP)) {
                rect_union(&s_dirty[i], &s_dirty[j]);
                s_dirty[j] = s_dirty[--s_dirty_cnt];   /* 用尾巴填坑 */
                j = i + 1;                             /* 重新扫 */
            } else {
                j++;
            }
        }
        return;
    }

    if (s_dirty_cnt < DIRTY_MAX) {
        s_dirty[s_dirty_cnt++] = *box;
        return;
    }

    /* 满员：并入"面积增量最小"的那个矩形，把总失效面积压住 */
    int32_t best = 0;
    int32_t best_grow = 0x7FFFFFFF;
    for (int32_t i = 0; i < s_dirty_cnt; i++) {
        lv_area_t u = s_dirty[i];
        rect_union(&u, box);
        int32_t grow = rect_size(&u) - rect_size(&s_dirty[i]);
        if (grow < best_grow) { best_grow = grow; best = i; }
    }
    rect_union(&s_dirty[best], box);
}

/* 把一个圆心(cx,cy)半径r的圆的外接矩形裁剪到画布范围内后登记 */
static void dirty_push_dot(int32_t cx, int32_t cy, int32_t r)
{
    if (r <= 0) return;
    /* 左右上下各留 1px 余量，避免取整误差留下残影 */
    lv_area_t a;
    a.x1 = cx - r - 1;
    a.y1 = cy - r - 1;
    a.x2 = cx + r + 1;
    a.y2 = cy + r + 1;
    if (a.x1 < 0) a.x1 = 0;
    if (a.y1 < 0) a.y1 = 0;
    if (a.x2 > (int32_t)s_w - 1) a.x2 = (int32_t)s_w - 1;
    if (a.y2 > (int32_t)s_h - 1) a.y2 = (int32_t)s_h - 1;
    if (a.x1 > a.x2 || a.y1 > a.y2) return;   /* 完全在屏幕外 */
    dirty_push(&a);
}

/* 把合并后的矩形一次性提交给 LVGL */
static void dirty_flush(void)
{
    if (s_dirty_cnt == 0) return;

    /* ⚠ lv_obj_invalidate_area() 收的是**屏幕绝对坐标**（内部会拿它和 obj->coords 求交，
     *   见 lv_obj_pos.c: lv_obj_area_is_visible()）。而我们记录的脏矩形是"画布缓冲坐标"，
     *   所以这里必须补上画布在屏幕上的偏移 —— 否则一旦父容器有 padding/偏移，
     *   就会去失效屏幕上另一块区域（表现为粒子拖影、残影）。
     *   这也是为什么不能直接用 lv_obj_invalidate()：那个与位置无关，但代价是整屏重绘。 */
    lv_area_t coords;
    lv_obj_get_coords(s_canvas, &coords);
    const int32_t ox = coords.x1;
    const int32_t oy = coords.y1;

    for (int32_t i = 0; i < s_dirty_cnt; i++) {
        lv_area_t a = s_dirty[i];
        a.x1 += ox; a.x2 += ox;
        a.y1 += oy; a.y2 += oy;
        lv_obj_invalidate_area(s_canvas, &a);
    }
    s_dirty_cnt = 0;
}

/* ---------------------------------------------------------------- 壁纸底图（§75 借用） */

/* Decode xwkkk.jpg 的自解码路径已删（V2.1）：底图由 wallpaper_service 开机
 * 同步解码并提供（见 build()），本文件只负责"gen 轮询 → 切指针 → commit"。 */

/* ---------------------------------------------------------------- 绘制 */

static inline void put_px(size_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    s_buf[idx] = rgb565(r, g, b);
}

static void draw_dot(const particle_t *p)
{
    int32_t cx = (int32_t)p->x, cy = (int32_t)p->y, r = p->rsz;
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t y = cy + dy;
        if (y < 0 || y >= (int32_t)s_h) continue;
        for (int32_t dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int32_t x = cx + dx;
            if (x < 0 || x >= (int32_t)s_w) continue;
            put_px((size_t)y * s_w + x, p->r, p->g, p->b);
        }
    }
}

/**
 * @brief 用壁纸像素回填一块矩形，撤销上一帧画下的粒子（只动几个小矩形，不做全屏 memcpy）。
 */
static void erase_dot(int32_t cx, int32_t cy, int32_t r)
{
    if (!s_base) return;            /* §75：service 未就绪降级态，无底图可回填 */
    if (r <= 0) return;
    int32_t x1 = cx - r, x2 = cx + r;
    int32_t y1 = cy - r, y2 = cy + r;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)s_w - 1) x2 = (int32_t)s_w - 1;
    if (y2 > (int32_t)s_h - 1) y2 = (int32_t)s_h - 1;
    if (x1 > x2 || y1 > y2) return;

    size_t bytes = (size_t)(x2 - x1 + 1) * sizeof(uint16_t);
    for (int32_t y = y1; y <= y2; y++) {
        size_t off = (size_t)y * s_w + x1;
        memcpy(&s_buf[off], &s_base[off], bytes);
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

    /* §75：代际号轮询（30ms 一拍代价可忽略）—— service 换图后把 s_base
     * 指针切到新 buffer（零拷贝），下一拍回填自然带出新图。splash 粒子层
     * 叠在底图上，跳变不可见，不做 crossfade。
     * ★ 只在 LVGL 线程切指针（本回调）；service 任务只写"另一块 buffer"，
     *   且翻面后等 commit 才会再解码 ⇒ 读写永不重叠（§75 契约）。 */
    uint32_t gen = wallpaper_service_get_gen();
    if (gen != s_wp_gen_seen) {
        s_wp_gen_seen = gen;
        uint16_t *b = (uint16_t *)(void *)wallpaper_service_get_buffer(
                          wallpaper_service_get_current());
        if (b) {
            s_base = b;
            ESP_LOGI(TAG, "base switched to service buf%d (gen=%u)",
                     wallpaper_service_get_current(), (unsigned)gen);
        }
        wallpaper_service_commit();     /* splash 不播动画，看完立即归还 */
    }

    dirty_reset();

    /* 1) 回填上一帧的脚印（局部矩形，代价可忽略） */
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        const particle_t *p = &s_p[i];
        if (p->pr > 0) {
            erase_dot(p->px, p->py, p->pr);
            dirty_push_dot(p->px, p->py, p->pr);
        }
    }

    /* 2) 推进物理 + 画新位置 */
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        particle_t *p = &s_p[i];

        /* repel from touch point */
        if (s_touch_x >= 0) {
            float dx = p->x - s_touch_x;
            float dy = p->y - s_touch_y;
            float d2 = dx * dx + dy * dy;
            if (d2 < 120 * 120 && d2 > 1) {
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

        p->px = (int32_t)p->x;
        p->py = (int32_t)p->y;
        p->pr = p->rsz;
        draw_dot(p);
        dirty_push_dot(p->px, p->py, p->pr);
    }

    /* 3) 一次性提交（<= DIRTY_MAX 个小矩形，绝不是全屏） */
    dirty_flush();
}

lv_obj_t *overlay_wp_particles_build(lv_obj_t *parent)
{
    s_w = BSP_LCD_H_RES;
    s_h = BSP_LCD_V_RES;
    s_pixels = (size_t)s_w * s_h;
    /* RGB565: 800*480*2 = 768000 B（显示画布，必须与屏幕同格式） */
    size_t buflen = s_pixels * sizeof(uint16_t);

    s_buf  = heap_caps_malloc(buflen, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "psram alloc fail (%u bytes)", (unsigned)buflen);
        return NULL;
    }

    /* §75：底图借用 wallpaper_service 当前 buffer（app_main 已在 bsp_display_start
     * 之前同步解码"上次壁纸/兜底图"，此处零解码零拷贝，同时省掉旧版自持的 768KB）。
     * service 异常未就绪时退深蓝纯色（erase/回填路径已做 NULL 守卫）。 */
    s_base = (uint16_t *)(void *)wallpaper_service_get_buffer(
                 wallpaper_service_get_current());
    s_wp_gen_seen = wallpaper_service_get_gen();
    if (!s_base) {
        ESP_LOGW(TAG, "service buffer unavailable -> solid dark blue base");
    }

    /* 先铺深蓝兜底色，service 异常时也不会是花屏/纯黑 */
    for (size_t i = 0; i < s_pixels; i++) s_buf[i] = rgb565(10, 20, 40);
    if (s_base) {
        memcpy(s_buf, s_base, buflen);
    }

    s_canvas = lv_canvas_create(parent);
    /* ⚠ 必须与屏幕帧缓冲同格式（RGB565）。用 RGB888 会让 LVGL 每帧走
     *   rgb888_image_blend() 逐像素转换，把 taskLVGL 顶到 100% 触发 task_wdt。 */
    lv_canvas_set_buffer(s_canvas, s_buf, s_w, s_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    /* 画布不能滚动：否则拖动会让画布内容整体偏移，而我们按屏幕绝对坐标做局部失效 */
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);

    /* touch: repel particles from finger
     *
     * ⚠ 只监听 PRESSING/RELEASED，**不要注册 LV_EVENT_ALL**。
     *   注册 ALL 会让 canvas 成为"事件目标"，LVGL 命中测试停在它身上，
     *   父级（splash 的壁纸层）永远收不到 PRESSED/PRESSING ——
     *   表现为"上拉要划两次才生效"（见 F31）。
     *   这里只做粒子避让的视觉反馈，顺带保留 CLICKABLE 即可。 */
    lv_obj_add_event_cb(s_canvas, wp_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, wp_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_set_clickable(s_canvas, true);

    for (int i = 0; i < PARTICLE_COUNT; i++) spawn(&s_p[i], true);

    s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    ESP_LOGI(TAG, "particles running %dx%d RGB565 (buf %u B x2)",
             s_w, s_h, (unsigned)buflen);
    return s_canvas;
}

void overlay_wp_particles_stop(void)
{
    if (s_timer) {
        ESP_LOGI(TAG, "particles timer stopped");
        lv_timer_del(s_timer); s_timer = NULL;
    }
}

lv_obj_t *overlay_wp_particles_canvas(void)
{
    return s_canvas;
}
