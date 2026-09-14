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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

#define MAX_AP 16
#define WIFI_MAX_RETRY 5

static wifi_evt_cb_t s_evt_cb;
static wifi_evt_cb_t s_sys_cb;
static bool s_connected;
static int  s_retry_num;
static char s_ip[16];

static wifi_ap_info_t s_ap_list[MAX_AP];
static int s_ap_count;

static void notify(wifi_evt_t evt, const char *info)
{
    ESP_LOGI(TAG, "evt=%d ip=%s", evt, info ? info : "-");
    if (s_sys_cb) {
        s_sys_cb(evt, info);
    }
    if (s_evt_cb) {
        s_evt_cb(evt, info);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started (waiting UI trigger)");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "disconnected reason=%d", d->reason);
        notify(WIFI_EVT_DISCONNECTED, NULL);
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
        notify(WIFI_EVT_SCAN_DONE, NULL);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip, sizeof(s_ip));
        s_connected = true;
        ESP_LOGI(TAG, "got ip: %s", s_ip);
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

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi STA init done (no auto-connect)");
}

void wifi_manager_set_evt_cb(wifi_evt_cb_t cb)
{
    s_evt_cb = cb;
}

void wifi_manager_set_sys_cb(wifi_evt_cb_t cb)
{
    s_sys_cb = cb;
}

void wifi_manager_start_scan(void)
{
    ESP_LOGI(TAG, "scan start");
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
    s_retry_num = 0;
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    notify(WIFI_EVT_CONNECTING, NULL);
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

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
