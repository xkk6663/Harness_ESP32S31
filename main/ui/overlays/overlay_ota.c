/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* =============================================================================
 * overlay_ota.c —— M3：OTA 升级浮层（居中卡片 + 进度条 + "请勿断电"）
 *
 * 方案：docs/项目规划/WiFi局域网OTA升级实施方案.md §5（M3）
 *
 * ── 线程模型（本文件的核心，改前必读）─────────────────────────────
 *  `ota_service` 的 evt_cb 在 **ota_task 上下文**（PSRAM 栈）。规范 §4：
 *  非 LVGL 线程碰 LVGL = 未定义行为（本工程历史上"按下一首 PC 闪退"就是
 *  Qt 侧同款问题，设备侧同理）。所以：
 *
 *      evt_cb（ota_task）  ──►  只写 s_pending_* （volatile）
 *      lv_timer（taskLVGL）──►  读标志 → 更新 label / bar / 显隐
 *
 *  ★ 回调里**唯一**允许做的事就是给 volatile 赋值 + 拷一个 ≤64B 的字符串。
 *    绝不允许 lv_*、绝不 malloc、绝不阻塞。
 *
 * ── 内存纪律（docs/工程规范/13_内存管理规范.md）───────────────────
 *  - 本文件**零堆分配**：所有控件由 LVGL 内部管理，字符串走栈上 buf；
 *    事件 detail 用固定 64B 静态缓冲（S1 静态池优先）。
 *  - 卡片常驻但默认隐藏：隐藏用 `lv_obj_set_hidden()`，**不删不建**
 *    （避免升级过程中反复建/删控件在紧张内部 RAM 上引入碎片）。
 *  - 无 assert：运行期任何失败只打日志降级（A2/A3）。
 *  - LVGL v9 API：flag 一律走 `lv_obj_set_<flag>()` 专用 setter
 *    （`lv_obj_add_flag/remove_flag` 已 deprecated，见 include/lvgl/core/lv_obj.h:238）。
 * ========================================================================== */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"   /* BSP_LCD_H_RES / BSP_LCD_V_RES */
#include "lvgl.h"

#include "overlay_ota.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_binfont.h"
#include "middleware/network/ota_service.h"

static const char *TAG = "OTA_UI";

/* ---- 卡片尺寸（全屏 800x480 遮罩上的居中盒子）---- */
#define OTA_BOX_W         560
#define OTA_BOX_H         300
#define OTA_TICK_MS       300      /* 刷新周期：进度条够顺滑，又不抢 LVGL 时间片 */
#define OTA_TERM_TICKS    12       /* 终态展示时长 = 12 × 300ms ≈ 3.6s */

/* ---- 跨线程传送带（只允许 evt_cb 写、lv_timer 读）---- */
#define OTA_EVT_NONE ((ota_evt_t)-1)
static volatile ota_evt_t s_pending_evt = OTA_EVT_NONE;
static volatile uint8_t   s_pending_pct = 0;
static volatile bool      s_pending_valid = false;
static char               s_pending_detail[64];   /* 单写者：evt_cb */

/* ---- 当前 UI 状态（只在 taskLVGL 上下文读写）---- */
static ota_evt_t s_shown_evt = OTA_EVT_NONE;
static bool      s_terminal;      /* 已进入终态（失败/中止），等待收摊 */
static uint8_t   s_term_ticks;

/* ---- LVGL 对象 ---- */
static lv_obj_t *s_mask;      /* 全屏遮罩（拦触摸） */
static lv_obj_t *s_box;       /* 居中卡片 */
static lv_obj_t *s_from_to;   /* 版本行 "vA -> vB"（ASCII） */
static lv_obj_t *s_status;    /* 状态文本（CJK） */
static lv_obj_t *s_bar;       /* 进度条 */
static lv_obj_t *s_warn;      /* 警示语（CJK，红） */
static lv_timer_t *s_timer;
static bool s_visible;

/* 上一版/新版的显示缓冲（taskLVGL 上下文，不进堆） */
static char s_ver_from[32] = "";
static char s_ver_to[32]   = "";

/* =====================================================================
 * 事件回调（★ ota_task 上下文 —— 只准写 volatile）
 * ===================================================================== */

static void ota_ui_evt_cb(const ota_event_t *ev)
{
    if (!ev) {
        return;
    }
    /* ★ 严格顺序：先写 detail / pct（单写者），再置 valid，最后写 evt（发布点）。
       lv_timer 侧读到 valid 后先拷 detail 再用 —— 不会有半截字符串。 */
    s_pending_pct = ev->percent;
    if (ev->detail[0]) {
        snprintf(s_pending_detail, sizeof(s_pending_detail), "%s", ev->detail);
    } else {
        s_pending_detail[0] = '\0';
    }
    s_pending_valid = true;
    s_pending_evt = ev->event;
    /* ★★ 到此为止 —— 下面一行都不能有 lv_* */
}

