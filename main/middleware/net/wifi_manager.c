/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "middleware/mem_diag.h"
#include <string.h>

/* gcc -Werror=stringop-truncation: 本文件 strncpy 均按 sizeof(dst)-1 拷贝并由
 * 零初始化保证终止，无真实越界；抑制该告警以免中断构建。 */
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

static const char *TAG = "WIFI_MGR";

#define MAX_AP                  16
#define WIFI_NVS_NAMESPACE      "wifi_cfg"
#define WIFI_NVS_KEY            "creds"
#define WIFI_STORE_MAGIC        0x57494649u   /* 'WIFI' */
#define WIFI_STORE_VERSION      1
#define WIFI_RECONNECT_DELAY_MS 1000
#define AP_DEFAULT_SSID   "KS31-Korvo"
#define AP_DEFAULT_PASS   "800180Xiao"
#define AP_GW_IP          "192.168.4.1"
#define AP_CHANNEL        6

/* ================= NVS 凭证存储层 ================= */

typedef struct {
    char ssid[WIFI_SSID_LEN_MAX + 1];
    char password[WIFI_PASS_LEN_MAX + 1];
} wifi_cred_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    wifi_cred_t creds[WIFI_SAVED_MAX];
} wifi_store_t;

static esp_err_t store_load(wifi_store_t *store)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(*store);
    err = nvs_get_blob(h, WIFI_NVS_KEY, store, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    if (store->magic != WIFI_STORE_MAGIC ||
        store->version != WIFI_STORE_VERSION ||
        store->count > WIFI_SAVED_MAX) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t store_commit(const wifi_store_t *store)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, WIFI_NVS_KEY, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* 相同 SSID 更新密码并置顶；新 SSID 插队首；满了淘汰队尾。
 *
 * ★★ F48：返回 true 只代表"内容真的变了、需要落 NVS"。
 *   背景：原实现在 GOT_IP 里无条件 store_commit() ⇒ 每次连接成功都擦写 flash。
 *   spi_flash 擦写会走 spi_flash_disable_interrupts_caches_and_other_cpu()：
 *   关中断 + 关 cache（PSRAM 与 flash 映射共用同一 cache）⇒ 实测单次窗口可达
 *   94~157 ms（`[panelprobe] gap max`）。这个窗口会打乱 WiFi 收发与音频 DMA。
 *   ★ 2026-09-25 起屏已切到 auto refresh 模式（不再有 bounce buffer），**"花屏"**
 *     这一后果已被结构性消除；但**窗口本身仍在** ⇒ 少写 flash 依然是净收益。
 *   自动重连时凭证本来就是同一组、且已在队首 ⇒ 判定"未变"直接跳过，0 次 flash 写。 */
static bool store_upsert(wifi_store_t *store, const char *ssid, const char *pass)
{
    /* 队首就是同一组 SSID+密码 ⇒ upsert 是幂等的，无变化 */
    bool changed = !(store->count > 0 &&
                     strcmp(store->creds[0].ssid, ssid) == 0 &&
                     strcmp(store->creds[0].password, pass ? pass : "") == 0);

    wifi_cred_t node;
    memset(&node, 0, sizeof(node));
    strncpy(node.ssid, ssid, WIFI_SSID_LEN_MAX);
    if (pass) {
        strncpy(node.password, pass, WIFI_PASS_LEN_MAX);
    }

    /* 已存在则先从旧位置删除 */
    for (int i = 0; i < store->count; i++) {
        if (strcmp(store->creds[i].ssid, ssid) == 0) {
            memmove(&store->creds[i], &store->creds[i + 1],
                    (store->count - i - 1) * sizeof(wifi_cred_t));
            store->count--;
            break;
        }
    }

    /* 整体后移腾出 0 号位；已满则丢弃队尾 */
    int move = (store->count < WIFI_SAVED_MAX) ? store->count : (WIFI_SAVED_MAX - 1);
    memmove(&store->creds[1], &store->creds[0], move * sizeof(wifi_cred_t));
    store->creds[0] = node;
    if (store->count < WIFI_SAVED_MAX) {
        store->count++;
    }
    store->magic = WIFI_STORE_MAGIC;
    store->version = WIFI_STORE_VERSION;
    return changed;
}

/* 连接成功后保存当前凭证（★ F48：仅在有真实变化时才落 NVS） */
static void save_connected_cred(void)
{
    wifi_config_t cur = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) {
        return;
    }
    wifi_store_t store;
    memset(&store, 0, sizeof(store));
    if (store_load(&store) != ESP_OK) {
        store.count = 0;   /* 首次使用，空表 */
    }
    if (!store_upsert(&store, (const char *)cur.sta.ssid, (const char *)cur.sta.password)) {
        /* ★ F48：凭证没变（自动重连的常态）⇒ 跳过 NVS 落盘。
         * 省掉的这次 flash 擦写会把中断 + cache 关掉上百 ms，足以打乱 WiFi
         * 收发与音频 DMA（★ 屏侧"花屏"已因切到 auto refresh 模式而消失，
         * 所以这里现在是"减少抖动"，不再是"治花屏"）。 */
        ESP_LOGI(TAG, "cred \"%s\" unchanged, skip NVS write (no flash erase)",
                 cur.sta.ssid);
        return;
    }
    esp_err_t err = store_commit(&store);
    ESP_LOGI(TAG, "saved \"%s\" to NVS (%s), total=%d",
             cur.sta.ssid, esp_err_to_name(err), store.count);
}

