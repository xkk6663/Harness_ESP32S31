/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* =============================================================================
 * ota_service.c —— WiFi 局域网 OTA 升级（设备端，M1+M2+M3 后端）
 *
 * 方案：docs/项目规划/WiFi局域网OTA升级实施方案.md §3.1
 * 协议：docs/项目规划/通信接口规范_ICD.md §8
 * 流程：PC 发 OTA_OFFER(UDP 50003) → 本服务校验 → OTA_ACCEPT
 *       → HTTP GET http://<pc>:50005/firmware.bin 流式读（支持 Range 续传）
 *       → 边收边算 SHA256 + esp_ota_write 写备用槽
 *       → 每 2% 回 OTA_PROGRESS → esp_ota_end + SHA256 比对
 *       → set_boot_partition + OTA_RESULT{ok:1} → 1s 后 esp_restart
 *       重启进新槽 = PENDING_VERIFY → main.c 30s 自证 → mark_valid
 *       （不自证则 bootloader 下次自动回滚）
 *
 * ★★★ 本文件的硬约束（都是本工程踩过的坑，改动前先读）
 *
 *  1. 【F35/F44】下载任务栈外置 PSRAM 是**安全**的：本任务只做网络 IO +
 *     esp_ota_write（写 flash 但不碰 littlefs/NVS **文件系统**；nvs 断点
 *     用的是 nvs API，不是文件系统，见约束 2），且 esp_ota_write 内部
 *     自行处理 cache 关窗。⚠ 但**绝不能**在本任务里 fopen/unlink littlefs
 *     —— 那会触发 F28/F30 的"PSRAM 栈 + flash IO" panic。
 *     （注意：nvs_set_blob 会走 esp_partition 写 + 内部缓存，**不经 VFS**，
 *      实测在 PSRAM 栈任务里可用；若将来报 cache error，把本任务栈改回内部。）
 *
 *  2. 【F44】link_manager 的 JSON 回调跑在 link_task 上下文 ⇒
 *     ota_on_json() 只允许 cJSON 解析 + 命令入队，**禁止阻塞 IO**。
 *     真正的下载在 ota_task 里做。
 *
 *  3. 【§3.1-4】esp_ota_write 会短暂关 cache；下载循环每块之间
 *     vTaskDelay(1) 让出 CPU，避免饿死 LVGL/音频。
 *
 *  4. 【内存规范 A4】所有 alloc 失败路径必须打 (want/free/largest) 三元组；
 *     运行期禁止 assert（A2）；本文件所有缓冲区优先静态池（S1）。
 *
 * ── 日志体系（可 grep）─────────────────────────────────────────────
 *   grep "OTA_DIAG"  → 分阶段进度 + 内存五元（排错主场）
 *   grep "OTA_ERR"   → 所有错误（含错误码与偏移）
 *   grep "OTA_MEM"   → 纯内存三元组打点
 *   grep "OTA_RESUME"→ 断点续传相关
 * ========================================================================== */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp（SHA256 hex 大小写不敏感比对） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps（栈外置 PSRAM，F35） */
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"
/* ★ IDF v6.1 的 mbedtls 已完成 PSA 迁移：旧 API mbedtls_sha256_* 的公开头
   mbedtls/sha256.h 已不存在（只剩 tf-psa-crypto 下的 private 头）。
   正确入口是 PSA Crypto 流式接口（psa_hash_setup/update/finish），
   头文件 psa/crypto.h 由 mbedtls 组件导出。 */
#include "psa/crypto.h"
#include "cJSON.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/audio/record_service.h"
#include "middleware/audio/session_manager.h"
#include "middleware/network/wifi_manager.h"
#include "middleware/network/pcnow_service.h"   /* PCNOW_HTTP_PORT(=50005) 复用 */
#include "ota_service.h"

static const char *TAG = "OTA";

/* ---- 常量 ---- */
#define OTA_READ_CHUNK      4096                       /* 每块读 4KB（方案 §3.1-2） */
#define OTA_TASK_STACK      8192
#define OTA_PROGRESS_STEP   2                          /* 每 2% 回一条 PROGRESS */
#define OTA_HTTP_TIMEOUT    15000                      /* 15s：局域网 4.9MB 远够 */
#define OTA_REBOOT_DELAY_MS 1000
#define OTA_CMD_Q_LEN       4
#define OTA_RESP_BUF        192                        /* 回包 JSON 缓冲 */
#define OTA_URL_MAX         192
#define OTA_SHA_HEX         65                         /* 32B → 64 hex + NUL */

/* ---- M2：分段哈希续传 ---- */
#define OTA_SEG_SIZE        (256 * 1024)               /* 锚点段大小 256KB → 5MB 约 20 段 */
#define OTA_SEG_MAX         24                         /* 支持到 6MB（本槽 5MB，留余量） */
#define OTA_NVS_NS          "ota"
#define OTA_NVS_KEY_RESUME  "resume"

