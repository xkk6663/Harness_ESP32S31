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
 *  1. 【F35/铁律·实机验证 2026-10-07】ota_task 栈**必须在内部 RAM**（用
 *     xTaskCreatePinnedToCore，栈由 pvPortMalloc 从 INTERNAL 拿）。
 *     曾把栈外置 PSRAM，实机一收 OFFER 就 assert：
 *     `esp_cache_freeze_caches_disable_interrupts esp_cache_utils.c:96
 *     (s_task_stack_is_sane_when_cache_frozen())` —— 该检查要求**当前任务
 *     SP 在内部 DRAM**，而 otadata 读 / esp_ota_begin 擦槽 / esp_ota_write
 *     全都会冻结 ext-mem cache。F35 的完整表述：
 *     凡做 flash IO（含 esp_ota_x / NVS / littlefs）的任务栈一律内部 RAM，
 *     "esp_ota_write 自行处理 cache 关窗"不豁免 SP 检查。
 *     另：nvs_set_blob 断点写同样跑在本任务 ⇒ 同受此约束，一并内栈。
 *
 *  2. 【F44】link_manager 的 JSON 回调跑在 link_task 上下文 ⇒
 *     ota_on_json() 只允许 cJSON 解析 + 命令入队，**禁止阻塞 IO**。
 *     真正的下载在 ota_task 里做。
 *
 *  3. 【§80 S1】esp_ota_write 会短暂关 cache；下载循环每 **64KB** 让出一次 CPU
 *     （原为每 4KB，tick=1000Hz 时 5MB 白等 1.2s 且吞吐被压到 4MB/s）。
 *     ABORT 仍在每块（8KB）边界检查，中止延迟远小于一个 staging。
 *
 *  4. 【§80 S2】整包 SHA-256 必须闭环：全新下载流式累积、续传回读 [0, expect_size)，
 *     在 esp_ota_set_boot_partition 之前比对，不符不切槽。
 *     回读**绝不能读到槽大小**（槽尾擦除态 0xFF 会让哈希永远对不上）。
 *
 *  5. 【§80 S3】OFFER 必须带 hmac（HMAC-SHA256 over 规范化串）；PSK 在 NVS
 *     "ota"/"psk"，开发期回落 OTA_DEV_PSK（与 pc_host/ota/auth.py 逐字一致）。
 *     nonce 严格递增并落 NVS，防重放。
 *
 *  6. 【内存规范 A4】所有 alloc 失败路径必须打 (want/free/largest) 三元组；
 *     运行期禁止 assert（A2）；本文件所有缓冲区优先静态池（S1）。
 *
 * ── 日志体系（可 grep）─────────────────────────────────────────────
 *   grep "OTA_DIAG"  → 分阶段进度 + 内存五元（排错主场）+ timing 三段耗时
 *   grep "OTA_ERR"   → 所有错误（含错误码与偏移）
 *   grep "OTA_MEM"   → 纯内存三元组打点
 *   grep "OTA_RESUME"→ 断点续传相关
 *   grep "OTA_AUTH"  → 鉴权（psk 来源 / nonce / 失败原因）
 * ========================================================================== */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>    /* strtol（ver_cmp 解析版本号数字段） */
#include <string.h>
#include <strings.h>   /* strcasecmp（SHA256 hex 大小写不敏感比对） */
#include <time.h>      /* time()：SNTP 校时后的 epoch 秒（ts 新鲜度） */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"   /* §72 真续传：esp_image_verify（与 esp_ota_end 同源校验） */
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
#include "middleware/network/net_time_service.h" /* SNTP 已校时判定（OFFER ts 新鲜度） */
#include "middleware/network/pcnow_service.h"   /* PCNOW_HTTP_PORT(=50005) 复用 */
#include "ota_service.h"

static const char *TAG = "OTA";

/* ---- 常量 ---- */
/* ★ §80 S1（2026-11-12）：读块 4KB → 8KB。
 * read syscall 次数减半；esp_http_client 的内部 buffer 与本值同尺寸（+4KB 堆），
 * s_rd_buf 由 4KB 变 8KB（+4KB .bss）。再往上一档（16KB）要先看 [OTA_MEM] largest。 */
#define OTA_READ_CHUNK      (8 * 1024)
#define OTA_WRITE_STAGE     (32 * 1024)                /* ★ 写槽攒块 32KB（§79f 回退：64KB 让单次 cache 断供翻倍，条纹更长更扎眼） */
/* ★ §80 S1：让出粒度 4KB → 64KB。
 * 原来每读 4KB 就 vTaskDelay(1)；CONFIG_FREERTOS_HZ=1000 ⇒ 5MB 要停 1220 次、
 * 白等 ~1.2s，还把吞吐理论上限压到 4MB/s。改成每 64KB 让出一次（8×8KB），
 * 中断延迟（ABORT/abort 检查）最多 8KB，仍远小于一个 staging。 */
#define OTA_YIELD_BYTES     (64 * 1024)
#define OTA_TASK_STACK      8192
#define OTA_PROGRESS_STEP   2                          /* 每 2% 回一条 PROGRESS（§79f 回退：5% 步进让进度条"跳变"，观感变慢） */
#define OTA_HTTP_TIMEOUT    15000                      /* 15s：局域网 4.9MB 远够 */
#define OTA_REBOOT_DELAY_MS 1000
#define OTA_CMD_Q_LEN       4
#define OTA_RESP_BUF        192                        /* 回包 JSON 缓冲 */
#define OTA_URL_MAX         192
#define OTA_SHA_HEX         65                         /* 32B → 64 hex + NUL */
#define OTA_HMAC_HEX        65                         /* HMAC-SHA256 32B → 64 hex + NUL */
#define OTA_CANON_MAX       512                        /* 规范化签名串缓冲 */

/* ---- M2：分段哈希续传 ---- */
#define OTA_SEG_SIZE        (256 * 1024)               /* 锚点段大小 256KB → 5MB 约 20 段 */
#define OTA_SEG_MAX         24                         /* 支持到 6MB（本槽 5MB，留余量） */
#define OTA_NVS_NS          "ota"
#define OTA_NVS_KEY_RESUME  "resume"

/* ---- §80 S3：OFFER 鉴权（HMAC-SHA256 + nonce 防重放 + ts 新鲜度）---- */
#define OTA_NVS_KEY_PSK     "psk"                      /* blob，任意长度（推荐 32B） */
#define OTA_NVS_KEY_NONCE   "nonce"                    /* u64，最近一次通过鉴权的 nonce */
#define OTA_AUTH_SKEW_S     300                        /* ts 与设备时钟允许的最大偏差（秒） */
/* 开发期兜底 PSK：与 PC 端 pc_host/ota/auth.py 的 DEFAULT_PSK **必须逐字一致**
 * （tests/unit/test_ota_auth.py 会直接读本文件做一致性断言，改一处漏另一处会红）。
 * 量产应改为产线写 NVS "ota"/"psk"（每台独立），本宏仅用于联调/开发板。 */
#ifndef OTA_DEV_PSK
#define OTA_DEV_PSK         "harness-dev-psk-2026-11"
#endif

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

/* ---- §80：一次 OFFER 解析后的全部字段（鉴权/门禁都在这个结构上判）---- */
typedef struct {
    char     ver[32];                  /* 目标版本 */
    char     min_ver[32];              /* 最低可接受版本（可空） */
    int      force;                    /* 1=允许降级 */
    long long size;                    /* 镜像字节数 */
    char     sha256[OTA_SHA_HEX];      /* 整包 SHA256（hex，**必需**） */
    char     url[OTA_URL_MAX];
    uint64_t nonce;                    /* 严格递增（防重放） */
    uint64_t ts;                       /* PC 侧 epoch 秒 */
    char     hmac[OTA_HMAC_HEX];       /* 规范化串的 HMAC-SHA256（hex） */
} ota_offer_t;

