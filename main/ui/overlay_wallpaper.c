/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_wallpaper.c
 * @brief Pull-down wallpaper overlay: JPEG decode + drag gestures.
 *        Extracted from the former ui_splash.c / ui_disp.c.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"

#include "overlay_wallpaper.h"
#include "overlay_manager.h"
#include "ui_theme.h"
#include "middleware/fs_service.h"

static const char *TAG = "WP_OVERLAY";

#define WALLPAPER_PATH   FS_MNT_PATH"/xwkkk.jpg"
#define PULL_TOP_ZONE    60     /* px from top to arm pull-down   */
#define PULL_DOWN_OPEN   200    /* px travel to fully open        */
#define PULL_ANIM_MS     220

static lv_obj_t *s_wallpaper;
static lv_obj_t *s_tab_btns;
static int32_t   s_pull_start_y;
static bool      s_pull_armed;

/* Decode xwkkk.jpg into the shared PSRAM file_buffer and bind to a canvas. */
static void wallpaper_decode_into(lv_obj_t *canvas)
{
    struct stat st;
    if (stat(WALLPAPER_PATH, &st) != 0) {
        ESP_LOGE(TAG, "wallpaper not found: %s", WALLPAPER_PATH);
        return;
    }
    size_t fsize = st.st_size;
    uint8_t *jpeg_buf = heap_caps_malloc(fsize, MALLOC_CAP_DMA);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "no dma mem for jpeg buf");
        return;
    }

    int fd = open(WALLPAPER_PATH, O_RDONLY);
    if (fd < 0) {
        free(jpeg_buf);
        return;
    }
    read(fd, jpeg_buf, fsize);
    close(fd);

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = {0};
    jpeg_dec_io_t io = {
        .inbuf = jpeg_buf,
        .inbuf_len = (int)fsize,
        .outbuf = file_buffer,
    };
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
#if CONFIG_LV_COLOR_16_SWAP
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
#endif

    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        goto cleanup;
    }
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg header parse failed");
        goto cleanup;
    }
    int out_len = 0;
    jpeg_dec_get_outbuf_len(dec, &out_len);
    if ((size_t)out_len > file_buffer_size) {
        ESP_LOGE(TAG, "jpeg too large: %d > %u", out_len, (unsigned)file_buffer_size);
        goto cleanup;
    }
    if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
        lv_canvas_set_buffer(canvas, file_buffer, info.width, info.height,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_center(canvas);
        ESP_LOGI(TAG, "wallpaper %dx%d decoded", info.width, info.height);
    } else {
        ESP_LOGE(TAG, "jpeg decode failed");
    }

cleanup:
    if (dec) {
        jpeg_dec_close(dec);
    }
    free(jpeg_buf);
}

lv_obj_t *overlay_wallpaper_build(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(layer, COL_BG, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_radius(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *canvas = lv_canvas_create(layer);
    lv_obj_center(canvas);
    wallpaper_decode_into(canvas);
    return layer;
}

/* ---- anim helper ---- */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void wallpaper_animate_to(lv_obj_t *wp, int32_t target_y)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wp);
    lv_anim_set_values(&a, lv_obj_get_y(wp), target_y);
    lv_anim_set_time(&a, PULL_ANIM_MS);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ---- pull-down: drag wallpaper out of the top edge ---- */
static void main_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = (p.y < PULL_TOP_ZONE);
}

static void main_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy > 0) {
        int32_t y = -BSP_LCD_V_RES + dy;
        if (y > 0) {
            y = 0;
        }
        lv_obj_set_y(s_wallpaper, y);
    }
}

static void main_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        s_pull_armed = false;
        return;
    }
    int32_t y = lv_obj_get_y(s_wallpaper);
    if (y > -BSP_LCD_V_RES + PULL_DOWN_OPEN) {
        overlay_open(OVERLAY_WALLPAPER);   /* snaps to y=0 via show() */
    } else {
        wallpaper_animate_to(s_wallpaper, -BSP_LCD_V_RES);
    }
    s_pull_armed = false;
}

/* ---- swipe-up on open wallpaper to dismiss ---- */
static void wp_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_start_y = p.y;
    s_pull_armed = true;
}

static void wp_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_pull_start_y;
    if (dy < 0) {
        int32_t y = dy;
        if (y < -BSP_LCD_V_RES) {
            y = -BSP_LCD_V_RES;
        }
        lv_obj_set_y(s_wallpaper, y);
    }
}

static void wp_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pull_armed || !s_wallpaper) {
        s_pull_armed = false;
        return;
    }
    int32_t y = lv_obj_get_y(s_wallpaper);
    if (y < -120) {
        overlay_close(OVERLAY_WALLPAPER);  /* snaps shut via hide() */
    } else {
        wallpaper_animate_to(s_wallpaper, 0);
    }
    s_pull_armed = false;
}

void overlay_wallpaper_init(lv_obj_t *tab_btns)
{
    s_tab_btns = tab_btns;

    s_wallpaper = overlay_wallpaper_build(lv_layer_top());
    lv_obj_set_y(s_wallpaper, -BSP_LCD_V_RES);
    lv_obj_add_flag(s_wallpaper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wallpaper, wp_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_wallpaper, wp_released_cb, LV_EVENT_RELEASED, NULL);

    if (s_tab_btns) {
        lv_obj_add_event_cb(s_tab_btns, main_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_tab_btns, main_pressing_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(s_tab_btns, main_released_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_flag(s_tab_btns, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
}

void overlay_wallpaper_deinit(void)
{
    if (s_wallpaper) {
        lv_obj_del(s_wallpaper);
        s_wallpaper = NULL;
    }
    s_tab_btns = NULL;
    s_pull_armed = false;
}

void overlay_wallpaper_show(void)
{
    if (s_wallpaper) {
        wallpaper_animate_to(s_wallpaper, 0);
    }
}

void overlay_wallpaper_hide(void)
{
    if (s_wallpaper) {
        wallpaper_animate_to(s_wallpaper, -BSP_LCD_V_RES);
    }
}
