/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tab_video.c
 * @brief VIDEO tab: SD-card media capture (still photo / video recording)
 *        + 实时取景器（2026-09-25 新增，用户需求："就跟正常的相机设备一样，
 *          原画面可见，拍摄、录视频的原画面可预览"）。
 *
 * ★ 本页**不自己碰 V4L2、不自己建任务、不自己开文件** —— 它只是中间层服务的
 *   遥控器 + 仪表盘 + 取景器显示端：
 *     - capture_service        （S2a，单次拍照事务，"接管 → 抓帧 → 落盘 → 归还"）
 *     - record_video_service   （S2b，MJPEG AVI 录像）
 *     - viewfinder             （取景器：硬件解码 720p JPEG → 320x180 RGB565）
 *   ⇒ 页面刷新（50 ms）只做"读状态 → 画字/贴帧"，任何耗时动作都投给服务里的任务。
 *     这是本工程的线程铁律：LVGL 线程绝不做阻塞 IO。
 *
 * ★★ 取景器的数据通路（本页只做最后一格）：
 *       cam_task ──观察者──> viewfinder(限速+memcpy) ──> vf任务(硬解码+抽点)
 *                                                    └─> viewfinder_take() ← 本页
 *   关键点：viewfinder 注册的是 camera_service 的**观察者**，所以无论此刻跑的是
 *   本页的取景流、拍照流还是录像流，取景器都能持续刷新 —— 拍照/录像期间画面不冻结。
 *
 * ★ 与 CAM tab 的关系（方案 §2.5.1 / §2.5.5）：
 *   - CAM  = RGB565 240x240 预览（模型 / 人脸检测）
 *   - VIDEO = JPEG 1280x720 媒体采集 + 取景
 *   传感器同一时刻只能有一种格式 ⇒ 取景流由**本页活跃与否**决定起停
 *   （tab_video_set_active，见下），拍照/录像仍走"接管 → 归还"事务。
 *
 * ★ 布局（F46 教训：tabview 的"页"对象自带 pad_normal=12，内容区实际 ≈776x408；
 *   子对象越界会被裁）⇒ 本页**绝对定位**（flex 会跟"点击放大"的 lv_obj_set_pos 抢定位权）。
 *   2026-09-26 用户要求"取景框居中、信息简单明了"⇒ 改为居中纵向流：
 *     (0,0)        标题                 (右上) 状态行（顶右对齐）
 *     (228,60)     取景画布 320x180（水平居中）
 *     (88,272)     信息两行 600 宽（居中文本，无卡片框）
 *     (90,316)     按钮行 596x60（4 键，水平居中）
 *   放大态：画布移到 (228,114) 并 transform_zoom 512（256=1x）⇒ 渲染成 640x360，
 *   中心正好落在页中心 (388,204)，覆盖 x 68..708 / y 24..384 不越界；标题/状态/信息/
 *   按钮全部隐藏，画布下方浮现"tap again to exit"提示。再点一次还原。
 */

#include "lvgl.h"

#include "tab_video.h"
#include "ui_theme.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/capture_service.h"
#include "middleware/camera/viewfinder.h"
#include "middleware/media/record_video_service.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "middleware/mem_diag.h"
#include <string.h>

#define TAG "TAB_VID"

/* ---------------------------------------------------------------------------
 *  布局常量（页内容区 = 776 x 408，见文件头；UI_FONT = montserrat_14，行高 ~17）
 * ------------------------------------------------------------------------- */
#define VF_X            ((776 - 320) / 2)   /* 228：取景框水平居中 */
#define VF_Y            60
#define VF_ZOOM_X       228         /* 放大态画布左上角：渲染中心 = 页中心(388,204) */
#define VF_ZOOM_Y       114
#define INFO_W          600
#define INFO_X          ((776 - INFO_W) / 2)    /* 88 */
#define INFO_Y          272
#define BTNROW_W        292         /* ★ §73.12：Q + PREVIEW 两个按钮 ×140 + 12（拍照/录像已移到画面右侧竖排） */
#define BTNROW_H        60
#define BTNROW_X        ((776 - BTNROW_W) / 2)  /* 242 */
#define BTNROW_Y        316
/* ★ §73.12：拍照/录像两颗圆形按钮 —— 移到取景画面**右侧竖排**（相机 App 布局）。
 *  画布右边缘 x=548（VF_X+320），右侧余量 548..776 共 228 px；按钮 72 圆，
 *  x = 590（右区水平居中 548+(228-72)/2=626 太靠边，取 590 视觉更均衡）。
 *  竖排：两颗 + 20 间距 = 164，画布高 180 ⇒ 顶起 y=68 上下各留 8。 */
#define SIDE_BTN_SIZE   72
#define SIDE_X          590
#define SIDE_CAP_Y      68
#define SIDE_REC_Y      (SIDE_CAP_Y + SIDE_BTN_SIZE + 20)   /* 160 */
#define STATUS_HOLD_TICKS  40       /* 按键提示保持 40×50ms = 2s，不被状态机冲掉 */