/* ---- 静态状态 ---- */
static QueueHandle_t s_cmd_q;
static ota_evt_cb_t  s_evt_cb;
static volatile bool s_active;
static volatile bool s_abort;            /* ★ M2：下载循环每个块边界检查它 */
static volatile ota_state_t s_state;
static volatile uint8_t s_percent;
static volatile bool s_pending_verify;   /* 本固件处于 PENDING_VERIFY */
static bool s_rb_notified;               /* ROLLBACK_ARMED 只通知一次（见 mark_valid_if_pending） */

/* 4KB 读缓冲：静态 .bss（规范 S1：静态池优先，不进堆、零碎片）。
   栈在 PSRAM 且本缓冲与 DMA 无关 —— 静态化只为"零运行期分配风险"，
   并把地址固定下来便于日志核对。 */
static uint8_t s_rd_buf[OTA_READ_CHUNK];

/* ★ 写槽 staging（PSRAM，init 时分配）：读 4KB/次、写 32KB/次。
 * esp_ota_write 每次调用都冻结 ext-mem cache → PSRAM 帧缓冲的 GDMA 扫描被饿
 * → 屏闪 + 可能残留几行向下偏移。攒块把冻结频率从 ~1270 次/包降到 ~160 次/包。
 * ★ §79f 定论：总断供时长 ≈ 固件大小 ÷ flash 写速，是常数 —— 攒块只改
 * "每次冻结的长度"；64KB 实测让条纹更长更扎眼（单次断供翻倍），回退 32KB。
 * 分配失败退化为 4KB 直写（只影响闪烁观感，不影响正确性）。 */
static uint8_t *s_wr_stage;

/* 段哈希计算器（下载中持续累积；到段边界 flush 到 resume 结构） */
static ota_resume_t s_resume;

/* 最近一次 OFFER 的 ver（UI 浮层显示 "from → to" 的 to；仅展示用） */
static char s_offer_ver[32];

/* ---- §79 双向 OTA：设备端发起升级的"查新"状态 ----
 * 状态值宏（OTA_CHECK_*）在 ota_service.h 定义。写侧：link_task（ota_on_json）
 * + UI 线程（request_update）；读侧 LVGL 定时器。uint8_t 单字节读写原子，够用。 */
#define OTA_CHECK_TIMEOUT_US (10LL * 1000000LL)   /* 10s 无应答即超时 */
static volatile uint8_t s_check_state;
static int64_t s_check_sent_us;

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

/** hex（64 字符）→ 32B 二进制。长度不足/含非 hex 字符一律 false（不做部分解析）。 */
static bool hex2bin(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || !out || strlen(hex) < n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i * 2 + k];
            int d;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10;
            } else {
                return false;          /* 空格/0x/其它一律拒，别让 strtol 宽松放行 */
            }
            v = (v << 4) | d;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

/** SemVer 粗比：<0 / 0 / >0。规则与 pc_host/ota/session.py:compare_semver 一致 ——
 *  容忍 v/V 前缀、-dirty/+build 后缀、缺位补 0、非法段按 0 处理。
 *  ★ 只用于"要不要升/是不是降级"的粗判，不参与完整性判定（那是 SHA256 的事）。 */
static void ver_parse(const char *s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!s) {
        return;
    }
    while (*s == ' ') {
        ++s;
    }
    if (*s == 'v' || *s == 'V') {
        ++s;
    }
    for (int i = 0; i < 3; ++i) {
        if (*s < '0' || *s > '9') {
            break;                       /* 该段非数字（如 "1.x.0"）→ 留 0 并停 */
        }
        char *end = NULL;
        long v = strtol(s, &end, 10);
        out[i] = (int)v;
        s = end;
        if (*s != '.') {
            break;                       /* 段数不足或遇到 -dirty / +build */
        }
        ++s;
    }
}

static int ver_cmp(const char *a, const char *b)
{
    int x[3], y[3];
    ver_parse(a, x);
    ver_parse(b, y);
    for (int i = 0; i < 3; ++i) {
        if (x[i] != y[i]) {
            return x[i] - y[i];
        }
    }
    return 0;
}

/* =====================================================================
 * §72 真续传（esp_partition 裸写 + 自管擦除）专用
 * ===================================================================== */

/* 裸写路径收尾校验：与 esp_ota_end **完全同源**（esp_ota_ops.c:587
 * ota_verify_partition 里就是这一句）——镜像头 + 段 CRC 全量校验。
 * 纯校验不持有 ota handle，回滚状态机在 otadata，不受影响。 */
static bool verify_image_on_partition(const esp_partition_t *p)
{
    esp_image_metadata_t data;
    const esp_partition_pos_t pos = {
        .offset = p->address,
        .size   = p->size,
    };
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &data) != ESP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "[OTA_DIAG] image verify OK (esp_image_verify, bare-write path)");
    return true;
}

/* 续传前回读校验：槽里已写的前 seg_done 个 256KB 段必须与 nvs 段摘要一致。
 * 这是裸写路径的"旧前缀完整性证明"——nvs 只证明"当时收到过什么"，
 * 回读才证明"flash 里现在是什么"。4KB 分块读进 s_rd_buf（此刻 http 还没开，无冲突）。
 * @return true=全部一致；false=不一致（调用方必须清断点改走全新下载）。 */
