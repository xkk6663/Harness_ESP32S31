/* face_service.cpp - esp-dl 视觉模型服务（C ABI），§60 异族模型重构
 *
 * 两档异族（见头文件）；
 * ★ §62（2026-09-28 用户拍板）已**阉割 POSE 档**：3.0MB 的 yolo11n-pose 需要一块
 *   3.04MB **连续** PSRAM，本机碎片环境下装不下（详见 §61/§62）。
 *   FACE  HumanFaceDetect      —— 单模型，输出人脸框；
 *   HAND  HandDetect + HandGestureCls —— 两段式（检手 → 裁剪 128x128 → 10+1 类手势）。
 *
 * ★ 两类 wrapper 的公共基座：
 *   HumanFaceDetect / HandDetect 都是 dl::detect::DetectWrapper
 *   ⇒ 一个 s_detector 指针覆盖两档；手势 cls 是 HAND 独有的第二模型（s_gcls）。
 * ★ 构建仍由 cam_task 的 face_service_poll() 驱动（LVGL 线程绝不构建）；
 *   先卸后建（D8）、失败回退上一档（D9）、D3 内存埋点全部保留。
 */
#include "face_service.h"
#include "human_face_detect.hpp"
#include "hand_detect.hpp"
#include "hand_gesture_recognition.hpp"
#include "dl_image_define.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "middleware/mem_diag.h"
#include <string.h>
#include <stdio.h>
#include <list>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>

static const char *TAG = "VISION";

static dl::detect::DetectWrapper *s_detector = nullptr;  /* FACE/HAND(检手) 共用 */
static HandGestureCls            *s_gcls     = nullptr;  /* HAND 独有的第二段 */
static SemaphoreHandle_t s_lock;
static uint8_t *s_scratch;
static face_state_t s_state = FACE_IDLE;
static bool s_load_request = false;
static int64_t s_load_req_us = 0;
/* ★ D3 埋点：load/unload 配对编号。日志里 #N 对齐即可看出
 * "反复开关后 INT largest 是否单调下滑"（＝反复 new/delete 的碎片证据）。 */
static uint32_t s_load_seq = 0;
static uint32_t s_unload_seq = 0;

/* ================= ★ §60 异族档位表 =================
 * family 决定 do_build 走哪条构建路径、detect 走哪条后处理路径。
 * files[] 供"卸之前预检"（两段式要 2 个文件都在盘上才允许切）。
 * ★ 本表顺序即 UI 循环切换顺序；开机默认 = 0（FACE）。 */
typedef enum {
    FAM_FACE = 0,   /* HumanFaceDetect(ESPDET_PICO_224_224_FACE) */
    FAM_HAND = 1,   /* HandDetect + HandGestureCls 两段式 */
    /* ★ §62：FAM_POSE 已阉割（原 POSE = COCOPose(YOLO11N_POSE_S8_V2)，
     *   3,117,744 B ⇒ 需一块 3.04MB 连续 PSRAM，碎片环境下装不下）。 */
} model_family_t;

typedef struct {
    const char    *label;    /* UI 短文案（ASCII） */
    model_family_t family;
    const char    *files[2]; /* NULL 结尾 */
} vision_model_entry_t;

#define FACE_MODEL_DIR (CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR)

static const vision_model_entry_t s_models[] = {
    { "FACE", FAM_FACE,
      { "espdet_pico_224_224_face.espdl", nullptr } },
    { "HAND", FAM_HAND,
      { "espdet_pico_224_224_hand.espdl",
        "mobilenetv2_0_5_128_128_gesture.espdl" } },
    /* ★ §62 阉割 POSE：模型文件已从 littlefs_content/models/ 删除、档位表不再收录
     *   （UI 的 MODEL 按钮按 face_service_model_count() 自动变 2 档，无需改 UI 逻辑）。 */
};
#define FACE_MODEL_COUNT ((int)(sizeof(s_models) / sizeof(s_models[0])))

/* S1 沿用状态。
 * ★ s_cur_id 与 s_last_good_id 分开的原因：切换失败时要能"回到上一个能用的档位"，
 *   而 s_cur_id 在"卸掉旧的、还没建起新的"这个窗口里必须是 -1（此时确实没有可用模型）。 */
static int  s_cur_id       = -1;   /* 已就绪的档位；无模型时为 -1 */
static int  s_last_good_id = -1;   /* 最近一次**成功**的档位（回退用） */
static int  s_pending_id   = -1;   /* LVGL 线程投递的切换请求目标 */
static bool s_model_req    = false;
static bool s_enabled      = false; /* 检测开关：与"模型是否就绪"解耦（D6 常驻） */


