/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_pcnow.c
 * @brief PC 副屏 Tab：局域网 PC（网易云/QQ音乐/foobar…）把"正在播放"
 *        推到本机当副屏。布局模仿 tab_btsink：左侧圆形封面（方案 B 旋转）、
 *        右侧曲名/歌手/进度，下方三行歌词窗，**媒体按键竖排在右手边**。
 *
 * ── 布局（2026-09-24 改版 / F47）─────────────────────────────────────
 *   ┌──────────────────────────────────────────┬────────┐
 *   │ ● PC NOW                         [同步]  │  ⏮  上 │  ← 顶栏下移，让开 tab 栏
 *   │  ┌────┐   曲  名                         │  ⏯  中 │
 *   │  │封面│   歌  手                         │  ⏭  下 │
 *   │  └────┘   ────滑条────  00:00 / 03:20    │        │
 *   │  上 一 行歌词                            │        │
 *   │  当前行歌词（亮）                        │        │
 *   │  下 一 行歌词                            │        │
 *   └──────────────────────────────────────────┴────────┘
 *   媒体键放右手侧竖排是**按得顺手**（拇指自然落点）＋不跟进度条抢横排空间；
 *   上=上一首 / 中=播放暂停 / 下=下一首，**三键同色、按下整键翻转为青色**。
 *   顶栏只留 "● PC NOW" + 右侧同步置信度小字：连通与否由状态点的颜色表达；
 *   "在线"两字与右端播放器名（恒为 "PC"，与左边 PC NOW 语义重复）F47 已删。
 *
 * ── 歌词时间轴提前量（F47）──────────────────────────────────────────
 *   实机实测设备端高亮行比 PC 端**晚 1.25s**（恒定偏置、非抖动）⇒ 补偿放在
 *   **数据层** pcnow_service.c 的 PCNOW_LYR_LEAD_MS，本页不做任何偏移换算。
 *   本页只认 info.lrc_seq 变化后换文本；补偿值改大改小都不影响这里。
 *
 * ── 两个必须记住的布局坑 ────────────────────────────────────────────
 *  1. tabview 的"页"对象被默认主题加了 `pad_normal`（LV_DPX，实机约 13px），
 *     子对象超出页对象边界的部分**会被 LVGL 裁掉**。所以**不能**写死
 *     `800 × (V_RES-48)` 再 `BOTTOM_MID` 对齐（旧版就是这样，顶边伸出页对象
 *     9~13px）。这里一律按"页内容区实际尺寸"布局。
 *  2. bin 字体行高 52（ascent 37/descent -15），而顶栏里的小字（montserrat_14）
 *     行高只有 14。两者用 `LV_ALIGN_OUT_RIGHT_MID` 对齐时，bin 文字的**盒顶**会
 *     比小字高出约 19px —— 顶栏中线给不够就会裁字。故顶栏单独定 `HDR_MID`。
 *
 * ── 线程纪律 ─────────────────────────────────────────────────────────
 * 数据全部来自 pcnow_service 快照（pcnow_service_get_info，锁内、非阻塞）；
 * 本页只在 LVGL 线程的 lv_timer 里轮询刷新，绝不从服务线程碰 LVGL。
 *
 * ── 封面（方案 B：LVGL 软件旋转，见方案文档 §6.3）───────────────────
 *  - 像素来自 pcnow_service_get_cover()（RGB888 240×240，解码在服务线程）；
 *  - 用 lv_image + lv_image_dsc_t 包装，lv_image_set_rotation() 连续旋转
 *    （单位 0.1°，每帧 +2°@30fps ≈ 6s/圈）；**仅 LIVE 且 playing 且封面
 *    就绪时转**（动画按需起停约定，切走 tab 由 destroy 收掉定时器）；
 *  - 圆形遮罩：外层容器 radius=圆 + clip_corner=true；旋转的方形图的
 *    内切圆半径恒等于容器半径 ⇒ 圆内永远被像素覆盖，露不出四角；
 *  - 显示缩放 lv_image_set_scale(256=1.0)：240 源 → 176 显示。
 *
 * ── 歌词（v1.1：时间轴在设备，见方案文档 §4.6）──────────────────────
 * 服务端已按内插进度自算 lrc_index / lrc_seq，本页只在 seq 变化时换文本。
 * 首版先做"三行静态高亮"（上一行暗/当前亮/下一行暗）；上滑动效是后续打磨项，
 * 不影响正确性。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_pcnow.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "ui/common/ui_binfont.h"