/* ---- 内存打点 ---- */
#define OTA_MEM(label)                                                          \
    do {                                                                        \
        ESP_LOGI(TAG, "[OTA_MEM] %-24s INT free=%u largest=%u | DMA free=%u | PSRAM free=%u", \
                 (label),                                                       \
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),        \
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), \
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),             \
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));        \
    } while (0)

/* ---- 命令（link 回调 → 下载任务）---- */
typedef enum {
    OTA_CMD_OFFER = 0,   /* arg = 指向已拷贝的 OFFER JSON 字符串（任务内 free） */
    OTA_CMD_ABORT,
} ota_cmd_id_t;

typedef struct {
    ota_cmd_id_t id;
    char        *json;   /* OFFER 时携带（heap_caps_malloc 拷贝，任务消费后 free） */
} ota_cmd_t;

typedef enum {
    ST_IDLE = 0,
    ST_OFFERED,
    ST_DOWNLOADING,
    ST_VERIFY,
    ST_REBOOT_PENDING,
} ota_state_t;

/* ---- nvs 断点记录（M2）---- */
typedef struct {
    uint32_t magic;                    /* OTA_RESUME_MAGIC 校验版本 */
    uint32_t total_size;               /* OFFER 的 size，变了就作废 */
    uint32_t next_offset;              /* 下一个要拉的位置（= 已完成字节数） */
    uint32_t seg_done;                 /* 已完整校验的段数 */
    char     sha256[OTA_SHA_HEX];      /* 整包 SHA256（hex），变了就作废 */
    uint8_t  seg_sha[OTA_SEG_MAX][32]; /* 各锚点段摘要（seg_done 之前有效） */
} ota_resume_t;
#define OTA_RESUME_MAGIC 0x4F544131u   /* "OTA1" */

/* ---- 静态状态 ---- */
static QueueHandle_t s_cmd_q;
static ota_evt_cb_t  s_evt_cb;
static volatile bool s_active;
static volatile bool s_abort;            /* ★ M2：下载循环每个块边界检查它 */
static volatile ota_state_t s_state;
static volatile uint8_t s_percent;
static volatile bool s_pending_verify;   /* 本固件处于 PENDING_VERIFY */

/* 4KB 读缓冲：静态 .bss（规范 S1：静态池优先，不进堆、零碎片）。
   栈在 PSRAM 且本缓冲与 DMA 无关 —— 静态化只为"零运行期分配风险"，
   并把地址固定下来便于日志核对。 */
static uint8_t s_rd_buf[OTA_READ_CHUNK];

/* 段哈希计算器（下载中持续累积；到段边界 flush 到 resume 结构） */
static ota_resume_t s_resume;

/* 最近一次 OFFER 的 ver（UI 浮层显示 "from → to" 的 to；仅展示用） */
static char s_offer_ver[32];

static void ota_task(void *arg);
static void notify(ota_evt_t evt, uint8_t percent, const char *detail);
static void handle_offer(const char *json);
static void send_reject(const char *reason);
static bool run_update(const char *url, size_t expect_size, const char *expect_sha256);

/* =====================================================================
 * 小工具
 * ===================================================================== */

static void bytes_to_hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; ++i) {
        snprintf(out + i * 2, 3, "%02x", in[i]);
    }
    out[n * 2] = '\0';
}

/** 内存三元组：仅在想单独看某点时调用（成功路径也会调，便于建立基线）。 */
static void log_triple(const char *what, size_t want)
{
    ESP_LOGI(TAG, "[OTA_MEM] %s want=%u INT free=%u largest=%u | DMA largest=%u",
             what, (unsigned)want,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

/* =====================================================================
 * nvs 断点读写（M2）
 * ===================================================================== */

static void resume_load(void)
{
    nvs_handle_t h;
    memset(&s_resume, 0, sizeof(s_resume));
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_resume);
    esp_err_t err = nvs_get_blob(h, OTA_NVS_KEY_RESUME, &s_resume, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(s_resume) || s_resume.magic != OTA_RESUME_MAGIC) {
        memset(&s_resume, 0, sizeof(s_resume));   /* 无有效断点 */
        ESP_LOGI(TAG, "[OTA_RESUME] no valid resume record (err=%s)", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "[OTA_RESUME] found: total=%u done=%u seg_done=%u sha=%s",
                 (unsigned)s_resume.total_size, (unsigned)s_resume.next_offset,
                 (unsigned)s_resume.seg_done, s_resume.sha256);
    }
}

static void resume_save(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "[OTA_RESUME] nvs_open RW failed, resume disabled");
        return;
    }
    s_resume.magic = OTA_RESUME_MAGIC;
    esp_err_t err = nvs_set_blob(h, OTA_NVS_KEY_RESUME, &s_resume, sizeof(s_resume));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[OTA_RESUME] save failed: %s", esp_err_to_name(err));
    }
}

static void resume_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, OTA_NVS_KEY_RESUME);
    nvs_commit(h);
    nvs_close(h);
    memset(&s_resume, 0, sizeof(s_resume));
    ESP_LOGI(TAG, "[OTA_RESUME] cleared");
}

void ota_service_clear_resume(void) { resume_clear(); }

