/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bt_audio_service.c
 * @brief 蓝牙 A2DP 中间层实现（封装 esp_bt_audio）。
 *
 * 阶段一：电源开关 + host init/deinit + 事件回调 + 扫描/连接控制。
 * Source 推送的本地文件数据流（local2bt GMF pipeline）在后续阶段接入。
 */

#include "bt_audio_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"   /* ★ F35：xTaskCreateWithCaps */

#include "esp_bt_audio.h"
#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_event.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"
#include "esp_bt_audio_vol.h"

#include "bt_push_service.h"

#include "middleware/system/button_service.h"
#include "middleware/audio/session_manager.h"

static const char *TAG = "BT_SVC";

static bt_audio_evt_cb_t s_evt_cb = NULL;
static void *s_evt_ctx = NULL;
static bool s_on = false;
static bool s_controller_ready = false;
static bt_role_t s_role = BT_ROLE_SOURCE;
static bt_device_t s_found_dev;
static uint8_t s_last_addr[6] = {0};
static bool s_last_addr_valid = false;

/* ---- 连接状态：音量分发据此决定"归蓝牙管"还是"归本地 codec 管" ---- */
static volatile bool s_connected = false;

/* ---- 蓝牙音量状态 ----
 * 音量体系换算（★ 三层量纲，别搞混）：
 *   ① 本地 s_volume        0..90    （button_service 的 VOL_MAX=90）
 *   ② esp_bt_audio 应用层  0..100   ← 我们调 esp_bt_audio_vol_set_absolute 时传这个
 *   ③ AVRCP absolute       0..127   ← 协议线上值，由库内部转换，我们永远不直接碰
 *
 * ★★ 不要把 ②③ 弄反（这正是 2026-09-21 修复的 bug）：
 *   库内 bt_audio_avrcp_ct.c:136 的 vol_to_avrc() 期望入参是 **0..100**：
 *       return (uint8_t)((app_vol * 127) / 100);
 *   早期版本我们在 local_vol_to_avrcp() 里已经先映射到了 0..127，
 *   库再乘一次 127/100 → 双重映射溢出。90 档时 127*127/100 = 161，
 *   超出 AVRCP 上限 127 被截断，耳机音量条直接顶满。
 *   所以本文件的映射目标必须是 **0..100**，不是 0..127。
 *
 * 关于「绝对 vs 相对」：
 *   esp_bt_audio 的 vol_ops.set_absolute 在 AVRCP CT **连接建立**后才会注册
 *   （见 bt_audio_avrcp_ct.c:567-601，触发事件是 ESP_AVRC_CT_CONNECTION_STATE_EVT
 *    且 conn_stat.connected），未注册时 esp_bt_audio_vol_set_absolute 会打出
 *   "vol_ops.set_absolute is not registered" 并返回 ESP_ERR_INVALID_STATE。
 *   也就是说：**耳机支不支持 Absolute Volume，协议层返回都是 ESP_OK**，
 *   我们无法从返回值判断成功与否。因此不做"自动降级"（那只是假象）。
 *   默认走绝对音量（A2DP 1.3+ 耳机普遍支持，且两端档位严格一致）；
 *   遇到个别老耳机不听指挥时，用 bt_audio_service_set_abs_volume(false)
 *   运行时切到相对音量模式。
 *
 * ★★★ 前提条件（2026-09-21 补）：上面那句"连接建立后就会注册"在本工程
 *   曾经**永远不成立** —— 上游 A2DP SRC 路径漏调 BTA_AvOpenRc()，导致
 *   AVRCP CT 连接从未建立。已在本文件用 avrcp_ensure_ct_link() 修复，
 *   详见该函数上方注释。
 */
#define BT_VOL_APP_MAX      100   /* esp_bt_audio 应用层量纲上限（勿改） */
#define BT_VOL_LOCAL_MAX    90

/* ---- 开蓝牙 host 前的内部 RAM 门槛 ----
 * bluedroid host 起来时要一口气建出 BTU/BTC 任务、各层队列和信号量。
 * 这些分配一旦失败，库内部会直接 assert 崩溃（future_ready / btc_config_init），
 * 而不是返回错误码 —— 实测过。所以开之前先体检，不够就**干脆不开**：
 * 功能不可用好过整机重启。
 * 注：这里只做"开之前"的闸门；host 每次开关都是真正的 init/deinit（见下方
 * set_power 的说明），所以这道闸门每次打开都要走一遍，不能只在开机时检查。
 *
 * ★ 门槛值的来历（2026-09-21 实测）：
 *   修 F30 之前，实测 free=25987 / largest=23552 时开蓝牙**必崩** ——
 *   即 25KB 是不够的。当时定 48KB/24KB 为保守值。
 *   修 F30 之后（750KB JPEG 缓冲移出内部 RAM），内部 RAM 应大幅回升，
 *   正常应能轻松满足；若实测仍偏紧，可按下表调整：
 *     free 余量 ≈ 峰值需求 × 1.5
 *   调参依据：看日志里 "mem check ok: internal free=... largest=..."，
 *   只要开机后 + 各功能满载时都能满足，就说明门槛合理。
 * 诊断口径见 docs/工程规范/14_堆管理宝贵经验.md §1.6.5「三种假性内存不足」：
 *   free 小 + largest 小 → 真不够；free 大 + largest 小 → 碎片化。两者都拦。
 */
