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
#include "bt_sink_service.h"   /* ★ A2DP Sink 接收：bt2codec 管线 + 元数据/封面 */

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

/* ---- 蓝牙音量（★ 2026-09-22 起走推流数字增益，AVRCP 音量已整体移除） ----
 * 历史上这里维护过一套"本地 0..90 → esp_bt_audio 应用层 0..100 → AVRCP 0..127"
 * 的三层量纲换算，以及绝对/相对音量的自动降级逻辑。该方案最终被证实
 * **在本工程不可用**（AVRCP RC 通道建不起来，见下方长注释），已整体删除。
 *
 * 现在音量只有一条路：bt_push_service_set_volume() —— 在 local2bt pipeline
 * 的 aud_alc 元素上做"编码前 PCM"的数字衰减。本地档位 0..100 直接映射到 dB
 * 增益，不再有任何协议层量纲换算。
 *
 * 保留的认知（仍有用）：esp_bt_audio 的 vol_ops 只有在 AVRCP CT **连接建立**
 * 后才注册；未注册时调用会打 "vol_ops.set_absolute is not registered" 并返回
 * ESP_ERR_INVALID_STATE。而**耳机支不支持 Absolute Volume，协议层返回都是
 * ESP_OK**，无法从返回值判断成功与否 —— 这也正是当初"降级兜底"必然失败的根因。
 */

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

/* ---------------------------------------------------------------------------
 * 对外广播的蓝牙设备名（手机/电脑搜索时看到的名字）
 * ---------------------------------------------------------------------------
 * ★ 2026-09-22 起改为**固定名**，不再拼 MAC 后缀。
 *   原实现是 "Korvo_%02X%02X"(mac[4], mac[5])，目的是让多块板子能区分开；
 *   现按需求改为固定 "Korvo_kk"。
 *   ⚠ 若以后同时用多块板子联调，固定名会导致列表里出现多个同名设备、
 *     无法分辨是哪一块 —— 那时把这里改回带 MAC 后缀即可（见下方注释里的原式）。
 *
 * 长度上限 ESP_BT_AUDIO_HOST_MAX_DEV_NAME_LEN = 32，改名注意别超。
 */
#define BT_DEV_NAME                   "Korvo_kk"

/* ---------------------------------------------------------------------------
 * 推流数字音量（★ 2026-09-22 起，AVRCP 音量整条链路已移除）
 * ---------------------------------------------------------------------------
 * 为什么放弃 AVRCP：
 *   1) 本工程是 A2DP SRC，上游 IDF 的 SRC 路径**不建立 AVRCP RC 通道** ——
 *      BTA_AvOpenRc() 全 IDF 只有一个调用点，位于 peer_sep == AVDT_TSEP_SRC
 *      分支内；本工程对端是 sink，永远走不到那一支。
 *   2) 应用层手工补调 BTA_AvOpenRc() 后，实测耳机侧仍不建立 AVCTP 通道，
 *      ESP_AVRC_CT_CONNECTION_STATE_EVT 始终不来 → vol_ops 整张表从未注册。
 *   3) 于是 set_absolute 与 set_relative **必然同时**返回 ESP_ERR_INVALID_STATE
 *      （两者在同一张表里一起赋值），"绝对失败就降级相对"的兜底思路天然无效。
 *   4) main.c 的 "fallback to local codec" 同样无用：推流路径
 *      （SD→解码→ASRC→SBC→蓝牙）根本不经过本地 ES8389。
 *
 * 现方案：音量交给 local2bt pipeline 里的 aud_alc 做数字衰减
 *   （见 bt_push_service.c / bt_push_service_set_volume()）——
 *   我们是源，这段 PCM 完全可控，必然生效且与耳机能力无关。
 *
 * 本文件只负责"要不要接管"：仅在真正推流时接管音量，
 *   否则返回 ESP_ERR_INVALID_STATE，让上层继续用本地 codec
 *   （扬声器播放 / 开机提示音的正确出口）。
 * ------------------------------------------------------------------------- */

