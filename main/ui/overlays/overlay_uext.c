/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* =============================================================================
 * overlay_uext.c —— USB 副屏切换确认浮层（2s 倒计时卡，D2）
 *
 * 方案：docs/项目规划/USB副屏集成技术方案.md V1.1 §4.2 / §4.4
 *
 * ── 线程模型（与 overlay_ota 同款，改前必读）──────────────────────
 *   overlay_uext_notify_set_click()（iot_button 任务）──► 只写 s_click_pending
 *   uext_ui_tick()（taskLVGL，lv_timer 100ms）        ──► 消费标志、碰 LVGL
 *
 *   ★ 二次 SET 确认在 taskLVGL 上下文执行 commit：
 *     esp_ota_set_boot_partition + esp_restart —— 两者都是幂等/安全 API，
 *     不依赖文件系统、不碰 PSRAM 栈约束，可在任意任务调用。
 *
 * ── 内存纪律 ──────────────────────────────────────────────────────
 *  - 零堆分配；卡片常驻默认隐藏（lv_obj_set_hidden，不删不建）。
 *  - LVGL v9：显隐用 lv_obj_set_hidden，flag 走专用 setter。
 * ========================================================================== */

#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "overlay_uext.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_binfont.h"
#include "middleware/network/ota_service.h"

static const char *TAG = "UEXT_UI";

/* ---- 倒计时参数 ---- */
#define UEXT_TICK_MS        100     /* 消费/倒计时周期 */
#define UEXT_CONFIRM_TICKS  20      /* 20 × 100ms = 2.0s 确认窗口 */

/* ---- 卡片尺寸 ---- */
#define UEXT_BOX_W  480
#define UEXT_BOX_H  220

/* ---- 跨线程传送带（按钮任务写、lv_timer 读）---- */
static volatile bool s_inited = false;
static volatile bool s_click_pending = false;

/* ---- UI 状态（仅 taskLVGL 上下文）---- */
static lv_obj_t   *s_mask;
static lv_obj_t   *s_bar;
static lv_obj_t   *s_status;
static lv_timer_t *s_timer;
static bool        s_visible;
static int         s_remain_ticks;

/* =====================================================================
 * UI 构建（taskLVGL，init 上下文）
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

/* CJK 用 bin 字体；未就绪（NULL）回退内置字体。 */
static const lv_font_t *cjk_font(void)
{
    lv_font_t *f = ui_binfont_get();
    return f ? f : UI_FONT;
}

static void uext_card_build(void)
{
    /* 全屏遮罩：拦下层触摸 */
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
    lv_obj_t *box = lv_obj_create(s_mask);
    lv_obj_set_size(box, UEXT_BOX_W, UEXT_BOX_H);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COL_CARD, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, COL_CYAN, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_scrollable(box, false);

    /* 标题 */
    lv_obj_t *title = mk_label(box, cjk_font(), COL_CYAN, "USB 副屏模式");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* 倒计时进度条 */
    s_bar = lv_bar_create(box);
    lv_obj_set_size(s_bar, UEXT_BOX_W - 80, 18);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 52);
    lv_bar_set_range(s_bar, 0, UEXT_CONFIRM_TICKS);
    lv_bar_set_value(s_bar, UEXT_CONFIRM_TICKS, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 9, LV_PART_INDICATOR);

    /* 提示行 */
    s_status = mk_label(box, cjk_font(), COL_TEXT, "再次按 SET 进入副屏");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 88);

    /* 底部说明 */
    lv_obj_t *note = mk_label(box, cjk_font(), COL_SUBTEXT,
                              "超时自动取消；重启约 3-5 秒");
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_set_hidden(s_mask, true);
    s_visible = false;

    ESP_LOGI(TAG, "[UEXT_UI] card built (box %dx%d), hidden", UEXT_BOX_W, UEXT_BOX_H);
}

/* =====================================================================
 * 显隐 + 倒计时（★ taskLVGL 上下文）
 * ===================================================================== */

static void uext_set_visible(bool vis)
{
    if (!s_mask || s_visible == vis) {
        return;
    }
    lv_obj_set_hidden(s_mask, !vis);
    if (vis) {
        lv_obj_move_foreground(s_mask);
    }
    s_visible = vis;
    ESP_LOGI(TAG, "[UEXT_UI] overlay %s", vis ? "SHOWN" : "HIDDEN");
}

