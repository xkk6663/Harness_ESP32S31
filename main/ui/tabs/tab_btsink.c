/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_btsink.c
 * @brief SINK Tab：A2DP 接收页 —— 手机/电脑推音乐过来，本机当蓝牙音箱放。
 *
 * ── 本页与其它 tab 的本质差异：它是"有前置条件"的页 ──────────────
 * 只有当 A2DP 角色 = SINK 时才有内容可显示。而**角色切换的唯一入口是
 * Bluetooth 子页面（page_bluetooth.c）里的按钮** —— 因为 A2DP 是全局单例，
 * 切角色必须重建整个 host，必然断开当前连接，所以这件事绝不能是
 * "划了一下 tab"的副作用（详见 bt_audio_service_switch_role 的调用契约）。
 *
 * 因此本页对角色变化只做一件事：**动态显示/隐藏内容**。
 * 既不在这里改角色，也不在切走时清状态 —— 所有状态都由 bt_sink_service
 * 持有，页面的存在与否不影响蓝牙行为。
 *
 * ── 线程纪律（本项目铁律）─────────────────────────────────────────
 * 元数据/歌词在 BT 线程里由 bt_sink_service 缓存；本文件只在 **LVGL 线程**
 * 用 500ms lv_timer 轮询（bt_sink_service_get_* 全部加锁、非阻塞）。
 * 反过来，本文件绝不从回调里碰 LVGL。
 *
 * ── 2026-09-22 改版：删封面、曲名/歌手居中、歌词逐句渐入渐出 ────────
 * ① **封面整块删除**。依据 docs/项目规划/改动记录.md §28.28：AVRCP Cover Art
 *    在安卓侧默认关闭（默认 AVRCP 1.5 + OEM overlay 常为 false + App 未必交图），
 *    实测对端从不推送。原 lv_canvas + 双缓冲 240×240 RGB888 的整套显示逻辑一并
 *    移除（服务侧也删了，省下 ~337KB PSRAM，见 bt_sink_service.c 文件头）。
 * ② **歌词从哪来**：国内音乐 App 的"车载蓝牙歌词"是把当前歌词行**覆盖写进
 *    AVRCP TITLE(0x01) 属性**。服务侧已负责把"真曲名"锁住、把后续 TITLE 变化
 *    当歌词吐出来（bt_sink_service_get_lyric），本页只负责画。
 * ③ 歌词用一个"三行窗口"呈现：上一行（暗）/ 当前行（亮）/ 待入行（全透明）。
 *    新句到达时整列上滑约一行高度，同时三行各自做透明度插值 —— 视觉上就是
 *    "旧句上滑淡出、新句自下方滑入淡显"。动画只用一个 lv_anim（var = 窗口），
 *    一帧内改位移 + 三个 opacity，代价极小。
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "tab_btsink.h"
#include "ui/common/ui_theme.h"
#include "ui/common/ui_common.h"
#include "ui/common/ui_binfont.h"
#include "middleware/bluetooth/bt_audio_service.h"
#include "middleware/bluetooth/bt_sink_service.h"

static const char *TAG = "TAB_BTSINK";

/* 工程内置中文点阵字体（与其它 tab 一致）。
 * ★ 曲名/歌手/歌词这类**运行时**文本来自对端 AVRCP，字符集不可预测，
 *   所以一旦 ui_binfont 就绪就换成全 CJK bin 字体（见 apply_fonts）。 */
LV_FONT_DECLARE(lv_font_cn_20);
LV_FONT_DECLARE(lv_font_cn_30);

#define CARD_W        (BSP_LCD_H_RES - 40)
#define HEADER_H      44
#define CTRL_H        56
#define POLL_MS       500

/* ---- 歌词窗口 ----
 * ★ 行距**不写死**：lv_font_cjk_28.bin 的 head 是 ascent=37 / descent=-15，
 *   即 line_height = 52px，而字形墨迹只有约 28px 高（上下留白很大）。
 *   如果硬写一个"看起来合适"的行高（比如 44）去 lv_obj_set_size，
 *   就会把 >44px 的行框裁掉一截 —— LVGL 的 label 是**按自身 coords 裁剪**的
 *   （见 lv_draw_label.c: lv_draw_add_task(layer, coords, ...)）。
 *   所以这里让标签高度 = SIZE_CONTENT（正好一个行框），
 *   行距 s_lyr_pitch 在 apply_fonts() 里用 lv_font_get_line_height() 现算。
 *   换字体、换字号都不会错位，也不会裁字。 */
#define LYR_LINES     3           /* 同屏行数：上一行 / 当前行 / 待入行 */
#define LYR_GAP       0           /* 行框之间再加的空隙（行框本身留白已够） */
#define LYR_OPA_DIM   90          /* 上一行的残留亮度（0-255） */
#define LYR_ANIM_MS   380