/* ================= 自动连接状态机 ================= */

typedef enum {
    PHASE_IDLE = 0,
    PHASE_SCANNING,   /* 自动连接：等待扫描结果 */
    PHASE_TRYING,     /* 正在尝试某个候选 */
} connect_phase_t;

static wifi_evt_cb_t s_evt_cb;
static wifi_evt_cb_t s_sys_cb;
static bool s_connected;
static bool s_enabled = true;
static char s_ip[16];
static wifi_role_t s_role = WIFI_ROLE_STA;   /* 默认上电 STA（连已保存路由），UI 可切 AP */

/* AP 客户端表 */
#define AP_MAX_CLIENTS 4
typedef struct {
    uint8_t mac[6];
    char    ip[16];
    bool    used;
} ap_client_t;
static ap_client_t s_clients[AP_MAX_CLIENTS];
static SemaphoreHandle_t s_client_mtx;   /* 保护 s_clients：事件任务写，LVGL 任务读 */

static void start_ap_mode(void);

static wifi_ap_info_t s_ap_list[MAX_AP];
static int  s_ap_count;

static connect_phase_t s_phase = PHASE_IDLE;
static bool s_have_active;                 /* 当前候选表有效（尝试中/已连接） */
static wifi_cred_t s_candidates[WIFI_SAVED_MAX];
static int  s_cand_count;
static int  s_cand_idx;
static int  s_retry_num;
static char s_cur_ssid[WIFI_SSID_LEN_MAX + 1];
static wifi_store_t s_pending_store;       /* 自动连接扫描期间暂存凭证表 */
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_heal_timer;
static bool s_wifi_stopped;    /* give-up 后 esp_wifi_stop 过，自愈时要先 start */

/* ★ §41 断连自愈：give up ≠ 永久死亡。
 * 实证（2026-09-26）：reason=200 bcn_timeout 后 10 连击 NO_AP_FOUND（两个 SSID
 * 同时不可见）= 路由器 2.4G 假死/重启；旧逻辑 give up + esp_wifi_stop() 后
 * **永不重试**，路由器几分钟后恢复、音箱却永久离线。
 * 自愈 = 每 60s 重跑 auto_connect（scan→connect），连上即由 GOT_IP 停表。 */
#define WIFI_HEAL_PERIOD_MS  60000

static void heal_timer_arm(void)
{
    esp_timer_stop(s_heal_timer);   /* 幂等：已停时无害 */
    esp_timer_start_periodic(s_heal_timer, (uint64_t)WIFI_HEAL_PERIOD_MS * 1000ULL);
}

