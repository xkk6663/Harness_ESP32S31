/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"

#include "ui/ui_manager/page_manager.h"
#include "ui/pages/page_splash.h"
#include "ui/pages/page_main.h"
#include "ui/overlays/overlay_manager.h"
#include "middleware/audio/audio_service.h"
#include "middleware/system/fs_service.h"
#include "middleware/system/sdcard_service.h"
#include "middleware/system/led_service.h"
#include "middleware/system/button_service.h"
#include "middleware/network/wifi_manager.h"
#include "middleware/audio/session_manager.h"
#include "middleware/bluetooth/link_manager.h"
#include "middleware/audio/udp_voice.h"
#include "middleware/audio/record_service.h"
#include "middleware/network/net_time_service.h"
#include "middleware/bluetooth/bt_audio_service.h"
#include "middleware/bluetooth/bt_push_service.h"

static const char *TAG = "main";

/* ★ F35 一次性诊断：把"BT_PUSH ready → WIFI enter init"之间那 99.1 KB 的无埋点
   盲区逐项拆开。用法：在每步前后各打一行，diff 就是这一步的净消耗。
   判定完成后连同调用点一起删除。
   ★ 同时打印 largest —— 这是判断"还能不能拿到连续块"的关键（F35 已两次验证）。 */
#define BOOT_DIAG(stage)                                                        \
    ESP_LOGI(TAG, "[boot-diag] %-34s internal free=%u largest=%u | DMA free=%u | PSRAM free=%u", \
             (stage),                                                           \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),            \
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),   \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),                 \
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM))

/* 本次上电周期内，开机提示音是否已播过（只在第一次上滑进主界面时播一次）。
   ★ 暂时未使用：相关逻辑在 on_splash_enter() 里被整段注释掉（等 boot_tone.wav 就位）。
     用 __attribute__((unused)) 压掉 -Wunused-variable，保留变量以免将来取消注释时漏改。 */
static bool s_boot_tone_played __attribute__((unused)) = false;

/* Swipe-up on splash recognized -> push the main page. */
static void on_splash_enter(void)
{
    PageManager_Load(&Page_Main);

    // /* 第一次上滑进主界面：自动播放一次开机提示音 */
    // if (!s_boot_tone_played) {
    //     s_boot_tone_played = true;
    //     audio_cmd_t cmd = {
    //         .id = AUDIO_CMD_PLAY_FILE,
    //         .value = 0,
    //     };
    //     snprintf(cmd.path, sizeof(cmd.path), "%s/boot_tone.wav", FS_MNT_PATH);
    //     audio_service_post_cmd(&cmd);
    //     ESP_LOGI(TAG, "boot tone requested: %s", cmd.path);
    // }
}

/* 统一音量出口：硬件 vol+/vol- 键与 UI 音量条都走这里。
   蓝牙已连接 → 音量归蓝牙管（AVRCP 推给耳机）；否则 → 本地 ES8389 codec。
   这样两个入口行为一致，不会出现"按键能调、滑块不能调"。 */
static void on_volume_change(int volume)
{
    if (bt_audio_service_is_connected()) {
        esp_err_t ret = bt_audio_service_set_volume(volume);
        if (ret == ESP_OK) {
            return;
        }
        /* 蓝牙音量下发失败（未连/不支持）：退回本地 codec，保证不出哑音 */
        ESP_LOGW(TAG, "bt vol set failed (%s), fallback to local codec", esp_err_to_name(ret));
    }
    audio_cmd_t cmd = {
        .id = AUDIO_CMD_SET_VOLUME,
        .value = volume,
    };
    audio_service_post_cmd(&cmd);
}