/* ---------------------------------------------------------------------------
 *  控件句柄
 * ------------------------------------------------------------------------- */
static lv_obj_t *s_canvas;          /* 取景画布（数据源由 viewfinder 提供） */
static uint16_t *s_canvas_own;      /* 首帧到来前的占位缓冲（黑屏） */
static lv_obj_t *s_cap_btn;
static lv_obj_t *s_cap_label;
static lv_obj_t *s_rec_btn;
static lv_obj_t *s_rec_label;
static lv_obj_t *s_q_btn;
static lv_obj_t *s_q_label;
static lv_obj_t *s_vf_btn;          /* ★ PREVIEW 开关（用户要求：预览要有按钮开关） */
static lv_obj_t *s_vf_btn_label;
static lv_obj_t *s_status;      /* 一行状态（顶右对齐）：**只由 refresh 写**（单一真相源） */
static lv_obj_t *s_info;        /* 信息两行（画布下方居中） */
static lv_obj_t *s_hint;        /* 放大态的"tap again to exit"提示（画布之上） */
static lv_obj_t *s_title;
static lv_obj_t *s_btn_row;
static lv_timer_t *s_timer;

static bool s_active;           /* 本页是否活跃（由 page_main 通知） */
static bool s_zoomed;
static bool s_vf_err;           /* viewfinder 起不来（一次性提示，避免刷屏） */
/* ★ 按键回调写的临时提示（"JPEG quality -> Q24" 等）保持 2s 不被状态机覆盖。
 *   根因：refresh 每 50ms 无条件重写状态行 ⇒ 回调写的字下一拍就没了（用户看到
 *   的就是"提示一闪而过/好像没反馈"）。40 拍 ×50ms = 2s。 */
static uint8_t s_status_hold;
/* ★ PREVIEW 开关（用户要求"设置一个按钮负责打开和关闭预览"）。
 *   只管**空闲取景流**；拍照/录像期间的取景画面同样受它管 —— 关了就是"无预览"，
 *   画布停在最后一帧。★ 默认**关**（2026-09-26 用户拍板：进页面不自动占相机，
 *   画布黑屏占位，按 PREVIEW 才起流）。跨 tab 切换/页面重建都保留用户的选择。 */
static bool s_preview_on = false;

/** JPEG 质量档位循环表（OV3660 语义：**值越小质量越高、码流越大**，见 camera_service.h）。
 *  ★ 留着这个按钮是为了上机时能**当场观察"质量 ↔ 帧长"**：720p 帧长到底多大是
 *    方案 §2.5.7 风险 1 的未实测项，而这个按钮让那笔账一眼可算。 */
static const int s_q_table[] = { CAM_JPEG_Q_BEST, CAM_JPEG_Q_DEFAULT, 24, CAM_JPEG_Q_WORST };
static int s_q_idx = 1;         /* 默认落在 CAM_JPEG_Q_DEFAULT(=8) */

/* ---------------------------------------------------------------------------
 *  取景画布：只做"有新帧就把 viewfinder 的缓冲交给 canvas"
 * ------------------------------------------------------------------------- */