static bool verify_written_segments(const esp_partition_t *p, uint32_t seg_done)
{
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    for (uint32_t i = 0; i < seg_done; ++i) {
        size_t remain = OTA_SEG_SIZE;
        size_t off = (size_t)i * OTA_SEG_SIZE;
        if (psa_hash_setup(&h, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "[OTA_ERR] verify seg %u: hash_setup failed", (unsigned)i);
            return false;
        }
        while (remain) {
            size_t want = remain < OTA_READ_CHUNK ? remain : OTA_READ_CHUNK;
            if (esp_partition_read(p, off, s_rd_buf, want) != ESP_OK) {
                ESP_LOGE(TAG, "[OTA_ERR] verify seg %u: read failed at %u", (unsigned)i, (unsigned)off);
                psa_hash_abort(&h);
                return false;
            }
            if (psa_hash_update(&h, s_rd_buf, want) != PSA_SUCCESS) {
                ESP_LOGE(TAG, "[OTA_ERR] verify seg %u: hash_update failed", (unsigned)i);
                psa_hash_abort(&h);
                return false;
            }
            off += want;
            remain -= want;
        }
        uint8_t digest[32];
        size_t dlen = 0;
        if (psa_hash_finish(&h, digest, sizeof(digest), &dlen) != PSA_SUCCESS || dlen != 32 ||
            memcmp(digest, s_resume.seg_sha[i], 32) != 0) {
            ESP_LOGE(TAG, "[OTA_ERR] verify seg %u: digest mismatch (flash != nvs) -> resume record stale", (unsigned)i);
            return false;
        }
        ESP_LOGI(TAG, "[OTA_RESUME] verify seg %u/%u OK", (unsigned)(i + 1), (unsigned)seg_done);
    }
    return true;
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
    } else if (s_resume.total_size == 0 || s_resume.next_offset >= s_resume.total_size) {
        /* ★ 断点已"下满"却没能切槽（上次死在尾部校验）——这种记录没有任何可续内容，
           留着只会让之后每次 OFFER 都走 0 字节续传再撞同一个校验，永久自锁。
           2026-11-12 真机验收就落到过这个状态（末段计数 bug 把一次完整下载判死）。 */
        ESP_LOGW(TAG, "[OTA_RESUME] stale record done=%u/total=%u (nothing left) -> discard",
                 (unsigned)s_resume.next_offset, (unsigned)s_resume.total_size);
        memset(&s_resume, 0, sizeof(s_resume));
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
 * §80 S3：OFFER 鉴权（HMAC-SHA256 + nonce 防重放 + ts 新鲜度）
 *
 * 为什么不是"链路加密"：局域网内真正的威胁不是被监听，而是**谁能发 OFFER**
 * —— 原来六道检查全是状态检查，同网段任意主机给个 url 就能让设备刷固件。
 * HMAC 把门槛从"同网段任何人"抬到"知道 PSK 的人"（PSK 在 NVS，未开 Flash
 * Encryption 时能被物理读出 —— 这是本档位的诚实边界，根治靠量产 Secure Boot）。
 * ===================================================================== */

/** 取 PSK：NVS "ota"/"psk" 优先，缺省回落到编译期 OTA_DEV_PSK（仅开发用）。
 *  @return PSK 字节数；0 = 两处都没有（此时必须拒绝升级，不能"放行兜底"）。 */
static size_t ota_psk_load(uint8_t *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        esp_err_t err = nvs_get_blob(h, OTA_NVS_KEY_PSK, out, &len);
        nvs_close(h);
        if (err == ESP_OK && len > 0) {
            ESP_LOGI(TAG, "[OTA_AUTH] psk from nvs (%u B)", (unsigned)len);
            return len;
        }
    }
    size_t dl = strlen(OTA_DEV_PSK);
    if (dl == 0 || dl > cap) {
        return 0;
    }
    memcpy(out, OTA_DEV_PSK, dl);
    ESP_LOGW(TAG, "[OTA_AUTH] psk = build-time OTA_DEV_PSK (%u B) —— 仅开发期，量产须写 NVS",
             (unsigned)dl);
    return dl;
}

static uint64_t last_nonce_load(void)
{
    nvs_handle_t h;
    uint64_t v = 0;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u64(h, OTA_NVS_KEY_NONCE, &v) != ESP_OK) {
            v = 0;
        }
        nvs_close(h);
    }
    return v;
}

static void last_nonce_save(uint64_t v)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "[OTA_AUTH] nvs_open RW failed, nonce not persisted");
        return;
    }
    esp_err_t err = nvs_set_u64(h, OTA_NVS_KEY_NONCE, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[OTA_AUTH] nonce save failed: %s", esp_err_to_name(err));
    }
}

/** 规范化串：**双方必须一字不差**（ICD §8.1 已固化为字面量）。
 *  顺序：ver|min_version|force|size|sha256|url|nonce|ts
 *  force 由 cJSON 布尔归一成 0/1；size/nonce/ts 一律十进制无符号整数。 */
static int build_canon(const ota_offer_t *o, char *out, size_t cap)
{
    return snprintf(out, cap, "%s|%s|%d|%lld|%s|%s|%llu|%llu",
                    o->ver, o->min_ver, o->force, o->size, o->sha256, o->url,
                    (unsigned long long)o->nonce, (unsigned long long)o->ts);
}

/** 鉴权全链。@return NULL = 通过；非 NULL = REJECT reason（调用方直接回给 PC）。
 *  顺序刻意是 HMAC 优先：先证明字段没被改，再拿 nonce/ts 做状态判断 ——
 *  否则伪造包可以用一个巨大 nonce 把 NVS 计数顶上去。 */
static const char *ota_auth_check(const ota_offer_t *o)
{
    if (!o->hmac[0]) {
        ESP_LOGE(TAG, "[OTA_ERR] OFFER without hmac (ICD §8.1: auth is mandatory)");
        return "noauth";
    }
    uint8_t psk[64];
    size_t psk_len = ota_psk_load(psk, sizeof(psk));
    if (psk_len == 0) {
        ESP_LOGE(TAG, "[OTA_ERR] no PSK provisioned (nvs ota/psk empty) -> refuse");
        return "nopsk";
    }
    uint8_t mac[32];
    if (!hex2bin(o->hmac, mac, sizeof(mac))) {
        ESP_LOGE(TAG, "[OTA_ERR] hmac hex malformed (len=%u)", (unsigned)strlen(o->hmac));
        return "badauth";
    }
    char canon[OTA_CANON_MAX];
    int n = build_canon(o, canon, sizeof(canon));
    if (n <= 0 || n >= (int)sizeof(canon)) {
        ESP_LOGE(TAG, "[OTA_ERR] canon overflow (%d B), OFFER too long", n);
        return "badauth";
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, psk_len * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t key = 0;
    psa_status_t st = psa_import_key(&attr, psk, psk_len, &key);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] psa_import_key failed: %d", (int)st);
        return "nopsk";
    }
    st = psa_mac_verify(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                        (const uint8_t *)canon, strlen(canon), mac, sizeof(mac));
    psa_destroy_key(key);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] hmac verify failed (st=%d) -> REJECT badauth", (int)st);
        ESP_LOGD(TAG, "[OTA_AUTH] canon=\"%s\"", canon);
        return "badauth";
    }

    /* 时间新鲜度：只在 SNTP 已校时的情况下判 —— 无外网设备 time() 是 1970，
       若照判会把"没网但有局域网 PC"的正常升级全拒掉。 */
    if (o->ts > 0 && net_time_service_time_valid()) {
        long long now = (long long)time(NULL);
        long long skew = now - (long long)o->ts;
        if (skew > OTA_AUTH_SKEW_S || skew < -OTA_AUTH_SKEW_S) {
            ESP_LOGE(TAG, "[OTA_ERR] OFFER stale: ts=%lld now=%lld (skew=%llds > %ds)",
                     (long long)o->ts, now, skew, OTA_AUTH_SKEW_S);
            return "stale";
        }
    }

    /* nonce 严格递增：NVS 里存最近一次通过鉴权的值，重启后依旧有效（防重放） */
    uint64_t last = last_nonce_load();
    if (o->nonce <= last) {
        ESP_LOGE(TAG, "[OTA_ERR] OFFER replay: nonce=%llu <= last=%llu",
                 (unsigned long long)o->nonce, (unsigned long long)last);
        return "replay";
    }
    last_nonce_save(o->nonce);
    ESP_LOGI(TAG, "[OTA_AUTH] OK (nonce=%llu prev=%llu, ts=%llu)",
             (unsigned long long)o->nonce, (unsigned long long)last,
             (unsigned long long)o->ts);
    return NULL;
}

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
        s_check_state = OTA_CHECK_FOUND;    /* §79：有更新（无论谁发起） */
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
    } else if (strstr(json, "OTA_NOUPDATE")) {
        /* §79 双向 OTA：PC 判定无更新（设备版本 >= 本地最新），查新收尾 */
        s_check_state = OTA_CHECK_NOUPDATE;
        ESP_LOGI(TAG, "[OTA_DIAG] OTA_NOUPDATE received (already up to date)");
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

    /* ★ 写槽 staging（PSRAM 32KB，屏闪缓解，见 s_wr_stage 注释） */
    s_wr_stage = heap_caps_malloc(OTA_WRITE_STAGE, MALLOC_CAP_SPIRAM);
    if (!s_wr_stage) {
        ESP_LOGW(TAG, "[OTA_MEM] write-stage 32KB alloc failed -> direct 4KB writes");
    }

    /* 记录本固件是否处于自证窗口（供 UI 提示用） */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_pending_verify = (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
                        st == ESP_OTA_IMG_PENDING_VERIFY);

    /* ★ 栈必须在内部 RAM（F35 铁律 + 实机验证，见文件头约束 1）：
       otadata 读 / esp_ota_begin 擦槽 / esp_ota_write 都会冻结 ext-mem cache，
       IDF 的 s_task_stack_is_sane_when_cache_frozen() 要求当前任务 SP 在内部 DRAM。
       xTaskCreatePinnedToCore 走 pvPortMalloc → INTERNAL（heap_idf.c 硬写）。
       优先级 3：低于 link(4)/LVGL(4)，避免下载挤占交互；高于 IDLE 保证推进。 */
    BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota_service", OTA_TASK_STACK, NULL, 3, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "[OTA_ERR] ota_task create failed (8KB internal stack)");
    }
    assert(ok == pdPASS);

    link_manager_add_json_cb(ota_on_json);
    resume_load();

    /* §80 S3：PSA 子系统初始化（幂等）。psa_hash_* 在 v6.1 里通常已被别的组件
       初始化过，但 psa_import_key / psa_mac_verify 走密钥子系统，显式初始化一次更稳。 */
    psa_status_t pst = psa_crypto_init();
    if (pst != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] psa_crypto_init failed: %d (HMAC auth unavailable)", (int)pst);
    }
    {
        uint8_t psk_probe[64];
        size_t psk_len = ota_psk_load(psk_probe, sizeof(psk_probe));
        ESP_LOGI(TAG, "[OTA_AUTH] psk %s (%u B), last_nonce=%llu",
                 psk_len ? "ready" : "MISSING -> 所有 OFFER 都会被拒",
                 (unsigned)psk_len, (unsigned long long)last_nonce_load());
    }

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