static void heal_timer_cb(void *arg)
{
    (void)arg;
    if (s_connected || s_role != WIFI_ROLE_STA) {
        return;                       /* 兜底：连着/AP 角色不动作 */
    }
    if (s_phase != PHASE_IDLE || s_have_active) {
        return;                       /* 正在尝试或用户手动流程中，不打扰 */
    }
    if (s_wifi_stopped) {
        if (esp_wifi_start() != ESP_OK) {
            return;
        }
        s_wifi_stopped = false;
    }
    ESP_LOGW(TAG, "heal: retry auto-connect (router may be back)");
    wifi_manager_auto_connect();
}

static void notify(wifi_evt_t evt, const char *info)
{
    ESP_LOGI(TAG, "evt=%d info=%s", evt, info ? info : "-");
    if (s_sys_cb) {
        s_sys_cb(evt, info);
    }
    if (s_evt_cb) {
        s_evt_cb(evt, info);
    }
}

/* 扫描结果与已保存凭证匹配：扫描到的按 RSSI 降序，隐藏 SSID 追加末尾 */
static void build_candidates(void)
{
    s_cand_count = 0;
    bool matched[WIFI_SAVED_MAX] = {0};

    /* 第一轮：每次从扫描结果中挑"未匹配且信号最强"的 AP */
    for (;;) {
        int best_ap = -1, best_cred = -1;
        for (int a = 0; a < s_ap_count; a++) {
            for (int c = 0; c < s_pending_store.count; c++) {
                if (matched[c]) {
                    continue;
                }
                if (strcmp(s_ap_list[a].ssid, s_pending_store.creds[c].ssid) != 0) {
                    continue;
                }
                if (best_ap < 0 || s_ap_list[a].rssi > s_ap_list[best_ap].rssi) {
                    best_ap = a;
                    best_cred = c;
                }
            }
        }
        if (best_ap < 0) {
            break;
        }
        s_candidates[s_cand_count++] = s_pending_store.creds[best_cred];
        matched[best_cred] = true;
    }

    /* 第二轮：扫描不到的已保存 Wi-Fi（隐藏 SSID）按存储顺序追加 */
    for (int c = 0; c < s_pending_store.count; c++) {
        if (!matched[c] && s_cand_count < WIFI_SAVED_MAX) {
            s_candidates[s_cand_count++] = s_pending_store.creds[c];
        }
    }
}

static void apply_candidate(const wifi_cred_t *cred)
{
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, cred->ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (cred->password[0]) {
        strncpy((char *)sta_cfg.sta.password, cred->password,
                sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    strncpy(s_cur_ssid, cred->ssid, sizeof(s_cur_ssid) - 1);
    s_retry_num = 0;
    s_phase = PHASE_TRYING;
    s_have_active = true;
    notify(WIFI_EVT_CONNECTING, cred->ssid);
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to \"%s\"", cred->ssid);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_phase == PHASE_TRYING) {
        s_retry_num++;
        ESP_LOGW(TAG, "retry \"%s\" (%d/%d)", s_cur_ssid,
                 s_retry_num + 1, WIFI_MAX_RETRY);
        esp_wifi_connect();
    }
}