/* =====================================================================
 * UI 构建
 * ===================================================================== */

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t color, const char *text)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_color(lb, color, 0);
    lv_obj_set_style_text_font(lb, font, 0);
    return lb;
}

/* CJK 用 bin 字体；未就绪（NULL）必须回退内置字体，否则中文变方块。 */
static const lv_font_t *cjk_font(void)
{
    lv_font_t *f = ui_binfont_get();
    return f ? f : UI_FONT;
}

static void ota_card_build(void)
{
    /* 全屏遮罩：拦下层触摸（升级中不该让用户点到别的 tab）。 */
    s_mask = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_mask, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_mask, COL_DARK, 0);
    lv_obj_set_style_bg_opa(s_mask, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_mask, 0, 0);
    lv_obj_set_style_radius(s_mask, 0, 0);
    lv_obj_set_style_pad_all(s_mask, 0, 0);
    lv_obj_set_scrollable(s_mask, false);
    lv_obj_set_clickable(s_mask, true);

    /* 居中卡片 */
    s_box = lv_obj_create(s_mask);
    lv_obj_set_size(s_box, OTA_BOX_W, OTA_BOX_H);
    lv_obj_center(s_box);
    lv_obj_set_style_bg_color(s_box, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_box, COL_CYAN, 0);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_radius(s_box, 18, 0);
    lv_obj_set_style_pad_all(s_box, 20, 0);
    lv_obj_set_style_shadow_width(s_box, 0, 0);
    lv_obj_set_scrollable(s_box, false);

    /* 标题（CJK）*/
    lv_obj_t *title = mk_label(s_box, cjk_font(), COL_CYAN, "固件升级");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* 版本 from → to（ASCII，用 UI_FONT —— 避免 bin 字体撑高行）*/
    s_from_to = mk_label(s_box, UI_FONT, COL_TEXT, "..  ->  ..");
    lv_obj_align(s_from_to, LV_ALIGN_TOP_MID, 0, 46);

    /* 进度条 */
    s_bar = lv_bar_create(s_box);
    lv_obj_set_size(s_bar, OTA_BOX_W - 80, 22);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 92);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 11, LV_PART_INDICATOR);

    /* 状态文本（CJK）：下载中 42% / 校验中 / 即将重启 … */
    s_status = mk_label(s_box, cjk_font(), COL_SUBTEXT, "准备中…");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 132);

    /* 警示语（CJK，红）：下载中闪烁 */
    s_warn = mk_label(s_box, cjk_font(), COL_RED, "请勿断电、勿断网");
    lv_obj_align(s_warn, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_set_hidden(s_mask, true);
    s_visible = false;

    ESP_LOGI(TAG, "[OTA_UI] card built (mask %dx%d, box %dx%d), hidden",
             (int)BSP_LCD_H_RES, (int)BSP_LCD_V_RES, OTA_BOX_W, OTA_BOX_H);
}

/* =====================================================================
 * 显隐
 * ===================================================================== */

static void ota_set_visible(bool vis)
{
    if (!s_mask || s_visible == vis) {
        return;
    }
    lv_obj_set_hidden(s_mask, !vis);
    if (vis) {
        lv_obj_move_foreground(s_mask);
    }
    s_visible = vis;
    ESP_LOGI(TAG, "[OTA_UI] overlay %s", vis ? "SHOWN" : "HIDDEN");
}

/* =====================================================================
 * 定时刷新（★ taskLVGL 上下文 —— 这里才允许碰 LVGL）
 * ===================================================================== */

static void ota_ui_apply_evt(ota_evt_t evt, uint8_t pct)
{
    char pbuf[48];

    switch (evt) {
    case OTA_EVT_OFFERED:
        lv_label_set_text(s_status, "已接受请求，开始下载…");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        ota_set_visible(true);
        break;

    case OTA_EVT_DOWNLOADING:
        snprintf(pbuf, sizeof(pbuf), "下载中 %u%%", (unsigned)pct);
        lv_label_set_text(s_status, pbuf);
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        ota_set_visible(true);
        break;

    case OTA_EVT_VERIFYING:
        lv_label_set_text(s_status, "校验固件完整性…");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        ota_set_visible(true);
        break;

    case OTA_EVT_DONE:
        lv_label_set_text(s_status, "升级成功，正在重启…");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_warn, "即将重启，请勿操作");
        ota_set_visible(true);
        break;

    case OTA_EVT_FAILED:
        lv_label_set_text(s_status, "升级失败，断点已保留");
        lv_label_set_text(s_warn, "可重试续传");
        ota_set_visible(true);
        s_terminal = true;
        s_term_ticks = 0;
        break;

    case OTA_EVT_ABORTED:
        lv_label_set_text(s_status, "升级已取消");
        lv_label_set_text(s_warn, "设备未改动");
        ota_set_visible(true);
        s_terminal = true;
        s_term_ticks = 0;
        break;

    case OTA_EVT_REJECTED:
    case OTA_EVT_ROLLBACK_ARMED:
    default:
        /* 未真正开始升级 ⇒ 不打扰用户（只留日志）。 */
        ESP_LOGI(TAG, "[OTA_UI] evt=%d not shown on card", (int)evt);
        break;
    }
}

