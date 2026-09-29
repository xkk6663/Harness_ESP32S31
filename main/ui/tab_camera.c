/* tab_camera.c - Camera preview tab
 *
 * ★★ S8 起本页承担三条链路（改动前先读这一节）
 *
 *   【1】预览 + 检测（原有）—— 240x240 RGB565 帧 → canvas → espdet 检测 → 画框。
 *   【2】识别 + 打卡（S8-2/S8-3）——
 *        · 触发条件全在 `refresh()` 里判（USER 态、检测开着、FACE 档、库可用、
 *          框过质量门槛、距上次 ≥ RECOG_PERIOD_MS）；
 *        · 识别本体由 `face_recog_submit()` 起的**一次性任务**跑（MFN 单次 324 ms，
 *          绝不能进 LVGL 线程 —— 见 main.c 里 taskLVGL 栈那一段）；
 *        · 打卡落盘由 `punch_service` 的一次性任务跑。
 *   【3】录入（S8-1 补完）—— ENROLL 态：姓名输入覆盖层 → 连采 3 张特征 → 入库 →
 *        自动退回 USER。
 *
 *  ★ 三条链路共用**同一个** `s_last_box` / 质量门槛（face_quality）——
 *    "引导说 Ready、闸门说不合格"这种两套真相源的事在本页是被禁止的。
 *
 *  ★★ 本页有一条**极易踩空**的线程纪律：
 *      `refresh()` 跑在 **LVGL 线程**，`on_frame()` 跑在 **cam_task**。
 *      两者都能看到的只有 `s_canvas_buf`（LVGL 线程独占写）与几个 volatile 标志。
 *      `recog_pump()` / `draw_watermark()` / 录入流程**全部**在 LVGL 线程里 ——
 *      它们碰 LVGL 对象、碰字体、起任务，但**从不阻塞等待**任何慢操作。
 */
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "tab_camera.h"
#include "ui_theme.h"
#include "ui_common.h"
#include "ui_binfont.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/face_service.h"
#include "middleware/camera/face_quality.h"     /* ★ S8-1：录入质量门槛（引导文案的唯一来源） */
#include "middleware/camera/face_recog_service.h" /* ★ S8-2：识别引擎 */
#include "middleware/camera/punch_service.h"    /* ★ S8-2/S8-3：打卡落盘 + 底片编码 */
#include "middleware/face_db/face_db.h"         /* ★ S8-1：ENROLL 态的准入判据 + 入库 */
#include "middleware/periph/sdcard_service.h"   /* ★ S8-2：拒绝日志要报 SD 挂载状态 */
#include "middleware/net/net_time_service.h"    /* ★ S8-3：打卡时间（自带"可信"判定） */
#include "middleware/mem_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>   /* ★ S8-2：tip_set() 用 va_list / vsnprintf（少了它会在 vsnprintf 处报未定义） */

#define TAG "TAB_CAM"
#define CAM_W 240
#define CAM_H 240

/* ★ S8-1：运行模式（需求 R8"录入模式 / 用户模式"，两个明确区分的运行态）。
 *  ★ 状态的**唯一持有者**是本文件；文案/颜色一律由 refresh() 写（同 FACE/MODEL 的规矩）。 */
#define CAM_MODE_USER   0
#define CAM_MODE_ENROLL 1

/* ★★ S8-2/S8-3 的节奏参数（都在这一个地方，方便上机后调） */
/** 两次识别作业之间的最小间隔。MFN 324 ms + MSR/MNP ≈ 400 ms 一次 ⇒
 *  1.5 s 一拍意味着"识别只占 27% 的时间"，取景器仍然流畅（方案 §14.2 第 1 条）。 */
#define RECOG_PERIOD_MS     1500
/** 录入采样间隔（比识别快：要连采 3 张，用户得举着不动） */
#define ENROLL_SHOT_GAP_MS   700
/** 录入总超时：过了就放弃并说明原因，绝不让用户对着镜头干等 */
#define ENROLL_TIMEOUT_MS  25000
/** 采集张数（§5.6：3 张两两相似度 ≥ 0.5） */
#define ENROLL_SHOTS           3
/** 一次性提示（打卡结果等）在状态栏停留多久 */
#define TIP_HOLD_MS         5000

/* ===========================================================================
 *  ★★★ 2026-09-28：USER 态打卡从"自动"改成"手动按钮"（用户要求）
 *
 *  旧行为：USER 态每 `RECOG_PERIOD_MS` 自动提交一次 IDENTIFY，只要脸合格就
 *          悄悄拍照 + 落盘 + 记流水。用户反馈"完全不知道要怎么打卡"——
 *          因为它**什么都没等用户做**，界面上也就没有任何"该做什么"的线索。
 *  新行为：右侧一颗圆形按钮，**按下才**拍一张、识别一次：
 *            命中库 → 画水印、写 `/sdcard/attendance/<NN_姓名>/`、提示 "Punched: xx"
 *            没命中 → 只提示，不落盘（见 PUNCH_RECORD_GUEST）
 *          没按下时本页**一次识别都不发起**（省 CPU / 省 PSRAM，也不再偷拍）。
 * =========================================================================== */
/** 按下按钮后等一次识别结果的窗口。超时 = 没人脸 / 引擎还没就绪 ⇒ 复位并说明。
 *  ★ 3 s 的依据：MFN 一次 ≈ 324 ms，加上提交与出框，正常 1 s 内必出结果；
 *    留 3 倍余量给"用户按下时还没正对镜头、调整一下"这种情况。 */
#define PUNCH_ARM_TIMEOUT_MS   3000
/** 圆形按钮直径（px）。96 ≈ 屏高的 2/9，明显大于底部那排 140x60 的功能按钮，
 *  符合"这是主操作"的视觉层级；又不会盖到 240x240 的取景画面。 */
#define PUNCH_BTN_SIZE          96

/** ★ §73.11：画布中心的**垂直偏移**（相对 tabview 内容区中心）。
 *  内容区 800x432（顶部 tab bar 48）⇒ 对齐基准中心 y=216；而画布在纵向
 *  flex 里的真实中心 = pad_top(16) + 标题行高(17) + 行距(12) + 半高(120) = 165。
 *  ⇒ 悬浮元素（打卡按钮 / 时间标签）要对齐**画布**就得向上偏 51 px。
 *  全屏态 toggle_fullscreen() 会临时改回 0（那时画布真的在内容区正中）。 */
#define CAM_VIEW_Y_OFF          (-51)

/** 未命中人脸库（访客）时**是否也落盘**。
 *  ★ 用户 2026-09-28 的明确要求是"识别到**符合人脸库**就显示打卡成功并录照片"
 *    ⇒ 置 0：访客不写盘，只提示 "Not in face db"（**不产生任何文件**）。
 *  ★ 想恢复"访客也留档"（旧行为，流水里有 `matched=false` 的条目）：
 *    把这里改 1 即可 —— punch_service 与记录浏览页两侧都已支持，不需要别的改动。 */
#ifndef PUNCH_RECORD_GUEST
#define PUNCH_RECORD_GUEST     0
#endif

/* ★ 录入覆盖层的三个子状态 */
#define ENROLL_OFF        0
#define ENROLL_NAME       1   /* 覆盖层可见，等用户输姓名 */
#define ENROLL_CAPTURING  2   /* 覆盖层已收，正在连采 */

/* ★ 人名/水印用字体：优先 LittleFS 里那份全字表 bin 字体（ui_binfont）——
 *   它的字表覆盖整个 CJK 基本区，人名不会出方块。回退的 lv_font_cn_20 是
 *   **固定小字表**，只保证"不崩"，不保证每个名字都好看。
 *   ⚠ 两者都**不能**拿去套"图标 + 英文"的标签（无 fallback；且 bin 字体 28 px
 *      会把原本 14 px 的英文撑大）—— 见 ui_binfont.h 的"使用禁区"。 */
LV_FONT_DECLARE(lv_font_cn_20);

static lv_obj_t *s_canvas;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_btn_face;
static lv_obj_t *s_btn_face_label;
static lv_obj_t *s_btn_model;        /* ★ S1 新增：档位切换（页内按钮循环切档） */
static lv_obj_t *s_btn_model_label;
static lv_obj_t *s_btn_mode;         /* ★ S8-1 新增：USER ↔ ENROLL 循环（需求 R8） */
static lv_obj_t *s_btn_mode_label;
/* ★ §73.12：原 HSV 专用的 s_bbox（黄色框）已随功能删除；s_face_bbox 不受影响。 */
static lv_obj_t *s_face_bbox;        /* 检测结果框（FACE 脸 / HAND 手）复用 */
static lv_obj_t *s_gest_label;       /* ★ §60：手势名标签（HAND 档显示） */
/* ★ §62：原 POSE 档的 17 个关节点对象（s_kp[17]）已随该档一并删除。 */
static lv_obj_t *s_recog_label;      /* ★ S8-2：识别/打卡结果标签（画布底部） */
static lv_obj_t *s_status;
static lv_obj_t *s_screen;
static lv_timer_t *s_timer;
static uint16_t *s_canvas_buf;
static volatile const uint8_t *s_last_frame;
static volatile bool s_frame_new;
static bool s_fullscreen;
static bool s_face_on;
static int s_frame_skip;
static bool s_media_taken;      /* ★ S2-0：相机是否被 VIDEO tab 独占（避免每帧都写 LVGL 状态） */

/* ★★★ 2026-09-28 二次事故诊断计数器（只增不减，回答"为什么一直没有检测框"）。
 *
 *  背景：UI 上"没有人脸"永远只显示一句 `No face in view`，但它背后有**四种**
 *  完全不同的原因，一次上机根本分不清是哪一种（于是只能反复试）：
 *
 *    s_frames_in      不涨            ⇒ 相机根本没把帧送到本页（没按 START / 流没起）
 *    s_frames_dropped ≈ in（全在涨）  ⇒ 送了帧但尺寸不是 CAM_W×CAM_H（档位被别的 tab 占着）
 *    s_detect_calls   不涨            ⇒ 帧到了但检测没被调（s_face_on / canvas / bbox）
 *    s_detect_calls 涨、hits == 0     ⇒ espdet 真的没检出脸（档位 / 光照 / 距离 / 模型状态）
 *
 *  ★ 前两个由 cam_task 写、后两个由 LVGL 线程写，另一侧只读 ⇒ `volatile` 足够
 *    （统计量，不需要原子自增；漏掉一两拍不影响判读结论）。
 *  ★ 归零点在 `tab_camera_build()`（"本页"语义），**不是**开机。 */
static volatile uint32_t s_frames_in, s_frames_dropped, s_detect_calls, s_detect_hits;
static bool s_noframe_warned;       /* "相机在跑但一帧都没到"这条只报一次 */
static int  s_noframe_ticks;        /* 连续多少拍 refresh 都没见到合格帧 */

/* ★★ S8-1：运行模式与录入引导
 *  s_last_box / s_box_valid —— 最近一次检测出的**最大**框。
 *  为什么要在 refresh() 里缓存一份：ENROLL 态的质量引导（"Move closer" /
 *  "Center the face"）需要人脸框，而它是在**帧处理块**里算出来的（每 3 帧一次）。
 *  与其把引导也塞进那个块（它跑在 lv_lock 临界区里、且只在有新帧时执行），
 *  不如缓存一个只读快照出来 —— 最坏晚 50 ms（一个 refresh 周期），肉眼无感。 */
static int  s_mode;             /* CAM_MODE_USER / CAM_MODE_ENROLL */
static int  s_mode_shown;       /* 上次已上屏的模式（只在**切换的那一拍**写状态栏） */
static vision_result_t s_last_box;
static bool s_box_valid;

/* ===========================================================================
 *  ★★ S8-2/S8-3：识别 + 打卡的常驻状态
 * =========================================================================== */
static lv_obj_t *s_wm_canvas;      /* 隐藏的离屏 canvas（RGB888），只用于画水印 */
static int64_t   s_next_recog_us;  /* 下一次可提交识别作业的时刻（单调时钟） */
static bool      s_recog_want;     /* 上一次下发给识别引擎的开关期望值（用于对账） */

/* ★★★ 2026-09-28：USER 态**手动打卡**（见文件头 PUNCH_ARM_* 那段注释）。
 *   s_punch_armed       = "按钮已按下，正等这一拍识别结果"，是本页唯一的手动意图标志；
 *                        没有它时 recog_pump() 在 USER 态**什么都不提交**。
 *   s_punch_deadline_us = 本次 armed 的截止时刻（没人脸/引擎没就绪就用它兜底复位，
 *                        否则按钮会永远停在"扫描中"，比自动打卡更难理解）。 */
static bool      s_punch_armed;
static int64_t   s_punch_deadline_us;
static int       s_punch_window_ms;   /* 本次 armed 给的窗口（引擎没就绪时会放宽到 8 s）。
                                       * ★ 只用于日志：把"实际给了多久"打出来，
                                       *   否则超时日志里写死 3000 会与真实窗口不符。 */
static lv_obj_t *s_btn_punch;         /* ★ 右侧圆形打卡按钮（仅 USER 态可见） */
static lv_obj_t *s_btn_punch_label;
static bool      s_dir_dumped;        /* ★ 本次进页面是否已打印过"打卡目录体检" */
/* ★★★ 2026-09-28（解耦）：det 的**借用标记**（所有权模式）。
 *   ENROLL 流程会自动开检测（要框对齐），但那个开关的真相源是 FACE 按钮
 *   ⇒ 只有"我们自动开的"才由我们归还，用户自己按开的绝不动。 */
static bool      s_det_borrowed;

/* ★★★ 2026-09-28 新增：画布**左侧**的两行时间显示（打卡系统的"现在几点"）。
 *   第一行 年.月.日，第二行 时:分:秒 —— 只在已校时（net_time_t.valid）时显示；
 *   断网/未校时**整块隐藏**（用户明确要求"不显示"，而不是显示占位横线）。 */
static lv_obj_t *s_time_label;
static char      s_time_txt[24];      /* "2026.09.28\n22:58:06" = 20 B + NUL */

/** 一次性提示（打卡结果等）。
 * ★ 为什么单开一个而不是直接写 s_status：FACE/MODEL 两块也在写 s_status，
 *   直接写会被它们下一拍盖掉 —— 用户就看不到"打卡成功"了。
 *   这里用"带过期时间的覆盖层"语义：有效期内**优先**显示，过期后交还。 */
static char    s_tip[96];
static int64_t s_tip_until_us;
static bool    s_tip_was_active;

/* ---- 录入流程状态 ---- */
static lv_obj_t *s_enroll_ov;      /* 姓名输入覆盖层（默认隐藏） */
static lv_obj_t *s_enroll_msg;
static lv_obj_t *s_enroll_ta;
static lv_obj_t *s_enroll_kb;
static lv_obj_t *s_btn_enroll_go;
static int       s_enroll_state;   /* ENROLL_OFF / ENROLL_NAME / ENROLL_CAPTURING */
static char      s_enroll_name[FACE_NAME_MAX];
static int       s_enroll_have;    /* 已采到几张 */
static int64_t   s_enroll_next_us; /* 下一次采样时刻 */
static int64_t   s_enroll_deadline_us;
/** 特征暂存（**PSRAM**）：ENROLL_SHOTS 张样本 + 1 张均值位。
 *  ★ 为什么不放栈上：3×512×4 = 6 KB，而 taskLVGL 栈只有 9728 B、实测峰值已 6576 B
 *    —— 直接顶穿。本工程在"大缓冲"上一律走 PSRAM 是硬规矩（约束①）。 */
static float    *s_enroll_feats;

/* ===========================================================================
 *  小工具
 * =========================================================================== */

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/** 写一条一次性提示（有效期内覆盖状态栏） */
static void tip_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_tip, sizeof(s_tip), fmt, ap);
    va_end(ap);
    s_tip_until_us = now_us() + (int64_t)TIP_HOLD_MS * 1000;
}

static bool tip_active(void)
{
    return s_tip[0] != '\0' && now_us() < s_tip_until_us;
}

