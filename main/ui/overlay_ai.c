/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file overlay_ai.c
 * @brief AI 浮窗实现（S6，自 tab_ai.c 平移改造）。
 *
 * 结构：lv_layer_top() 上全屏容器（pad 12 内容区 776×456），常驻构建，
 *       show/hide = y 轴滑入/滑出（overlay_wallpaper 同款动画骨架）。
 * 布局：标题(+X 收窗钮)/状态行 → 滚动对话记录（bin 字体）→ 激活码提示行
 *       → 底部按钮行（TALK/END + HOLD TALK + CLEAR）。
 *
 * 线程纪律（F44 同族）：
 *   - xiaozhi_service 回调（协议任务）：只拷贝进事件队列；
 *   - button_service 回调（iot_button 任务）：只置 volatile 标志 + 调
 *     xiaozhi_service_*（只置期望态，无 LVGL）；
 *   - LVGL 全部落笔在 50ms lv_timer（状态/按钮/请求排空/PTT pending）。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "overlay_ai.h"
#include "overlay_manager.h"
#include "ui_theme.h"
#include "ui_binfont.h"
#include "middleware/ai/xiaozhi_service.h"
#include "middleware/ai/ai_camera.h"
#include "middleware/periph/button_service.h"

LV_FONT_DECLARE(lv_font_cn_20);   /* 中文正文（bin 字体，桌面挂件同款） */

static const char *TAG = "UI_AI";

/* 内容区：全屏容器 pad 12（层在 lv_layer_top，无 tabview 页 pad/F46 约束） */
#define AI_W   (BSP_LCD_H_RES - 24)
#define AI_H   (BSP_LCD_V_RES - 24)
#define AI_SLIDE_MS   300

#define AI_EVT_Q_DEPTH   12
#define AI_TEXT_MAX      128
#define AI_LOG_MAX       4096

/* ---- 事件桥（服务任务 → LVGL 线程）---- */
typedef struct {
    xiaozhi_evt_id_t id;
    int  role;
    char text[AI_TEXT_MAX];
} ai_evt_msg_t;

static lv_obj_t   *s_win;            /* 全屏浮窗容器（lv_layer_top 子） */
static lv_obj_t   *s_btn;
static lv_obj_t   *s_btn_label;
static lv_obj_t   *s_ptt_btn;        /* 按住说话（manual 模式 PTT） */
static lv_obj_t   *s_ptt_label;
static lv_obj_t   *s_status;
static lv_obj_t   *s_act_label;      /* 激活码提示行（默认隐藏） */
static lv_obj_t   *s_emo_badge;      /* §58 A2 情绪徽章（彩色胶囊，默认隐藏） */
static lv_obj_t   *s_emo_label;
static lv_obj_t   *s_log_cont;       /* 滚动容器 */
static lv_obj_t   *s_log;            /* 对话文本 label */
static lv_timer_t *s_timer;
static QueueHandle_t s_evt_q;
static char         s_log_buf[AI_LOG_MAX];
static bool         s_prev_chatting;

/* ---- §59b 拍照预览卡（MCP 摄像头工具触发，3s 自动消失） ---- */
static lv_obj_t      *s_pv_box;      /* 右上角浮层卡片 */
static lv_obj_t      *s_pv_img;      /* 缩略图 */
static lv_timer_t    *s_pv_timer;    /* 3s one-shot（repeat_count=1 到期自删） */
static uint8_t       *s_pv_show;     /* 显示中帧（UI 持有，隐藏时 free） */
static lv_image_dsc_t s_pv_dsc;

/* ---- 可见性与按键请求（按键回调只写这些标志） ---- */
static volatile bool s_visible;        /* 浮窗在屏上（show/hide 维护） */
static volatile bool s_set_req;        /* SET 单击：timer 排空执行状态机 */
static volatile bool s_open_req;       /* MODE 按下且窗关：只开窗不 start */
static volatile bool s_hw_ptt_pending; /* MODE 按住但会话未就绪，CHATTING 后补 */
static volatile bool s_hw_ptt_ui;      /* MODE 驱动的 PTT 生效态 */
static volatile bool s_touch_ptt_ui;   /* 屏幕按钮驱动的 PTT 生效态 */
static bool          s_prev_ptt_vis;   /* PTT 视觉态去重 */

/* ============================================================
 * 服务回调（xiaozhi 服务任务上下文）：只拷贝入队
 * ============================================================ */