static void vf_feed(void)
{
    viewfinder_frame_t f;
    if (!viewfinder_take(&f)) return;
    if (s_canvas == NULL || f.buf == NULL) return;

    /* ★ 零拷贝：lv_canvas_set_buffer 只是把指针写进 canvas 内嵌的 static draw_buf
     *   （lv_canvas.c:73-93，无堆分配），随后 set_src + invalidate ⇒ 下一拍重绘。
     *   传 const 进来再转 void* 是安全的：LVGL 只读这块缓冲，写它的是 viewfinder。
     *   ★ 双缓冲由 viewfinder 负责（两个 115 KB 缓冲轮换）⇒ 这里读到的一定是
     *   一块"本帧已完成"的画面，不会读到写了一半的。 */
    lv_canvas_set_buffer(s_canvas, (void *)f.buf, (int32_t)f.w, (int32_t)f.h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(s_canvas);
}

/* ---------------------------------------------------------------------------
 *  刷新（LVGL 定时器，50 ms）—— 文案/颜色的**唯一**写入点
 * ------------------------------------------------------------------------- */
static void refresh(lv_timer_t *t)
{
    (void)t;

    vf_feed();

    const cam_owner_t   owner  = camera_service_owner();
    const video_rec_state_t rstate = record_video_service_state();
    const capture_state_t   cstate = capture_service_state();

    const bool recording  = (rstate == VIDEO_REC_RECORDING);
    const bool finalizing = (rstate == VIDEO_REC_FINALIZING);
    const bool capturing  = (cstate == CAPTURE_BUSY);

    /* ---- ① 按钮可用性：只看**我们自己的**忙状态 ----
     * ★ 收尾（FINALIZING）期间两个按钮都必须停：这一刻正在重扫 movi 生成 idx1，
     *   再起一轮会把同一个 FILE 交给两个写者。 */
    {
        lv_obj_t *btns[3] = { s_cap_btn, s_rec_btn, s_q_btn };
        const bool block_cap = (recording || finalizing || capturing);
        const bool block_rec = (capturing || finalizing);
        const bool blocks[3] = { block_cap, block_rec, (recording || finalizing || capturing) };
        for (int i = 0; i < 3; i++) {
            if (!btns[i]) continue;
            if (blocks[i]) lv_obj_add_state(btns[i], LV_STATE_DISABLED);
            else           lv_obj_remove_state(btns[i], LV_STATE_DISABLED);
        }
    }

    /* ---- ② 状态行（顶右对齐）----
     * ★ 顺序即优先级：**先**说"正在发生的危险事"（保存中 / 录制中），
     *   再说别的。用户最需要知道的是"现在能不能拔卡"。
     * ★ s_status_hold > 0 时跳过状态机：按键回调刚写的提示要保持满 2s。 */
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, 0);   /* 文本宽度会变，每拍重新锚右 */
    if (s_status_hold) {
        s_status_hold--;
    } else if (finalizing) {
        lv_label_set_text_static(s_status, "SAVING - do NOT remove the card");
        lv_obj_set_style_text_color(s_status, COL_RED, 0);
    } else if (recording) {
        lv_label_set_text_static(s_status, "RECORDING (live view on)");
        lv_obj_set_style_text_color(s_status, COL_RED, 0);
    } else if (capturing) {
        lv_label_set_text_static(s_status, "Capturing one frame...");
        lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    } else if (s_vf_err) {
        lv_label_set_text_static(s_status, "Viewfinder init FAILED (see log)");
        lv_obj_set_style_text_color(s_status, COL_RED, 0);
    } else if (!s_preview_on) {
        lv_label_set_text_static(s_status, "Preview OFF - tap PREVIEW to enable");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    } else if (owner == CAM_OWNER_NONE) {
        lv_label_set_text_static(s_status, "Starting viewfinder...");
        lv_obj_set_style_text_color(s_status, COL_SUBTEXT, 0);
    } else {
        lv_label_set_text_static(s_status, "Live");
        lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
    }

    /* ---- ③ 按钮文案 ----
     * ★ 颜色设在**按钮**上（F47：要"随状态变化的颜色通道"必须落在按钮/容器，
     *   设到子 label 的 text_color 上会因为"子对象不继承 state"而永远不生效）。
     *   label 固定 COL_DARK：红底/青底上深字才看得清。 */
    if (s_rec_label) {
        /* ★ §73.12：按钮已是 72 圆 —— 文案用短词（"STOP REC" 太长会被圆边裁掉）。
         *  录制中 = 红色（录制语义），空闲 = 绿色；按下的青色由 LV_STATE_PRESSED
         *  样式自动叠加（见 build 注释），这里不用管。 */
        const char *want = recording ? "STOP" : "REC";
        lv_obj_set_style_bg_color(s_rec_btn, recording ? COL_RED : COL_GREEN, 0);
        const char *cur  = lv_label_get_text(s_rec_label);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_static(s_rec_label, want);
        }
    }
    if (s_cap_label) {
        const char *want = capturing ? "..." : LV_SYMBOL_IMAGE;
        const char *cur  = lv_label_get_text(s_cap_label);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_static(s_cap_label, want);
        }
    }
    if (s_vf_btn_label) {
        const char *want = s_preview_on ? "PREVIEW ON" : "PREVIEW OFF";
        const char *cur  = lv_label_get_text(s_vf_btn_label);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_static(s_vf_btn_label, want);
            /* F47：随状态变的颜色通道设在**按钮**上，不设在子 label。 */
            lv_obj_set_style_bg_color(s_vf_btn, s_preview_on ? COL_CYAN : COL_BORDER, 0);
        }
    }
    if (s_q_label) {
        /* ★ 必须用 set_text_fmt（拷贝语义），不能用 set_text_static ——
         *   后者只存指针，而 want 是栈数组，函数一返回就是悬垂指针（F50）。
         *   ★ 标签从 "Q8" 改成 "JPEG Q8"：用户问"Q8 是干什么的"就是它自解释失败。 */
        const char *cur = lv_label_get_text(s_q_label);
        char want[16];
        lv_snprintf(want, sizeof(want), "JPEG Q%d", s_q_table[s_q_idx]);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_fmt(s_q_label, "JPEG Q%d", s_q_table[s_q_idx]);
        }
    }

    /* ---- ④ 信息两行（简单明了，2026-09-26 用户要求）----
     * 只放用户要看的：正在录什么 / 上次结果。JPEG Q 值**常驻**在这里（换档
     * 立刻可见）；vf 诊断与文件路径撤掉 —— 排查看日志，别占屏幕。 */
    if (recording) {
        video_rec_stats_t st;
        record_video_service_get_stats(&st);
        lv_label_set_text_fmt(s_info,
                              "REC  %02u:%02u    %u KB    %u.%02u fps\n"
                              "max frame %u KB    JPEG Q%d",
                              (unsigned)(st.seconds / 60u), (unsigned)(st.seconds % 60u),
                              (unsigned)(st.bytes / 1024u),
                              (unsigned)(st.fps_x100 / 100u), (unsigned)(st.fps_x100 % 100u),
                              (unsigned)(st.max_frame / 1024u),
                              s_q_table[s_q_idx]);
    } else if (finalizing) {
        lv_label_set_text_static(s_info,
                                 "SAVING (rebuilding index)...\n"
                                 "Do NOT remove the card.");
    } else if (capturing) {
        lv_label_set_text_static(s_info,
                                 "Capturing one frame...\n"
                                 "1280x720 JPEG");
    } else {
        /* 空闲：两个服务各自的上一次结果 + 当前 JPEG 质量档 */
        capture_result_t   cr;
        video_rec_result_t vr;
        capture_service_get_result(&cr);
        record_video_service_get_result(&vr);

        char cap_line[64];
        if (cr.err == ESP_OK && cr.bytes > 0) {
            lv_snprintf(cap_line, sizeof(cap_line), "photo: %u KB in %u ms",
                        (unsigned)(cr.bytes / 1024u), (unsigned)cr.elapsed_ms);
        } else if (cr.err != ESP_OK) {
            lv_snprintf(cap_line, sizeof(cap_line), "photo: FAILED (%s)",
                        esp_err_to_name(cr.err));
        } else {
            lv_snprintf(cap_line, sizeof(cap_line), "photo: -");
        }

        char vid_line[64];
        if (vr.path[0] != '\0') {
            lv_snprintf(vid_line, sizeof(vid_line),
                        "video: %u frames, %u KB, %u.%02u fps",
                        (unsigned)vr.frames, (unsigned)(vr.bytes / 1024u),
                        (unsigned)(vr.fps_x100 / 100u), (unsigned)(vr.fps_x100 % 100u));
        } else {
            lv_snprintf(vid_line, sizeof(vid_line), "video: -");
        }

        lv_label_set_text_fmt(s_info, "JPEG Q%d    %s\n%s",
                              s_q_table[s_q_idx], cap_line, vid_line);
    }
}