/* ===========================================================================
 *  ★★ S8-2：限频日志 —— 为什么不能直接 ESP_LOGI
 *
 *  USER 态**每 1.5 s** 就会走一遍 user_on_result()，而"脸不合格""冷却期内"
 *  是**常态**而不是异常：无条件打日志会在 5 分钟里刷出 200 行同样的字，
 *  把真正有用的行（punch OK / 失败原因）冲到屏幕外面去。
 *
 *  ⇒ 按"原因槽"各自限频 `SKIP_LOG_PERIOD_US`。既留下验收证据，又不制造噪声：
 *     · 验收"连站 5 分钟只产生 1 条记录"时，日志里应该看到**一次** cooldown，
 *       而不是 200 次；
 *     · 真正异常的路径（commit 失败 / 画布没建）用 ESP_LOGE **无条件**打。
 * =========================================================================== */
#define SKIP_LOG_SLOTS      8
#define SKIP_LOG_PERIOD_US  (30 * 1000 * 1000)      /* 同槽 30 s 只报一次 */

#define SKIP_QUALITY    0       /* USER：脸不合格（框太小/不居中/置信度低） */
#define SKIP_COOLDOWN   1       /* USER：60 s 冷却期内 */
#define SKIP_SD         2       /* USER：SD 未挂载 */
#define SKIP_DB         3       /* USER：人脸库不可用 */
#define SKIP_MEM        4       /* USER：帧缓冲/输出缓冲分配失败 */
#define SKIP_BUSY       5       /* USER：上一次落盘还没写完 */
#define SKIP_ENROLL_Q   6       /* ENROLL：脸不合格（引导语已在覆盖层上） */
/* ★ 2026-09-28 加的：提交识别作业被拒 = 引擎没就绪或正忙。
 *   这是"卡在 capturing 0/3 / 一直不打卡"的**第一个**诊断点，以前是静默的。 */
#define SKIP_NOT_READY  7

static int64_t s_skip_us[SKIP_LOG_SLOTS];

/** 限频日志（同一槽 30 s 最多一条）。★ 只在 LVGL 线程调用 —— 无锁。 */
static void skip_log(int slot, const char *fmt, ...)
{
    if (slot < 0 || slot >= SKIP_LOG_SLOTS) return;

    int64_t now = esp_timer_get_time();
    if (now - s_skip_us[slot] < SKIP_LOG_PERIOD_US) return;
    s_skip_us[slot] = now;

    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "punch skipped (%ds window): %s", SKIP_LOG_PERIOD_US / 1000000, buf);
}

/* ===========================================================================
 *  ★★★ 2026-09-28 二次事故诊断："人明明站在镜头前，却一直 No face in view"
 *
 *  UI 上"没有框"永远只有一句提示，而它背后有四种原因（见计数器声明处的表）。
 *  本函数把它们一次性打出来，让**一次上机**就能定位，不必再来回试。
 *
 *  ★ 单独一条日志、**不复用 skip_log**：后者的前缀是 "punch skipped"，
 *    对"检测诊断"是错误语义（会把排查方向带偏），而且它的 buf 只有 96 B 装不下。
 *  ★ 只在 `!s_box_valid` 时被调用（就是"要诊断的那一刻"），内部再限频 5 s。
 * =========================================================================== */
#define NOBOX_LOG_PERIOD_US (5 * 1000 * 1000)      /* 同一现象 5 s 一条：够密，又不刷屏 */
static int64_t s_nobox_log_us;

static void nobox_diag(void)
{
    int64_t now = now_us();
    if (now - s_nobox_log_us < NOBOX_LOG_PERIOD_US) return;
    s_nobox_log_us = now;

    ESP_LOGW(TAG, "no-box diag | frames in=%u dropped=%u | detect calls=%u hits=%u"
                  " | face_on=%d svc_en=%d state=%d model=%s"
                  " | canvas=%d buf=%d bbox=%d media=%d cam_run=%d",
             (unsigned)s_frames_in, (unsigned)s_frames_dropped,
             (unsigned)s_detect_calls, (unsigned)s_detect_hits,
             (int)s_face_on, (int)face_service_enabled(), (int)face_service_state(),
             face_service_model_label(face_service_current_model()),
             (int)(s_canvas != NULL), (int)(s_canvas_buf != NULL), (int)(s_face_bbox != NULL),
             (int)s_media_taken, (int)camera_service_is_running());
}

/** 当前可用的 CJK 字体（bin 字体优先，回退内置 cn_20）
 *  ★ 返回 **const**：`lv_font_cn_20` 与 LVGL 的字体对象都是 const 的
 *    （`LV_FONT_DECLARE` 展开成 `extern const lv_font_t`），
 *    写 `lv_font_t *` 会在 `return &lv_font_cn_20;` 处报
 *    `discards 'const' qualifier`（-Werror）。 */
static const lv_font_t *cjk_font(void)
{
    const lv_font_t *f = ui_binfont_get();
    return f ? f : &lv_font_cn_20;
}

/** 余弦相似度（两个特征都已 L2 归一化 ⇒ 点积即可） */
static float cos_sim(const float *a, const float *b, int dim)
{
    float acc = 0.0f;
    for (int i = 0; i < dim; i++) acc += a[i] * b[i];
    return acc;
}

/* ★★ S2-0：回调签名加了 size（JPEG 是变长帧），并换成**运行时** w/h。
 *   ★ 为什么必须挡一道尺寸：VIDEO tab 会把流切成 1280x720 JPEG，
 *     而本页 canvas 缓冲只有 240x240x2 字节。若照单全收就是**堆溢出**。
 *   ★ 也**不要**用 size 去推算 w*h：RGB565 定长可以，JPEG 不行（变长）。
 *     这里直接用 w/h 判断，size 只做"至少够一帧"的下限校验。 */
static void on_frame(const uint8_t *data, uint32_t size, uint32_t w, uint32_t h, void *user)
{
    (void)user;
    s_frames_in++;               /* ★ 诊断：进本回调就算，不计后续是否被挡 */
    if (w != CAM_W || h != CAM_H || size < (uint32_t)(CAM_W * CAM_H * 2)) {
        s_frames_dropped++;      /* ★ 诊断：尺寸不符 = "档位被别人占着"的直接证据 */
        return;                  /* 不是本页要的档位：直接丢，不要记指针 */
    }
    s_last_frame = data;
    s_frame_new = true;
}

/* ===========================================================================
 *  ★★ S8-1：当前档位是不是"人脸检测档"
 *
 *  为什么需要这一条：HAND 档输出的检测框是**手**。ENROLL 的质量门槛（§5.6）
 *  是给脸定的，拿一只手去过它就是荒谬引导（"Move closer"），而 S8-2 接上采集后
 *  更会把一只手录成人脸 —— 数据污染比"引导不准"严重得多。
 *
 *  ★ 用**档名**判断而不是档位序号，有两个理由：
 *    ① face_service.h 明确要求"UI 不要写死档数/档名"（§62 阉割 POSE 时，
 *       档位表从 3 档变 2 档，UI 一行没改就自动跟上 —— 序号是会漂移的）；
 *    ② 本工程**没有**"档位族"的对外接口（FAM_FACE 是 face_service.cpp 内部枚举），
 *       加一个 `face_service_is_face_model()` 会牵动已验收的模块。档名在这里
 *       反而更稳定：它是 UI 本来就在显示的字符串（"Model: FACE"）。
 *  ⚠ 代价是"档名 FACE/HAND"成了跨模块的隐含契约。若将来改名，这里会失效 ——
 *    所以档名字面量**只在本函数里出现一次**，改的时候一处即可。
 *
 *  ★ 模型从未成功加载过时，face_service_current_model() 返回 0（文档保证），
 *    而 0 号档就是 FACE ⇒ 这里返回 true，不会把"模型还没建"误判成"档位不对"
 *    （真正该表达的是"正在加载"，那由下面的 FACE_LOADING 分支负责）。
 * =========================================================================== */
static bool face_model_active(void)
{
    const char *lab = face_service_model_label(face_service_current_model());
    return (lab != NULL && strcmp(lab, "FACE") == 0);
}

/** 用最近一次的检测框做质量判定（s_last_box + 尺寸） */
static void last_box_quality(face_quality_t *q)
{
    if (!s_box_valid) {
        q->has_face = false;
        q->ok       = false;
        q->score_pct = 0;
        q->hint     = "No face in view";
        /* ★★★ 就是这一刻需要诊断：UI 只会有这一句话，而原因有四种。
         *   nobox_diag() 内部限频 5 s，所以这里每 50 ms 被调也没关系。 */
        nobox_diag();
        return;
    }
    face_quality_check(&s_last_box, CAM_W, CAM_H, q);
}

/* ===========================================================================
 *  ★★★ 2026-09-28：手动打卡（USER 态）—— 按钮 + 武装/解除
 *
 *  职责边界（别把这些搬去别处）：
 *   · 本块只负责"**用户想不想打卡**"（s_punch_armed）与"**按不下去时为什么**"；
 *   · "这一拍能不能识别"由 recog_pump() ④ 判（它才看得到检测框是否合格）；
 *   · "落在哪个目录、叫什么名字"由 punch_service 决定（它才知道库里的序号）。
 * =========================================================================== */

/** 结束一次"已按下"的状态。
 *  ★ 每一个终点都必须调它：成功 / 未命中 / 超时 / 前置失败。
 *    漏掉任何一条，按钮就永久停在 SCAN 上 —— 那是"按了没反应"的另一种写法。 */
static void punch_disarm(void)
{
    s_punch_armed       = false;
    s_punch_deadline_us = 0;
}

/** 圆形打卡按钮回调（跑在 LVGL 线程）。
 *  ★ 它**不做识别**：只把意图置上、并做完所有"按不下去"的前置检查 ——
 *    提交必须留给 recog_pump() ④，因为只有那里能同时看到"本拍检测框是否合格"。
 *  ★ 前置检查**逐条给话**：下面每一条都曾是"按了没反应"的真实原因，
 *    静默 return 等于把排查成本转嫁给用户。 */
static void punch_btn_cb(lv_event_t *e)
{
    (void)e;

    if (s_mode != CAM_MODE_USER) {
        return;                             /* ENROLL 态按钮是隐藏的，这里只是兜底 */
    }

    /* ★★★ 2026-09-28：**第一次**按下时打印一次"打卡目录体检"。
     *   为什么挂在这里而不是建页时：建页可能早于 SD 挂载完成（会误报"SD 未挂载"），
     *   而这里恰好是"用户就要打卡"的那一刻 —— 卡上现在有什么，是回答
     *   "上次到底写成没有"的唯一可靠来源（见 punch_service_dump_dir()）。
     *   每次进页面只打一次，不占刷新路径。 */
    if (!s_dir_dumped) {
        s_dir_dumped = true;
        punch_service_dump_dir();
    }

    if (s_punch_armed) {
        tip_set("Already scanning ...");    /* 防连点：窗口内重复按不排队 */
        return;
    }

    /* ---- 前置条件：从外到内，与 enroll_status() 同一套因果顺序 ---- */
    if (!camera_service_is_running()) {
        ESP_LOGW(TAG, "punch refused: camera not running");
        tip_set("Tap START to open the camera first");
        return;
    }
    if (s_media_taken) {
        ESP_LOGW(TAG, "punch refused: camera owned by VIDEO (recording)");
        tip_set("Camera busy (recording) - try later");
        return;
    }
    if (!sdcard_service_is_mounted()) {
        ESP_LOGW(TAG, "punch refused: SD not mounted");
        tip_set("No SD card - cannot save");
        return;
    }
    if (!face_db_is_ready()) {
        ESP_LOGW(TAG, "punch refused: face db not ready");
        tip_set("Face db not ready (SD?)");
        return;
    }
    if (face_db_count() <= 0) {
        ESP_LOGW(TAG, "punch refused: face db empty");
        tip_set("Face db is empty - enroll someone first");
        return;
    }
    /* ★★★ 2026-09-28（解耦）：**删掉旧版的"档位不是 FACE"与"检测关"两条拦截**。
     *   打卡识别走的是识别引擎自带的 MSR+MNP（自己检脸 + 5 点关键点），
     *   与 espdet 的档位/开关**无关** —— det 按钮开着还是关着、在 FACE 还是 HAND 档，
     *   都不影响打卡。这样 det 按钮才是纯粹的"face/hand 检测"开关。 */
    if (s_frames_in == 0) {
        /* ★ 与 refresh() 里那条硬故障日志同源：相机在跑，但本页一帧都没收到。
         *   ★ §73.13 分流：owner=MEDIA（VIDEO 正在拍照/录像）时帧本来就不给本页，
         *     属正常暂态，提示"稍候"而不是误导性的"tap STOP then START"。 */
        if (camera_service_is_running() &&
            camera_service_owner() == CAM_OWNER_MEDIA) {
            ESP_LOGW(TAG, "punch refused: camera is owned by VIDEO (recording/capture) - wait for it to finish");
            tip_set("Camera busy (VIDEO) - try again");
        } else {
            ESP_LOGE(TAG, "punch refused: camera running but ZERO frames reached this page "
                          "-> tap STOP then START");
            tip_set("No frames - tap STOP then START");
        }
        return;
    }

    /* ---- 武装。**
     *  引擎还没就绪时给更长的窗口：第一次在本页按 FACE 之后，识别引擎要几百 ms
     *  到数秒才可用（MFN 加载），那期间按下的按钮**不应该白按**。 */
    int64_t win_ms = PUNCH_ARM_TIMEOUT_MS;
    if (!face_recog_ready()) {
        win_ms = 8000;
        tip_set("Starting engine - hold on ...");
        ESP_LOGI(TAG, "punch armed, engine not ready yet (state=%d) - window %d ms",
                 (int)face_recog_state(), (int)win_ms);
    } else {
        tip_set("Hold still - scanning ...");
        ESP_LOGI(TAG, "punch armed by button");
    }
    s_punch_armed       = true;
    s_punch_window_ms   = (int)win_ms;
    s_punch_deadline_us = now_us() + win_ms * 1000;
}

/* ===========================================================================
 *  ★★ S8-3：水印绘制（**必须在 LVGL 线程**）
 *
 *  为什么是"离屏 canvas + 立即模式绘制"而不是"建两个 label 再截图"：
 *   · 方案 §3-约束②已经证明"在 720p 硬编码流上画字"做不到；
 *   · 而在 RGB888 缓冲上画字，LVGL 9 提供的正是 `lv_canvas_init_layer()` /
 *     `lv_draw_rect()` / `lv_draw_label()` / `lv_canvas_finish_layer()` 这一套
 *     立即模式 API —— 不需要把 canvas 挂到屏幕上渲染，也不需要读回屏幕像素。
 *
 *  ★ 画布对象本身是**隐藏的**（挂在 CAM 页根对象下）：它只是个"绘制目标"的载体，
 *    真正的内容落在我们传给它的那份 RGB888 缓冲里。
 *  ★ 行高用字体自带的 `font->line_height` **动态算**，不写死：
 *    bin 字体的行高（28）与内置 cn_20（~20）不同，写死必然在某一档下压字。
 *    （LVGL 9 没有 `lv_font_get_line_height()` 这个函数，line_height 就是公开成员。）
 * =========================================================================== */
static void watermark_draw(uint8_t *rgb, int w, int h, const char *name, const char *timestr)
{
    /* ★ 拆成两条**有声**的早退：原来那句合并的 `if (...) return;` 会把
     *   "画布没建 ⇒ 这张打卡照片上没有水印"这种**必须知道**的事静默吞掉 ——
     *   而水印恰恰是 R6 的验收物。 */
    if (!rgb || w <= 0 || h <= 0) {
        ESP_LOGE(TAG, "watermark: bad frame (buf=%p %dx%d)", (void *)rgb, w, h);
        return;
    }
    if (!s_wm_canvas) {
        ESP_LOGE(TAG, "watermark: canvas missing - photo will be saved WITHOUT watermark");
        return;
    }

    lv_canvas_set_buffer(s_wm_canvas, rgb, w, h, LV_COLOR_FORMAT_RGB888);

    const lv_font_t *nf = cjk_font();
    int lh_name = nf ? nf->line_height : 20;
    int lh_time = UI_FONT->line_height;

    int band_h = lh_name + lh_time + 8;
    if (band_h > h / 2) band_h = h / 2;         /* 极端字体下也别把整张图糊掉 */
    int band_y = h - band_h;

    lv_layer_t layer;
    lv_canvas_init_layer(s_wm_canvas, &layer);

    /* 半透明黑带：**必须有** —— 否则浅色背景上的白字看不见 */
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color = lv_color_black();
    rdsc.bg_opa   = LV_OPA_50;
    rdsc.radius   = 0;
    lv_area_t band = { 0, band_y, w - 1, h - 1 };
    lv_draw_rect(&layer, &rdsc, &band);

    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color = lv_color_white();
    ldsc.opa   = LV_OPA_COVER;

    /* 第一行：姓名（CJK 字体） */
    if (name && name[0]) {
        ldsc.font = nf;
        ldsc.text = name;
        lv_area_t a = { 6, band_y + 4, w - 7, band_y + 4 + lh_name - 1 };
        lv_draw_label(&layer, &ldsc, &a);
    }
    /* 第二行：时间（ASCII 字体；未校时时是 "---- -- -- --:--:--" 这种可辨认占位） */
    if (timestr && timestr[0]) {
        ldsc.font = UI_FONT;
        ldsc.text = timestr;
        lv_area_t a = { 6, band_y + 4 + lh_name, w - 7, band_y + 4 + lh_name + lh_time - 1 };
        lv_draw_label(&layer, &ldsc, &a);
    }

    lv_canvas_finish_layer(s_wm_canvas, &layer);
}

