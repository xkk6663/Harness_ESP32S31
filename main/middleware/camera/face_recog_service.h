/* face_recog_service.h - 人脸识别服务（特征提取 + 库比对）—— C ABI for C UI layer
 *
 * ★ 目的（S8）：把人脸"检测"升级为"识别"。
 *   检测（face_service）只回答"有没有脸、在哪"；识别要回答"这是谁"。
 *   二者靠不同的模型：检测用 espdet_pico_224_face，识别用 MFN（MobileFaceNet）。
 *
 * ★ 与官方 human_face_recognition 组件的关系（**S8-0 定案：不引入该组件**）：
 *   官方组件里真正干活的只有两类东西——
 *     ① `human_face_recognition::MFN`：一个 5 行的薄子类，继承 `dl::feat::FeatImpl`；
 *     ② `HumanFaceFeat` / `HumanFaceRecognizer`：把 MFN 与 `dl::recognition::DataBase` 串起来。
 *   而 ①② 依赖的**基础设施全部已在本工程的 esp-dl 3.3.11 里**：
 *     dl::feat::FeatImpl                           (<esp-dl>/vision/recognition/dl_feat_base.hpp)
 *     dl::image::FeatImagePreprocessor             (.../dl_feat_image_preprocessor.hpp)
 *     dl::feat::FeatPostprocessor                  (.../dl_feat_postprocessor.hpp)
 *     dl::recognition::DataBase                    (.../dl_recognition_database.hpp)
 *   ⇒ 只需要**一个 MFN 权重文件**，类自己复刻（见 .cpp），**零新组件、零依赖版本变更**。
 *   （这正是 S8 方案 §7-D1 选 A1 而非 A0 的原因。）
 *   ★ 而且本工程**连 `dl::recognition::DataBase` 都不用** —— 库与比对已经有
 *     `face_db`（双份世代号 + CRC + 拷贝语义 API），再造一份内存库等于两套真相源。
 *     比对直接走 `face_db_find_best()`。
 *
 * ★★ 模型文件（放 littlefs 的 models/ 目录，由 littlefs_content/models/ 烧录）：
 *     human_face_feat_mfn_s8_v1.espdl              1,295,200 B  (112x112x3, MFN)
 *     human_face_detect_msr_s8_v1.espdl               61,248 B  (MSR：粗检出)
 *     human_face_detect_mnp_s8_v1.espdl              129,968 B  (MNP：5 点关键点)
 *   ⚠ 文件名**必须一字不差** —— 官方 README 原话：
 *     "Do not change the model name when copy the models to sdcard."
 *
 * ★★ 识别还需要**5 点关键点**（不是可选项）：
 *   `dl::feat::Feat::run(img, landmarks)` 的第二个参数就是关键点，用于把人脸仿射对齐到
 *   112x112 标准位姿。而本工程现役的 FACE 档（espdet_pico）**只输出检测框、没有关键点**
 *   ⇒ 走识别必须另配 MSR + MNP 两段式（两个文件都已在 littlefs 里）。
 *   （官方源码写死的：`auto feat = m_feat.run(img, detect_res.back().keypoint);`）
 *
 *   ★★ 关键点的**点序**是个静默陷阱（本文件踩过）：
 *     `esp-dl/vision/recognition/dl_feat_image_preprocessor.cpp` 里写死的标准布局是
 *         s_std_ldks_112 = { 左眼, 左嘴角, 鼻尖, 右眼, 右嘴角 }
 *     而**不是**"左眼→右眼→鼻尖→左嘴角→右嘴角"这个更符合直觉的顺序。
 *     MNP 输出的 `result_t.keypoint` 正好与前者一致 ⇒ 必须**原样**喂给
 *     `FeatImagePreprocessor`，**一个都不许重排**。
 *     ⚠ 顺序搞错**不会报错**，只会让对齐矩阵算歪、特征悄悄变差（表现为"谁都认不出"），
 *       所以 S8-0 的自检（用合成图跑通即可）**测不出**这个错 —— 只有真机认人才会发现。
 *
 * ===========================================================================
 *  ★ 线程纪律（S8-2 的硬约束，来自 §14.2 的实测：MFN 单次推理 324 ms）
 * ===========================================================================
 *   识别**绝不能**跑在 LVGL 线程（taskLVGL 栈只有 9728 B、峰值已 6576 B，
 *   再叠 esp-dl 的栈需求会顶穿；而且 324 ms 会卡掉 6.5 帧预览）。
 *   ⇒ 本模块的分工：
 *     · `face_recog_poll()`     —— 由 **cam_task** 驱动，**只做模型加载**；
 *     · 识别本体               —— 由 `face_recog_submit()` 起一个**一次性任务**
 *                                 （跑完自删），与预览帧率**完全解耦**；
 *     · LVGL 线程              —— 只 `submit()` + 读结果快照 + `consume()`。
 *   ★ `submit()` 里会把 RGB565 帧**同步转成 RGB888 拷进引擎缓冲**，
 *     所以 LVGL 线程返回后可以立刻改写自己的画布（没有跨线程的指针共享）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "face_service.h"       /* face_state_t（与检测服务同风格，方便 UI 统一判断） */