#define SCRATCH_BYTES (240 * 240 * 3)
/* ★ 看门狗上限：真正的加载由 cam_task 里的 face_service_poll() 驱动。
 *   正常单模型加载约 900ms；HAND 两段式（两个模型先后建）更久，5s 宽松上限。 */
#define FACE_LOAD_TIMEOUT_US (5000 * 1000)

/* ============ ★ D3 埋点：把"模型常驻代价"直接算出来 ============
 * ★ §61 起加 psram_largest：模型加载失败的真判据是**最大连续块**而不是 free 总量
 *   （esp-dl 把一个 .espdl 读进一块连续 RAM），只看 free 会误判"内存还够"。 */
typedef struct {
    uint32_t int_free, int_largest, dma_free, psram_free, psram_largest;
} face_mem_t;

static face_mem_t face_mem_now(void)
{
    face_mem_t m;
    m.int_free      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m.int_largest   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    m.dma_free      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    m.psram_free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m.psram_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return m;
}

/* delta = after - before。全部用"释放为正"的语义打印，负数即"被拿走"。 */
static void face_mem_report(const char *what, uint32_t seq, const face_mem_t *before,
                            const face_mem_t *after)
{
    ESP_LOGI(TAG, "[resident] %s #%u  INT free %u->%u (%d) | largest %u->%u (%d)",
             what, (unsigned)seq,
             (unsigned)before->int_free, (unsigned)after->int_free,
             (int)(after->int_free - before->int_free),
             (unsigned)before->int_largest, (unsigned)after->int_largest,
             (int)(after->int_largest - before->int_largest));
    ESP_LOGI(TAG, "[resident] %s #%u  DMA free %u->%u (%d) | PSRAM free %u->%u (%d) largest %u->%u (%d)",
             what, (unsigned)seq,
             (unsigned)before->dma_free, (unsigned)after->dma_free,
             (int)(after->dma_free - before->dma_free),
             (unsigned)before->psram_free, (unsigned)after->psram_free,
             (int)(after->psram_free - before->psram_free),
             (unsigned)before->psram_largest, (unsigned)after->psram_largest,
             (int)(after->psram_largest - before->psram_largest));
}

/* ================= 档位选择与预检（辅助） ================= */

/* "当前/下次该用哪个档位"：已就绪的 → 上次成功的 → 开机默认(0=FACE)。
 * ★★ 有未完成的切换请求时，UI 该显示的是**目标**档位而不是旧档位。 */
static int target_model_id(void)
{
    if (s_model_req && s_pending_id >= 0) return s_pending_id;
    if (s_cur_id >= 0)       return s_cur_id;
    if (s_last_good_id >= 0) return s_last_good_id;
    return 0;   /* FACE */
}

/* 预检：本档位需要的每个文件都必须存在且非空（在"卸"之前做，素材缺失不许切换）。 */
static bool model_files_present(int id)
{
    for (int i = 0; i < 2 && s_models[id].files[i]; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", FACE_MODEL_DIR, s_models[id].files[i]);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size == 0) {
            /* ★ 素材"不见了"按成因概率排序（别照抄地址数字，地址唯一真源是 partitions.csv）：
             *   ① 设备分区表是旧的（改过 partitions.csv 却只跑了 app-flash）⇒ mount 落到
             *      旧地址后 format_if_mount_failed 静默格式化 ⇒ 空 FS；
             *   ② storage.bin 没烧 / 烧到旧偏移；
             *   ③ littlefs_content/models/ 里确实没这个文件。
             *   ① ② 都能在开机 FS_SVC 那行的 "@地址/容量" 上直接看出来。 */
            ESP_LOGE(TAG, "model file missing/empty: %s "
                          "(storage partition empty or mounted at a stale offset? "
                          "compare FS_SVC mount log vs partitions.csv)", path);
            return false;
        }
    }
    return true;
}

