/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_system.c
 * @brief System info page.
 */

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"

#include "page_system.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "middleware/network/wifi_manager.h"
#include "middleware/network/ota_service.h"      /* §79：双向 OTA（查新/发起/进度） */
#include "middleware/bluetooth/link_manager.h"   /* §79：PC 心跳 fw_latest */
#include "middleware/system/sdcard_service.h"
#include "esp_app_desc.h"
#include "esp_mac.h"

/* 实时监视控件（页面销毁时清空，timer 同步删除） */
static lv_obj_t *s_storage_bar;
static lv_obj_t *s_storage_label;
static lv_obj_t *s_heap_label;
static lv_timer_t *s_stat_timer;

/* §79 双向 OTA 控件 */
static lv_obj_t *s_fw_latest_label;
static lv_obj_t *s_fw_status;
static lv_obj_t *s_btn_check;
static lv_obj_t *s_btn_switch;      /* §79e 手动切槽 */

/* §79c 抖动修复：1s 定时器的四个 label 全部缓存上次文本 ——
 * lv_label_set_text 即使内容相同也会 invalidate（LVGL 无条件重算+重绘），
 * 而文本长度变化还会引发 LV_SIZE_CONTENT 卡高重算 → 整页 flex 重排（全屏重绘）。
 * 每秒一次"全屏重绘 + esp_littlefs_info 冻结 cache"叠加 = v1.6 起实机可见的周期抖动。 */
static char s_last_storage[96];
static char s_last_heap[64];
static char s_last_latest[64];
static char s_last_status[64];

/** 文本变了才 set_text（不变 = 零 invalidate、零重排）。 */
static void label_set_if_changed(lv_obj_t *label, char *cache, size_t cache_len,
                                 const char *text)
{
    if (!label || !text) {
        return;
    }
    if (strncmp(cache, text, cache_len) != 0) {
        snprintf(cache, cache_len, "%s", text);
        lv_label_set_text(label, cache);
    }
}

static void check_update_cb(lv_event_t *e)
{
    (void)e;
    /* 一次非阻塞 sendto，LVGL 线程可直接调；结果由 1s 定时器轮询展示 */
    ota_service_request_update();
}

/* §79e 手动切槽（A/B 双系统）：校验/set_boot 在 ota_service 内完成，
 * 这里只负责把结果显示到状态行。OK 时 ota_service 内部 800ms 后自动重启。 */
static void switch_slot_cb(lv_event_t *e)
{
    (void)e;
    ota_switch_result_t r = ota_service_switch_slot();
    const char *msg;
    switch (r) {
    case OTA_SWITCH_OK:
        msg = "Switched! Rebooting...";          break;
    case OTA_SWITCH_BUSY:
        msg = "Switch refused (updating)";       break;
    case OTA_SWITCH_NO_TARGET:
        msg = "Switch refused (no target)";      break;
    case OTA_SWITCH_INVALID:
        msg = "Switch refused (target dead)";    break;
    default:
        msg = "Switch failed (set_boot)";        break;
    }
    if (s_fw_status) {
        /* 同步文本缓存：绕过缓存直接 set 会被 1s 定时器按旧缓存写回 */
        snprintf(s_last_status, sizeof(s_last_status), "%s", msg);
        lv_label_set_text(s_fw_status, s_last_status);
    }
}