static void on_xz_evt(const xiaozhi_evt_t *evt)
{
    if (s_evt_q == NULL) {
        return;
    }
    ai_evt_msg_t msg = { .id = evt->id, .role = evt->role };
    if (evt->text) {
        snprintf(msg.text, sizeof(msg.text), "%s", evt->text);
    }
    xQueueSend(s_evt_q, &msg, 0);     /* 满了就丢（UI 连点保护） */
}

/* ============================================================
 * 情绪映射（§58 A2）：bin 字体无 emoji 字形 ⇒ 视觉化 = 主题色胶囊
 * + ASCII 颜文字（LLM 情绪标签 → 颜色/脸）
 * ============================================================ */

typedef struct {
    const char *name;
    uint32_t    color;     /* 徽章底色（RGB888） */
    const char *face;      /* ASCII 颜文字 */
} emo_map_t;

static const emo_map_t s_emo_map[] = {
    { "neutral",     0x94A3B8, ":-|" },
    { "happy",       0x22C55E, ":-D" },
    { "laughing",    0xFACC15, "xD"  },
    { "funny",       0xFACC15, "xD"  },
    { "delighted",   0x4ADE80, "XD"  },
    { "sad",         0x3B82F6, ":-(" },
    { "crying",      0x38BDF8, "T_T" },
    { "angry",       0xEF4444, ">:-(" },
    { "loving",      0xEC4899, "<3"  },
    { "embarrassed", 0xF97316, ":$"  },
    { "surprised",   0xFB923C, ":-O" },
    { "shocked",     0xF87171, "O_O" },
    { "thinking",    0xA78BFA, ":-?" },
    { "winking",     0x2DD4BF, ";-)" },
    { "cool",        0x67E8F9, "8-)" },
    { "sleepy",      0x64748B, "zzZ" },
    { "zany",        0xE879F9, "%-)" },
};

#define EMO_MAP_N (sizeof(s_emo_map) / sizeof(s_emo_map[0]))

static const emo_map_t *emo_lookup(const char *name)
{
    for (unsigned i = 0; i < EMO_MAP_N; i++) {
        if (name && strcmp(s_emo_map[i].name, name) == 0) {
            return &s_emo_map[i];
        }
    }
    return &s_emo_map[0];   /* 未知情绪 → neutral 灰 */
}

/* ============================================================
 * 对话记录
 * ============================================================ */

static void log_append(const char *who, const char *text)
{
    /* 超限：淘汰前 1/3（跳过其后第一个换行），保住最近对话 */
    size_t used = strlen(s_log_buf);
    size_t need = used + strlen(who) + strlen(text) + 4;
    if (need >= sizeof(s_log_buf)) {
        const char *cut = strchr(s_log_buf + sizeof(s_log_buf) / 3, '\n');
        if (cut) {
            memmove(s_log_buf, cut + 1, strlen(cut + 1) + 1);
        } else {
            s_log_buf[0] = '\0';
        }
    }
    int n = (int)strlen(s_log_buf);
    snprintf(s_log_buf + n, sizeof(s_log_buf) - n, "%s%s\n", who, text);
    lv_label_set_text(s_log, s_log_buf);
    lv_obj_scroll_to_y(s_log_cont, LV_COORD_MAX, LV_ANIM_OFF);
}

static void evt_drain(void)
{
    ai_evt_msg_t m;
    while (s_evt_q && xQueueReceive(s_evt_q, &m, 0) == pdPASS) {
        switch (m.id) {
        case XZ_EVT_TEXT:
            log_append(m.role == 0 ? "You: " : "AI:  ", m.text);
            break;
        case XZ_EVT_ACT_CODE:
            lv_label_set_text_fmt(s_act_label,
                "ACT CODE: %s  (enter at xiaozhi.me)", m.text);
            lv_obj_set_hidden(s_act_label, false);
            log_append("** not activated, code: ", m.text);
            break;
        case XZ_EVT_ERROR:
            if (m.text[0]) {
                log_append("ERR: ", m.text);
            }
            break;
        case XZ_EVT_EMOJI: {
            /* §58 A2：LLM 情绪 → 徽章（彩色胶囊+颜文字）+ 对话记录行。
             * line 要装下 face(≤5) + " (" + text(≤127) + ")" ⇒ 160 起步 */
            if (m.text[0]) {
                const emo_map_t *e = emo_lookup(m.text);
                char line[160];
                snprintf(line, sizeof(line), "%s (%s)", e->face, m.text);
                if (s_emo_badge) {
                    lv_obj_set_style_bg_color(s_emo_badge, lv_color_hex(e->color), 0);
                    lv_obj_set_hidden(s_emo_badge, false);
                }
                if (s_emo_label) {
                    lv_label_set_text(s_emo_label, line);
                }
                log_append("EMOJI: ", line);
            }
            break;
        }
        default:
            break;    /* STATE/TTS_STATE 由轮询与状态行表达，不进记录 */
        }
    }
}