/* ================= ★ §61 P0：连续块预检（卸旧模型**之前**做） =================
 * 病史（2026-09-28 上机）：切 POSE（3.0MB）时 esp-dl 报
 *     E esp-dl: Failed to alloc 3044.66KB memory, largest available block size 1280.00KB.
 * 紧接着 **panic（Load access fault）**，栈是
 *     dl::Model::minimize() → fbs::FbsModel::clear_map()（fbs_model.cpp:195）
 * 真因 = human_face_detect.cpp / coco_pose.cpp 的构造里 `m_model->minimize();`
 * **无条件**跟在 `new dl::Model(...)` 后面 —— 而 Model::load() 失败只打日志返回
 * ESP_FAIL，不抛不返回空 ⇒ minimize() 作用在"没装上的" FbsModel 上 ⇒ 空指针。
 *   ⇒ 结论：**内存不够这一情况绝不能交给 esp-dl 去失败**，必须在建之前拦掉。
 *
 * 判据（两条都要过；只看 free 总量会误判）：
 *   ① 连续块：esp-dl SDCARD 模式把整个 .espdl 读进**一块连续** RAM
 *      （fbs_loader.cpp:311 `dl::tool::malloc_aligned(size, MALLOC_CAP_DEFAULT)`）
 *      ⇒ psram_largest ≥ 最大单文件 + 余量；
 *   ② 总量：HAND 是两段式，两个模型**同时常驻** ⇒ psram_free ≥ 文件之和 + 余量。
 * ★ 与《内存优化可执行方案》§3.4 同一条铁律："盯 largest，不看 free" —— 这条
 *   在内部堆上已被验证，本次证明它在 **PSRAM 侧同样成立**。
 *   ★ §62 后本工程不再有 3MB 档，但这道预检**保留**：任何新档/新模型（含未来
 *     把模型搬回 PSRAM 的方案）都得先过它，否则同样会踩 minimize-after-failed-load。 */
#define MODEL_ALLOC_SLACK  (64 * 1024)   /* malloc_aligned 对齐 + 上下文小对象余量 */
#define MODEL_TOTAL_SLACK  (256 * 1024)

/* 族内各模型文件字节数：max_one = 最大单文件（决定连续块需求），sum = 之和。 */
static void model_bytes(int id, size_t *max_one, size_t *sum)
{
    *max_one = 0;
    *sum     = 0;
    for (int i = 0; i < 2 && s_models[id].files[i]; i++) {
        char path[160];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", FACE_MODEL_DIR, s_models[id].files[i]);
        if (stat(path, &st) != 0 || st.st_size <= 0) continue;
        size_t sz = (size_t)st.st_size;
        if (sz > *max_one) *max_one = sz;
        *sum += sz;
    }
}

static bool model_mem_ok(int id)
{
    size_t max_one = 0, sum = 0;
    model_bytes(id, &max_one, &sum);

    size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t int_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (psram_largest >= max_one + MODEL_ALLOC_SLACK &&
        psram_free    >= sum     + MODEL_TOTAL_SLACK) {
        return true;
    }

    ESP_LOGE(TAG, "refuse switch to \"%s\": need contiguous %uKB (largest file) + %uKB total, "
                  "but PSRAM largest=%uKB free=%uKB | INT largest=%uKB",
             s_models[id].label,
             (unsigned)((max_one + MODEL_ALLOC_SLACK) / 1024),
             (unsigned)((sum + MODEL_TOTAL_SLACK) / 1024),
             (unsigned)(psram_largest / 1024),
             (unsigned)(psram_free / 1024),
             (unsigned)(int_largest / 1024));

    /* 一次性碎片取证（只在拒绝时打，正常路径零开销）：PSRAM 空闲块分布 ——
     * 拿它才能判断"释放某块素材能否腾出连续区"（P1-C）还是"必须走零拷贝"（P1-A）。 */
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    heap_caps_dump(MALLOC_CAP_SPIRAM);
    return false;
}

void face_service_request_load(void)
{
    (void)face_service_set_enabled(true);
}

/* 卸掉当前全部模型（两个指针都要卸；HAND 卸两段）。 */
static void unload_all(void)
{
    if (s_gcls) {
        delete s_gcls;
        s_gcls = nullptr;
    }
    if (s_detector) {
        delete s_detector;
        s_detector = nullptr;
    }
}

/* 按族构建（不卸旧的 —— 卸是 do_build 的事）。成功返回 true。 */
static bool build_family(int id)
{
    switch (s_models[id].family) {
    case FAM_FACE:
        s_detector = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, false);
        return s_detector != nullptr;

    case FAM_HAND:
        s_detector = new HandDetect(HandDetect::ESPDET_PICO_224_224_HAND, false);
        if (!s_detector) return false;
        s_gcls = new HandGestureCls(HandGestureCls::MOBILENETV2_0_5_S8_V1, false);
        if (!s_gcls) {
            /* 第二段失败：不能留一个"只检手不识别"的半成品 —— 整族回滚。 */
            delete s_detector;
            s_detector = nullptr;
            return false;
        }
        return true;
    }
    return false;
}