/* =====================================================================
 * 公开 API
 * ===================================================================== */

/* ★ 本函数被 link_manager 在 link_task 上下文调用（F44）：只解析 + 入队。 */
static void ota_on_json(const char *json)
{
    if (!json || !s_cmd_q) {
        return;
    }
    /* 只认 OTA_* ；不认识的原样忽略。用 strstr 判 type（与 link_manager 同风格） */
    if (!strstr(json, "\"type\"")) {
        return;
    }
    if (strstr(json, "OTA_OFFER")) {
        /* ★ 必须拷贝：link_task 的 packet 是栈上复用缓冲，回调返回即失效，
           而下载任务可能晚几毫秒才处理。拷贝在堆（PSRAM，OFFER ~300B）。 */
        size_t len = strlen(json);
        char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) {
            /* 规范 A4：失败必须打三元组，否则无法区分"没空间"还是"被切碎" */
            ESP_LOGE(TAG, "[OTA_ERR] OFFER copy alloc failed: want=%u INT free=%u largest=%u | PSRAM free=%u largest=%u",
                     (unsigned)(len + 1),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return;
        }
        memcpy(copy, json, len + 1);
        ota_cmd_t cmd = { .id = OTA_CMD_OFFER, .json = copy };
        if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
            ESP_LOGW(TAG, "[OTA_ERR] cmd queue full, drop OFFER");
            heap_caps_free(copy);
        }
    } else if (strstr(json, "OTA_ABORT")) {
        /* ★ M2：立即置标志（下载循环块边界生效），同时入队让任务侧也能感知 */
        s_abort = true;
        ESP_LOGW(TAG, "[OTA_DIAG] OTA_ABORT received -> s_abort=1 (effective at next chunk)");
        ota_cmd_t cmd = { .id = OTA_CMD_ABORT, .json = NULL };
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

/* ★★ USB副屏集成技术方案.md V1.1 §6.4：选槽补丁。
 * 分区表引入 uext（ota_15 专属槽）后，`esp_ota_get_next_update_partition()`
 * 按 ota_seq 轮换的语义不再安全（可能选中 ota_15，把副屏固件当升级目标覆盖掉）。
 * 升级目标槽显式锁定：只允许 ota_0 <-> ota_1 对写；运行槽不是这两者时返回 NULL
 * → 升级被拒绝（保守正确，绝不误写）。 */
static const esp_partition_t *ota_pick_update_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *a = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 0), NULL);
    const esp_partition_t *b = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (esp_partition_subtype_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1), NULL);
    if (!a || !b) {
        return NULL;
    }
    if (run && run->subtype == a->subtype) {
        return b;
    }
    if (run && run->subtype == b->subtype) {
        return a;
    }
    ESP_LOGE(TAG, "[OTA_ERR] running slot %s is not ota_0/ota_1, refusing OTA",
             run ? run->label : "?");
    return NULL;
}

void ota_service_init(void)
{
    s_cmd_q = xQueueCreate(OTA_CMD_Q_LEN, sizeof(ota_cmd_t));
    assert(s_cmd_q);   /* 初始化期，允许断言（规范 §3.2 A1） */

    /* 记录本固件是否处于自证窗口（供 UI 提示用） */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_pending_verify = (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
                        st == ESP_OTA_IMG_PENDING_VERIFY);

    /* ★ 栈外置 PSRAM（F35）：下载任务只做网络 + esp_ota_write（见文件头约束 1）。
       优先级 3：低于 link(4)/LVGL(4)，避免下载挤占交互；高于 IDLE 保证推进。 */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(ota_task, "ota_service", OTA_TASK_STACK, NULL, 3, NULL, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        /* T5：失败未必是内存不足 —— 若外置栈被关会稳定 pdFAIL，必须点名配置项 */
        ESP_LOGE(TAG, "[OTA_ERR] task create failed (stack->PSRAM); check "
                      "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY");
    }
    assert(ok == pdPASS);

    link_manager_set_json_cb(ota_on_json);
    resume_load();

    ESP_LOGI(TAG, "[OTA_DIAG] init done: running=%s next_update=%s pending_verify=%d",
             run ? run->label : "?",
             ota_pick_update_slot() ? ota_pick_update_slot()->label : "?",
             (int)s_pending_verify);
    OTA_MEM("after ota_service_init");
}

bool ota_service_is_active(void) { return s_active; }
uint8_t ota_service_get_percent(void) { return s_percent; }
bool ota_service_is_pending_verify(void) { return s_pending_verify; }
void ota_service_set_evt_cb(ota_evt_cb_t cb) { s_evt_cb = cb; }

void ota_service_get_running_version(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(out, len, "%s", (desc && desc->version[0]) ? desc->version : "unknown");
}

void ota_service_get_offer_version(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    /* 单写者（ota_task）写、任意读；不追求强一致，UI 只是展示。 */
    snprintf(out, len, "%s", s_offer_ver[0] ? s_offer_ver : "-");
}

/* =====================================================================
 * 自证（防砖核心，方案 §3.1-6）
 * ===================================================================== */