/* 当前候选 5 次失败后：切换下一个，或全部放弃 */
static void next_candidate_or_giveup(void)
{
    s_cand_idx++;
    if (s_cand_idx < s_cand_count) {
        ESP_LOGW(TAG, "switch to next saved Wi-Fi: \"%s\"",
                 s_candidates[s_cand_idx].ssid);
        apply_candidate(&s_candidates[s_cand_idx]);
    } else {
        s_phase = PHASE_IDLE;
        s_have_active = false;
        s_connected = false;
        ESP_LOGE(TAG, "all saved Wi-Fi failed after retries, give up (heal timer armed)");
        s_enabled = false; esp_wifi_stop();  /* auto-disable after 5 retries */
        s_wifi_stopped = true;
        heal_timer_arm();                    /* ★ §41：60s 后自愈重扫 */
        notify(WIFI_EVT_ALL_FAILED, NULL);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started, ssid=%s gw=%s", AP_DEFAULT_SSID, AP_GW_IP);
        s_connected = true;
        strncpy(s_ip, AP_GW_IP, sizeof(s_ip) - 1);
        notify(WIFI_EVT_AP_UP, AP_GW_IP);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *c =
            (wifi_event_ap_staconnected_t *)event_data;
        if (xSemaphoreTake(s_client_mtx, portMAX_DELAY) == pdTRUE) {
            int slot = -1;
            for (int i = 0; i < AP_MAX_CLIENTS; i++) {
                if (!s_clients[i].used) { slot = i; break; }
            }
            if (slot >= 0) {
                s_clients[slot].used = true;
                memcpy(s_clients[slot].mac, c->mac, 6);
                s_clients[slot].ip[0] = 0;
            }
            xSemaphoreGive(s_client_mtx);
        }
        s_connected = true;
        ESP_LOGI(TAG, "client joined AP, aid=%d", c->aid);
        notify(WIFI_EVT_STA_JOINED, NULL);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *dc =
            (wifi_event_ap_stadisconnected_t *)event_data;
        int remain = 0;
        if (xSemaphoreTake(s_client_mtx, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < AP_MAX_CLIENTS; i++) {
                if (s_clients[i].used && memcmp(s_clients[i].mac, dc->mac, 6) == 0) {
                    s_clients[i].used = false;
                    s_clients[i].ip[0] = 0;
                }
            }
            for (int i = 0; i < AP_MAX_CLIENTS; i++) {
                if (s_clients[i].used) {
                    remain++;
                }
            }
            xSemaphoreGive(s_client_mtx);
        }
        ESP_LOGW(TAG, "client left AP, aid=%d, remain=%d", dc->aid, remain);
        /* 只要还有客户端连着就保持绿灯，全部离开才红 */
        if (remain > 0) {
            s_connected = true;
            notify(WIFI_EVT_STA_JOINED, NULL);   /* 仍有客户端，保持绿 */
        } else {
            s_connected = false;
            notify(WIFI_EVT_STA_LEFT, NULL);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *as =
            (ip_event_ap_staipassigned_t *)event_data;
        if (xSemaphoreTake(s_client_mtx, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < AP_MAX_CLIENTS; i++) {
                if (s_clients[i].used && memcmp(s_clients[i].mac, as->mac, 6) == 0) {
                    esp_ip4addr_ntoa(&as->ip, s_clients[i].ip, sizeof(s_clients[i].ip));
                    ESP_LOGI(TAG, "client IP assigned: %s", s_clients[i].ip);
                }
            }
            xSemaphoreGive(s_client_mtx);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d =
            (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        /* ★ F48 取证：断联瞬间把内部堆真值一起打出来。判读口径照工程约定 ——
         * 只看 INT largest（不看 free）。若这里 largest 已经很低，说明断联与
         * "内部 RAM 被挤干"同源；若 largest 正常，则更像 RF/共存问题。 */
        ESP_LOGW(TAG, "disconnected from \"%s\", reason=%d | " MEM_DIAG_FMT,
                 s_cur_ssid, d->reason, MEM_DIAG_ARGS(0));

        if (s_phase == PHASE_TRYING) {
            /* 连接阶段失败：计数重试 */
            if (s_retry_num + 1 < WIFI_MAX_RETRY) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer,
                                     WIFI_RECONNECT_DELAY_MS * 1000);
            } else {
                ESP_LOGW(TAG, "\"%s\" failed after %d attempts",
                         s_cur_ssid, WIFI_MAX_RETRY);
                next_candidate_or_giveup();
            }
        } else if (s_have_active && s_cand_count > 0) {
            /* 已连接后运行中掉线：重连当前 Wi-Fi（重新计数） */
            ESP_LOGW(TAG, "link lost, reconnect \"%s\"", s_cur_ssid);
            apply_candidate(&s_candidates[s_cand_idx]);
        } else {
            notify(WIFI_EVT_DISCONNECTED, NULL);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        s_ap_count = 0;
        uint16_t num = 0;
        if (esp_wifi_scan_get_ap_num(&num) == ESP_OK && num > 0) {
            wifi_ap_record_t *rec = calloc(num, sizeof(wifi_ap_record_t));
            if (rec) {
                uint16_t got = num;
                if (esp_wifi_scan_get_ap_records(&got, rec) == ESP_OK) {
                    for (uint16_t i = 0; i < got && s_ap_count < MAX_AP; i++) {
                        memset(&s_ap_list[s_ap_count], 0, sizeof(wifi_ap_info_t));
                        strncpy(s_ap_list[s_ap_count].ssid,
                                (const char *)rec[i].ssid, WIFI_SSID_LEN_MAX);
                        s_ap_list[s_ap_count].rssi = rec[i].rssi;
                        s_ap_list[s_ap_count].open =
                            (rec[i].authmode == WIFI_AUTH_OPEN) ? 1 : 0;
                        s_ap_count++;
                    }
                }
                free(rec);
            }
        }
        ESP_LOGI(TAG, "scan done: %d APs", s_ap_count);

        if (s_phase == PHASE_SCANNING) {
            /* 上电自动连接流程：匹配候选并开始尝试 */
            build_candidates();
            if (s_cand_count > 0) {
                s_cand_idx = 0;
                apply_candidate(&s_candidates[0]);
            } else {
                s_phase = PHASE_IDLE;
                ESP_LOGW(TAG, "no saved Wi-Fi visible, give up (heal timer armed)");
                s_enabled = false; esp_wifi_stop();  /* auto-disable: no saved AP */
                s_wifi_stopped = true;
                heal_timer_arm();                    /* ★ §41：60s 后自愈重扫 */
                notify(WIFI_EVT_ALL_FAILED, NULL);
            }
        } else {
            /* UI 手动扫描 */
            notify(WIFI_EVT_SCAN_DONE, NULL);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip, sizeof(s_ip));
        s_connected = true;
        s_phase = PHASE_IDLE;
        s_have_active = true;
        s_enabled = true;               /* ★ §41：自愈重连成功，UI 开关态归位 */
        esp_timer_stop(s_reconnect_timer);
        esp_timer_stop(s_heal_timer);   /* ★ §41：连上了，自愈表停 */
        ESP_LOGI(TAG, "got ip: %s", s_ip);
        /* ★ P0-1 补盲区：上电账单原来看不出"WiFi 关联 + DHCP"这一段吃了多少内部 RAM。
         *   实测：关联+TLS 那段窗口（splash 与 camera 埋点之间）内部 heap 少 28KB、
         *   largest 从 90,112 掉到 55,296 且不再回升 —— 必须在这里切一刀才定位得到。 */
        MEM_STAGE(TAG, "assoc+dhcp done (got ip)");

        /* 联网成功：保存/置顶凭证到 NVS */
        save_connected_cred();

        notify(WIFI_EVT_GOT_IP, s_ip);
    }
}

void wifi_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();   /* AP 模式需要独立 netif 起 DHCP server */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* ★★ F48：斩断运行期 flash 写入（减少"关中断 + 关 cache"窗口的次数）。
     * 默认 WIFI_STORAGE_FLASH 下，每次 esp_wifi_set_config() 都会把 sta 配置写进 NVS；
     * 而"运行中掉线 → apply_candidate()"这条重连路径正是每次都会调它（见本文件
     * WIFI_EVENT_STA_DISCONNECTED 分支）⇒ 每次重连都擦写一次 flash。
     * flash 擦写走 spi_flash_disable_interrupts_caches_and_other_cpu()：关中断 + 关 cache
     * （PSRAM/flash 共用同一 cache），实测窗口 94~157 ms ⇒ 期间 WiFi/音频都会被打乱。
     * ★ 屏侧后果已变：2026-09-25 起切到 auto refresh 模式（无 bounce buffer），
     *   不再有"中断停摆 ⇒ 补不上 bounce buffer ⇒ 花屏"这条链；这里纯粹是减少抖动。
     * 凭证持久化本文件已有自己的 NVS 表（store_commit），驱动的这份是纯冗余。
     * WIFI_STORAGE_RAM 下 esp_wifi_get_config() 照常可读（save_connected_cred 依赖它）。 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                               &wifi_event_handler, NULL));

    /* 重连定时器（单次，1s） */
    esp_timer_create_args_t targ = {
        .callback = reconnect_timer_cb,
        .name = "wifi_rc",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targ, &s_reconnect_timer));

    /* §41 断连自愈定时器（60s 周期，give up 后武装） */
    esp_timer_create_args_t harg = {
        .callback = heal_timer_cb,
        .name = "wifi_heal",
    };
    ESP_ERROR_CHECK(esp_timer_create(&harg, &s_heal_timer));
    s_wifi_stopped = false;

    s_client_mtx = xSemaphoreCreateMutex();

    /* 默认上电 STA 模式：开发板自动连已保存路由；UI 可切 AP 开热点 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* ★ F48 针对 reason=200 (WIFI_REASON_BEACON_TIMEOUT) 的实验项：
     * 默认 WIFI_PS_MIN_MODEM 让射频在 DTIM 之间休眠，靠软件按时唤醒收 beacon；
     * 本工程早期 CPU 长期被 RGB bounce 中断（1699 次/秒 x ~300 us）占满 ⇒
     * 唤醒编程一迟就漏 beacon ⇒ 连续漏 N 个就报 beacon timeout 断联。
     * ★ 该中断已随 2026-09-25 的 auto refresh 切换消失（现 ~35 次/秒），
     *   但电源域这条**别无副作用**，且仍有其它抖动源（相机/模型加载）⇒ 保留。
     * 本机是市电供电的音箱，放弃省电换稳定是划算的。
     * 若日后要恢复默认省电：删掉本行即可。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    wifi_manager_auto_connect();

    ESP_LOGI(TAG, "wifi STA init done, auto-connecting saved creds");
}

/* AP DHCP 不下发默认网关(option 3)与 DNS(option 6)：
   PC 连上后只保留 192.168.4.0/24 直连路由，不抢占校园网默认上网路由 */