/* ===========================================================================
 *  ★★ S8-2：识别/打卡链路的驱动（**LVGL 线程**，由 refresh() 每 50 ms 调一次）
 * =========================================================================== */

/** 识别结果 → 打卡（USER 态） */
static void user_on_result(const recog_result_t *r)
{
    /* ★★★ 2026-09-28：本函数现在**只由"用户按下打卡按钮"触发** ——
     *   recog_pump() ④ 只在 s_punch_armed 为真时才提交 IDENTIFY。
     *   于是它不再是"每 1.5 s 自动跑一次的后台流水线"，而是**一次用户操作的结论**：
     *     · 所有**终局**分支都必须调 punch_disarm()，否则按钮永远停在 SCAN；
     *     · "没检出脸 / 脸不合格"**不**disarm —— 让用户在窗口内调整姿势重试，
     *       窗口到期由 recog_pump() ④ 统一收尾（那是唯一会超时的地方）。 */
    if (!r->valid) return;              /* 没检出脸：保留上一条提示，窗口内重试 */

    /* ★ 质量门槛与 ENROLL **共用同一份判据**（face_quality）：
     *   不合格就不打卡，并把同一句话显示出来 —— 用户不用学两套规则。 */
    vision_result_t vr;
    memset(&vr, 0, sizeof(vr));
    vr.x = r->x; vr.y = r->y; vr.w = r->w; vr.h = r->h;
    vr.score = r->det_score;
    face_quality_t q;
    face_quality_check(&vr, CAM_W, CAM_H, &q);
    if (!q.ok) {
        /* ★ 这是**最高频**的一条（没人在镜头前也会走到这里）：必须限频，
         *   否则 5 分钟 200 行。它同时也是"为什么没打卡"的第一个答案。
         * ★ 这里**保持 armed**：把 hint 显示出来，用户按提示调整后窗口内会自动重试。 */
        skip_log(SKIP_QUALITY, "quality - %s (det=%d%% box=%d,%d %dx%d)",
                 q.hint, q.score_pct, r->x, r->y, r->w, r->h);
        tip_set("%s", q.hint);
        return;
    }

    const char *who = r->name[0] ? r->name : FACE_DB_GUEST_NAME;

#if !PUNCH_RECORD_GUEST
    /* ★★★ 2026-09-28 用户要求："识别到**符合人脸库**就显示打卡成功并将照片录入到
     *   sd 卡对应名字的文件夹内" ⇒ 访客（库里没有这个人）**不落盘、不记流水**。
     * ★ 判据用 `r->person_id == FACE_DB_GUEST_ID`，**不要**用 `r->name[0]`：
     *   名字为空串是另一回事（入库时 sanitize 已保证非空），用它判会把
     *   "库里有条目但名字异常"错判成访客。 */
    if (r->person_id == FACE_DB_GUEST_ID) {
        skip_log(SKIP_DB, "no match in face db (guest, det=%.2f) - nothing written",
                 (double)r->det_score);
        tip_set("Not in face db - not recorded");
        ESP_LOGI(TAG, "punch: NO MATCH (guest, det=%.2f) - no file written",
                 (double)r->det_score);
        punch_disarm();
        return;
    }
#endif

    /* 帧来源 = **引擎里那份刚被识别过的 RGB888**（所见即所存，方案 §5.4）。 */
    int sw = 0, sh = 0;
    const uint8_t *src = face_recog_frame(&sw, &sh);
    if (src == NULL) {
        skip_log(SKIP_MEM, "engine frame unavailable (frame ptr NULL)");
        return;
    }

    uint8_t *dst = NULL;
    punch_prep_t prep = punch_service_begin(r, src, (uint16_t)sw, (uint16_t)sh, &dst);
    switch (prep) {
    case PUNCH_PREP_OK:
        break;
    case PUNCH_PREP_COOLDOWN:
        /* ★ 这不是错误：60 s 内同一人只记一次（§9-R8）。照实说"刚才已打卡"，
         *   而不是让用户以为坏了。
         *   ★★ 这条日志是验收项"连按 5 分钟只产生 1 条记录"的**唯一直接证据**：
         *      不留它，用户只能靠数 CSV 行数反推"到底是被挡住了还是识别没跑"。
         * ★ 终局 ⇒ 收掉 armed：这次按下已经有结论了，再按也不会有新结果。 */
        skip_log(SKIP_COOLDOWN, "cooldown - %s (id=%d, %ds window)", who,
                 r->person_id, PUNCH_COOLDOWN_S);
        tip_set("Already punched: %s", who);
        punch_disarm();
        return;
    case PUNCH_PREP_BUSY:
        /* 上一次还在写：**保持 armed** 等下一拍（写盘只要百 ms 级，属于瞬时状态），
         * 但留一条限频日志（长时间没闲下来就是异常）。 */
        skip_log(SKIP_BUSY, "previous write still in flight - %s", who);
        return;
    case PUNCH_PREP_NO_SD:
        skip_log(SKIP_SD, "no SD card - %s not saved", who);
        tip_set("No SD card - punch not saved");
        punch_disarm();
        return;
    case PUNCH_PREP_NO_DB:
        skip_log(SKIP_DB, "face db not ready - %s not saved", who);
        tip_set("Face db not ready - punch not saved");
        punch_disarm();
        return;
    case PUNCH_PREP_NO_MEM:
        skip_log(SKIP_MEM, "frame/jpeg buffer alloc failed - %s not saved", who);
        tip_set("Low memory - punch not saved");
        punch_disarm();
        return;
    default:
        skip_log(SKIP_MEM, "unexpected prep=%d - %s not saved", (int)prep, who);
        tip_set("Punch skipped");
        punch_disarm();
        return;
    }

    /* ---- 水印（本函数就在 LVGL 线程，是唯一合法的绘制位置）---- */
    net_time_t nt;
    char ts[24];
    net_time_service_get(&nt);
    net_time_service_format(&nt, ts, sizeof(ts));   /* 未校时时是长横线占位，不是 1970 */
    watermark_draw(dst, sw, sh, who, ts);

    if (punch_service_commit() != ESP_OK) {
        /* ★ 异常路径：无条件打（已经 begin 了却 commit 不上 = 占位没建立，
         *   冷却**已经扣掉**，这一拍不会重试 —— 必须留下痕迹）。 */
        ESP_LOGE(TAG, "commit failed after begin (prep=OK) - aborting, cooldown stays charged");
        punch_service_abort();
        tip_set("Punch start failed");
        return;
    }
    /* ★★ 措辞用 submitted 而不是 accepted：本行只表示"作业已投递给一次性任务"，
     *   **不是**"打卡成功"。真正的结论由 recog_pump() ② 在任务跑完后显示为
     *   "Punched: xx" / "Punch failed: xx"。2026-09-28 用户把这一行当成了成功凭证，
     *   而落盘其实失败了（见 punch_service.c §70）—— 日志用词必须和语义一致，
     *   否则它自己就是下一个排查陷阱。 */
    ESP_LOGI(TAG, "punch submitted (async): %s | frame %dx%d | t=%s | det=%.2f",
             who, sw, sh, ts, (double)r->det_score);
    tip_set("Punching %s ...", who);
    /* ★ 作业已投递 ⇒ 本次"按下"的动作结束，立刻收掉 armed：
     *   按钮回到 PUNCH 待命态，而结论（Punched / Punch failed）由 recog_pump() ②
     *   在任务跑完后显示 —— 两者是**两个不同的时刻**，别在这里等。 */
    punch_disarm();
}

/* ===========================================================================
 *  ★★ S8-1 补完：录入流程
 * =========================================================================== */

/** 置一条覆盖层提示（录入期间用户看的是这一层，不是状态栏） */
static void enroll_msg_set(const char *txt)
{
    if (s_enroll_msg) ui_common_set_label_text_if_changed(s_enroll_msg, txt);
}

/* ===========================================================================
 *  ★★ S8-1：录入特征暂存的生命周期（★★ 2026-09-28 修正回归）
 *
 *  历史：这块 8 KB（(3+1)×512×4 B）过去只在建页时分配、永不释放。
 *  §72 想把它做成"录入会话的一部分"（结束时释放）—— 但只做了释放、
 *  **忘了把分配挪到"进入录入模式"**：build() 只在页面创建时跑一次，
 *  于是第二次进录入模式时 s_enroll_feats == NULL ⇒ enroll_on_result() 在
 *  `if (!s_enroll_feats) return;` 处**静默丢弃每一张特征** ⇒ 表现恰好是
 *  "capturing 0/3 卡住不动"，而串口里 RECOG 侧 extract 明明成功了。
 *  教训：**把一个缓冲纳入某个会话的生命周期，分配和释放必须成对搬家**，
 *  只搬一半就是把"建页期"的资源错误地挂到了"会话期"。
 * =========================================================================== */

/** 录入特征暂存的惰性分配（幂等）。失败返回 false（录入会给出明确提示）。
 *  ★ 在**进入录入模式**时调用（mode_btn_cb），不再依赖"页面恰好新建过"。 */
static bool enroll_feats_alloc(void)
{
    if (s_enroll_feats) return true;
    s_enroll_feats = (float *)heap_caps_malloc(
        (size_t)(ENROLL_SHOTS + 1) * RECOG_FEAT_DIM * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_enroll_feats) {
        MEM_ERR_DIAG(TAG, "enroll feat buf alloc FAILED (SPIRAM)",
                     (ENROLL_SHOTS + 1) * RECOG_FEAT_DIM * (int)sizeof(float));
        return false;
    }
    memset(s_enroll_feats, 0,
           (size_t)(ENROLL_SHOTS + 1) * RECOG_FEAT_DIM * sizeof(float));
    MEM_STAGE(TAG, "enroll feat buf allocated");
    /* ★ §73.9：带指针的成对日志 —— 分配/释放若不成对，下一份串口日志里
     *   两个地址一对就能当场抓住（2026-09-28 深夜 viewfinder 差 8,192 B
     *   拿不到连续块，头号嫌疑人就是这 8 KB；有指针就不再是悬案）。 */
    ESP_LOGI(TAG, "enroll feat buf = %p (8192 B)", (void *)s_enroll_feats);
    return true;
}

/** 录入特征暂存的释放（录入会话结束、页面销毁时调）。 */
static void enroll_feats_free(void)
{
    if (s_enroll_feats) {
        /* ★ §73.9：与 alloc 的 %p 日志成对，释放没发生一眼可见 */
        ESP_LOGI(TAG, "enroll feat buf FREE %p", (void *)s_enroll_feats);
        heap_caps_free(s_enroll_feats);
        s_enroll_feats = NULL;
    }
}

static void enroll_overlay_show(bool show)
{
    if (!s_enroll_ov) return;
    lv_obj_set_hidden(s_enroll_ov, !show);
    if (show) {
        lv_obj_move_foreground(s_enroll_ov);
        if (s_enroll_kb && s_enroll_ta) lv_keyboard_set_textarea(s_enroll_kb, s_enroll_ta);
    } else if (s_enroll_kb) {
        lv_keyboard_set_textarea(s_enroll_kb, NULL);
    }
}

/** 回到 USER 态（录入的**唯一**出口，任何失败/完成都走这里） */
static void enroll_finish_to_user(const char *tip)
{
    s_mode            = CAM_MODE_USER;
    s_enroll_state    = ENROLL_OFF;
    s_enroll_have     = 0;
    s_enroll_name[0]  = '\0';
    enroll_overlay_show(false);
    if (s_enroll_ta) lv_textarea_set_text(s_enroll_ta, "");
    s_box_valid = false;
    /* ★★ 2026-09-28：**显式藏掉最后一帧的检测框**（用户报"录入完成后剩绿框"）。
     *   旧版只置 s_box_valid=false，指望下一拍 refresh 的帧处理块走 else 分支藏框；
     *   但那条路径有两种"跑不到"的可能：
     *     ① 检测被归还后 s_face_on 变 false ⇒ 整个画框块被跳过；
     *     ② 相机已停 ⇒ 没有帧 ⇒ 块也不执行。
     *   两种情况下最后一次的绿框都**永远挂在画布上**。框对象归 LVGL 线程独占，
     *   这里就在 LVGL 线程里直接藏 —— 不依赖任何"下一拍会不会跑"的假设。 */
    if (s_face_bbox)  lv_obj_set_hidden(s_face_bbox, true);
    if (s_gest_label) lv_obj_set_hidden(s_gest_label, true);
    if (tip) tip_set("%s", tip);

    /* =======================================================================
     *  ★★★ 2026-09-28（解耦）：录入会话的**完整结束**。
     *
     *  用户要求："录入完之后要彻底释放和结束人脸检测"。两条都做：
     *
     *  ① **识别引擎**：s_enroll_state 已归零 ⇒ recog_need() 变 false ⇒
     *     recog_pump() ③ 下一拍自动 face_recog_stop()，把 MSR+MNP+MFN+帧缓冲
     *     （峰值 ≈ 2.11 MB）全部还回去。这里不用手动调 stop —— 对账会收敛。
     *
     *  ② **人脸检测**：只归还**我们借来的**。`s_det_borrowed` 记录的是
     *     "进入 ENROLL 时检测是本流程自动开的" —— 那么退出时就要停检测
     *     **并彻底卸载模型**（face_service_unload 会连 scratch 一起释放，
     *     FACE 档 ≈ 548 KB 连续块 + 172,800 B scratch）。
     *     用户自己按 FACE 开的绝不动（否则"明明开着的开关莫名其妙变灰"
     *     是最招人烦的一类行为，见 mode_btn_cb 的旧注释）。
     *
     *  ★ 顺序：先 set_enabled(false)（立刻停止每帧检测），再 unload()
     *    （投递给 cam_task 下一拍执行 —— 真正的 free 必须在 detect() 不再
     *     使用 scratch 的上下文里做，见 face_service_poll() 的注释）。
     * ======================================================================= */
    if (s_det_borrowed) {
        s_det_borrowed = false;
        face_service_set_enabled(false);
        face_service_unload();
        ESP_LOGI(TAG, "ENROLL end: det returned + model unload requested");
    }
    /* ★ 特征暂存也一并归还（8,192 B，PSRAM）："彻底释放"就要连小头一起收。
     *   ★ 分配的对应端在 mode_btn_cb（进入录入模式时）—— 两端必须成对，
     *     只有释放没有分配就是 §73 修掉的那次 0/3 回归。 */
    enroll_feats_free();
}