/* ---- 均衡器指示（仅"正在收流"时显示）---- */
#define EQ_BARS       4
#define EQ_BAR_W      5
#define EQ_BAR_GAP    4
#define EQ_BOX_W      (EQ_BARS * EQ_BAR_W + (EQ_BARS - 1) * EQ_BAR_GAP)
#define EQ_BOX_H      22
#define EQ_H_MIN      5
#define EQ_H_MAX      19
#define EQ_X(i)       ((i) * (EQ_BAR_W + EQ_BAR_GAP))
#define EQ_CYCLE      1600        /* 一次循环时长(ms)，越大越慢 */

/* ---- 门禁卡片（非 SINK / 蓝牙未开时显示）---- */
static lv_obj_t *s_gate;
static lv_obj_t *s_gate_title;
static lv_obj_t *s_gate_hint;

/* ---- 媒体区整体（满足条件时显示）---- */
static lv_obj_t *s_media;

/* ---- 顶栏 ---- */
static lv_obj_t *s_dot;
static lv_obj_t *s_state;
static lv_obj_t *s_eq_box;
static lv_obj_t *s_eq_bar[EQ_BARS];
static bool      s_eq_running;        /* EQ 动画是否在跑（按需起停，见 eq_set_running） */

/* ---- 曲名 / 歌手 ---- */
static lv_obj_t *s_title;
static lv_obj_t *s_artist;

/* ---- 歌词窗口 ---- */
static lv_obj_t *s_lyr_box;          /* 裁剪窗口：只露出 LYR_LINES 行 */
static lv_obj_t *s_lyr_col;          /* 被整体上下平移的那一列 */
static lv_obj_t *s_lyr[LYR_LINES];   /* [0]=上一行 [1]=当前行 [2]=待入行 */
static int       s_lyr_pitch;        /* 行距 = 字体行高 + LYR_GAP（apply_fonts 现算） */
static bool      s_lyr_have_prev;    /* 是否已有"上一行"（否则中间格是占位符，不能上挪） */
static bool      s_lyr_busy;         /* 上滑动画是否在跑 */
static bool      s_lyr_queued;       /* 动画期间到达的新句（不丢，播完补上） */
static char      s_lyr_next[BT_SINK_LYRIC_LEN];

/* ---- 控制 ---- */
static lv_obj_t *s_play_btn;
static lv_obj_t *s_prev_btn;
static lv_obj_t *s_next_btn;

/* ---- 状态跟踪（只在变化时刷 UI，避免无谓重排）---- */
static lv_timer_t  *s_timer;
static bt_sink_state_t s_state_last = BT_SINK_STATE_IDLE;
static int          s_font_applied = 0;   /* 0 = 尚未套用 CJK 字体 */
static uint32_t     s_lyric_seq = 0;      /* 已呈现的歌词序号（0 = 还没同步过） */

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

/* 透明容器：只做布局，不显示任何底色/边框 */
static lv_obj_t *transparent_box(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 把 CJK 字体套到"运行期可能出现中文/日文"的标签上：曲名 / 歌手 / 三行歌词。
 *
 * ★ 为什么不能套到 s_state / s_gate_title / s_gate_hint：
 *   这三个标签的文案是写死的英文 + LV_SYMBOL_*（FontAwesome 私用区 0xF2xx）。
 *   而 lv_binfont 生成的字体结构是 lv_zalloc 出来的，fallback == NULL，
 *   且本字体 bin 的字形区间只有 0x20-0x7E / 0x3000-0x30FF / 0x4E00-0x9FFF /
 *   0xFF00-0xFFEF —— 不含 0xF000-0xF8FF。一旦套上去，蓝牙/播放图标就会变方块。
 *
 * ★ 为什么字体可能是"晚于本页就绪"的：app_main 在 splash 之后才调用
 *   ui_binfont_init()。所以这里做成**可重复调用**：字体一到位就补套一次
 *   （见 poll_cb 顶部的 s_font_applied 守卫）。
 *
 * ★ 歌词/曲名标签的行高问题：LVGL 的 label 只做水平 text_align，竖直方向永远
 *   从内容区顶部开始画，而且**按自身 coords 裁剪**。所以歌词标签的高度必须是
 *   "正好一个行框"（SIZE_CONTENT），行距也只能来自字体的 line_height。
 *   这里把行距现算出来存进 s_lyr_pitch，动画平移就用它。 */
static void apply_fonts(void)
{
    lv_font_t *f = ui_binfont_get();

    if (s_title) {
        lv_obj_set_style_text_font(s_title, f ? f : &lv_font_cn_30, 0);
    }
    if (s_artist) {
        lv_obj_set_style_text_font(s_artist, f ? f : &lv_font_cn_20, 0);
    }

    lv_font_t *lf = f ? f : &lv_font_cn_20;
    for (int i = 0; i < LYR_LINES; i++) {
        if (s_lyr[i]) {
            lv_obj_set_style_text_font(s_lyr[i], lf, 0);
        }
    }
    s_lyr_pitch = (int)lv_font_get_line_height(lf) + LYR_GAP;
    if (s_lyr_pitch < 1) {
        s_lyr_pitch = 1;
    }

    s_font_applied = 1;
    ESP_LOGI(TAG, "fonts applied to SINK tab (%s font, lyric pitch=%d)",
             f ? "bin" : "builtin", s_lyr_pitch);
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */

static void on_prev_click(lv_event_t *e)
{
    (void)e;
    esp_err_t r = bt_sink_service_prev();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "prev failed: %s", esp_err_to_name(r));
    }
}