/* ★ 2026-09-22 起：角色位按 s_role 动态返回（此前写死 Source，导致角色按钮形同虚设）。
 *
 * 为什么不能"同时注册 SRC+SNK"（源码级依据，见
 * docs/项目规划/btsink蓝牙接收移植方案.md §3.1）：
 *   - A2DP 是**全局单例**：esp_a2d_sink_init() 与 esp_a2d_source_init()
 *     共用同一个 g_a2dp_on_init，且 btc_av.c 只用 if/else if 起一个 AV 状态机
 *     → 同一时刻只能是 SRC 或 SNK，同时注册会返回 ESP_ERR_INVALID_STATE。
 *   - 角色位在 esp_bt_audio_init(&cfg) 里**一次性传入**，init 之后改不了。
 *     所以切换角色只能"整个 host 推倒重建"：
 *         set_power(false) → 改 s_role → set_power(true)
 *     这正是 bt_audio_service_switch_role() 做的事，且**只能由页内按钮触发**。
 *   - AVRCP 的 CT/TG 是各自独立注册的，可与任一 A2DP 角色共存。
 *
 * Sink 下同时开 CT + TG（参考仓库用 SNK|CT|TG）：
 *   手机既能把本机当"被控播放器"（TG 收命令），本机也能主动发播放控制（CT）。
 */
