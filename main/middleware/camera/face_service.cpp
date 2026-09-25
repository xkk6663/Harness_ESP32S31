/* face_service.cpp - esp-dl face detection wrapper, C ABI */
#include "face_service.h"
#include "human_face_detect.hpp"
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
#include <fcntl.h>
#include <sys/stat.h>

static const char *TAG = "FACE";

static HumanFaceDetect *s_detector = nullptr;
static SemaphoreHandle_t s_lock;
static uint8_t *s_scratch;
static face_state_t s_state = FACE_IDLE;
static bool s_load_request = false;
static int64_t s_load_req_us = 0;
/* ★ D3 埋点：load/unload 配对编号。日志里 #N 对齐即可看出
 * "反复开关后 INT largest 是否单调下滑"（＝反复 new/delete 的碎片证据）。 */
static uint32_t s_load_seq = 0;
static uint32_t s_unload_seq = 0;

/* ================= ★ S1 多模型档位表 =================
 * 三档**同族**：全部走 HumanFaceDetect，只是 model_type_t 不同 ⇒ 条目只需要
 * "枚举 + 文件名"的映射，**不需要 create/destroy/infer 函数指针**
 * （那是 S2 要加"异族模型"时才需要的东西，现在引入只会多一层没人读的间接）。
 *
 * ★★ 本表顺序**刻意**与组件 Kconfig 里 model_type_t 的枚举顺序一致
 *    （0 = MSRMNP_S8_V1，1 = ESPDET_PICO_224_224_FACE，2 = ESPDET_PICO_416_416_FACE）
 *    ⇒ 开机默认档可以直接拿 CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL 当 id 用，
 *      不必再写一层容易写错的映射。**改动本表顺序必须同步改这里的语义。**
 * ★ 两个 ESPDet 档各要 1 个文件；MSRMNP 是两阶段（MSR + MNP），要 2 个
 *    （human_face_detect.cpp:197-201 / :210 / :218，文件名在组件里是硬编码的）。
 * ★ 文件名与目录都是"组件硬编码 + 编译期常量"（CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR）
 *    ⇒ 切档只能换枚举值，**不能靠换目录**；做变体 A/B 也只能同名覆盖文件内容。
 * ★ 这里**不写模型输入尺寸**：尺寸由模型自己决定，detect() 传任意尺寸都会被
 *   内部 ImagePreprocessor 缩放（见方案 §2.4）。写死尺寸反而会误导 ——
 *   416 档吃 240 输入只是插值放大、不增信息，标个 "416" 会让人以为更准。 */
typedef struct {
    const char *label;      /* UI 短文案（ASCII） */
    int         hfdet_enum; /* HumanFaceDetect::model_type_t */
    const char *files[2];   /* NULL 结尾 */
} face_model_entry_t;

#define FACE_MODEL_DIR (CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR)

static const face_model_entry_t s_models[] = {
    { "MSR", HumanFaceDetect::MSRMNP_S8_V1,
      { "human_face_detect_msr_s8_v1.espdl",
        "human_face_detect_mnp_s8_v1.espdl" } },
    { "224", HumanFaceDetect::ESPDET_PICO_224_224_FACE,
      { "espdet_pico_224_224_face.espdl", nullptr } },
    { "416", HumanFaceDetect::ESPDET_PICO_416_416_FACE,
      { "espdet_pico_416_416_face.espdl", nullptr } },
};
#define FACE_MODEL_COUNT ((int)(sizeof(s_models) / sizeof(s_models[0])))

/* S1 新增状态。
 * ★ s_cur_id 与 s_last_good_id 分开的原因：切换失败时要能"回到上一个能用的档位"，
 *   而 s_cur_id 在"卸掉旧的、还没建起新的"这个窗口里必须是 -1（此时确实没有可用模型），
 *   不能拿它去回退。 */
static int  s_cur_id       = -1;   /* 已就绪的档位；无模型时为 -1 */
static int  s_last_good_id = -1;   /* 最近一次**成功**的档位（回退用） */
static int  s_pending_id   = -1;   /* LVGL 线程投递的切换请求目标 */
static bool s_model_req    = false;
static bool s_enabled      = false; /* 检测开关：与"模型是否就绪"解耦（D6 常驻） */