/* ---- §79 双向 OTA：设备端发起 ---- */

void ota_service_request_update(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "[OTA_DIAG] request_update ignored: update already running");
        return;
    }
    if (!s_cmd_q) {
        return;     /* init 未跑（防御） */
    }
    char fw[32];
    ota_service_get_running_version(fw, sizeof(fw));
    char req[96];
    snprintf(req, sizeof(req), "{\"type\":\"OTA_REQ\",\"fw\":\"%s\"}", fw);
    if (link_manager_send_json(req) == ESP_OK) {
        s_check_state = OTA_CHECK_PENDING;
        s_check_sent_us = esp_timer_get_time();
        ESP_LOGI(TAG, "[OTA_DIAG] device-initiated update request sent (fw=%s)", fw);
    } else {
        s_check_state = OTA_CHECK_TIMEOUT;   /* link 离线，立即给 UI 失败态 */
        ESP_LOGW(TAG, "[OTA_DIAG] OTA_REQ send failed (link offline?)");
    }
}

uint8_t ota_service_get_check_state(void)
{
    if (s_check_state == OTA_CHECK_PENDING &&
        esp_timer_get_time() - s_check_sent_us > OTA_CHECK_TIMEOUT_US) {
        s_check_state = OTA_CHECK_TIMEOUT;   /* 10s 无应答 → 超时 */
    }
    return s_check_state;
}

/* ---- §79e 手动切槽（A/B 双系统）---- */

static esp_timer_handle_t s_switch_timer;    /* 切槽后延迟重启（一次性） */

static void slot_switch_reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "[OTA_DIAG] slot switched, rebooting now");
    esp_restart();                           /* 不返回 */
}

void ota_service_get_slot_pair(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = ota_pick_update_slot();
    snprintf(out, len, "Slot: %s  ->  %s",
             run ? run->label : "?", next ? next->label : "?");
}