/* ============================================================
 * §59b 拍照预览卡（ai_camera 单槽 → 50ms 轮询取走 → 3s 后消失）
 * ============================================================ */

static void preview_hide_now(void)
{
    lv_obj_set_hidden(s_pv_box, true);
    if (s_pv_show) {
        heap_caps_free(s_pv_show);   /* 所有权在 UI（take 时移交） */
        s_pv_show = NULL;
    }
}

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_pv_timer = NULL;               /* repeat_count=1 到期自删 */
    preview_hide_now();
}

static void preview_poll(void)
{
    uint8_t *buf = NULL;
    int w = 0, h = 0;
    if (!ai_camera_preview_take(&buf, &w, &h)) {
        return;
    }
    if (s_pv_show) {
        heap_caps_free(s_pv_show);   /* 新帧顶掉显示中的旧帧 */
    }
    s_pv_show = buf;
    lv_memset(&s_pv_dsc, 0, sizeof(s_pv_dsc));
    s_pv_dsc.header.w = (uint32_t)w;
    s_pv_dsc.header.h = (uint32_t)h;
    s_pv_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_pv_dsc.data = buf;
    s_pv_dsc.data_size = (uint32_t)w * (uint32_t)h * 2u;
    lv_image_set_src(s_pv_img, &s_pv_dsc);
    lv_obj_set_hidden(s_pv_box, false);
    lv_obj_move_foreground(s_pv_box);   /* 兜底：确保浮在日志容器之上 */
    if (s_pv_timer) {
        lv_timer_del(s_pv_timer);
    }
    s_pv_timer = lv_timer_create(preview_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_pv_timer, 1);
    ESP_LOGI(TAG, "camera preview shown %dx%d", w, h);
}

/* ============================================================
 * PTT 视觉反馈（屏幕按钮与 MODE 键合一，OR 语义）
 * ============================================================ */

static void ptt_ui_apply(bool on)
{
    if (on) {
        lv_label_set_text_static(s_ptt_label, LV_SYMBOL_AUDIO "  SPEAKING...");
        lv_obj_set_style_bg_color(s_ptt_btn, COL_RED, 0);
    } else {
        lv_label_set_text_static(s_ptt_label, LV_SYMBOL_AUDIO "  HOLD TALK");
        lv_obj_set_style_bg_color(s_ptt_btn, COL_CYAN, 0);
    }
}

/* ============================================================
 * 按键请求排空（timer/LVGL 上下文）
 * ============================================================ */