static void on_next_click(lv_event_t *e)
{
    (void)e;
    esp_err_t r = bt_sink_service_next();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "next failed: %s", esp_err_to_name(r));
    }
}

/* 播放/暂停：按当前已知状态发反命令。
 * ★ AVRCP 命令是**异步**的：发出去之后真实状态要靠 PLAYBACK_STATUS_CHG
 *   事件回来才知道。所以这里不本地翻转状态，等 poll 从服务里读到新状态
 *   再更新按钮图标 —— 否则对端不响应时按钮会显示错的图标。 */
static void on_play_click(lv_event_t *e)
{
    (void)e;
    bool playing = (bt_sink_service_get_state() == BT_SINK_STATE_PLAYING);
    esp_err_t r = playing ? bt_sink_service_pause() : bt_sink_service_play();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", playing ? "pause" : "play", esp_err_to_name(r));
    }
}

static void set_play_button(bool playing)
{
    if (!s_play_btn) {
        return;
    }
    lv_obj_t *lab = lv_obj_get_child(s_play_btn, 0);
    if (lab) {
        lv_label_set_text_static(lab, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

/* ------------------------------------------------------------------ */
/* 均衡器指示条                                                        */
/* ------------------------------------------------------------------ */

/* 四根柱子由**同一个** anim 驱动：v 是 0..EQ_CYCLE 的相位，
 * 每根用不同的速度倍率 + 初相位取三角波 ⇒ 看起来无规律跳动。
 * 用"一个 anim 驱动全部"而不是"四根各一个 anim"，是为了
 * 销毁时一句 lv_anim_delete(s_eq_box, ...) 就能全停干净。 */
static void eq_anim_exec(void *var, int32_t v)
{
    (void)var;
    static const int spd[EQ_BARS] = { 600, 900, 450, 750 };
    static const int off[EQ_BARS] = {   0,  70, 130,  40 };

    for (int i = 0; i < EQ_BARS; i++) {
        if (!s_eq_bar[i]) {
            continue;
        }
        int32_t ph = (v * spd[i] / 1000 + off[i]) % 200;
        int32_t t  = (ph < 100) ? ph : (200 - ph);        /* 0..100 三角波 */
        int32_t h  = EQ_H_MIN + (EQ_H_MAX - EQ_H_MIN) * t / 100;
        lv_obj_set_height(s_eq_bar[i], h);
        lv_obj_set_pos(s_eq_bar[i], EQ_X(i), EQ_BOX_H - h);   /* 底部对齐 */
    }
}

static void eq_start(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_eq_box);
    lv_anim_set_values(&a, 0, EQ_CYCLE);
    lv_anim_set_duration(&a, EQ_CYCLE);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, eq_anim_exec);
    lv_anim_start(&a);
}

/* ★ 2026-09-22：EQ 动画改为**按需起停**，不再从 build 起永久空转。
 *
 * 为什么必须这样 —— 这不是省 CPU，是省**中断**：
 *   本工程的显示是 `avoid_tearing + DIRECT` 模式，帧缓冲直接给 RGB panel；
 *   每一帧都会产生 1 次 vsync ISR + 480/BOUNCE_HEIGHT 次 bounce ISR
 *   （BOUNCE_HEIGHT=10 ⇒ 每帧 48 次）。这些 ISR 要和 BT 的 HCI/AVDTP ISR
 *   在同一核上**嵌套**，每嵌套一层就往 1536B 的**中断栈**上压 ~140B。
 *   改版前 SINK 页几乎静止（LVGL 无重绘 ⇒ 不刷帧 ⇒ 几乎没有 display ISR）；
 *   加了永久 EQ 动画后变成**只要页面可见就持续 ~30fps 刷帧**，
 *   于是"刚连上蓝牙 + 切进 SINK"这个组合直接把中断栈打穿
 *   → HW 栈保护 panic（见 sdkconfig.defaults 的 CONFIG_FREERTOS_ISR_STACKSIZE 段）。
 *
 * 所以：只有"**真的在收流** 且 **本页真的在屏幕上**"时才让动画跑。
 *   离开 SINK 页 / 没在放歌 → 一行 lv_anim_delete，重绘与 display ISR 立刻归零。
 *   （lv_obj_is_visible() 会把 LV_OBJ_FLAG_HIDDEN 沿父链一路查上去，
 *     所以 tabview 把非活动 tab 藏起来时它自动为 false，无需额外挂钩子。） */
static void eq_set_running(bool on)
{
    if (on == s_eq_running) {
        return;
    }
    if (on) {
        eq_start();
    } else {
        lv_anim_delete(s_eq_box, eq_anim_exec);
        /* 停下来时把 4 根柱子归到最低位，避免冻结在某个中间高度 */
        for (int i = 0; i < EQ_BARS; i++) {
            if (s_eq_bar[i]) {
                lv_obj_set_height(s_eq_bar[i], EQ_H_MIN);
                lv_obj_set_pos(s_eq_bar[i], EQ_X(i), EQ_BOX_H - EQ_H_MIN);
            }
        }
    }
    s_eq_running = on;
}

/* ------------------------------------------------------------------ */
/* 歌词窗口                                                            */
/* ------------------------------------------------------------------ */

/* 动画插值：v = 0..1000。整列上移恰好一个行距，同时三行各自做透明度过渡。 */
static void lyr_anim_exec(void *var, int32_t v)
{
    (void)var;
    if (!s_lyr_col || !s_lyr[0] || !s_lyr[1] || !s_lyr[2]) {
        return;
    }
    lv_obj_set_style_translate_y(s_lyr_col, -(s_lyr_pitch * v) / 1000, 0);

    int32_t o0 = LYR_OPA_DIM + (0 - LYR_OPA_DIM) * v / 1000;          /* 暗 -> 透明  */
    int32_t o1 = 255 + (LYR_OPA_DIM - 255) * v / 1000;                /* 亮 -> 暗    */
    int32_t o2 = 0 + (255 - 0) * v / 1000;                            /* 透明 -> 亮  */
    lv_obj_set_style_opa(s_lyr[0], (lv_opa_t)o0, 0);
    lv_obj_set_style_opa(s_lyr[1], (lv_opa_t)o1, 0);
    lv_obj_set_style_opa(s_lyr[2], (lv_opa_t)o2, 0);
}

/* 动画收尾：先把位移清零、再把文本整体上挪一格。
 * ★ 这两步必须一起做，否则会闪：清零前"中间格"显示的是旧当前行（暗）、
 *   清零后同一物理位置由文本上挪来接管，画面完全等价。
 * ★ s_lyr_have_prev 用来挡住"占位符变成历史歌词"：本曲第一句到达前，
 *   中间格放的是 "-- waiting for lyrics --"，它绝不能被挪到"上一行"去。 */
static void lyr_anim_done(lv_anim_t *a)
{
    (void)a;
    if (!s_lyr_col || !s_lyr[0] || !s_lyr[1] || !s_lyr[2]) {
        return;
    }
    char cur[BT_SINK_LYRIC_LEN];
    const char *p = lv_label_get_text(s_lyr[2]);
    size_t n = strlen(p);
    if (n >= sizeof(cur)) {
        n = sizeof(cur) - 1;
    }
    memcpy(cur, p, n);
    cur[n] = '\0';

    if (s_lyr_have_prev) {
        lv_label_set_text(s_lyr[0], lv_label_get_text(s_lyr[1]));  /* 上一行 <- 旧当前行 */
        lv_obj_set_style_opa(s_lyr[0], (lv_opa_t)LYR_OPA_DIM, 0);
    } else {
        lv_label_set_text(s_lyr[0], "");                           /* 本曲第一句：上面留空 */
        lv_obj_set_style_opa(s_lyr[0], LV_OPA_TRANSP, 0);
    }
    s_lyr_have_prev = true;

    lv_label_set_text(s_lyr[1], cur);                              /* 当前行 <- 刚滑入的新句 */
    lv_obj_set_style_opa(s_lyr[1], LV_OPA_COVER, 0);
    lv_label_set_text(s_lyr[2], "");                               /* 待入行清空，等下一句 */
    lv_obj_set_style_opa(s_lyr[2], LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(s_lyr_col, 0, 0);

    s_lyr_busy = false;

    /* 动画期间被挤掉的新句在这里补播，保证"一句不丢" */
    if (s_lyr_queued) {
        s_lyr_queued = false;
        lv_label_set_text(s_lyr[2], s_lyr_next);
        lv_anim_t na;
        lv_anim_init(&na);
        lv_anim_set_var(&na, s_lyr_col);
        lv_anim_set_values(&na, 0, 1000);
        lv_anim_set_duration(&na, LYR_ANIM_MS);
        lv_anim_set_path_cb(&na, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&na, lyr_anim_exec);
        lv_anim_set_completed_cb(&na, lyr_anim_done);
        lv_anim_start(&na);
        s_lyr_busy = true;
    }
}

static void lyr_start_anim(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lyr_col);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, LYR_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, lyr_anim_exec);
    lv_anim_set_completed_cb(&a, lyr_anim_done);
    lv_anim_start(&a);
    s_lyr_busy = true;
}