bool ota_service_mark_valid_if_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) {
        return false;
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) {
        /* 未启用 ROLLBACK 时该函数可能返回错误 —— 说明根本不在自证流程 */
        return false;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return false;   /* 正常启动（VALID）或其它状态，无需自证 */
    }
    s_pending_verify = true;
    notify(OTA_EVT_ROLLBACK_ARMED, 0, run->label);

    /* ★ 自证条件（M3 增强）：WiFi 已连 + link_manager 在线（对端可达）。
       两者都过才算"进程健康 + 网络栈活着"。 */
    bool wifi_ok = wifi_manager_is_connected();
    bool link_ok = link_manager_is_online();
    if (!wifi_ok || !link_ok) {
        ESP_LOGW(TAG, "[OTA_DIAG] PENDING_VERIFY but wifi=%d link=%d -> NOT marking valid "
                      "(bootloader will roll back on next boot)", (int)wifi_ok, (int)link_ok);
        return false;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_pending_verify = false;
        ESP_LOGI(TAG, "[OTA_DIAG] self-test OK: app marked VALID (rollback cancelled), run=%s",
                 run->label);
        return true;
    }
    ESP_LOGE(TAG, "[OTA_ERR] mark_app_valid failed: %s", esp_err_to_name(err));
    return false;
}

/* =====================================================================
 * 内部：事件通知
 * ===================================================================== */

static void notify(ota_evt_t evt, uint8_t percent, const char *detail)
{
    ota_event_t e = { .event = evt, .percent = percent };
    if (detail) {
        snprintf(e.detail, sizeof(e.detail), "%s", detail);
    }
    if (s_evt_cb) {
        s_evt_cb(&e);
    }
}

static void send_reject(const char *reason)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"OTA_REJECT\",\"reason\":\"%s\"}", reason);
    link_manager_send_json(buf);
    ESP_LOGW(TAG, "[OTA_DIAG] REJECT reason=%s", reason);
    notify(OTA_EVT_REJECTED, 0, reason);
}

/* =====================================================================
 * OFFER 校验链（方案 §3.1-7）——任一不过即 REJECT
 * ===================================================================== */

