/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file page_face.c
 * @brief 人脸库管理页 + 打卡记录浏览页（S8-1 + S8-4）。
 *
 * 方案：docs/项目规划/S8_人脸识别打卡系统方案.md §5.5（库管理）、§8-S8-4（记录浏览）
 *
 * ===========================================================================
 *  ★ 为什么开独立子页，而不是往 CAM 页里塞
 * ===========================================================================
 *  CAM 页已经有 START/HSV/FACE/MODEL/MODE 五个按钮（tab_camera.c），
 *  再塞"列表 + 删除"必然挤爆。库管理是**低频**操作（录完就不再碰），
 *  走 SYS 页入口 + page_manager 页面栈，与 WIFI / RGB / System
 *  四个子页同构 —— 用户不用学第二套导航。
 *
 * ===========================================================================
 *  ★★ S8-4：两块内容为什么共用**同一个列表容器**
 * ===========================================================================
 *  "人脸库"和"打卡记录"是两个数据源、两套生命周期，但它们**长得一模一样**
 *  （一堆等高的卡行 + 一个滚动区 + 一行状态文本）。共用 `s_list` 换来的是：
 *    · 垂直预算不用重算（本页总高 8+48+8+48+8+48+8+250+8+36 = 470 ≤ 480）；
 *    · 不会出现"两个滚动区各自记着滚动位置"的怪状态；
 *    · 切视图时只有 **一次** lv_obj_clean()，不可能有残留行。
 *  ⇒ 用 `s_view` 一个变量切换，**同时只有一个数据源在渲染**。
 *
 *  ★★ 但数据源**绝不共用**（这条是红线，来自 face_db.h 文件头第 1 条）：
 *    · 人脸库 → 一律走 `face_db_*` 接口，本页**不直接 open/read** 那个目录；
 *      索引的双份世代号、CRC 校验、加锁都在 face_db 内部，绕过去就等于把
 *      "掉电一致性"重新实现一遍，必定做错。
 *    · 打卡记录 → 一律走 `punch_service_log_*`，本页也**不直接 open/read** CSV。
 *    两者物理上就是两个目录（/sdcard/face_db 与 /sdcard/attendance），
 *    "清空打卡记录"因此**不可能**碰到人脸库 —— 这条是 S8-4 的验收项。
 *
 * ===========================================================================
 *  ★★ S8-4 的三条实现纪律
 * ===========================================================================
 *  ① **分页渲染，不是"全渲染再滚动"**：`PUNCH_LOG_MAX` 条里一次只建
 *     `REC_PAGE_SIZE` 行对象。100 条全建 = 700 个 LVGL 对象（≈ 100 KB 堆），
 *     而 LVGL 的 flex 布局每帧都要遍历所有可见子对象 —— 那是"滚起来掉帧"的
 *     直接成因。20 行 = 140 个对象，与记录总数**无关**。
 *  ② **CSV 只在必要时重读**（进页 / 切分组 / 清空后 / 3 s 轮询且**预览没开**）。
 *     `punch_service_log_read()` 是**同步**读全文件，跑在 LVGL 线程里；
 *     3 s 一次、文件几十 KB，代价可以忽略；1 s 一次就没必要了。
 *     预览开着时**不重读**：否则 `s_rec[]` 会被就地覆盖，而用户手上正按着
 *     某一行 —— 那会预览到"另一条记录的照片"。
 *  ③ **不依赖识别引擎**：本页不 include 任何 esp-dl 头。即使识别没跑起来，
 *     只要库/记录里有数据，这页就能显示。
 *
 * ===========================================================================
 *  ★ 两个环境事实决定了本页的形态（都来自既有模块的硬约束，不是设计偏好）
 * ===========================================================================
 *  ① **不支持热插拔**（sdcard_service.h）：运行期拔卡不会自动卸载，插回也不会
 *     自动重挂。所以这里给一个 `RETRY` 按钮 = 重挂载 + 重 init。
 *     没有它，"开机时没插卡"就是一块**永久砖**（页面永远 not ready，只能重启），
 *     而那正是本工程最讨厌的"按下去没反应 / 只能重启"体验。
 *  ② 挂载/初始化失败**不是致命错误**（face_db.h 文件头第 3 条红线）：
 *     本页所有分支都必须能在"库不可用"下正常渲染，绝不 ESP_ERROR_CHECK。
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>             /* qsort */
#include <string.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "page_face.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "ui_binfont.h"
#include "middleware/face_db/face_db.h"
#include "middleware/periph/sdcard_service.h"
#include "middleware/camera/punch_service.h"   /* ★ S8-4：打卡流水（含 epoch_s） */
#include "middleware/media/file_mgr_service.h" /* ★ S8-4：预览复用 FMC_PREVIEW */
#include "middleware/camera/viewfinder.h"      /* ★ §73：预览前必须 acquire 解码缓冲 */

/* ★ 人名用字体：优先 LittleFS 里那份全字表 bin 字体（ui_binfont）—— 它的字表
 *   覆盖整个 CJK 基本区，人名不会出方块。回退的 lv_font_cn_20 是**固定小字表**
 *   （只覆盖界面上写过的死字），人名很可能不在其中 ⇒ 回退档只保证"不崩"，
 *   不保证每个名字都好看。
 *   ⚠ 两者都**不能**拿去套"图标 + 英文"的标签（无 fallback；且 bin 字体 28 px
 *      会把原本 14 px 的英文撑大）—— 见 ui_binfont.h 的"使用禁区"。 */
LV_FONT_DECLARE(lv_font_cn_20);

#define TAG "PAGE_FACE"

/* ===========================================================================
 *  布局常量（垂直预算见文件头，改动其中一个就要重算那一行）
 * =========================================================================== */
#define FACE_ROW_H      52
/* ★ 行宽必须比列表的**内容区**窄：列表 760 宽、pad_all 6 ⇒ 内容区 748，
 *   再留 8 px 给竖向滚动条 ⇒ 740。（10 行时必然出滚动条 —— 算错了就是
 *   DEL 按钮被推出可视区，而这属于"看得见按钮、点不到右半边"的隐性故障。） */
#define FACE_ROW_W      (BSP_LCD_H_RES - 40 - 12 - 8)
#define FACE_LIST_H     250     /* ★ S8-4：从 300 降到 250，给底部页码行腾地方 */
#define BOTTOM_H        36

/* ---- S8-4 ---- */
/** 一页渲染多少行。★ 见文件头纪律①：这是"100 条不卡"的关键，不是省事。 */
#define REC_PAGE_SIZE   20
/** 轮询周期。300 ms 而非 1 s：预览是**异步**完成的（file_mgr 服务任务），
 *  1 s 才去看一眼会让"点了预览要等一秒才出图"这种观感问题变成常态。 */
#define REC_POLL_MS     300
/** 多少拍重读一次 CSV ≈ 3 s（10 × 300 ms） */
#define REC_RESCAN_TICKS 10
/** 一条清理请求等待多久算超时（50 × 300 ms = 15 s） */
#define REC_CLEAR_TIMEOUT_TICKS 50
/** 两步确认的自动还原时间（10 × 300 ms = 3 s），与 tab_files 的 DELETE 同款 */
#define CLR_ARM_TICKS   10

/** 视图 */
#define VIEW_PERSONS    0
#define VIEW_RECORDS    1
/** ★ §73.11（用户要求）：记录按**时间升/降序**排列，取代旧的 BY DATE/BY PERSON 分组 */
#define SORT_DESC       0       /* 新的在上（默认） */
#define SORT_ASC        1       /* 旧的在上 */

/* ===========================================================================
 *  状态
 * =========================================================================== */
static lv_obj_t   *s_info;         /* 顶栏状态文本（两种视图共用，内容不同） */
static lv_obj_t   *s_list;         /* 行容器（scrollable，两种视图共用） */
static lv_obj_t   *s_hint;         /* 底部提示（空库 / 未就绪 / 操作结果） */
static lv_obj_t   *s_bar2;         /* ★ S8-4：第二行工具条（仅 RECORDS 可见） */
static lv_obj_t   *s_btn_view;     /* 顶栏右侧：PERSONS <-> RECORDS */
static lv_obj_t   *s_btn_view_lab;
static lv_obj_t   *s_btn_group;    /* 第二行：NEW FIRST <-> OLD FIRST（时间排序切换） */
static lv_obj_t   *s_btn_group_lab;
static lv_obj_t   *s_btn_sync;     /* ★ 2026-09-28：第二行 SYNC（照片缺失行对账清理） */
static lv_obj_t   *s_btn_sync_lab;
static lv_obj_t   *s_btn_clear;    /* 第二行：CLEAR（两步确认） */
static lv_obj_t   *s_btn_clear_lab;
static lv_obj_t   *s_pager;        /* 底行右侧页码容器（仅 RECORDS 可见） */
static lv_obj_t   *s_pager_lab;
static lv_obj_t   *s_btn_prev;
static lv_obj_t   *s_btn_next;
static lv_timer_t *s_timer;

