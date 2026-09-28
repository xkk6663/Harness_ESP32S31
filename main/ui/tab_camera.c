/* tab_camera.c - Camera preview tab */
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "tab_camera.h"
#include "ui_theme.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/color_hsv.h"
#include "middleware/camera/face_service.h"
#include "middleware/mem_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

#define TAG "TAB_CAM"
#define CAM_W 240
#define CAM_H 240

static lv_obj_t *s_canvas;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_btn_hsv;
static lv_obj_t *s_btn_hsv_label;
static lv_obj_t *s_btn_face;
static lv_obj_t *s_btn_face_label;
static lv_obj_t *s_btn_model;        /* ★ S1 新增：档位切换（页内按钮循环切档） */
static lv_obj_t *s_btn_model_label;
static lv_obj_t *s_bbox;
static lv_obj_t *s_face_bbox;        /* 检测结果框（FACE 脸 / HAND 手）复用 */
static lv_obj_t *s_gest_label;       /* ★ §60：手势名标签（HAND 档显示） */
/* ★ §62：原 POSE 档的 17 个关节点对象（s_kp[17]）已随该档一并删除。 */
static lv_obj_t *s_status;
static lv_obj_t *s_screen;
static lv_timer_t *s_timer;
static uint16_t *s_canvas_buf;
static volatile const uint8_t *s_last_frame;
static volatile bool s_frame_new;
static bool s_fullscreen;
static bool s_hsv_on;
static bool s_face_on;
static int s_frame_skip;
static bool s_media_taken;      /* ★ S2-0：相机是否被 VIDEO tab 独占（避免每帧都写 LVGL 状态） */

/* ★★ S2-0：回调签名加了 size（JPEG 是变长帧），并换成**运行时** w/h。
 *   ★ 为什么必须挡一道尺寸：VIDEO tab 会把流切成 1280x720 JPEG，
 *     而本页 canvas 缓冲只有 240x240x2 字节。若照单全收就是**堆溢出**。
 *   ★ 也**不要**用 size 去推算 w*h：RGB565 定长可以，JPEG 不行（变长）。
 *     这里直接用 w/h 判断，size 只做"至少够一帧"的下限校验。 */
static void on_frame(const uint8_t *data, uint32_t size, uint32_t w, uint32_t h, void *user)
{
    (void)user;
    if (w != CAM_W || h != CAM_H || size < (uint32_t)(CAM_W * CAM_H * 2)) {
        return;                  /* 不是本页要的档位：直接丢，不要记指针 */
    }
    s_last_frame = data;
    s_frame_new = true;
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
             * （比手工调 bg_color 更稳，也不会和 FACE 按钮的状态色逻辑打架）。 */
            lv_obj_t *btns[3] = { s_btn, s_btn_face, s_btn_model };
            for (int i = 0; i < 3; i++) {
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
        if (s_hsv_on && s_bbox) {
            color_bbox_t b;
            /* detect on the corrected canvas buffer (already flipped + byteswapped) */
            color_hsv_detect_red(s_canvas_buf, CAM_W, CAM_H, &b);
            if (b.w > 0 && b.h > 0) {
                lv_obj_set_pos(s_bbox, b.x, b.y);
                lv_obj_set_size(s_bbox, b.w, b.h);
                lv_obj_set_hidden(s_bbox, false);
            } else {
                lv_obj_set_hidden(s_bbox, true);
            }
        }
        if (s_face_on && s_face_bbox) {
            s_frame_skip++;
            if (s_frame_skip >= 3) {  /* run detection every 3 frames */
                s_frame_skip = 0;
                vision_result_t vr[VISION_MAX_RESULTS];
                int n = face_service_detect(s_canvas_buf, CAM_W, CAM_H, vr, VISION_MAX_RESULTS);
                /* 画"最大"的那个结果（与旧人脸逻辑同款选择） */
                int idx = -1;
                int max_area = 0;
                for (int i = 0; i < n; i++) {
                    int a = vr[i].w * vr[i].h;
                    if (a > max_area) { max_area = a; idx = i; }
                }
                if (idx >= 0) {
                    lv_obj_set_pos(s_face_bbox, vr[idx].x, vr[idx].y);
                    lv_obj_set_size(s_face_bbox, vr[idx].w, vr[idx].h);
                    lv_obj_set_hidden(s_face_bbox, false);
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
                }
            }
        }
        lv_unlock();
    }
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
}

static void hsv_btn_cb(lv_event_t *e)
{
    (void)e;
    s_hsv_on = !s_hsv_on;
    lv_label_set_text_static(s_btn_hsv_label, s_hsv_on ? "HSV ON" : "HSV");
    lv_obj_set_style_bg_color(s_btn_hsv, s_hsv_on ? COL_RED : COL_CYAN, 0);
    if (!s_hsv_on && s_bbox) lv_obj_set_hidden(s_bbox, true);
}