#include "middleware/network/pcnow_service.h"

static const char *TAG = "TAB_PCNOW";

LV_FONT_DECLARE(lv_font_cn_30);

#define COVER_BOX    176                     /* 圆形封面显示直径 */
#define POLL_MS      200                     /* 快照轮询 */
#define ROT_MS       33                      /* 旋转帧周期 ≈30fps */
#define ROT_STEP     20                      /* 每帧 2.0°（单位 0.1°）→ 6s/圈 */
#define LYR_LINES    3

/* ---- 布局常量（坐标均为 page 内容区内的相对值，见文件头两个坑） ---- */
#define TAB_BAR_H    48                      /* ★ 必须与 page_main.c 的
                                                lv_tabview_set_tab_bar_size(48) 一致 */
#define HDR_MID      30                      /* 顶栏视觉中线：>=19 才能装下 bin 字体
                                                行高 52 的状态文字（盒顶 = MID-26） */
#define COVER_Y      62                      /* 封面顶边 */
#define BTN_SIZE     80                      /* 右侧竖排媒体键直径（原 48 且挤在右上角） */
#define BTN_GAP      20
#define BTN_MARGIN   10                      /* 按钮列距可用区右边缘 */
#define COL_GAP      24                      /* 按钮列与文本列之间的留白 */
#define EDGE         4                       /* 左/右统一内边距 */

/* ---- 门禁卡片（非 LIVE 时显示） ---- */
static lv_obj_t *s_gate;
static lv_obj_t *s_gate_title;
static lv_obj_t *s_gate_hint;

/* ---- 媒体区 ---- */
static lv_obj_t *s_media;
static lv_obj_t *s_dot;
static lv_obj_t *s_state_lbl;                /* 异常态文字：等待推流/未连接（LIVE 时留空） */
static lv_obj_t *s_btn_play;                 /* 播放/暂停切换按钮（图标随状态换） */
static int       s_shown_playing = -1;       /* 已上屏的播放态（避免重复刷图标） */

/* ---- 封面 ---- */
static lv_obj_t *s_cover_box;                /* 圆形裁剪容器 */
static lv_obj_t *s_cover_img;                /* lv_image（方案 B 旋转载体） */
static lv_obj_t *s_cover_ph;                 /* 无封面占位图标 */
static lv_image_dsc_t s_cover_dsc;           /* 包装服务端像素 */
static int      s_shown_cover_id = -1;       /* 已上屏的 cover_id */
static int      s_cover_px = COVER_BOX;      /* 本次构建实际用的封面直径（自适应，见 build） */
static uint16_t s_rot_deg10;                 /* 累计角（0.1°） */

/* ---- 曲名 / 歌手 / 进度 ---- */
static lv_obj_t *s_title;
static lv_obj_t *s_artist;
static lv_obj_t *s_bar;                     /* lv_slider：可拖动 seek（§28.46） */
static bool      s_seeking;                 /* 手指按住滑条期间 poll 不覆盖 */
static lv_obj_t *s_time_cur;
static lv_obj_t *s_time_tot;

/* ---- 同步置信度（ICD V1.3 的 sync_src）----
 * 为什么要显示：网易云这类播放器**没有 SMTC 时间轴**，进度是 PC 本地内插的，
 * 上位机中途启动时会整体偏移。把"当前同步来源"摆到用户眼前，比默默错位诚实。 */
static lv_obj_t *s_sync_lbl;
static char      s_shown_sync[PCNOW_SYNC_SRC_LEN];

/* ---- 歌词三行窗 ---- */
static lv_obj_t *s_lyr[LYR_LINES];           /* [0]上一行 [1]当前行 [2]下一行 */
static lv_font_t *s_lyr_font;
static int       s_lyr_pitch;

/* ---- 定时器 ---- */
static lv_timer_t *s_poll_timer;
static lv_timer_t *s_rot_timer;

/* =====================================================================
 * 工具
 * ===================================================================== */