#include "face_db.h"            /* FACE_NAME_MAX, FACE_DB_DIM_MAX, FACE_DB_GUEST_ID */

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  S8-0 自检（保留：探针开关打开时才会被调用）
 * ========================================================================== */
/** S8-0 自检结果快照 */
typedef struct {
    bool      loaded;              /* 模型是否加载成功（**这是 S8-0 的核心结论**） */
    int       feat_len;            /* 特征维度（实测；MFN 应为 512） */
    uint32_t  load_ms;             /* 模型加载耗时 */
    uint32_t  infer_ms;            /* 单次推理耗时（不含加载） */
    size_t    model_bytes;         /* 模型文件字节数 */
    size_t    psram_free_kb;       /* 模型加载后的 PSRAM free */
    size_t    psram_largest_kb;    /* 模型加载后的 PSRAM largest */
    float     feat_norm;           /* 特征向量 L2 模长（正常应 ≈1.0） */
    float     feat_head[4];        /* 特征向量前 4 个分量（"不是全 0"的肉眼判据） */
    esp_err_t err;                 /* 失败原因（ESP_OK / NOT_FOUND / NO_MEM / FAIL） */
    char      note[96];            /* 一行补充说明 */
} face_recog_probe_t;

/**
 * @brief S8-0 可行性自检：预检 → 加载 MFN → 合成图推理一次 → 释放。
 *
 * ★★ 这是一次**阻塞**调用（加载 + 推理，数秒），必须放在独立任务里调，
 *    **绝不能在 LVGL 线程**。
 * ★ 它**不验证识别准确率** —— 输入是合成渐变图而非真人脸，只看"通不通"。
 *
 * @param out 结果快照（可为 NULL，此时只打印日志）
 * @return ESP_OK 全通过；否则为失败原因（同 out->err）
 */
esp_err_t face_recog_probe(face_recog_probe_t *out);

/* ==========================================================================
 *  S8-2：常驻识别引擎
 * ========================================================================== */

/** 特征维度（MFN = 512，与 face_db 的 dim 一致） */
#define RECOG_FEAT_DIM       512
/** 关键点个数（MSR+MNP 的 5 点：左眼/左嘴角/鼻尖/右眼/右嘴角） */
#define RECOG_KP_NUM         5

/** 命中阈值（余弦相似度）。
 *  ★ 0.6 是 §9-R11 的建议起点：**宁可拒识（落到访客目录）也不要认错人** ——
 *    打卡场景里"把 A 认成 B"的后果远重于"把 A 认成陌生人"。
 *    真机用起来若觉得太严，调这一个宏即可（库里补录同一人多张也能显著提高分数）。 */
#define RECOG_MATCH_THR      0.60f