static void face_btn_cb(lv_event_t *e)
{
    (void)e;
    /* ★ 只发动作，**不写文案/颜色** —— 全部交给 refresh() 这个单一真相源。
     *   否则 cb 里写一套、refresh 里写一套，两个状态机早晚不一致（旧代码就是这样）。 */
    if (face_service_state() == FACE_LOADING) return;   /* 构建/切换中，忽略点击 */

    if (face_service_enabled()) {
        face_service_stop();        /* ★ 只关检测：模型常驻、不卸载（D6） */
        if (s_face_bbox) lv_obj_set_hidden(s_face_bbox, true);
        if (s_gest_label) lv_obj_set_hidden(s_gest_label, true);
    } else {
        /* ★★ 只有"模型还没就绪"才需要相机在跑 —— 因为构建只能由 cam_task 的
         *    face_service_poll() 驱动。相机没跑就没人消费这个请求，状态会一直卡在
         *    FACE_LOADING，3 s 后被看门狗判 FAIL（有提示，但不如当场说清楚）。
         *    ★ 模型已常驻时不需要构建，只翻一个 bool ⇒ 此时没相机也能开。 */
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

void tab_camera_build(lv_obj_t *screen, lv_group_t *group)
{
    s_screen = screen;
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
    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CAM_W, CAM_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_border_color(s_canvas, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_canvas, 1, 0);
    lv_obj_set_style_radius(s_canvas, 8, 0);
    lv_obj_add_event_cb(s_canvas, toggle_fullscreen, LV_EVENT_CLICKED, NULL);
    lv_obj_set_clickable(s_canvas, true);

    /* Bounding box overlay (child of canvas, hidden by default) */
    s_bbox = lv_obj_create(s_canvas);
    lv_obj_set_size(s_bbox, 10, 10);
    lv_obj_set_style_border_color(s_bbox, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_pad_all(s_bbox, 0, 0);
    lv_obj_set_style_border_post(s_bbox, true, 0);
    lv_obj_set_style_border_width(s_bbox, 2, 0);
    lv_obj_set_style_border_opa(s_bbox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_bbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_bbox, 0, 0);
    lv_obj_set_hidden(s_bbox, true);

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

    s_status = lv_label_create(screen);
    lv_label_set_text_static(s_status, "Stopped");
    lv_obj_set_style_text_color(s_status, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT, 0);

    lv_obj_t *btn_row = lv_obj_create(screen);
    /* ★ S1：4 个按钮（START/HSV/FACE/MODEL）⇒ 140x4 + 12x3 = 596，行宽必须 ≥596。
     *   原来是 480（3 个按钮），加第 4 个后不改这里就会把 MODEL 挤出可视区。
     *   600 仍在 tabview 页内容区内（800 − 2x12 pad = 776），下面会居中。 */
    lv_obj_set_size(btn_row, 600, 60);
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

    s_btn_hsv = lv_btn_create(btn_row);
    lv_obj_set_size(s_btn_hsv, 140, 60);
    lv_obj_set_style_radius(s_btn_hsv, 30, 0);
    lv_obj_set_style_bg_color(s_btn_hsv, COL_CYAN, 0);
    lv_obj_add_event_cb(s_btn_hsv, hsv_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_hsv_label = lv_label_create(s_btn_hsv);
    lv_label_set_text_static(s_btn_hsv_label, "HSV");
    lv_obj_set_style_text_color(s_btn_hsv_label, COL_DARK, 0);
    lv_obj_set_style_text_font(s_btn_hsv_label, UI_FONT, 0);
    lv_obj_center(s_btn_hsv_label);

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

    if (group) lv_group_add_obj(group, s_btn);
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
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_canvas = NULL; s_btn = NULL; s_btn_label = NULL; s_status = NULL;
    s_btn_hsv = NULL; s_btn_hsv_label = NULL; s_bbox = NULL;
    /* ★ S1：MODEL 按钮也要清；顺带补上原本漏掉的 FACE 三个指针
     *   （旧版这里没清 s_btn_face / s_btn_face_label / s_face_bbox，
     *    页面销毁后它们仍指向已释放的 LVGL 对象 —— refresh() 里一旦用到就是 UAF）。 */
    s_btn_face = NULL; s_btn_face_label = NULL; s_face_bbox = NULL;
    s_btn_model = NULL; s_btn_model_label = NULL;
    s_gest_label = NULL;
    s_last_frame = NULL; s_frame_new = false;
    s_hsv_on = false; s_fullscreen = false; s_face_on = false;
    s_media_taken = false;      /* ★ S2-0：下次建页时必须从"未占用"重新开始 */
}
