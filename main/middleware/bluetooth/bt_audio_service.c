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

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_heap_caps.h"

#include "esp_bt_audio.h"
#include "esp_bt_audio_defs.h"
#include "esp_bt_audio_event.h"
#include "esp_bt_audio_host.h"
#include "esp_bt_audio_classic.h"
#include "esp_bt_audio_media.h"

#include "bt_push_service.h"

#include "session_manager.h"

static const char *TAG = "BT_SVC";

static bt_audio_evt_cb_t s_evt_cb = NULL;
static void *s_evt_ctx = NULL;
static bool s_on = false;
static bool s_controller_ready = false;
static bt_role_t s_role = BT_ROLE_SOURCE;
static bt_device_t s_found_dev;
static uint8_t s_last_addr[6] = {0};
static bool s_last_addr_valid = false;

/* 当前阶段只做 Source（推送 SD 音乐到耳机）；Sink 模式后续再开。
   同时注册 SRC+SNK 会导致 esp_a2d_sink_init 返回 INVALID_STATE（profile 状态机冲突）。 */
static uint32_t classic_roles(void)
{
    return ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC |
           ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT;
}

static uint32_t active_role_bit(void)
{
    if (s_role == BT_ROLE_SOURCE) {
        return ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC | ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT;
    }
    return ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK | ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_TG;
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
    case ESP_BT_AUDIO_EVENT_CONNECTION_STATE_CHG: {
        esp_bt_audio_event_connection_st_t *c = (esp_bt_audio_event_connection_st_t *)event_data;
        ESP_LOGI(TAG, "connection %s addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 c->connected ? "CONNECTED" : "DISCONNECTED",
                 c->addr[0], c->addr[1], c->addr[2], c->addr[3], c->addr[4], c->addr[5]);
        if (s_evt_cb) s_evt_cb(c->connected ? BT_EVT_CONNECTED : BT_EVT_DISCONNECTED, c->addr, s_evt_ctx);
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
        esp_bt_audio_event_vol_absolute_t *v = (esp_bt_audio_event_vol_absolute_t *)event_data;
        ESP_LOGI(TAG, "vol abs=%d mute=%d", v->vol, v->mute);
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

esp_err_t bt_audio_service_set_power(bool on)
{
    if (on && s_on) return ESP_OK;
    if (!on && !s_on) return ESP_OK;

    if (on) {
        if (!s_controller_ready) {
            ESP_LOGE(TAG, "controller not ready (early init failed)");
            return ESP_ERR_INVALID_STATE;
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