/* 收到新的一句歌词 */
static void lyr_push(const char *line)
{
    if (!s_lyr_col || !s_lyr[2]) {
        return;
    }
    if (s_lyr_busy) {
        /* 上一句还没滑完：先存下来，播完立刻补上（实测句间隔 2~6s，极少触发） */
        size_t n = strlen(line);
        if (n >= sizeof(s_lyr_next)) {
            n = sizeof(s_lyr_next) - 1;
        }
        memcpy(s_lyr_next, line, n);
        s_lyr_next[n] = '\0';
        s_lyr_queued = true;
        return;
    }

    lv_label_set_text(s_lyr[2], line);
    lv_obj_set_style_opa(s_lyr[2], LV_OPA_TRANSP, 0);
    /* 还没有"上一行"时（本曲第一句），别让空格子占一份暗亮度 */
    if (lv_label_get_text(s_lyr[0])[0] == '\0') {
        lv_obj_set_style_opa(s_lyr[0], LV_OPA_TRANSP, 0);
    }
    lyr_start_anim();
}

/* 换曲 / 尚无歌词时的静止态 */
static void lyr_set_idle(void)
{
    s_lyr_busy = false;
    s_lyr_queued = false;
    s_lyr_have_prev = false;
    s_lyr_next[0] = '\0';
    if (!s_lyr_col || !s_lyr[1]) {
        return;
    }
    lv_anim_delete(s_lyr_col, lyr_anim_exec);
    lv_obj_set_style_translate_y(s_lyr_col, 0, 0);
    lv_label_set_text(s_lyr[0], "");
    lv_label_set_text(s_lyr[1], "-- waiting for lyrics --");
    lv_label_set_text(s_lyr[2], "");
    lv_obj_set_style_opa(s_lyr[0], LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_lyr[1], (lv_opa_t)110, 0);
    lv_obj_set_style_opa(s_lyr[2], LV_OPA_TRANSP, 0);
}