/* ★ S8-4 预览覆盖层（建页时一次性建好、平时 HIDDEN） */
static lv_obj_t   *s_pv_ov;
static lv_obj_t   *s_pv_canvas;
/* ★ 顶部说明**拆成两个标签**，不是一个：
 *   · 姓名必须用 CJK 字体（bin 字体 28 px，字表覆盖整个 CJK 基本区）；
 *   · 序号/日期/时间必须用 UI_FONT（montserrat_14）。
 *   混在一个标签里的话，要么中文出方块（用 UI_FONT），要么数字被撑到 28 px
 *   而挤掉右边的 CLOSE 按钮（用 CJK 字体）。—— 见 ui_binfont.h 的"使用禁区"。 */
static lv_obj_t   *s_pv_cap;       /* 姓名（CJK 字体） */
static lv_obj_t   *s_pv_meta;      /* "#12  2026-09-28 10:30:45"（UI_FONT） */
static lv_obj_t   *s_pv_stat;      /* 底部状态："Loading..." / "Preview failed: ..." */

/* 上一次渲染时的库状态 —— 只在**变化**时重建列表。
 * ★ 不这么做的话，300 ms 定时器会把 10 行对象每秒三次删光重建：
 *   堆分配 + 全屏重绘，纯属自残。 */
static int  s_last_count = -1;
static bool s_last_ready;

/* ---- S8-4 视图/排序/分页 ---- */
static int  s_view  = VIEW_PERSONS;
static int  s_sort  = SORT_DESC;    /* ★ §73.11：时间排序方向（取代旧分组） */
static int  s_rec_page = 0;
static int  s_rec_n    = 0;         /* s_rec[] 里有效条数 */
static punch_log_t *s_rec = NULL;   /* ★ PSRAM：REC_LOG_MAX × ~120 B ≈ 15 KB */
static int  s_rec_ticks = 0;        /* 距下次重读还剩几拍 */

/* ---- S8-4 清空（异步 + 两步确认） ---- */
static bool s_clr_armed = false;
static int  s_clr_arm_ticks = 0;
static bool s_clr_pending = false;  /* 已发起，等 punch_service_busy() 落下 */
static int  s_clr_ticks = 0;

/* ---- ★ 2026-09-28：行内删除（两步确认）与照片对账（异步，同 CLEAR 的收尾模式） ---- */
static bool s_del_pending  = false; /* 删单条已发起，等 busy 落下 */
static int  s_del_ticks    = 0;
static bool s_sync_pending = false; /* 对账已发起，等 busy 落下 */
static int  s_sync_ticks   = 0;
/* ★ 行内删除的两步确认：记"待确认的 seq"而不是给按钮换文案 ——
 *   列表随时可能被 records_load/rebuild 重建，按钮对象是新的，
 *   在旧按钮上改文案会 dangling；提示走底部的 hint 条（对象是常驻的）。 */
static int  s_del_armed_seq  = 0;
static int  s_del_arm_ticks  = 0;
/* ★ 已发起删除的那一行的 seq：收尾时在重读结果里找它，判断删没删成
 *  （不能靠 punch_service_get_last()，那个槽会被 CAM 页的打卡轮询偷走）。 */
static int  s_del_target_seq = 0;

/* ---- S8-4 预览（异步，事件来自 file_mgr 服务任务） ---- */
static bool         s_pv_pending = false;
static volatile int s_pv_result  = -2;   /* -2 = 还没结果；否则 esp_err 值 */
/* ★ §73：解码缓冲的持有标记。viewfinder_acquire() 幂等，release 也幂等，
 *   但"该不该 release"必须由我们自己记账 —— 离开 RECORDS 视图 / 销毁页面时
 *   归还那 ~2.93 MB；没持有就 release 是无害空操作，但标记避免误账。 */
static bool         s_pv_held    = false;
/* ★ 1×1 RGB565 占位缓冲：`lv_canvas_create()` **不会**自带 draw buffer，
 *   没有它就去渲染那个 canvas 会解引用空指针。覆盖层在被显示出来的那一刻
 *   可能还没拿到真帧（解码是异步的），所以必须有一个合法的占位。 */
static uint16_t     s_pv_dummy   = 0;
/* clear_btn_apply() 的"已应用"三态缓存：-1 = 未知（建页后要先摆一次）、
 * 0 = 普通态已应用、1 = 待确认态已应用。用文件级变量而不是函数内 static，
 * 是为了让 face_create() 能把它复位 —— 否则"上次退出时按钮正停在 SURE?"
 * 会让新页面的按钮第一次点击就失效（判据直接短路）。 */
static int          s_clr_applied = -1;
/* ★ 借调 file_mgr 的**唯一**回调槽：存下原主（tab_files）的注册，离开时还原。
 *   没有还原的话，从本页返回后 FILES 页会彻底收不到事件（点了没反应）。
 *   为此给 file_mgr_service 加了 file_mgr_get_evt_cb()（见其头文件注释）。 */
static file_mgr_evt_cb_t s_saved_cb   = NULL;
static void             *s_saved_user = NULL;

/* 前向声明（全是 static，定义在下面）：
 * ★ 本文件里"回调先被用到、后定义"是必然的（列表行创建时要挂 del_btn_cb，
 *   而 del_btn_cb 又要重建列表）。集中声明在顶部比到处挪函数顺序清楚。 */
static void list_rebuild(void);
static void page_refresh(bool force);
static void del_btn_cb(lv_event_t *e);
static void retry_btn_cb(lv_event_t *e);
static void face_poll_cb(lv_timer_t *t);
static void view_apply(void);
static void records_rebuild(void);
static void rec_chrome_apply(void);
static void clear_btn_apply(void);
static void prev_btn_cb(lv_event_t *e);
static void prev_close_cb(lv_event_t *e);
static void preview_request(int idx);
static void preview_pump(void);
static void del_rec_btn_cb(lv_event_t *e);   /* ★ 2026-09-28：行内删除 */
static void sync_btn_cb(lv_event_t *e);      /* ★ 2026-09-28：照片对账 */
static void hint_set(const char *txt);
static void hint_hide(void);
static void async_refresh_cb(void *arg);

/* ---------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------- */
static const lv_font_t *cjk_font(void)
{
    const lv_font_t *f = ui_binfont_get();
    return f ? f : &lv_font_cn_20;
}

/** 底部提示：显示 + 写文本（内容相同则零代价） */
static void hint_set(const char *txt)
{
    if (!s_hint) return;
    ui_common_set_label_text_if_changed(s_hint, txt);
    lv_obj_set_hidden(s_hint, false);
}

static void hint_hide(void)
{
    if (!s_hint) return;
    lv_obj_set_hidden(s_hint, true);
}

/** 路径拷贝（照抄 tab_files 的同名小工具：越界即拒，绝不静默截断） */
static bool path_copy(char *dst, size_t dstsz, const char *src)
{
    if (!dst || !src || dstsz == 0) return false;
    int n = snprintf(dst, dstsz, "%s", src);
    return (n > 0 && (size_t)n < dstsz);
}

/* ---------------------------------------------------------------------------
 *  渲染：人脸库视图
 * ------------------------------------------------------------------------- */