#define BT_HOST_MIN_FREE_INTERNAL     (48 * 1024)
#define BT_HOST_MIN_LARGEST_BLOCK     (24 * 1024)

/* 记录上次成功下发的目标档位（本地量纲），相对模式据此算差值 */
static int s_last_sent_vol = -1;
/* 是否使用绝对音量（true: 绝对，收音机式直接设定；false: 相对，逐格加减） */
static bool s_use_abs_vol = true;
/* vol sync 短命任务是否在跑。
 * ★ 用途是**防重入**：连接事件可能因重连/双角色回调被触发多次，
 *   若不拦就会并发创建多个 sync_vol_task，同时下发音量造成跳档与任务泄漏。
 *   由事件回调置 true，由 sync_vol_task 在每条退出路径（含提前 return）置 false。 */
static volatile bool s_sync_task_running = false;

/* 本地档位(0..90) → esp_bt_audio 应用层量纲(0..100)。
 * ★ 目标量纲是 0..100 而非 0..127 —— 库内部还会再乘一次 127/100，
 *   这里若直接给 127 就会双重映射溢出（见文件上方 BT_VOL_APP_MAX 注释）。
 * 四舍五入：vol/90*100 */
static int local_vol_to_app(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > BT_VOL_LOCAL_MAX) vol = BT_VOL_LOCAL_MAX;
    return (vol * BT_VOL_APP_MAX + BT_VOL_LOCAL_MAX / 2) / BT_VOL_LOCAL_MAX;
}

/* ============================================================================
 * AVRCP CT 连接补建（★ 2026-09-21 修复 "set_absolute is not registered"）
 * ============================================================================
 * 现象：
 *   A2DP 推流一切正常，但按键调音量必失败：
 *     E BT_AUD_OPS: Operation failed: vol_ops.set_absolute is not registered
 *     W BT_SVC:      set abs vol failed: ESP_ERR_INVALID_STATE
 *
 * 根因（上游 ESP-IDF bluedroid A2DP SRC 路径缺陷）：
 *   vol_ops.set_absolute 只在 ESP_AVRC_CT_CONNECTION_STATE_EVT 且 connected=true
 *   时注册（bt_audio_avrcp_ct.c:567-601）。而该事件的源头是 BTA_AV_RC_OPEN_EVT
 *   → btc_rc_handler → handle_rc_connect → btc_avrc_ct_cb_to_app()。
 *   要产生 BTA_AV_RC_OPEN_EVT，BTA 层必须真正建立 AVRCP 连接：
 *
 *     BTA_AvOpenRc(handle)                       [bta_av_api.c:571]
 *       → BTA_AV_API_RC_OPEN_EVT
 *       → bta_av_rc_opened()                     [bta_av_act.c:465]
 *       → (*p_cback)(BTA_AV_RC_OPEN_EVT, ...)    [bta_av_act.c:551]
 *
 *   而 BTA_AvOpenRc() 在整份 IDF 里**只有一个调用点**：
 *     btc_av.c:561，位于 `if (btc_av_cb.peer_sep == AVDT_TSEP_SRC)` 分支内。
 *   本工程角色是 A2DP SRC，连接建立后 peer_sep == AVDT_TSEP_SNK（对端是 sink），
 *   于是走 btc_av.c:565 的 else-if 分支 —— **那一支里没有 BTA_AvOpenRc()**。
 *   结果：AVRCP CT 连接从未建立 → vol_ops 从未注册 → 绝对/相对音量全废。
 *
 *   旁证（源码内部自相矛盾，证明这是缺陷而非设计）：
 *     - btc_av.c:861 / 1050（src 断开路径）：peer_sep == AVDT_TSEP_SRC 才 CloseRc
 *     - btc_av.c:2079（src deinit 路径）：peer_sep == AVDT_TSEP_SNK 才 CloseRc ← 相反
 *     - bta_av_aact.c:3010 的 bta_av_open_rc() 里 `p_scb->use_rc` 才是设计中的
 *       开关，但 bta_av_open_rc() 全 IDF 无调用点（死函数）。
 *
 * 修法：应用层主动补这一脚。
 *   1) 拿 BTA handle：btc_a2d_conn_handle_get()（btc_av.c:2275，公开非 static，
 *      声明在 btc_av.h:313，本工程固件已链接，map 里 .text 地址 0x400d19ae）。
 *      它返回的就是 btc_av_cb.bta_handle —— 与 btc_av.c:561 用的是同一个值，
 *      即 BTA_AvRegister(BTA_AV_CHNL_AUDIO) 分配出的 hndl（本工程 = (0+1)|0x40 = 0x41）。
 *   2) 调 BTA_AvOpenRc(handle)（map 地址 0x400f49c4）。
 *      ★ 必须走这条旁路，**不能**再调一次 esp_a2d_source_connect()：
 *        btc_profile_queue.c:43 的连接队列按 uuid 去重，同 uuid 会被静默丢弃。
 *
 * 为什么不用 IDF 私有头：
 *   两个函数都在 libbt.a 里是全局符号；直接 extern 声明可避免把
 *   btc/profile/std/include 这类内部目录加进本组件的 INCLUDE_DIRS，
 *   也就不会在上游 minor 升级时因头文件路径变动而编译失败。
 *
 * 时机：必须在 A2DP 连接**完全建立之后**（此时 SCB 已存在，bta_av_rc_opened
 *   才能通过 peer_addr 匹配到 SCB 并把 rc_handle 回填给 p_scb）。
 *   放在 sync_vol_task 的 800ms 延时窗口里，天然晚于连接事件。
 * ==========================================================================*/