/* 二次 SET 确认 → 切槽重启（taskLVGL 上下文） */
static void uext_commit_switch(void)
{
    /* 防护 1：OTA 正在推固件时绝不允许切走（写槽进行中重启 = 半个坏镜像） */
    if (ota_service_is_active()) {
        ESP_LOGW(TAG, "[UEXT_UI] OTA active, switch request ignored");
        lv_label_set_text(s_status, "升级进行中，暂不能切换");
        s_remain_ticks = UEXT_CONFIRM_TICKS;   /* 停留展示这句直到超时 */
        return;
    }

    /* ★ V1.3：uext 槽 = ota_2（连续 OTA 子类型）。
       [V1.2 factory 已废弃：IDF flash 工具链 factory 优先选 app 落点，会打乱 main 的
       idf.py flash；V1.1 ota_15 已废弃：非连续 OTA 子类型无法作为 set_boot 目标。]
       ota_2 在连续计数内（app_count=3），set_boot 走标准 OTA 机制；main 的 OTA 轮替
       被 ota_service 的 ota_pick_update_slot() 锁定在 ota_0<->ota_1，永不碰本槽。 */
    const esp_partition_t *uext = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 2),
        NULL);
    if (!uext) {
        ESP_LOGE(TAG, "[UEXT_UI] uext partition (ota_15) not found in table");
        lv_label_set_text(s_status, "未找到副屏固件分区");
        s_remain_ticks = UEXT_CONFIRM_TICKS;
        return;
    }

    ESP_LOGW(TAG, "[UEXT_UI] confirmed -> boot uext (%s @0x%08x) and restart",
             uext->label, (unsigned)uext->address);
    lv_label_set_text(s_status, "正在切换，即将重启…");
    lv_obj_set_hidden(s_mask, false);   /* 确保文字可见 */

    esp_err_t err = esp_ota_set_boot_partition(uext);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[UEXT_UI] set_boot_partition failed: %s", esp_err_to_name(err));
        lv_label_set_text(s_status, "切换失败，设备未改动");
        s_remain_ticks = UEXT_CONFIRM_TICKS;
        return;
    }
    esp_restart();   /* 不返回 */
}

static void uext_ui_tick(lv_timer_t *t)
{
    (void)t;

    /* ---- 1) 消费 SET 单击 ---- */
    if (s_click_pending) {
        s_click_pending = false;
        if (!s_visible) {
            /* 第一次单击：弹出确认卡，倒计时开始 */
            s_remain_ticks = UEXT_CONFIRM_TICKS;
            lv_bar_set_value(s_bar, UEXT_CONFIRM_TICKS, LV_ANIM_OFF);
            lv_label_set_text(s_status, "再次按 SET 进入副屏");
            uext_set_visible(true);
        } else {
            /* 第二次单击：确认切换 */
            uext_commit_switch();   /* 可能 esp_restart 不返回 */
        }
    }

    /* ---- 2) 倒计时 ---- */
    if (s_visible) {
        if (--s_remain_ticks <= 0) {
            ESP_LOGI(TAG, "[UEXT_UI] confirm timeout, cancelled");
            uext_set_visible(false);
        } else {
            lv_bar_set_value(s_bar, s_remain_ticks, LV_ANIM_OFF);
        }
    }
}

/* =====================================================================
 * 公开 API
 * ===================================================================== */

void overlay_uext_init(void)
{
    if (s_mask) {
        return;   /* 幂等 */
    }
    uext_card_build();
    if (!s_timer) {
        s_timer = lv_timer_create(uext_ui_tick, UEXT_TICK_MS, NULL);
    }
    s_inited = true;
    ESP_LOGI(TAG, "[UEXT_UI] init done (tick=%dms, confirm=%dms)",
             UEXT_TICK_MS, UEXT_CONFIRM_TICKS * UEXT_TICK_MS);
}

void overlay_uext_deinit(void)
{
    s_inited = false;
    s_click_pending = false;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_mask) {
        lv_obj_del(s_mask);
        s_mask = NULL;
    }
    s_bar = NULL;
    s_status = NULL;
    s_visible = false;
    s_remain_ticks = 0;
    ESP_LOGI(TAG, "[UEXT_UI] deinit done");
}

void overlay_uext_notify_set_click(void)
{
    if (s_inited) {
        s_click_pending = true;
    }
}

void overlay_uext_show(void) { uext_set_visible(true); }
void overlay_uext_hide(void) { uext_set_visible(false); }
bool overlay_uext_is_visible(void) { return s_visible; }