/* ---------------------------------------------------------------------------
 *  事件回调：只发动作、不写文案（文案由 refresh 统一负责）
 * ------------------------------------------------------------------------- */
/* ★ §73.12：拍照 = **按下即拍**（LV_EVENT_PRESSED，不再等抬起） */
static void cap_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = capture_service_request();
    if (err == ESP_OK) {
        lv_label_set_text_static(s_status, "Capturing one frame...");
    } else if (err == ESP_ERR_NO_MEM) {
        lv_label_set_text_static(s_status, "Capture FAILED (internal RAM tight)");
    } else {
        /* INVALID_STATE 的两种成因：SD 卡没挂载 / 上一轮还有媒体会话在跑。
         * ★ 不硬猜是哪一种 —— 由 refresh 下一拍把真实状态画出来，这里只给一句提示。 */
        lv_label_set_text_static(s_status, "Capture refused (SD card? busy?)");
    }
    s_status_hold = STATUS_HOLD_TICKS;  /* 提示保持 2s，别被状态机一拍冲掉 */
}

/* ★ §73.12：录像 = **按住录、松手停**（PRESSED 起录，RELEASED/PRESS_LOST 收）。
 *  ⚠ PRESS_LOST 必须处理：手指滑出按钮松开时 RELEASED 不会发 ——
 *    漏了它就是"松手了还在录"，只能进页面再按一次才能救。 */
static void rec_btn_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (record_video_service_is_recording()) return;    /* 双保险：已录则忽略 */
    esp_err_t err = record_video_service_start();
    if (err == ESP_OK) {
        lv_label_set_text_static(s_status, "RECORDING (release to stop)");
    } else if (err == ESP_ERR_NO_MEM) {
        lv_label_set_text_static(s_status, "REC FAILED (internal RAM tight)");
    } else {
        lv_label_set_text_static(s_status, "REC refused (mount SD card first)");
    }
    s_status_hold = STATUS_HOLD_TICKS;
}

static void rec_btn_released_cb(lv_event_t *e)
{
    (void)e;
    if (!record_video_service_is_recording()) return;   /* 起录被拒过：无东西可停 */
    esp_err_t err = record_video_service_stop();
    if (err == ESP_OK) {
        lv_label_set_text_static(s_status, "SAVING - do NOT remove the card");
    } else {
        lv_label_set_text_static(s_status, "Stop refused");
    }
    s_status_hold = STATUS_HOLD_TICKS;
}

static void q_btn_cb(lv_event_t *e)
{
    (void)e;
    s_q_idx = (s_q_idx + 1) % (int)(sizeof(s_q_table) / sizeof(s_q_table[0]));

    /* ★ 未在 JPEG 档时 set_jpeg_quality() 会返回 ESP_ERR_INVALID_STATE，
     *   但值**已经记下**，下次起 JPEG 流自动生效 ⇒ 这里不当失败处理。
     *   refresh() 下一拍会把新档位画到按钮和info上（info 里的 Q 常驻显示）。 */
    camera_service_set_jpeg_quality(s_q_table[s_q_idx]);
    lv_label_set_text_fmt(s_status, "JPEG quality -> Q%d (smaller = better)",
                          s_q_table[s_q_idx]);
    s_status_hold = STATUS_HOLD_TICKS;  /* 提示保持 2s，别被状态机一拍冲掉 */
}