extern uint16_t btc_a2d_conn_handle_get(void);
extern void     BTA_AvOpenRc(uint16_t handle);

/* 只在每次 A2DP 连接后补建一次，避免重复下发 */
static bool s_avrcp_link_requested = false;

/* AVRCP CT 补建的重试账本。
 * ★ 2026-09-22：原实现把"已发出请求"当成"已建立连接"—— BTA_AvOpenRc() 一发出
 *   就把 s_avrcp_link_requested 置 true，若 RC 建立失败（对端拒绝 / 超时 /
 *   BTA 侧尚未就绪）标志仍为 true，后续**永不再试** → 死锁，
 *   表现为"按键永远 set_absolute is not registered"。
 *   现在改为：只有确认建链成功（收到 CT 连接事件）才置 true；
 *   失败则留在 false，下次按键或下次 sync 任务会再试。
 *   另加重试计数上限，避免对端根本不支持时无限重试刷屏。 */
static int s_avrcp_retry = 0;
#define AVRCP_MAX_RETRY  5

/* 绝对音量连续失败计数：到阈值就自动降级到相对模式。
 * ★ 意义：与其让用户面对"按键永远没反应"，不如降级成"能调但由耳机自己按格走"。
 *   相对模式下发的是 AV/C 面板 passthrough 按键，不依赖 AVRCP CT 连接。 */
static int s_abs_fail_count = 0;
#define ABS_FAIL_DOWNGRADE  3

/* 前向声明：bt_audio_service_init() 在定义之前注册它 */
static void on_push_finished(void);

static void avrcp_ensure_ct_link(void)
{
    if (s_avrcp_link_requested) {
        return;   /* 已确认建链成功，无需重复 */
    }

    uint16_t hndl = btc_a2d_conn_handle_get();
    if (hndl == 0) {
        /* bta_handle 还没被 BTA_AV_REGISTER_EVT 填上，说明 BTA 侧尚未就绪。
           ★ 不置 s_avrcp_link_requested —— 让调用方稍后可以再试。 */
        ESP_LOGW(TAG, "avrcp link: bta handle not ready (retry %d/%d), skip",
                 s_avrcp_retry, AVRCP_MAX_RETRY);
        s_avrcp_retry++;
        return;
    }

    if (s_avrcp_retry >= AVRCP_MAX_RETRY) {
        /* 已试够次数仍未成功：大概率对端不支持 AVRCP CT（或拒绝了 RC 通道）。
           ★ 不再重试，避免刷屏；音量将走相对模式兜底（见 set_volume）。 */
        ESP_LOGW(TAG, "avrcp link: giving up after %d tries (peer likely no AVRCP CT)",
                 s_avrcp_retry);
        s_avrcp_link_requested = true;   /* 置位以止住重试；语义改为"已放弃" */
        return;
    }

    BTA_AvOpenRc(hndl);
    s_avrcp_retry++;
    ESP_LOGI(TAG, "avrcp link: BTA_AvOpenRc(0x%04x) issued (try %d/%d)",
             hndl, s_avrcp_retry, AVRCP_MAX_RETRY);
    /* ★ 注意：这里**不置** s_avrcp_link_requested —— 等 CT 连接事件确认。 */
}

/* ★ 为什么不用"CT 连接事件"来确认建链成功：
 *   esp_bt_audio 的公开事件枚举里**没有** AVRCP CT 连接事件
 *   （只有 CONNECTION_STATE_CHG / VOL_ABSOLUTE / VOL_RELATIVE 等，见
 *   esp_bt_audio_event.h）。库内部在 bt_audio_avrcp_ct.c:567 收到
 *   ESP_AVRC_CT_CONNECTION_STATE_EVT 时才注册 vol_ops.set_absolute，
 *   但那个事件**不透出到应用层**，我们无从直接观测。
 *
 *   因此改用**主动探测**：直接调一次 set_absolute，
 *   返回 ESP_OK → 说明已注册（链路通）；返回 INVALID_STATE → 还没好。
 *   这是唯一可靠的判据，因为库对"耳机支不支持绝对音量"一律返回 ESP_OK，
 *   只有"未注册"才返回 INVALID_STATE。
 *
 * ★ 探测副作用：成功时耳机会真的被设一次音量。这是可接受的 ——
 *   探测值就是当前档位，本来就是我们要下发的值。
 */