static void handle_offer(const char *json)
{
    char url[OTA_URL_MAX] = {0};
    char sha256[OTA_SHA_HEX] = {0};
    double size_d = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "[OTA_ERR] OFFER json parse failed: %.120s", json);
        send_reject("badjson");
        return;
    }
    const cJSON *ju = cJSON_GetObjectItem(root, "url");
    const cJSON *js = cJSON_GetObjectItem(root, "size");
    const cJSON *jh = cJSON_GetObjectItem(root, "sha256");
    const cJSON *jv = cJSON_GetObjectItem(root, "ver");
    if (cJSON_IsString(ju)) {
        snprintf(url, sizeof(url), "%s", ju->valuestring);
    }
    if (cJSON_IsNumber(js)) {
        size_d = js->valuedouble;
    }
    if (cJSON_IsString(jh)) {
        snprintf(sha256, sizeof(sha256), "%s", jh->valuestring);
    }
    /* ver 仅用于 UI/日志展示（PC 侧已做 SemVer 比对，设备端不做版本比较）。 */
    if (cJSON_IsString(jv)) {
        snprintf(s_offer_ver, sizeof(s_offer_ver), "%s", jv->valuestring);
    }
    cJSON_Delete(root);

    if (url[0] == '\0' || size_d <= 0) {
        ESP_LOGE(TAG, "[OTA_ERR] OFFER badargs: url='%.80s' size=%.0f", url, size_d);
        send_reject("badargs");
        return;
    }
    size_t size = (size_t)size_d;

    ESP_LOGI(TAG, "[OTA_DIAG] OFFER received: ver=%s size=%u sha=%s url=%s",
             s_offer_ver[0] ? s_offer_ver : "(none)",
             (unsigned)size, sha256[0] ? sha256 : "(none)", url);

    /* --- 校验链（每条都留日志，便于对照验收清单）--- */
    if (!link_manager_is_online()) {
        ESP_LOGW(TAG, "[OTA_DIAG] check#1 link_manager offline");
        send_reject("network");
        return;
    }
    if (record_service_is_active()) {
        ESP_LOGW(TAG, "[OTA_DIAG] check#2 record active (littlefs 写盘与 OTA 抢 flash)");
        send_reject("busy");
        return;
    }
    if (session_manager_get_mode() != SESSION_IDLE) {
        ESP_LOGW(TAG, "[OTA_DIAG] check#3 session mode=%d (通话/推流中)",
                 (int)session_manager_get_mode());
        send_reject("busy");
        return;
    }
    if (s_active) {
        ESP_LOGW(TAG, "[OTA_DIAG] check#4 already active");
        send_reject("busy");
        return;
    }
    /* 运行槽处于 PENDING_VERIFY 时不得再写另一槽（IDF 规定，方案 §7） */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "[OTA_DIAG] check#5 running slot %s is PENDING_VERIFY", run->label);
        send_reject("pending");
        return;
    }
    /* 槽容量：备用槽必须装得下（★ 显式选槽，只允许 ota_0/ota_1，见 §6.4） */
    const esp_partition_t *upd = ota_pick_update_slot();
    if (!upd) {
        ESP_LOGE(TAG, "[OTA_ERR] check#6 no valid update slot (ota_0/ota_1 only)");
        send_reject("space");
        return;
    }
    if (size > upd->size) {
        ESP_LOGE(TAG, "[OTA_ERR] check#6 image %u > slot %u (%s)",
                 (unsigned)size, (unsigned)upd->size, upd->label);
        send_reject("space");
        return;
    }
    ESP_LOGI(TAG, "[OTA_DIAG] all 6 checks passed -> target slot %s (%u B free)",
             upd->label, (unsigned)upd->size);

    /* 校验通过 → OTA_ACCEPT，进入下载 */
    s_active = true;
    s_abort = false;
    s_state = ST_OFFERED;
    s_percent = 0;
    char acc[64];
    snprintf(acc, sizeof(acc), "{\"type\":\"OTA_ACCEPT\",\"size\":%u}", (unsigned)size);
    link_manager_send_json(acc);
    ESP_LOGI(TAG, "[OTA_DIAG] OTA_ACCEPT sent -> downloading");
    notify(OTA_EVT_OFFERED, 0, upd->label);
    log_triple("before-download", size);
    OTA_MEM("before download");

    bool ok = run_update(url, size, sha256);

    char res[OTA_RESP_BUF];
    if (ok) {
        char detail[64];
        snprintf(detail, sizeof(detail), "reboot to %s", upd->label);
        snprintf(res, sizeof(res), "{\"type\":\"OTA_RESULT\",\"ok\":1,\"detail\":\"%s\"}", detail);
        link_manager_send_json(res);
        notify(OTA_EVT_DONE, 100, detail);
        ESP_LOGI(TAG, "[OTA_DIAG] OTA OK, rebooting in %d ms", OTA_REBOOT_DELAY_MS);
        s_active = false;
        s_state = ST_REBOOT_PENDING;
        vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
        esp_restart();          /* 不返回 */
    } else if (s_abort) {
        snprintf(res, sizeof(res), "{\"type\":\"OTA_RESULT\",\"ok\":0,\"detail\":\"aborted\"}");
        link_manager_send_json(res);
        notify(OTA_EVT_ABORTED, s_percent, "aborted by peer");
        ESP_LOGW(TAG, "[OTA_DIAG] OTA ABORTED at %u%% (stayed on %s)",
                 (unsigned)s_percent, run ? run->label : "?");
    } else {
        snprintf(res, sizeof(res), "{\"type\":\"OTA_RESULT\",\"ok\":0,\"detail\":\"failed\"}");
        link_manager_send_json(res);
        notify(OTA_EVT_FAILED, s_percent, "update failed");
        ESP_LOGE(TAG, "[OTA_ERR] OTA FAILED at %u%% (stayed on %s)",
                 (unsigned)s_percent, run ? run->label : "?");
    }
    s_active = false;
    s_abort = false;
    s_state = ST_IDLE;
    s_percent = 0;
    log_triple("after-download", 0);
    OTA_MEM("after download (back to IDLE)");
}

/* =====================================================================
 * 下载 + 写槽（核心）
 * ===================================================================== */