/** 重建列表（逐个 face_db_get 读出人名） */
static void persons_list_rebuild(void)
{
    face_db_info_t info;

    face_db_get_info(&info);

    /* ★ lv_obj_clean() 只删子对象、不删自己 —— 与 page_wifi 的 AP 列表同一手法。 */
    lv_obj_clean(s_list);

    if (!info.ready) {
        hint_set("SD card not mounted - insert it, then tap RETRY");
        return;
    }
    if (info.count <= 0) {
        hint_set("Empty. Enroll from CAMERA page (MODE -> ENROLL)");
        return;
    }

    hint_hide();

    for (int i = 0; i < info.count; i++) {
        face_person_t p;

        if (face_db_get(i, &p) != ESP_OK) {
            continue;               /* 槽位空/越界：跳过即可，不要因此中断整表 */
        }

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, FACE_ROW_W, FACE_ROW_H);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_border_color(row, COL_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 10, 0);
        /* ★ 顺序有讲究：先 pad_all 再覆盖左右 —— 反过来写会让 pad_all 把
         *   刚设好的左右又抹成 0（样式按 selector 覆盖，同 selector 后写的生效）。 */
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_pad_right(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* 序号（青色）。★ id 一旦分配**永不复用**（face_db.h），所以序号是稳定标识。 */
        lv_obj_t *idx_lab = lv_label_create(row);
        lv_label_set_text_fmt(idx_lab, "#%02u", (unsigned)p.id);
        lv_obj_set_style_text_color(idx_lab, COL_CYAN, 0);
        lv_obj_set_style_text_font(idx_lab, UI_FONT, 0);

        /* 姓名（CJK 字体）。★ 必须用**拷贝**语义：p 是栈上临时量。
         *   name 已由 face_db 做过 UTF-8 边界截断（不会切半个汉字）与
         *   FATFS 非法字符替换，所以这里可以放心直显。 */
        lv_obj_t *nm_lab = lv_label_create(row);
        lv_label_set_text(nm_lab, p.name);
        lv_obj_set_style_text_color(nm_lab, COL_TEXT, 0);
        lv_obj_set_style_text_font(nm_lab, cjk_font(), 0);

        /* 采样张数（质量参考，S8-1 恒为 3） */
        lv_obj_t *sh_lab = lv_label_create(row);
        lv_label_set_text_fmt(sh_lab, "%u shots", (unsigned)p.sample_cnt);
        lv_obj_set_style_text_color(sh_lab, COL_SUBTEXT, 0);
        lv_obj_set_style_text_font(sh_lab, UI_FONT, 0);

        /* 撑开中间空隙，把 DEL 顶到最右。
         * ★ 不用 flex_align=SPACE_BETWEEN：那会让姓名长短直接改变 DEL 的横向位置，
         *   10 行的按钮排不成一列，看着很乱。
         *   （本 LVGL 版本没有 lv_obj_set_flex_grow，只有 style 版。） */
        lv_obj_t *sp = lv_obj_create(row);
        lv_obj_set_size(sp, 1, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        lv_obj_set_scrollable(sp, false);
        lv_obj_set_style_flex_grow(sp, 1, 0);

        lv_obj_t *del = lv_btn_create(row);
        lv_obj_set_size(del, 56, 34);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_bg_color(del, COL_RED, 0);
        /* ★ id 随事件 user_data 传入（本工程既有手法，见 tab_sys_entry 的 row_click_cb）。
         *   ⚠ 不要用 lv_obj_set_user_data 存再解引用 —— 这里存的就是一个整数。 */
        lv_obj_add_event_cb(del, del_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)p.id);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text_static(dl, LV_SYMBOL_TRASH);      /* 字面量 ⇒ static 安全 */
        lv_obj_set_style_text_color(dl, COL_DARK, 0);
        lv_obj_set_style_text_font(dl, UI_FONT, 0);
        lv_obj_center(dl);
    }
}

/* ---------------------------------------------------------------------------
 *  渲染：打卡记录视图（S8-4）
 * ------------------------------------------------------------------------- */

/** 排序判据。★ 用模块静态 s_sort 而不是给 qsort 传上下文：标准 qsort 没有
 *  context 参数（qsort_r 不是可移植扩展），为它引入平台分支不值得。
 *  ⚠ 只有在"没有别的线程"时才安全 —— 本函数只在 LVGL 线程调用，成立。 */
static int rec_cmp(const void *pa, const void *pb)
{
    const punch_log_t *a = (const punch_log_t *)pa;
    const punch_log_t *b = (const punch_log_t *)pb;

    /* ★ 未校时的（epoch_s == 0）一律沉到最后（两个方向都一样）：
     *   不去比较 date/time 字符串：那批是 "--"，字典序会插在 "2026-…" 前面，
     *   表现成"新记录被一堆 -- 顶到下面"—— 数据少的时候根本看不出来。 */
    bool za = (a->epoch_s == 0);
    bool zb = (b->epoch_s == 0);
    if (za != zb) return za ? 1 : -1;

    bool asc = (s_sort == SORT_ASC);
    if (a->epoch_s != b->epoch_s) {
        return (a->epoch_s > b->epoch_s) ? (asc ? 1 : -1)
                                         : (asc ? -1 : 1);
    }
    /* 同一秒：按 seq 排（seq 单调递增 ⇒ 升序时小的在前、降序时大的在前） */
    return asc ? (a->seq - b->seq) : (b->seq - a->seq);
}