/* 跑在 LVGL task：读 littlefs 用量 + 堆空闲，刷新 UI */
static void system_stat_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* §79c：esp_littlefs_info 是 flash IO（LVGL 线程冻结 ext-mem cache → GDMA
     * 饿一拍 → 撕裂）。OTA 升级期本就每秒多次 flash 写，这里再叠加读 = 滑屏
     * 加重 —— 升级期直接跳过（存储行保持上次值，升级完成自然恢复）。 */
    if (!ota_service_is_active()) {
        size_t total = 0, used = 0;
        if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
            int pct = total ? (int)((used * 100U) / total) : 0;
            char buf[96];
            snprintf(buf, sizeof(buf), "Storage  %u / %u KB  (%d%%)",
                     (unsigned)(used / 1024), (unsigned)(total / 1024), pct);
            label_set_if_changed(s_storage_label, s_last_storage,
                                 sizeof(s_last_storage), buf);
            if (s_storage_bar) {
                int cur = (int)lv_bar_get_value(s_storage_bar);
                if (cur != pct) {
                    lv_bar_set_value(s_storage_bar, pct, LV_ANIM_OFF);
                }
            }
        }
    }
    if (s_heap_label) {
        char hb[64];
        snprintf(hb, sizeof(hb), "Heap free  %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        label_set_if_changed(s_heap_label, s_last_heap, sizeof(s_last_heap), hb);
    }

    /* §79：Firmware Update 卡刷新（LVGL 线程只读 volatile 状态 + 一次栈上拷贝） */
    if (s_fw_latest_label) {
        char latest[32];
        link_manager_get_fw_latest(latest, sizeof(latest));
        char lb[64];
        snprintf(lb, sizeof(lb), "Latest  %s", latest[0] ? latest : "-");
        label_set_if_changed(s_fw_latest_label, s_last_latest,
                             sizeof(s_last_latest), lb);
    }
    if (s_fw_status) {
        char st[64];
        if (ota_service_is_active()) {
            snprintf(st, sizeof(st), "Updating  %u%%",
                     (unsigned)ota_service_get_percent());
        } else {
            switch (ota_service_get_check_state()) {
            case OTA_CHECK_PENDING:
                snprintf(st, sizeof(st), "Checking with PC..."); break;
            case OTA_CHECK_NOUPDATE:
                snprintf(st, sizeof(st), "Up to date"); break;
            case OTA_CHECK_FOUND:
                snprintf(st, sizeof(st), "Update available"); break;
            case OTA_CHECK_TIMEOUT:
                snprintf(st, sizeof(st), "No response (PC offline?)"); break;
            default:
                snprintf(st, sizeof(st), "Firmware update  -"); break;
            }
        }
        label_set_if_changed(s_fw_status, s_last_status, sizeof(s_last_status), st);
        if (s_btn_check) {
            if (ota_service_is_active() ||
                ota_service_get_check_state() == OTA_CHECK_PENDING) {
                lv_obj_add_state(s_btn_check, LV_STATE_DISABLED);
            } else {
                lv_obj_clear_state(s_btn_check, LV_STATE_DISABLED);
            }
        }
        if (s_btn_switch) {          /* §79e：升级进行中禁止切槽 */
            if (ota_service_is_active()) {
                lv_obj_add_state(s_btn_switch, LV_STATE_DISABLED);
            } else {
                lv_obj_clear_state(s_btn_switch, LV_STATE_DISABLED);
            }
        }
    }
}