/** 录入成功：算均值 → 入库 → 写底片 */
static void enroll_commit(void)
{
    const int dim = RECOG_FEAT_DIM;

    /* ---- ① 一致性检查（§5.6：3 张两两相似度 ≥ 0.5）----
     * ★ 这一条不是"锦上添花"：它挡的是"拍到的是三张不同的脸"或"拍到半张脸"，
     *   那会把一锅杂特征入库，之后这个人**永远认不出来**。 */
    for (int i = 0; i < ENROLL_SHOTS && s_enroll_feats; i++) {
        for (int j = i + 1; j < ENROLL_SHOTS; j++) {
            float s = cos_sim(&s_enroll_feats[i * dim], &s_enroll_feats[j * dim], dim);
            if (s < 0.5f) {
                ESP_LOGW(TAG, "enroll: sample %d/%d similarity %.3f < 0.5, retry", i, j, (double)s);
                s_enroll_have = 0;
                s_enroll_next_us = now_us() + 800 * 1000;
                enroll_msg_set("Photos differ too much - hold still and try again");
                return;
            }
        }
    }

    /* ---- ② 均值特征（放到第 ENROLL_SHOTS 号槽位）----
     * ★ 用均值而不是"随便挑一张"：均值对单帧噪声/光照最稳，是标准做法。
     *   （方案 §5.5 原文是"取与均值最接近的一张"，那需要**再存一份**特征来算
     *     距离；10 人规模下均值与"最接近均值的那张"效果无实质差别 —— 这里选均值，
     *     省 2 KB PSRAM 与一轮循环，并且在注释里把偏离写清楚。） */
    float *mean = &s_enroll_feats[ENROLL_SHOTS * dim];
    memset(mean, 0, (size_t)dim * sizeof(float));
    for (int k = 0; k < ENROLL_SHOTS; k++) {
        const float *f = &s_enroll_feats[k * dim];
        for (int i = 0; i < dim; i++) mean[i] += f[i];
    }
    for (int i = 0; i < dim; i++) mean[i] /= (float)ENROLL_SHOTS;

    uint16_t id = 0;
    esp_err_t e = face_db_enroll(s_enroll_name, mean, dim, ENROLL_SHOTS, &id);
    if (e != ESP_OK) {
        char m[96];
        switch (e) {
        case ESP_ERR_NO_MEM:        snprintf(m, sizeof(m), "Face db full (%d/%d)", FACE_DB_MAX, FACE_DB_MAX); break;
        case ESP_ERR_INVALID_STATE: snprintf(m, sizeof(m), "Name already used"); break;
        case ESP_ERR_INVALID_ARG:   snprintf(m, sizeof(m), "Name not acceptable"); break;
        default:                    snprintf(m, sizeof(m), "Enroll failed (%s)", esp_err_to_name(e)); break;
        }
        ESP_LOGE(TAG, "enroll failed: %s", esp_err_to_name(e));
        enroll_msg_set(m);
        s_enroll_have = 0;
        s_enroll_next_us = now_us() + 800 * 1000;
        return;
    }

    /* ---- ③ 底片（可选，失败不影响入库结果）----
     * ★ 用引擎里那份**刚采过特征的** RGB888：所见即所存，不必再抓一帧。 */
    {
        int tw = 0, th = 0;
        const uint8_t *f = face_recog_frame(&tw, &th);
        if (f && tw > 0 && th > 0) {
            /* ★ 同步阻塞 ~30~60 ms：整个录入流程**只此一次**，可以接受。
             *   失败只 WARN —— 库里的特征才是识别要用的东西。 */
            (void)punch_service_write_thumb(id, f, tw, th);
        }
    }

    ESP_LOGI(TAG, "enrolled: id=%u name=%s", (unsigned)id, s_enroll_name);

    char tip[96];
    snprintf(tip, sizeof(tip), "Enrolled: %s  (%d/%d)", s_enroll_name, face_db_count(), FACE_DB_MAX);
    enroll_finish_to_user(tip);
}

/** 录入采样：拿到一张特征 */
static void enroll_on_result(const recog_result_t *r)
{
    if (s_enroll_state != ENROLL_CAPTURING) return;

    if (!r->valid) {
        enroll_msg_set("No face detected - keep facing the camera");
        s_enroll_next_us = now_us() + 400 * 1000;    /* 快一点重试，别让用户干等 */
        return;
    }
    if (!s_enroll_feats) {
        /* ★★★ 2026-09-28：这条**绝不能再静默** —— 上面那次回归就是死在这里，
         *   用户看到的是"capturing 0/3 卡住"，串口却没有任何解释。
         *   现在直接把原因显示到覆盖层上（用户看的就是这一层）。 */
        ESP_LOGE(TAG, "enroll: feat buf NULL - alloc was skipped (see enroll_feats_alloc)");
        enroll_msg_set("Internal: feature buffer missing - re-enter ENROLL");
        s_enroll_state = ENROLL_OFF;
        return;
    }

    const int dim = RECOG_FEAT_DIM;
    memcpy(&s_enroll_feats[s_enroll_have * dim], r->feat, (size_t)dim * sizeof(float));
    s_enroll_have++;

    char m[64];
    snprintf(m, sizeof(m), "Captured %d/%d - hold still", s_enroll_have, ENROLL_SHOTS);
    enroll_msg_set(m);
    ESP_LOGI(TAG, "enroll: sample %d/%d", s_enroll_have, ENROLL_SHOTS);

    if (s_enroll_have >= ENROLL_SHOTS) {
        enroll_commit();
        return;
    }
    s_enroll_next_us = now_us() + (int64_t)ENROLL_SHOT_GAP_MS * 1000;
}

/** 覆盖层上的 START（也由键盘的 ✓ 触发） */
static void enroll_start_cb(lv_event_t *e)
{
    (void)e;
    if (s_enroll_state != ENROLL_NAME) return;

    const char *nm = s_enroll_ta ? lv_textarea_get_text(s_enroll_ta) : "";
    if (nm == NULL || nm[0] == '\0') {
        ESP_LOGW(TAG, "enroll start refused: empty name");
        enroll_msg_set("Please type a name first");
        return;
    }

    /* ★ 重名在这里先挡一次（face_db_enroll 也会挡，但那时已经采完 3 张了，
     *   让用户白站几秒很不礼貌）。 */
    face_person_t exist;
    if (face_db_find_by_name(nm, &exist) == ESP_OK) {
        ESP_LOGW(TAG, "enroll start refused: name '%s' already used (id=%u)", nm, (unsigned)exist.id);
        enroll_msg_set("Name already used - pick another");
        return;
    }

    /* ★ 长度截断按 UTF-8 边界（face_db 的同一套规则）——
     *   绝不能切出半个汉字，那会让目录名与 CSV 都变乱码。 */
    face_db_sanitize_name(nm, s_enroll_name, sizeof(s_enroll_name));
    if (s_enroll_name[0] == '\0') {
        ESP_LOGW(TAG, "enroll start refused: name '%s' not acceptable after sanitize", nm);
        enroll_msg_set("Name not acceptable");
        return;
    }
    if (strcmp(nm, s_enroll_name) != 0) {
        /* 截断/替换发生了 ⇒ 让用户看见"实际会存成什么"，别等入库后才发现 */
        ESP_LOGW(TAG, "enroll name sanitized: '%s' -> '%s'", nm, s_enroll_name);
    }

    s_enroll_state        = ENROLL_CAPTURING;
    s_enroll_have         = 0;
    s_enroll_next_us      = now_us();
    s_enroll_deadline_us  = now_us() + (int64_t)ENROLL_TIMEOUT_MS * 1000;
    enroll_overlay_show(false);        /* 收掉覆盖层，让用户看见自己的脸 */
    enroll_msg_set("");
    s_frame_skip = 99;
    ESP_LOGI(TAG, "enroll start: name=%s", s_enroll_name);
}

static void enroll_cancel_cb(lv_event_t *e)
{
    (void)e;
    enroll_finish_to_user("Enrollment cancelled");
}

static void enroll_ta_ready_cb(lv_event_t *e)
{
    (void)e;
    enroll_start_cb(e);                /* 键盘 ✓ = 点 START */
}

/* ===========================================================================
 *  ★★★ 2026-09-28（解耦）：识别引擎的**需求判据** —— 全页唯一真相源
 *
 *  用户要求："按下打卡的时候人脸识别和模型才开始加载，打卡完成后模型要彻底
 *  释放掉，人脸录入模式同理；det 按钮仅仅触发 face/hand 模型检测"。
 *
 *  ⇒ 识别引擎（MSR/MNP + MFN，峰值 ≈ 2.11 MB PSRAM）**只**由下面两个会话驱动：
 *      · 打卡会话：右侧圆形按钮已按下（s_punch_armed）；
 *      · 录入会话：ENROLL 覆盖层已打开（ENROLL_NAME）或正在采集（ENROLL_CAPTURING）。
 *    两个会话都结束 ⇒ recog_need()=false ⇒ recog_pump() ③ 下一拍
 *    face_recog_stop() 把全部内存还回去。
 *
 *  ★ 它**不看** face_service_enabled() / face_service_state() / face_model_active()
 *    —— 那三个是 det 按钮的领地。识别引擎自带 MSR+MNP（自己检脸 + 5 点关键点），
 *    根本不需要 espdet 的框，两者本来就是两套独立的模型。
 * =========================================================================== */
static bool recog_need(void)
{
    if (s_mode == CAM_MODE_ENROLL) return s_enroll_state != ENROLL_OFF;
    return s_punch_armed;
}

/* ===========================================================================
 *  ★★ S8-2：识别/录入的**唯一**驱动点
 * =========================================================================== */
static void recog_pump(void)
{
    int64_t now = now_us();

    /* ---- ① 结果回收（无论哪个模式都要做，否则引擎永远处于 busy）---- */
    const recog_result_t *r = face_recog_result();
    if (r != NULL) {
        if (s_mode == CAM_MODE_ENROLL && s_enroll_state == ENROLL_CAPTURING) {
            enroll_on_result(r);
        } else if (s_mode == CAM_MODE_USER) {
            user_on_result(r);
        }
        face_recog_result_consume();
    }

    /* ---- ② 打卡落盘收尾提示（消费掉结果，保证只提示一次）---- */
    if (!punch_service_busy()) {
        punch_record_t pr;
        memset(&pr, 0, sizeof(pr));
        punch_service_get_last(&pr);
        /* ★ 判据是 `has_result`，**不是** `elapsed_ms != 0` ——
         *   后者会被"同一 tick 内立刻失败"的早退路径撞上（见 punch_service.h），
         *   那样"打卡失败"永远弹不出来，用户只看到"按了没反应"。 */
        if (pr.has_result) {
            if (pr.clear_op) {
                /* ★ §73：clear_op 现在覆盖三种维护操作（清空/删单条/对账），
                 *   文案用中性的 "updated"，避免删一条也弹 "records cleared"。 */
                tip_set(pr.err == ESP_OK ? "Attendance records updated"
                                         : "Attendance update failed");
            } else if (pr.err == ESP_OK) {
                tip_set("Punched: %s  %s", pr.name,
                        pr.time_valid ? pr.path + strlen(pr.path) - 15 : "--");
            } else {
                tip_set("Punch failed: %s", esp_err_to_name(pr.err));
            }
            punch_service_clear_last();
        }
    }

    /* ---- ③ 引擎开关对账（单一真相源：recog_need()）----
     * ★★★ 2026-09-28（解耦）：旧表达式是
     *   `want = face_service_enabled() && face_model_active() && state==RUNNING && db_ready`
     *   ⇒ 只要 det 按钮开着，识别引擎（MSR/MNP+MFN ≈ 2.11 MB 峰值）就**常驻**，
     *     用户不打卡它也一直占着 —— 这正是"按钮功能没解耦"的病根。
     *   新判据只看"有没有会话需要它"（打卡 / 录入），det 开关不再是输入。
     * ★ 用对账而不是"在回调里开关"：期望值一处算、一处执行，
     *   切档失败、拔卡、会话结束这些路径都自动收敛。 */
    {
        bool want = recog_need()
                 && face_db_is_ready()
                 && camera_service_is_running();
        if (want != s_recog_want) {
            if (want) {
                face_recog_set_enabled(true);
                s_recog_want = true;
            } else {
                /* ★ stop() 在"识别任务或加载任务正在跑"时会拒绝（防止 UAF，见实现）；
                 *   那时把期望值留在 true，下一拍再试 —— 这就是"重试"的写法。 */
                esp_err_t e = face_recog_stop();
                s_recog_want = (e == ESP_ERR_INVALID_STATE);
            }
        }
    }

    /* ---- ③b ★★★ 驱动识别引擎（**这一行是 2026-09-28 P0 事故的修复**）----
     *
     * `face_recog_poll()` 是 `s_load_pending` 的**唯一消费者**。漏掉它 ⇒
     * `face_recog_set_enabled(true)` 只把标志置上、**永远没人执行加载**
     * ⇒ 引擎停在 FACE_LOADING ⇒ `face_recog_submit()` 每次都被拒且**静默**
     * ⇒ ENROLL 卡 `capturing 0/3`、USER 永远不打卡，用户侧表现 = "按了没反应"。
     *
     * ★ 本函数（非阻塞：真正的 engine_load 被派发到一次性任务，栈 10240）在
     *   LVGL 线程调用是**安全的、且刻意如此** —— 不再依赖 cam_task 是否在跑
     *   （cam_task 只有 3584 B 栈，装不下 MFN）。
     * ★ 必须放在"对账"之后：这一拍刚 set_enabled 置上的 pending 当拍就能被消费。 */
    face_recog_poll();

    /* ---- ④ 发起新作业 ---- */
    if (!s_recog_want || face_recog_busy()) return;
    if (!camera_service_is_running() || s_media_taken) return;
    /* ★ 用户看得见的"脸"还没被检测出来 ⇒ 不烧 400 ms 去做识别（先便宜后昂贵，
     *   方案 §14.2 第 2 条）。检测框来自 espdet（每 3 帧一次），够用了。 */
    face_quality_t q;
    last_box_quality(&q);

    if (s_mode == CAM_MODE_USER) {
        /* ★★★ 2026-09-28：**只有按下按钮才识别**（旧行为是每 1.5 s 自动来一发，
         *   用户完全感觉不到"打卡"这件事需要他做什么）。
         *   没按下 ⇒ 这里什么都不做，CPU/PSRAM 全让给取景器。 */
        if (!s_punch_armed) return;

        /* ---- 窗口收尾：一直没拿到可用结果（没人脸 / 引擎始终没就绪）---- */
        if (now > s_punch_deadline_us) {
            /* ★ 必须给出**具体**原因：只写"超时"等于把"按了没反应"换个说法。
             * ★★ 2026-09-28（解耦）：这里**不再看 espdet 的框**（q.hint）——
             *   det 关着时那是假的"没脸"。识别引擎自己会报有没有检出。 */
            const char *why;
            if (!face_recog_ready()) {
                why = (face_recog_state() == FACE_FAIL) ? "Engine load FAILED - see log"
                                                        : "Engine not ready - try again";
            } else if (s_frames_in == 0) {
                why = "No frames - tap STOP then START";
            } else {
                why = "No face detected - try again";
            }
            ESP_LOGW(TAG, "punch window expired (%d ms) - %s", s_punch_window_ms, why);
            tip_set("%s", why);
            punch_disarm();
            return;
        }

        if (now < s_next_recog_us) return;       /* 提交节流（含"上次被拒"的退避） */
        /* ★★★ 2026-09-28（解耦）：**删掉旧版的 `if (!q.ok) return;`**。
         *   旧逻辑用 espdet 的框做"先便宜后昂贵"的前置门，但那会让打卡
         *   **依赖 det 按钮的状态** —— 与"det 按钮只管 face/hand 检测"直接冲突。
         *   识别引擎自带 MSR+MNP（自己检脸 + 5 点），不需要 espdet 的框。
         *   代价 = 一次按下会真跑一次 MSR+MNP+MFN（≈400 ms）；但打卡是**手动**
         *   触发的，窗口内最多重试几次 —— 用这点代价换两个功能彻底解耦，划算。 */
        /* ★ 失败必须留话（以前完全静默，是"按了没反应"的来源之一）：
         *   最常见的就是"引擎还没加载完"，其次是"上一拍还在跑"。
         *   失败时把下一拍推后 500 ms，别每 50 ms 撞一次。 */
        esp_err_t se = face_recog_submit(s_canvas_buf, CAM_W, CAM_H, RECOG_JOB_IDENTIFY);
        if (se == ESP_OK) {
            s_next_recog_us = now + (int64_t)RECOG_PERIOD_MS * 1000;
        } else {
            skip_log(SKIP_NOT_READY, "identify submit refused (%s) ready=%d engine=%d",
                     esp_err_to_name(se), (int)face_recog_ready(), (int)face_recog_state());
            s_next_recog_us = now + 500 * 1000;
        }
        return;
    }

    if (s_mode == CAM_MODE_ENROLL && s_enroll_state == ENROLL_CAPTURING) {
        if (now > s_enroll_deadline_us) {
            /* ★ 超时是"用户站了很久却没采满"—— 验收时要知道**采到了几张**才能判断
             *   是识别太慢还是人脸一直不合格，所以把计数打出来。 */
            ESP_LOGW(TAG, "enroll timeout after %d ms: only %d/%d samples"
                          " (last hint: %s)",
                     ENROLL_TIMEOUT_MS, s_enroll_have, ENROLL_SHOTS,
                     s_enroll_msg ? lv_label_get_text(s_enroll_msg) : "?");
            enroll_finish_to_user("Enroll timeout - face the camera and retry");
            return;
        }
        if (now < s_enroll_next_us) return;

        /* ★★ 引擎没就绪 —— 这一步以前**完全静默**，用户只看到 "capturing 0/3" 不动，
         *   唯一的解释只存在于代码里。现在把它显示在覆盖层上（用户看的就是这一层），
         *   并且限频留一条日志（含状态码，一次烧录就能定位是"没加载"还是"加载失败"）。 */
        if (!face_recog_ready()) {
            face_state_t rs = face_recog_state();
            const char *why = (rs == FACE_FAIL)  ? "Engine load FAILED - see serial log"
                            : (rs == FACE_LOADING) ? "Starting engine (takes a few seconds)"
                                                   : "Engine off - tap FACE / MODEL";
            enroll_msg_set(why);
            skip_log(SKIP_NOT_READY, "enroll waiting on engine (state=%d, ready=0)", (int)rs);
            s_enroll_next_us = now + 400 * 1000;
            return;
        }

        if (!q.ok) {
            /* 不合格就只在覆盖层上给引导，不动 next_us 太多（快重试） */
            skip_log(SKIP_ENROLL_Q, "enroll quality - %s (det=%d%%)", q.hint, q.score_pct);
            enroll_msg_set(q.hint);
            s_enroll_next_us = now + 300 * 1000;
            return;
        }
        esp_err_t se = face_recog_submit(s_canvas_buf, CAM_W, CAM_H, RECOG_JOB_EXTRACT);
        if (se != ESP_OK) {
            skip_log(SKIP_NOT_READY, "extract submit refused (%s) - enroll cannot capture",
                     esp_err_to_name(se));
            s_enroll_next_us = now + 500 * 1000;
        }
    }
}