static void ap_setup_dhcp_no_route(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) {
        return;
    }
    esp_netif_dhcps_stop(ap);
    /* 把 AP netif 自身网关置 0，dhcpd 下发的 router(option 3) 即为空 */
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap, &info) == ESP_OK) {
        info.gw.addr = 0;
        esp_netif_set_ip_info(ap, &info);
    }
    uint32_t zero = 0;
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                            ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &zero, sizeof(zero));
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                            ESP_NETIF_DOMAIN_NAME_SERVER, &zero, sizeof(zero));
    esp_netif_dhcps_start(ap);
    ESP_LOGI(TAG, "AP dhcps: no gateway/dns announced");
}

static void start_ap_mode(void)
{
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, AP_DEFAULT_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, AP_DEFAULT_PASS, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(AP_DEFAULT_SSID);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ap_setup_dhcp_no_route();
}

void wifi_manager_set_role(wifi_role_t role)
{
    if (role == s_role) {
        return;
    }
    esp_wifi_stop();
    esp_timer_stop(s_reconnect_timer);
    s_phase = PHASE_IDLE;
    s_have_active = false;
    s_connected = false;
    s_ip[0] = 0;
    s_role = role;

    if (role == WIFI_ROLE_AP) {
        start_ap_mode();
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_manager_auto_connect();   /* 切回 STA：读 NVS 自动连 */
    }
}

