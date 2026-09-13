/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"

#include "ui_splash.h"
#include "middleware/fs_service.h"

static const char *TAG = "SPLASH";

#define SPLASH_PATH      FS_MNT_PATH"/xwkkk.jpg"
#define SWIPE_UP_DY      (-80)   /* swipe-up threshold (px) */

static ui_splash_enter_cb_t s_enter_cb;
static lv_obj_t *s_canvas;
static lv_obj_t *s_hint;
static int32_t s_press_start_y;
static bool s_entered;

/* Decode xwkkk.jpg into the shared PSRAM file_buffer and draw into canvas. */
static void splash_decode_into(lv_obj_t *canvas)
{
    struct stat st;
    if (stat(SPLASH_PATH, &st) != 0) {
        ESP_LOGE(TAG, "splash image not found: %s", SPLASH_PATH);
        return;
    }
    size_t fsize = st.st_size;
    uint8_t *jpeg_buf = heap_caps_malloc(fsize, MALLOC_CAP_DMA);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "no dma mem for jpeg buf");
        return;
    }

    int fd = open(SPLASH_PATH, O_RDONLY);
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
        lv_canvas_set_buffer(canvas, file_buffer, info.width, info.height, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(canvas);
        ESP_LOGI(TAG, "splash image %dx%d decoded", info.width, info.height);
    } else {
        ESP_LOGE(TAG, "jpeg decode failed");
    }

cleanup:
    if (dec) {
        jpeg_dec_close(dec);
    }
    free(jpeg_buf);
}

/* ---------------- public: reusable wallpaper layer ---------------- */

lv_obj_t *ui_wallpaper_attach(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(layer, lv_color_hex(0x0F2547), 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_radius(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    s_canvas = lv_canvas_create(layer);
    lv_obj_center(s_canvas);

    splash_decode_into(s_canvas);
    return layer;
}

/* ---------------- splash swipe-up handlers ---------------- */

static void splash_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_press_start_y = p.y;
    }
}

static void splash_pressing_cb(lv_event_t *e)
{
    if (s_entered) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_press_start_y;

    if (s_hint) {
        int32_t move = dy / 4;
        if (move < 0) {
            lv_obj_set_y(s_hint, BSP_LCD_V_RES - 90 + move);
        }
    }

    if (dy < SWIPE_UP_DY) {
        s_entered = true;
        ESP_LOGI(TAG, "swipe up detected, entering main UI");
        if (s_enter_cb) {
            s_enter_cb();
        }
    }
}

void ui_splash_show(ui_splash_enter_cb_t cb)
{
    s_enter_cb = cb;
    s_entered = false;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F2547), 0);

    lv_obj_t *wp = ui_wallpaper_attach(scr);
    lv_obj_add_flag(wp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wp, splash_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(wp, splash_pressing_cb, LV_EVENT_PRESSING, NULL);

    s_hint = lv_label_create(scr);
    lv_label_set_text(s_hint, LV_SYMBOL_UP"  SWIPE UP TO ENTER");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x22D3EE), 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(s_hint, 2, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -30);
}