/* ===========================================================================
 *  ★★ S8-1：ENROLL 态的"就绪度"判定
 *
 *  这套判据在 S8-2 接上采集之后仍然有用：它回答的是"**为什么现在采不了**"。
 *  顺序是"从外到内"的因果链，不要打乱：
 *      有没有预览 → 有没有库 → 库满没满 → 档位对不对 → 模型在不在
 *      → 检测开没开 → 有没有脸 → 脸合不合格
 *  反过来说，若把"脸在哪"排在"有没有库"前面，就会出现"提示你靠近一点，
 *  走近了才告诉你库根本没插卡"这种来回折腾人的引导。
 * =========================================================================== */
static const char *enroll_status(char *buf, size_t n)
{
    face_state_t   st;
    face_quality_t q;

    /* ★★ 2026-09-28 事故补的：**识别引擎**没就绪时，采集一步都走不动
     *   （recog_pump 里的 submit 会被直接拒），而以前这里照样显示 "capturing 0/3"
     *   ⇒ 用户看到的是"卡住没反应"，唯一解释只存在于代码里。
     *   ⚠ 必须排在 CAPTURING 分支**之前**：否则进了 CAPTURING 就再也看不到原因。
     *   ⚠ 与下面那段 face_service_state() 的区别：那个管**检测模型**（espdet），
     *     这个管**识别引擎**（MSR/MNP + MFN）—— 两者是独立加载的，报错也要分开报。 */
    if (!face_recog_ready()) {
        st = face_recog_state();
        if (st == FACE_FAIL)    return "ENROLL: engine load FAIL (see log)";
        if (st == FACE_LOADING) return "ENROLL: engine loading ...";
        /* ★（解耦）引擎是按需加载的：进入 ENROLL 后 recog_need() 恒真，
         *   IDLE 只会存在一两拍，随后自动转 LOADING —— 措辞要跟上这个语义。 */
        return "ENROLL: engine starting ...";
    }

    if (s_enroll_state == ENROLL_CAPTURING) {
        snprintf(buf, n, "ENROLL: capturing %d/%d", s_enroll_have, ENROLL_SHOTS);
        return buf;
    }
    if (!camera_service_is_running()) {
        return "ENROLL: start camera first";
    }
    if (!face_db_is_ready()) {
        return "ENROLL: face db not ready (SD?)";
    }
    if (face_db_count() >= FACE_DB_MAX) {
        return "ENROLL: db full 10/10";
    }
    if (!face_model_active()) {
        return "ENROLL: switch MODEL to FACE";
    }
    /* ★ 模型状态单独报：加载要 1 s 上下，这期间"no face in view"是**假**的
     *   （相机在跑、只是还没人消费帧），照实说"loading"才不误导。 */
    st = face_service_state();
    if (st == FACE_FAIL) {
        return "ENROLL: model build FAIL";
    }
    if (st == FACE_LOADING) {
        return "ENROLL: model loading...";
    }
    /* ★ 检测开关的唯一真相源仍是 FACE 按钮。进入 ENROLL 时我们会自动打开它，
     *   但用户随时可以按 FACE 关掉 ⇒ 这里必须再查一次，不能假设它一直开着。 */
    if (!face_service_enabled()) {
        return "ENROLL: tap FACE to detect";
    }
    if (!s_box_valid) {
        /* ★★★ 2026-09-28：必须把"相机一帧都没送到"与"有帧但没脸"分开说。
         *   前者用户凑得再近也没用（回调没注册 / 档位不对），后者的对策才是"站近一点"。
         *   上面已经挡掉了 `!camera_service_is_running()`，所以走到这里 frames==0
         *   就只有一种可能：流在跑，但帧没送到本页。 */
        if (s_frames_in == 0) {
            return "ENROLL: no frames - tap STOP then START";
        }
        return "ENROLL: no face in view";
    }

    last_box_quality(&q);
    if (q.ok) {
        snprintf(buf, n, "ENROLL: ready - %d/%d used", face_db_count(), FACE_DB_MAX);
    } else {
        /* 置信度那一条额外的百分比对排查光照问题很有用（"是没正对"还是"太暗"）。 */
        snprintf(buf, n, "ENROLL: %s (%d%%)", q.hint, q.score_pct);
    }
    return buf;
}