wifi_role_t wifi_manager_get_role(void) { return s_role; }

const char *wifi_manager_get_ap_ssid(void) { return AP_DEFAULT_SSID; }

const char *wifi_manager_get_sta_ssid(void) { return s_cur_ssid; }

/* 以硬件真实关联列表为准清理幽灵客户端（软断开可能收不到 DISCONNECT 事件） */
static void reconcile_clients_locked(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return;
    }
    for (int i = 0; i < AP_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) {
            continue;
        }
        bool found = false;
        for (int j = 0; j < list.num; j++) {
            if (memcmp(s_clients[i].mac, list.sta[j].mac, 6) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGW(TAG, "purge stale client (no longer associated)");
            s_clients[i].used = false;
            s_clients[i].ip[0] = 0;
        }
    }
}

int wifi_manager_get_client_count(void)
{
    int n = 0;
    if (xSemaphoreTake(s_client_mtx, portMAX_DELAY) == pdTRUE) {
        reconcile_clients_locked();
        for (int i = 0; i < AP_MAX_CLIENTS; i++) {
            if (s_clients[i].used) {
                n++;
            }
        }
        xSemaphoreGive(s_client_mtx);
    }
    return n;
}

int wifi_manager_get_client_ips(char (*out)[16], int max)
{
    int n = 0;
    if (xSemaphoreTake(s_client_mtx, portMAX_DELAY) == pdTRUE) {
        reconcile_clients_locked();
        for (int i = 0; i < AP_MAX_CLIENTS && n < max; i++) {
            if (s_clients[i].used) {
                strncpy(out[n], s_clients[i].ip, 15);
                out[n][15] = 0;
                n++;
            }
        }
        xSemaphoreGive(s_client_mtx);
    }
    return n;
}