/** 一次识别作业的结果（**约 2.2 KB**，由引擎持有；调用方只拿指针）。 */
typedef struct {
    bool  valid;                  /* 本次作业是否真的检出了可用人脸 */
    bool  matched;                /* 是否命中库（EXTRACT 作业恒 false） */
    int   person_id;              /* 命中者 id；未命中 = FACE_DB_GUEST_ID(0) */
    char  name[FACE_NAME_MAX];    /* 命中者姓名；未命中为空串 */
    float det_score;              /* MSR 检出置信度（**不是**相似度！质量门槛用它） */
    float score;                  /* 与命中者的余弦相似度（未命中 = 最高分） */
    int   x, y, w, h;             /* MSRMNP 的检出框（像素，已裁剪到画面内） */
    int   kp[RECOG_KP_NUM * 2];   /* 5 点关键点，顺序见文件头"点序陷阱" */
    float feat[RECOG_FEAT_DIM];   /* 特征向量（L2 归一化）—— 录入作业要用它 */
} recog_result_t;

/** 作业类型 */
typedef enum {
    RECOG_JOB_IDENTIFY = 0,   /* 检出 → 对齐 → 提特征 → **查库**（打卡用） */
    RECOG_JOB_EXTRACT  = 1,   /* 检出 → 对齐 → 提特征 → **不查库**（录入用） */
} recog_job_t;

/** 初始化（幂等，只归零软件状态，**不加载任何模型、不分配大缓冲**）。 */
esp_err_t face_recog_init(void);

/** 请求异步加载模型（任意线程可调，等价于 `face_recog_set_enabled(true)`）。 */
void face_recog_request_load(void);

/**
 * @brief **驱动本模块的唯一入口**：必须在宿主页面里周期调用（CAM 页每 50 ms 一次）。
 *
 * ★ **非阻塞**（2026-09-28 起）：真正的 `engine_load()` 被派发到一条一次性任务
 *   （`RECOG_LOAD_STACK` 10240 B）里执行，本函数只做两件事 ——
 *   ① 看门狗（请求了 8 s 还没开始加载 ⇒ 说明本函数根本没被调用，报错留痕）；
 *   ② 起那条加载任务（已在跑就不重复起）。
 *
 * ★★ 不调用本函数的后果（2026-09-28 的 P0 事故）：引擎永远停在 `FACE_LOADING`，
 *   `face_recog_submit()` 永远返回 `ESP_ERR_INVALID_STATE`，而**日志一行都不出**
 *   —— ENROLL 卡在 `capturing 0/3`，表现成"按了没反应"。
 */
void face_recog_poll(void);

/** 停止并释放模型 + 帧缓冲（幂等）。
 *  ★ 识别任务**或加载任务**在跑时会被拒（返回 `ESP_ERR_INVALID_STATE`），此时
 *    状态保持 RUNNING/LOADING，调用方应下一拍重试（防 UAF）。 */
esp_err_t face_recog_stop(void);

/* ==========================================================================
 *  §73.21 会话独占（claim）—— 多驱动竞争的根治
 * ==========================================================================
 *  ★ 背景（2026-09-30 实测事故）：MCP 打卡会话（ai_punch）接管相机期间，
 *    CAM 页的 recog_pump 用"停流前的陈旧 canvas 帧"继续提交识别作业、并
 *    抢走/消费了会话的结果（单槽结果总线被两个驱动共写共读）—— 会话读到
 *    "路过时的陈旧帧"的 guest 结论，误判"人脸库里没有你"。
 *
 *  ★ 语义：认领后，**其他任务**的 face_recog_submit() 返回
 *    ESP_ERR_INVALID_STATE、face_recog_result() 返回 NULL、
 *    face_recog_result_consume() 被忽略 —— 引擎与结果通道由认领任务独占。
 *    认领者自己的调用完全不受影响。face_recog_poll()/stop() 不受认领影响
 *    （poll 单飞幂等；stop 只由认领者自己调）。
 *  ★ 判定按**任务句柄**（不用锁）：认领/释放/提交/取结果必须都在同一任务
 *    里完成 —— TAB_CAM 在 LVGL 任务、ai_punch 在 WS 收包任务，天然互斥。
 *  ★ 幂等：重复认领返回 true；被他人持有时返回 false（附警告日志）。
 *  ★ 责任：认领者必须在会话结束（含所有失败路径）时 release —— 参照
 *    ai_punch 的 cleanup 写法。 */