static uint32_t classic_roles(void)
{
    if (s_role == BT_ROLE_SINK) {
        return ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK |
               ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT |
               ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_TG;
    }
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
        if (s_evt_cb) s_evt_cb(c->connected ? BT_EVT_CONNECTED : BT_EVT_DISCONNECTED, c->addr, s_evt_ctx);
        /* ★ 2026-09-22：连接/断开都不再需要"音量同步任务" ——
           数字增益保存在常驻的 ALC 元素里，推流开始时自动生效
           （见 STREAM_STATE_CHG 分支与 bt_push_service_set_volume）。 */
        break;
    }
    case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
        esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;

        /* ★ 2026-09-22：必须按**流方向**分流，不能一律喂给 Source 的推流服务。
           Sink 模式下 bt_push_service 会去 set_in_uri / 起 local2bt 管线，
           方向完全反了（本该 io_bt→解码→codec）。 */
        esp_bt_audio_stream_dir_t dir = ESP_BT_AUDIO_STREAM_DIR_UNKNOWN;
        esp_bt_audio_stream_get_dir(st->stream_handle, &dir);
        ESP_LOGI(TAG, "stream %p state=%d dir=%d", st->stream_handle, st->state, dir);

        if (dir == ESP_BT_AUDIO_STREAM_DIR_SINK) {
            /* 手机/电脑推音频过来 → bt2codec 接收管线（io_bt→aud_dec→aud_asrc→io_codec_dev） */
            bt_sink_service_on_stream_state(st->stream_handle, st->state);
        } else {
            /* 本机推 SD 音乐到耳机 → local2bt 管线（io_file→…→io_bt） */
            bt_push_service_on_stream_state(st->stream_handle, st->state);

            /* ★ 推流开始时把当前档位同步成数字增益：用户在未推流期间调音量
               走的是本地 codec 路径（见 bt_audio_service_set_volume），
               ALC 元素里存的可能是旧档位，这里补一次。 */
            if (st->state == ESP_BT_AUDIO_STREAM_STATE_ALLOCATED ||
                st->state == ESP_BT_AUDIO_STREAM_STATE_STARTED) {
                bt_push_service_set_volume(button_service_get_volume());
            }
        }

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
    /* ★ 2026-09-22 新增（btsink）：播放状态 + 元数据，全部丢给 Sink 服务。
     * 事件跑在 BT 线程，Sink 服务内部只缓存/投队列，**绝不碰 LVGL**。 */
    case ESP_BT_AUDIO_EVENT_PLAYBACK_STATUS_CHG: {
        esp_bt_audio_event_playback_st_t *ps = (esp_bt_audio_event_playback_st_t *)event_data;
        ESP_LOGI(TAG, "playback status evt=0x%02x param=%u",
                 (unsigned)ps->event, (unsigned)ps->evt_param.play_status);
        bt_sink_service_on_playback_status(ps);
        break;
    }
    case ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA: {
        esp_bt_audio_event_playback_metadata_t *md = (esp_bt_audio_event_playback_metadata_t *)event_data;
        ESP_LOGI(TAG, "playback metadata type=0x%02x len=%u",
                 (unsigned)md->type, (unsigned)md->length);
        bt_sink_service_on_metadata(md);
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

/* 前向声明：bt_audio_service_init() 在 on_push_finished 定义之前就要注册它。
 * ★ 2026-09-22 补：此前删 AVRCP 音量代码块时，把这条声明连同被删区间一起误删了，
 *   导致 bt_audio_service_init 里报 'on_push_finished' undeclared。
 *   注意此处是**作为函数指针传递**（后面跟 ')' 不是 '('），所以编译期只在
 *   "未声明标识符"这一层报错，而不是"未定义函数"。 */
static void on_push_finished(void);

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
        /* 固定设备名（见文件上方 BT_DEV_NAME 的说明）。
           原式为按 BT MAC 后两字节生成，便于多板区分：
             uint8_t mac[6] = {0};
             esp_read_mac(mac, ESP_MAC_BT);
             snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "Korvo_%02X%02X", mac[4], mac[5]); */
        snprintf(host_cfg.dev_name, sizeof(host_cfg.dev_name), "%s", BT_DEV_NAME);
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

        /* ★ Sink 模式必须"可被发现 + 可被连接"，手机才搜得到本机并主动连上。
           Source 模式反过来：由本机去扫别人，不需要对外可见，保持默认不可见更省电。 */
        if (s_role == BT_ROLE_SINK) {
            esp_err_t sr = esp_bt_audio_classic_set_scan_mode(true, true);
            ESP_LOGI(TAG, "sink scan mode (connectable+discoverable) ret=%s", esp_err_to_name(sr));
        }

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

/* ★ 仅"记录"角色，不重建 host —— 真正的角色切换请用 bt_audio_service_switch_role()。
   保留本函数是因为 classic_roles() 在 init 时会读它，且它可安全地在"蓝牙未开"时使用
   （例如开机时预设下次要以哪个角色起来）。 */
void bt_audio_service_set_role(bt_role_t role)
{
    s_role = role;
    ESP_LOGI(TAG, "role recorded -> %s (host NOT rebuilt; call switch_role to apply)",
             role == BT_ROLE_SOURCE ? "SOURCE" : "SINK");
}

bt_role_t bt_audio_service_get_role(void) { return s_role; }

/* ★ 2026-09-22 新增：真正切换 A2DP 角色（重建 host）。
 *
 * ★★ 调用契约（重要，勿改）：
 *   本文件的**唯一合法调用点**是 Bluetooth 子页面（page_bluetooth.c）的角色按钮。
 *   **绝对不要**在 tab 切换（page_main.c 的 tab_changed_event）里调用 ——
 *   角色切换必然断开当前蓝牙连接（A2DP 全局单例，只能重建 host），
 *   必须是用户"明确按按钮"的结果，而不是划 tab 的副作用。
 *
 * 实现：set_power(false) → 改 s_role → set_power(true)
 *   - 未开蓝牙时只记录角色，返回 ESP_OK（下次开就按新角色起）；
 *   - set_power(true) 内部有内部 RAM 体检闸门，不足会返回 ESP_ERR_NO_MEM。
 *     ★ 此时必须回滚角色并把蓝牙拉回可用状态，否则用户会得到"两边都用不了"。
 */
esp_err_t bt_audio_service_switch_role(bt_role_t role)
{
    if (role != BT_ROLE_SINK && role != BT_ROLE_SOURCE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_role == role) {
        ESP_LOGI(TAG, "switch_role: already %s, no-op",
                 role == BT_ROLE_SINK ? "SINK" : "SOURCE");
        return ESP_OK;
    }

    bt_role_t prev = s_role;
    bool was_on = s_on;
    ESP_LOGI(TAG, "switch_role %s -> %s (host rebuild, was_on=%d)",
             prev == BT_ROLE_SOURCE ? "SOURCE" : "SINK",
             role == BT_ROLE_SINK ? "SINK" : "SOURCE", (int)was_on);

    if (was_on) {
        bt_audio_service_set_power(false);   /* deinit host：连接会断，这是必然代价 */
    }
    s_role = role;

    if (!was_on) {
        return ESP_OK;   /* 没开就不需要重建，角色已记下 */
    }

    esp_err_t ret = bt_audio_service_set_power(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "switch_role: re-init failed (%s) -> rollback to %s",
                 esp_err_to_name(ret), prev == BT_ROLE_SOURCE ? "SOURCE" : "SINK");
        s_role = prev;
        esp_err_t rb = bt_audio_service_set_power(true);
        ESP_LOGE(TAG, "switch_role: rollback re-init ret=%s", esp_err_to_name(rb));
        return ret;
    }
    ESP_LOGI(TAG, "switch_role done: now %s", role == BT_ROLE_SINK ? "SINK" : "SOURCE");
    return ESP_OK;
}

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
    /* connect 的 role 参数要传单个 profile（SRC=0x1 / SNK=0x2），不是 bitmask */
    uint32_t role = (s_role == BT_ROLE_SINK) ? ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK
                                             : ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC;
    ESP_LOGI(TAG, "connect role=%s addr=%02x:%02x:%02x:%02x:%02x:%02x",
             s_role == BT_ROLE_SINK ? "SNK" : "SRC",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    esp_err_t ret = esp_bt_audio_classic_connect(role, (uint8_t *)addr);
    ESP_LOGI(TAG, "classic_connect ret=%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t bt_audio_service_disconnect(void)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    /* 经典蓝牙一次只连一台。★ 按当前角色决定收尾动作：
       - Source：先停 A2DP 流（media_stop 只支持 SRC），再断开；
       - Sink：流由对端发起，本机不调 media_stop（会返回 INVALID_STATE）。 */
    if (s_role == BT_ROLE_SOURCE) {
        esp_bt_audio_media_stop(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC);
    }
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

    /* 通知 UI 回到"已停止"态（page_bluetooth / tab_audio 复用 BT_EVT_STREAM_OFF） */
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

/* button_service 的档位上限（其 VOL_MAX 是 .c 里的私有宏，这里显式对齐）。
 * ★ 必须折算：数字增益按 0..100 满幅映射，若直接传 0..90，
 *   90 档算出来只有约 -0.9 dB，用户会感觉"音量没推满"。
 * ⚠ 若 button_service 改了 VOL_MAX，这里要同步。 */
#define BT_VOL_LOCAL_MAX   90

esp_err_t bt_audio_service_set_volume(int volume)
{
    if (!s_on) {
        return ESP_ERR_INVALID_STATE;
    }
    /* ★ 2026-09-22：音量改为「推流数字增益」，不再走 AVRCP（原因见文件上方注释）。
     * 只在真正推流时接管：否则返回 ESP_ERR_INVALID_STATE，
     * 让上层（main.c on_volume_change）继续走本地 codec ——
     * 那是扬声器播放 / 开机提示音的正确出口。 */
    if (!s_connected || !bt_push_service_is_playing()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 本地 0..90 → 数字增益量纲 0..100（四舍五入） */
    int v100 = (volume * 100 + BT_VOL_LOCAL_MAX / 2) / BT_VOL_LOCAL_MAX;
    return bt_push_service_set_volume(v100);
}