/* actual model build, called from cam_task context only */
static void do_build(int id)
{
    if (!model_files_present(id)) {
        /* ★ 拒绝切换并**保持现有模型不动**：预检放在卸之前的意义。 */
        ESP_LOGE(TAG, "refuse switch to \"%s\": files missing, keeping current model",
                 s_models[id].label);
        s_state = s_detector ? FACE_RUNNING : FACE_FAIL;
        return;
    }

    /* ★ §61 内存预检（同样必须在"卸"之前）：不够就拒绝，绝不让 esp-dl 走到
     *   "load 失败 + minimize()"那条会 panic 的路径上去。旧模型**原样留着**。 */
    if (!model_mem_ok(id)) {
        ESP_LOGE(TAG, "refuse switch to \"%s\": not enough contiguous PSRAM, keeping current model",
                 s_models[id].label);
        s_state = s_detector ? FACE_RUNNING : FACE_FAIL;
        return;
    }

    int64_t t0 = esp_timer_get_time();
    face_mem_t mem0 = face_mem_now();
    MEM_STAGE(TAG, "model build (pre)");

    /* ★★ 先卸后建（D8）：峰值 = 一个族的模型。 */
    if (s_detector || s_gcls) {
        face_mem_t mu0 = face_mem_now();
        unload_all();
        face_mem_t mu1 = face_mem_now();
        face_mem_report("unload", ++s_unload_seq, &mu0, &mu1);
    }
    s_cur_id = -1;                  /* 这个窗口里确实没有可用模型 */

    if (!build_family(id)) {
        MEM_ERR_DIAG(TAG, "model build FAILED (see esp-dl 'Failed to alloc' line)", 0);
        ESP_LOGE(TAG, "build \"%s\" FAILED", s_models[id].label);
        /* ★ 回退（D9）：优先回到上次成功的档位。wrapper 加载失败只打日志不抛异常
         *   ⇒ 这层只能抓"连对象都没建起来"的情况，更深一层靠搜 esp-dl 那句日志。 */
        if (s_last_good_id >= 0 && s_last_good_id != id) {
            ESP_LOGW(TAG, "falling back to last good \"%s\"", s_models[s_last_good_id].label);
            /* ★ 回退同样过内存预检：回退失败也会踩同一个 panic（minimize-after-failed-load）。 */
            if (model_mem_ok(s_last_good_id) && build_family(s_last_good_id)) {
                s_cur_id = s_last_good_id;
                s_state  = FACE_RUNNING;
                return;
            }
        }
        s_state = FACE_FAIL;
        return;
    }

    /* ★ scratch 与档位无关（输入永远是 240x240 的 RGB888 中转缓冲）
     *   ⇒ 常驻之后只需在**第一次**构建时分配，切档不再重复分配。
     *   ★ 注意失败分支**不释放 scratch**：它会被下一次构建复用。 */
    if (!s_scratch) {
        s_scratch = (uint8_t *)heap_caps_aligned_alloc(64, SCRATCH_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_scratch) {
            MEM_ERR_DIAG(TAG, "scratch alloc FAILED (SPIRAM|CACHE_ALIGNED)", SCRATCH_BYTES);
            unload_all();
            s_state = FACE_FAIL;
            return;
        }
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        MEM_ERR_DIAG(TAG, "xSemaphoreCreateMutex FAILED", 0);
        unload_all();
        s_state = FACE_FAIL;
        return;
    }

    s_cur_id       = id;
    s_last_good_id = id;
    s_state        = FACE_RUNNING;
    MEM_STAGE(TAG, "model build (post)");
    ESP_LOGI(TAG, "model \"%s\" ready in %lld ms | cam_task stack hwm=%u B",
             s_models[id].label,
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* ★★ D3 判据行：常驻不卸载，内部 RAM 永久少多少 —— 看 INT largest 括号值。 */
    face_mem_t mem1 = face_mem_now();
    face_mem_report("load", ++s_load_seq, &mem0, &mem1);
}

void face_service_poll(void)
{
    /* ★ 切换请求优先：它自己就包含"确保目标档位可用"的完整流程。 */
    if (s_model_req) {
        int id = s_pending_id;
        s_model_req  = false;
        s_pending_id = -1;
        if (id >= 0 && id < FACE_MODEL_COUNT) {
            ESP_LOGI(TAG, "switch -> \"%s\"", s_models[id].label);
            do_build(id);   /* 状态由调用方置 LOADING，这里收尾成 RUNNING / FAIL */
        }
        return;
    }
    if (!s_load_request) return;
    s_load_request = false;
    do_build(target_model_id());
}

esp_err_t face_service_stop(void)
{
    /* ★★ D6 常驻：只**关检测**，**不卸载模型**（实测常驻只花 264 B 内部 RAM）。
     *   切族才卸（do_build 里的 unload_all）。 */
    s_enabled      = false;
    s_load_request = false;
    s_model_req    = false;
    s_pending_id   = -1;
    ESP_LOGI(TAG, "detect OFF | model \"%s\" stays resident | state=%d",
             s_cur_id >= 0 ? s_models[s_cur_id].label : "-", (int)s_state);
    return ESP_OK;
}

int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        vision_result_t *out, int max)
{
    /* ★ s_enabled：常驻策略下"模型就绪"不等于"检测开着"，两者解耦。 */
    if (!s_enabled || s_state != FACE_RUNNING || !s_detector ||
        !rgb565le || !out || max <= 0) return 0;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return 0;

    uint8_t *dst = s_scratch;
    for (int i = 0; i < w * h; i++) {
        uint16_t p = rgb565le[i];
        dst[0] = ((p >> 11) & 0x1F) << 3;
        dst[1] = ((p >> 5) & 0x3F) << 2;
        dst[2] = (p & 0x1F) << 3;
        dst += 3;
    }

    dl::image::img_t img = {
        .data = s_scratch,
        .width = w,
        .height = h,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };

    auto &res = s_detector->run(img);
    const model_family_t fam = s_models[s_cur_id].family;
    int n = 0;
    for (auto &r : res) {
        if (n >= max) break;
        if (r.box.size() < 4) continue;
        out[n].x = r.box[0];
        out[n].y = r.box[1];
        out[n].w = r.box[2] - r.box[0];
        out[n].h = r.box[3] - r.box[1];
        out[n].score = r.score;
        out[n].label[0] = '\0';

        if (fam == FAM_HAND && s_gcls) {
            /* 第二段：按检测框裁剪 → 128x128 → 手势类别 */
            std::vector<int> area = { r.box[0], r.box[1], r.box[2], r.box[3] };
            auto g = s_gcls->run_crop(img, area);
            if (!g.empty() && g[0].cat_name) {
                snprintf(out[n].label, sizeof(out[n].label), "%s", g[0].cat_name);
                out[n].score = g[0].score;   /* 手势置信度比框置信度更有意义 */
            }
        }
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

face_state_t face_service_state(void)
{
    /* ★ 看门狗（见 FACE_LOAD_TIMEOUT_US 注释）。 */
    if (s_state == FACE_LOADING &&
        esp_timer_get_time() - s_load_req_us > FACE_LOAD_TIMEOUT_US) {
        s_load_request = false;
        s_model_req = false;
        s_pending_id = -1;
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "load request timed out after %d ms (cam_task not polling? camera not running?)",
                 (int)(FACE_LOAD_TIMEOUT_US / 1000));
    }
    return s_state;
}

/* ===================== 多模型档位：对外接口 =====================
 * 线程约定：UI/LVGL 线程只置标志/读表；**只有 cam_task** 执行 do_build()。 */

int face_service_model_count(void)
{
    return FACE_MODEL_COUNT;
}

const char *face_service_model_label(int id)
{
    return (id >= 0 && id < FACE_MODEL_COUNT) ? s_models[id].label : "?";
}

int face_service_current_model(void)
{
    return target_model_id();
}

esp_err_t face_service_request_model(int id)
{
    if (id < 0 || id >= FACE_MODEL_COUNT) return ESP_ERR_INVALID_ARG;
    /* 已经在目标档位上，且模型确实是好的 ⇒ 没有可做的事（避免白跑一次开关） */
    if (id == s_cur_id && s_detector && s_state == FACE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pending_id = id;
    s_model_req  = true;
    /* 立刻进"忙"态，UI 不用等 cam_task 的下一轮才变色；看门狗也一起重新计时。 */
    s_state      = FACE_LOADING;
    s_load_req_us = esp_timer_get_time();
    ESP_LOGI(TAG, "switch requested -> \"%s\"", s_models[id].label);
    return ESP_OK;
}

esp_err_t face_service_set_enabled(bool on)
{
    if (!on) return face_service_stop();
    s_enabled = true;
    if (s_state != FACE_RUNNING || !s_detector) {
        /* 模型还没就绪 ⇒ 顺带请求构建（档位 = target_model_id()）。 */
        if (s_state != FACE_LOADING) {
            s_state = FACE_LOADING;
            s_load_req_us = esp_timer_get_time();
        }
        s_load_request = true;
        ESP_LOGI(TAG, "detect ON, model build requested (\"%s\")",
                 s_models[target_model_id()].label);
    } else {
        ESP_LOGI(TAG, "detect ON (model \"%s\" already resident)",
                 s_models[s_cur_id].label);
    }
    return ESP_OK;
}

bool face_service_enabled(void)
{
    return s_enabled;
}