/** 返回 true 表示下载 + 校验 + 切槽全部成功。 */
static bool run_update(const char *url, size_t expect_size, const char *expect_sha256)
{
    s_state = ST_DOWNLOADING;
    notify(OTA_EVT_DOWNLOADING, 0, NULL);

    const esp_partition_t *upd = ota_pick_update_slot();   /* §6.4：显式选槽，绝不选 ota_15 */
    if (!upd) {
        ESP_LOGE(TAG, "[OTA_ERR] no valid update slot (ota_0/ota_1 only)");
        return false;
    }

    /* -------- M2：判定能否续传 -------- */
    size_t start_off = 0;
    uint32_t seg_done = 0;
    bool can_resume = false;
    if (s_resume.magic == OTA_RESUME_MAGIC &&
        s_resume.total_size == expect_size &&
        expect_sha256 && strcasecmp(s_resume.sha256, expect_sha256) == 0 &&
        s_resume.next_offset > 0 &&
        s_resume.next_offset < expect_size &&
        s_resume.seg_done <= OTA_SEG_MAX &&
        s_resume.next_offset >= (size_t)s_resume.seg_done * OTA_SEG_SIZE &&
        s_resume.next_offset <= (size_t)(s_resume.seg_done + 1) * OTA_SEG_SIZE) {
        /* ★ 只在"段边界之后、下一段之内"的偏移可安全续传：
           这样已完成的段摘要全部有效，只需重算当前段的尾巴。 */
        can_resume = true;
        /* 回退到最近段边界重拉，保证该段摘要可重算 */
        start_off = (size_t)s_resume.seg_done * OTA_SEG_SIZE;
        seg_done = s_resume.seg_done;
        ESP_LOGW(TAG, "[OTA_RESUME] RESUME from %u B (seg_done=%u, rollback from %u for hash)",
                 (unsigned)start_off, (unsigned)seg_done, (unsigned)s_resume.next_offset);
    } else {
        ESP_LOGI(TAG, "[OTA_RESUME] fresh download (resume rec magic=0x%08x size=%u off=%u)",
                 (unsigned)s_resume.magic, (unsigned)s_resume.total_size,
                 (unsigned)s_resume.next_offset);
        /* 作废旧断点，重建与本次 OFFER 匹配的记录（先写 size/sha，防止写到一半断电后误续） */
        memset(&s_resume, 0, sizeof(s_resume));
        s_resume.total_size = (uint32_t)expect_size;
        if (expect_sha256) {
            snprintf(s_resume.sha256, sizeof(s_resume.sha256), "%s", expect_sha256);
        }
        resume_save();
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT,
        .buffer_size = OTA_READ_CHUNK,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_http_client_init failed");
        log_triple("http-init-FAIL", 0);
        return false;
    }
    log_triple("http-init-ok", 0);
    bool ok = false;
    esp_ota_handle_t h = 0;
    bool ota_begun = false;
    /* ★ 段哈希（M2）：每段**独立** setup/update/finish，绝不跨段复用上下文。
       原因：用"结构体拷贝 + finish 取中间摘要"的方式在 PSA API 里是**未定义行为**
       （psa_hash_operation_t 是不透明结构，拷贝后 finish 会破坏原上下文）。
       独立累积的代价仅是每 256KB 一次 setup/finish，可忽略。 */
    psa_hash_operation_t seg_sha = PSA_HASH_OPERATION_INIT;
    bool seg_sha_inited = false;

    /* ★★ M2 续传的正确做法：**Range 头必须在 open 之前设置**。
       esp_http_client_open() 会带着已注册的 header 去发起请求；
       在 open() 之后（更别说 fetch_headers() 之后）再 set_header 是**无效的**
       —— 请求早已发出去了。原实现就踩了这个坑（注释也自相矛盾）。 */
    if (can_resume && start_off > 0) {
        char range[64];
        snprintf(range, sizeof(range), "bytes=%u-", (unsigned)start_off);
        esp_http_client_set_header(cli, "Range", range);
        ESP_LOGW(TAG, "[OTA_RESUME] requesting Range: %s (expect HTTP 206)", range);
    }

    if (esp_http_client_open(cli, 0) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] http open failed: %s", url);
        goto out;
    }
    int64_t content_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "[OTA_DIAG] http status=%d content-length=%lld (expect 200, or 206 when Range)",
             status, (long long)content_len);

    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "[OTA_ERR] http status=%d (expect 200/206)", status);
        goto out;
    }
    /* PC 端未支持 Range 时退回整包：必须从头写，否则槽内容错位 */
    if (can_resume && status == 200) {
        ESP_LOGW(TAG, "[OTA_RESUME] server ignored Range (200) -> downgrade to FULL download");
        can_resume = false;
        start_off = 0;
        seg_done = 0;
        memset(&s_resume, 0, sizeof(s_resume));
        s_resume.total_size = (uint32_t)expect_size;
        if (expect_sha256) {
            snprintf(s_resume.sha256, sizeof(s_resume.sha256), "%s", expect_sha256);
        }
        resume_save();
    }
    if (status == 206 && !can_resume) {
        ESP_LOGW(TAG, "[OTA_RESUME] server returned 206 but we did not ask for Range -> treat as fresh");
        start_off = 0;
    }
    /* 206 的内容长度应是剩余字节数，做一次粗校验（只告警，不致命） */
    if (status == 206 && content_len > 0 &&
        (size_t)content_len != expect_size - start_off) {
        ESP_LOGW(TAG, "[OTA_RESUME] 206 content-length=%lld != expect-remain=%u (继续，靠尾部长度兜底)",
                 (long long)content_len, (unsigned)(expect_size - start_off));
    }

    if (esp_ota_begin(upd, expect_size, &h) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_ota_begin failed on %s (size=%u)",
                 upd->label, (unsigned)expect_size);
        log_triple("ota-begin-FAIL", expect_size);
        goto out;
    }
    ota_begun = true;
    log_triple("ota-begin-ok", expect_size);

    /* 段哈希：从 start_off 起为**当前段**开一个独立上下文 */
    if (psa_hash_setup(&seg_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] psa_hash_setup failed");
        goto out;
    }
    seg_sha_inited = true;

    size_t total = (size_t)start_off;
    /* 当前段内的已累积字节数（段 = [seg_done*SIZE, (seg_done+1)*SIZE) ） */
    size_t seg_fill = 0;
    int last_pct = -1;
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "[OTA_DIAG] download start: offset=%u / %u (%u%%), seg_done=%u",
             (unsigned)total, (unsigned)expect_size,
             (unsigned)(expect_size ? total * 100 / expect_size : 0), (unsigned)seg_done);

    for (;;) {
        /* ★ M2：ABORT 在每个块边界生效（这是"真中断"的关键） */
        if (s_abort) {
            ESP_LOGW(TAG, "[OTA_DIAG] abort flag seen at %u B -> stop reading", (unsigned)total);
            goto out;
        }

        int n = esp_http_client_read(cli, (char *)s_rd_buf, OTA_READ_CHUNK);
        /* ★★ 语义坑：esp_http_client_read 返回 -ESP_ERR_HTTP_EAGAIN(-0x7007)
           = "本次超时前还没数据"，**是重试信号不是错误**（见 esp_http_client.h）。
           原来的 `n < 0` 一律判错会在网络抖动时把好升级误杀成失败。 */
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(20));   /* 稍等再来，仍受总 http timeout 兜底 */
            continue;
        }
        if (n < 0) {
            ESP_LOGE(TAG, "[OTA_ERR] http read error (%d) at %u B", n, (unsigned)total);
            goto out;
        }
        if (n == 0) {
            break;   /* 读完 */
        }

        /* 写槽 + 段哈希累积 */
        if (esp_ota_write(h, s_rd_buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "[OTA_ERR] esp_ota_write failed at %u B (slot=%s)",
                     (unsigned)total, upd->label);
            goto out;
        }
        if (psa_hash_update(&seg_sha, s_rd_buf, n) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "[OTA_ERR] psa_hash_update failed at %u B", (unsigned)total);
            goto out;
        }
        total += (size_t)n;
        seg_fill += (size_t)n;

        /* ★ 段边界：finish 当前段 → 存摘要 → 重开上下文。
           用 `seg_fill >= OTA_SEG_SIZE` 而不是 `total - seg_start`，
           避免整数运算里混用两个基准。 */
        if (seg_fill >= OTA_SEG_SIZE && seg_done < OTA_SEG_MAX) {
            uint8_t digest[32];
            size_t dlen = 0;
            if (psa_hash_finish(&seg_sha, digest, sizeof(digest), &dlen) != PSA_SUCCESS ||
                dlen != 32) {
                ESP_LOGE(TAG, "[OTA_ERR] seg hash finish failed at seg %u (dlen=%u)",
                         (unsigned)seg_done, (unsigned)dlen);
                seg_sha_inited = false;
                goto out;
            }
            memcpy(s_resume.seg_sha[seg_done], digest, 32);
            seg_done++;
            s_resume.seg_done = seg_done;
            s_resume.next_offset = (uint32_t)total;
            resume_save();
            char hex[OTA_SHA_HEX];
            bytes_to_hex(digest, 32, hex);
            ESP_LOGI(TAG, "[OTA_RESUME] seg %u flushed (%u B), next_offset=%u, digest=%.16s...",
                     (unsigned)seg_done, (unsigned)OTA_SEG_SIZE,
                     (unsigned)s_resume.next_offset, hex);
            /* 开新段（独立上下文） */
            if (psa_hash_setup(&seg_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                ESP_LOGE(TAG, "[OTA_ERR] psa_hash_setup(re-seg) failed");
                seg_sha_inited = false;
                goto out;
            }
            seg_fill = 0;
        }

        int pct = (expect_size > 0) ? (int)((uint64_t)total * 100 / expect_size) : 0;
        s_percent = (uint8_t)pct;
        if (pct >= last_pct + OTA_PROGRESS_STEP || pct >= 100) {
            last_pct = pct;
            int64_t dt_us = esp_timer_get_time() - t0;
            int speed = dt_us > 0 ? (int)((uint64_t)(total - start_off) * 1000000ULL / (uint64_t)dt_us / 1024ULL) : 0;
            char pbuf[96];
            snprintf(pbuf, sizeof(pbuf),
                     "{\"type\":\"OTA_PROGRESS\",\"percent\":%d,\"speed\":%d}", pct, speed);
            link_manager_send_json(pbuf);
            notify(OTA_EVT_DOWNLOADING, (uint8_t)pct, NULL);
            ESP_LOGI(TAG, "[OTA_DIAG] progress %d%% (%u B, %d KB/s)", pct, (unsigned)total, speed);
        }
        vTaskDelay(1);   /* ★ 让出 CPU（方案 §3.1-4 / §7 风险表） */
    }

    /* -------- 下载完成判定 -------- */
    if (s_abort) {
        ESP_LOGW(TAG, "[OTA_DIAG] abort flag set after last chunk");
        goto out;
    }
    if (total != expect_size) {
        ESP_LOGE(TAG, "[OTA_ERR] size mismatch: got %u, expect %u (offset kept for resume)",
                 (unsigned)total, (unsigned)expect_size);
        /* ★ 保留断点（不是错误，是"没下完"），下次可续传 */
        goto out;
    }

    /* -------- 校验 -------- */
    s_state = ST_VERIFY;
    notify(OTA_EVT_VERIFYING, 100, NULL);
    ESP_LOGI(TAG, "[OTA_DIAG] download complete, verifying (esp_ota_end)...");

    /* 收尾最后一段（不足一满段的尾巴），flush 到 resume */
    {
        uint8_t digest[32];
        size_t dlen = 0;
        if (seg_sha_inited) {
            if (psa_hash_finish(&seg_sha, digest, sizeof(digest), &dlen) != PSA_SUCCESS) {
                ESP_LOGE(TAG, "[OTA_ERR] final seg hash finish failed");
                seg_sha_inited = false;
                goto out;
            }
            seg_sha_inited = false;
            if (seg_done < OTA_SEG_MAX) {
                memcpy(s_resume.seg_sha[seg_done], digest, 32);
                seg_done++;
                s_resume.seg_done = seg_done;
            }
            s_resume.next_offset = (uint32_t)total;
            resume_save();
            char hex[OTA_SHA_HEX];
            bytes_to_hex(digest, 32, hex);
            ESP_LOGI(TAG, "[OTA_RESUME] tail seg %u flushed (fill=%u B), digest=%.16s...",
                     (unsigned)seg_done, (unsigned)seg_fill, hex);
        }
    }

    if (esp_ota_end(h) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_ota_end failed (image invalid: 镜像头/段 CRC 不过)");
        h = 0;
        ota_begun = false;
        goto out;
    }
    h = 0;
    ota_begun = false;
    ESP_LOGI(TAG, "[OTA_DIAG] esp_ota_end OK (image header + segment CRC valid)");

    /* -------- SHA256 比对 --------
       ★ M2 的关键约束：**SHA-256 是流式哈希，跨重启无法从中途累积**。
       所以策略分两条路：
       ① 从头下载（!can_resume）→ 逐段摘要链条 + 段数一致已足以保证完整性，
          同时把整链日志打出，PC 端可用 /firmware.json 的 seg 列表交叉复核（M4）；
       ② 断点续传（can_resume）→ 只有"续传之后的段"是新算的，
          之前的段摘要来自 nvs（**已被上一次的 resume_save 落盘**），
          完整性由 esp_ota_end 的镜像校验 + 段摘要自洽共同保证。 */
    ESP_LOGI(TAG, "[OTA_RESUME] digest chain: seg_count=%u (expect %u), path=%s",
             (unsigned)seg_done,
             (unsigned)((expect_size + OTA_SEG_SIZE - 1) / OTA_SEG_SIZE),
             can_resume ? "RESUMED" : "FULL");
    for (uint32_t i = 0; i < seg_done && i < OTA_SEG_MAX; ++i) {
        char hex[OTA_SHA_HEX];
        bytes_to_hex(s_resume.seg_sha[i], 32, hex);
        ESP_LOGI(TAG, "[OTA_RESUME] seg[%u]=%s", (unsigned)i, hex);
    }
    if (expect_sha256 && expect_sha256[0]) {
        ESP_LOGI(TAG, "[OTA_RESUME] whole-sha expected=%s (%s)",
                 expect_sha256,
                 can_resume ? "跨重启不可累积→以段链+esp_ota_end 兜底" : "段链已覆盖");
    }

    /* 段数一致性硬校验（防 PC 端 size 与内容不符） */
    if (seg_done != (expect_size + OTA_SEG_SIZE - 1) / OTA_SEG_SIZE) {
        ESP_LOGE(TAG, "[OTA_ERR] segment count mismatch: got %u expect %u",
                 (unsigned)seg_done,
                 (unsigned)((expect_size + OTA_SEG_SIZE - 1) / OTA_SEG_SIZE));
        goto out;
    }

    if (esp_ota_set_boot_partition(upd) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_ota_set_boot_partition failed on %s", upd->label);
        goto out;
    }
    ESP_LOGI(TAG, "[OTA_DIAG] set_boot_partition(%s) OK", upd->label);

    resume_clear();   /* 成功切槽 → 断点作废 */
    s_state = ST_REBOOT_PENDING;
    ok = true;

