/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
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
#include "middleware/network/pcnow_service.h"
#include "middleware/bluetooth/bt_audio_service.h"
#include "middleware/bluetooth/bt_push_service.h"
#include "middleware/bluetooth/bt_sink_service.h"
#include "ui/common/ui_binfont.h"

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

/* §28.31 临时诊断：taskLVGL 栈水位。回调在 taskLVGL 上下文执行，NULL = 当前任务。
 * ★ 本移植（riscv，portmacro.h:95）StackType_t = uint8_t —— 水位返回值就是**字节数**，
 *   实测日志 "9128 words (9128 bytes)" 两数相等即证。 */
static void stack_hwm_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "[stack-hwm] taskLVGL high-water = %u words (%u bytes)",
             (unsigned)words, (unsigned)words * (unsigned)sizeof(StackType_t));
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

    /* ★★ F40（2026-09-24，修"上电必崩"）：字库的**读**必须排在这里 ——
       所有 PSRAM 栈任务的"上游"。
       原因：读 2.7MB 字库 = esp_flash_read() 按 16KB 分块（MAX_READ_CHUNK）
       ≈168 次，每块一对 spiflash_start_read/end ＝ 关一次全局 cache；
       而 IDF 的 spi_flash_disable_interrupts_caches_and_other_cpu()
       （cache_utils.c:283）**只关 cache，并不停住另一核** ——
       这些窗口内任何核上的任何 PSRAM 访问（含别的任务压/弹栈）都直接
       Cache error（实机 MEPC 落在 spi_flash_hal_read 的收尾弹栈 lw ra,44(sp)）。
       此刻系统里只有内部栈任务（IDLE/esp_timer/ipc/sdmmc），taskLVGL 也还没建，
       读到 PSRAM 的全程没有任何 PSRAM 访问者 ⇒ 天然零风险。
       ⚠ 绝不能再挪回 PageManager_Load(splash) 之后 —— 那时 bt_push(local2bt_task)、
       bt_sink(bt2codec_task)、audio_task、led_task、link_manager、udp_voice×3、
       pcnow_rx 的 PSRAM 栈任务已全部在跑，必崩。
       代价：黑屏时间多出字库读取的约 2 秒（远优于上电必崩）。
       解析仍须在 lv_init() 之后 → 见下面的 ui_binfont_attach()。 */
    ui_binfont_load();
    BOOT_DIAG("after ui_binfont_load");

    /* ★ 时序纪律：所有"大块 + 只分一次 + 活到关机"的缓冲，必须在
       bsp_display_start() 之前拿完。此时内部 RAM 最空、PSRAM 也还没被切碎。
       漏一个就会像 F28/F30 那样：晚分配吃掉内部 RAM，后续开蓝牙直接 assert。
       ---- 本段按此顺序：BT controller → GMF pool(Source/Sink 各一套) → JPEG 帧缓冲 ---- */
    bt_audio_service_controller_init();
    BOOT_DIAG("after bt ctrl init");
    bt_push_service_early_init();
    BOOT_DIAG("after bt_push early init");
    /* ★ Sink 接收管线（io_bt→aud_dec→aud_asrc→io_codec_dev）+ 封面解码任务，
       与 Source 一样走"方案 B 延迟常驻"：在这里建好，之后只 run/stop 不重建。
       失败只打日志（蓝牙非必需功能），不影响开机。 */
    bt_sink_service_early_init();
    BOOT_DIAG("after bt_sink early init");
    fs_service_init();          /* JPEG 输出缓冲 1.10MB（800*480*3，LV_DEPTH=24），显式 PSRAM（F30） */
    BOOT_DIAG("after fs_service_init");

    /* ★ §28.31：taskLVGL 任务栈 7168 → 16384。
     * 两次 "Stack protection fault"（进 SINK / back to Main 整页重绘瞬间）都发生在
     * taskLVGL 上下文。panic dump 里的 Stack bounds/SP 永远是 ISR 栈区间+栈顶
     * （portasm.S:669/:699-709 进中断就重写），判不出真凶 —— 上次按"中断栈溢出"
     * 把 ISR_STACKSIZE 提到 4096 无效即是佐证。守卫是同步触发的，被打断的当前任务
     * （taskLVGL）就是肇事者：esp_lvgl_port 默认 task_stack=7168（内部 RAM），
     * 整页构建 + bin 字体动态字形加载 + 歌词/EQ 动画的调用链把它压穿了。
     * bsp_display_start() 内部就是组装下面这份默认 cfg（见 bsp_display.c:274-291），
     * 这里照抄后只覆盖 task_stack，其余行为不变。 */
    bsp_display_cfg_t dsp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = 1,
#else
        .double_buffer = 0,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        }
    };
    /* §28.31：默认 7168，整页重绘深度不够。不能写进上面的指定初始化器 ——
     * 整体初始化 .lvgl_port_cfg 后再覆盖子成员会触发 -Woverride-init。 */
    dsp_cfg.lvgl_port_cfg.task_stack = 16384;
    bsp_display_start_with_config(&dsp_cfg);
    BOOT_DIAG("after bsp_display_start");

    /* ★ F40：字库阶段 2 —— 把已读进 PSRAM 的 bin 解析成 LVGL 字体。
       纯 RAM 操作（lv_binfont_create_ex 展开 glyph 描述符 + loca），
       没有任何 flash 读 ⇒ 不会产生 cache 关窗，放这里安全；
       唯一约束是必须在 lv_init()（bsp_display_start 内部）之后。 */
    ui_binfont_attach();
    BOOT_DIAG("after ui_binfont_attach");

    /* ---- §28.31 临时诊断：taskLVGL 栈水位（配合 16384 验证用）----
     * 定时器回调跑在 taskLVGL 上下文，uxTaskGetStackHighWaterMark(NULL) 即 taskLVGL
     * 自己的水位，单位是 StackType_t 字数。判读：16384 下若重绘高峰后仍剩 ≥5KB，
     * 实锤"7168 不够"结案；若仍贴 0，说明另有深调用，继续查。
     * 定位完成后整段（回调 + 创建）可删。 */
    bsp_display_lock(0);
    //lv_timer_create(stack_hwm_timer_cb, 5000, NULL);
    bsp_display_unlock();
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
    /* PC 音乐副屏数据服务：挂 link_manager 第 4 槽位 + 绑 UDP 50004。
       必须在 link_manager_init() 之后（observer 槽位/对端学习依赖它）。 */
    pcnow_service_init();
    BOOT_DIAG("after pcnow_service_init");
    /* 默认上电 STA 模式（自动连已保存路由）；UI 可切 AP 开热点 */

    /* UI framework */
    PageManager_Init();
    BOOT_DIAG("after PageManager_Init");
    page_splash_set_enter_cb(on_splash_enter);
    PageManager_Load(&Page_Splash);     /* stack root */
    BOOT_DIAG("after PageManager_Load(splash)");

    /* ★ 全 CJK bin 字体（LittleFS → PSRAM → LVGL 运行时字体）已在前面两处完成：
         · 读取  → ui_binfont_load()   （紧跟 bsp_i2c_init，F40：早于所有 PSRAM 栈任务）
         · 解析  → ui_binfont_attach() （紧跟 bsp_display_start，需要 lv_init）
       这里不再调用 —— 旧代码把整段放在此处，正是"上电必崩"的根因（见 F40）。
       SINK / PC 页要显示外部推来的曲名，字符集不可预测，没有它中文会变方块；
       主页在上滑（on_splash_enter）时才构建，此时字体早已就绪。 */

    ESP_LOGI(TAG, "boot done, waiting swipe up");
}