static void refresh(lv_timer_t *t)
{
    (void)t;

    /* ---- ★★ S2-0 / 方案 §2.5.5：相机被 VIDEO tab 独占时的处理 ----
     * 录像可能持续几分钟。若此时用户切回 CAM 页，**必须明确置灰 + 提示**，
     * 绝不能"按下去没反应"（那是最糟的体验，也是本工程反复踩过的坑）。
     * ★ 唯一真相源是 camera_service_owner()：不要用 is_running() 去猜
     *   —— "有流在跑" ≠ "本页拥有它"。 */
    {
        bool taken = (camera_service_owner() == CAM_OWNER_MEDIA);
        if (taken != s_media_taken) {
            s_media_taken = taken;
            /* LV_STATE_DISABLED 一举两得：既挡点击，又让 LVGL 自动把按钮画成灰态
             * （比手工调 bg_color 更稳，也不会和 FACE 按钮的状态色逻辑打架）。
             * ★ S8-1：MODE 也加进来 —— 录像独占相机时进 ENROLL 毫无意义
             *   （mode_btn_cb 也会因为 camera_service_is_running() 被拒，但那是
             *    "按下去弹一句错误"，不如直接置灰来得体面）。 */
            lv_obj_t *btns[4] = { s_btn, s_btn_face, s_btn_model, s_btn_mode };
            for (int i = 0; i < 4; i++) {
                if (!btns[i]) continue;
                if (taken) lv_obj_add_state(btns[i], LV_STATE_DISABLED);
                else       lv_obj_remove_state(btns[i], LV_STATE_DISABLED);
            }
            lv_label_set_text_static(s_status, taken ? "Cam busy: VIDEO recording"
                                                    : (camera_service_is_running() ? "Preview running"
                                                                                  : "Stopped"));
        }
    }

    /* ---- ★★ 悬垂指针防护（S2-0 顺带修的既有隐患）----
     * on_frame() 只记指针**不拷贝**，而 cam_task 在退出时会 munmap 掉那些缓冲
     * （camera_service.c 里 "资源释放由本任务独占负责"）。
     * ⇒ 只要流被停掉（本页按 STOP、或 VIDEO tab 接管），s_last_frame 立刻变成
     *   **指向已解除映射地址的野指针**，下一帧 refresh 读它就是 LoadProhibited。
     * ★ 判据用 is_running()（它把"任务还没退"也算 running）⇒ 只有任务**确实退出、
     *   缓冲确实已释放**时才清，绝不会清掉正在用的指针。 */
    if (!camera_service_is_running()) {
        s_last_frame = NULL;
        s_frame_new = false;
    }

    /* ★ 让 START/STOP 文案跟**真实**状态走：camera_service_is_running() 把"任务还没退"
     *   也算 running（stop() 超时返回时不能显示成"已停"）。不做这一步的话，超时后
     *   重进页面会显示 START，而按下时 start() 会以 ESP_ERR_INVALID_STATE 拒绝，
     *   用户只看到一句 "Camera open failed" —— 正是 P0-3 想消掉的那种误导。 */
    if (s_btn_label) {
        const char *want = camera_service_is_running() ? "STOP" : "START";
        const char *txt  = lv_label_get_text(s_btn_label);
        if (txt == NULL || strcmp(txt, want) != 0) {
            lv_label_set_text_static(s_btn_label, want);
        }
    }
    /* ---- FACE 按钮：文案/颜色由 (模型状态, 检测开关) **两个**值决定 ----
     * ★★ 常驻策略下模型状态会长期停在 FACE_RUNNING，"检测开着没有"只能看
     *    face_service_enabled()。只看 state 会让按钮永远显示 "FACE ON"。
     * ★ F47 的精确表述：**要"随 state 变化的那个颜色通道"必须设在按钮/容器上**。
     *    text_color 是可继承属性，继承时取父对象**当前 state** 的 selector；子对象
     *    不继承 state ⇒ 一旦在 label 上写死 text_color，父的按下/切换态就再也传不进文字。
     *    ⇒ 这里状态是由**按钮背景色**表达的（bg_color 设在按钮上），文字固定 COL_DARK
     *      是**有意为之**：青/红底上深字才看得清。**别把 label 的 text_color 删掉去"改正"**，
     *      删了会退回主题继承色，在青底上变成浅字。
     *    （tab_pcnow.c 三个媒体键是"文字/图标本身要变色"的场景，那种才要求 label 不设色。）
     * ★ 单一真相源：本函数每 50 ms 跑一次，是**唯一**写文案/颜色的地方；
     *    事件回调只发动作、不碰文案（否则两处早晚不一致）。 */
    face_state_t st = face_service_state();
    bool face_en = face_service_enabled();
    s_face_on = face_en;                  /* 让下面 detect() 的调用条件跟着走 */
    if (s_btn_face_label) {
        const char *want;
        lv_color_t  col;
        if (st == FACE_FAIL)         { want = "FAIL";    col = lv_color_hex(0xFF0000); }
        else if (st == FACE_LOADING) { want = "LOAD";    col = lv_color_hex(0xFFFF00); }
        else if (face_en)            { want = "DET ON";  col = lv_color_hex(0x00FF00); }
        else                         { want = "DET";     col = COL_CYAN; }
        const char *cur = lv_label_get_text(s_btn_face_label);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_static(s_btn_face_label, want);
            lv_obj_set_style_bg_color(s_btn_face, col, 0);
            if (st == FACE_FAIL)         lv_label_set_text_static(s_status, "Model build FAIL");
            else if (st == FACE_LOADING) lv_label_set_text_static(s_status, "Model working...");
            else if (face_en)            lv_label_set_text_static(s_status, "Model ready");
            else                         lv_label_set_text_static(s_status, "Detect off");
        }
    }
    /* ---- MODEL 按钮：显示**目标**档位 ----
     * 切换进行中时 current_model() 已经返回目标档位（见 target_model_id 的注释），
     * 所以这里不需要额外处理"正在切"的情况；忙态由 FACE 按钮的 LOAD 表达。 */
    if (s_btn_model_label) {
        const char *want = face_service_model_label(face_service_current_model());
        const char *cur  = lv_label_get_text(s_btn_model_label);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_static(s_btn_model_label, want);
            lv_label_set_text_fmt(s_status, "Model: %s", want);
        }
    }
    /* ★ 录像期间把状态钉住：上面 FACE/MODEL 两块在状态变化时也会写 s_status，
     *   这里再写一次，保证"被占用"这条信息永远不被盖掉
     *   （set_text_static 幂等、无堆分配，每 50 ms 调一次没有代价）。 */
    if (s_media_taken) {
        lv_label_set_text_static(s_status, "Cam busy: VIDEO recording");
    }

    /* ★ §60：模型不在 RUNNING / 检测关 ⇒ 覆盖层收掉（切档/关断不残留旧结果） */
    if (st != FACE_RUNNING || !face_en) {
        if (s_gest_label) lv_obj_set_hidden(s_gest_label, true);
        /* ★ S8-1：检测关掉后那份缓存框就过期了。不清的话 ENROLL 态会拿着一张
         *   "上一档位/上一次检测"的旧框给出误导性引导（比如人早就走开了还显示 Ready）。 */
        s_box_valid = false;
    }

    if (s_frame_new && s_last_frame && s_canvas) {
        s_frame_new = false;
        lv_lock();
        /* Sensor outputs RGB565 big-endian; LVGL canvas expects little-endian.
         * ★★ F48/CPU-1：hflip + vflip（= 旋转 180°）已经挪到 OV3660 硬件做，
         *   见 camera_service_start() 里的 VIDIOC_S_EXT_CTRLS(V4L2_CID_HFLIP/VFLIP)。
         *   本循环因此从"逆序读 + 跨行倒序 + swap"退化成**纯顺序**的 swap。
         *
         *   为什么这个顺序化值钱：原来的 `src_row[CAM_W-1-x]` 是逆序访问，一个 64 B
         *   cache line 只命中 1 个像素，57,600 像素/帧全变 cache miss，而 src/dst 都在
         *   PSRAM —— 和 RGB 屏 GDMA 持续读帧缓冲（36 MB/s）正面抢带宽。
         *   （改前还要再叠一次 bounce ISR 每 444 us 搬 16,000 B，那个已随
         *     2026-09-25 切到 auto refresh 模式而消失；但本循环的收益不变。）
         *   顺序访问后一条 cache line 能覆盖 16 个像素，等于把这里的 PSRAM 事务数砍到 1/16。
         *
         *   ★ 不要在这里再加任何索引反转 —— 画面上/下或左/右反了，去改传感器那两行寄存器，
         *     不要在 CPU 上加回来（那正是这条链路的病根）。 */
        const uint16_t *src = (const uint16_t *)s_last_frame;
        uint16_t *dst = s_canvas_buf;
        for (int y = 0; y < CAM_H; y++) {
            const uint16_t *s = src + y * CAM_W;
            uint16_t *d = dst + y * CAM_W;
            for (int x = 0; x < CAM_W; x++) {
                uint16_t p = s[x];
                d[x] = (uint16_t)((p >> 8) | (p << 8));   /* byte swap */
            }
        }
        lv_obj_invalidate(s_canvas);
        /* ★ §73.12：HSV 颜色识别块已移除（原 s_hsv_on && s_bbox 的红框检测）——
         *   该功能与"人脸打卡"主线无关，按用户要求整体下线。 */
        if (s_face_on && s_face_bbox) {
            s_frame_skip++;
            if (s_frame_skip >= 3) {  /* run detection every 3 frames */
                s_frame_skip = 0;
                vision_result_t vr[VISION_MAX_RESULTS];
                int n = face_service_detect(s_canvas_buf, CAM_W, CAM_H, vr, VISION_MAX_RESULTS);
                s_detect_calls++;    /* ★ 诊断：真正调进检测的次数（不涨 = 检测根本没跑） */
                /* 画"最大"的那个结果（与旧人脸逻辑同款选择） */
                int idx = -1;
                int max_area = 0;
                for (int i = 0; i < n; i++) {
                    int a = vr[i].w * vr[i].h;
                    if (a > max_area) { max_area = a; idx = i; }
                }
                if (idx >= 0) {
                    s_detect_hits++;    /* ★ 诊断：检测确实给出了框（=0 说明真没检出脸） */
                    lv_obj_set_pos(s_face_bbox, vr[idx].x, vr[idx].y);
                    lv_obj_set_size(s_face_bbox, vr[idx].w, vr[idx].h);
                    lv_obj_set_hidden(s_face_bbox, false);
                    /* ★ S8-1：把"最大框"缓存给 ENROLL 态的质量引导用
                     *   （本块跑在 lv_lock 临界区里，只做一次结构体拷贝，不加负担）。 */
                    s_last_box  = vr[idx];
                    s_box_valid = true;
                    /* ★ HAND：手势名 + 置信度 */
                    if (s_gest_label) {
                        if (vr[idx].label[0]) {
                            lv_label_set_text_fmt(s_gest_label, "%s %d%%",
                                                  vr[idx].label, (int)(vr[idx].score * 100));
                            lv_obj_set_hidden(s_gest_label, false);
                        } else {
                            lv_obj_set_hidden(s_gest_label, true);
                        }
                    }
                } else {
                    lv_obj_set_hidden(s_face_bbox, true);
                    if (s_gest_label) lv_obj_set_hidden(s_gest_label, true);
                    s_box_valid = false;
                }
            }
        }
        lv_unlock();
    }

    /* =======================================================================
     *  ★★★ 2026-09-28：把"相机在跑，但本页拿不到合格帧"这条**硬故障**显式报出来。
     *
     *  为什么必须有这一块：`on_frame()` 只在 `btn_cb` 里 `camera_service_start(on_frame)`
     *  被调用时注册。于是存在一种**静默失效**——相机因为别的原因在跑（owner 仍是
     *  PREVIEW，`camera_service_is_running()` 为真），但**这个回调从没注册过**
     *  （页面重建后相机还在跑、用户没按过 START）。此时：
     *      · 相机在跑 ⇒ recog_pump 一路通过，ENROLL 会走到 "capturing 0/3" 然后超时；
     *      · 没有帧   ⇒ 检测一次都不跑 ⇒ `s_box_valid` 永远 false；
     *      · 用户看到的就是那句**假的** "No face in view"，怎么站近都没用。
     *  同理，帧尺寸不是 240×240 时会被 `on_frame` 全丢（档位没切回来），
     *  症状一模一样。
     *
     *  两者都只是"静默"而已 —— 本块把它们变成一条带对策的错误日志。
     *  连报 40 拍（2 s）才认，避免刚建页那几拍误报。
     * ======================================================================= */
    if (camera_service_is_running() && !s_noframe_warned &&
        camera_service_has_frame_cb() &&   /* ★ §73.16：VIDEO 页取景流回调为 NULL —— 它收不到帧是正常，不吼 */
        (s_frames_in == 0 || s_frames_in == s_frames_dropped)) {
        if (++s_noframe_ticks >= 40) {
            s_noframe_warned = true;
            if (s_frames_in == 0) {
                ESP_LOGE(TAG, "camera running (owner=%d) but ZERO frames reached this page "
                              "-> on_frame() is NOT registered: tap STOP then START "
                              "(detect/recog are dead without frames)",
                         (int)camera_service_owner());
            } else {
                ESP_LOGE(TAG, "camera running but ALL %u frames were dropped as wrong size "
                              "-> stream is not %ux%u (another tab's format still active?)",
                         (unsigned)s_frames_in, CAM_W, CAM_H);
            }
        }
    } else if (s_frames_in != 0 && s_frames_in != s_frames_dropped) {
        s_noframe_ticks = 0;        /* 有合格帧 ⇒ 复位，下一轮故障重新计数 */
    }

    /* =======================================================================
     *  ★★ S8-2：识别/打卡/录入驱动
     *
     *  ★ 位置必须在帧处理块**之后**：它要用刚更新的 s_box_valid / s_last_box
     *    （"先便宜后昂贵"：检测确认有脸才值得烧 400 ms 做识别）。
     *  ★ 本函数跑在 LVGL 线程；所有慢活都由它**投递**给一次性任务，自己不阻塞。
     * ======================================================================= */
    recog_pump();

    /* =======================================================================
     *  ★★ S8-1：MODE 按钮（USER ↔ ENROLL）与模式化状态栏
     *
     *  ★ 位置必须在这里（帧处理块之后）、且必须判 !s_media_taken：
     *    上面 s_media_taken 那段是"状态栏钉死"逻辑（录像独占时不许被盖掉），
     *    本块若写在它前面就会把那条提示盖回去。
     *  ★ 文案/颜色仍由本函数独占（同 FACE/MODEL）：mode_btn_cb 只翻 s_mode。
     * ======================================================================= */
    if (s_btn_mode_label) {
        const char *want = (s_mode == CAM_MODE_ENROLL) ? "ENROLL" : "USER";
        const char *cur  = lv_label_get_text(s_btn_mode_label);
        if (cur == NULL || strcmp(cur, want) != 0) {
            lv_label_set_text_static(s_btn_mode_label, want);
            /* 琥珀色 = "这是特殊态，行为与平时不同"；与 FACE 的绿/青/红三态不冲突。 */
            lv_obj_set_style_bg_color(s_btn_mode,
                                      (s_mode == CAM_MODE_ENROLL) ? lv_color_hex(0xFBBF24)
                                                                  : COL_CYAN, 0);
        }
    }

    /* ---- 画布上的识别/打卡结果标签（USER 态）---- */
    if (s_recog_label) {
        if (s_mode == CAM_MODE_ENROLL) {
            lv_obj_set_hidden(s_recog_label, true);
        } else if (tip_active()) {
            lv_label_set_text(s_recog_label, s_tip);     /* ★ 拷贝语义：s_tip 是可变缓冲 */
            lv_obj_set_hidden(s_recog_label, false);
        } else {
            lv_obj_set_hidden(s_recog_label, true);
        }
    }

    /* ---- ★★★ 2026-09-28：画布左侧的两行时间显示 ----
     *  · 数据源 = net_time_service（与打卡水印同一套真相源）；
     *  · 未校时（断网/还没拿到 NTP）⇒ **整块隐藏**，不是显示横线占位；
     *  · 文本没变就不写 label（refresh 50ms 一拍，秒没跳时零开销）。 */
    if (s_time_label) {
        net_time_t nt;
        net_time_service_get(&nt);
        if (!nt.valid) {
            lv_obj_set_hidden(s_time_label, true);
            s_time_txt[0] = '\0';
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "%04d.%02d.%02d\n%02d:%02d:%02d",
                     nt.year, nt.mon, nt.day, nt.hour, nt.min, nt.sec);
            if (strcmp(buf, s_time_txt) != 0) {
                snprintf(s_time_txt, sizeof(s_time_txt), "%s", buf);
                lv_label_set_text(s_time_label, s_time_txt);
            }
            lv_obj_set_hidden(s_time_label, false);
        }
    }

    /* ---- ★★★ 2026-09-28：手动打卡按钮的外观（**唯一**写入者，同 FACE/MODEL/MODE）----
     *   · ENROLL 态隐藏：ENROLL 有自己的 "OK START"，两颗启动按钮并存只会按错；
     *   · 文案 PUNCH ↔ SCAN 表示"待命 / 正在扫"；
     *   · 颜色：绿=可按下，琥珀=正在扫（琥珀在本工程的含义是"特殊态/等一下"，
     *     与 MODE 按钮 ENROLL 态同色）；**不用红** —— 红是"录制中/危险"。 */
    if (s_btn_punch) {
        if (s_mode == CAM_MODE_ENROLL) {
            lv_obj_set_hidden(s_btn_punch, true);
        } else {
            lv_obj_set_hidden(s_btn_punch, false);
            if (s_btn_punch_label) {
                const char *want = s_punch_armed ? "SCAN" : "PUNCH";
                const char *cur  = lv_label_get_text(s_btn_punch_label);
                if (cur == NULL || strcmp(cur, want) != 0) {
                    lv_label_set_text_static(s_btn_punch_label, want);
                }
            }
            lv_obj_set_style_bg_color(s_btn_punch,
                                      s_punch_armed ? lv_color_hex(0xFBBF24) : COL_GREEN, 0);
        }
    }

    if (!s_media_taken && s_status) {
        if (s_mode == CAM_MODE_ENROLL) {
            char txt[64];
            ui_common_set_label_text_if_changed(s_status, enroll_status(txt, sizeof(txt)));
        } else if (tip_active()) {
            ui_common_set_label_text_if_changed(s_status, s_tip);
        } else if (s_tip_was_active || s_mode != s_mode_shown) {
            /* ★ 一次性提示**过期的那一拍**要把状态栏交还回去；否则它永远停在
             *   "Punched: 张三" 上，检测状态就再也看不见了。
             *   若每拍都写，"Model ready"/"Detect off" 这些有用提示又会被盖掉 ——
             *   所以只在"过期的那一拍"或"刚切模式那一拍"写。
             * ★★ 2026-09-28：文案第二次改写。上一版讲的是"自动打卡"，现在打卡改成
             *   **右侧圆形按钮手动触发**，于是这里的任务是：
             *     ① 告诉用户"按那颗圆按钮"（这是"不知道怎么打卡"的直接答案）；
             *     ② 按不下去时，**提前**说清还差什么（而不是让用户白按一次再看 tip）。 */
            if (!camera_service_is_running()) {
                lv_label_set_text_static(s_status, "USER mode - tap START to preview");
            } else if (s_frames_in == 0) {
                lv_label_set_text_static(s_status, "USER mode - no frames (STOP then START)");
            } else if (s_punch_armed) {
                /* ★（解耦）引擎平时**未加载**（按需），所以只有 armed 期间才可能
                 *   看到"starting engine"；显示的是打卡会话自己的两个阶段。 */
                if (!face_recog_ready()) {
                    lv_label_set_text_static(s_status, "USER mode - starting engine ...");
                } else {
                    lv_label_set_text_static(s_status, "USER mode - scanning, hold still ...");
                }
            } else if (!sdcard_service_is_mounted()) {
                lv_label_set_text_static(s_status, "USER mode - no SD card (cannot save)");
            } else if (!face_db_is_ready()) {
                lv_label_set_text_static(s_status, "USER mode - face db not ready");
            } else if (face_db_count() <= 0) {
                lv_label_set_text_static(s_status, "USER mode - db empty, use ENROLL");
            } else {
                /* ★★★（解耦）USER 态**不再问 det 的状态**：旧版这里会提示
                 *   "switch MODEL to FACE" / "tap FACE to detect" —— 那是耦合的痕迹。
                 *   打卡走识别引擎自带的 MSR+MNP，与检测开关/档位无关。
                 *   引擎平时未加载（按需），所以也不显示 "engine loading"。 */
                lv_label_set_text_static(s_status, "USER mode - tap PUNCH to check in");
            }
        }
    }
    s_tip_was_active = tip_active();
    s_mode_shown = s_mode;
}

static void toggle_fullscreen(lv_event_t *e)
{
    (void)e;
    s_fullscreen = !s_fullscreen;
    if (s_fullscreen) {
        /* Fullscreen: zoom 2x (240->480), center on 800x480 */
        lv_obj_set_style_transform_zoom(s_canvas, 512, 0);
        lv_obj_set_style_transform_pivot_x(s_canvas, CAM_W/2, 0);
        lv_obj_set_style_transform_pivot_y(s_canvas, CAM_H/2, 0);
        lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_style_transform_zoom(s_canvas, 256, 0);
        lv_obj_set_parent(s_canvas, s_screen);
        lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    }
    /* ★ §73.11：两个悬浮元素跟随画布的两种中心 ——
     *   全屏态画布在内容区正中（偏移 0），普通态在 flex 列里（CAM_VIEW_Y_OFF）。 */
    int16_t yoff = s_fullscreen ? 0 : CAM_VIEW_Y_OFF;
    if (s_btn_punch)  lv_obj_align(s_btn_punch,  LV_ALIGN_RIGHT_MID, -34, yoff);
    if (s_time_label) lv_obj_align(s_time_label, LV_ALIGN_LEFT_MID,   34, yoff);
}

/* ★ §73.12：HSV 颜色识别已整体移除（用户要求阉割）——
 *   按钮 / 回调 / 黄色检测框 / color_hsv.c 全部下线（见 §73.12 清单）。 */

static void face_btn_cb(lv_event_t *e)
{
    (void)e;
    /* ★ 只发动作，**不写文案/颜色** —— 全部交给 refresh() 这个单一真相源。
     *   否则 cb 里写一套、refresh 里写一套，两个状态机早晚不一致（旧代码就是这样）。 */
    if (face_service_state() == FACE_LOADING) return;   /* 构建/切换中，忽略点击 */

    /* ★★★ 2026-09-28（解耦）：用户**手动**碰了 det 开关 ⇒ 所有权转移给用户。
     *   若此刻是 ENROLL 借用期（s_det_borrowed=true），退出时就不能代为归还了 ——
     *   否则用户明明自己按开的开关，会被录入流程的收尾悄悄关掉。 */
    s_det_borrowed = false;

    if (face_service_enabled()) {
        /* ★★ §73.10（用户定案：det 按钮也要"即用即释放"）：
         *   旧版这里只 face_service_stop() —— 关检测但模型**常驻**（D6 策略），
         *   ~1.25 MB PSRAM 一直挂到断电，VIDEO/取景器要连续块时全被它挤压
         *  （2026-09-28 深夜日志实录）。用户明确要求：按钮 OFF = 检测停止
         *   **且模型彻底卸载**，下次按下重新加载（~0.5 s，与首次开相同）。
         *   ★ 用 sync 版：相机在跑 → 投递给 cam_task 的 poll 消费（下一帧就执行）；
         *     相机停了 → 就地卸载（此时异步请求会变成死信 —— poll 只在 cam_task 跑）。 */
        face_service_set_enabled(false);
        face_service_unload_sync();
        if (s_face_bbox) lv_obj_set_hidden(s_face_bbox, true);
        if (s_gest_label) lv_obj_set_hidden(s_gest_label, true);
        /* ★ S8-2：识别引擎也跟着关（它的开关是 refresh() 对账出来的，
         *   这里不用管 —— 但状态栏上那条一次性提示要清掉，否则会看着很突兀）。 */
    } else {
        /* ★★ 只有"模型还没就绪"才需要相机在跑 —— 因为构建只能由 cam_task 的
         *    face_service_poll() 驱动。相机没跑就没人消费这个请求，状态会一直卡在
         *    FACE_LOADING，3 s 后被看门狗判 FAIL（有提示，但不如当场说清楚）。
         *    ★ §73.10 后 OFF 必卸载 ⇒ 再次 ON 时模型必然不在内存，"没相机也能开"
         *      的旧捷径不复存在 —— 这是"即用即释放"的代价，0.5 s 构建换 1.25 MB。 */
        if (face_service_state() != FACE_RUNNING && !camera_service_is_running()) {
            lv_label_set_text_static(s_status, "Start camera first");
            return;
        }
        face_service_set_enabled(true);   /* 模型没就绪会顺带触发构建 */
        s_frame_skip = 99;                /* 让下一帧立刻检测一次，别干等 3 帧 */
    }
}

/* ★ S1 新增：档位循环切换。
 *   ★ 走"页内按钮"而不是 tab/下拉，符合本项目 UI 约定
 *     （模式/角色切换用页内按钮触发，不用 tab 切换触发）。
 *   ★ 只投递请求：真正的"先卸后建"（~0.5 s 阻塞）由 cam_task 的
 *     face_service_poll() 执行 —— 绝不能在 LVGL 线程里构建模型。 */
static void model_btn_cb(lv_event_t *e)
{
    (void)e;
    if (face_service_state() == FACE_LOADING) return;   /* 切换中不叠请求 */

    /* ★★ 同上：切换也是由 cam_task 的 poll 执行。相机没跑就没人消费这个请求，
     *    状态会卡在 FACE_LOADING 直到看门狗判 FAIL ⇒ 这里当场拦住并说明。 */
    if (!camera_service_is_running()) {
        lv_label_set_text_static(s_status, "Start camera first");
        return;
    }

    int n = face_service_model_count();
    if (n <= 0) return;
    int nxt = (face_service_current_model() + 1) % n;

    esp_err_t err = face_service_request_model(nxt);
    if (err == ESP_ERR_INVALID_ARG) {
        lv_label_set_text_static(s_status, "Bad model id");
    }
    /* ESP_ERR_INVALID_STATE = 已经在这个档位，静默即可（不是错误）。
     * 成功时也不用在这里写文案 —— refresh() 会显示目标档位名与 LOAD。 */
}