static void mmss(char *buf, size_t len, uint32_t ms)
{
    snprintf(buf, len, "%02u:%02u",
             (unsigned)(ms / 60000), (unsigned)((ms / 1000) % 60));
}

static lv_font_t *dyn_font(void)
{
    lv_font_t *f = ui_binfont_get();
    return f ? f : (lv_font_t *)&lv_font_cn_30;
}

/* =====================================================================
 * 媒体控制按钮：NOW_CMD → pcnow_service → link_manager → PC:50003
 * ===================================================================== */

/* =====================================================================
 * 进度滑条：拖动跳转（NOW_CMD seek）。网易云 Win32 不支持 SMTC seek，
 * 表现为"只动显示"；B 站等上报时间轴的播放器可真跳。
 * ===================================================================== */

static void bar_events_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_seeking = true;                    /* 拖动期间 poll 不覆盖滑条值 */
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        pcnow_service_send_seek((uint32_t)lv_slider_get_value(s_bar));
        s_seeking = false;
    }
}

static void media_btn_cb(lv_event_t *e)
{
    /* ★ 命令字规则（F41-A 的最终形态）：
       - "play" 键是**自适应**的：单击时按当前播放态取反。真值只有 s_shown_playing
         一个来源 —— 它同时也是**图标**的来源，所以"图标显示暂停"与"实际发 pause"
         不可能再分叉（这是早先那版 bug：命令字存在对象 user_data 上，而
         lv_obj_set_user_data() 与 lv_event_get_user_data() 是**两个不同字段**）。
       - 其余键（prev/next）命令字固定，直接用注册时给的值。 */
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (!cmd || !cmd[0]) {
        return;
    }
    if (strcmp(cmd, "play") == 0) {
        /* >0 才认为在播放；-1（还没收到任何 META）当作未播放 ⇒ 发 play */
        cmd = (s_shown_playing > 0) ? "pause" : "play";
    }
    ESP_LOGI(TAG, "media cmd -> %s (shown_playing=%d)", cmd, s_shown_playing);
    pcnow_service_send_cmd(cmd);
}

/**
 * 建一个圆形媒体键（TOP_LEFT 定点摆放，便于竖排对齐）。
 * @param x,y 相对父对象内容区左上角。
 *
 * ★ 三个键**同色**（卡片底 + 细描边 + 浅色图标），按下时**颜色翻转**为
 *   "强调青底 + 深色图标"。为什么不沿用"中间主键做成青色实心"：
 *   三键同色时"哪个是主键"由**位置**（居中）表达就够了；青色实心反而会把旁边
 *   两个衬成"不可点"的观感。同色 + 按下翻转，既有统一感又有明确反馈。
 *
 * ★★ 图标颜色**必须设在按钮上**，绝不能设在 label 上（这条踩过就难查）：
 *   `text_color` 是 LVGL 的**可继承**属性（lv_style.c:119 INHERITABLE）；
 *   子 label 只要**不设**自己的 text_color，就会向上取父对象**在当前 state 下**
 *   的值 —— 继承时把 selector 换成 `part | obj->state`（lv_obj_style.c:1582），
 *   即用的是**父对象的** state。而 state 变化会走
 *   `lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY)`
 *   ⇒ `refresh_children_style()` 递归重绘子对象（lv_obj_style.c:1167 / :315）。
 *   ⇒ 所以"btn 上设 0 态与 PRESSED 两个 text_color、label 一个都不设"就能让图标
 *     跟着翻转。反之若在 label 上写死颜色，PRESSED 永远传不到 label
 *     （子对象不继承 state），图标会始终一个色 —— 看起来像"按下态没做"。
 */
static lv_obj_t *media_btn_create(lv_obj_t *parent, const char *sym,
                                  const char *cmd, int x, int y)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    /* 常态：卡片底 + 细描边 + 浅色图标（label 继承该 text_color） */
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, COL_CARD, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_text_color(btn, COL_TEXT, 0);

    /* 按下：颜色翻转 —— 底/描边变强调青，图标变深色底（高对比，指下即知） */
    lv_obj_set_style_bg_color(btn, COL_CYAN, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, COL_CYAN, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, COL_BG, LV_STATE_PRESSED);

    lv_obj_add_event_cb(btn, media_btn_cb, LV_EVENT_CLICKED, (void *)cmd);

    lv_obj_t *lbl = lv_label_create(btn);
    /* ★ 必须用 montserrat 系列：LV_SYMBOL_* 在 FontAwesome 私用区(U+F000+)，bin 中文字
       体不含（会显示方块）。40 号字在 80px 圆键里留白刚好。 */
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_40, 0);
    /* ★ 故意**不设** text_color —— 让它继承按钮的，按下才能一起翻转（见函数头） */
    lv_label_set_text(lbl, sym);
    lv_obj_center(lbl);
    return btn;
}