/* Wi-Fi status -> main-interface RGB LED (red / blue breathe / green) */
static void on_wifi_evt(wifi_evt_t evt, const char *info)
{
    switch (evt) {
    case WIFI_EVT_DISCONNECTED:
    case WIFI_EVT_ALL_FAILED:
    case WIFI_EVT_STA_LEFT:
        led_service_set_status(LED_STATUS_DISCONNECTED);
        break;
    case WIFI_EVT_CONNECTING:
    case WIFI_EVT_AP_UP:            /* AP 热点已开，等待客户端接入 */
        led_service_set_status(LED_STATUS_CONNECTING);
        break;
    case WIFI_EVT_GOT_IP:
    case WIFI_EVT_STA_JOINED:       /* AP 有客户端连上 / STA 拿到 IP */
        led_service_set_status(LED_STATUS_CONNECTED);
        break;
    default:
        break;
    }

    /* STA 真正拿到 IP 才开始校时/取天气；AP 热点自身不算联网。
       用 notify_network_up() 而不是 start()：它能兜住"IP 事件比服务初始化更早"的情况
       （开机自动连已保存路由时经常发生），不会出现"漏一次事件就永久不拉天气"。 */
    if (evt == WIFI_EVT_GOT_IP) {
        net_time_service_notify_network_up();
    }
}

void app_main(void)
{
    BOOT_DIAG("app_main enter");

    ESP_ERROR_CHECK(fs_service_mount());
    BOOT_DIAG("after fs_service_mount");
    sdcard_service_mount();
    BOOT_DIAG("after sdcard_service_mount");
    bsp_i2c_init();
    BOOT_DIAG("after bsp_i2c_init");

    /* ★ 时序纪律：所有"大块 + 只分一次 + 活到关机"的缓冲，必须在
       bsp_display_start() 之前拿完。此时内部 RAM 最空、PSRAM 也还没被切碎。
       漏一个就会像 F28/F30 那样：晚分配吃掉内部 RAM，后续开蓝牙直接 assert。
       ---- 本段按此顺序：BT controller → GMF pool → JPEG 帧缓冲 ---- */
    bt_audio_service_controller_init();
    BOOT_DIAG("after bt ctrl init");
    bt_push_service_early_init();
    BOOT_DIAG("after bt_push early init");
    fs_service_init();          /* JPEG 输出缓冲 1.10MB（800*480*3，LV_DEPTH=24），显式 PSRAM（F30） */
    BOOT_DIAG("after fs_service_init");

    bsp_display_start();
    BOOT_DIAG("after bsp_display_start");
    //bsp_display_brightness_set(50);
    audio_service_init();
    BOOT_DIAG("after audio_service_init");

    led_service_init();
    BOOT_DIAG("after led_service_init");
    button_service_init();
    BOOT_DIAG("after button_service_init");
    /* 音量统一出口：硬件键 + UI 滑块 → 蓝牙（已连）/ 本地 codec */
    button_service_set_vol_out_cb(on_volume_change);
    BOOT_DIAG("after vol_out_cb set");

    /* ★ 顺序有讲究：先注册 wifi 事件回调，再 wifi_manager_init()（后者内部就 esp_wifi_start()
       + auto_connect）。以前是反的 —— 开机自动连已保存路由时，GOT_IP 会在
       wifi_manager_init() 返回前就派发完，而那时 sys_cb 还是 NULL，
       通知被丢掉 → 任务永不启动、天气永远不刷新。 */
    wifi_manager_set_sys_cb(on_wifi_evt);
    BOOT_DIAG("before wifi_manager_init");
    wifi_manager_init();
    BOOT_DIAG("after wifi_manager_init");
    session_manager_init();
    BOOT_DIAG("after session_manager_init");
    link_manager_init();
    BOOT_DIAG("after link_manager_init");
    udp_voice_init();
    BOOT_DIAG("after udp_voice_init");
    record_service_init();
    BOOT_DIAG("after record_service_init");
    /* 默认上电 STA 模式（自动连已保存路由）；UI 可切 AP 开热点 */

    /* UI framework */
    PageManager_Init();
    BOOT_DIAG("after PageManager_Init");
    page_splash_set_enter_cb(on_splash_enter);
    PageManager_Load(&Page_Splash);     /* stack root */
    BOOT_DIAG("after PageManager_Load(splash)");

    ESP_LOGI(TAG, "boot done, waiting swipe up");
}