void wifi_manager_auto_connect(void)
{
    memset(&s_pending_store, 0, sizeof(s_pending_store));
    if (store_load(&s_pending_store) != ESP_OK || s_pending_store.count == 0) {
        ESP_LOGI(TAG, "no saved Wi-Fi in NVS, skip auto-connect");
        return;
    }
    ESP_LOGI(TAG, "auto-connect: %d saved Wi-Fi, scanning...",
             s_pending_store.count);

    esp_timer_stop(s_reconnect_timer);
    s_phase = PHASE_SCANNING;
    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = true;       /* 允许发现隐藏 SSID */
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&scan_cfg, false);
}

void wifi_manager_set_evt_cb(wifi_evt_cb_t cb) { s_evt_cb = cb; }
void wifi_manager_set_sys_cb(wifi_evt_cb_t cb) { s_sys_cb = cb; }

void wifi_manager_start_scan(void)
{
    ESP_LOGI(TAG, "manual scan start");
    s_phase = PHASE_IDLE;              /* 手动扫描不影响后续手动连接 */
    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&scan_cfg, false);
}

int wifi_manager_get_scan_results(wifi_ap_info_t *out, int max)
{
    int n = (s_ap_count < max) ? s_ap_count : max;
    memcpy(out, s_ap_list, n * sizeof(wifi_ap_info_t));
    return n;
}

void wifi_manager_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);

    /* 手动连接：候选表只有 1 项，复用同一套重试逻辑 */
    memset(s_candidates, 0, sizeof(s_candidates));
    strncpy(s_candidates[0].ssid, ssid, WIFI_SSID_LEN_MAX);
    if (pass) {
        strncpy(s_candidates[0].password, pass, WIFI_PASS_LEN_MAX);
    }
    s_cand_count = 1;
    s_cand_idx = 0;
    apply_candidate(&s_candidates[0]);
}

bool wifi_manager_is_connected(void) { return s_connected; }

void wifi_manager_set_enabled(bool on)
{
    s_enabled = on;
    if (on) {
        ESP_ERROR_CHECK(esp_wifi_start());
        if (s_role == WIFI_ROLE_AP) {
            /* AP mode starts DHCP in start_ap_mode already; just restart */
            start_ap_mode();
        } else {
            wifi_manager_auto_connect();
        }
        ESP_LOGI(TAG, "wifi enabled");
    } else {
        esp_timer_stop(s_reconnect_timer);
        s_phase = PHASE_IDLE;
        s_have_active = false;
        s_connected = false;
        s_ip[0] = 0;
        esp_wifi_stop();
        ESP_LOGI(TAG, "wifi disabled (radio off)");
    }
    notify(WIFI_EVT_DISCONNECTED, NULL);
}

bool wifi_manager_is_enabled(void) { return s_enabled; }

void wifi_manager_get_ip(char *buf, int len)
{
    if (!buf || len <= 0) {
        return;
    }
    buf[0] = 0;
    if (s_connected) {
        strncpy(buf, s_ip, len - 1);
        buf[len - 1] = 0;
    }
}

int wifi_manager_get_saved_count(void)
{
    wifi_store_t store;
    if (store_load(&store) != ESP_OK) {
        return 0;
    }
    return store.count;
}

esp_err_t wifi_manager_forget(const char *ssid)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_store_t store;
    if (store_load(&store) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    int found = -1;
    for (int i = 0; i < store.count; i++) {
        if (strcmp(store.creds[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    memmove(&store.creds[found], &store.creds[found + 1],
            (store.count - found - 1) * sizeof(wifi_cred_t));
    store.count--;
    return store_commit(&store);
}