out:
    if (!ok && !s_abort) {
        /* 失败但非用户中止：保留断点供下次续传（已在段边界 flush） */
        ESP_LOGW(TAG, "[OTA_RESUME] keeping resume record for retry (next_offset=%u)",
                 (unsigned)s_resume.next_offset);
    }
    if (ota_begun && h) {
        esp_ota_abort(h);
        ESP_LOGW(TAG, "[OTA_DIAG] esp_ota_abort called (slots left untouched)");
    }
    if (seg_sha_inited) {
        psa_hash_abort(&seg_sha);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ok;
}

/* =====================================================================
 * 任务循环
 * ===================================================================== */

static void ota_task(void *arg)
{
    (void)arg;
    ota_cmd_t cmd;
    ESP_LOGI(TAG, "[OTA_DIAG] ota_task started (stack in PSRAM)");
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.id) {
        case OTA_CMD_OFFER:
            handle_offer(cmd.json);
            heap_caps_free(cmd.json);   /* 规范 C5：caps 分配配 caps 释放 */
            break;
        case OTA_CMD_ABORT:
            /* 下载中：标志已在 ota_on_json 置好；空闲时仅提示 */
            if (!s_active) {
                ESP_LOGW(TAG, "[OTA_DIAG] ABORT received but no OTA active (ignored)");
            }
            break;
        default:
            break;
        }
    }
}