/* SET 单击状态机（S6）：窗关=弹出+唤醒；CHATTING=END；IDLE(已结束)=收窗 */
static void set_click_logic(void)
{
    if (!s_visible) {
        overlay_open(OVERLAY_AI);    /* → overlay_ai_show()（滑入动画） */
        if (xiaozhi_service_get_state() == XZ_STATE_IDLE) {
            esp_err_t err = xiaozhi_service_start();
            if (err != ESP_OK) {
                lv_label_set_text_static(s_status,
                    (err == ESP_ERR_INVALID_STATE) ? "Audio busy (MP3/REC?)" : "Start failed");
                lv_obj_set_style_text_color(s_status, COL_RED, 0);
                ESP_LOGW(TAG, "set: start failed: %s", esp_err_to_name(err));
            }
        }
        return;
    }
    xiaozhi_state_t st = xiaozhi_service_get_state();
    if (st == XZ_STATE_CHATTING || st == XZ_STATE_PREPARING) {
        xiaozhi_service_stop();
        lv_label_set_text_static(s_status, "Saving...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    } else if (st == XZ_STATE_IDLE) {
        overlay_close(OVERLAY_AI);   /* → overlay_ai_hide()（滑出动画） */
    }
    /* RELEASING：忽略（teardown 进行中，下一击再议） */
}

static void key_req_drain(void)
{
    if (s_set_req) {
        s_set_req = false;
        set_click_logic();
    }
    if (s_open_req) {
        s_open_req = false;
        if (!s_visible) {
            overlay_open(OVERLAY_AI);
        }
    }
    if (s_hw_ptt_pending && xiaozhi_service_get_state() == XZ_STATE_CHATTING) {
        s_hw_ptt_pending = false;
        s_hw_ptt_ui = (xiaozhi_service_ptt_start() == ESP_OK);
        ESP_LOGI(TAG, "hw ptt resumed after session ready: %d", (int)s_hw_ptt_ui);
    }
}

/* ============================================================
 * 硬件按键入口（iot_button 任务上下文）：无 LVGL，只标志 + 服务 API
 * ============================================================ */

void overlay_ai_key_feed(btn_id_t id, btn_evt_t evt)
{
    if (id == BTN_ID_SET) {
        if (evt == BTN_EVT_SINGLE_CLICK) {
            s_set_req = true;
        }
        return;    /* DOUBLE_CLICK / PRESS_END 忽略 */
    }
    if (id == BTN_ID_MODE) {
        if (evt == BTN_EVT_PRESS_DOWN) {
            xiaozhi_state_t st = xiaozhi_service_get_state();
            if (st == XZ_STATE_CHATTING) {
                s_hw_ptt_ui = (xiaozhi_service_ptt_start() == ESP_OK);
            } else if (st == XZ_STATE_IDLE) {
                /* 窗关先开窗（有字幕反馈），同时建会话；PTT 进入 CHATTING 补 */
                if (!s_visible) {
                    s_open_req = true;
                }
                s_hw_ptt_pending = true;
                if (xiaozhi_service_start() != ESP_OK) {
                    s_hw_ptt_pending = false;
                    ESP_LOGW(TAG, "mode: start failed");
                }
            }
            /* PREPARING/RELEASING：忽略本次按下 */
        } else if (evt == BTN_EVT_PRESS_UP || evt == BTN_EVT_PRESS_END) {
            s_hw_ptt_pending = false;
            if (s_hw_ptt_ui) {
                xiaozhi_service_ptt_stop();
                s_hw_ptt_ui = false;
            }
        }
    }
}

/* ============================================================
 * 轮询刷新（50ms）：状态行/按钮/请求唯一落笔点
 * ============================================================ */

static void ai_timer_cb(lv_timer_t *t)
{
    (void)t;
    evt_drain();
    key_req_drain();
    preview_poll();

    /* PTT 视觉态 = 屏幕按钮 OR 硬件 MODE 键 */
    bool ptt_vis = (s_hw_ptt_ui || s_touch_ptt_ui);
    if (ptt_vis != s_prev_ptt_vis) {
        ptt_ui_apply(ptt_vis);
        s_prev_ptt_vis = ptt_vis;
    }

    xiaozhi_state_t st = xiaozhi_service_get_state();
    bool chatting = (st == XZ_STATE_CHATTING);

    if (chatting != s_prev_chatting) {
        if (chatting) {
            lv_label_set_text_static(s_btn_label, LV_SYMBOL_STOP "  END");
            lv_obj_set_style_bg_color(s_btn, COL_RED, 0);
        } else {
            lv_label_set_text_static(s_btn_label, LV_SYMBOL_AUDIO "  TALK");
            lv_obj_set_style_bg_color(s_btn, COL_GREEN, 0);
        }
        s_prev_chatting = chatting;
    }

    switch (st) {
    case XZ_STATE_IDLE:      lv_label_set_text_static(s_status, "Tap TALK to chat"); break;
    case XZ_STATE_PREPARING: lv_label_set_text_static(s_status, "Connecting...");   break;
    case XZ_STATE_CHATTING:  lv_label_set_text_static(s_status, "Hold mic to speak"); break;
    case XZ_STATE_RELEASING: lv_label_set_text_static(s_status, "Closing...");      break;
    }
}

/* ============================================================
 * 屏幕交互（与硬件键等价，驱动同一组请求函数/标志）
 * ============================================================ */

static void talk_btn_cb(lv_event_t *e)
{
    (void)e;
    xiaozhi_state_t st = xiaozhi_service_get_state();
    if (st == XZ_STATE_IDLE) {
        esp_err_t err = xiaozhi_service_start();
        if (err != ESP_OK) {
            lv_label_set_text_static(s_status,
                (err == ESP_ERR_INVALID_STATE) ? "Audio busy (MP3/REC?)" : "Start failed");
            lv_obj_set_style_text_color(s_status, COL_RED, 0);
        }
    } else if (st == XZ_STATE_PREPARING || st == XZ_STATE_CHATTING) {
        xiaozhi_service_stop();
        lv_label_set_text_static(s_status, "Saving...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    }
}

static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    s_log_buf[0] = '\0';
    lv_label_set_text(s_log, "");
    lv_obj_set_hidden(s_act_label, true);
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    /* 会话中先停（异步，服务后台收尾），再收窗 */
    xiaozhi_state_t st = xiaozhi_service_get_state();
    if (st == XZ_STATE_CHATTING || st == XZ_STATE_PREPARING) {
        xiaozhi_service_stop();
    }
    overlay_close(OVERLAY_AI);
}

/* PTT：按住 = listen start(manual)，松开/滑出 = listen stop。
 * 回调只置标志 + 调服务 API，视觉统一由 timer 应用（与 MODE 键 OR 合流）。 */
static void ptt_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (xiaozhi_service_ptt_start() != ESP_OK) {
            lv_label_set_text_static(s_status, "Tap TALK first");
            lv_obj_set_style_text_color(s_status, COL_RED, 0);
            s_touch_ptt_ui = false;
            return;
        }
        s_touch_ptt_ui = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        xiaozhi_service_ptt_stop();
        s_touch_ptt_ui = false;
    }
}