static lv_obj_t *system_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(page, 6, 0);
    lv_obj_set_style_pad_row(page, 6, 0);
    /* §79b 排查修复（用户拍板：**不要滚动**，一屏放下）：旧布局三卡 240+150+~90
     * + 标题栏48 + 间距 ≈ 554px，> tabview 页可视 ~408px（480 − tab栏48 − 页 pad
     * 12×2，F46），而页根工厂默认 scrollable=false（ui_common.c:30）⇒ 超界内容
     * 被**静默裁掉**，Firmware Update 卡整卡不可见（v1.6 实机复现，bin 已确认
     * 是 v1.6，纯布局 bug）。
     * 修复 = 三卡全部 LV_SIZE_CONTENT + pad 收紧（12/6）+ 页 pad_top/row 6：
     *   标题48 + 设备信息~172 + OTA卡~62 + 存储卡~82 + pad/gap ~24 ≈ 388 ≤ 408，
     * 一屏放下，零信息删除。★ 改本页布局前先按这个预算算高度。 */
    lv_obj_set_scrollable(page, false);

    ui_common_title_bar(page, LV_SYMBOL_SETTINGS"  System");

    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_size(card, BSP_LCD_H_RES - 40, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_border_color(card, COL_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);

    char buf[96];
    lv_obj_t *l;

    /* ---- Device info（§70）：固件版本 / 编译时间 / 网络 / MAC ----
       数据全部来自 esp_app_desc + wifi_manager + sdcard_service，
       进页面时一次读取（静态快照）。全 ASCII 文案，纯 montserrat_14。 */
    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ip[16] = "-";
    wifi_manager_get_ip(ip, sizeof(ip));

    l = lv_label_create(card);
    lv_label_set_text_static(l, "Board: ESP32-S31-Korvo-1 V1.1");
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    lv_snprintf(buf, sizeof(buf), "Firmware: %s  (IDF %s)", app->version, app->idf_ver);
    l = lv_label_create(card);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, COL_CYAN, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    lv_snprintf(buf, sizeof(buf), "Build: %s %s", app->date, app->time);
    l = lv_label_create(card);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    l = lv_label_create(card);
    /* §79e：LCD/音频规格行换成槽位对信息（等高，一屏预算不变） */
    {
        char pair[48];
        ota_service_get_slot_pair(pair, sizeof(pair));
        char slot_line[96];     /* 48 + 常量段 18 → ≥66，64 会触发 format-truncation */
        snprintf(slot_line, sizeof(slot_line), "%s  |  Audio ES8389", pair);
        lv_label_set_text(l, slot_line);
    }
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    lv_snprintf(buf, sizeof(buf), "IP: %s  |  SD: %s", ip,
                sdcard_service_is_mounted() ? "OK" : "NO");
    l = lv_label_create(card);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    lv_snprintf(buf, sizeof(buf), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    l = lv_label_create(card);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    l = lv_label_create(card);
    lv_label_set_text_static(l, "GitHub: xkk6663");
    lv_obj_set_style_text_color(l, COL_CYAN, 0);
    lv_obj_set_style_text_font(l, UI_FONT, 0);

    /* ---- Firmware Update 卡（§79 双向 OTA）----
     * Latest 行 = PC 心跳 fw_latest（PC 侧最新编译固件版本）；
     * Check update 按钮 = 设备端主动发起升级（OTA_REQ，PC 有更新自动开拉）；
     * 状态行由下方 1s 定时器刷新（is_active 进度 / 查新状态）。
     * ★ 排在存储卡之前：一屏内直接可见。 */
    lv_obj_t *ucard = lv_obj_create(page);
    lv_obj_set_size(ucard, BSP_LCD_H_RES - 40, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ucard, COL_CARD, 0);
    lv_obj_set_style_border_color(ucard, COL_BORDER, 0);
    lv_obj_set_style_border_width(ucard, 1, 0);
    lv_obj_set_style_radius(ucard, 16, 0);
    lv_obj_set_style_pad_all(ucard, 12, 0);
    lv_obj_set_flex_flow(ucard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ucard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ucard, 14, 0);
    /* §79c：固定高度 —— LV_SIZE_CONTENT 卡里的 label 每秒文本变化会引发
     * "label 重算 → 卡高重算 → 页 flex 全列重排（全屏 invalidate）"级联，
     * 是连接后周期抖动的主源之一。内容两行 16px + gap4 = 36，卡 = 36+24 = 62。 */
    lv_obj_set_height(ucard, 62);

    lv_obj_t *ucol = lv_obj_create(ucard);      /* 左：两行文字（透明容器） */
    lv_obj_set_size(ucol, 460, 38);
    lv_obj_set_style_bg_opa(ucol, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ucol, 0, 0);
    lv_obj_set_style_pad_all(ucol, 0, 0);
    lv_obj_set_flex_flow(ucol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ucol, 4, 0);

    s_fw_latest_label = lv_label_create(ucol);
    lv_label_set_text_static(s_fw_latest_label, "Latest  -");
    lv_obj_set_style_text_color(s_fw_latest_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_fw_latest_label, UI_FONT, 0);

    s_fw_status = lv_label_create(ucol);
    lv_label_set_text_static(s_fw_status, "Firmware update  -");
    lv_obj_set_style_text_color(s_fw_status, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_fw_status, UI_FONT, 0);

    s_btn_check = lv_btn_create(ucard);         /* 右：发起升级 */
    lv_obj_set_size(s_btn_check, 120, 38);
    lv_obj_set_style_bg_color(s_btn_check, COL_CYAN, 0);
    lv_obj_set_style_bg_color(s_btn_check, COL_BORDER, LV_STATE_DISABLED);
    lv_obj_t *bl = lv_label_create(s_btn_check);
    lv_label_set_text_static(bl, "Update");
    lv_obj_set_style_text_color(bl, COL_DARK, 0);   /* F47 附：文字恒色可设 label */
    lv_obj_set_style_text_font(bl, UI_FONT, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_btn_check, check_update_cb, LV_EVENT_CLICKED, NULL);

    /* §79e：手动切槽（A/B 双系统）。防砖闸在 ota_service 内（镜像/状态校验） */
    s_btn_switch = lv_btn_create(ucard);
    lv_obj_set_size(s_btn_switch, 120, 38);
    lv_obj_set_style_bg_color(s_btn_switch, COL_CARD_HI, 0);
    lv_obj_set_style_bg_color(s_btn_switch, COL_BORDER, LV_STATE_DISABLED);
    lv_obj_t *sl = lv_label_create(s_btn_switch);
    lv_label_set_text_static(sl, "Switch");
    lv_obj_set_style_text_color(sl, COL_TEXT, 0);
    lv_obj_set_style_text_font(sl, UI_FONT, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_btn_switch, switch_slot_cb, LV_EVENT_CLICKED, NULL);

    /* ---- Flash / storage 实时监视卡片（压缩版，§79b）----
     * §79c：固定高度 84（16+6+12+6+16+pad24）—— 理由同 ucard，禁止每秒重排。 */
    lv_obj_t *scard = lv_obj_create(page);
    lv_obj_set_size(scard, BSP_LCD_H_RES - 40, 84);
    lv_obj_set_style_bg_color(scard, COL_CARD, 0);
    lv_obj_set_style_border_color(scard, COL_BORDER, 0);
    lv_obj_set_style_border_width(scard, 1, 0);
    lv_obj_set_style_radius(scard, 16, 0);
    lv_obj_set_style_pad_all(scard, 12, 0);
    lv_obj_set_flex_flow(scard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scard, 6, 0);

    s_storage_label = lv_label_create(scard);
    lv_label_set_text_static(s_storage_label, "Storage  -- KB");
    lv_obj_set_style_text_color(s_storage_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_storage_label, UI_FONT, 0);

    s_storage_bar = lv_bar_create(scard);
    lv_obj_set_size(s_storage_bar, BSP_LCD_H_RES - 72, 12);
    lv_bar_set_range(s_storage_bar, 0, 100);
    lv_obj_set_style_bg_color(s_storage_bar, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_storage_bar, COL_CYAN, LV_PART_INDICATOR);

    s_heap_label = lv_label_create(scard);
    lv_label_set_text_static(s_heap_label, "Heap free  -- KB");
    lv_obj_set_style_text_color(s_heap_label, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_heap_label, UI_FONT, 0);

    /* 1s 节拍刷新（跑在 LVGL task，安全） */
    s_stat_timer = lv_timer_create(system_stat_timer_cb, 1000, NULL);
    system_stat_timer_cb(s_stat_timer); /* 立刻刷一次 */

    return page;
}

static void system_destroy(lv_obj_t *page)
{
    if (s_stat_timer) {
        lv_timer_del(s_stat_timer);
        s_stat_timer = NULL;
    }
    s_storage_bar = NULL;
    s_storage_label = NULL;
    s_heap_label = NULL;
    s_fw_latest_label = NULL;
    s_fw_status = NULL;
    s_btn_check = NULL;
    s_btn_switch = NULL;
    lv_obj_del(page);
}

const Page_t Page_System = {
    .create = system_create,
    .destroy = system_destroy,
    .name = "System",
};