static bool avrcp_probe_ready(void)
{
    if (s_avrcp_link_requested) {
        return true;
    }
    esp_err_t ret = esp_bt_audio_vol_set_absolute((uint32_t)local_vol_to_app(
                        s_last_sent_vol >= 0 ? s_last_sent_vol : BT_VOL_LOCAL_MAX / 2));
    if (ret == ESP_OK) {
        s_avrcp_link_requested = true;
        s_avrcp_retry = 0;
        ESP_LOGI(TAG, "avrcp link: CONFIRMED ready (probe set_absolute OK)");
        return true;
    }
    return false;
}


static void sync_vol_task(void *arg)
{
    (void)arg;
    /* 等 A2DP 连接完全建立（连接事件通常早于 SCB/profile 就绪） */
    vTaskDelay(pdMS_TO_TICKS(800));

    if (!s_connected || !s_on) {
        s_sync_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* ① 补建 AVRCP CT 连接（上游 SRC 路径漏了这一脚，详见函数上方注释），
          然后用**主动探测**确认是否真的可用。
          ★ 2026-09-22：BTA 侧就绪时刻不确定（首次 RF 全校准 + 136kHz RC 慢时钟
          都会让链路建立明显变慢），单次尝试极易打空。改为轮询：
          每 300ms 一轮，每轮「补建一次 + 探测一次」，最多 10 轮（≈3s 窗口）。 */
    for (int i = 0; i < 10; i++) {
        if (!s_connected || !s_on) {
            s_sync_task_running = false;
            vTaskDelete(NULL);
            return;
        }
        avrcp_ensure_ct_link();
        if (avrcp_probe_ready()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!s_avrcp_link_requested) {
        ESP_LOGW(TAG, "avrcp link: still not ready after ~3s, "
                      "vol will use relative fallback");
        s_use_abs_vol = false;
    }

    if (!s_connected || !s_on) {
        s_sync_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* ② 下发当前档位 */
    int vol = button_service_get_volume();
    int target = local_vol_to_app(vol);
    if (s_use_abs_vol) {
        esp_err_t ret = esp_bt_audio_vol_set_absolute((uint32_t)target);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "vol sync to headset: local %d -> app %d (ret=%s)",
                     vol, target, esp_err_to_name(ret));
            s_last_sent_vol = vol;
        } else {
            /* ★ 绝对音量用不了 —— 自动降级到相对模式，保证"按键能调音量"
                 这个最基本诉求。相对模式走 AV/C 面板按键，不依赖 CT 连接。 */
            ESP_LOGW(TAG, "vol sync: abs failed (%s), fallback to relative mode",
                     esp_err_to_name(ret));
            s_use_abs_vol = false;
            s_last_sent_vol = vol;   /* 相对模式以当前档位为基准 */
        }
    } else {
        s_last_sent_vol = vol;   /* 相对模式也以当前档位为基准 */
    }

    s_sync_task_running = false;
    vTaskDelete(NULL);
}

/* 当前阶段只做 Source（推送 SD 音乐到耳机）；Sink 模式后续再开。
   同时注册 SRC+SNK 会导致 esp_a2d_sink_init 返回 INVALID_STATE（profile 状态机冲突）。 */
static uint32_t classic_roles(void)
{
    return ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC |
           ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT;
}

/* ---- esp_bt_audio 事件回调（跑在 BT/GMF 线程，只转发事件） ---- */
static void on_bt_audio_event(esp_bt_audio_event_t event, void *event_data, void *user_data)
{
    (void)user_data;
    switch (event) {
    case ESP_BT_AUDIO_EVENT_DISCOVERY_STATE_CHG: {
        esp_bt_audio_event_discovery_st_t *d = (esp_bt_audio_event_discovery_st_t *)event_data;
        ESP_LOGI(TAG, "discovery %s", d->discovering ? "start" : "stop");
        if (s_evt_cb) s_evt_cb(d->discovering ? BT_EVT_DISCOVERY_START : BT_EVT_DISCOVERY_STOP, NULL, s_evt_ctx);
        break;
    }
    case ESP_BT_AUDIO_EVENT_DEVICE_DISCOVERED: {
        esp_bt_audio_event_device_discovered_t *dev = (esp_bt_audio_event_device_discovered_t *)event_data;
        if (dev->tech != ESP_BT_AUDIO_TECH_CLASSIC) break;
        /* 去重：同一 MAC 重复上报只回调一次，rssi 更新放 UI 层不管 */
        if (s_last_addr_valid && memcmp(s_last_addr, dev->addr, 6) == 0) break;
        memcpy(s_last_addr, dev->addr, 6);
        s_last_addr_valid = true;
        memcpy(s_found_dev.name, dev->name, sizeof(s_found_dev.name));
        memcpy(s_found_dev.addr, dev->addr, 6);
        s_found_dev.rssi = dev->rssi;
        ESP_LOGI(TAG, "found: %s [%02x:%02x:%02x:%02x:%02x:%02x] rssi=%d",
                 s_found_dev.name, dev->addr[0], dev->addr[1], dev->addr[2],
                 dev->addr[3], dev->addr[4], dev->addr[5], dev->rssi);
        if (s_evt_cb) s_evt_cb(BT_EVT_DEVICE_FOUND, &s_found_dev, s_evt_ctx);
        break;
    }
    case ESP_BT_AUDIO_EVENT_VOL_RELATIVE: {
        esp_bt_audio_event_vol_relative_t *v = (esp_bt_audio_event_vol_relative_t *)event_data;
        ESP_LOGI(TAG, "VOL_REL from headset: up=%d", (int)v->up_down);
        break;
    }
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        esp_bt_audio_event_connection_st_t *c = (esp_bt_audio_event_connection_st_t *)event_data;
        ESP_LOGI(TAG, "connection %s addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 c->connected ? "CONNECTED" : "DISCONNECTED",
                 c->addr[0], c->addr[1], c->addr[2], c->addr[3], c->addr[4], c->addr[5]);
        s_connected = c->connected;
        if (!c->connected) {
            /* 断开后重置音量下发基准：重连时 sync_vol_task 会重新下发一次当前档位。
               注意这里**不重置模式开关 s_use_abs_vol** —— 它是对端能力的选择结果
               （由 bt_audio_service_set_abs_volume 设置），换一副耳机前一直有效。
               同理重置 AVRCP 补建标志：BTA 侧 RC 连接已随 A2DP 一起关闭，
               下次连上需要重新补建。 */
            s_last_sent_vol = -1;
            s_avrcp_link_requested = false;
            s_avrcp_retry = 0;   /* 重连给足重试预算 */
            /* ★ 不复位 s_use_abs_vol / s_abs_fail_count：降级是对端能力的结果，
               换一副耳机前一直有效（与 s_use_abs_vol 语义一致）。 */
        }
        if (s_evt_cb) s_evt_cb(c->connected ? BT_EVT_CONNECTED : BT_EVT_DISCONNECTED, c->addr, s_evt_ctx);
        if (c->connected) {
            /* 连上后把本地当前档位同步到耳机，避免屏幕显示与耳机实际音量不一致。
               AVRCP 通常比 A2DP 稍晚就绪，起一个短延时任务错开这个窗口。 */
            if (!s_sync_task_running) {
                s_sync_task_running = true;
                /* 2.5KB 短命任务（同步完即退出）。也一并外置 PSRAM（F35）：
                   本文件在"内存最紧张"的 BT 连上瞬间被调用，正是最不能夺内部 RAM 的时刻。 */
                BaseType_t ok = xTaskCreateWithCaps(sync_vol_task, "bt_vol_sync", 2560, NULL, 5, NULL,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (ok != pdPASS) {
                    s_sync_task_running = false;
                    ESP_LOGW(TAG, "vol sync task create failed (stack->PSRAM)");
                }
            }
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;
        ESP_LOGI(TAG, "stream %p state=%d", st->stream_handle, st->state);

        /* 转发给推流服务：带 stream_handle + 完整四态语义
           （ALLOCATED / STARTED / STOPPED / RELEASED），由它驱动 local2bt pipeline */
        bt_push_service_on_stream_state(st->stream_handle, st->state);

        /* UI 层仍按原 BOOL 语义收（page_bluetooth 不用改）：
           只有真正 STARTED 才算"正在推流" */
        if (s_evt_cb) {
            bool on = (st->state == ESP_BT_AUDIO_STREAM_STATE_STARTED);
            s_evt_cb(on ? BT_EVT_STREAM_ON : BT_EVT_STREAM_OFF, NULL, s_evt_ctx);
        }
        break;
    }
    case ESP_BT_AUDIO_EVENT_MEDIA_CTRL_CMD: {
        esp_bt_audio_event_media_ctrl_t *m = (esp_bt_audio_event_media_ctrl_t *)event_data;
        ESP_LOGI(TAG, "media ctrl cmd=%d", m->cmd);
        if (s_evt_cb) s_evt_cb(BT_EVT_MEDIA_CMD, &m->cmd, s_evt_ctx);
        break;
    }
    case ESP_BT_AUDIO_EVENT_VOL_ABSOLUTE: {
        /* ★ 耳机主动设定音量（CT→TG 的 VOLUME_CHANGE 通知）。
           这条能出现 = AVRCP 控制通道确实通了，是"set_absolute 是否已注册"的关键旁证。 */
        esp_bt_audio_event_vol_absolute_t *v = (esp_bt_audio_event_vol_absolute_t *)event_data;
        ESP_LOGI(TAG, "VOL_ABS from headset: vol=%u mute=%d (AVRCP alive)",
                 (unsigned)v->vol, (int)v->mute);
        if (s_evt_cb) s_evt_cb(BT_EVT_VOL_ABSOLUTE, &v->vol, s_evt_ctx);
        break;
    }
    default:
        ESP_LOGD(TAG, "unhandled event %d", event);
        break;
    }
}

void bt_audio_service_init(bt_audio_evt_cb_t cb, void *ctx)
{
    s_evt_cb = cb;
    s_evt_ctx = ctx;
    /* ★ 2026-09-22：注册"播完自动收尾"回调（修复"音乐播完不退出"） */
    bt_push_service_set_finished_cb(on_push_finished);
    ESP_LOGI(TAG, "service ready (power off)");
}

/* 开机早期调用：此时 LCD/WiFi/摄像头/GMF 都还没起，内部 RAM 最空最连续，
   controller 的 HCI 内存池一次性拿到。之后常驻，set_power 只开关 host。 */
void bt_audio_service_controller_init(void)
{
    ESP_LOGI(TAG, "early controller init: free internal=%d largest=%d free PSRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s (BT disabled)", esp_err_to_name(ret));
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s (BT disabled)", esp_err_to_name(ret));
        esp_bt_controller_deinit();
        return;
    }
    s_controller_ready = true;
    ESP_LOGI(TAG, "controller ready (host off)");
}

/* bluedroid host 起来时要一口气建出 BTU/BTC 任务、各层队列和信号量。
   这些分配一旦失败，库内部会直接 assert 崩溃（future_ready / btc_config_init），
   而不是返回错误码 —— 实测过。所以开之前先体检，不够就**干脆不开**：
   功能不可用好过整机重启。
   注：这里只做"开之前"的闸门；host 每次开关都是真正的 init/deinit（见下方
   set_power 的说明），所以这道闸门每次打开都要走一遍，不能只在开机时检查。 */
static bool host_mem_sufficient(void)
{
    const size_t need_free    = BT_HOST_MIN_FREE_INTERNAL;
    const size_t need_largest = BT_HOST_MIN_LARGEST_BLOCK;

    size_t free_sz = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (free_sz < need_free || largest < need_largest) {
        ESP_LOGE(TAG, "internal RAM too low for BT host: free=%u (need %u) "
                      "largest=%u (need %u) -- refuse to power on",
                 (unsigned)free_sz, (unsigned)need_free,
                 (unsigned)largest, (unsigned)need_largest);
        ESP_LOGE(TAG, "hint: close running features (WiFi/GMF pipeline/recording) first");
        /* ★ 只看 Internal 总量是**查不出凶手**的（F35）。必须连 PSRAM 一起打：
           若 PSRAM 同时告急 → 是"总量/泄漏"问题；若 PSRAM 尚有余而 Internal
           只剩个位数 KB → 是"内部 RAM 专项被吃"，凶手一定是**只用内部 RAM 的
           大额消费者**（TLS / LWIP / LVGL / bluedroid 这类），而不是业务缓冲
           （业务缓冲都已显式 PSRAM）。另外 RTCRAM(31KB) 与"内部 32KB DMA 保留池"
           都不计入这里的 free，所以 free 会显著小于开机时的 210KB 物理减损。 */
        ESP_LOGE(TAG, "[diag] internal free=%u largest=%u | DMA-capable free=%u largest=%u | "
                      "PSRAM free=%u largest=%u",
                 (unsigned)free_sz, (unsigned)largest,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return false;
    }
    ESP_LOGI(TAG, "mem check ok: internal free=%u largest=%u (need %u/%u)",
             (unsigned)free_sz, (unsigned)largest,
             (unsigned)need_free, (unsigned)need_largest);
    return true;
}

esp_err_t bt_audio_service_set_power(bool on)
{
    if (on && s_on) return ESP_OK;
    if (!on && !s_on) return ESP_OK;

    if (on) {
        if (!s_controller_ready) {
            ESP_LOGE(TAG, "controller not ready (early init failed)");
            return ESP_ERR_INVALID_STATE;
        }
        /* ★ 内部 RAM 体检必须放在 esp_bt_audio_init() 之前：
           进去之后再失败就没法优雅退出了（对方内部会 assert）。 */
        if (!host_mem_sufficient()) {
            return ESP_ERR_NO_MEM;
        }
        /* Bluedroid host 配置：Just Works（IO_CAP_NONE），无需耳机端确认 */
        esp_bt_audio_host_bluedroid_cfg_t host_cfg = ESP_BT_AUDIO_HOST_BLUEDROID_CFG_DEFAULT();
        snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "Korvo_%02X%02X", 0, 0);
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BT);
        snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "Korvo_%02X%02X", mac[4], mac[5]);
        host_cfg.iocap = ESP_BT_IO_CAP_NONE;

        esp_bt_audio_config_t cfg = {0};
        cfg.host_config = &host_cfg;
        cfg.event_cb = on_bt_audio_event;
        cfg.event_user_ctx = NULL;
        cfg.classic.roles = classic_roles();
        cfg.classic.a2dp_src_send_task_core_id = 1;
        cfg.classic.a2dp_src_send_task_prio = 10;
        cfg.classic.a2dp_src_send_task_stack_size = 4096;

        esp_err_t ret = esp_bt_audio_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_audio_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        /* host 起来后再建 local2bt pipeline，避免 io_bt 在 BT 未就绪时半初始化 */
        s_on = true;
        ESP_LOGI(TAG, "ON, device name=%s, role=%s", host_cfg.dev_name,
                 s_role == BT_ROLE_SOURCE ? "SOURCE" : "SINK");
        if (s_evt_cb) s_evt_cb(BT_EVT_ON, NULL, s_evt_ctx);
    } else {
        esp_bt_audio_deinit();
        s_on = false;
        ESP_LOGI(TAG, "OFF (controller stays resident)");
        if (s_evt_cb) s_evt_cb(BT_EVT_OFF, NULL, s_evt_ctx);
    }
    return ESP_OK;
}