/* 轮询：只在 seq 前进时动作（每句歌词一次，不是每 500ms 一次） */
static void update_lyrics(void)
{
    static bt_sink_lyric_t ly;      /* 132B，同理不下栈（见 update_meta） */

    if (!bt_sink_service_get_lyric(&ly) || ly.seq == s_lyric_seq) {
        return;
    }
    s_lyric_seq = ly.seq;
    if (ly.text[0] == '\0') {
        lyr_set_idle();      /* 换曲通知：服务把歌词清了并推进了 seq */
        return;
    }
    lyr_push(ly.text);
}

/* ------------------------------------------------------------------ */
/* 轮询（LVGL 线程，500ms）                                            */
/* ------------------------------------------------------------------ */

static void update_status(void)
{
    const char *txt;
    lv_color_t col;

    bool connected = bt_audio_service_is_connected();
    bool streaming = bt_sink_service_is_streaming();
    bt_sink_state_t st = bt_sink_service_get_state();

    if (!connected) {
        txt = LV_SYMBOL_BLUETOOTH "  WAITING  |  connect from your phone";
        col = COL_CYAN;
    } else if (streaming) {
        txt = LV_SYMBOL_PLAY "  RECEIVING  |  phone -> speaker";
        col = COL_GREEN;
    } else {
        switch (st) {
        case BT_SINK_STATE_PAUSED:
            txt = LV_SYMBOL_PAUSE "  CONNECTED  |  paused";
            col = COL_CYAN;
            break;
        case BT_SINK_STATE_STOPPED:
            txt = LV_SYMBOL_STOP "  CONNECTED  |  stopped";
            col = COL_SUBTEXT;
            break;
        default:
            txt = LV_SYMBOL_OK "  CONNECTED  |  idle";
            col = COL_GREEN;
            break;
        }
    }

    lv_obj_set_style_bg_color(s_dot, col, 0);
    lv_label_set_text_static(s_state, txt);
    set_hidden(s_eq_box, !streaming);
    /* ★ 先 set_hidden 再判可见性：EQ 只在"收流中 + 本页在屏幕上"时才让动画跑。
     *   离开这一页就停，重绘和 display ISR 一起归零（理由见 eq_set_running）。 */
    eq_set_running(streaming && lv_obj_is_visible(s_eq_box));

    if (st != s_state_last) {
        s_state_last = st;
        set_play_button(st == BT_SINK_STATE_PLAYING);
    }
}

