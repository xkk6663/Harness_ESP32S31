/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_idf_version.h"

#include "ui_disp.h"
#include "ui_settings.h"
#include "middleware/audio_service.h"
#include "middleware/button_service.h"
#include "freertos/queue.h"

#define COL_BG        lv_color_hex(0x0F2547)
#define COL_CARD      lv_color_hex(0x1E3A5F)
#define COL_BORDER    lv_color_hex(0x3B5A82)
#define COL_CYAN      lv_color_hex(0x22D3EE)
#define COL_TEXT      lv_color_hex(0xE2E8F0)
#define COL_SUBTEXT   lv_color_hex(0x94A3B8)

static lv_obj_t *s_vol_slider;

static void volume_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int vol = lv_slider_get_value(slider);
    button_service_set_volume(vol);
    audio_cmd_t cmd = {
        .id = AUDIO_CMD_SET_VOLUME,
        .value = vol,
    };
    audio_service_post_cmd(&cmd);
}

/* lv_timer: drain button volume queue and update slider */
static void vol_timer_cb(lv_timer_t *t)
{
    QueueHandle_t q = button_service_get_vol_queue();
    btn_vol_msg_t msg;
    while (q && xQueueReceive(q, &msg, 0) == pdPASS) {
        if (s_vol_slider) {
            lv_slider_set_value(s_vol_slider, msg.volume, true);
        }
        audio_cmd_t cmd = {
            .id = AUDIO_CMD_SET_VOLUME,
            .value = msg.volume,
        };
        audio_service_post_cmd(&cmd);
    }
}

void app_disp_lvgl_show_settings(lv_obj_t *screen, lv_group_t *group)
{
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 14, 0);
    lv_obj_set_style_pad_top(screen, 20, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_SETTINGS"  SYSTEM");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    /* Info card */
    lv_obj_t *info = lv_obj_create(screen);
    lv_obj_set_size(info, BSP_LCD_H_RES - 40, 180);
    lv_obj_set_style_bg_color(info, COL_CARD, 0);
    lv_obj_set_style_border_color(info, COL_BORDER, 0);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_radius(info, 16, 0);
    lv_obj_set_style_pad_all(info, 16, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 8, 0);

    char buf[96];
    lv_obj_t *l;

    l = lv_label_create(info);
    lv_label_set_text_static(l, "ESP32-S31-Korvo-1 V1.1");
    lv_obj_set_style_text_color(l, COL_TEXT, 0);

    snprintf(buf, sizeof(buf), "IDF %s", esp_get_idf_version());
    l = lv_label_create(info);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

    l = lv_label_create(info);
    lv_label_set_text_static(l, "Wi-Fi:   not connected");
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

    l = lv_label_create(info);
    lv_label_set_text_static(l, "UDP 50000/50001  |  TCP 50002");
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

    l = lv_label_create(info);
    lv_label_set_text_static(l, "GitHub: xkk6663");
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);

    /* Volume card */
    lv_obj_t *vol = lv_obj_create(screen);
    lv_obj_set_size(vol, BSP_LCD_H_RES - 40, 90);
    lv_obj_set_style_bg_color(vol, COL_CARD, 0);
    lv_obj_set_style_border_color(vol, COL_BORDER, 0);
    lv_obj_set_style_border_width(vol, 1, 0);
    lv_obj_set_style_radius(vol, 16, 0);
    lv_obj_set_style_pad_left(vol, 16, 0);
    lv_obj_set_style_pad_right(vol, 16, 0);
    lv_obj_set_flex_flow(vol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    l = lv_label_create(vol);
    lv_label_set_text_static(l, LV_SYMBOL_VOLUME_MAX"  VOL");
    lv_obj_set_style_text_color(l, COL_TEXT, 0);

    s_vol_slider = lv_slider_create(vol);
    lv_obj_set_width(s_vol_slider, BSP_LCD_H_RES - 220);
    lv_slider_set_range(s_vol_slider, 0, 90);
    lv_slider_set_value(s_vol_slider, button_service_get_volume(), false);
    lv_obj_set_style_bg_color(s_vol_slider, COL_BORDER, 0);
    lv_obj_set_style_bg_color(s_vol_slider, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_vol_slider, volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (group) {
        lv_group_add_obj(group, s_vol_slider);
    }

    /* Poll button volume queue every 100ms */
    lv_timer_create(vol_timer_cb, 100, NULL);
}