bool bt_audio_service_is_on(void) { return s_on; }

void bt_audio_service_set_role(bt_role_t role)
{
    s_role = role;
    ESP_LOGI(TAG, "role -> %s", role == BT_ROLE_SOURCE ? "SOURCE" : "SINK");
}

bt_role_t bt_audio_service_get_role(void) { return s_role; }

esp_err_t bt_audio_service_start_discovery(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    s_last_addr_valid = false;   /* 新一轮扫描允许同一设备再次出现 */
    ESP_LOGI(TAG, "start discovery");
    return esp_bt_audio_classic_discovery_start();
}

esp_err_t bt_audio_service_stop_discovery(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    return esp_bt_audio_classic_discovery_stop();
}

esp_err_t bt_audio_service_connect(const uint8_t *addr)
{
    if (!s_on || !addr) return ESP_ERR_INVALID_STATE;
    /* connect 的 role 参数要传单个 profile（A2DP_SRC=0x1），不是 bitmask */
    ESP_LOGI(TAG, "connect role=SRC addr=%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    esp_err_t ret = esp_bt_audio_classic_connect(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, (uint8_t *)addr);
    ESP_LOGI(TAG, "classic_connect ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bt_audio_service_disconnect(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    /* 断开用最近连接地址：这里由组件内部维护，传 NULL 由组件处理；
       经典蓝牙一次只连一台，直接调 media_stop + disconnect 当前 role。 */
    esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    return ESP_OK;
}

/* ============================================================================
 * ★ 2026-09-22 修复"音乐播完不自动退出"
 * ============================================================================
 * 现象：整首播完后 GMF 打 "playback finished"，但设备端**仍认为在播** ——
 *   A2DP 流保持 STARTED，session 不释放，UI 的 Pause 按钮停在"暂停/继续"态。
 *   用户必须手动点 Stop 才能回到空闲。
 *
 * 根因：bt_push_service 的 pipe_event_cb 收到 FINISHED 只做了
 *   `s_playing = false`，**没有停 A2DP 流、没有释放 session**。
 *   而 A2DP 流不停，蓝牙协议层就不会发 STOPPED 事件，
 *   于是 bt_push_service_on_stream_state() 里的收尾分支永远不被触发 ——
 *   整条收尾链路断在第一步。
 *
 * 修复：push 层在 FINISHED/ERROR 时回调 here，由本函数做完整收尾。
 *   ★ 关键约束：回调运行在 **GMF 事件线程**，而
 *     esp_bt_audio_media_stop() 是阻塞调用（内部要等流真正停下），
 *     直接在事件线程里调用会造成**重入死锁**（等的事件正是当前线程该处理的）。
 *     所以这里起一个 2KB 短命任务（栈外置 PSRAM）来执行收尾。
 */
static void push_finish_cleanup_task(void *arg)
{
    (void)arg;
    /* 让 GMF 事件线程先把当前事件处理完退出，再动手停流 */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "auto-stop: stopping A2DP stream & releasing session");
    esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    session_manager_end(SESSION_BT_PUSH);   /* ★ 这一步此前完全缺失 */
    bt_push_service_clear_finished();
    s_last_sent_vol = -1;

    /* 通知 UI 回到"已停止"态（page_bluetooth / tab_btpush 复用 BT_EVT_STREAM_OFF） */
    if (s_evt_cb) {
        s_evt_cb(BT_EVT_STREAM_OFF, NULL, s_evt_ctx);
    }

    vTaskDelete(NULL);
}

/* 由 bt_push_service 在 GMF 事件线程回调（★ 不可阻塞！只看标志、起任务） */
static void on_push_finished(void)
{
    ESP_LOGI(TAG, "push finished -> schedule cleanup");
    BaseType_t ok = xTaskCreateWithCaps(push_finish_cleanup_task, "bt_push_stop",
                                        2048, NULL, 5, NULL,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        /* 兜底：起不了任务就只释放 session，至少不把通路锁死 */
        ESP_LOGW(TAG, "cleanup task create failed; release session only");
        session_manager_end(SESSION_BT_PUSH);
    }
}

esp_err_t bt_audio_service_play_source(const char *path)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!path) return ESP_ERR_INVALID_ARG;

    /* 推流占用音频通路（codec/PSRAM 带宽），与通话/录音互斥 */
    if (session_manager_begin(SESSION_BT_PUSH) != ESP_OK) {
        ESP_LOGW(TAG, "play denied: another session active (%d)", session_manager_get_mode());
        return ESP_ERR_INVALID_STATE;
    }

    /* 1) uri 交给 push 服务暂存（等 ALLOCATED 事件拿到 stream 才真正下发） */
    esp_err_t err = bt_push_service_set_uri(path);
    if (err != ESP_OK) {
        session_manager_end(SESSION_BT_PUSH);
        ESP_LOGE(TAG, "set_uri failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2) 触发 A2DP 流；后续由 STREAM_STATE_CHG 驱动 pipeline（set_uri 内部已调用） */
    return ESP_OK;
}

esp_err_t bt_audio_service_pause_source(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    /* 暂停只停 pipeline（保留 A2DP 链路），以便 resume 继续推同一首 */
    return bt_push_service_pause();
}

esp_err_t bt_audio_service_resume_source(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    return bt_push_service_resume();
}

esp_err_t bt_audio_service_stop_source(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    esp_err_t err = bt_push_service_stop();
    session_manager_end(SESSION_BT_PUSH);
    return err;
}

bool bt_audio_service_is_connected(void) { return s_connected; }

esp_err_t bt_audio_service_set_volume(int volume)
{
    if (!s_on || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ① 绝对音量：一步到位，两端档位严格一致（A2DP 1.3+ 耳机普遍支持） */
    if (s_use_abs_vol) {
        esp_err_t ret = esp_bt_audio_vol_set_absolute((uint32_t)local_vol_to_app(volume));
        if (ret == ESP_OK) {
            s_last_sent_vol = volume;
            s_abs_fail_count = 0;
        } else {
            ESP_LOGW(TAG, "set abs vol failed: %s (%d consecutive)",
                     esp_err_to_name(ret), s_abs_fail_count + 1);
            if (ret == ESP_ERR_INVALID_STATE) {
                /* ① 补建一次（若还没放弃重试） */
                avrcp_ensure_ct_link();
                /* ② 探测：万一其实已经好了（补建刚生效），立刻重试一次 */
                if (avrcp_probe_ready()) {
                    ret = esp_bt_audio_vol_set_absolute((uint32_t)local_vol_to_app(volume));
                    if (ret == ESP_OK) {
                        s_last_sent_vol = volume;
                        s_abs_fail_count = 0;
                        ESP_LOGI(TAG, "set abs vol OK after re-probe");
                        return ret;
                    }
                }
                /* ③ 连续失败到阈值 → 自动降级相对模式，保证"按键能调音量" */
                if (++s_abs_fail_count >= ABS_FAIL_DOWNGRADE) {
                    ESP_LOGW(TAG, "abs vol failed %d times, downgrade to RELATIVE mode",
                             s_abs_fail_count);
                    s_use_abs_vol = false;
                    s_last_sent_vol = volume;
                }
            }
        }
        return ret;
    }

    /* ② 相对音量：按与上次目标档位的差值，逐格发 ±N
     *    （个别老耳机不听 Absolute Volume 时可切到这个模式） */
    int base = (s_last_sent_vol >= 0) ? s_last_sent_vol : (BT_VOL_LOCAL_MAX / 2);
    int diff_ticks = local_vol_to_app(volume) - local_vol_to_app(base);
    if (diff_ticks == 0) {
        return ESP_OK;   /* 档位未变，无需下发 */
    }

    bool up = (diff_ticks > 0);
    int steps = (diff_ticks > 0) ? diff_ticks : -diff_ticks;
    /* 上限保护：拖滑块时避免一次刷爆 AVRCP（正常一次按键最多 20 格） */
    if (steps > 32) {
        steps = 32;
    }
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < steps; i++) {
        ret = esp_bt_audio_vol_set_relative(up);
        if (ret != ESP_OK) {
            break;
        }
    }
    if (ret == ESP_OK) {
        s_last_sent_vol = volume;
    }
    ESP_LOGD(TAG, "vol rel %s x%d -> local %d, ret=%s",
             up ? "up" : "down", steps, volume, esp_err_to_name(ret));
    return ret;
}

void bt_audio_service_set_abs_volume(bool use_absolute)
{
    s_use_abs_vol = use_absolute;
    s_last_sent_vol = -1;   /* 切模式后基准失效，重新基线 */
    ESP_LOGI(TAG, "volume mode -> %s", use_absolute ? "absolute" : "relative");
}