#define SCRATCH_BYTES (240 * 240 * 3)
/* ★ 看门狗上限：真正的加载由 cam_task 里的 face_service_poll() 驱动。
 *   如果相机没在跑（cam_task 卡住/未启动），poll 永远不会被调用 ——
 *   2026-09-25 实测过一次：cam_task 卡在 DQBUF，UI 就永远停在 "LOADING..."。
 *   超时后报 FACE_FAIL，让 UI 说话，而不是假装还在加载。
 *   正常加载约 900ms，3s 是很宽松的上限。 */
#define FACE_LOAD_TIMEOUT_US (3000 * 1000)

/* ============ ★ D3 埋点：把"模型常驻代价"直接算出来 ============
 * 背景（方案 §9-D3）：模型加载策略是 (a) 每次开关 delete+重载，还是 (b) 常驻不卸载？
 * 当时定的是"先加埋点、拿数据再决定"。这里就是那个埋点。
 *
 * ★ 为什么不能只靠 (pre)/(post) 两行 MEM_STAGE 人肉减：
 *   那两个数只给出"这一段净消耗"，既分不清**瞬时峰值**与**常驻**，也看不出
 *   "反复开关后 largest 是否单调下降"（碎片趋势）。所以这里：
 *     · load  ：进/出各取一次快照，打印 **INT largest 的差 = 常驻内部 RAM**（D3 的核心数字）
 *     · unload：卸载前后各取一次，打印"还回来多少"
 *               ⇒ 还回来 ≈ 拿走说明不泄漏；还回来 < 拿走 = 泄漏或碎片；
 *                 连做 N 次开关看 largest 是否持续下滑 = 反复 new/delete 的碎片证据。
 *   用 #N 编号把 load/unload 配对，日志里直接就能对齐看趋势。 */
typedef struct {
    uint32_t int_free, int_largest, dma_free, psram_free;
} face_mem_t;

static face_mem_t face_mem_now(void)
{
    face_mem_t m;
    m.int_free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m.int_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    m.dma_free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    m.psram_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
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
    ESP_LOGI(TAG, "[resident] %s #%u  DMA free %u->%u (%d) | PSRAM free %u->%u (%d)",
             what, (unsigned)seq,
             (unsigned)before->dma_free, (unsigned)after->dma_free,
             (int)(after->dma_free - before->dma_free),
             (unsigned)before->psram_free, (unsigned)after->psram_free,
             (int)(after->psram_free - before->psram_free));
}

/* ================= S1：档位选择与预检（辅助） ================= */

/* "当前/下次该用哪个档位"：已就绪的 → 上次成功的 → 开机默认。
 * ★ 让 UI 在"模型还没构建"时也能显示一个合理的档位名。
 * ★★ 有未完成的切换请求时，UI 该显示的是**目标**档位而不是旧档位 ——
 *    否则用户按了切档、按钮文案纹丝不动，会以为没生效（而实际上正在切）。 */
static int target_model_id(void)
{
    if (s_model_req && s_pending_id >= 0) return s_pending_id;
    if (s_cur_id >= 0)       return s_cur_id;
    if (s_last_good_id >= 0) return s_last_good_id;
    return CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL;
}

/* 预检：本档位需要的每个文件都必须存在且非空。
 * ★★ 为什么必须在"卸"之前做：切换是"先卸后建"。若新档位的文件根本没烧进
 *    littlefs，旧的已经卸了、新的建不起来 ⇒ 用户**当场失去可用模型**，而且 UI
 *    只会显示 FAIL，完全看不出是"素材没烧对"。这一步把这种情况挡在"卸"之前，
 *    代价只是一次 stat()。 */
static bool model_files_present(int id)
{
    for (int i = 0; i < 2 && s_models[id].files[i]; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", FACE_MODEL_DIR, s_models[id].files[i]);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size == 0) {
            ESP_LOGE(TAG, "model file missing/empty: %s "
                          "(did you flash build/storage.bin to 0xA20000?)", path);
            return false;
        }
    }
    return true;
}

void face_service_request_load(void)
{
    (void)face_service_set_enabled(true);
}