ota_switch_result_t ota_service_switch_slot(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "[OTA_DIAG] switch_slot ignored: update running");
        return OTA_SWITCH_BUSY;
    }
    const esp_partition_t *target = ota_pick_update_slot();
    if (!target) {
        return OTA_SWITCH_NO_TARGET;
    }
    /* 防砖闸 1：对侧必须能解析出合法镜像描述（头部 + 段表 OK） */
    const esp_app_desc_t *desc = NULL;
    esp_app_desc_t desc_buf;
    if (esp_ota_get_partition_description(target, &desc_buf) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] switch_slot: target %s image invalid, refuse",
                 target->label);
        return OTA_SWITCH_INVALID;
    }
    desc = &desc_buf;
    /* 防砖闸 2：槽状态 INVALID/ABORTED（曾升级失败被回滚标记）拒切 ——
     * 切向 boot 都会拒绝的槽等于白切；VALID/UNDEFINED/PENDING_VERIFY 放行。 */
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(target, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) {
        ESP_LOGE(TAG, "[OTA_ERR] switch_slot: target %s state=%d (dead), refuse",
                 target->label, (int)st);
        return OTA_SWITCH_INVALID;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] switch_slot: set_boot_partition failed");
        return OTA_SWITCH_FAIL;
    }
    ESP_LOGW(TAG, "[OTA_DIAG] boot slot -> %s (image ver=%s), reboot in 800ms",
             target->label, desc ? desc->version : "?");
    /* 800ms 后重启：给 UI 状态行一拍显示提示（esp_timer 回调里 esp_restart） */
    if (!s_switch_timer) {
        const esp_timer_create_args_t targs = {
            .callback = slot_switch_reboot_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ota_slot_sw",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&targs, &s_switch_timer) != ESP_OK) {
            esp_restart();                   /* 定时器建不出来：立即重启兜底 */
        }
    }
    esp_timer_start_once(s_switch_timer, 800 * 1000ULL);
    return OTA_SWITCH_OK;
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
    /* ★ 去重（§80j）：main.c 的 tick 每 1s 调一次本函数，未满足条件期间会**重复**走到这里。
       原来每次都 notify → UI 层每秒打一条 "[OTA_UI] evt=6 not shown on card"，
       几秒就把 OTA 的真实日志刷没了（验收时看不到 progress/error）。
       用独立标志而不是 s_pending_verify 判断：后者在 ota_service_init() 里已被置位，
       拿它做门闩会让首次通知永远发不出去。 */
    if (!s_rb_notified) {
        s_rb_notified = true;
        ESP_LOGW(TAG, "[OTA_DIAG] boot=%s state=PENDING_VERIFY -> rollback armed "
                      "(自证窗口 ~30s，WiFi+link 就绪 10s 后转正)", run->label);
        notify(OTA_EVT_ROLLBACK_ARMED, 0, run->label);
    }

    /* ★ 自证条件（M3 增强）：WiFi 已连 + link_manager 在线（对端可达）。
       两者都过才算"进程健康 + 网络栈活着"。 */
    bool wifi_ok = wifi_manager_is_connected();
    bool link_ok = link_manager_is_online();
    if (!wifi_ok || !link_ok) {
        /* 未就绪：静默返回 false，等调用方（main.c 1s tick）下一秒再试；
           窗口耗尽的 W 告警由 tick 统一打一次，避免周期性刷屏。 */
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
    ota_offer_t of;
    memset(&of, 0, sizeof(of));

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
    const cJSON *jm = cJSON_GetObjectItem(root, "min_version");
    const cJSON *jf = cJSON_GetObjectItem(root, "force");
    const cJSON *jn = cJSON_GetObjectItem(root, "nonce");
    const cJSON *jt = cJSON_GetObjectItem(root, "ts");
    const cJSON *jmc = cJSON_GetObjectItem(root, "hmac");
    if (cJSON_IsString(ju)) {
        snprintf(of.url, sizeof(of.url), "%s", ju->valuestring);
    }
    if (cJSON_IsNumber(js)) {
        of.size = (long long)js->valuedouble;
    }
    if (cJSON_IsString(jh)) {
        snprintf(of.sha256, sizeof(of.sha256), "%s", jh->valuestring);
    }
    if (cJSON_IsString(jv)) {
        snprintf(of.ver, sizeof(of.ver), "%s", jv->valuestring);
    }
    if (cJSON_IsString(jm)) {
        snprintf(of.min_ver, sizeof(of.min_ver), "%s", jm->valuestring);
    }
    /* force 归一成 0/1。ICD §8.1 把 force 定义为 0/1，PC 端 build_offer_fields
       发的就是 int（`"force": 1 if force else 0`）。
       ★ 不能只用 cJSON_IsTrue：它只判 JSON 字面量 true（类型 cJSON_True），
       数字 1 的类型是 cJSON_Number → 返回 false。那样勾了 force 仍读成 0，
       且规范串里设备端算 "0"、PC 端签 "1" ⇒ 直接 REJECT badauth（踩过）。
       所以布尔与数字两种写法都收。 */
    of.force = (cJSON_IsTrue(jf) || (cJSON_IsNumber(jf) && jf->valueint != 0)) ? 1 : 0;
    if (cJSON_IsNumber(jn)) {
        of.nonce = (uint64_t)jn->valuedouble;
    }
    if (cJSON_IsNumber(jt)) {
        of.ts = (uint64_t)jt->valuedouble;
    }
    if (cJSON_IsString(jmc)) {
        snprintf(of.hmac, sizeof(of.hmac), "%s", jmc->valuestring);
    }
    cJSON_Delete(root);

    /* 展示用（UI 浮层 "from → to" 的 to） */
    snprintf(s_offer_ver, sizeof(s_offer_ver), "%s", of.ver);

    if (of.url[0] == '\0' || of.size <= 0) {
        ESP_LOGE(TAG, "[OTA_ERR] OFFER badargs: url='%.80s' size=%lld", of.url, of.size);
        send_reject("badargs");
        return;
    }
    size_t size = (size_t)of.size;

    ESP_LOGI(TAG, "[OTA_DIAG] OFFER received: ver=%s min_ver=%s force=%d size=%u sha=%s nonce=%llu ts=%llu",
             of.ver[0] ? of.ver : "(none)", of.min_ver[0] ? of.min_ver : "(none)", of.force,
             (unsigned)size, of.sha256[0] ? of.sha256 : "(none)",
             (unsigned long long)of.nonce, (unsigned long long)of.ts);

    /* ---------- 第一段：身份（§80 S3）—— 早于所有状态检查 ---------- */
    const char *auth_err = ota_auth_check(&of);
    if (auth_err) {
        send_reject(auth_err);
        return;
    }

    /* ---------- 第二段：完整性前提（§80 S2）---------- */
    /* sha256 由"可选"改"必需"：没有整包摘要，下面的闭环校验就无从谈起，
       宁可在切槽前 10ms 拒掉，也不要刷一个"没人校验过内容"的包。 */
    if (of.sha256[0] == '\0' || strlen(of.sha256) != OTA_SHA_HEX - 1) {
        ESP_LOGE(TAG, "[OTA_ERR] OFFER sha256 missing/short (len=%u, need 64) -> refuse",
                 (unsigned)strlen(of.sha256));
        send_reject("nosha");
        return;
    }

    /* ---------- 第三段：版本门禁（§80 S4）----------
     * 原来设备端完全不比版本（"ver 仅供 UI 展示"），PC 侧勾一下"忽略版本比对"
     * 就能推低版本 —— 设备侧没有任何防线。这里补两条硬规则：
     *   ① 低于 min_version（PC 声明的最低可接受版本）→ 无条件拒；
     *   ② 低于当前运行版本 → 拒，除非 OFFER 显式 force=1。 */
    char running[32];
    ota_service_get_running_version(running, sizeof(running));
    if (of.min_ver[0] && ver_cmp(of.ver, of.min_ver) < 0) {
        ESP_LOGE(TAG, "[OTA_ERR] offer ver=%s < min_version=%s -> REJECT oldver",
                 of.ver, of.min_ver);
        send_reject("oldver");
        return;
    }
    if (ver_cmp(of.ver, running) < 0 && !of.force) {
        ESP_LOGE(TAG, "[OTA_ERR] downgrade refused: offer=%s running=%s (force=0)",
                 of.ver, running);
        send_reject("downgrade");
        return;
    }
    ESP_LOGI(TAG, "[OTA_DIAG] version gate OK: %s -> %s (force=%d, min_ver=%s)",
             running, of.ver, of.force, of.min_ver[0] ? of.min_ver : "-");

    /* ---------- 第四段：状态（原六道检查，顺序不变）---------- */
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
    ESP_LOGI(TAG, "[OTA_DIAG] checks passed (auth+sha+ver+6 state) -> target slot %s (%u B free)",
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

    bool ok = run_update(of.url, size, of.sha256);

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

/* =====================================================================
 * 写槽 sink：读缓冲 → staging(32KB) → 擦/写 flash，同时累积两种哈希
 * =====================================================================
 * 为什么抽出来（§80 S1 顺带修的隐患）：原实现把"攒块"内联在下载循环里，
 * `memcpy(stage + fill, buf, n)` 没有按 staging 空余裁剪 —— TCP 半包时
 * n 可能小于读块，fill 会停在 32767 之类的非对齐水位，再 memcpy 一个整块就
 * **越界写坏 PSRAM staging**（堆破坏，现场只表现为随机 panic）。
 * 现在统一走 sink_feed()：先按空余切分，满块即 commit，余量留到下一轮。
 * 附带收益：段哈希与整包哈希在 **同一处** 更新，"主循环 + 尾部"两处漏一处
 * 的老问题由结构消除。
 * ===================================================================== */
typedef struct {
    const esp_partition_t *upd;
    esp_ota_handle_t       h;
    bool                   bare_write;
    size_t                 erased_upto;    /* 裸写路径的擦除水位（4KB 对齐） */
    size_t                 total;          /* 已写进槽的字节数（= 下一个写偏移） */
    uint8_t               *stage;          /* NULL = staging 分配失败，直写 */
    size_t                 stage_fill;
    uint32_t               seg_done;
    size_t                 seg_fill;       /* 当前 256KB 段内已累积字节 */
    psa_hash_operation_t  *seg_sha;
    psa_hash_operation_t  *full_sha;       /* NULL = 续传路径（改走回读校验） */
} ota_sink_t;

/** 真正落盘一块（长度任意）+ 更新两种哈希 + 段边界 flush。
 *  @return false = 已打日志，调用方 goto out。 */
static bool sink_commit(ota_sink_t *s, const uint8_t *src, size_t len)
{
    if (s->bare_write) {
        /* §72 裸写：按水位擦（擦除分摊到每个 staging 块，无整槽 3.5s 阻塞） */
        size_t need_end = (s->total + len + 4095u) & ~(size_t)4095u;
        if (need_end > s->upd->size) {
            need_end = s->upd->size;
        }
        if (need_end > s->erased_upto) {
            esp_err_t er = esp_partition_erase_range(s->upd, s->erased_upto,
                                                     need_end - s->erased_upto);
            if (er != ESP_OK) {
                ESP_LOGE(TAG, "[OTA_ERR] esp_partition_erase_range failed at %u..%u (%s)",
                         (unsigned)s->erased_upto, (unsigned)need_end, esp_err_to_name(er));
                return false;
            }
            s->erased_upto = need_end;
        }
        if (esp_partition_write(s->upd, s->total, src, len) != ESP_OK) {
            ESP_LOGE(TAG, "[OTA_ERR] esp_partition_write failed at %u B (slot=%s)",
                     (unsigned)s->total, s->upd->label);
            return false;
        }
    } else if (esp_ota_write(s->h, src, len) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_ota_write failed at %u B (slot=%s)",
                 (unsigned)s->total, s->upd->label);
        return false;
    }

    /* 段哈希 + 整包哈希（同序：先写后算，保证"槽里内容"与"哈希覆盖内容"一致） */
    if (psa_hash_update(s->seg_sha, src, len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] psa_hash_update failed at %u B", (unsigned)s->total);
        return false;
    }
    if (s->full_sha && psa_hash_update(s->full_sha, src, len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] full sha update failed at %u B", (unsigned)s->total);
        return false;
    }
    s->total += len;
    s->seg_fill += len;

    /* 段边界：finish 当前段 → 存摘要 → 重开独立上下文（不跨段复用，PSA 不允许） */
    if (s->seg_fill >= OTA_SEG_SIZE && s->seg_done < OTA_SEG_MAX) {
        uint8_t digest[32];
        size_t dlen = 0;
        if (psa_hash_finish(s->seg_sha, digest, sizeof(digest), &dlen) != PSA_SUCCESS ||
            dlen != 32) {
            ESP_LOGE(TAG, "[OTA_ERR] seg hash finish failed at seg %u (dlen=%u)",
                     (unsigned)s->seg_done, (unsigned)dlen);
            return false;
        }
        memcpy(s_resume.seg_sha[s->seg_done], digest, 32);
        s->seg_done++;
        s_resume.seg_done = s->seg_done;
        s_resume.next_offset = (uint32_t)s->total;
        resume_save();
        char hex[OTA_SHA_HEX];
        bytes_to_hex(digest, 32, hex);
        ESP_LOGI(TAG, "[OTA_RESUME] seg %u flushed (%u B), next_offset=%u, digest=%.16s...",
                 (unsigned)s->seg_done, (unsigned)OTA_SEG_SIZE,
                 (unsigned)s_resume.next_offset, hex);
        if (psa_hash_setup(s->seg_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "[OTA_ERR] psa_hash_setup(re-seg) failed");
            return false;
        }
        s->seg_fill = 0;
    }
    return true;
}

/** 把 len 字节喂进 staging（跨块、半包都安全）；满 32KB 落盘一次。 */
static bool sink_feed(ota_sink_t *s, const uint8_t *data, size_t len)
{
    if (!s->stage) {
        return sink_commit(s, data, len);   /* staging 不可用：直写（仅影响闪烁观感） */
    }
    while (len > 0) {
        size_t space = OTA_WRITE_STAGE - s->stage_fill;
        size_t take = len < space ? len : space;
        memcpy(s->stage + s->stage_fill, data, take);
        s->stage_fill += take;
        data += take;
        len -= take;
        if (s->stage_fill == OTA_WRITE_STAGE) {
            if (!sink_commit(s, s->stage, s->stage_fill)) {
                return false;
            }
            s->stage_fill = 0;
        }
    }
    return true;
}

/** 收尾：把 staging 残余（<32KB 的尾巴）落盘。abort 时不要调用（"取消即放弃"）。 */
static bool sink_flush(ota_sink_t *s)
{
    if (s->stage && s->stage_fill > 0) {
        size_t fill = s->stage_fill;
        s->stage_fill = 0;
        if (!sink_commit(s, s->stage, fill)) {
            return false;
        }
    }
    return true;
}

/** §80 S2 续传路径的整包回读校验。
 *  ★ 只读到 img_len（bin 实际长度），**绝不能读到槽大小** —— 槽尾是擦除态
 *    0xFF，算进哈希就永远对不上（这是本节最容易被"顺手写成 upd->size"的坑）。
 *  复用 s_rd_buf（此刻 HTTP 已读完，无冲突），避免在 8KB 任务栈上再开 4KB。 */
static bool verify_image_sha(const esp_partition_t *p, size_t img_len, const char *expect_hex)
{
    uint8_t digest[32];
    size_t dlen = 0;
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&h, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] whole-sha: hash_setup failed");
        return false;
    }
    for (size_t off = 0; off < img_len; off += OTA_READ_CHUNK) {
        size_t n = (img_len - off) > OTA_READ_CHUNK ? OTA_READ_CHUNK : (img_len - off);
        if (esp_partition_read(p, off, s_rd_buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "[OTA_ERR] whole-sha: read failed at %u", (unsigned)off);
            psa_hash_abort(&h);
            return false;
        }
        if (psa_hash_update(&h, s_rd_buf, n) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "[OTA_ERR] whole-sha: hash_update failed at %u", (unsigned)off);
            psa_hash_abort(&h);
            return false;
        }
    }
    if (psa_hash_finish(&h, digest, sizeof(digest), &dlen) != PSA_SUCCESS || dlen != 32) {
        ESP_LOGE(TAG, "[OTA_ERR] whole-sha: hash_finish failed");
        return false;
    }
    char hex[OTA_SHA_HEX];
    bytes_to_hex(digest, 32, hex);
    bool ok = (strcasecmp(hex, expect_hex) == 0);
    ESP_LOGI(TAG, "[OTA_DIAG] whole-sha %s (readback calc=%.16s... expect=%.16s..., %u B)",
             ok ? "MATCH" : "MISMATCH", hex, expect_hex, (unsigned)img_len);
    return ok;
}