/* ---------------------------------------------------------------------------
 *  放大 / 还原（画布点击）
 * ------------------------------------------------------------------------- */
static void canvas_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_canvas == NULL) return;

    s_zoomed = !s_zoomed;
    if (s_zoomed) {
        /* 256 = 1x（LVGL 的 zoom 是 8.8 定点）。放大 2x 后渲染 640x360，
         * 渲染中心 = 页中心 (388,204)，覆盖 x 68..708 / y 24..384 —— 不越出
         * 776x408 内容区（F46）。 */
        lv_obj_set_pos(s_canvas, VF_ZOOM_X, VF_ZOOM_Y);
        lv_obj_set_style_transform_zoom(s_canvas, 512, 0);
        lv_obj_set_style_transform_pivot_x(s_canvas, VF_W / 2, 0);
        lv_obj_set_style_transform_pivot_y(s_canvas, VF_H / 2, 0);
        /* 文字块全撤（状态行的"tap picture to exit"提示会被放大的画面盖住，
         * 改用画布之上的 hint label —— 它创建在 canvas 之后，绘制在最上层）。 */
        lv_obj_set_hidden(s_title, true);
        lv_obj_set_hidden(s_status, true);
        lv_obj_set_hidden(s_info, true);
        lv_obj_set_hidden(s_btn_row, true);
        lv_obj_set_hidden(s_hint, false);
    } else {
        lv_obj_set_style_transform_zoom(s_canvas, 256, 0);
        lv_obj_set_pos(s_canvas, VF_X, VF_Y);
        lv_obj_set_hidden(s_title, false);
        lv_obj_set_hidden(s_status, false);
        lv_obj_set_hidden(s_info, false);
        lv_obj_set_hidden(s_btn_row, false);
        lv_obj_set_hidden(s_hint, true);
    }
    lv_obj_invalidate(s_canvas);
}

/* ---------------------------------------------------------------------------
 *  取景流的起 / 停（供 tab 激活与 PREVIEW 按钮共用）
 * ------------------------------------------------------------------------- */
/* ★★ 起"空闲取景流"，**必须接管**（2026-09-25 黑屏根因）：
 *   CAM 页的预览**不会**因为切走而停（tab_camera 没有"切走就停自己"的钩子，
 *   它只在页面销毁时才停）⇒ 从 CAM 切到 VIDEO 时相机还归 PREVIEW（240x240），
 *   取景器收不到 720p 帧 ⇒ 画布永远黑屏（上机实锤：按 REC 时日志仍是
 *   `sensor switch: 240x240 -> JPEG`）。所以这里要像拍照/录像一样**显式接管**。
 *   MEDIA（正在拍照/录像）绝不能碰。
 * ★ 若在跑的 PREVIEW 流本来就是取景流（720p JPEG，例如拍照/录像结束后的
 *   restore_preview），直接返回 —— 重启会白闪一下。 */
static void vf_stream_ensure(void)
{
    if (camera_service_is_running()) {
        uint32_t w, h;
        cam_fmt_t f;
        camera_service_get_stream(&w, &h, &f);
        if (camera_service_owner() == CAM_OWNER_PREVIEW && f == CAM_FMT_JPEG) {
            return;
        }
    }
    if (camera_service_owner() == CAM_OWNER_PREVIEW && camera_service_is_running()) {
        esp_err_t se = camera_service_stop();
        if (se != ESP_OK) {
            ESP_LOGE(TAG, "take over CAM preview failed: %s", esp_err_to_name(se));
            return;
        }
        ESP_LOGI(TAG, "took over CAM preview (240x240 was still running)");
    }
    if (camera_service_owner() == CAM_OWNER_NONE && !camera_service_is_running()) {
        esp_err_t err = camera_service_start_fmt_owner(NULL, NULL, CAM_FMT_JPEG,
                                                       CAM_MEDIA_W, CAM_MEDIA_H,
                                                       CAM_OWNER_PREVIEW);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start viewfinder stream failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "viewfinder stream on (720p JPEG, owner=PREVIEW)");
        }
    }
}

/* 停取景流：**只停归 PREVIEW 所有的**。MEDIA（正在拍照/录像）绝不能掐 ——
 * 那是"切走再切回录像不中断"的承诺。 */
static void vf_stream_stop(void)
{
    if (camera_service_owner() == CAM_OWNER_PREVIEW && camera_service_is_running()) {
        camera_service_stop();
        ESP_LOGI(TAG, "viewfinder stream off");
    }
}

