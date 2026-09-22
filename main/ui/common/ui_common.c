/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui_common.c
 * @brief Reusable widget factory implementation.
 */

#include "bsp/esp-bsp.h"
#include "ui_common.h"
#include "ui_theme.h"
#include "ui/ui_manager/page_manager.h"

#define TITLE_BAR_H   48
#define ROW_H         56

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    PageManager_Back();
}

lv_obj_t *ui_common_page_root(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_style_bg_color(page, COL_BG, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    return page;
}

lv_obj_t *ui_common_title_bar(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, BSP_LCD_H_RES - 40, TITLE_BAR_H);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_radius(bar, 12, 0);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_left(bar, 12, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);

    lv_obj_t *back = lv_btn_create(bar);
    lv_obj_set_size(back, 36, 32);
    lv_obj_set_style_bg_color(back, COL_BORDER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text_static(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_center(bl);

    lv_obj_t *txt = lv_label_create(bar);
    lv_label_set_text(txt, title);
    lv_obj_set_style_text_color(txt, COL_CYAN, 0);
    lv_obj_set_style_text_font(txt, UI_FONT, 0);

    return bar;
}

lv_obj_t *ui_common_setting_row(lv_obj_t *parent, const char *icon,
                                const char *text, lv_event_cb_t click_cb,
                                void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, ROW_H);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(row, true);
    if (click_cb) {
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *left = lv_label_create(row);
    lv_label_set_text_fmt(left, "%s  %s", icon ? icon : "", text);
    lv_obj_set_style_text_color(left, COL_TEXT, 0);
    lv_obj_set_style_text_font(left, UI_FONT, 0);

    lv_obj_t *right = lv_label_create(row);
    lv_label_set_text_static(right, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(right, COL_SUBTEXT, 0);

    return row;
}

lv_obj_t *ui_common_placeholder(lv_obj_t *parent, const char *text)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, 200);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_center(card);

    lv_obj_t *lab = lv_label_create(card);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(lab, UI_FONT, 0);
    lv_obj_center(lab);
    return card;
}

/* ------------------------------------------------------------------
 * 列表控件：替代已弃用的 lv_list
 *
 * LVGL v9 把整个 lv_list 控件族标为 deprecated（见
 * managed_components/lvgl__lvgl/include/lvgl/widgets/lv_list.h:24-31），
 * 官方建议直接用 flex 纵向容器自己搭。下面两个函数就是按
 * src/widgets/list/lv_list.c 的实现逐行复刻的等价物，视觉与行为不变。
 * ------------------------------------------------------------------ */

lv_obj_t *ui_common_list(lv_obj_t *parent)
{
    /* 对应 lv_list_create()：lv_list_class.base_class == lv_obj_class，
       唯二差别是默认宽高（调用方都会 set_size 覆盖）与 flex 纵向流。
       ★ 2026-09-22：原实现没套样式，lv_obj_create 沿用 LVGL 默认主题的
       浅色外观，在深蓝页面上就是一块刺眼的白底。此处补齐主题化。 */
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(list, COL_CARD, 0);
    lv_obj_set_style_border_color(list, COL_BORDER, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_shadow_width(list, 0, 0);  /* 扁平化：默认主题阴影在深蓝底上显脏 */
    lv_obj_set_style_radius(list, 16, 0);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    /* 滚动条也用主题色，否则默认是灰色滑块 */
    lv_obj_set_style_bg_color(list, COL_CYAN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    return list;
}

lv_obj_t *ui_common_list_add_button(lv_obj_t *list, const void *symbol,
                                    const char *text)
{
    /* 对应 lv_list_add_button()（lv_list.c:90-115）：
       lv_list_button_class.base_class == lv_button_class，
       默认 width=100% / height=CONTENT，内部 flex 横向排列。 */
    lv_obj_t *btn = lv_btn_create(list);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 行样式：深色卡片 + 细描边；按下时青色高亮，做出"可点"反馈 */
    lv_obj_set_style_bg_color(btn, COL_CARD_HI, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_pad_column(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, COL_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, COL_CYAN, LV_STATE_PRESSED);

    /* ★ 这里仍用 image 控件承载图标，与原实现一致。
       LV_SYMBOL_* 是纯文本符号（首字节 >= 0x80），LVGL 的
       lv_image_src_get_type() 会据此判定为 LV_IMAGE_SRC_SYMBOL 并渲染成字形
       （见 src/draw/lv_draw_image.c:219），所以符号能正常显示。
       再把字形本身染成青色 —— 这是"AI / 物联网"观感的关键一笔。 */
    if (symbol) {
        lv_obj_t *icon = lv_img_create(btn);
        lv_image_set_src(icon, symbol);
        lv_obj_set_style_image_recolor(icon, COL_CYAN, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    if (text) {
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, text);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(lab, 1);
        lv_obj_set_style_text_color(lab, COL_TEXT, 0);
        lv_obj_set_style_text_font(lab, UI_FONT, 0);
    }
    return btn;
}

/* ------------------------------------------------------------------
 * 卡片 / 按钮工厂：统一 tab 页的视觉语言（对齐 tab_call.c 的既有风格）
 * ------------------------------------------------------------------ */

lv_obj_t *ui_common_card(lv_obj_t *parent, int w, int h, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

lv_obj_t *ui_common_accent_button(lv_obj_t *parent, int w, int h,
                                  const char *text)
{
    /* 主按钮：青色实底 + 深色文字，视线焦点 */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, COL_CYAN, 0);
    lv_obj_set_style_bg_color(btn, COL_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, COL_DARK, 0);
    lv_obj_set_style_text_font(lab, UI_FONT, 0);
    lv_obj_center(lab);
    return btn;
}

lv_obj_t *ui_common_ghost_button(lv_obj_t *parent, int w, int h,
                                 const char *text)
{
    /* 次按钮：透明底 + 青色描边 + 青色文字 */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, COL_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, COL_CYAN, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, COL_CYAN, 0);
    lv_obj_set_style_text_font(lab, UI_FONT, 0);
    lv_obj_center(lab);
    return btn;
}