/* actual model build, called from cam_task context only */
static void do_build(int id)
{
    if (!model_files_present(id)) {
        /* ★ 拒绝切换并**保持现有模型不动**：s_detector / s_cur_id 一律不碰。
         *   这正是"预检放在卸之前"的意义 —— 素材没烧对不该让用户连旧的都用不了。 */
        ESP_LOGE(TAG, "refuse switch to \"%s\": files missing, keeping current model",
                 s_models[id].label);
        s_state = s_detector ? FACE_RUNNING : FACE_FAIL;
        return;
    }

    int64_t t0 = esp_timer_get_time();
    /* ★ D3 埋点：进入快照。出栈时的差就是"这次构建永久拿走了多少"。 */
    face_mem_t mem0 = face_mem_now();

    /* ★ 判读：largest(INT) 在构建前后**不塌**才算健康。若塌了，就是 esp-dl 构建期
     * 那批小对象（算子注册 / std::map 建表 / std::string）把内部堆切碎了 ——
     * 那正是 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 必须为 0 的直接证据（也是 D3 的判据）。
     * ★ esp-dl 自己也会打一句 "Failed to alloc %.2fKB internal RAM, largest available
     *   internal RAM block size %.2fKB"（dl_model_context.cpp:176），搜那句能读到它要多少。 */
    MEM_STAGE(TAG, "model build (pre)");

    /* ★★ 先卸后建（D8）：峰值 = 一个模型。两个模型同时存在会让 esp-dl 构建期的
     *    内部 RAM 瞬时峰值**叠加** —— 那正是 NO_MEM 的高发区；而"先建后卸"没有任何好处。
     * ★ 这里给"卸"单独记一笔账（S1 的碎片验收判据就是它）：
     *   "反复切档 N 次后 INT largest 不单调下滑" 只能靠 load/unload 两笔账配对看出来
     *   —— 还回来 ≈ 拿走 ⇒ 健康；持续少还 ⇒ 泄漏或碎片。 */
    if (s_detector) {
        face_mem_t mu0 = face_mem_now();
        delete s_detector;
        s_detector = nullptr;
        face_mem_t mu1 = face_mem_now();
        face_mem_report("unload", ++s_unload_seq, &mu0, &mu1);
    }
    s_cur_id = -1;                  /* 这个窗口里确实没有可用模型 */

    HumanFaceDetect *d = new HumanFaceDetect(
        static_cast<HumanFaceDetect::model_type_t>(s_models[id].hfdet_enum), false);
    if (!d) {
        MEM_ERR_DIAG(TAG, "new HumanFaceDetect FAILED (see esp-dl 'Failed to alloc' line)", 0);
        ESP_LOGE(TAG, "build \"%s\" FAILED", s_models[id].label);
        /* ★ 回退（D9）：优先回到上次成功的档位。
         * ⚠ 已知局限：HumanFaceDetect::load_model() 失败时**只打日志、不抛异常**，
         *   new 仍可能返回非空指针 ⇒ 这一层只能抓住"连对象都没建起来"的情况。
         *   更深一层要等 esp-dl 暴露加载结果，或靠搜它那句 'Failed to alloc' 日志。 */
        if (s_last_good_id >= 0 && s_last_good_id != id) {
            ESP_LOGW(TAG, "falling back to last good \"%s\"", s_models[s_last_good_id].label);
            d = new HumanFaceDetect(
                static_cast<HumanFaceDetect::model_type_t>(s_models[s_last_good_id].hfdet_enum),
                false);
            if (d) { s_detector = d; s_cur_id = s_last_good_id; s_state = FACE_RUNNING; return; }
        }
        s_state = FACE_FAIL;
        return;
    }
    s_detector = d;

    /* ★ scratch 与本档位无关（输入永远是 240x240 的 RGB888 中转缓冲）
     *   ⇒ 常驻之后只需在**第一次**构建时分配，切档不再重复分配。
     *   ★ 注意下面失败分支**不释放 scratch**：它会被下一次构建复用。 */
    if (!s_scratch) {
        s_scratch = (uint8_t *)heap_caps_aligned_alloc(64, SCRATCH_BYTES,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_scratch) {
            MEM_ERR_DIAG(TAG, "scratch alloc FAILED (SPIRAM|CACHE_ALIGNED)", SCRATCH_BYTES);
            delete s_detector; s_detector = nullptr;
            s_state = FACE_FAIL;
            return;
        }
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        MEM_ERR_DIAG(TAG, "xSemaphoreCreateMutex FAILED", 0);
        delete s_detector; s_detector = nullptr;
        s_state = FACE_FAIL;
        return;
    }

    s_cur_id       = id;
    s_last_good_id = id;
    s_state        = FACE_RUNNING;
    MEM_STAGE(TAG, "model build (post)");
    /* ★ P2-5 的数据来源：构建跑在 cam_task 的 6144 B 栈上，
     * 这里读它的最小剩余栈 —— 若已贴 0，说明必须扩栈或把 do_build 挪走。 */
    ESP_LOGI(TAG, "model \"%s\" ready in %lld ms | cam_task stack hwm=%u B",
             s_models[id].label,
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* ★★ D3 判据行：这一行就是"常驻不卸载，内部 RAM 会永久少多少"的答案。
     * 判读：看 INT largest 的括号值 —— 它才是能不能再挤出连续块的唯一口径。
     *   · largest 掉得多（如 -20KB）⇒ 常驻代价高；
     *   · largest 基本不动 ⇒ 常驻几乎不花内部 RAM（都落在 PSRAM），放心常驻。
     * PSRAM 那一项主要来自模型文件 + 172,800 B scratch。 */
    face_mem_t mem1 = face_mem_now();
    face_mem_report("load", ++s_load_seq, &mem0, &mem1);
}

void face_service_poll(void)
{
    /* ★ 切换请求优先：它自己就包含"确保目标档位可用"的完整流程（预检 → 卸 → 建），
     *   所以处理完它就不再走下面的首次加载分支。 */
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
    /* ★★ S1 起语义变更（D6 常驻）：只**关检测**，**不再卸载模型**。
     *   依据（`[resident]` 实测，见方案 §3.2）：
     *     · 常驻代价 = 264 B 内部 RAM（largest 零变化）+ 1.038 MB PSRAM；
     *     · 换来免掉每次进页面 ~0.9 s 加载，并把"加载期花屏"限制成开机后**仅一次**
     *       （加载期 memcpy16k 602 us = 预算的 136%，是全部场景里最差的一档）。
     *   ★ 所以这里**绝不能** delete s_detector —— 否则 UI 一个"关检测"动作就要隐式
     *     付出 ~1 MB PSRAM 重新加载 + 一次花屏窗口的代价。
     *   ★ 若将来真要"卸载"（内存告急），入口应是一个**显式**的 *_release()，
     *     而不是把本函数改回去。s_state 也**保持 RUNNING**（模型确实还在）。 */
    s_enabled      = false;
    s_load_request = false;
    s_model_req    = false;
    s_pending_id   = -1;
    ESP_LOGI(TAG, "detect OFF | model \"%s\" stays resident | state=%d",
             s_cur_id >= 0 ? s_models[s_cur_id].label : "-", (int)s_state);
    return ESP_OK;
}

int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        face_box_t *out, int max)
{
    /* ★ S1：多一个 s_enabled —— 常驻策略下"模型就绪"不等于"检测开着"，两者解耦。 */
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
    int n = 0;
    for (auto &r : res) {
        if (n >= max) break;
        if (r.box.size() < 4) continue;
        out[n].x = r.box[0];
        out[n].y = r.box[1];
        out[n].w = r.box[2] - r.box[0];
        out[n].h = r.box[3] - r.box[1];
        out[n].score = r.score;
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

face_state_t face_service_state(void)
{
    /* ★ 看门狗（见 FACE_LOAD_TIMEOUT_US 注释）。
     * 这里由 UI 每 50ms 轮询触发；若 poll 一直没被调用（相机没跑），超时即转 FAIL。
     * 竞态说明：极端情况下 do_build() 恰好还在跑超过 3s，会先被判 FAIL、随后 do_build
     * 完成又置 RUNNING —— 状态自愈，不影响正确性。 */
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

/* ===================== ★ S1 多模型档位：对外接口 =====================
 * 线程约定（与原有 face_service_request_load 一致）：
 *   · UI/LVGL 线程可以调：count / label / current_model / request_model / set_enabled / enabled / state
 *   · **只有 cam_task 线程**会执行 do_build() —— 由 face_service_poll() 驱动。
 *   ★ 这里所有"写"都只是置标志/置 int（32 位对齐，原子），真正的重活在 poll 里做。
 *     绝不能在 LVGL 线程里 new HumanFaceDetect：那是 ~0.9 s 的阻塞，会直接卡住 UI。 */

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
        /* 模型还没就绪 ⇒ 顺带请求构建（档位 = target_model_id()）。
         * ★ 已在 LOADING 时不要重置看门狗计时，否则"迟迟加载不完"永远等不到超时。 */
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