/* ★★ 关取景 = ①让画布松开 viewfinder 的缓冲 ②停收帧 ③把 ~2.93 MB 还给系统。
 *
 *  为什么顺序不能颠倒（2026-09-28 按需化引入）：
 *    `lv_canvas_set_buffer()` 是**零拷贝**（只把指针写进 canvas 内嵌 draw_buf），
 *    vf_feed() 已经把 s_canvas 指到了 viewfinder 的 s_disp[]。先 release 的话，
 *    s_disp 被 free 而画布仍指着它 ⇒ 下一拍重绘就是 UAF。所以必须**先换回自持缓冲**。
 *
 *  ★ 保留画面：release 前把"最后一帧"拷进 s_canvas_own（自持、常驻），
 *    这样既释放了 viewfinder 的双缓冲，画面也不会变黑 —— 用户仍看得见最后一帧。
 *
 *  ★ 必须 LVGL 线程调用（canvas 归它独占）；幂等。 */
static void vf_release_all(void)
{
    if (s_canvas != NULL && s_canvas_own != NULL) {
        const uint16_t *last = viewfinder_last_frame();
        if (last != NULL) {
            memcpy(s_canvas_own, last, VF_W * VF_H * 2);    /* 两块都是 320x180 RGB565 */
        }
        lv_canvas_set_buffer(s_canvas, s_canvas_own, VF_W, VF_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_canvas);
    }
    viewfinder_disable();               /* 先停任务（幂等）——release 内部也会再调一次 */
    viewfinder_release();               /* 释放 s_dec_out / s_stage / s_disp[] */
}

/* 开取景器 = 起流 → 申请缓冲 → 挂观察者。返回 false 表示取景器自身起不来。
 *
 * ★★ §73.16（2026-09-29 晚）时序反转：**先起流，后 acquire 三件套**。
 *   旧顺序（acquire → enable → 起流）的现场后果（§73.15 日志）：
 *   三件套先占 2.93 MB（dec_out 还要 1.84 MB 连续大块）→ 起流侧 3 块
 *   （REQBUFS 2 块 + STREAMON 阶段的 DVP backup）在碎洞里切不出 →
 *   起流自动腾地把三件套又放掉 → 重新起流 → 再 acquire —— 绕一圈，
 *   用户看到进页面黑屏 ~2 秒。
 *   新顺序：起流在内存最宽裕时拿到 backup；三件套晚 ~1 个采样周期（150 ms）
 *   attach。acquire 万一失败也只是画面停在自持缓冲的最后一帧 + VFD 渐进
 *   重试（§73.3 幂等），**失败形态从"黑屏"降级为"卡"**。
 *   线程：本函数跑在 LVGL 线程（按钮回调 / tab 激活），满足 acquire 要求。 */
static bool vf_start(void)
{
    vf_stream_ensure();                  /* ★ 先起流（失败时 §73.15 自动腾地重试） */

    esp_err_t err = viewfinder_acquire();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "viewfinder_acquire failed: %s "
                      "(stream stays on; retry by toggling PREVIEW)", esp_err_to_name(err));
        s_vf_err = true;
        return false;
    }
    err = viewfinder_enable();
    s_vf_err = (err != ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "viewfinder_enable failed: %s", esp_err_to_name(err));
        /* ★ 这里**不** release：release 必须与"画布换回自持缓冲"成对做，
         *   而此刻画布状态不确定（可能已指过 s_disp）。留给 vf_release_all()
         *   （PREVIEW 关闭 / 离开页面时必经过），保证释放永远在安全时机发生。 */
        return false;
    }
    return true;
}

static void preview_btn_cb(lv_event_t *e)
{
    (void)e;
    /* ★ 只翻开关 + 发动作，不写文案/颜色 —— refresh() 是唯一真相源。 */
    if (s_preview_on) {
        s_preview_on = false;
        vf_stream_stop();
        /* ★ 2026-09-28：disable → release（把 ~2.93 MB 还给系统）。
         *   画布会换成自持缓冲，但**内容仍是最后一帧**（vf_release_all 里拷过去的）。 */
        vf_release_all();
    } else {
        s_preview_on = true;
        vf_start();
    }
}

/* ---------------------------------------------------------------------------
 *  tab 激活：起 / 停取景流
 * ------------------------------------------------------------------------- */
void tab_video_set_active(bool active)
{
    if (active == s_active) return;
    s_active = active;

    if (active) {
        /* PREVIEW 关着就不起流、不挂观察者（省电；画布停最后一帧）。
         * 拍照/录像不受影响 —— 它们自己接管相机。 */
        if (s_preview_on) {
            vf_start();
        }
    } else {
        /* ① 先关取景器（换回自持画布 → 停解码任务 → 释放 ~2.93 MB PSRAM）。
         *    ★ 2026-09-28 按需化：这一句就是"离开 VIDEO 页把内存让出去"的落点。 */
        vf_release_all();
        /* ② 再停自己拥有的流。 */
        vf_stream_stop();
    }
}

/* ---------------------------------------------------------------------------
 *  建 / 拆
 * ------------------------------------------------------------------------- */