/* =====================================================================
 * 封面：cover_id 变化时重绑像素源
 * ===================================================================== */

static void cover_apply(void)
{
    int w = 0, h = 0;
    const uint8_t *px = pcnow_service_get_cover(&w, &h);
    if (!px || w <= 0 || h <= 0) {
        return;
    }
    memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
    s_cover_dsc.header.w = (uint32_t)w;
    s_cover_dsc.header.h = (uint32_t)h;
    s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_cover_dsc.header.stride = (uint32_t)w * 3;
    s_cover_dsc.data = px;
    s_cover_dsc.data_size = (size_t)w * h * 3;

    lv_image_set_src(s_cover_img, &s_cover_dsc);
    lv_image_set_scale(s_cover_img, s_cover_px * 256 / w);
    lv_image_set_pivot(s_cover_img, w / 2, h / 2);
    lv_image_set_antialias(s_cover_img, true);
    lv_obj_set_clickable(s_cover_ph, false);
    lv_obj_set_hidden(s_cover_ph, true);
}

/* =====================================================================
 * 旋转（方案 B）：仅在播放中转
 * ===================================================================== */

static void rot_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_rot_deg10 = (uint16_t)((s_rot_deg10 + ROT_STEP) % 3600);
    lv_image_set_rotation(s_cover_img, s_rot_deg10);
}

static void rot_set_running(bool run)
{
    /* LVGL9 的 lv_timer_t 是不透明结构，读不到 ->paused；好在 pause/resume
     * 本身幂等且 NULL 安全（LV_CHECK_ARG 直接 return），无条件调用即可。 */
    if (run) {
        lv_timer_resume(s_rot_timer);
    } else {
        lv_timer_pause(s_rot_timer);
    }
}

/* =====================================================================
 * 200ms 轮询：状态 / 文本 / 进度 / 歌词 seq
 * ===================================================================== */