/* ============================================================
 * 动画（overlay_wallpaper 同款骨架）
 * ============================================================ */

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void ai_animate_to(int32_t target_y)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_win);
    lv_anim_set_values(&a, lv_obj_get_y(s_win), target_y);
    lv_anim_set_time(&a, AI_SLIDE_MS);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ============================================================
 * overlay_manager 钩子 / 建 / 拆
 * ============================================================ */

void overlay_ai_show(void)
{
    if (s_win) {
        ai_animate_to(0);
        s_visible = true;
    }
}

void overlay_ai_hide(void)
{
    if (s_win) {
        ai_animate_to(BSP_LCD_V_RES);
        s_visible = false;
        s_hw_ptt_pending = false;   /* 收窗不收会话；按住态一并松开防悬挂 */
        if (s_hw_ptt_ui) {
            xiaozhi_service_ptt_stop();
            s_hw_ptt_ui = false;
        }
    }
}

bool overlay_ai_is_visible(void)
{
    return s_visible;
}

void overlay_ai_init(void)
{
    lv_obj_t *top = lv_layer_top();
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    /* ★ S6b：层（layer）默认带可滚动标志，而 AI 窗隐藏时停在 y=+480，
     * 把层内容区撑到 ~960px —— 壁纸"上拉关闭"手势自己滚不动时经
     * 滚动链传导到层上，整层上滚 ⇒ AI 窗跟着被"顺带拉出来"（实测 BUG）。
     * 层必须禁滚动：浮窗定位全部由各自手势/动画自管。 */
    lv_obj_set_scrollable(top, false);

    /* 全屏浮窗容器（不透明背景 = 挡住 tab 栏与触摸穿透） */
    s_win = lv_obj_create(top);
    lv_obj_set_size(s_win, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(s_win, 0, BSP_LCD_V_RES);   /* 初始在屏幕下方 */
    lv_obj_set_scrollable(s_win, false);
    lv_obj_set_style_bg_color(s_win, COL_BG, 0);
    lv_obj_set_style_border_width(s_win, 0, 0);
    lv_obj_set_style_radius(s_win, 0, 0);
    lv_obj_set_style_pad_all(s_win, 12, 0);

    /* 标题（左上）+ X 收窗钮（右上）+ 状态（右上、X 左侧） */
    lv_obj_t *title = lv_label_create(s_win);
    lv_label_set_text_static(title, LV_SYMBOL_AUDIO "  AI");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* §58 A2 情绪徽章（标题右侧，EMOJI 事件才显示）：
     * 亮色底 + 深色字（F47 附：固定文字色设在 label 上是安全的） */
    s_emo_badge = lv_obj_create(s_win);
    lv_obj_set_size(s_emo_badge, 150, 26);
    lv_obj_align(s_emo_badge, LV_ALIGN_TOP_LEFT, 64, 2);
    lv_obj_set_style_radius(s_emo_badge, 13, 0);
    lv_obj_set_style_bg_color(s_emo_badge, COL_SUBTEXT, 0);
    lv_obj_set_style_border_width(s_emo_badge, 0, 0);
    lv_obj_set_style_pad_all(s_emo_badge, 0, 0);
    lv_obj_set_scrollable(s_emo_badge, false);
    lv_obj_set_hidden(s_emo_badge, true);
    s_emo_label = lv_label_create(s_emo_badge);
    lv_label_set_text_static(s_emo_label, ":-|");
    lv_obj_set_style_text_color(s_emo_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_emo_label, UI_FONT, 0);
    lv_obj_center(s_emo_label);

    lv_obj_t *xbtn = lv_btn_create(s_win);
    lv_obj_set_size(xbtn, 72, 36);
    lv_obj_set_pos(xbtn, AI_W - 72, -6);
    lv_obj_set_style_radius(xbtn, 10, 0);
    lv_obj_set_style_bg_color(xbtn, COL_CARD, 0);
    lv_obj_set_style_border_color(xbtn, COL_BORDER, 0);
    lv_obj_set_style_border_width(xbtn, 1, 0);
    lv_obj_add_event_cb(xbtn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xlb = lv_label_create(xbtn);
    lv_label_set_text_static(xlb, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xlb, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(xlb, UI_FONT, 0);
    lv_obj_center(xlb);

    s_status = lv_label_create(s_win);
    lv_label_set_text_static(s_status, "Tap TALK to chat");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -80, 4);
    lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    /* 对话记录：滚动容器 + 单 label（bin 字体） */
    s_log_cont = lv_obj_create(s_win);
    lv_obj_set_pos(s_log_cont, 0, 26);
    lv_obj_set_size(s_log_cont, AI_W, AI_H - 26 - 100);
    lv_obj_set_style_bg_color(s_log_cont, COL_CARD, 0);
    lv_obj_set_style_border_color(s_log_cont, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_log_cont, 1, 0);
    lv_obj_set_style_radius(s_log_cont, 8, 0);
    lv_obj_set_style_pad_all(s_log_cont, 8, 0);
    lv_obj_set_scrollbar_mode(s_log_cont, LV_SCROLLBAR_MODE_AUTO);

    s_log = lv_label_create(s_log_cont);
    lv_label_set_text_static(s_log, "");
    lv_obj_set_width(s_log, AI_W - 32);
    lv_label_set_long_mode(s_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_log, COL_TEXT, 0);
    /* ★ §50：优先全 CJK bin 字体；未就绪/加载失败回退 cn_20 */
    lv_font_t *cjk = ui_binfont_get();
    lv_obj_set_style_text_font(s_log, cjk ? cjk : &lv_font_cn_20, 0);
    lv_obj_align(s_log, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 激活码提示行（默认隐藏） */
    s_act_label = lv_label_create(s_win);
    lv_label_set_text_static(s_act_label, "");
    lv_obj_set_pos(s_act_label, 0, AI_H - 92);
    lv_obj_set_width(s_act_label, AI_W);
    lv_label_set_long_mode(s_act_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_act_label, lv_color_hex(0xFBBF24), 0);
    lv_obj_set_style_text_font(s_act_label, &lv_font_cn_20, 0);
    lv_obj_set_hidden(s_act_label, true);

    /* 底部按钮行：TALK 大按钮 + PTT + CLEAR（y=AI_H-64，高 56） */
    s_btn = lv_btn_create(s_win);
    lv_obj_set_size(s_btn, 220, 56);
    lv_obj_set_pos(s_btn, 0, AI_H - 64);
    lv_obj_set_style_radius(s_btn, 14, 0);
    lv_obj_set_style_bg_color(s_btn, COL_GREEN, 0);
    lv_obj_set_style_border_width(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_darken(COL_GREEN, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn, talk_btn_cb, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text_static(s_btn_label, LV_SYMBOL_AUDIO "  TALK");
    lv_obj_set_style_text_color(s_btn_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_label, UI_FONT, 0);
    lv_obj_center(s_btn_label);

    s_ptt_btn = lv_btn_create(s_win);
    lv_obj_set_size(s_ptt_btn, 220, 56);
    lv_obj_set_pos(s_ptt_btn, (AI_W - 220) / 2, AI_H - 64);
    lv_obj_set_style_radius(s_ptt_btn, 14, 0);
    lv_obj_set_style_bg_color(s_ptt_btn, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_ptt_btn, 0, 0);
    lv_obj_set_style_bg_color(s_ptt_btn, lv_color_darken(COL_CYAN, LV_OPA_30),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_ptt_btn, ptt_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ptt_btn, ptt_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_ptt_btn, ptt_cb, LV_EVENT_PRESS_LOST, NULL);

    s_ptt_label = lv_label_create(s_ptt_btn);
    lv_label_set_text_static(s_ptt_label, LV_SYMBOL_AUDIO "  HOLD TALK");
    lv_obj_set_style_text_color(s_ptt_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_ptt_label, UI_FONT, 0);
    lv_obj_center(s_ptt_label);

    lv_obj_t *clr = lv_btn_create(s_win);
    lv_obj_set_size(clr, 120, 56);
    lv_obj_set_pos(clr, AI_W - 120, AI_H - 64);
    lv_obj_set_style_radius(clr, 14, 0);
    lv_obj_set_style_bg_color(clr, COL_CARD, 0);
    lv_obj_set_style_border_color(clr, COL_BORDER, 0);
    lv_obj_set_style_border_width(clr, 1, 0);
    lv_obj_add_event_cb(clr, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clr_lb = lv_label_create(clr);
    lv_label_set_text_static(clr_lb, "CLEAR");
    lv_obj_set_style_text_color(clr_lb, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(clr_lb, UI_FONT, 0);
    lv_obj_center(clr_lb);

    /* §59b 拍照预览卡：★必须最后创建——LVGL 后创建者在上层，若排在
     * s_log_cont（不透明卡底、几乎全屏宽）之前会被整块盖住（实机踩过：
     * "预览没看见"）。浮层语义 + 显示时 move_foreground 双保险。 */
    s_pv_box = lv_obj_create(s_win);
    lv_obj_set_size(s_pv_box, 216, 126);
    lv_obj_align(s_pv_box, LV_ALIGN_TOP_RIGHT, -4, 40);
    lv_obj_set_style_bg_color(s_pv_box, COL_DARK, 0);
    lv_obj_set_style_border_color(s_pv_box, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_pv_box, 1, 0);
    lv_obj_set_style_radius(s_pv_box, 8, 0);
    lv_obj_set_style_pad_all(s_pv_box, 3, 0);
    lv_obj_set_scrollable(s_pv_box, false);
    lv_obj_set_hidden(s_pv_box, true);

    s_pv_img = lv_image_create(s_pv_box);
    lv_obj_set_size(s_pv_img, 208, 118);
    lv_image_set_inner_align(s_pv_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(s_pv_img);

    /* 事件队列 + 轮询 + 硬件按键注册（全仓唯一注册者，覆盖式） */
    s_evt_q = xQueueCreate(AI_EVT_Q_DEPTH, sizeof(ai_evt_msg_t));
    s_timer = lv_timer_create(ai_timer_cb, 50, NULL);
    xiaozhi_service_set_evt_cb(on_xz_evt);
    button_service_set_evt_cb(overlay_ai_key_feed);

    s_visible = false;
    s_prev_chatting = false;
    s_prev_ptt_vis = false;
    s_touch_ptt_ui = false;
    s_hw_ptt_ui = false;
    s_hw_ptt_pending = false;
    s_set_req = false;
    s_open_req = false;
    s_log_buf[0] = '\0';

    ESP_LOGI(TAG, "AI overlay ready (%dx%d, hw keys SET/MODE bound)",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
}

void overlay_ai_deinit(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_pv_timer) {
        lv_timer_del(s_pv_timer);
        s_pv_timer = NULL;
    }
    if (s_pv_show) {
        heap_caps_free(s_pv_show);
        s_pv_show = NULL;
    }
    if (s_evt_q) {
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
    }
    xiaozhi_service_set_evt_cb(NULL);
    button_service_set_evt_cb(NULL);
    if (s_win) {
        lv_obj_del(s_win);
        s_win = NULL;
    }
    s_visible = false;
    s_btn = NULL;
    s_btn_label = NULL;
    s_ptt_btn = NULL;
    s_ptt_label = NULL;
    s_status = NULL;
    s_act_label = NULL;
    s_log_cont = NULL;
    s_log = NULL;
    s_pv_box = NULL;
    s_pv_img = NULL;
}