void tab_video_build(lv_obj_t *screen, lv_group_t *group)
{
    (void)group;        /* 触摸方案下无 encoder 组；保留形参以对齐其它 tab 的签名 */

    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    /* ★ 绝对定位布局：把主题 pad 全部归零，让下面所有坐标都以内容区 (0,0) 为原点
     *   （内容区 = 800-2*12 x 480-48-2*12 = 776x408）。不再用 flex ——
     *   "点击放大"要移动画布，flex 会跟 lv_obj_set_pos 抢定位权。 */
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    s_title = lv_label_create(screen);
    lv_label_set_text_static(s_title, LV_SYMBOL_IMAGE "  VIDEO");
    lv_obj_set_pos(s_title, 0, 0);
    lv_obj_set_style_text_color(s_title, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_title, UI_FONT, 0);

    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Idle");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    /* ---- 取景画布 320x180 RGB565 ----
     * ★ 首帧前先给一块自己的黑屏占位缓冲（PSRAM，115,200 B，常驻）——
     *   绝不能让 canvas 指向未初始化内存。首帧到来后数据源换成 viewfinder 的双缓冲
     *   （见 vf_feed）；关闭预览时 vf_release_all() 会把最后一帧拷回它、再让画布指回它。
     *
     * ★★ 2026-09-28 修一个既有泄漏：原代码**无条件**分配本缓冲，而
     *   tab_video_destroy() **故意不释放**它（注释写的是"重复 build/destroy 不重复分配"，
     *   但代码并没有判空复用 —— 注释与实现矛盾）。而 page_main 的 create/destroy 是成对
     *   回调 ⇒ 每次重新进主页面都会再吃 115,200 B PSRAM，永不归还。
     *   修法：非 NULL 则复用（保持"常驻"语义，但真正做到不重复分配）。 */
    if (s_canvas_own == NULL) {
        s_canvas_own = heap_caps_aligned_alloc(64, VF_W * VF_H * 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_canvas_own == NULL) {
            MEM_ERR_DIAG(TAG, "vf placeholder buf alloc FAILED", VF_W * VF_H * 2);
        } else {
            memset(s_canvas_own, 0, VF_W * VF_H * 2);
        }
    }
    if (s_canvas_own == NULL) {
        s_canvas = NULL;
    } else {
        s_canvas = lv_canvas_create(screen);
        lv_canvas_set_buffer(s_canvas, s_canvas_own, VF_W, VF_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_canvas, VF_X, VF_Y);
        lv_obj_set_style_border_color(s_canvas, COL_BORDER, 0);
        lv_obj_set_style_border_width(s_canvas, 1, 0);
        lv_obj_set_style_radius(s_canvas, 8, 0);
        lv_obj_set_clickable(s_canvas, true);
        lv_obj_add_event_cb(s_canvas, canvas_click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ---- 信息两行（画布正下方，无卡片框 —— 简单明了）----
     * hint 在 info 之后建：它要盖在放大后的画布上面（创建顺序 = 绘制顺序）。 */
    s_info = lv_label_create(screen);
    lv_label_set_text_static(s_info, "photo: -\nvideo: -");
    lv_obj_set_pos(s_info, INFO_X, INFO_Y);
    lv_obj_set_width(s_info, INFO_W);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_info, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_info, UI_FONT, 0);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);

    /* 放大态提示：画布之上、底部居中，默认隐藏。 */
    s_hint = lv_label_create(screen);
    lv_label_set_text_static(s_hint, "tap again to exit");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_hint, COL_SUBTEXT, 0);
    lv_obj_set_style_text_font(s_hint, UI_FONT, 0);
    lv_obj_set_hidden(s_hint, true);

    /* ---- 按钮行：Q + PREVIEW 两个 ×140 + 12 = 292（§73.12 后只剩这两个） ---- */
    s_btn_row = lv_obj_create(screen);
    lv_obj_set_pos(s_btn_row, BTNROW_X, BTNROW_Y);
    lv_obj_set_size(s_btn_row, BTNROW_W, BTNROW_H);
    lv_obj_set_style_bg_opa(s_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_row, 0, 0);
    lv_obj_set_style_pad_all(s_btn_row, 0, 0);
    lv_obj_set_scrollable(s_btn_row, false);
    lv_obj_set_flex_flow(s_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_btn_row, 12, 0);

    /* ★★ §73.12：拍照 / 录像两颗**圆形**按钮 —— 移到取景画面右侧竖排。
     *  · 拍照：按下即拍（LV_EVENT_PRESSED）；
     *  · 录像：**按住录、松手停**（PRESSED 起录 / RELEASED+PRESS_LOST 收）；
     *  · 按下颜色翻转：基础色走默认 state，按下色走 LV_STATE_PRESSED ——
     *    这是 LVGL 的状态样式通道，无需 refresh 参与，按下的一瞬自动生效。 */
    s_cap_btn = lv_btn_create(screen);
    lv_obj_set_size(s_cap_btn, SIDE_BTN_SIZE, SIDE_BTN_SIZE);
    lv_obj_set_pos(s_cap_btn, SIDE_X, SIDE_CAP_Y);
    lv_obj_set_style_radius(s_cap_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_cap_btn, COL_CYAN, 0);
    lv_obj_set_style_bg_color(s_cap_btn, COL_RED, LV_STATE_PRESSED);    /* 按下翻转 */
    lv_obj_set_style_shadow_width(s_cap_btn, 0, 0);
    lv_obj_add_event_cb(s_cap_btn, cap_btn_cb, LV_EVENT_PRESSED, NULL);
    s_cap_label = lv_label_create(s_cap_btn);
    lv_label_set_text_static(s_cap_label, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(s_cap_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_cap_label, UI_FONT, 0);
    lv_obj_center(s_cap_label);

    s_rec_btn = lv_btn_create(screen);
    lv_obj_set_size(s_rec_btn, SIDE_BTN_SIZE, SIDE_BTN_SIZE);
    lv_obj_set_pos(s_rec_btn, SIDE_X, SIDE_REC_Y);
    lv_obj_set_style_radius(s_rec_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rec_btn, COL_GREEN, 0);
    lv_obj_set_style_bg_color(s_rec_btn, COL_CYAN, LV_STATE_PRESSED);   /* 按下翻转 */
    lv_obj_set_style_shadow_width(s_rec_btn, 0, 0);
    lv_obj_add_event_cb(s_rec_btn, rec_btn_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_rec_btn, rec_btn_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_rec_btn, rec_btn_released_cb, LV_EVENT_PRESS_LOST, NULL);
    s_rec_label = lv_label_create(s_rec_btn);
    lv_label_set_text_static(s_rec_label, "REC");
    lv_obj_set_style_text_color(s_rec_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_rec_label, UI_FONT, 0);
    lv_obj_center(s_rec_label);

    s_q_btn = lv_btn_create(s_btn_row);
    lv_obj_set_size(s_q_btn, 140, 60);
    lv_obj_set_style_radius(s_q_btn, 30, 0);
    lv_obj_set_style_bg_color(s_q_btn, COL_CYAN, 0);
    lv_obj_add_event_cb(s_q_btn, q_btn_cb, LV_EVENT_CLICKED, NULL);
    s_q_label = lv_label_create(s_q_btn);
    lv_label_set_text_fmt(s_q_label, "Q%d", s_q_table[s_q_idx]);
    lv_obj_set_style_text_color(s_q_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_q_label, UI_FONT, 0);
    lv_obj_center(s_q_label);

    /* ★ PREVIEW 开关（用户要求）。初始文案/颜色按真状态画（refresh 会纠偏，
     *   这里只是别先闪一下错的）。 */
    s_vf_btn = lv_btn_create(s_btn_row);
    lv_obj_set_size(s_vf_btn, 140, 60);
    lv_obj_set_style_radius(s_vf_btn, 30, 0);
    lv_obj_set_style_bg_color(s_vf_btn, s_preview_on ? COL_CYAN : COL_BORDER, 0);
    lv_obj_add_event_cb(s_vf_btn, preview_btn_cb, LV_EVENT_CLICKED, NULL);
    s_vf_btn_label = lv_label_create(s_vf_btn);
    lv_label_set_text_static(s_vf_btn_label, s_preview_on ? "PREVIEW ON" : "PREVIEW OFF");
    lv_obj_set_style_text_color(s_vf_btn_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_vf_btn_label, UI_FONT, 0);
    lv_obj_center(s_vf_btn_label);

    s_timer = lv_timer_create(refresh, 50, NULL);
    /* ★ 初值必须是 false：tabview 起始页是 CALL，本页从未活跃。
     *   （不要在这里调 viewfinder_enable —— 那会让不开 VIDEO 页也占着相机。）
     * ★★ 2026-09-28 按需化之后：取景器的 ~2.93 MB PSRAM **只在本页真要预览时才有**
     *   （preview_btn_cb / tab_video_set_active 里 viewfinder_acquire()），离开页面即
     *   viewfinder_release() 归还。⇒ 现在"没进过 VIDEO 页"确实 = "取景器不占 PSRAM"。
     *   （历史注释"开机无条件 init、永不释放"已随本次改造作废。） */
    s_active = false;
    s_zoomed = false;
    s_vf_err = false;
    s_status_hold = 0;
}

void tab_video_destroy(void)
{
    /* ★ 离开主页面时，若还在录像就**请求停止**（走正常收尾，文件是完整的）。 */
    if (record_video_service_state() != VIDEO_REC_IDLE) {
        record_video_service_stop();
    }
    /* ★ 收尾取景：set_active(false) 里只会停"归 PREVIEW 所有的"流，
     *   正在录的 MEDIA 流不受影响（它已由上面请求停止，走正常收尾）。 */
    tab_video_set_active(false);

    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_canvas = NULL;
    /* s_canvas_own 故意不释放：常驻占位缓冲（115,200 B），由 build 侧**判空复用**——
     * 见 tab_video_build 里 2026-09-28 的注释（修掉了"每次 build 都重复分配"的泄漏）。 */
    s_cap_btn = NULL; s_cap_label = NULL;
    s_rec_btn = NULL; s_rec_label = NULL;
    s_q_btn = NULL;   s_q_label = NULL;
    s_vf_btn = NULL;  s_vf_btn_label = NULL;
    s_status = NULL;  s_info = NULL;  s_hint = NULL;
    s_title = NULL;   s_btn_row = NULL;
    s_active = false; s_zoomed = false; s_status_hold = 0;
}