/** 返回 true 表示下载 + 校验 + 切槽全部成功。 */
static bool run_update(const char *url, size_t expect_size, const char *expect_sha256)
{
    s_state = ST_DOWNLOADING;
    notify(OTA_EVT_DOWNLOADING, 0, NULL);

    /* §80 S2：没有整包摘要就不写槽 —— 否则"校验闭环"无从谈起（防御性二次拦截，
       handle_offer 已经用 nosha 拒过一次） */
    if (!expect_sha256 || expect_sha256[0] == '\0') {
        ESP_LOGE(TAG, "[OTA_ERR] run_update called without sha256 -> refuse");
        return false;
    }

    const esp_partition_t *upd = ota_pick_update_slot();   /* §6.4：显式选槽，绝不选 ota_15 */
    if (!upd) {
        ESP_LOGE(TAG, "[OTA_ERR] no valid update slot (ota_0/ota_1 only)");
        return false;
    }

    /* -------- §72 真续传判定（esp_partition 裸写 + 自管擦除） --------
     * 2026-10-08 实现。此前（§67）曾因 esp_ota_begin 整槽擦除 / esp_ota_write
     * 顺序写 + 0xE9 魔数检查而降级全新下载；现续传路径彻底弃用 esp_ota_write：
     *   erase：esp_partition_erase_range 按"写进度水位"逐块擦（不预擦整槽）
     *   write：esp_partition_write(upd, total, src, n)（绝对偏移，无魔数检查）
     *   verify：收尾 esp_image_verify（= esp_ota_end 同源）+ 旧段回读比对 nvs 摘要
     * 回滚/自证状态在 otadata（esp_ota_set_boot_partition 语义不变），防砖不受影响。
     * 续传前提：nvs 断点 total/next/seg 三元自洽 + 整包 sha 匹配（PC 报了才比）。 */
    size_t start_off = 0;
    uint32_t seg_done = 0;
    bool can_resume = false;
    bool bare_write = false;            /* true=esp_partition 裸写路径 */
    bool sha_known = expect_sha256 && expect_sha256[0];
    if (s_resume.magic == OTA_RESUME_MAGIC && s_resume.next_offset > 0 &&
        s_resume.total_size == (uint32_t)expect_size &&
        s_resume.next_offset < (uint32_t)expect_size &&
        s_resume.next_offset % OTA_SEG_SIZE == 0 &&
        s_resume.seg_done == s_resume.next_offset / OTA_SEG_SIZE &&
        s_resume.seg_done > 0 && s_resume.seg_done < OTA_SEG_MAX &&
        (!sha_known || strcasecmp(s_resume.sha256, expect_sha256) == 0)) {
        ESP_LOGW(TAG, "[OTA_RESUME] found: total=%u done=%u seg_done=%u sha=%s",
                 (unsigned)s_resume.total_size, (unsigned)s_resume.next_offset,
                 (unsigned)s_resume.seg_done, s_resume.sha256);
        /* ★ 先回读校验旧前缀：flash 现状 != nvs 摘要 ⇒ 断点过期，降级全新下载 */
        if (verify_written_segments(upd, s_resume.seg_done)) {
            can_resume = true;
            bare_write = true;
            start_off = s_resume.next_offset;
            seg_done = s_resume.seg_done;
            ESP_LOGW(TAG, "[OTA_RESUME] RESUME from %u B (seg_done=%u, bare-write via esp_partition)",
                     (unsigned)start_off, (unsigned)seg_done);
        } else {
            ESP_LOGW(TAG, "[OTA_RESUME] resume record stale -> discard, full download");
            memset(&s_resume, 0, sizeof(s_resume));
            resume_save();
        }
    } else if (s_resume.magic == OTA_RESUME_MAGIC && s_resume.next_offset > 0) {
        ESP_LOGW(TAG, "[OTA_RESUME] record mismatch (total=%u off=%u seg=%u vs offer size=%u)"
                 " -> discard, full download",
                 (unsigned)s_resume.total_size, (unsigned)s_resume.next_offset,
                 (unsigned)s_resume.seg_done, (unsigned)expect_size);
        memset(&s_resume, 0, sizeof(s_resume));
        resume_save();
    }
    if (!can_resume) {
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

    /* ★ 整包哈希上下文必须**在所有 goto out 之前**声明并初始化。
       原来声明在 http_open 之后，任何更早的 goto out（open 失败 / Range 降级 /
       ota_begin 失败）都会跳过初始化语句，而 out 段又要读 full_sha_inited
       → gcc -O2 内联后报 -Werror=maybe-uninitialized。
       scope 顺序错位在 C 里是未定义行为，不是误报。 */
    psa_hash_operation_t full_sha = PSA_HASH_OPERATION_INIT;
    bool full_sha_inited = false;

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
    /* PC 端未支持 Range 时退回整包：必须从头写，否则槽内容错位。
       §72：此时已选裸写路径 ⇒ 不回 esp_ota_begin（那会再整槽擦一遍），
       直接把裸写水位拨回 0 从头写。 */
    if (can_resume && status == 200) {
        ESP_LOGW(TAG, "[OTA_RESUME] server ignored Range (200) -> downgrade to FULL download");
        can_resume = false;
        bare_write = true;
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

    size_t erased_upto = start_off;   /* §72 裸写路径的擦除水位（start_off 256K 对齐 ⇒ 扇区对齐） */
    if (bare_write) {
        ESP_LOGI(TAG, "[OTA_DIAG] bare-write mode: erase on demand from %u B (esp_partition)",
                 (unsigned)erased_upto);
        log_triple("bare-resume-ok", expect_size - start_off);
    } else if (esp_ota_begin(upd, expect_size, &h) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_ota_begin failed on %s (size=%u)",
                 upd->label, (unsigned)expect_size);
        log_triple("ota-begin-FAIL", expect_size);
        goto out;
    } else {
        ota_begun = true;
        log_triple("ota-begin-ok", expect_size);
    }

    /* 段哈希：从 start_off 起为**当前段**开一个独立上下文 */
    if (psa_hash_setup(&seg_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "[OTA_ERR] psa_hash_setup failed");
        goto out;
    }

    /* ★ §80 S2：整包哈希上下文
       SHA-256 是流式哈希，**跨重启无法从中途累积**，所以两条路分开：
         · start_off==0（全新下载）→ 与段哈希同序累积，收尾 finish 即得整包摘要，零额外 IO；
         · start_off>0（续传）      → 不累积，收尾走 verify_image_sha() 回读 [0, expect_size)。
       注意此处必须放在 "server ignored Range → 降级 FULL" 之后判 start_off，
       否则降级场景会白白多花 1~2s 回读。
       （full_sha / full_sha_inited 的声明已上移到函数前部，见那里注释。） */
    if (start_off == 0) {
        if (psa_hash_setup(&full_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "[OTA_ERR] full sha_setup failed");
            goto out;
        }
        full_sha_inited = true;
    }

    /* 写槽 sink（staging + 双哈希 + 擦除水位全在这里，见 sink_commit 注释） */
    ota_sink_t sink = {
        .upd         = upd,
        .h           = h,
        .bare_write  = bare_write,
        .erased_upto = erased_upto,
        .total       = (size_t)start_off,
        .stage       = s_wr_stage,
        .stage_fill  = 0,
        .seg_done    = seg_done,
        .seg_fill    = 0,
        .seg_sha     = &seg_sha,
        .full_sha    = full_sha_inited ? &full_sha : NULL,
    };

    size_t total = (size_t)start_off;
    int last_pct = -1;
    size_t since_yield = 0;            /* §80 S1：累计字节，够 64KB 才让出一次 */
    int64_t t_dl0 = esp_timer_get_time();
    ESP_LOGI(TAG, "[OTA_DIAG] download start: offset=%u / %u (%u%%), seg_done=%u, chunk=%u, yield=%u",
             (unsigned)total, (unsigned)expect_size,
             (unsigned)(expect_size ? total * 100 / expect_size : 0), (unsigned)seg_done,
             (unsigned)OTA_READ_CHUNK, (unsigned)OTA_YIELD_BYTES);

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

        /* ★ 攒块写槽（屏闪缓解）：读块只入 staging，攒满 32KB 才落盘一次。
           sink_feed 会按 staging 剩余空间切分 —— 半包（n < 读块）不会再像旧实现
           那样把 memcpy 写出块外。擦除/写入/段哈希/整包哈希都在 sink_commit 里
           同序完成，失败即返回 false。 */
        if (!sink_feed(&sink, s_rd_buf, (size_t)n)) {
            goto out;
        }
        total = sink.total;

        int pct = (expect_size > 0) ? (int)((uint64_t)total * 100 / expect_size) : 0;
        s_percent = (uint8_t)pct;
        if (pct >= last_pct + OTA_PROGRESS_STEP || pct >= 100) {
            last_pct = pct;
            int64_t dt_us = esp_timer_get_time() - t_dl0;
            int speed = dt_us > 0 ? (int)((uint64_t)(total - start_off) * 1000000ULL / (uint64_t)dt_us / 1024ULL) : 0;
            char pbuf[96];
            snprintf(pbuf, sizeof(pbuf),
                     "{\"type\":\"OTA_PROGRESS\",\"percent\":%d,\"speed\":%d}", pct, speed);
            link_manager_send_json(pbuf);
            notify(OTA_EVT_DOWNLOADING, (uint8_t)pct, NULL);
            ESP_LOGI(TAG, "[OTA_DIAG] progress %d%% (%u B, %d KB/s)", pct, (unsigned)total, speed);
        }
        /* ★ §80 S1 让出粒度：每 64KB 一次（原实现每 4KB 一次 vTaskDelay(1)，
           tick=1000Hz ⇒ 5MB 白等 ~1.2s，且把吞吐理论上限压到 4MB/s）。
           ABORT 仍在每块边界检查，中止延迟最多 8KB。 */
        since_yield += (size_t)n;
        if (since_yield >= OTA_YIELD_BYTES) {
            since_yield = 0;
            vTaskDelay(1);
        }
    }

    /* ★ 收尾：staging 残余（<32KB 的尾巴）落盘。abort 时丢弃（"取消即放弃"语义，
       槽反正会在下次 begin 时整槽擦除）。 */
    if (!s_abort && !sink_flush(&sink)) {
        goto out;
    }
    total = sink.total;

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
    int64_t t_dl1 = esp_timer_get_time();     /* §80 S0：下载段结束 */

    /* 收尾最后一段（不足一满段的尾巴），flush 到 resume。
       ★★ 段计数一律以 sink.seg_done 为准（2026-11-12 真机验收实测 bug）：
       §80 S1 把段边界处理抽进 sink_commit 后，满段的 ++ 发生在 s->seg_done 上，
       函数开头的局部 seg_done 从此**再没人更新** —— 尾部收尾和下面的"段数一致性
       硬校验"拿到的都是建 sink 时的旧值（全新下载=0，续传=记录里的 18），
       于是 sha 都对上了却在最后一步报 got 19 expect 20 / got 1 expect 20，
       升级永远切不了槽。这里先同步，尾部再按"真有尾巴字节"才计一段
       （原实现整段对齐时会多算一段，size 恰为 256KB 整数倍时同样会误判）。 */
    seg_done = sink.seg_done;
    {
        uint8_t digest[32];
        size_t dlen = 0;
        if (psa_hash_finish(&seg_sha, digest, sizeof(digest), &dlen) != PSA_SUCCESS ||
            dlen != 32) {
            ESP_LOGE(TAG, "[OTA_ERR] final seg hash finish failed (dlen=%u)", (unsigned)dlen);
            goto out;
        }
        if (sink.seg_fill > 0 && seg_done < OTA_SEG_MAX) {
            memcpy(s_resume.seg_sha[seg_done], digest, 32);
            seg_done++;
        }
        s_resume.seg_done = seg_done;
        s_resume.next_offset = (uint32_t)total;
        resume_save();
        char hex[OTA_SHA_HEX];
        bytes_to_hex(digest, 32, hex);
        ESP_LOGI(TAG, "[OTA_RESUME] tail seg %u flushed (fill=%u B), digest=%.16s...",
                 (unsigned)seg_done, (unsigned)sink.seg_fill, hex);
    }

    if (bare_write) {
        /* §72 裸写路径：无 ota handle ⇒ 用与 esp_ota_end 同源的
           esp_image_verify(ESP_IMAGE_VERIFY) 做镜像头 + 段 CRC 全量校验。 */
        if (!verify_image_on_partition(upd)) {
            ESP_LOGE(TAG, "[OTA_ERR] image verify failed (bare-write path)");
            goto out;
        }
    } else if (esp_ota_end(h) != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ERR] esp_ota_end failed (image invalid: 镜像头/段 CRC 不过)");
        h = 0;
        ota_begun = false;
        goto out;
    } else {
        h = 0;
        ota_begun = false;
        ESP_LOGI(TAG, "[OTA_DIAG] esp_ota_end OK (image header + segment CRC valid)");
    }

    /* -------- §80 S2：整包 SHA-256 闭环（切槽前的最后一道闸）--------
       旧实现把 OFFER 里的 sha256 只 snprintf 进 NVS 和日志，**从不与自己算的比对**
       —— 能拦住坏包全靠 esp_image_verify 的段 CRC，拦不住"给错包/给旧包/人为构造
       的合法镜像"。现在两条路径都必须算出一个整包摘要并比对：
         · 全新下载 → 下载时与段哈希同序累积（full_sha_inited），此处 finish 即得；
         · 断点续传 → 跨重启不可累积，改回读镜像区 [0, expect_size) 重算（~1~2s）。
       任一不符 → 不 set_boot，运行槽不受影响。 */
    bool sha_ok = false;
    if (full_sha_inited) {
        uint8_t digest[32];
        size_t dlen = 0;
        psa_status_t hst = psa_hash_finish(&full_sha, digest, sizeof(digest), &dlen);
        full_sha_inited = false;
        if (hst == PSA_SUCCESS && dlen == 32) {
            char hex[OTA_SHA_HEX];
            bytes_to_hex(digest, 32, hex);
            sha_ok = (strcasecmp(hex, expect_sha256) == 0);
            ESP_LOGI(TAG, "[OTA_DIAG] whole-sha %s (stream calc=%.16s... expect=%.16s...)",
                     sha_ok ? "MATCH" : "MISMATCH", hex, expect_sha256);
        } else {
            ESP_LOGE(TAG, "[OTA_ERR] whole-sha finish failed (st=%d dlen=%u)",
                     (int)hst, (unsigned)dlen);
        }
    } else {
        sha_ok = verify_image_sha(upd, expect_size, expect_sha256);
    }
    if (!sha_ok) {
        ESP_LOGE(TAG, "[OTA_ERR] whole-image SHA256 mismatch -> refuse to switch slot");
        goto out;
    }

    /* 段数一致性硬校验（防 PC 端 size 与内容不符） */
    if (seg_done != (expect_size + OTA_SEG_SIZE - 1) / OTA_SEG_SIZE) {
        ESP_LOGE(TAG, "[OTA_ERR] segment count mismatch: got %u expect %u",
                 (unsigned)seg_done,
                 (unsigned)((expect_size + OTA_SEG_SIZE - 1) / OTA_SEG_SIZE));
        goto out;
    }

    /* -------- §80 S0：三段耗时打点（判读优化是否真的生效）--------
       dl/total > 0.7 ⇒ 瓶颈在链路；< 0.4 ⇒ 瓶颈在 flash/校验，网络优化收益有限。 */
    {
        int64_t t_vf_done = esp_timer_get_time();
        int64_t dl_ms = (t_dl1 - t_dl0) / 1000;
        int64_t vf_ms = (t_vf_done - t_dl1) / 1000;
        int64_t all_ms = (t_vf_done - t_dl0) / 1000;
        int kbps = dl_ms > 0
                 ? (int)(((uint64_t)(expect_size - start_off) * 1000ULL / (uint64_t)dl_ms) / 1024ULL)
                 : 0;
        ESP_LOGI(TAG, "[OTA_DIAG] timing dl=%lldms verify=%lldms total=%lldms throughput=%dKB/s size=%u%s",
                 (long long)dl_ms, (long long)vf_ms, (long long)all_ms, kbps,
                 (unsigned)expect_size, can_resume ? " (resumed)" : "");
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
    psa_hash_abort(&seg_sha);              /* 已 finish 的上下文 abort 是安全空操作 */
    if (full_sha_inited) {
        psa_hash_abort(&full_sha);
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
    ESP_LOGI(TAG, "[OTA_DIAG] ota_task started (stack in INTERNAL, F35)");
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
