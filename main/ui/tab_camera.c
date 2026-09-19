/* tab_camera.c - Camera preview tab */
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "tab_camera.h"
#include "ui_theme.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/color_hsv.h"
#include "middleware/camera/face_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

#define TAG "TAB_CAM"
#define CAM_W 240
#define CAM_H 240

static lv_obj_t *s_canvas;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_btn_hsv;
static lv_obj_t *s_btn_hsv_label;
static lv_obj_t *s_btn_face;
static lv_obj_t *s_btn_face_label;
static lv_obj_t *s_bbox;
static lv_obj_t *s_face_bbox;
static lv_obj_t *s_status;
static lv_obj_t *s_screen;
static lv_timer_t *s_timer;
static uint16_t *s_canvas_buf;
static volatile const uint8_t *s_last_frame;
static volatile bool s_frame_new;
static bool s_fullscreen;
static bool s_hsv_on;
static bool s_face_on;
static int s_frame_skip;

static void on_frame(const uint8_t *rgb565, uint32_t w, uint32_t h, void *user)
{
    (void)w; (void)h; (void)user;
    s_last_frame = rgb565;
    s_frame_new = true;
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    /* update face button state (async load completes on cam_task) */
    face_state_t st = face_service_state();
    if (st == FACE_RUNNING) {
        if (s_face_on && !strstr(lv_label_get_text(s_btn_face_label), "ON")) {
            lv_label_set_text_static(s_btn_face_label, "FACE ON");
            lv_obj_set_style_bg_color(s_btn_face, lv_color_hex(0x00FF00), 0);
            lv_label_set_text_static(s_status, "Face ready");
        }
    } else if (st == FACE_FAIL) {
        if (s_face_on) {
            s_face_on = false;
            lv_label_set_text_static(s_btn_face_label, "FAIL");
            lv_obj_set_style_bg_color(s_btn_face, lv_color_hex(0xFF0000), 0);
            lv_label_set_text_static(s_status, "Face load FAIL");
        }
    }
    if (s_frame_new && s_last_frame && s_canvas) {
        s_frame_new = false;
        lv_lock();
        /* Sensor outputs RGB565 big-endian; LVGL canvas expects little-endian.
         * Also flip vertically (BSP default VFLIP=0). */
        const uint16_t *src = (const uint16_t *)s_last_frame;
        uint16_t *dst = s_canvas_buf;
        for (int y = 0; y < CAM_H; y++) {
            const uint16_t *src_row = src + (CAM_H - 1 - y) * CAM_W;
            uint16_t *dst_row = dst + y * CAM_W;
            for (int x = 0; x < CAM_W; x++) {
                uint16_t p = src_row[CAM_W - 1 - x];  /* hflip */
                dst_row[x] = ((p >> 8) | (p << 8));    /* byte swap */
            }
        }
        lv_obj_invalidate(s_canvas);
        if (s_hsv_on && s_bbox) {
            color_bbox_t b;
            /* detect on the corrected canvas buffer (already flipped + byteswapped) */
            color_hsv_detect_red(s_canvas_buf, CAM_W, CAM_H, &b);
            if (b.w > 0 && b.h > 0) {
                lv_obj_set_pos(s_bbox, b.x, b.y);
                lv_obj_set_size(s_bbox, b.w, b.h);
                lv_obj_clear_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_face_on && s_face_bbox) {
            s_frame_skip++;
            if (s_frame_skip >= 3) {  /* run detection every 3 frames */
                s_frame_skip = 0;
                face_box_t fb[FACE_SERVICE_MAX_FACES];
                int n = face_service_detect(s_canvas_buf, CAM_W, CAM_H, fb, FACE_SERVICE_MAX_FACES);
                if (n > 0) {
                    /* draw largest face */
                    int idx = 0;
                    int max_area = 0;
                    for (int i = 0; i < n; i++) {
                        int a = fb[i].w * fb[i].h;
                        if (a > max_area) { max_area = a; idx = i; }
                    }
                    lv_obj_set_pos(s_face_bbox, fb[idx].x, fb[idx].y);
                    lv_obj_set_size(s_face_bbox, fb[idx].w, fb[idx].h);
                    lv_obj_clear_flag(s_face_bbox, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_face_bbox, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        lv_unlock();
    }
}

static void toggle_fullscreen(lv_event_t *e)
{
    (void)e;
    s_fullscreen = !s_fullscreen;
    if (s_fullscreen) {
        /* Fullscreen: zoom 2x (240->480), center on 800x480 */
        lv_obj_set_style_transform_zoom(s_canvas, 512, 0);
        lv_obj_set_style_transform_pivot_x(s_canvas, CAM_W/2, 0);
        lv_obj_set_style_transform_pivot_y(s_canvas, CAM_H/2, 0);
        lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_style_transform_zoom(s_canvas, 256, 0);
        lv_obj_set_parent(s_canvas, s_screen);
        lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    }
}

static void hsv_btn_cb(lv_event_t *e)
{
    (void)e;
    s_hsv_on = !s_hsv_on;
    lv_label_set_text_static(s_btn_hsv_label, s_hsv_on ? "HSV ON" : "HSV");
    lv_obj_set_style_bg_color(s_btn_hsv, s_hsv_on ? COL_RED : COL_CYAN, 0);
    if (!s_hsv_on && s_bbox) lv_obj_add_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);
}

static void face_btn_cb(lv_event_t *e)
{
    (void)e;
    face_state_t st = face_service_state();
    if (st == FACE_LOADING) return;  /* loading in progress, ignore */

    if (st == FACE_RUNNING) {
        /* stop */
        face_service_stop();
        s_face_on = false;
        if (s_face_bbox) lv_obj_add_flag(s_face_bbox, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(s_btn_face_label, "FACE");
        lv_obj_set_style_bg_color(s_btn_face, COL_CYAN, 0);
        lv_label_set_text_static(s_status, "Face off");
    } else {
        /* request async load on cam_task */
        face_service_request_load();
        s_face_on = true;
        s_frame_skip = 99;
        lv_label_set_text_static(s_btn_face_label, "LOADING...");
        lv_obj_set_style_bg_color(s_btn_face, lv_color_hex(0xFFFF00), 0);
        lv_label_set_text_static(s_status, "Loading model...");
    }
}

static void btn_cb(lv_event_t *e)
{
    (void)e;
    if (camera_service_is_running()) {
        camera_service_stop();
        lv_label_set_text_static(s_btn_label, "START");
        lv_label_set_text_static(s_status, "Stopped");
    } else {
        esp_err_t err = camera_service_start(on_frame, NULL);
        if (err == ESP_OK) {
            lv_label_set_text_static(s_btn_label, "STOP");
            lv_label_set_text_static(s_status, "Preview running");
        } else {
            lv_label_set_text_static(s_status, "Camera open failed");
        }
    }
}

void tab_camera_build(lv_obj_t *screen, lv_group_t *group)
{
    s_screen = screen;
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_top(screen, 16, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_VIDEO "  CAMERA");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Canvas 320x240 RGB565 */
    if (!s_canvas_buf) {
        s_canvas_buf = heap_caps_aligned_alloc(64, CAM_W * CAM_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_canvas_buf) {
            ESP_LOGE(TAG, "canvas buf alloc fail");
            return;
        }
        memset(s_canvas_buf, 0, CAM_W * CAM_H * 2);
    }
    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CAM_W, CAM_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_border_color(s_canvas, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_canvas, 1, 0);
    lv_obj_set_style_radius(s_canvas, 8, 0);
    lv_obj_add_event_cb(s_canvas, toggle_fullscreen, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    /* Bounding box overlay (child of canvas, hidden by default) */
    s_bbox = lv_obj_create(s_canvas);
    lv_obj_set_size(s_bbox, 10, 10);
    lv_obj_set_style_border_color(s_bbox, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_pad_all(s_bbox, 0, 0);
    lv_obj_set_style_border_post(s_bbox, true, 0);
    lv_obj_set_style_border_width(s_bbox, 2, 0);
    lv_obj_set_style_border_opa(s_bbox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_bbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_bbox, 0, 0);
    lv_obj_add_flag(s_bbox, LV_OBJ_FLAG_HIDDEN);

    /* Face bbox (green) */
    s_face_bbox = lv_obj_create(s_canvas);
    lv_obj_set_size(s_face_bbox, 10, 10);
    lv_obj_set_style_border_color(s_face_bbox, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(s_face_bbox, 2, 0);
    lv_obj_set_style_border_opa(s_face_bbox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_face_bbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_face_bbox, 0, 0);
    lv_obj_add_flag(s_face_bbox, LV_OBJ_FLAG_HIDDEN);

    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Stopped");
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    lv_obj_t *btn_row = lv_obj_create(screen);
    lv_obj_set_size(btn_row, 480, 60);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 12, 0);

    s_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn, 140, 60);
    lv_obj_set_style_radius(s_btn, 30, 0);
    lv_obj_set_style_bg_color(s_btn, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text_static(s_btn_label, "START");
    lv_obj_set_style_text_color(s_btn_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_label, UI_FONT, 0);
    lv_obj_center(s_btn_label);

    s_btn_hsv = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn_hsv, 140, 60);
    lv_obj_set_style_radius(s_btn_hsv, 30, 0);
    lv_obj_set_style_bg_color(s_btn_hsv, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn_hsv, hsv_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_hsv_label = lv_label_create(s_btn_hsv);
    lv_label_set_text_static(s_btn_hsv_label, "HSV");
    lv_obj_set_style_text_color(s_btn_hsv_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_hsv_label, UI_FONT, 0);
    lv_obj_center(s_btn_hsv_label);

    s_btn_face = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn_face, 140, 60);
    lv_obj_set_style_radius(s_btn_face, 30, 0);
    lv_obj_set_style_bg_color(s_btn_face, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn_face, face_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_face_label = lv_label_create(s_btn_face);
    lv_label_set_text_static(s_btn_face_label, "FACE");
    lv_obj_set_style_text_color(s_btn_face_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_face_label, UI_FONT, 0);
    lv_obj_center(s_btn_face_label);

    if (group) lv_group_add_obj(group, s_btn);
    s_timer = lv_timer_create(refresh, 50, NULL);
}

void tab_camera_destroy(void)
{
    if (camera_service_is_running()) camera_service_stop();
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_canvas = NULL; s_btn = NULL; s_btn_label = NULL; s_status = NULL;
    s_btn_hsv = NULL; s_btn_hsv_label = NULL; s_bbox = NULL;
    s_last_frame = NULL; s_frame_new = false;
    s_hsv_on = false; s_fullscreen = false;
}