static void apply_state(pcnow_state_t st)
{
    bool live = (st == PCNOW_LIVE);
    if (live) {
        lv_obj_set_hidden(s_gate, true);
        lv_obj_set_hidden(s_media, false);
        lv_obj_set_style_bg_color(s_dot, COL_GREEN, 0);
        /* ★ F47：LIVE 时**不再写"在线"**（用户要求）—— 顶栏左侧只剩 "● PC NOW"，
         *   连通与否由状态点的颜色表达，比多两个字干净。
         *   但"等待推流 / 未连接"必须留着：那两种情况没有绿点，光看"没文字"分不清
         *   "已经连上只是没在放"还是"根本没连上 PC"。 */
        lv_label_set_text(s_state_lbl, "");
    } else {
        lv_obj_set_hidden(s_gate, false);
        lv_obj_set_hidden(s_media, true);
        lv_obj_set_style_bg_color(s_dot, COL_RED, 0);
        lv_label_set_text(s_state_lbl, st == PCNOW_WAITING ? "等待推流" : "未连接");
        lv_label_set_text(s_gate_hint,
                          st == PCNOW_WAITING
                              ? "PC 已连接，等待推流…"
                              : "未连接 PC，请在 PC 端启动上位机");
    }
    rot_set_running(live);
}

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    pcnow_state_t st = pcnow_service_get_state();
    apply_state(st);
    if (st != PCNOW_LIVE) {
        return;
    }

    pcnow_info_t info;
    if (!pcnow_service_get_info(&info)) {
        return;                                  /* 还没有任何 META */
    }

    /* 封面换面 */
    if ((int)info.cover_id != s_shown_cover_id && info.cover_ready) {
        cover_apply();
        s_shown_cover_id = (int)info.cover_id;
    }

    /* 曲名 / 歌手 */
    lv_label_set_text(s_title, info.title[0] ? info.title : "—");
    lv_label_set_text(s_artist, info.artist[0] ? info.artist : "—");

    /* 播放/暂停按钮图标随播放态切换。
       ★ 只改**图标**，不再另存一份"命令字"—— 命令字由 media_btn_cb 现算，
         单一真值来源，杜绝两处状态分叉（F41-A）。 */
    if ((int)info.playing != s_shown_playing) {
        s_shown_playing = (int)info.playing;
        if (s_btn_play) {
            lv_obj_t *lbl = lv_obj_get_child(s_btn_play, 0);
            lv_label_set_text(lbl, info.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }

    /* 同步置信度（ICD V1.3）：只在来源变化时改文字，避免每拍刷 LVGL。
       老版 PC 不带该字段 ⇒ 空串 ⇒ 按 "event"（本地内插）显示。 */
    {
        const char *sst = info.sync_src[0] ? info.sync_src : "event";
        if (strcmp(sst, s_shown_sync) != 0) {
            strlcpy(s_shown_sync, sst, sizeof(s_shown_sync));
            const char *txt;
            lv_color_t  col;
            if (!strcmp(sst, "timeline") || !strcmp(sst, "dom") || !strcmp(sst, "a11y")) {
                txt = "同步准确";                 /* 有真位置来源 */
                col = COL_CYAN;
            } else if (!strcmp(sst, "manual")) {
                txt = "已校准";                   /* 用户拖过滑条，锚点被重设 */
                col = COL_CYAN;
            } else {
                txt = "本地内插";                 /* 仅靠换曲/暂停事件锚定 */
                col = COL_SUBTEXT;
            }
            if (s_sync_lbl) {
                lv_label_set_text(s_sync_lbl, txt);
                lv_obj_set_style_text_color(s_sync_lbl, col, 0);
            }
        }
    }

    /* 进度（服务端内插，同源时钟；曲长变化时重设量程；拖动中不覆盖） */
    static uint32_t s_seen_dur;
    if (info.duration_ms != s_seen_dur) {
        s_seen_dur = info.duration_ms;
        lv_slider_set_range(s_bar, 0, info.duration_ms > 0 ? (int32_t)info.duration_ms : 1);
    }
    if (!s_seeking) {
        lv_slider_set_value(s_bar, (int32_t)info.pos_ms, LV_ANIM_OFF);
    }
    char tbuf[10];
    mmss(tbuf, sizeof(tbuf), info.pos_ms);
    lv_label_set_text(s_time_cur, tbuf);
    mmss(tbuf, sizeof(tbuf), info.duration_ms);
    lv_label_set_text(s_time_tot, tbuf);

    /* 歌词：seq 变化才动；三行 = 服务端锁内算好的 prev/cur/next */
    static uint32_t s_seen_seq;
    if (info.lrc_seq != s_seen_seq) {
        s_seen_seq = info.lrc_seq;
        lv_label_set_text(s_lyr[0], info.lrc_prev);
        /* 占位用 ASCII "..."：bin 字体不含 ♪(U+266A)，会显示方块 */
        lv_label_set_text(s_lyr[1], info.lrc_text[0] ? info.lrc_text : "...");
        lv_label_set_text(s_lyr[2], info.lrc_next);
        lv_obj_set_style_text_color(s_lyr[0], COL_SUBTEXT, 0);
        lv_obj_set_style_text_color(s_lyr[1], COL_TEXT, 0);
        lv_obj_set_style_text_color(s_lyr[2], COL_SUBTEXT, 0);
        lv_obj_set_style_text_opa(s_lyr[0], LV_OPA_50, 0);
        lv_obj_set_style_text_opa(s_lyr[1], LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(s_lyr[2], LV_OPA_50, 0);
    }
}

/* =====================================================================
 * 构建 / 销毁
 * ===================================================================== */

void tab_pcnow_build(lv_obj_t *parent, lv_group_t *g)
{
    (void)g;
    s_lyr_font = dyn_font();
    s_lyr_pitch = lv_font_get_line_height(s_lyr_font);

    /* ---- 门禁卡（中文走 bin 字体；WIFI 是 FontAwesome 私用区符号，
     *       bin 字表不含 → 必须独立 label 用 UI_FONT(montserrat)）---- */
    s_gate = ui_common_card(parent, 520, 200, 16);
    lv_obj_center(s_gate);
    s_gate_title = lv_label_create(s_gate);
    lv_obj_set_style_text_font(s_gate_title, s_lyr_font, 0);
    lv_obj_set_style_text_color(s_gate_title, COL_CYAN, 0);
    lv_label_set_text(s_gate_title, "音乐副屏");
    lv_obj_align(s_gate_title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_t *gate_wifi = lv_label_create(s_gate);
    lv_obj_set_style_text_font(gate_wifi, UI_FONT, 0);
    lv_obj_set_style_text_color(gate_wifi, COL_CYAN, 0);
    lv_label_set_text(gate_wifi, LV_SYMBOL_WIFI);
    lv_obj_align_to(gate_wifi, s_gate_title, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    s_gate_hint = lv_label_create(s_gate);
    lv_obj_set_style_text_font(s_gate_hint, s_lyr_font, 0);
    lv_obj_set_style_text_color(s_gate_hint, COL_SUBTEXT, 0);
    lv_label_set_text(s_gate_hint, "未连接 PC，请在 PC 端启动上位机");
    lv_obj_align(s_gate_hint, LV_ALIGN_TOP_MID, 0, 96);
    lv_label_set_long_mode(s_gate_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_gate_hint, 520 - 48);

    /* ★ 可用区尺寸：**必须扣掉 tabview"页"对象自己的内边距**。
     *   默认主题给 tabview 的页加了 pad_normal（LV_DPX，实机约 13px），子对象超出
     *   页边界的部分会被 LVGL 直接裁掉。旧版写死 800×(V_RES-48) 再 BOTTOM_MID 对齐，
     *   顶边会伸出页对象 9~13px —— 左上角"在线"（bin 字体，行高 52，盒顶原本就在 -7）
     *   因此被裁掉上面一截，看起来就像"被上面的 tab 栏压住"。 */
    const int W = BSP_LCD_H_RES
                  - (int)lv_obj_get_style_pad_left(parent, LV_PART_MAIN)
                  - (int)lv_obj_get_style_pad_right(parent, LV_PART_MAIN);
    const int H = (BSP_LCD_V_RES - TAB_BAR_H)
                  - (int)lv_obj_get_style_pad_top(parent, LV_PART_MAIN)
                  - (int)lv_obj_get_style_pad_bottom(parent, LV_PART_MAIN);

    /* 右侧按钮列几何（先算好：顶栏的同步小字也要按 content_r 右对齐） */
    const int btn_x = W - BTN_MARGIN - BTN_SIZE;
    int btn_top = (H - (BTN_SIZE * 3 + BTN_GAP * 2)) / 2;
    if (btn_top < EDGE + 4) {
        btn_top = EDGE + 4;                                  /* 小屏兜底 */
    }
    const int content_r = btn_x - COL_GAP;                   /* 文本/歌词的右边界 */

    /* ---- 媒体区（隐藏起点）：填满页内容区（不写死尺寸、不靠 align） ---- */
    s_media = lv_obj_create(parent);
    lv_obj_set_size(s_media, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_media, 0, 0);
    lv_obj_set_style_bg_opa(s_media, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_media, 0, 0);
    lv_obj_set_style_pad_all(s_media, 0, 0);
    lv_obj_set_scrollable(s_media, false);
    lv_obj_set_hidden(s_media, true);

    /* 顶栏：状态点 + "PC NOW" + 状态文字。整行按 HDR_MID 对齐 —— 状态文字是 bin
     * 字体（行高 52），它的盒顶 = HDR_MID - 26，必须 >= 0 才不会被裁。 */
    s_dot = lv_obj_create(s_media);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, COL_RED, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, EDGE, HDR_MID - 6);
    lv_obj_t *hdr = lv_label_create(s_media);
    lv_obj_set_style_text_font(hdr, UI_FONT, 0);
    lv_obj_set_style_text_color(hdr, COL_TEXT, 0);
    lv_label_set_text(hdr, "PC NOW");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, EDGE + 20, HDR_MID - 7);
    s_state_lbl = lv_label_create(s_media);
    lv_obj_set_style_text_font(s_state_lbl, s_lyr_font, 0);
    lv_obj_set_style_text_color(s_state_lbl, COL_SUBTEXT, 0);
    lv_label_set_text(s_state_lbl, "未连接");
    lv_obj_align_to(s_state_lbl, hdr, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    /* 同步置信度小字（ICD V1.3 的 sync_src）—— 也放**顶栏**。
     * 它和"在线/未连接"同属"链路可信度"，凑一堆最合理；更关键的是它是 bin 字体
     * （盒高 52），若摆在时间行，会直接顶到下面三行歌词上（那之间的缝只有 ~30px）。
     * 位置**固定**在内容区右上（不跟状态文案相对对齐，免得字数一变就跳位、最长时还会挤上）。
     * ★ F47：原来这里留了 120px 给右端的"播放器名"，播放器名删掉后不再需要留白。 */
    s_sync_lbl = lv_label_create(s_media);
    lv_obj_set_style_text_font(s_sync_lbl, s_lyr_font, 0);
    lv_obj_set_style_text_color(s_sync_lbl, COL_SUBTEXT, 0);
    lv_label_set_text(s_sync_lbl, "");
    lv_obj_align(s_sync_lbl, LV_ALIGN_TOP_RIGHT, -(W - content_r),
                 HDR_MID - s_lyr_pitch / 2);

    /* ---- 媒体控制：竖排贴右侧（上=上一首 / 中=播放暂停 / 下=下一首） ----
     * 放右手边是拇指自然落点；整列在内容区里垂直居中，不跟进度条抢横向空间。
     * （btn_x / btn_top 已在上面算好；三键同色，按下翻转，见 media_btn_create） */
    media_btn_create(s_media, LV_SYMBOL_PREV, "prev", btn_x, btn_top);
    s_btn_play = media_btn_create(s_media, LV_SYMBOL_PLAY, "play", btn_x,
                                  btn_top + BTN_SIZE + BTN_GAP);
    media_btn_create(s_media, LV_SYMBOL_NEXT, "next", btn_x,
                     btn_top + (BTN_SIZE + BTN_GAP) * 2);

    /* ★ 纵向排布：**歌词贴底、封面吃剩余空间**。
     *   三行歌词（行高 52）必须完整，所以先定歌词带，再把封面塞进剩下的空间；
     *   封面不超过设计值 COVER_BOX，也不小于 120（再小就看不清了）。
     *   这样无论实机的主题内边距是 9 还是 16，都不会出现"第三行歌词出界"。 */
    const int ly_top = H - LYR_LINES * s_lyr_pitch - EDGE;
    int cover_box = ly_top - 8 - COVER_Y;
    if (cover_box > COVER_BOX) {
        cover_box = COVER_BOX;
    }
    if (cover_box < 120) {
        cover_box = 120;
    }
    s_cover_px = cover_box;

    /* 封面：圆形裁剪容器 + lv_image（方案 B 旋转） */
    s_cover_box = lv_obj_create(s_media);
    lv_obj_set_size(s_cover_box, cover_box, cover_box);
    lv_obj_align(s_cover_box, LV_ALIGN_TOP_LEFT, EDGE, COVER_Y);
    lv_obj_set_style_radius(s_cover_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_cover_box, true, 0);
    lv_obj_set_style_bg_color(s_cover_box, COL_CARD, 0);
    lv_obj_set_style_border_width(s_cover_box, 2, 0);
    lv_obj_set_style_border_color(s_cover_box, COL_BORDER, 0);
    lv_obj_set_style_pad_all(s_cover_box, 0, 0);
    lv_obj_set_scrollable(s_cover_box, false);
    s_cover_img = lv_image_create(s_cover_box);
    lv_obj_center(s_cover_img);
    s_cover_ph = lv_label_create(s_cover_box);
    lv_obj_set_style_text_font(s_cover_ph, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_cover_ph, COL_SUBTEXT, 0);
    lv_label_set_text(s_cover_ph, LV_SYMBOL_IMAGE);
    lv_obj_center(s_cover_ph);

    /* 右列：曲名 / 歌手 / 进度条 / 时间（宽度收到 content_r，给按钮列让路） */
    const int rx = EDGE + cover_box + 24;
    const int tx = COVER_Y + 8;
    const int colw = content_r - rx;
    s_title = lv_label_create(s_media);
    lv_obj_set_style_text_font(s_title, s_lyr_font, 0);
    lv_obj_set_style_text_color(s_title, COL_TEXT, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title, colw);
    lv_label_set_text(s_title, "—");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, rx, tx);

    s_artist = lv_label_create(s_media);
    lv_obj_set_style_text_font(s_artist, s_lyr_font, 0);
    lv_obj_set_style_text_color(s_artist, COL_SUBTEXT, 0);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_artist, colw);
    lv_label_set_text(s_artist, "—");
    lv_obj_align(s_artist, LV_ALIGN_TOP_LEFT, rx, tx + s_lyr_pitch);

    /* 进度条（lv_slider：拖动 = NOW_CMD seek，见 bar_events_cb） */
    s_bar = lv_slider_create(s_media);
    lv_slider_set_range(s_bar, 0, 100);
    lv_obj_set_size(s_bar, colw, 8);
    lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, rx, tx + 2 * s_lyr_pitch + 12);
    lv_obj_set_ext_click_area(s_bar, 14);    /* 触摸区扩大，8px 条不好按 */
    lv_obj_add_event_cb(s_bar, bar_events_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(s_bar, COL_CARD, 0);
    lv_obj_set_style_bg_color(s_bar, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, COL_CYAN, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bar, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(s_bar, 4, 0);
    lv_slider_set_value(s_bar, 0, LV_ANIM_OFF);

    const int tym = tx + 2 * s_lyr_pitch + 26;   /* 时间行 y（只有 00:00 / 03:20，14 高） */
    s_time_cur = lv_label_create(s_media);
    lv_obj_set_style_text_font(s_time_cur, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_cur, COL_SUBTEXT, 0);
    lv_label_set_text(s_time_cur, "00:00");
    lv_obj_align(s_time_cur, LV_ALIGN_TOP_LEFT, rx, tym);
    s_time_tot = lv_label_create(s_media);
    lv_obj_set_style_text_font(s_time_tot, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_tot, COL_SUBTEXT, 0);
    lv_label_set_text(s_time_tot, "00:00");
    lv_obj_align(s_time_tot, LV_ALIGN_TOP_RIGHT, -(W - content_r), tym);

    /* 歌词三行窗（贴底，宽度收到 content_r；ly_top 已在上面与封面一起算好） */
    for (int i = 0; i < LYR_LINES; i++) {
        s_lyr[i] = lv_label_create(s_media);
        lv_obj_set_style_text_font(s_lyr[i], s_lyr_font, 0);
        lv_obj_set_style_text_color(s_lyr[i], COL_SUBTEXT, 0);
        lv_label_set_long_mode(s_lyr[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_lyr[i], content_r - EDGE);
        lv_label_set_text(s_lyr[i], "");
        /* 标签高度 = SIZE_CONTENT（行框自算，防裁字，见 tab_btsink 教训） */
        lv_obj_align(s_lyr[i], LV_ALIGN_TOP_LEFT, EDGE, ly_top + i * s_lyr_pitch);
    }
    lv_obj_set_style_text_color(s_lyr[1], COL_TEXT, 0);

    /* ---- 定时器 ---- */
    s_poll_timer = lv_timer_create(poll_timer_cb, POLL_MS, NULL);
    s_rot_timer = lv_timer_create(rot_timer_cb, ROT_MS, NULL);
    lv_timer_pause(s_rot_timer);                 /* 按需起停：LIVE+playing 才转 */

    /* 版本标记：日志可判固件是否烧进去（UI 改版后务必换一个号） */
    ESP_LOGI(TAG, "built (rev UI3/F47 2026-09-24, area %dx%d) — 三键同色+按下翻转, "
                  "顶栏去'在线'/去播放器名; 歌词提前量在 pcnow_service", W, H);
}

void tab_pcnow_destroy(void)
{
    if (s_poll_timer) { lv_timer_del(s_poll_timer); s_poll_timer = NULL; }
    if (s_rot_timer)  { lv_timer_del(s_rot_timer);  s_rot_timer = NULL; }
    s_shown_cover_id = -1;
    s_shown_playing = -1;
    s_btn_play = NULL;
    s_sync_lbl = NULL;
    s_shown_sync[0] = '\0';
    s_seeking = false;
    s_rot_deg10 = 0;
    ESP_LOGI(TAG, "destroyed");
}