/** 读 CSV 到 s_rec[] 并排序。★ 只在 LVGL 线程调用（见文件头纪律②）。 */
static void records_load(void)
{
    if (s_rec == NULL) { s_rec_n = 0; return; }

    s_rec_n = punch_service_log_read(s_rec, PUNCH_LOG_MAX);
    if (s_rec_n > 1) {
        qsort(s_rec, (size_t)s_rec_n, sizeof(s_rec[0]), rec_cmp);
    }
    /* 页码夹紧：清空后 / 新读后条数可能变小，不夹紧就是空白页 */
    int pages = (s_rec_n + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
    if (pages <= 0) pages = 1;
    if (s_rec_page >= pages) s_rec_page = pages - 1;
    if (s_rec_page < 0) s_rec_page = 0;
}

/** 顶部状态 + 页码按钮的外观（只在"形状变了"时调用，不在轮询里调） */
static void rec_chrome_apply(void)
{
    int pages = (s_rec_n + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
    if (pages <= 0) pages = 1;

    if (s_pager_lab) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", s_rec_page + 1, pages);
        ui_common_set_label_text_if_changed(s_pager_lab, buf);
    }
    /* 首/末页把对应按钮置灰（比"点了没反应"清楚） */
    if (s_btn_prev) {
        if (s_rec_page <= 0) lv_obj_add_state(s_btn_prev, LV_STATE_DISABLED);
        else                 lv_obj_remove_state(s_btn_prev, LV_STATE_DISABLED);
    }
    if (s_btn_next) {
        if (s_rec_page >= pages - 1) lv_obj_add_state(s_btn_next, LV_STATE_DISABLED);
        else                         lv_obj_remove_state(s_btn_next, LV_STATE_DISABLED);
    }
}

/** 重建记录列表（**只建当前页的 REC_PAGE_SIZE 行**，见文件头纪律①） */
static void records_rebuild(void)
{
    lv_obj_clean(s_list);

    if (s_rec == NULL) {
        /* 建页时那一次性 15 KB PSRAM 没拿到 —— 说清楚原因，别报成"没有记录" */
        hint_set("Records unavailable: out of PSRAM");
        return;
    }
    if (!sdcard_service_is_mounted()) {
        hint_set("SD card not mounted - insert it, then tap RETRY");
        return;
    }
    if (s_rec_n <= 0) {
        hint_set("No attendance records yet");
        return;
    }

    hint_hide();

    int begin = s_rec_page * REC_PAGE_SIZE;
    int end   = begin + REC_PAGE_SIZE;
    if (end > s_rec_n) end = s_rec_n;

    for (int i = begin; i < end; i++) {
        const punch_log_t *r = &s_rec[i];

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, FACE_ROW_W, FACE_ROW_H);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_border_color(row, COL_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_pad_right(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* 流水号（青色）。★ 用它而不是数组下标：下标会随分组方式变化，
         *   而 seq 是 CSV 里写死的、跨视图稳定的标识（也是照片文件名的一部分）。 */
        lv_obj_t *seq_lab = lv_label_create(row);
        lv_label_set_text_fmt(seq_lab, "#%d", r->seq);
        lv_obj_set_style_text_color(seq_lab, COL_CYAN, 0);
        lv_obj_set_style_text_font(seq_lab, UI_FONT, 0);

        /* 姓名（CJK）。访客时 CSV 里写的就是"访客"两个字，直显即可。 */
        lv_obj_t *nm_lab = lv_label_create(row);
        lv_label_set_text(nm_lab, r->name[0] ? r->name : "?");
        lv_obj_set_style_text_color(nm_lab, COL_TEXT, 0);
        lv_obj_set_style_text_font(nm_lab, cjk_font(), 0);

        /* 日期 + 时间（等宽更好看，但本工程只有 montserrat_14，凑合用） */
        lv_obj_t *dt_lab = lv_label_create(row);
        lv_label_set_text_fmt(dt_lab, "%s %s", r->date, r->time);
        /* ★ 未校时（"-- --"）给灰色：这不是错误，但要一眼看出"时间不可信" */
        lv_obj_set_style_text_color(dt_lab, r->epoch_s ? COL_SUBTEXT : COL_BORDER, 0);
        lv_obj_set_style_text_font(dt_lab, UI_FONT, 0);

        /* 撑开中间空隙，把预览按钮顶到最右 */
        lv_obj_t *sp = lv_obj_create(row);
        lv_obj_set_size(sp, 1, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        lv_obj_set_scrollable(sp, false);
        lv_obj_set_style_flex_grow(sp, 1, 0);

        /* ★ user_data 传**数组下标**（不是指针）：指针会在下次 records_load()
         *   时指向被覆盖的记录内容；下标至少是"当次渲染"的稳定标识，
         *   而且 preview_request() 里还会再夹一次界。 */
        lv_obj_t *pv = lv_btn_create(row);
        lv_obj_set_size(pv, 44, 34);
        lv_obj_set_style_radius(pv, 8, 0);
        lv_obj_set_style_bg_color(pv, COL_BORDER, 0);
        lv_obj_add_event_cb(pv, prev_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *pl = lv_label_create(pv);
        lv_label_set_text_static(pl, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_color(pl, COL_TEXT, 0);
        lv_obj_set_style_text_font(pl, UI_FONT, 0);
        lv_obj_center(pl);

        /* ★ 2026-09-28：行内删除按钮（用户反馈"没有删除打卡照的按钮"）。
         *   user_data 传 **seq**（CSV 第一列）而不是数组下标 —— 删除是异步的，
         *   完成后 s_rec[] 会重读重排，只有 seq 是跨重载稳定的行标识。 */
        lv_obj_t *del = lv_btn_create(row);
        lv_obj_set_size(del, 44, 34);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_bg_color(del, COL_RED, 0);
        lv_obj_add_event_cb(del, del_rec_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)r->seq);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text_static(dl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(dl, COL_DARK, 0);
        lv_obj_set_style_text_font(dl, UI_FONT, 0);
        lv_obj_center(dl);
    }
}

/** list_rebuild 的派发点：两种视图**同一个容器**，同时只有一个在渲染 */
static void list_rebuild(void)
{
    if (s_list == NULL) return;

    if (s_view == VIEW_PERSONS) {
        persons_list_rebuild();
    } else {
        records_rebuild();
    }
}

/** 顶栏文本（人脸库） */
static void info_refresh(const face_db_info_t *info)
{
    char txt[96];

    if (info->ready) {
        snprintf(txt, sizeof(txt), "%d/%d persons   dim=%d   free=%d",
                 info->count, FACE_DB_MAX, info->feat_dim, info->free_slots);
    } else {
        snprintf(txt, sizeof(txt), "face db unavailable");
    }
    ui_common_set_label_text_if_changed(s_info, txt);
}

/** 顶栏文本（打卡记录） */
static void rec_info_refresh(void)
{
    char txt[96];

    if (!sdcard_service_is_mounted()) {
        snprintf(txt, sizeof(txt), "no SD card");
    } else {
        /* ★ 说清楚"最多读多少条"：超过 PUNCH_LOG_MAX 的旧记录确实看不到，
         *   与其让用户数着不对，不如写出来。 */
        snprintf(txt, sizeof(txt), "%d records   (last %d)   %s",
                 s_rec_n, PUNCH_LOG_MAX,
                 s_sort == SORT_ASC ? "old first" : "new first");
    }
    ui_common_set_label_text_if_changed(s_info, txt);
}

/**
 * 刷新入口。
 * @param force true = 无条件重建列表（删除后 / 重试后 / 建页时 / 切视图）
 */
static void page_refresh(bool force)
{
    face_db_info_t info;

    if (s_list == NULL) {
        return;                 /* 页面已销毁（异步回调可能晚到一拍） */
    }

    if (s_view == VIEW_RECORDS) {
        rec_info_refresh();
        if (force) records_rebuild();
        return;
    }

    face_db_get_info(&info);
    info_refresh(&info);

    /* ★ 只在"库的形状变了"时重建：count 变了（增删）或 ready 变了（插/拔卡）。 */
    if (force || info.count != s_last_count || info.ready != s_last_ready) {
        s_last_count = info.count;
        s_last_ready = info.ready;
        list_rebuild();
    }
}

/* ---------------------------------------------------------------------------
 *  视图切换（把"可见性 + 文案 + 数据"一次性摆正）
 * ------------------------------------------------------------------------- */
static void view_apply(void)
{
    if (s_list == NULL) return;

    bool rec = (s_view == VIEW_RECORDS);

    /* 第二行工具条、页码容器只在 RECORDS 出现。
     * ★ HIDDEN 的子对象**不参与 flex 布局**（LVGL 的 flex 会跳过 hidden/ignore_layout
     *   的子项），所以 PERSONS 下那 48 px 会自动让给列表 —— 不需要两套高度常量。 */
    if (s_bar2)     lv_obj_set_hidden(s_bar2, !rec);
    if (s_pager)    lv_obj_set_hidden(s_pager, !rec);

    if (s_btn_view_lab) {
        ui_common_set_label_text_if_changed(s_btn_view_lab,
                                            rec ? "PERSONS" : "RECORDS");
    }
    if (s_btn_group_lab) {
        ui_common_set_label_text_if_changed(s_btn_group_lab,
                                            s_sort == SORT_ASC ? "OLD FIRST" : "NEW FIRST");
    }

    /* 离开 RECORDS 时把预览关掉：覆盖层是全屏的，留着会挡住 PERSONS 列表。
     * ★ §73：同时归还 viewfinder 的解码缓冲（~2.93 MB）—— 预览不再看，
     *   内存该还就还（release 幂等，没持有也只是空操作）。 */
    if (!rec && s_pv_ov) {
        lv_obj_set_hidden(s_pv_ov, true);
        s_pv_pending = false;
        if (s_pv_held) { viewfinder_release(); s_pv_held = false; }
    }

    /* ★ 必须在这里也摆一次 CLEAR 按钮：一是进入本页时把三态缓存复位后的
     *   首次外观落地，二是"上次退出时按钮停在 SURE?"那一类残留的兜底。 */
    clear_btn_apply();

    if (rec) {
        records_load();
        rec_chrome_apply();
    } else {
        /* ★ 复位快照，强制重建一次 —— 否则回来时（count 与上次相同 ⇒ 不满足
         *   "变化"条件）列表会**空白**：对象已随上次 clean 释放，静态变量还记着旧值。 */
        s_last_count = -1;
        s_last_ready = false;
    }
    page_refresh(true);
}

/* ---------------------------------------------------------------------------
 *  回调：人脸库
 * ------------------------------------------------------------------------- */
/**
 * 删除按钮。
 *
 * ★★ 这里**不能**直接调 list_rebuild()：它第一件事就是 lv_obj_clean(s_list)，
 *   而本回调**正运行在被清理的子对象（del 按钮）上**。在事件派发中途销毁自己，
 *   轻则 LVGL 内部状态错乱、重则 free 后继续访问（UAF）。
 *   ⇒ 改用 lv_async_call()：LVGL 会在下一轮 lv_timer_handler 里、**事件已经返回之后**
 *     才执行它，此时销毁谁都是安全的。
 */
static void del_btn_cb(lv_event_t *e)
{
    uint16_t  id  = (uint16_t)(uintptr_t)lv_event_get_user_data(e);

    /* §73.25：删人前先取姓名（删完就查不到了）——打卡目录名/CSV 前缀都要用 */
    face_person_t p;
    char name[FACE_NAME_MAX] = "";
    if (face_db_find_by_id(id, &p) == ESP_OK) {
        snprintf(name, sizeof(name), "%s", p.name);
    }

    esp_err_t err = face_db_delete(id);

    if (err != ESP_OK) {
        face_db_info_t info;
        face_db_get_info(&info);
        /* 库不可用（拔卡）才是最可能的成因，直接说清楚，别只给一个错误码 */
        hint_set((err == ESP_ERR_INVALID_STATE && !info.ready)
                     ? "Delete failed: face db unavailable" : "Delete failed");
        ESP_LOGW(TAG, "delete id=%u rc=%s", (unsigned)id, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "deleted id=%u", (unsigned)id);
    /* §73.25：打卡目录（照片）+ 该人全部流水行同步清理（异步一次性任务）。
     * 失败只记日志不拦库删除 —— 残留可在 FILES 页对账/清空兜底。 */
    if (name[0] != '\0') {
        esp_err_t pe = punch_service_purge_person(id, name);
        if (pe != ESP_OK) {
            ESP_LOGW(TAG, "purge attendance of id=%u rc=%s (residual ok, FILES 可对账)",
                     (unsigned)id, esp_err_to_name(pe));
        }
    }
    lv_async_call(async_refresh_cb, NULL);          /* ← 延后一拍重建，原因见上 */
}

/* ---------------------------------------------------------------------------
 *  回调：视图 / 分组 / 分页 / 清空（S8-4）
 * ------------------------------------------------------------------------- */
static void view_btn_cb(lv_event_t *e)
{
    (void)e;
    s_view = (s_view == VIEW_RECORDS) ? VIEW_PERSONS : VIEW_RECORDS;
    /* ★ 用 async_call 而不是直接改：本回调跑在按钮上，而 view_apply() 会
     *   lv_obj_clean(s_list) —— 虽然按钮不在 s_list 里（安全），但保持
     *   "从事件回调里改整棵树一律延后一拍"这条纪律，省得以后有人往 s_list
     *   里加控件时踩到 UAF。 */
    lv_async_call(async_refresh_cb, NULL);
}

static void group_btn_cb(lv_event_t *e)
{
    (void)e;
    /* ★ §73.11：切换时间排序方向（NEW FIRST <-> OLD FIRST） */
    s_sort = (s_sort == SORT_ASC) ? SORT_DESC : SORT_ASC;
    if (s_rec_n > 1) qsort(s_rec, (size_t)s_rec_n, sizeof(s_rec[0]), rec_cmp);
    s_rec_page = 0;
    lv_async_call(async_refresh_cb, NULL);
}

static void prev_page_cb(lv_event_t *e)
{
    (void)e;
    if (s_rec_page > 0) s_rec_page--;
    rec_chrome_apply();
    lv_async_call(async_refresh_cb, NULL);
}

static void next_page_cb(lv_event_t *e)
{
    (void)e;
    int pages = (s_rec_n + REC_PAGE_SIZE - 1) / REC_PAGE_SIZE;
    if (s_rec_page < pages - 1) s_rec_page++;
    rec_chrome_apply();
    lv_async_call(async_refresh_cb, NULL);
}

static void async_refresh_cb(void *arg)
{
    (void)arg;
    view_apply();       /* 可见性 + 文案 + 重建，全在异步上下文里一次做掉 */
}

/** 两步确认按钮的外观同步（每拍都调，但只在**状态真的变了**时才动样式） */
static void clear_btn_apply(void)
{
    if (!s_btn_clear || !s_btn_clear_lab) return;

    int want = s_clr_armed ? 1 : 0;
    if (want == s_clr_applied) return;
    s_clr_applied = want;

    if (s_clr_armed) {
        ui_common_set_label_text_if_changed(s_btn_clear_lab, LV_SYMBOL_WARNING " SURE?");
        lv_obj_set_style_bg_color(s_btn_clear, COL_RED, 0);
    } else {
        ui_common_set_label_text_if_changed(s_btn_clear_lab, LV_SYMBOL_TRASH " CLEAR");
        lv_obj_set_style_bg_color(s_btn_clear, COL_BORDER, 0);
    }
}

/**
 * CLEAR：两步确认（同 tab_files 的 DELETE）。
 *
 * ★★ 红线：`punch_service_log_clear()` **只动 /sdcard/attendance**，
 *   绝不碰 /sdcard/face_db —— 这是本页最危险的一个按钮，也是
 *   "清空打卡记录不影响人脸库"这条验收项的落点。
 * ★ 它是**异步**的（要逐个 unlink 几百个文件，不能让 LVGL 线程等），
 *   所以这里只负责"发起 + 记下 pending"，结论在 face_poll_cb() 里收。
 */
static void clear_btn_cb(lv_event_t *e)
{
    (void)e;

    if (!s_clr_armed) {
        s_clr_armed = true;
        s_clr_arm_ticks = CLR_ARM_TICKS;
        clear_btn_apply();
        return;
    }

    s_clr_armed = false;
    clear_btn_apply();

    esp_err_t err = punch_service_log_clear();
    if (err != ESP_OK) {
        hint_set("Clear failed: attendance is busy, try again");
        ESP_LOGW(TAG, "log_clear rc=%s", esp_err_to_name(err));
        return;
    }
    s_clr_pending = true;
    s_clr_ticks   = 0;
    hint_set("Clearing attendance records...");
}

/* ---------------------------------------------------------------------------
 *  ★ 2026-09-28：行内删除 + 照片对账（用户反馈的两个 BUG）
 * ------------------------------------------------------------------------- */
/**
 * 行内删除 —— **两步确认**（与 CLEAR 同款，但状态按 seq 记）。
 * ★ 为什么不把按钮文案换成 "SURE?"：列表行随时会被 records_load()/rebuild
 *   重建（3 s 轮询就可能触发），在旧按钮对象上改文案 = 悬垂指针；
 *   而 hint 条是常驻对象，提示放那里最稳。
 * ★ 删除是**异步**的（punch 任务重写 CSV），结论在 face_poll_cb() 里收。
 */
static void del_rec_btn_cb(lv_event_t *e)
{
    int seq = (int)(uintptr_t)lv_event_get_user_data(e);
    if (seq <= 0) return;

    /* 第一按：只武装。换一条按则把武装转移到新 seq（符合直觉） */
    if (s_del_armed_seq != seq) {
        s_del_armed_seq = seq;
        s_del_arm_ticks = CLR_ARM_TICKS;
        char buf[64];
        snprintf(buf, sizeof(buf), "Tap the trash again to delete #%d", seq);
        hint_set(buf);
        return;
    }

    /* 第二按：真的删 */
    s_del_armed_seq = 0;
    if (punch_service_busy()) {
        hint_set("Attendance busy - try again");
        return;
    }
    esp_err_t err = punch_service_log_delete(seq);
    if (err != ESP_OK) {
        hint_set("Delete failed: busy or no SD card");
        ESP_LOGW(TAG, "log_delete(seq=%d) rc=%s", seq, esp_err_to_name(err));
        return;
    }
    s_del_pending    = true;
    s_del_ticks      = 0;
    s_del_target_seq = seq;
    hint_set("Deleting record...");
}

/** SYNC：把"照片已经被手动删掉"的死条目从流水里清掉 */
static void sync_btn_cb(lv_event_t *e)
{
    (void)e;
    if (punch_service_busy()) {
        hint_set("Attendance busy - try again");
        return;
    }
    esp_err_t err = punch_service_log_sync();
    if (err != ESP_OK) {
        hint_set("Sync failed: busy or no SD card");
        ESP_LOGW(TAG, "log_sync rc=%s", esp_err_to_name(err));
        return;
    }
    s_sync_pending = true;
    s_sync_ticks   = 0;
    hint_set("Syncing records with photos...");
}

/* ---------------------------------------------------------------------------
 *  回调：照片预览（S8-4）
 * ------------------------------------------------------------------------- */
/**
 * file_mgr 事件回调 —— ★★ 运行在 **file_mgr 服务任务**上下文！
 * 这里只允许写 volatile 标量，**绝不能碰 LVGL**（会 assert / 崩）。
 * 结果由 face_poll_cb() 在 LVGL 线程里取走。
 */
static void face_file_evt_cb(const file_mgr_evt_t *evt, void *user)
{
    (void)user;
    if (evt == NULL) return;
    switch (evt->id) {
    case FME_PREVIEW_DONE:
    case FME_BUSY:
        /* 只在"我们正等"的时候记：否则会把别处（FILES 页）的操作结果记进来 */
        if (s_pv_pending) s_pv_result = (int)evt->a;
        break;
    default:
        break;
    }
}

static void preview_request(int idx)
{
    if (idx < 0 || idx >= s_rec_n) return;

    const punch_log_t *r = &s_rec[idx];

    /* 相对路径 → 绝对路径。
     * ★ 相对路径由 punch_service 写入，也已由它做过安全化；这里只拼根。
     * ⚠ 缓冲必须用 FM_PATH_MAX（file_mgr 的 cmd.path 容量），不是我们自己的
     *   PUNCH_PATH_MAX —— 超长时 file_mgr 侧会静默截断成另一个路径。 */
    char abs[FM_PATH_MAX];
    int n = snprintf(abs, sizeof(abs), "%s%s/%s", SDCARD_MNT_PATH, PUNCH_DIR_NAME, r->rel);
    if (n <= 0 || n >= (int)sizeof(abs)) {
        hint_set("Preview failed: path too long");
        return;
    }

    /* 覆盖层的说明行先摆好（姓名与数字分属两个标签，原因见 s_pv_cap 的声明处） */
    if (s_pv_cap) {
        ui_common_set_label_text_if_changed(s_pv_cap, r->name[0] ? r->name : "?");
    }
    if (s_pv_meta) {
        char meta[64];
        snprintf(meta, sizeof(meta), "#%d   %s %s", r->seq, r->date, r->time);
        ui_common_set_label_text_if_changed(s_pv_meta, meta);
    }
    if (s_pv_stat) {
        ui_common_set_label_text_if_changed(s_pv_stat, "Loading...");
    }

    /* ★★ §73 修复（用户报"显示失败：ESP_ERR_NO_MEM"）：
     *   FMC_PREVIEW 的 decode 跑在 files 任务里，用的是 viewfinder 的共享
     *   staging（921,600 B）与解码引擎 —— 那是**纯 getter**，不会自己 init。
     *   旧版没 acquire 就 post ⇒ files 任务里 borrow_stage() 拿到 NULL ⇒
     *   do_preview 报 ESP_ERR_NO_MEM。必须在 LVGL 线程、post **之前** acquire
     *  （幂等：已持有则直接 OK，多次预览不会反复 alloc/free 2.93 MB）。
     *   归还点：离开 RECORDS 视图 / 销毁页面（见 view_apply 与 face_destroy）。 */
    if (viewfinder_acquire() == ESP_OK) {
        s_pv_held = true;
    } else {
        hint_set("Preview failed: decode buffer in use (camera on?)");
        return;
    }

    file_mgr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id = FMC_PREVIEW;
    if (!path_copy(cmd.path, sizeof(cmd.path), abs)) {
        hint_set("Preview failed: path too long");
        return;
    }

    s_pv_result  = -2;
    s_pv_pending = true;

    esp_err_t err = file_mgr_post_cmd(&cmd);
    if (err != ESP_OK) {
        s_pv_pending = false;
        hint_set("Preview failed: file manager busy");
        return;
    }

    /* ★ 先显示覆盖层（"Loading..." 已经在上面写好）：让用户立刻得到反馈，
     *   而不是盯着列表发呆 —— 解码要几十毫秒，异步回来还要等一拍轮询。 */
    if (s_pv_ov) lv_obj_set_hidden(s_pv_ov, false);
}

static void prev_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    preview_request(idx);
}

static void prev_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_pv_ov) lv_obj_set_hidden(s_pv_ov, true);
    s_pv_pending = false;
}

/** 预览结果收尾（LVGL 线程）：OK 就把画面挂到 canvas 上 */
static void preview_pump(void)
{
    if (!s_pv_pending || s_pv_result == -2) return;

    int res = s_pv_result;
    s_pv_result  = -2;
    s_pv_pending = false;

    if (res != (int)ESP_OK) {
        /* ★ 把 esp_err 名字直接写出来：最常见的几个是
         *   ESP_ERR_INVALID_STATE（取景器/录像占着共享缓冲）、
         *   ESP_ERR_NOT_FOUND（照片被单独删了 ⇒ 引导去按 SYNC 清死条目）、
         *   ESP_ERR_INVALID_SIZE（外来大图超了解码缓冲）——提示必须能区分。 */
        char msg[96];
        if (res == (int)ESP_ERR_NOT_FOUND) {
            snprintf(msg, sizeof(msg), "Photo missing - tap SYNC to clean records");
        } else {
            snprintf(msg, sizeof(msg), "Preview failed: %s", esp_err_to_name((esp_err_t)res));
        }
        if (s_pv_stat) ui_common_set_label_text_if_changed(s_pv_stat, msg);
        ESP_LOGW(TAG, "preview rc=%s", esp_err_to_name((esp_err_t)res));
        return;
    }

    file_mgr_frame_t f;
    if (!file_mgr_preview_take(&f)) {
        if (s_pv_stat) ui_common_set_label_text_if_changed(s_pv_stat, "Preview failed: no frame");
        return;
    }

    /* ★ 直接把 canvas 的缓冲指到 file_mgr 的双缓冲上（零拷贝，同 tab_files:456）：
     *   FM_W×FM_H×2 = 460 KB —— 再拷一份纯属浪费 PSRAM。
     *   ⚠ 不要 lv_canvas_set_buffer 之后又 lv_obj_set_size：尺寸由 set_buffer 定。 */
    lv_canvas_set_buffer(s_pv_canvas, (void *)f.buf, (int32_t)f.w, (int32_t)f.h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(s_pv_canvas);
    if (s_pv_stat) ui_common_set_label_text_if_changed(s_pv_stat, "Tap CLOSE to return");
}

/* ---------------------------------------------------------------------------
 *  RETRY
 * ------------------------------------------------------------------------- */
/** RETRY：重挂载 SD + 重 init 库。
 *  ★ 两条都是幂等调用（sdcard_service_mount 内有 s_mounted 短路；face_db_init 在
 *    "已就绪且根目录没变"时直接返回 OK），所以这个按钮可以随便按。 */
static void retry_btn_cb(lv_event_t *e)
{
    (void)e;

    if (sdcard_service_mount() != ESP_OK) {
        hint_set("SD mount failed - check the card");
        return;
    }
    face_db_init();             /* 看结果不看返回码 —— 状态统一走 get_info/is_ready */
    if (s_view == VIEW_RECORDS) {
        records_load();
        rec_chrome_apply();
    }
    page_refresh(true);         /* force：无论状态变没变都重建一次（成败都要立刻反映） */
}

/* ---------------------------------------------------------------------------
 *  轮询（300 ms）：把异步结果收进来 + 库状态对账
 * ------------------------------------------------------------------------- */
static void face_poll_cb(lv_timer_t *t)
{
    (void)t;

    /* ---- ① 预览结果收尾（每次都查，预览是"用户正等着"的那件事） ---- */
    preview_pump();

    /* ---- ② 清空结果收尾 ---- */
    if (s_clr_pending) {
        s_clr_ticks++;
        if (!punch_service_busy() || s_clr_ticks > REC_CLEAR_TIMEOUT_TICKS) {
            bool timeout = (s_clr_ticks > REC_CLEAR_TIMEOUT_TICKS);
            s_clr_pending = false;
            /* ★ 不去抢 punch_service_get_last()：那个槽是 tab_camera 的下游
             *   （它在脚下一刻不停地轮询打卡结果），两边都"读完就清"必然互相偷走。
             *   这里换个判据：重读一遍 CSV，还是空的就说明清干净了 —— 简单且自证。 */
            records_load();
            rec_chrome_apply();
            /* ★ 必须按当前视图重建：清空是异步的，用户完全可能在等待期间切回了
             *   PERSONS —— 那时若无条件调 records_rebuild()，人脸库列表会被
             *   **打卡记录**顶掉（而 s_list 只有一份，正是本文件最危险的耦合点）。 */
            if (s_view == VIEW_RECORDS) records_rebuild();
            else                        page_refresh(true);
            if (timeout)                            hint_set("Clear timed out - check the card");
            else if (s_rec_n > 0)                   hint_set("Clear failed - some records remain");
            else                                    hint_set("Attendance records cleared");
            ESP_LOGI(TAG, "clear finished: %d records left%s",
                     s_rec_n, timeout ? " (timeout)" : "");
        }
    }

    /* ---- ③ 两步确认倒计时还原 ---- */
    if (s_clr_armed && --s_clr_arm_ticks <= 0) {
        s_clr_armed = false;
        clear_btn_apply();
    }
    /* ★ 2026-09-28：行内删除的武装也有 3 s 保质期 —— 过期作废，防"误按一下、
     *   半分钟后手滑补一下"把不相干的记录删了。 */
    if (s_del_armed_seq > 0 && --s_del_arm_ticks <= 0) {
        s_del_armed_seq = 0;
    }

    /* ---- ③b 行内删除 / 照片对账的结果收尾（2026-09-28 新增）----
     *  收尾动作与 ② 完全同款（重读 CSV + 按当前视图重建）。
     *  ★ 不去抢 punch_service_get_last()（槽是 CAM 页的下游，见 ② 的注释），
     *    结论以"重读后 CSV 的现状"自证 —— 简单且不会被别的模块偷走。 */
    if (s_del_pending || s_sync_pending) {
        bool was_del = s_del_pending;
        int  *ticks  = was_del ? &s_del_ticks : &s_sync_ticks;
        (*ticks)++;
        if (!punch_service_busy() || *ticks > REC_CLEAR_TIMEOUT_TICKS) {
            bool timeout = (*ticks > REC_CLEAR_TIMEOUT_TICKS);
            s_del_pending  = false;
            s_sync_pending = false;
            *ticks = 0;
            records_load();
            rec_chrome_apply();
            if (s_view == VIEW_RECORDS) records_rebuild();
            if (timeout) {
                hint_set("Operation timed out - check the card");
            } else if (was_del) {
                /* 目标行还在 = 删失败（CSV 写不进去？），如实说 */
                bool still_there = false;
                for (int i = 0; i < s_rec_n; i++) {
                    if (s_rec[i].seq == s_del_target_seq) { still_there = true; break; }
                }
                hint_set(still_there ? "Delete failed - record remains" : "Record deleted");
            } else {
                hint_set("Sync finished");
            }
            ESP_LOGI(TAG, "%s finished: %d records now%s",
                     was_del ? "delete" : "sync", s_rec_n, timeout ? " (timeout)" : "");
        }
    }

    /* ---- ④ 人脸库状态对账（PERSONS 视图） ---- */
    if (s_view == VIEW_PERSONS) {
        page_refresh(false);
        return;
    }

    /* ---- ⑤ 记录页：3 s 重读一次 CSV（预览开着时**不**重读，见文件头纪律②） ---- */
    if (!s_clr_pending && !s_pv_pending && !punch_service_busy()) {
        if (++s_rec_ticks >= REC_RESCAN_TICKS) {
            s_rec_ticks = 0;
            int before = s_rec_n;
            records_load();
            rec_chrome_apply();
            /* ★ 只在"条数变了"时重建：每 3 s 无条件 lv_obj_clean + 重建 20 行
             *   是纯粹的浪费（对象分配 + 全屏重绘），而 99% 的时候什么都没变。 */
            if (s_rec_n != before) records_rebuild();
            rec_info_refresh();
        }
    }
}

/* ---------------------------------------------------------------------------
 *  页面生命周期
 * ------------------------------------------------------------------------- */
static lv_obj_t *face_create(lv_obj_t *parent)
{
    /* ★ 幂等且非致命：开机时 app_main 已经 init 过一次，这里是**兜底**
     *   （万一那次失败——比如当时没插卡——这里再试一次不亏）。
     *   绝不能因为返回错误就不渲染：本页必须能在"库不可用"下正常工作。 */
    face_db_init();
    punch_service_init();       /* ★ S8-4：幂等，不分配大缓冲 */

    lv_obj_t *page = ui_common_page_root(parent);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_style_pad_top(page, 8, 0);

    ui_common_title_bar(page, LV_SYMBOL_LIST"  FACE DB");

    /* ---- 第一行：状态文本（左） + RETRY + 视图切换（右） ---- */
    lv_obj_t *hdr = lv_obj_create(page);
    lv_obj_set_size(hdr, BSP_LCD_H_RES - 40, 48);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_scrollable(hdr, false);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);

    s_info = lv_label_create(hdr);
    lv_label_set_text_static(s_info, "...");
    lv_obj_set_style_text_color(s_info, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_info, UI_FONT, 0);

    s_btn_view = lv_btn_create(hdr);
    lv_obj_set_size(s_btn_view, 150, 36);
    lv_obj_set_style_radius(s_btn_view, 8, 0);
    lv_obj_set_style_bg_color(s_btn_view, COL_BORDER, 0);
    lv_obj_add_event_cb(s_btn_view, view_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_view_lab = lv_label_create(s_btn_view);
    lv_label_set_text_static(s_btn_view_lab, "RECORDS");
    lv_obj_set_style_text_color(s_btn_view_lab, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_btn_view_lab, UI_FONT, 0);
    lv_obj_center(s_btn_view_lab);

    lv_obj_t *retry = lv_btn_create(hdr);
    lv_obj_set_size(retry, 120, 36);
    lv_obj_set_style_radius(retry, 8, 0);
    lv_obj_set_style_bg_color(retry, COL_CYAN, 0);
    lv_obj_add_event_cb(retry, retry_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(retry);
    lv_label_set_text_static(rl, LV_SYMBOL_REFRESH"  RETRY");
    lv_obj_set_style_text_color(rl, COL_DARK, 0);
    lv_obj_set_style_text_font(rl, UI_FONT, 0);
    lv_obj_center(rl);

    /* ---- 第二行：BY DATE/BY PERSON + CLEAR（仅 RECORDS 可见） ---- */
    s_bar2 = lv_obj_create(page);
    lv_obj_set_size(s_bar2, BSP_LCD_H_RES - 40, 48);
    lv_obj_set_style_bg_opa(s_bar2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bar2, 0, 0);
    lv_obj_set_style_pad_all(s_bar2, 0, 0);
    lv_obj_set_scrollable(s_bar2, false);
    lv_obj_set_flex_flow(s_bar2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar2, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_btn_group = lv_btn_create(s_bar2);
    lv_obj_set_size(s_btn_group, 150, 36);
    lv_obj_set_style_radius(s_btn_group, 8, 0);
    lv_obj_set_style_bg_color(s_btn_group, COL_CARD, 0);
    lv_obj_add_event_cb(s_btn_group, group_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_group_lab = lv_label_create(s_btn_group);
    lv_label_set_text_static(s_btn_group_lab, "NEW FIRST");
    lv_obj_set_style_text_color(s_btn_group_lab, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_btn_group_lab, UI_FONT, 0);
    lv_obj_center(s_btn_group_lab);

    /* ★ 2026-09-28：SYNC —— 在 FILES 页单独删过照片后，把记录流水里
     *   "照片已不存在"的死条目清掉（异步，见 punch_service_log_sync）。 */
    s_btn_sync = lv_btn_create(s_bar2);
    lv_obj_set_size(s_btn_sync, 120, 36);
    lv_obj_set_style_radius(s_btn_sync, 8, 0);
    lv_obj_set_style_bg_color(s_btn_sync, COL_CARD, 0);
    lv_obj_add_event_cb(s_btn_sync, sync_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_sync_lab = lv_label_create(s_btn_sync);
    lv_label_set_text_static(s_btn_sync_lab, LV_SYMBOL_REFRESH " SYNC");
    lv_obj_set_style_text_color(s_btn_sync_lab, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_btn_sync_lab, UI_FONT, 0);
    lv_obj_center(s_btn_sync_lab);

    s_btn_clear = lv_btn_create(s_bar2);
    lv_obj_set_size(s_btn_clear, 150, 36);
    lv_obj_set_style_radius(s_btn_clear, 8, 0);
    lv_obj_set_style_bg_color(s_btn_clear, COL_BORDER, 0);
    lv_obj_add_event_cb(s_btn_clear, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_clear_lab = lv_label_create(s_btn_clear);
    lv_label_set_text_static(s_btn_clear_lab, LV_SYMBOL_TRASH " CLEAR");
    lv_obj_set_style_text_color(s_btn_clear_lab, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_btn_clear_lab, UI_FONT, 0);
    lv_obj_center(s_btn_clear_lab);

    /* ---- 列表（两种视图共用） ---- */
    s_list = lv_obj_create(page);
    lv_obj_set_size(s_list, BSP_LCD_H_RES - 40, FACE_LIST_H);
    lv_obj_set_style_bg_color(s_list, COL_DARK, 0);
    lv_obj_set_style_border_color(s_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 10, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);

    /* ---- 底行：提示文本（左，占满剩余宽度） + 页码（右，仅 RECORDS） ---- */
    lv_obj_t *bot = lv_obj_create(page);
    lv_obj_set_size(bot, BSP_LCD_H_RES - 40, BOTTOM_H);
    lv_obj_set_style_bg_opa(bot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_set_scrollable(bot, false);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bot, 10, 0);

    s_hint = lv_label_create(bot);
    lv_label_set_text_static(s_hint, "");
    lv_obj_set_style_text_color(s_hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT, 0);
    lv_obj_set_hidden(s_hint, true);

    s_pager = lv_obj_create(bot);
    lv_obj_set_size(s_pager, 190, BOTTOM_H);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_scrollable(s_pager, false);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pager, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_pager, 8, 0);

    s_btn_prev = lv_btn_create(s_pager);
    lv_obj_set_size(s_btn_prev, 40, 32);
    lv_obj_set_style_radius(s_btn_prev, 8, 0);
    lv_obj_set_style_bg_color(s_btn_prev, COL_CARD, 0);
    lv_obj_add_event_cb(s_btn_prev, prev_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(s_btn_prev);
    lv_label_set_text_static(pl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(pl, COL_TEXT, 0);
    lv_obj_set_style_text_font(pl, UI_FONT, 0);
    lv_obj_center(pl);

    s_pager_lab = lv_label_create(s_pager);
    lv_label_set_text_static(s_pager_lab, "1/1");
    lv_obj_set_style_text_color(s_pager_lab, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_pager_lab, UI_FONT, 0);

    s_btn_next = lv_btn_create(s_pager);
    lv_obj_set_size(s_btn_next, 40, 32);
    lv_obj_set_style_radius(s_btn_next, 8, 0);
    lv_obj_set_style_bg_color(s_btn_next, COL_CARD, 0);
    lv_obj_add_event_cb(s_btn_next, next_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(s_btn_next);
    lv_label_set_text_static(nl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nl, COL_TEXT, 0);
    lv_obj_set_style_text_font(nl, UI_FONT, 0);
    lv_obj_center(nl);

    /* ---- 预览覆盖层（全屏，平时 HIDDEN） ---- */
    s_pv_ov = lv_obj_create(page);
    lv_obj_set_size(s_pv_ov, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_pv_ov, COL_DARK, 0);
    lv_obj_set_style_bg_opa(s_pv_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pv_ov, 0, 0);
    lv_obj_set_style_radius(s_pv_ov, 0, 0);
    lv_obj_set_style_pad_all(s_pv_ov, 0, 0);
    lv_obj_set_scrollable(s_pv_ov, false);
    /* ★ 覆盖层只能用绝对定位（它要盖住整页，不能参与 column flex 的排布）
     *   ⇒ IGNORE_LAYOUT + 手动 set_pos。 */
    lv_obj_set_ignore_layout(s_pv_ov, true);
    lv_obj_set_pos(s_pv_ov, 0, 0);

    /* 说明行容器（透明、不参与布局 —— 覆盖层里所有东西都绝对定位） */
    lv_obj_t *caprow = lv_obj_create(s_pv_ov);
    lv_obj_set_size(caprow, BSP_LCD_H_RES - 140 - 32 - 12, 44);
    lv_obj_set_style_bg_opa(caprow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(caprow, 0, 0);
    lv_obj_set_style_pad_all(caprow, 0, 0);
    lv_obj_set_scrollable(caprow, false);
    lv_obj_set_ignore_layout(caprow, true);
    lv_obj_set_flex_flow(caprow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(caprow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(caprow, 10, 0);
    lv_obj_set_pos(caprow, 16, 14);

    s_pv_cap = lv_label_create(caprow);
    lv_label_set_text_static(s_pv_cap, "");
    lv_obj_set_style_text_color(s_pv_cap, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_pv_cap, cjk_font(), 0);

    s_pv_meta = lv_label_create(caprow);
    lv_label_set_text_static(s_pv_meta, "");
    lv_obj_set_style_text_color(s_pv_meta, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_pv_meta, UI_FONT, 0);

    lv_obj_t *pclose = lv_btn_create(s_pv_ov);
    lv_obj_set_size(pclose, 140, 44);
    lv_obj_set_style_radius(pclose, 8, 0);
    lv_obj_set_style_bg_color(pclose, COL_CYAN, 0);
    lv_obj_add_event_cb(pclose, prev_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_pos(pclose, BSP_LCD_H_RES - 140 - 16, 14);
    lv_obj_t *cl = lv_label_create(pclose);
    lv_label_set_text_static(cl, LV_SYMBOL_CLOSE "  CLOSE");
    lv_obj_set_style_text_color(cl, COL_DARK, 0);
    lv_obj_set_style_text_font(cl, UI_FONT, 0);
    lv_obj_center(cl);

    /* 画布：位置固定，尺寸由 lv_canvas_set_buffer() 决定（FM_W×FM_H = 640×360）。
     * ★ 必须先给一个 1×1 的合法缓冲：`lv_canvas_create()` 本身**不带** draw buffer，
     *   而覆盖层可能先于解码结果被显示出来 ⇒ 没有这块占位就是一次空指针解引用。 */
    s_pv_canvas = lv_canvas_create(s_pv_ov);
    lv_canvas_set_buffer(s_pv_canvas, &s_pv_dummy, 1, 1, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_pv_canvas, (BSP_LCD_H_RES - (int)FM_W) / 2, 70);

    s_pv_stat = lv_label_create(s_pv_ov);
    lv_label_set_text_static(s_pv_stat, "");
    lv_obj_set_style_text_color(s_pv_stat, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_pv_stat, UI_FONT, 0);
    lv_obj_set_pos(s_pv_stat, 80, BSP_LCD_V_RES - 40);

    lv_obj_set_hidden(s_pv_ov, true);

    /* ---- 记录缓存（PSRAM）。★ 分配失败不是致命错误：记录页会显示一条
     *      "Records unavailable" 并让库管理照常工作，而不是整页黑屏。
     *
     *      ★★ 只在**第一次**进本页时分配，之后**永不释放**（幂等）。
     *      为什么不随页面销毁而 free：本工程的 PSRAM 策略是"开机一次性分配、
     *      运行期不再切"（viewfinder / file_mgr 的双缓冲都是这个规矩）——
     *      反复 malloc/free 一块 15 KB 正是"留碎片"的典型来源，而我们后面还要
     *      加载 1.3 MB 的 MFN，它需要大块连续空间。 ---- */
    if (s_rec == NULL) {
        s_rec = (punch_log_t *)heap_caps_malloc(sizeof(punch_log_t) * PUNCH_LOG_MAX,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rec == NULL) {
            ESP_LOGW(TAG, "record buffer alloc failed (%u B), records view degraded",
                     (unsigned)(sizeof(punch_log_t) * PUNCH_LOG_MAX));
        } else {
            ESP_LOGI(TAG, "record buffer ready: %u entries x %u B (PSRAM)",
                     (unsigned)PUNCH_LOG_MAX, (unsigned)sizeof(punch_log_t));
        }
    }

    /* ---- 借调 file_mgr 的事件回调槽：先存原主，再换上我们的 ---- */
    file_mgr_get_evt_cb(&s_saved_cb, &s_saved_user);
    file_mgr_set_evt_cb(face_file_evt_cb, NULL);

    /* 复位视图状态（本页可能被反复进出，静态变量必须当"全新"用） */
    s_view       = VIEW_PERSONS;
    s_sort       = SORT_DESC;   /* ★ §73.11：时间排序方向，默认"新的在上" */
    s_rec_page   = 0;
    s_rec_n      = 0;
    s_rec_ticks  = 0;
    s_clr_armed  = false;
    s_clr_applied   = -1;       /* 新建的按钮是普通态，缓存必须跟着复位 */
    s_clr_arm_ticks = 0;
    s_clr_pending   = false;
    s_clr_ticks     = 0;
    /* ★ 2026-09-28：行内删除/对账的状态一并复位（页面可能反复进出） */
    s_del_pending    = false;
    s_del_ticks      = 0;
    s_sync_pending   = false;
    s_sync_ticks     = 0;
    s_del_armed_seq  = 0;
    s_del_arm_ticks  = 0;
    s_del_target_seq = 0;
    s_pv_held        = false;   /* §73：新页面从"未持有解码缓冲"开始 */
    s_pv_pending    = false;
    s_pv_result     = -2;
    s_last_count = -1;
    s_last_ready = false;

    view_apply();       /* 摆正可见性/文案 + 重建列表 */

    s_timer = lv_timer_create(face_poll_cb, REC_POLL_MS, NULL);

    return page;
}

static void face_destroy(lv_obj_t *page)
{
    /* ★ 必须先取消挂起的异步回调：它下一轮会去摸 s_list，
     *   而下面的 lv_obj_del 会把那棵对象树全部释放 ⇒ 晚到一拍就是 UAF。 */
    lv_async_call_cancel(async_refresh_cb, NULL);

    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    /* ★ 归还 file_mgr 的回调槽。不做这一步，FILES 页从本页返回后会**彻底聋掉**
     *   （事件发出来没人接，表现成"点了没反应"）。 */
    file_mgr_set_evt_cb(s_saved_cb, s_saved_user);
    s_saved_cb   = NULL;
    s_saved_user = NULL;

    lv_obj_del(page);

    s_info = s_list = s_hint = s_bar2 = NULL;
    s_btn_view = s_btn_view_lab = NULL;
    s_btn_group = s_btn_group_lab = NULL;
    s_btn_clear = s_btn_clear_lab = NULL;
    s_btn_sync  = s_btn_sync_lab  = NULL;   /* ★ 2026-09-28 */
    s_pager = s_pager_lab = s_btn_prev = s_btn_next = NULL;
    s_pv_ov = s_pv_canvas = s_pv_cap = s_pv_meta = s_pv_stat = NULL;
    /* ★ §73：页面销毁时归还 viewfinder 解码缓冲（预览可能正开着就退出） */
    if (s_pv_held) { viewfinder_release(); s_pv_held = false; }

    /* ★ s_rec **故意不释放**：见 face_create() 里那段"只在第一次分配、之后永不
     *   free"的理由（PSRAM 碎片 vs 后面要加载的 1.3 MB MFN）。 */

    s_last_count = -1;
    s_last_ready = false;
    s_rec_n      = 0;
    s_rec_page   = 0;
    s_view       = VIEW_PERSONS;
    s_clr_armed  = false;
    s_clr_pending = false;
    s_pv_pending  = false;
    s_pv_result   = -2;
    /* ★ 2026-09-28：两个维护操作的 pending 也要清 —— 页面销毁后没有 poll 来收尾，
     *   留着 true 的话下次进页会立刻误判"有结果要收"。任务本身照跑不误（punch
     *   任务不依赖本页），下次进页重读 CSV 就能看到它的最终效果。 */
    s_del_pending    = false;
    s_sync_pending   = false;
    s_del_armed_seq  = 0;
    s_del_target_seq = 0;
}

const Page_t Page_Face = {
    .create  = face_create,
    .destroy = face_destroy,
    .name    = "Face",
};