bool face_recog_session_claim(void);
void face_recog_session_release(void);

/** 当前是否有 MCP 会话持有引擎（§73.24：tab_camera 结果槽轮询的守卫——
 *  会话存续期间 CAM 页不得消费 punch_service 结果槽，否则会把 ai_punch
 *  还没读走的结果偷走并 clear，导致 MCP 打卡误报失败；2026-09-30 实测）。 */
bool face_recog_session_is_claimed(void);

/** 引擎状态（语义同 face_service_state）。★ 纯 getter，无副作用。 */
face_state_t face_recog_state(void);

/** 是否已就绪（= RUNNING 且内部三件套齐全）。 */
bool face_recog_ready(void);

/** 识别开关（与"模型是否就绪"解耦；语义同 face_service_set_enabled）。 */
esp_err_t face_recog_set_enabled(bool on);
bool      face_recog_enabled(void);

/**
 * @brief 提交一次识别作业（**非阻塞**，受理制）。任意线程可调（实际只从 LVGL 线程调）。
 *
 * ★ 内部**立即**把 240x240 RGB565 帧转成 RGB888 拷进引擎缓冲
 *   （约 57,600 次迭代，毫秒级），所以调用方返回后可以马上改写自己的画布。
 * ★ 结果通过 `face_recog_result()` 取，用完后必须 `face_recog_result_consume()`
 *   才能提交下一个作业（这就是 busi/空闲的握手）。
 *
 * @param rgb565le 240x240（或任意 w x h）的 RGB565 **小端**帧
 * @return ESP_OK                 已受理
 *         ESP_ERR_INVALID_STATE  上一次结果还没被 consume / 模型没就绪 / 未使能
 *         ESP_ERR_INVALID_ARG    尺寸不支持（超过编译期内部分配的缓冲）
 *         ESP_ERR_NO_MEM         起任务失败（内部 RAM 紧）
 */
esp_err_t face_recog_submit(const uint16_t *rgb565le, uint16_t w, uint16_t h, recog_job_t job);

/** 是否有作业在跑（或结果待取）。UI 据此避免叠加提交。 */
bool face_recog_busy(void);

/**
 * @brief 取最近一次结果（**只读快照**）。
 * @return 有新结果时返回指针；否则 NULL。指针在下次 submit 之前一直有效。
 */
const recog_result_t *face_recog_result(void);

/** 消费掉结果（清"新结果"标志）。**只有消费后**才允许再次 submit。 */
void face_recog_result_consume(void);

/** 单次识别的耗时（ms，最近一次；0 = 还没跑过）。供 UI/日志观测。 */
uint32_t face_recog_last_ms(void);

/**
 * @brief 取引擎里那份 **RGB888 帧**（就是最近一次识别所看到的那一帧）。
 *
 * ★ 用途：打卡存证要求"**所见即所存**" —— 直接把这张已经识别过的图交给
 *   `punch_service`，而不是从别处再抓一帧（那样两者可能不是同一瞬间）。
 * ★ 生命周期：在 `face_recog_submit()` 之前一直有效（下次 submit 会覆盖它）。
 *   调用方**不得**保留这个指针跨过下一次 submit。
 *
 * @param w,h 任一出参可为 NULL
 * @return 缓冲首地址；引擎未就绪时返回 NULL
 */
const uint8_t *face_recog_frame(int *w, int *h);

#ifdef __cplusplus
}
#endif