static void ota_ui_tick(lv_timer_t *t)
{
    (void)t;

    /* ---- 1) 版本行：OTA 活跃或浮层可见时刷新 "from -> to" ---- */
    if (s_from_to && (ota_service_is_active() || s_visible)) {
        char vf[32], vt[32], line[80];
        ota_service_get_running_version(vf, sizeof(vf));
        ota_service_get_offer_version(vt, sizeof(vt));
        if (strcmp(vf, s_ver_from) != 0 || strcmp(vt, s_ver_to) != 0) {
            snprintf(s_ver_from, sizeof(s_ver_from), "%s", vf);
            snprintf(s_ver_to, sizeof(s_ver_to), "%s", vt);
            snprintf(line, sizeof(line), "%.16s  ->  %.16s", vf, vt);
            lv_label_set_text(s_from_to, line);
        }
    }

    /* ---- 2) 消费跨线程事件 ---- */
    if (s_pending_valid) {
        ota_evt_t evt = s_pending_evt;
        uint8_t pct = s_pending_pct;
        char detail[64];
        memcpy(detail, s_pending_detail, sizeof(detail));
        s_pending_valid = false;      /* 先清标志再处理（丢一帧也无所谓） */
        s_shown_evt = evt;
        ESP_LOGI(TAG, "[OTA_UI] evt=%d pct=%u detail='%s'", (int)evt, (unsigned)pct, detail);
        ota_ui_apply_evt(evt, pct);
    }

    /* ---- 3) 终态收摊：展示 ~3.6s 后自动隐藏并复位 ---- */
    if (s_terminal && s_visible) {
        if (++s_term_ticks >= OTA_TERM_TICKS) {
            s_terminal = false;
            s_term_ticks = 0;
            s_shown_evt = OTA_EVT_NONE;
            lv_label_set_text(s_warn, "请勿断电、勿断网");
            lv_obj_set_style_opa(s_warn, LV_OPA_COVER, 0);
            ota_set_visible(false);
        }
        return;
    }

    /* ---- 4) 自证窗口提示（非升级中，仅在浮层已可见时改文案）---- */
    if (s_visible && ota_service_is_pending_verify() && s_shown_evt == OTA_EVT_NONE) {
        lv_label_set_text(s_status, "升级后自证中，请稍候…");
    }

    /* ---- 5) 下载中：警示语闪烁（300ms 相位）---- */
    if (s_visible && s_shown_evt == OTA_EVT_DOWNLOADING) {
        static uint8_t phase;
        phase ^= 1;
        lv_obj_set_style_opa(s_warn, phase ? LV_OPA_COVER : LV_OPA_40, 0);
    } else if (s_warn) {
        lv_obj_set_style_opa(s_warn, LV_OPA_COVER, 0);
    }
}

/* =====================================================================
 * 公开 API
 * ===================================================================== */

void overlay_ota_init(void)
{
    if (s_mask) {
        return;   /* 幂等 */
    }
    ota_card_build();
    ota_service_set_evt_cb(ota_ui_evt_cb);
    if (!s_timer) {
        s_timer = lv_timer_create(ota_ui_tick, OTA_TICK_MS, NULL);
    }
    ESP_LOGI(TAG, "[OTA_UI] init done (evt_cb subscribed, tick=%dms)", OTA_TICK_MS);
}

void overlay_ota_deinit(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    ota_service_set_evt_cb(NULL);
    if (s_mask) {
        lv_obj_del(s_mask);
        s_mask = NULL;
    }
    s_box = NULL;
    s_from_to = NULL;
    s_status = NULL;
    s_bar = NULL;
    s_warn = NULL;
    s_visible = false;
    s_shown_evt = OTA_EVT_NONE;
    s_terminal = false;
    s_term_ticks = 0;
    s_pending_valid = false;
    ESP_LOGI(TAG, "[OTA_UI] deinit done");
}

void overlay_ota_show(void)
{
    ota_set_visible(true);
}

void overlay_ota_hide(void)
{
    ota_set_visible(false);
}

bool overlay_ota_is_visible(void)
{
    return s_visible;
}