/* ★★ S8-1 新增：USER ↔ ENROLL 循环（需求 R8）。
 *
 *  ★ 与 model_btn_cb 同款：**只发动作**，文案/颜色全部交给 refresh()。
 *
 *  ★ 进 ENROLL 前把四条**真实阻塞项**当场拦下并说清楚，而不是"先进去再提示"：
 *    · 没预览 → 没帧可采（构建检测模型也只能由 cam_task 驱动，见 face_btn_cb 注释）
 *    · 没库   → 采下来没处存（SD 未挂载 / 库未就绪）
 *    · 库满   → 录不进去（R9：10 人是硬上限，第 11 人必须被明确拒绝）
 *    · 档位不对 → HAND 档给的是手的框，过不了人脸门槛（见 face_model_active 注释）
 *    这比"让用户进去看半天引导、最后才发现库是满的"好得多。
 *    ⚠ 这里**不自动替用户切档位**：MODEL 是用户自己选的，悄悄改掉它比多按一次更烦人。
 *
 *  ★ 进 ENROLL 会**自动打开检测**：ENROLL 本身就意味着"要看脸"，让用户再多按一次
 *    FACE 是多余的。出 ENROLL 时**不关**检测 —— 那个开关的真相源是 FACE 按钮，
 *    擅自关掉会让用户明明按开的开关莫名其妙变灰（最招人烦的一类行为）。
 */
static void mode_btn_cb(lv_event_t *e)
{
    (void)e;

    if (s_mode == CAM_MODE_USER) {
        /* ★ 四条准入拦截各自留一条日志：它们都是"按下去没反应"的高发点，
         *   光在状态栏闪 5 s 的话，事后没法回答"我刚才为什么进不去"。 */
        if (!camera_service_is_running()) {
            ESP_LOGW(TAG, "ENROLL refused: camera not running");
            lv_label_set_text_static(s_status, "Start camera first");
            return;
        }
        if (!face_db_is_ready()) {
            ESP_LOGW(TAG, "ENROLL refused: face db not ready (SD mounted=%d)",
                     (int)sdcard_service_is_mounted());
            lv_label_set_text_static(s_status, "Face db not ready (SD?)");
            return;
        }
        if (face_db_count() >= FACE_DB_MAX) {
            ESP_LOGW(TAG, "ENROLL refused: face db full (%d/%d)", face_db_count(), FACE_DB_MAX);
            lv_label_set_text_static(s_status, "Face db full (10/10)");
            return;
        }
        if (!face_model_active()) {
            ESP_LOGW(TAG, "ENROLL refused: active model is not FACE (needs MODEL=FACE)");
            lv_label_set_text_static(s_status, "ENROLL needs MODEL=FACE");
            return;
        }
        /* ★ S8-1 补完：进 ENROLL 先弹姓名输入层（复用 LVGL 键盘，
         *   与 page_wifi 的连接子视图同一套交互）。 */
        s_mode          = CAM_MODE_ENROLL;
        s_enroll_state  = ENROLL_NAME;
        s_enroll_have   = 0;
        s_enroll_name[0] = '\0';
        /* ★★★ 2026-09-28（解耦）：记录"这个检测是**我们借的**"。
         *   ENROLL 需要检测（要框对齐 + 给用户对准反馈），但那个开关的真相源
         *   是 FACE 按钮 ⇒ 只有"本来没开、我们替用户开的"才在退出时归还
         *   （见 enroll_finish_to_user）。用户自己开着的绝不动。 */
        /* ★★★ 2026-09-28（§73 补分配端）：特征暂存**在进入录入模式时分配**，
         *   与 enroll_finish_to_user() 里的 enroll_feats_free() 成对。
         *   §72 只搬了释放端、漏了分配端 ⇒ 第二次进 ENROLL 时 s_enroll_feats
         *   还是 NULL，enroll_on_result() 把 3 张特征全部静默丢掉，UI 卡
         *   "capturing 0/3" 而串口 RECOG 侧 extract 全部成功 —— 现在补上，
         *   且分配失败直接拒绝进入（宁可进不去，也不进去后卡 0/3）。 */
        if (!enroll_feats_alloc()) {
            ESP_LOGE(TAG, "ENROLL refused: enroll feat buf alloc failed (SPIRAM)");
            lv_label_set_text_static(s_status, "No mem for enroll (PSRAM)");
            return;
        }
        if (!face_service_enabled()) {
            s_det_borrowed = true;
            ESP_LOGI(TAG, "det borrowed for ENROLL (returned on exit)");
        }
        face_service_set_enabled(true);   /* 模型没就绪会顺带触发构建 */
        s_frame_skip = 99;                /* 让下一帧立刻检一次，别干等 3 帧 */
        enroll_msg_set("Type a name, then START");
        if (s_enroll_ta) lv_textarea_set_text(s_enroll_ta, "");
        enroll_overlay_show(true);
        ESP_LOGI(TAG, "mode -> ENROLL (name input)");
    } else {
        ESP_LOGI(TAG, "mode -> USER (enroll cancelled by user, %d/%d samples discarded)",
                 s_enroll_have, ENROLL_SHOTS);
        enroll_finish_to_user(NULL);
    }

    /* 切模式就把旧结果清掉：两个模式的语义不同，留着一张上一个模式的框/标签
     * 会让下一拍的引导自相矛盾（"no face in view" 配着一个绿框）。 */
    s_box_valid = false;
    if (s_face_bbox)  lv_obj_set_hidden(s_face_bbox, true);
    if (s_gest_label) lv_obj_set_hidden(s_gest_label, true);
}

static void btn_cb(lv_event_t *e)
{
    (void)e;
    if (camera_service_is_running()) {
        /* ★ P0-3：stop() 现在会等 cam_task 真的退出；超时（多半是它正在做模型加载）
         * 会返回 ESP_ERR_TIMEOUT。此时**不能**把按钮显示成"已停" —— 否则下次按
         * START 会被 camera_service_start() 拒绝，用户只看到"没反应"。 */
        esp_err_t err = camera_service_stop();
        if (err != ESP_OK) {
            lv_label_set_text_static(s_status, "Stop busy, retry");
            return;
        }
        /* ★★ S2-0 顺带修的悬垂指针隐患：**必须**在 stop() **返回之后**再清。
         *   stop() 成功返回 = cam_task 已退出且已经把 mmap 缓冲 munmap 掉，
         *   此刻不可能再有 on_frame 写回来 ⇒ 清掉是安全的。
         *   ★ 反过来，在调 stop() **之前**清是**没用**的：那之后 on_frame 还能再写一次，
         *     而写进来的正是马上要被 munmap 的那块缓冲 ⇒ 还是会读到野指针。 */
        s_last_frame = NULL;
        s_frame_new = false;
        lv_label_set_text_static(s_btn_label, "START");
        lv_label_set_text_static(s_status, "Stopped");
    } else {
        esp_err_t err = camera_service_start(on_frame, NULL);
        if (err == ESP_OK) {
            lv_label_set_text_static(s_btn_label, "STOP");
            lv_label_set_text_static(s_status, "Preview running");
        } else {
            lv_label_set_text_static(s_status, "Camera open failed");
        }
    }
}

/* ---------------------------------------------------------------------------
 *  识别服务的一次性初始化（**惰性**，只在第一次真正需要时调用）
 * ------------------------------------------------------------------------- */
static void recog_ensure_init(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    face_recog_init();
    punch_service_init();
    ESP_LOGI(TAG, "S8-2 services initialised");
}