/* 只画曲名 + 歌手。专辑/流派不再占版面（UI 刻意留白给歌词）。
 * ★ 这里拿到的 title 是服务**锁住的真曲名**，不是对端最后那条 TITLE
 *   —— 后者在对方开"车载蓝牙歌词"时会是歌词行。
 * ★ 只在真的变了才 lv_label_set_text：否则 500ms 一次重设文本会给
 *   SCROLL_CIRCULAR 的曲名标签反复重启滚动动画（长曲名永远滚不起来），
 *   而且每 500ms 白做一次堆分配。 */
static void update_meta(void)
{
    /* ★ 两个副本缓冲是 static 不是栈上局部：bt_sink_meta_t = 448B，
     *   放栈上会给 LVGL 任务每次 poll 都压 448B（该任务栈本就吃紧）。 */
    static char last_title[BT_SINK_TITLE_LEN];
    static char last_artist[BT_SINK_ARTIST_LEN];
    static bt_sink_meta_t m;

    const char *t, *a;

    if (!bt_sink_service_get_meta(&m)) {
        t = "-- no track info --";
        a = "--";
    } else {
        t = m.title[0]  ? m.title  : "-- no title --";
        a = m.artist[0] ? m.artist : "--";
    }

    if (strcmp(t, last_title) != 0) {
        size_t n = strlen(t);
        if (n >= sizeof(last_title)) {
            n = sizeof(last_title) - 1;
        }
        memcpy(last_title, t, n);
        last_title[n] = '\0';
        lv_label_set_text(s_title, last_title);
    }
    if (strcmp(a, last_artist) != 0) {
        size_t n = strlen(a);
        if (n >= sizeof(last_artist)) {
            n = sizeof(last_artist) - 1;
        }
        memcpy(last_artist, a, n);
        last_artist[n] = '\0';
        lv_label_set_text(s_artist, last_artist);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;

    /* 字体可能比本页晚就绪：一到位就补套一次 */
    if (!s_font_applied) {
        apply_fonts();
    }

    bt_role_t role = bt_audio_service_get_role();
    bool bt_on = bt_audio_service_is_on();

    /* ---- 门禁判定：不满足条件就只显示一张说明卡，媒体区整块藏掉 ---- */
    if (role != BT_ROLE_SINK) {
        lv_label_set_text_static(s_gate_title, LV_SYMBOL_BLUETOOTH "  NOT IN SINK MODE");
        lv_label_set_text_static(s_gate_hint,
            "This page needs the A2DP SINK role.\n"
            "Switch it with the role button in\n"
            "SYS > Bluetooth. (Switching will drop\n"
            "any active Bluetooth link.)");
        set_hidden(s_gate, false);
        set_hidden(s_media, true);
        return;
    }
    if (!bt_on) {
        lv_label_set_text_static(s_gate_title, LV_SYMBOL_BLUETOOTH "  BLUETOOTH OFF");
        lv_label_set_text_static(s_gate_hint,
            "Turn Bluetooth on in SYS > Bluetooth,\n"
            "then pair from your phone.\n"
            "This device will be discoverable\n"
            "as a speaker (A2DP Sink).");
        set_hidden(s_gate, false);
        set_hidden(s_media, true);
        return;
    }

    set_hidden(s_gate, true);
    set_hidden(s_media, false);
    update_status();
    update_meta();
    update_lyrics();
}

/* ------------------------------------------------------------------ */
/* 构建                                                                */
/* ------------------------------------------------------------------ */

/* 圆形图标按钮：播放键用青色实底，上/下一首用青色描边。
 * ★ 图标字体用 montserrat_28 —— FontAwesome 的 LV_SYMBOL_* 只在
 *   montserrat 系列里，28px 在 48~56px 的圆里视觉比例正好。 */
static lv_obj_t *circle_button(lv_obj_t *parent, int d, const char *sym, bool accent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, d, d);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    if (accent) {
        lv_obj_set_style_bg_color(btn, COL_CYAN, 0);
        lv_obj_set_style_bg_color(btn, COL_ACCENT_DIM, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(btn, COL_ACCENT_DIM, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, COL_CYAN, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
    }

    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text_static(lab, sym);
    lv_obj_set_style_text_color(lab, accent ? COL_DARK : COL_CYAN, 0);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_28, 0);
    lv_obj_center(lab);
    return btn;
}

void tab_btsink_build(lv_obj_t *parent, lv_group_t *g)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_set_style_pad_top(parent, 14, 0);
    lv_obj_set_style_pad_bottom(parent, 10, 0);

    /* ============ ① 门禁卡片 ============ */
    s_gate = ui_common_card(parent, CARD_W, 220, 16);
    lv_obj_set_flex_flow(s_gate, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_gate, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_gate, 14, 0);

    s_gate_title = lv_label_create(s_gate);
    lv_label_set_text_static(s_gate_title, LV_SYMBOL_BLUETOOTH "  NOT IN SINK MODE");
    lv_obj_set_style_text_color(s_gate_title, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_gate_title, UI_FONT, 0);

    s_gate_hint = lv_label_create(s_gate);
    lv_label_set_text_static(s_gate_hint, "");
    lv_label_set_long_mode(s_gate_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_gate_hint, LV_PCT(90));
    lv_obj_set_style_text_align(s_gate_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_gate_hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_gate_hint, UI_FONT, 0);

    /* ============ ② 媒体区：顶栏 + 正在播放卡 + 控制行 ============ */
    s_media = transparent_box(parent, CARD_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_media, 1);
    lv_obj_set_flex_flow(s_media, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_media, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_media, 10, 0);

    /* --- 顶栏：左「状态灯 + 状态文本」/ 右「均衡器条」 --- */
    lv_obj_t *header = ui_common_card(s_media, CARD_W, HEADER_H, 12);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 16, 0);

    lv_obj_t *hleft = transparent_box(header, LV_SIZE_CONTENT, 22);
    lv_obj_set_flex_flow(hleft, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hleft, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hleft, 10, 0);

    s_dot = lv_obj_create(hleft);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, COL_SUBTEXT, 0);
    lv_obj_set_style_pad_all(s_dot, 0, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_dot, LV_SCROLLBAR_MODE_OFF);

    s_state = lv_label_create(hleft);
    lv_label_set_text_static(s_state, LV_SYMBOL_BLUETOOTH "  WAITING");
    lv_obj_set_style_text_color(s_state, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_state, UI_FONT, 0);

    /* 均衡器指示：4 根柱子，只在"正在收流"时露出来 */
    s_eq_box = transparent_box(header, EQ_BOX_W, EQ_BOX_H);
    for (int i = 0; i < EQ_BARS; i++) {
        s_eq_bar[i] = lv_obj_create(s_eq_box);
        lv_obj_set_size(s_eq_bar[i], EQ_BAR_W, EQ_H_MIN);
        lv_obj_set_pos(s_eq_bar[i], EQ_X(i), EQ_BOX_H - EQ_H_MIN);
        lv_obj_set_style_radius(s_eq_bar[i], 2, 0);
        lv_obj_set_style_border_width(s_eq_bar[i], 0, 0);
        lv_obj_set_style_bg_color(s_eq_bar[i], COL_CYAN, 0);
        lv_obj_set_style_pad_all(s_eq_bar[i], 0, 0);
        lv_obj_remove_flag(s_eq_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_eq_bar[i], LV_SCROLLBAR_MODE_OFF);
    }
    set_hidden(s_eq_box, true);
    /* ★ 这里**故意不启** EQ 动画：由 poll_cb → update_status → eq_set_running
     *   按"收流中 && 本页可见"按需起停（理由见 eq_set_running 的注释）。 */
    s_eq_running = false;

    /* --- 正在播放卡：曲名 / 歌手居中，下面挂歌词窗口 --- */
    lv_obj_t *now = ui_common_card(s_media, CARD_W, LV_SIZE_CONTENT, 16);
    lv_obj_set_flex_grow(now, 1);
    lv_obj_remove_flag(now, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(now, 12, 0);
    lv_obj_set_style_pad_row(now, 4, 0);

    s_title = lv_label_create(now);
    lv_label_set_text_static(s_title, "-- no track info --");
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_cn_30, 0);

    s_artist = lv_label_create(now);
    lv_label_set_text_static(s_artist, "--");
    lv_obj_set_width(s_artist, LV_PCT(100));
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
    /* 歌手和曲名同为 28px（bin 字体只有一个字号），靠颜色 + 一点透明度拉开层次 */
    lv_obj_set_style_text_color(s_artist, COL_CYAN, 0);
    lv_obj_set_style_opa(s_artist, (lv_opa_t)200, 0);
    lv_obj_set_style_text_font(s_artist, &lv_font_cn_20, 0);

    /* --- 歌词窗口：三行裁剪窗，整列上下平移做出"逐句"的滚动 ---
       ★ 窗口高度 = flex_grow 吃掉剩余空间。三行组成的列在窗口里垂直居中，
         上滑一个行距后最上面那行自然被窗口裁掉。
       ★ 列高（3 × 行框）由字体行高决定，窗口足够大时上下都有余量；
         万一空间不够，被裁的也只会是"上一行"和"待入行"的边缘，
         中间那行（当前句）永远完整。 */
    s_lyr_box = transparent_box(now, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_lyr_box, 1);

    s_lyr_col = transparent_box(s_lyr_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lyr_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lyr_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_lyr_col, LYR_GAP, 0);
    lv_obj_center(s_lyr_col);

    for (int i = 0; i < LYR_LINES; i++) {
        s_lyr[i] = lv_label_create(s_lyr_col);
        /* ★ 宽度撑满，高度交给 SIZE_CONTENT（正好一个行框）——
           绝不能写死行高，见文件头 apply_fonts 的说明。 */
        lv_obj_set_width(s_lyr[i], LV_PCT(100));
        /* ★ 必须 DOTS 不能 WRAP：回绕会把这行撑成两行高，三行窗口就歪了 */
        lv_label_set_long_mode(s_lyr[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(s_lyr[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_lyr[i], COL_TEXT, 0);
        lv_obj_set_style_text_font(s_lyr[i], &lv_font_cn_20, 0);
        lv_obj_set_style_opa(s_lyr[i], LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(s_lyr[i], LV_SCROLLBAR_MODE_OFF);
    }
    /* 字体还没就绪时也给一个合理的行距，避免动画距离为 0 */
    s_lyr_pitch = (int)lv_font_get_line_height(&lv_font_cn_20) + LYR_GAP;
    lyr_set_idle();

    /* --- 控制行：上一首 / 播放暂停 / 下一首（圆形） --- */
    lv_obj_t *ctrl = transparent_box(s_media, CARD_W, CTRL_H);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 40, 0);

    s_prev_btn = circle_button(ctrl, 48, LV_SYMBOL_PREV, false);
    lv_obj_add_event_cb(s_prev_btn, on_prev_click, LV_EVENT_CLICKED, NULL);

    s_play_btn = circle_button(ctrl, 56, LV_SYMBOL_PLAY, true);
    lv_obj_add_event_cb(s_play_btn, on_play_click, LV_EVENT_CLICKED, NULL);

    s_next_btn = circle_button(ctrl, 48, LV_SYMBOL_NEXT, false);
    lv_obj_add_event_cb(s_next_btn, on_next_click, LV_EVENT_CLICKED, NULL);

    if (g) {
        lv_group_add_obj(g, s_play_btn);
        lv_group_add_obj(g, s_prev_btn);
        lv_group_add_obj(g, s_next_btn);
    }

    /* 首帧：先按当前状态判一次门禁，避免"闪一下媒体区" */
    if (!s_font_applied) {
        apply_fonts();
    }
    set_hidden(s_media, true);
    set_hidden(s_gate, false);

    if (!s_timer) {
        s_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
    }
    poll_cb(NULL);

    ESP_LOGI(TAG, "sink tab built (no cover, centered meta, lyric ticker)");
}

void tab_btsink_destroy(void)
{
    /* ★ 动画必须在对象被删**之前**停掉：lv_anim 里存的是裸指针，
       对象没了动画还在跑就是 use-after-free。父容器统一删对象树，
       所以这一步只能由我们在这里做。 */
    if (s_lyr_col) {
        lv_anim_delete(s_lyr_col, lyr_anim_exec);
    }
    if (s_eq_box) {
        lv_anim_delete(s_eq_box, eq_anim_exec);
    }
    if (s_title) {
        lv_anim_delete(s_title, NULL);   /* SCROLL_CIRCULAR 的自带动画 */
    }
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }

    s_gate = s_gate_title = s_gate_hint = NULL;
    s_dot = s_state = NULL;
    s_eq_box = NULL;
    for (int i = 0; i < EQ_BARS; i++) {
        s_eq_bar[i] = NULL;
    }
    s_title = s_artist = NULL;
    s_lyr_box = s_lyr_col = NULL;
    for (int i = 0; i < LYR_LINES; i++) {
        s_lyr[i] = NULL;
    }
    s_play_btn = s_prev_btn = s_next_btn = NULL;

    s_lyr_busy = false;
    s_lyr_queued = false;
    s_lyr_have_prev = false;
    s_lyr_next[0] = '\0';
    s_lyr_pitch = 0;
    s_lyric_seq = 0;
    s_state_last = BT_SINK_STATE_IDLE;
    s_font_applied = 0;
    s_eq_running = false;
}