void tab_camera_build(lv_obj_t *screen, lv_group_t *group)
{
    s_screen = screen;
    /* ★★★ 诊断计数器按"本页"归零（不是开机）—— 这样串口里 `no-box diag` 的
     *   四个数字就是"这次进来以后"的，判读时不用去减上一次会话的基线。
     *   ★ 只清统计值，不动 `s_noframe_warned` 之外的状态：后者必须清，
     *     否则第二次进页面时"没帧"这条错误日志不会重报（只报一次）。 */
    s_frames_in = 0; s_frames_dropped = 0; s_detect_calls = 0; s_detect_hits = 0;
    s_noframe_warned = false;
    s_noframe_ticks  = 0;
    /* ★ 手动打卡状态每次建页都归零（理由见 tab_camera_destroy() 同款注释）。 */
    s_punch_armed = false;
    s_punch_deadline_us = 0;
    s_punch_window_ms = 0;
    s_dir_dumped = false;      /* ★ 新的一次页面会话：允许再打印一次目录体检 */
    s_det_borrowed = false;    /* ★ 解耦：新会话从"没借过"开始 */
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_style_pad_top(screen, 16, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text_static(title, LV_SYMBOL_VIDEO "  CAMERA");
    lv_obj_set_style_text_color(title, COL_CYAN, 0);
    lv_obj_set_style_text_font(title, UI_FONT, 0);

    /* Canvas 240x240 RGB565 —— 与帧缓冲同深度，无需逐像素转换 */
    if (!s_canvas_buf) {
        s_canvas_buf = heap_caps_aligned_alloc(64, CAM_W * CAM_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_canvas_buf) {
            MEM_ERR_DIAG(TAG, "canvas buf alloc FAILED (SPIRAM|CACHE_ALIGNED)", CAM_W * CAM_H * 2);
            return;
        }
        memset(s_canvas_buf, 0, CAM_W * CAM_H * 2);
        MEM_STAGE(TAG, "canvas buf allocated");
    }
    /* ★ 2026-09-28（§73）：录入特征暂存的分配已搬到 mode_btn_cb（进 ENROLL 时），
     *   与 enroll_finish_to_user() 的释放端成对 —— 建页期不再预占这 8 KB PSRAM。
     *   把缓冲纳入某个会话的生命周期，分配和释放必须成对搬家（§73 教训）。 */
    recog_ensure_init();

    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CAM_W, CAM_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_border_color(s_canvas, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_canvas, 1, 0);
    lv_obj_set_style_radius(s_canvas, 8, 0);
    lv_obj_add_event_cb(s_canvas, toggle_fullscreen, LV_EVENT_CLICKED, NULL);
    lv_obj_set_clickable(s_canvas, true);

    /* ★ §73.12：原 HSV 黄框（s_bbox）已随功能删除；下面只建人脸/手势的绿框。 */

    /* Face bbox (green) */
    s_face_bbox = lv_obj_create(s_canvas);
    lv_obj_set_size(s_face_bbox, 10, 10);
    lv_obj_set_style_border_color(s_face_bbox, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(s_face_bbox, 2, 0);
    lv_obj_set_style_border_opa(s_face_bbox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_face_bbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_face_bbox, 0, 0);
    lv_obj_set_hidden(s_face_bbox, true);

    /* ★ §60 手势名标签（HAND 档；画布顶部居中小字，深底亮字） */
    s_gest_label = lv_label_create(s_canvas);
    lv_label_set_text_static(s_gest_label, "");
    lv_obj_align(s_gest_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(s_gest_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_gest_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_gest_label, 2, 0);
    lv_obj_set_style_text_color(s_gest_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(s_gest_label, UI_FONT, 0);
    lv_obj_set_hidden(s_gest_label, true);

    /* ★ §62：原"POSE 17 关节点"的 17 个 5x5 青色小方块创建循环已随该档删除。 */

    /* ★ S8-2：识别/打卡结果标签（画布底部居中，深底亮字）。
     *   ★ 必须用**拷贝**语义写文本（tip_set 的内容来自可变缓冲 s_tip）。 */
    s_recog_label = lv_label_create(s_canvas);
    lv_label_set_text_static(s_recog_label, "");
    lv_obj_align(s_recog_label, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(s_recog_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_recog_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_recog_label, 3, 0);
    lv_obj_set_style_text_color(s_recog_label, lv_color_hex(0x22D3EE), 0);
    lv_obj_set_style_text_font(s_recog_label, cjk_font(), 0);
    lv_obj_set_hidden(s_recog_label, true);

    /* ★ S8-3：水印绘制用的**隐藏**离屏 canvas。
     *   · 挂在本页根对象下 ⇒ 天然随页面销毁，且 lv_obj_get_display() 有效
     *     （lv_canvas_finish_layer 需要 display，画布没有 display 会断言）；
     *   · HIDDEN ⇒ LVGL 不会把它渲染到屏幕上；
     *   · IGNORE_LAYOUT ⇒ flex 布局不把它算进去（否则会顶掉下面的按钮）。
     *   ★ 它只是"绘制目标"的载体，真正的内容落在我们传进去的 RGB888 缓冲里。 */
    s_wm_canvas = lv_canvas_create(screen);
    /* ★ LVGL 9.6 起 `lv_obj_add_flag()` 已弃用（-Wdeprecated-declarations），
     *   用逐 flag 的专用 setter。IGNORE_LAYOUT 没有专用名，走通用的 set_flag()。 */
    lv_obj_set_hidden(s_wm_canvas, true);
    lv_obj_set_ignore_layout(s_wm_canvas, true);
    lv_obj_set_pos(s_wm_canvas, -1000, -1000);

    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Stopped");
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    lv_obj_t *btn_row = lv_obj_create(screen);
    /* ★ S8-1：现在是 5 个按钮（START / HSV / FACE / MODEL / MODE）
     *   ⇒ 140x5 + 12x4 = 748，行宽必须 ≥748。
     *   历史：原来 3 个按钮时 480，S1 加 MODEL 改 600，本次加 MODE 改 760。
     *   ★ 这个数字是**算出来的**，不是拍脑袋：漏改就会出现"最后一个按钮被挤出
     *     可视区"——按钮还在、回调还挂，但用户看不见也点不到（排查起来很费劲）。
     *     760 仍在 tabview 页内容区内（800 − 2x12 pad = 776），下面居中。 */
    lv_obj_set_size(btn_row, 760, 60);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_scrollable(btn_row, false);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 12, 0);

    s_btn = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn, 140, 60);
    lv_obj_set_style_radius(s_btn, 30, 0);
    lv_obj_set_style_bg_color(s_btn, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(s_btn);
    /* ★ 初值也要看真状态：上一轮 stop() 超时会留下仍在跑的 cam_task，
     *   那时显示 "START" 就是骗人（refresh() 每 50ms 也会纠回来，这里只是别先闪一下）。 */
    lv_label_set_text_static(s_btn_label, camera_service_is_running() ? "STOP" : "START");
    lv_obj_set_style_text_color(s_btn_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_label, UI_FONT, 0);
    lv_obj_center(s_btn_label);

    /* ★ §73.12：HSV 按钮已删除（btn_row 现在剩 4 个按钮：START/HSV 移除后
     *   flex 居中会自动收拢，无需改行宽）。 */

    s_btn_face = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn_face, 140, 60);
    lv_obj_set_style_radius(s_btn_face, 30, 0);
    lv_obj_set_style_bg_color(s_btn_face, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn_face, face_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_face_label = lv_label_create(s_btn_face);
    lv_label_set_text_static(s_btn_face_label, "FACE");
    lv_obj_set_style_text_color(s_btn_face_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_face_label, UI_FONT, 0);
    lv_obj_center(s_btn_face_label);

    /* ★ S1 新增：档位切换按钮。显示当前（或正在切往的）档位短文案。
     *   ★ 初值取真状态而非写死 "224" —— 若改成过常驻的档位，写死就会先闪一下错的。
     *   ★ label 文本来自 face_service 内部的**静态**字符串表 ⇒ set_text_static 安全
     *     （不要改成 set_text_fmt，那会 malloc）。 */
    s_btn_model = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn_model, 140, 60);
    lv_obj_set_style_radius(s_btn_model, 30, 0);
    lv_obj_set_style_bg_color(s_btn_model, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn_model, model_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_model_label = lv_label_create(s_btn_model);
    lv_label_set_text_static(s_btn_model_label,
                             face_service_model_label(face_service_current_model()));
    lv_obj_set_style_text_color(s_btn_model_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_model_label, UI_FONT, 0);
    lv_obj_center(s_btn_model_label);

    /* ★ S8-1 新增：运行模式切换按钮（USER ↔ ENROLL，需求 R8）。
     *   ★ 初值取真状态（s_mode 初值恒为 USER）而不是写死 "ENROLL"；
     *     tab_camera_destroy() 会把 s_mode 复位，所以重进页面不会残留上个模式。 */
    s_btn_mode = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn_mode, 140, 60);
    lv_obj_set_style_radius(s_btn_mode, 30, 0);
    lv_obj_set_style_bg_color(s_btn_mode, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn_mode, mode_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_mode_label = lv_label_create(s_btn_mode);
    lv_label_set_text_static(s_btn_mode_label, (s_mode == CAM_MODE_ENROLL) ? "ENROLL" : "USER");
    lv_obj_set_style_text_color(s_btn_mode_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_mode_label, UI_FONT, 0);
    lv_obj_center(s_btn_mode_label);

    /* =======================================================================
     *  ★★★ 2026-09-28：右侧**圆形打卡按钮**（USER 态的主操作）
     *
     *  · 位置：`LV_ALIGN_RIGHT_MID` + `ignore_layout` ⇒ 悬浮在取景画面右侧，
     *    **不参与**那条纵向 flex —— 否则会被塞到按钮行下面，还要重算 760 的行宽
     *    （那行宽度是算出来的，动它会连锁出"最后一个按钮被挤出可视区"）。
     *  · 圆形用 `LV_RADIUS_CIRCLE`（LVGL 取边长一半）：写死半径的话，以后改尺寸
     *    就变成圆角矩形，"圆圆的按钮"这条需求会静默失效。
     *  · 96x96 明显大于底部 140x60 的功能按钮 ⇒ 视觉层级上表明"这是主操作"。
     *  · 水平偏移 -34：画布 240 宽居中 ⇒ 右边缘在 x=520；按钮左边缘 = 800-34-96 = 670，
     *    中间留 150 px 空隙，不会盖住取景画面。
     *  ★ 只在 USER 态可见（ENROLL 态由 refresh() 隐藏）：ENROLL 有自己的"START"流程，
     *    两颗语义不同的启动按钮并存只会让人按错。
     * ======================================================================= */
    s_btn_punch = lv_btn_create(screen);
    lv_obj_set_size(s_btn_punch, PUNCH_BTN_SIZE, PUNCH_BTN_SIZE);
    lv_obj_set_style_radius(s_btn_punch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn_punch, COL_GREEN, 0);
    lv_obj_set_style_shadow_width(s_btn_punch, 0, 0);
    lv_obj_set_ignore_layout(s_btn_punch, true);
    /* ★ §73.11：y 偏移 CAM_VIEW_Y_OFF —— 画布在 flex 列里的真实中心是
     *   y≈165（内容区坐标），不是内容区中心 216；旧版对齐内容区中心
     *   ⇒ 按钮比取景画面低 51 px（用户实测"往上移一点"）。
     *   全屏态 toggle_fullscreen() 会把它改回 0（那时画布确实在正中）。 */
    lv_obj_align(s_btn_punch, LV_ALIGN_RIGHT_MID, -34, CAM_VIEW_Y_OFF);
    lv_obj_add_event_cb(s_btn_punch, punch_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_punch_label = lv_label_create(s_btn_punch);
    /* ★ 定值标签一律用 UI_FONT：bin 字体是 28 px 且字表只为"人名"准备，
     *   拿去套固定的英文短标签会被撑变形（见 ui_binfont.h 的使用禁区）。 */
    lv_label_set_text_static(s_btn_punch_label, "PUNCH");
    lv_obj_set_style_text_color(s_btn_punch_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_punch_label, UI_FONT, 0);
    lv_obj_center(s_btn_punch_label);

    /* =======================================================================
     *  ★★★ 2026-09-28 新增：画布**左侧**的两行时间显示（打卡系统的"现在几点"）
     *
     *  · 与右侧的打卡按钮**镜像对称**（LEFT_MID +34 ↔ RIGHT_MID -34），
     *    画布 240 宽居中 ⇒ 左边缘 x=280，标签从 x=34 起，互不重叠；
     *  · 第一行 `年.月.日`，第二行 `时:分:秒`（具体到秒，用户要求）；
     *  · **只在已校时（net_time_t.valid）时显示** —— 断网/没拿到时间就整块隐藏，
     *    绝不显示 `---- -- --` 这种占位横线（用户明确说"不显示时间"）。
     *    这与打卡水印的时间判据（punch_service 用同一个 net_time_service，
     *    无效时文件名退化为序号而不是 1970）是**同一套真相源**。
     *  · ignore_layout：不参与纵向 flex（同打卡按钮的理由）。
     * ======================================================================= */
    s_time_label = lv_label_create(screen);
    lv_label_set_text_static(s_time_label, "");
    lv_obj_set_ignore_layout(s_time_label, true);
    /* ★ §73.11：与打卡按钮同一条垂直基准（画布中心，CAM_VIEW_Y_OFF），
     *   两行文本**互相中心对齐**（LV_TEXT_ALIGN_CENTER）——
     *   "2026.09.29" 比 "09:45:20" 宽，左对齐会让两行看起来错位。 */
    lv_obj_align(s_time_label, LV_ALIGN_LEFT_MID, 34, CAM_VIEW_Y_OFF);
    lv_obj_set_style_text_color(s_time_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, UI_FONT_BIG, 0);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_time_label, true);      /* 初值隐藏，refresh() 按校时状态放开 */

    /* =======================================================================
     *  ★★ S8-1 补完：录入的姓名输入覆盖层（默认隐藏）
     *
     *  为什么是"页内覆盖层"而不是 page_manager 子页：
     *   · 录制要点：**相机必须继续跑**（覆盖层只挡视觉，不挡取流）——
     *     子页会走 PageManager，语义上把 CAM 页压栈，容易让人以为相机也停了；
     *   · 交互与 page_wifi 的"连接子视图"完全同款（textarea + lv_keyboard + 两个按钮），
     *     用户不用学第二套。
     *  ★ 尺寸按 tabview 内容区（800 x 432，顶部 tab bar 48）算：
     *     24(msg) + 40(ta) + 200(kb) + 40(btn) + 24(pad) + 16(pad_all) = 344 < 360 ✅
     * ======================================================================= */
    s_enroll_ov = lv_obj_create(screen);
    lv_obj_set_size(s_enroll_ov, BSP_LCD_H_RES - 60, 360);
    lv_obj_set_ignore_layout(s_enroll_ov, true);
    lv_obj_align(s_enroll_ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_enroll_ov, COL_CARD, 0);
    lv_obj_set_style_border_color(s_enroll_ov, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_enroll_ov, 1, 0);
    lv_obj_set_style_radius(s_enroll_ov, 12, 0);
    lv_obj_set_style_pad_all(s_enroll_ov, 8, 0);
    lv_obj_set_style_pad_row(s_enroll_ov, 8, 0);
    lv_obj_set_scrollable(s_enroll_ov, false);
    lv_obj_set_flex_flow(s_enroll_ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_enroll_ov, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_hidden(s_enroll_ov, true);

    s_enroll_msg = lv_label_create(s_enroll_ov);
    lv_label_set_text_static(s_enroll_msg, "Type a name, then START");
    lv_obj_set_style_text_color(s_enroll_msg, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_enroll_msg, UI_FONT, 0);

    s_enroll_ta = lv_textarea_create(s_enroll_ov);
    lv_obj_set_size(s_enroll_ta, BSP_LCD_H_RES - 100, 40);
    lv_textarea_set_one_line(s_enroll_ta, true);
    /* ★ 12 字节上限 ≈ 4 个汉字 —— 与 face_db 的 FACE_NAME_MAX(24 B) 是两回事：
     *   这里限制的是"输入"，超长会在 enroll_start_cb 里按 UTF-8 边界再截一次。
     *   名字太长在打卡目录名里会被挤掉，所以别放太松。 */
    lv_textarea_set_max_length(s_enroll_ta, 12);
    lv_textarea_set_placeholder_text(s_enroll_ta, "Name");
    /* ★ 名字本身要用 CJK 字体显示，否则用户输入的中文在输入框里就是方块。 */
    lv_obj_set_style_text_font(s_enroll_ta, cjk_font(), 0);
    lv_obj_add_event_cb(s_enroll_ta, enroll_ta_ready_cb, LV_EVENT_READY, NULL);

    s_enroll_kb = lv_keyboard_create(s_enroll_ov);
    lv_obj_set_size(s_enroll_kb, BSP_LCD_H_RES - 100, 200);
    lv_keyboard_set_textarea(s_enroll_kb, s_enroll_ta);

    {
        lv_obj_t *row = lv_obj_create(s_enroll_ov);
        lv_obj_set_size(row, BSP_LCD_H_RES - 100, 44);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 16, 0);

        s_btn_enroll_go = lv_btn_create(row);
        lv_obj_set_size(s_btn_enroll_go, 200, 44);
        lv_obj_set_style_radius(s_btn_enroll_go, 10, 0);
        lv_obj_set_style_bg_color(s_btn_enroll_go, COL_GREEN, 0);
        lv_obj_add_event_cb(s_btn_enroll_go, enroll_start_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *gl = lv_label_create(s_btn_enroll_go);
        lv_label_set_text_static(gl, LV_SYMBOL_OK "  START");
        lv_obj_set_style_text_color(gl, COL_DARK, 0);
        lv_obj_set_style_text_font(gl, UI_FONT, 0);
        lv_obj_center(gl);

        lv_obj_t *cb = lv_btn_create(row);
        lv_obj_set_size(cb, 200, 44);
        lv_obj_set_style_radius(cb, 10, 0);
        lv_obj_set_style_bg_color(cb, COL_RED, 0);
        lv_obj_add_event_cb(cb, enroll_cancel_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cl = lv_label_create(cb);
        lv_label_set_text_static(cl, LV_SYMBOL_CLOSE "  CANCEL");
        lv_obj_set_style_text_color(cl, COL_DARK, 0);
        lv_obj_set_style_text_font(cl, UI_FONT, 0);
        lv_obj_center(cl);
    }

    if (group) lv_group_add_obj(group, s_btn);
    /* ★ 手动打卡按钮也进编码器组：触摸之外多一条可用路径（不改变触摸行为）。 */
    if (group) lv_group_add_obj(group, s_btn_punch);
    s_timer = lv_timer_create(refresh, 50, NULL);
}

void tab_camera_destroy(void)
{
    /* ★★ S2-0：只停**自己拥有的**那条流。
     *   双 tab 架构下相机可能正被 VIDEO tab 独占（录像），
     *   这里若无脑 stop() 就等于"退出 CAM 页把别人的录像掐了"。 */
    if (camera_service_owner() == CAM_OWNER_PREVIEW && camera_service_is_running()) {
        camera_service_stop();
    }
    /* ★★★ 2026-09-28（解耦）：离开页面 = 识别会话必须结束。
     *   识别引擎现在是**按需**加载（只有打卡/录入会话需要它），离开 CAM 页
     *   没有任何理由让它活着 —— 把 MSR+MNP+MFN+帧缓冲（峰值 ≈ 2.11 MB）还回去。
     *   stop() 在"有作业在跑"时会拒绝 —— 那时**保留** s_recog_want=true，
     *   下次进页面 ③ 的对账会再试一次（旧语义，防 UAF 的写法不变）。 */
    if (face_recog_stop() == ESP_OK) {
        s_recog_want = false;
        ESP_LOGI(TAG, "page exit: recog engine released");
    } else {
        ESP_LOGW(TAG, "page exit: recog stop deferred (job/load running) - retry next visit");
    }
    /* ★ 解耦：若检测是 ENROLL 借来的，页面销毁同样要归还（否则借了不还）。 */
    if (s_det_borrowed) {
        s_det_borrowed = false;
        face_service_set_enabled(false);
        face_service_unload();
        ESP_LOGI(TAG, "page exit: det returned + unload requested");
    }
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    /* ★★ §73.9（用户验收标准："cam 里面的 PSRAM 都是按需分配、即用即释放"）：
     *   画布缓冲 115,200 B 旧版建页分配后**永不释放** —— 一次 CAM 会话就永久
     *   占掉一块，还把它下游的空闲区切成两段（2026-09-28 深夜日志：viewfinder
     *   重 acquire 差 8 KB 拿不到 1.84 MB 连续块，这类"差一点"的元凶就是各种
     *   永久小占住）。现在随页面销毁归还，下次进页重新分配（几十 µs）。
     *   ★ 安全性：face_recog_submit 是**同步拷贝**语义（引擎自持 RGB888 副本），
     *     释放画布不影响任何在途作业；canvas 对象已随页面树删除。 */
    if (s_canvas_buf) {
        heap_caps_free(s_canvas_buf);
        s_canvas_buf = NULL;
        MEM_STAGE(TAG, "canvas buf freed (page exit)");
    }
    s_canvas = NULL; s_btn = NULL; s_btn_label = NULL; s_status = NULL;
    s_btn_face = NULL; s_btn_face_label = NULL; s_face_bbox = NULL;
    /* ★ S1：MODEL 按钮也要清；顺带补上原本漏掉的 FACE 三个指针
     *   （旧版这里没清 s_btn_face / s_btn_face_label / s_face_bbox，
     *    页面销毁后它们仍指向已释放的 LVGL 对象 —— refresh() 里一旦用到就是 UAF）。 */
    s_btn_face = NULL; s_btn_face_label = NULL; s_face_bbox = NULL;
    s_btn_model = NULL; s_btn_model_label = NULL;
    s_btn_mode = NULL; s_btn_mode_label = NULL;      /* ★ S8-1 */
    s_btn_punch = NULL; s_btn_punch_label = NULL;    /* ★ 手动打卡按钮（页面子对象） */
    s_gest_label = NULL;
    s_recog_label = NULL;                            /* ★ S8-2 */
    s_time_label = NULL;                             /* ★ 画布左侧时间显示 */
    s_wm_canvas = NULL;                              /* ★ S8-3（随页面树一起释放） */
    /* ★ S8-1 补完：覆盖层的 6 个指针也要清（它们是页面子对象，已随 lv_obj_del 释放）。 */
    s_enroll_ov = NULL; s_enroll_msg = NULL; s_enroll_ta = NULL;
    s_enroll_kb = NULL; s_btn_enroll_go = NULL;
    s_last_frame = NULL; s_frame_new = false;
    s_fullscreen = false; s_face_on = false;   /* ★ §73.12：s_hsv_on 已随 HSV 删除 */
    s_media_taken = false;      /* ★ S2-0：下次建页时必须从"未占用"重新开始 */
    /* ★ S8-1：模式与引导缓存一并复位。不复位的话，下次进页面会带着上一次的
     *   ENROLL 态（按钮初值虽取真状态、但 refresh 的"切换那一拍"判断会错位）。 */
    s_mode = CAM_MODE_USER; s_mode_shown = CAM_MODE_USER; s_box_valid = false;
    /* ★ S8-2/S8-3：一次性提示与录入进度也复位 */
    s_enroll_state = ENROLL_OFF;
    s_enroll_have  = 0;
    s_enroll_name[0] = '\0';
    /* ★ §73：录入中途销毁页面也要归还特征暂存 —— 分配端在 mode_btn_cb（会话级），
     *   这里是唯一能兜底的"会话强制结束"点；不 free 的话这 8 KB 就一直挂在进程上。 */
    enroll_feats_free();
    s_tip[0] = '\0';
    s_tip_until_us = 0;
    s_tip_was_active = false;
    s_next_recog_us = 0;
    /* ★ 手动打卡状态必须复位：不复位的话，下次进页面按钮会一开始就显示 SCAN
     *   （armed 残留），而那个 armed 对应的 deadline 是上一次会话的时间戳 ——
     *   表现为"一进来就在扫，且永远不会结束"。 */
    s_punch_armed       = false;
    s_punch_deadline_us = 0;
    s_punch_window_ms   = 0;
    s_dir_dumped        = false;
    s_det_borrowed      = false;   /* ★ 解耦：借用标记一并复位（页面已销毁） */
    s_time_txt[0]       = '\0';    /* ★ 时间显示回到"未显示"，避免残留旧文本 */
    /* ★ s_recog_want **不清**：识别引擎是进程级单例，它的开关状态跨页面有效
     *   （清掉会导致"回到本页时以为引擎是关的，于是重复 enable"）。 */
}
