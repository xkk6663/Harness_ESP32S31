/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>                                  /* ★ S8-1 自检清场用 unlink/rmdir */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "ui/ui_manager/page_manager.h"
#include "ui/page_splash.h"
#include "ui/page_main.h"
#include "ui/overlay_manager.h"
#include "ui/ui_binfont.h"
#include "middleware/audio/audio_service.h"
#include "middleware/audio/rec_local_service.h"
#include "middleware/audio/media_player_service.h"
#include "middleware/ai/xiaozhi_service.h"
#include "middleware/periph/fs_service.h"
#include "middleware/periph/sdcard_service.h"
#include "middleware/periph/led_service.h"
#include "middleware/periph/button_service.h"
#include "middleware/net/wifi_manager.h"
#include "middleware/net/net_time_service.h"
#include "middleware/camera/camera_service.h"
#include "middleware/camera/capture_service.h"       /* ★ S2a 拍照 */
#include "middleware/camera/viewfinder.h"            /* ★ 取景器（2026-09-28 起按需分配） */
#include "middleware/media/record_video_service.h"   /* ★ S2b 录像 */
#include "middleware/media/file_mgr_service.h"       /* ★ S4 文件管理（预览/播放缓冲） */
#include "middleware/camera/face_recog_service.h"    /* ★ S8-0 MFN 可行性自检 */
#include "middleware/face_db/face_db.h"              /* ★ S8-1 人脸库（特征索引落盘） */
#include "middleware/camera/punch_service.h"         /* ★ S8-2b 打卡落盘（开机复位一次状态） */
#include "middleware/mem_diag.h"

static const char *TAG = "main";

/* 本次上电周期内，开机提示音是否已播过（只在第一次上滑进主界面时播一次） */
static bool s_boot_tone_played __attribute__((unused)) = false;

/* Swipe-up on splash recognized -> push the main page. */
static void on_splash_enter(void)
{
    /* ★ P0-1 补盲区：原来"after splash load"之后直到底层 camera 埋点才有下一行，
     *   中间把 WiFi 关联 / TLS / 主页构建全糊在一起（实测内部 largest 就在这段里
     *   从 90,112 掉到 55,296）。这里按"进主页前/后"切开。 */
    MEM_STAGE(TAG, "splash->main: pre");
    PageManager_Load(&Page_Main);
    MEM_STAGE(TAG, "splash->main: post");

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

    /* STA 真正拿到 IP 才开始校时/取天气；AP 热点自身不算联网 */
    if (evt == WIFI_EVT_GOT_IP) {
        net_time_service_start();
    }
}

/* ============ ★ 花屏调查脚手架（定位后 EN 改 0 或整段删除）============
 *
 * ★★ 2026-09-25：花屏**根因已修**（`sdkconfig` 改用 `BSP_LCD_RGB_REFRESH_AUTO=y`，
 *    关掉 bounce buffer 模式）。本脚手架**保留**，因为它测的两件事仍然有用，
 *    只是判据变了（见下）。先留档"改前的机制"，那是根因推导的依据：
 *
 * ── 改前（bounce buffer 模式）──────────────────────────────────────────
 *   旧配置：CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_MODE=y + HEIGHT=10，fb_in_psram=1，num_fbs=2。
 *   ⇒ GDMA 每消费 800x10 px（16,000 B）就发一次 EOF 中断；中断处理函数
 *     lcd_rgb_panel_fill_bounce_buffer() 里必须把 PSRAM 帧缓冲的这 16,000 B
 *     memcpy 进内部 bounce buffer（esp_lcd/rgb/esp_lcd_panel_rgb.c:1042）。
 *     ★ IDF 在这一行自己写了警告 —— 这就是根因的铁证：
 *       "if the frame buffer is behind a cache, and the cache is disabled,
 *        crash would happen here when auto write back happens"
 *   ⇒ pclk = 18 MHz ⇒ 800x10 px / 18 MHz = 【444 us】一个中断，1699 次/秒。
 *
 *   花屏的根因：flash 擦写窗口（spi_flash_disable_interrupts_caches_and_other_cpu）
 *   内 caches 被禁，而本工程 `LCD_RGB_ISR_IRAM_SAFE=n`（sdkconfig:1816）⇒ 该中断
 *   **被一起关掉** ⇒ 不 crash，但这 16,000 B 没被搬进 bounce buffer ⇒ 画面错位。
 *   ★ 所以 `ISR_IRAM_SAFE` 是**反向开关**：打开它只会把"花屏"变成"崩溃"（ISR 会
 *     真的去读 PSRAM，撞上上面那条 IDF 警告）。**别开。**
 *   ★ ESP32-S31 上还没有任何自愈机制：`#if CONFIG_IDF_TARGET_ESP32S3` /
 *     RGB_LCD_NEEDS_SEPARATE_RESTART_LINK 分支全被编译掉 ⇒ 没有"每 VBlank 复位
 *     GDMA"、没有 bb_eof_count 欠载检测、esp_lcd_rgb_panel_restart() 直接
 *     return ESP_ERR_NOT_SUPPORTED ⇒ 一旦错位【不自愈】。
 *   ★ 同一中断还每秒吃掉 444us x 1699 ≈ 55%~75% 的 CPU ⇒ 也是 WiFi
 *     reason=200 (BEACON_TIMEOUT) 断联的贡献项之一。
 *
 * ── 改后（auto refresh 模式）：软件不再参与供数 ──────────────────────────
 *   整个 768,000 B framebuffer 预串成一条 DMA link list（:1211-1236），GDMA 自己
 *   顺着描述符扫完整帧；EOF 时只做回调（:1084-1090），**不再有 memcpy**。
 *   ⇒ flash 擦写窗口内没有任何"必须由软件完成"的动作 ⇒ **花屏根因被结构性消除**
 *     （不是"把窗口变小"，是那个动作不存在了）。
 *   ⇒ 附带收益：显示中断 1699 次/秒 → ~35 次/秒（on_vsync）；省 32,000 B 内部 DMA。
 *   ⇒ cache 一致性由驱动自己在 flush 时 `esp_cache_msync(...C2M...)` 兜住，条件正是
 *     `!bb_size`（:750-753 / :764-767）—— 这条路径是它设计好的，不是我们在绕。
 *
 * 本脚手架只读真值、不改行为：
 *   (1) gap  ：1 kHz esp_timer 量两次回调的真实间隔 —— 大于 1 ms 的间隔就说明
 *              那段时间中断被关（flash 擦写）或 CPU 被长期占住。
 *   (2) memcpy：周期性量一次 16 KB PSRAM→内部 RAM 搬运的最坏耗时。
 * 判读（★ 已随根因修复而变化）：
 *   · gap_max 出现 5~50 ms 尖峰且紧跟 WiFi 重连日志 ⇒ 命中 flash 擦写窗口。
 *     **这条与改配置无关、仍然成立** —— 窗口本身没被消除，它现在只会打乱 WiFi/音频，
 *     不再造成花屏（已经没有"需要软件补供"的缓冲了）。
 *   · memcpy16k **不再有 444 us 死线**，改成「PSRAM 带宽健康度」指标：改后 GDMA 会
 *     持续从 PSRAM 读 18 MHz x 2 B = 36 MB/s。请与**改前的历史日志**在同负载下对比 ——
 *     明显变差 ⇒ 带宽竞争加剧（这是本次切换唯一的反向风险）；持平/变好 ⇒ 无副作用。
 *   ★ 尺寸保持 16,000 B 不动，就是为了**与改前日志逐字可比**。
 * ★ 代价：它自己占 16,000 B 内部 RAM + 周期性 PSRAM 读。
 *
 * ★★ 探针自己就是负载，别把它定激进了（观察者效应）：
 *   连续 N 次 memcpy 每次 ~300 us（空闲态），这几百 us 内探针**独占 PSRAM**。
 *   原参数 200 ms x 8 次 = 每 200 ms 制造一个 2.4 ms 的 PSRAM 独占窗口 ⇒ 探针自己
 *   就能把 memcpy16k 读数顶上去（改前还会顺手制造花屏）。
 *   现参数 1000 ms x 2 次：负载降 20 倍（每秒 2 次 vs 40 次），1 s 汇总仍有 2 个样本。
 *   ★ 真要长时间挂着跑，把 PANEL_PROBE_ENABLE 置 0 重新烧一次最干净。
 *
 * ★★★ 2026-09-25 22:09 上机后**已置 0**（用户确认），理由：
 *   (1) 使命完成 —— 花屏根因已由 `BSP_LCD_RGB_REFRESH_AUTO=y` 修掉，探针自己都打"无死线"；
 *   (2) "与改前对比 PSRAM 带宽"这个用途**也已完成**：那次上电日志给出了"改后"基线
 *       （稳态 avg 85~140 µs / 最坏 452 µs 出现在 TLS 握手期），**已留档在
 *       `docs/项目规划/S2_验收日志分析_2026-09-25.md` §3-A**，不必再挂探针；
 *   (3) 代价是真金白银：常驻 **16,000 B 内部 RAM**，直接把 `largest_free_block`
 *       从 118,784 压到 102,400 —— 而内部 RAM 本是本工程的头号结构性风险。
 *   ⇒ 需要复测时把这里改回 1 再烧；代码与判读说明全部保留（只是不再参与编译）。
 */
#define PANEL_PROBE_ENABLE      0
#if PANEL_PROBE_ENABLE
#define PROBE_COPY_BYTES        16000   /* 与改前历史日志可比（原 = 800x10 px x 2 B），勿改 */
#define PROBE_REF_US            444     /* 改前 bb 模式的死线；现在只是参考线，不是判据 */
#define PROBE_COPY_PERIOD_TICK  1000    /* 每 1000 个 1 ms tick（=1 s）搬一组 */
#define PROBE_COPIES_PER_ROUND  2

static uint8_t *s_probe_src;            /* PSRAM，代表屏帧缓冲所在的区域 */
static uint8_t *s_probe_dst;            /* 内部 RAM */
static esp_timer_handle_t s_probe_timer;
static volatile uint32_t s_probe_last_us;
static volatile uint32_t s_probe_gap_max_us;
static volatile uint32_t s_probe_gap_gt1ms;
static volatile uint32_t s_probe_gap_gt5ms;
static volatile uint32_t s_probe_copy_max_us;
static volatile uint32_t s_probe_copy_sum_us;
static volatile uint32_t s_probe_copy_n;
static volatile uint32_t s_probe_tick;

static void panel_probe_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    /* (1) 回调间隔：>1 ms 说明这段时间中断被关 / CPU 被占 */
    uint32_t gap = (uint32_t)(now - s_probe_last_us);
    s_probe_last_us = (uint32_t)now;
    if (gap > s_probe_gap_max_us) s_probe_gap_max_us = gap;
    if (gap > 1000) s_probe_gap_gt1ms++;
    if (gap > 5000) s_probe_gap_gt5ms++;

    /* (2) 16 KB PSRAM -> 内部 RAM 搬运耗时：现在**只是 PSRAM 带宽健康度指标**
     *     （改前它模拟的是 bounce ISR 那次 memcpy，那时有 444 us 死线）。 */
    if ((s_probe_tick % PROBE_COPY_PERIOD_TICK) == 0) {
        for (int i = 0; i < PROBE_COPIES_PER_ROUND; i++) {
            int64_t c0 = esp_timer_get_time();
            memcpy(s_probe_dst, s_probe_src, PROBE_COPY_BYTES);
            uint32_t c = (uint32_t)(esp_timer_get_time() - c0);
            if (c > s_probe_copy_max_us) s_probe_copy_max_us = c;
            s_probe_copy_sum_us += c;
            s_probe_copy_n++;
        }
    }

    if (++s_probe_tick >= 1000) {   /* 每 1 s 汇总 */
        ESP_LOGW(TAG, "[panelprobe] gap max=%u us (>1ms:%u >5ms:%u) | "
                      "memcpy16k avg=%u max=%u us (PSRAM 带宽指标, 无死线, ref %u)",
                 (unsigned)s_probe_gap_max_us,
                 (unsigned)s_probe_gap_gt1ms, (unsigned)s_probe_gap_gt5ms,
                 (unsigned)(s_probe_copy_n ? (s_probe_copy_sum_us / s_probe_copy_n) : 0),
                 (unsigned)s_probe_copy_max_us, PROBE_REF_US);
        s_probe_tick = 0;
        s_probe_gap_max_us = 0;
        s_probe_gap_gt1ms = 0;
        s_probe_gap_gt5ms = 0;
        s_probe_copy_max_us = 0;
        s_probe_copy_sum_us = 0;
        s_probe_copy_n = 0;
    }
}

static void panel_probe_start(void)
{
    s_probe_src = heap_caps_aligned_alloc(64, PROBE_COPY_BYTES, MALLOC_CAP_SPIRAM);
    s_probe_dst = heap_caps_aligned_alloc(64, PROBE_COPY_BYTES,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_probe_src || !s_probe_dst) {
        ESP_LOGE(TAG, "panelprobe alloc failed (src=%p dst=%p), probe off",
                 s_probe_src, s_probe_dst);
        return;
    }
    memset(s_probe_src, 0x5A, PROBE_COPY_BYTES);

    esp_timer_create_args_t a = {
        .callback = panel_probe_cb,
        .name = "panelprobe",
    };
    if (esp_timer_create(&a, &s_probe_timer) != ESP_OK) {
        ESP_LOGE(TAG, "panelprobe timer create failed");
        return;
    }
    s_probe_last_us = (uint32_t)esp_timer_get_time();
    if (esp_timer_start_periodic(s_probe_timer, 1000) != ESP_OK) {
        ESP_LOGE(TAG, "panelprobe timer start failed");
        return;
    }
    ESP_LOGW(TAG, "[panelprobe] on: 1 kHz gap + 16 KB PSRAM->INT (带宽指标, ref %d us), "
                  "costs %d B internal RAM",
             PROBE_REF_US, PROBE_COPY_BYTES);
}
#endif /* PANEL_PROBE_ENABLE */

/* ============================================================================
 *  ★ S8-0：MFN（人脸特征提取）可行性自检 —— 2026-09-28
 *
 *  目的：在动 S8 全套之前，用最小代价回答**一个**问题：
 *    「MFN 的 p4 变体到底能不能在 esp32s31 上加载并推理？」
 *
 *  为什么必须实测而不能推断：官方 human_face_recognition 的 README 芯片支持表
 *    只列了 **ESP32-S3 / ESP32-P4**，**没有 esp32s31**。
 *    缓解证据是本工程 human_face_detect 的 CMakeLists 已把 s31 当 p4 同代处理
 *    （`esp32p4 OR esp32s31 → models/p4`），且 FACE 档的 p4 模型确实在跑 ——
 *    但那是"高度可能"，不是"已验证"。这一条只能靠上机锤死。
 *
 *  它做什么（细节全在 face_recog_service.cpp 里）：
 *    ① 查模型文件 → ② PSRAM **连续块**预检（§61 的闸门）→ ③ 加载 MFN
 *    → ④ 用合成图跑一次前向 → ⑤ 打印 feat_len / 耗时 / L2 模长 → ⑥ 释放（不影响后续功能）
 *
 *  怎么读日志：串口搜 "S8-0"，正常会看到
 *      [OK-1/3] → [OK-2/3] → "loaded in ...ms" → "S8-0 PROBE PASS"
 *      ⇒ A1 路线成立，可以进 S8-1（人脸库 + 录入模式）；
 *    出现任一 [FAIL-n/3] ⇒ 紧跟其后那行 "-> ..." 就是针对该失败模式的下一步动作。
 *
 *  ★ 默认置 1（本次烧录就是为了验证）；**验证完请置 0 再烧一次**，别留在正式固件里：
 *    MFN 加载要 1.29 MB 连续 PSRAM + 数秒 CPU，挂在开机路径上纯属白费。
 *  ★ 编排方式与上面的 PANEL_PROBE_ENABLE 一致（本工程既有惯例）。
 * ========================================================================== */
#define FACE_RECOG_PROBE_ENABLE     0
#if FACE_RECOG_PROBE_ENABLE

/** 延时多久才开跑：避开开机高峰。
 *  ★ 不能是 0 —— 此刻 WiFi / 小智 / 文件管理都在抢占，PSRAM 也刚被切过。
 *    延时几秒还有个诊断价值：若 3 s 后 largest 仍然不足，那就是**真碎片**，
 *    而不是"启动瞬间的抖动"，两者的对策完全不同。 */
#define RECOG_PROBE_DELAY_MS        3000

/** 自检任务栈：走 esp-dl 前向 + 日志格式化。
 *  ★ 栈必须在**内部 RAM**（PSRAM 当任务栈与 cache 的交互有坑，本工程铁律）。
 *  ★ 这是一次性任务（跑完自删），所以**不登记进 s_task_reg** ——
 *    那张表是给长期任务看栈水位的，登记一个已删除的任务只会打出误导性的 0。
 *  ⚠ 若日志出现 "***ERROR*** A stack overflow in task recog_probe"，
 *    把这里调到 16384 再烧（esp-dl run() 的栈需求随模型结构变化）。 */
#define RECOG_PROBE_STACK           10240

static void face_recog_probe_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(RECOG_PROBE_DELAY_MS));

    face_recog_probe(NULL);          /* 传 NULL：只打日志，不需要结果快照 */

    /* 自删。★ 必须是最后一行 —— 之后不能再碰任何栈上变量。 */
    vTaskDelete(NULL);
}
#endif /* FACE_RECOG_PROBE_ENABLE */

/* ==========================================================================
 *  ★ S8-1：人脸库（face_db）自检 —— **存储层验收，不需要 UI、不需要识别模型**
 *
 *  为什么值得单独写一个自检：face_db 只收"调用方算好的特征向量"，
 *  与"MFN 到底能不能在 s31 上跑"**完全无关**。所以存储层的验收标准可以在
 *  S8-0 结论出来之前就独立锤死，两条线并行推进：
 *    ① 录入 3 人 → deinit → 重新 init → 数据仍在（验双份世代号落盘）
 *    ② 录满 10 人后第 11 人**被明确拒绝**（需求 R9）
 *    ③ 重名被挡；库外特征不误报（余弦阈值生效）
 *    ④ 拔 SD 卡 → init 干净失败、is_ready()==false（此时人脸检测应照常可用）
 *
 *  ★ 全程在 "/sdcard/face_db_selftest" 这个独立根目录里演，**绝不碰真库**
 *    （"/sdcard/face_db/"，真库）；跑完连文件带目录一起删掉，所以每次开机都从空库开始（幂等）。
 *
 *  ⚠ 写这段注释时踩过一次坑：注释里**千万别写 `*` 紧跟 `/` 的序列**（比如把路径
 *    写成 markdown 加粗 "**" + "/sdcard"），GCC 会把它当成注释结束符 `*`+`/`，
 *    于是整段注释变成代码、爆出几十条莫名其妙的错。
 *
 *  ★ 默认置 1（本次烧录就是为了验收）；**验完请置 0 再烧一次**，别留在正式固件里
 *    （它会真往 SD 卡写一个临时目录，虽然跑完就删）。
 * ========================================================================== */
#define FACE_DB_PROBE_ENABLE        0
#if FACE_DB_PROBE_ENABLE

#define FDB_PROBE_ROOT              "/sdcard/face_db_selftest"
#define FDB_PROBE_DELAY_MS          4500    /* 错开 RECOG_PROBE(3000ms)，免得抢 CPU/PSRAM */
#define FDB_PROBE_STACK             8192
#define FDB_PROBE_DIM               FACE_DB_DIM_MAX   /* 用真维度(512)跑，别用 8 维糊弄自己 */
#define FDB_PROBE_NAMEBUF           32

/* ==========================================================================
 *  ★ S8-1 的 UI 部分（库管理页）需要一个**有数据**的库才能验收：
 *    空库只能验"空态提示"，验不了列表排版 / 中文姓名字体 / 删除按钮 ——
 *    而"中文名在两套 CJK 字体下会不会出方块"恰恰是本次 UI 唯一的真风险点。
 *
 *  置 1  = 往**真库** (/sdcard/face_db) 写入 3 条演示数据（张三/李四/王五），
 *          随后可以在 SYS → Face DB (S8) 页面里看到 3 行、逐个删掉。
 *  置 0  = 什么都不做（默认）。
 *
 *  ★★ 验完请务必置回 0 并重烧：它写的是**真库**，正式固件里留 3 个假人
 *     会让 S8-2 的打卡记录里凭空多出三个不存在的人。
 *  （想清掉也不用重烧：在 Face DB 页把那 3 行逐个删掉即可。）
 *
 *  ★ 用 #ifndef 包住（与上面两个 *_PROBE_ENABLE 的写法不同，是有意的）：
 *    · 让 `-DFDB_PROBE_SEED_DEMO=1` 能在**不改源码**的情况下打开它
 *      （`.workbuddy/probe_idf_syntax.py` 就是这么静态校验这个分支的，
 *       见该脚本文件头 "校验 #if 开关里的代码"）；
 *    · 也给将来"验收脚本按参数烧不同的种子"留了口子。
 * ========================================================================== */
#ifndef FDB_PROBE_SEED_DEMO
#define FDB_PROBE_SEED_DEMO         0
#endif

static int s_fdb_probe_fail;

static void fdb_probe_expect(bool ok, const char *what)
{
    if (ok) {
        ESP_LOGI(TAG, "  [ok]   %s", what);
    } else {
        ESP_LOGE(TAG, "  [FAIL] %s", what);
        s_fdb_probe_fail++;
    }
}

/** 造一份**确定性**特征：第 k 维为 1、其余为 0。
 *  ⇒ 任意两份**正交**（余弦 0.0），与自身余弦 1.0。
 *  这样 find_best 的阈值与排序可以被精确断言，不受模型质量影响。 */
static void fdb_probe_make_feat(float *out, int dim, int k)
{
    memset(out, 0, sizeof(float) * (size_t)dim);
    if (k >= 0 && k < dim) {
        out[k] = 1.0f;
    }
}

/** 清场：删掉自检目录下的全部文件，再删掉空目录。
 *  ★ 必须**先 face_db_deinit() 再调** —— 库还开着这些文件时删它们是自找麻烦。 */
static void fdb_probe_cleanup(void)
{
    static const char *files[] = { "persons.idx.0", "persons.idx.1", "feats.bin" };
    char path[160];
    int  i;

    for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
        snprintf(path, sizeof(path), "%s/%s", FDB_PROBE_ROOT, files[i]);
        unlink(path);                       /* 不存在也无所谓，幂等 */
    }
    for (i = 1; i <= FACE_DB_MAX; i++) {
        snprintf(path, sizeof(path), "%s/thumbs/%02d.jpg", FDB_PROBE_ROOT, i);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/thumbs", FDB_PROBE_ROOT);
    rmdir(path);
    snprintf(path, sizeof(path), "%s", FDB_PROBE_ROOT);
    rmdir(path);
}

static void face_db_probe_run(void)
{
    /* ★ 所有声明放顶部：本函数用 goto 统一收尾（清场 + 汇总），
     *   跳转跨过带初始化的声明是经典踩坑点（本工程既有惯例）。 */
    static const char *NAMES[3] = { "张三", "李四", "王五" };
    face_db_info_t info;
    face_db_match_t mch;
    face_person_t  p;
    esp_err_t      err  = ESP_OK;
    uint16_t       id   = 0;
    int            at   = -1;
    int            i    = 0;
    float          feat[FACE_DB_DIM_MAX];
    char           name[FDB_PROBE_NAMEBUF];
    char           dirbuf[64];

    ESP_LOGI(TAG, "================ S8-1 face_db self test ================");
    s_fdb_probe_fail = 0;

    /* ---- ① 拔卡降级路径（放最前：后面所有用例都要求卡在位）---- */
    if (!sdcard_service_is_mounted()) {
        ESP_LOGW(TAG, "[NOTE] SD 未挂载 -> 落盘用例全部跳过。");
        ESP_LOGW(TAG, "  -> 这本身就是一条验收项：此刻人脸检测等其它功能应**照常可用**。");
        err = face_db_init();               /* 走默认根，应当干净失败 */
        ESP_LOGI(TAG, "  default init rc=%s, ready=%d（期望 rc!=OK 且 ready=0）",
                 esp_err_to_name(err), (int)face_db_is_ready());
        fdb_probe_expect(err != ESP_OK,      "SD 缺失时 init 干净失败（不崩、不死循环）");
        fdb_probe_expect(!face_db_is_ready(), "SD 缺失时 is_ready()==false（降级而非致命）");
        goto summary;
    }

    /* ---- ② 清场 + 空库 init ---- */
    face_db_deinit();                       /* 清掉上次自检留下的内存状态 */
    fdb_probe_cleanup();

    err = face_db_init_at(FDB_PROBE_ROOT);
    fdb_probe_expect(err == ESP_OK, "init_at(独立根) 成功");
    if (err != ESP_OK) {
        goto summary;
    }

    face_db_get_info(&info);
    fdb_probe_expect(info.ready && info.count == 0 && info.free_slots == FACE_DB_MAX,
                     "空库状态正确（0/10，free_slots=10）");

    /* ---- ③ 录入 3 人 ---- */
    for (i = 0; i < 3; i++) {
        fdb_probe_make_feat(feat, FDB_PROBE_DIM, i);
        snprintf(name, sizeof(name), "%s", NAMES[i]);
        err = face_db_enroll(name, feat, FDB_PROBE_DIM, 3, &id);
        if (err != ESP_OK) {
            break;
        }
    }
    fdb_probe_expect(err == ESP_OK, "录入 3 人成功");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "  -> 第 3 人的 id = %u（期望 3）", (unsigned)id);
        fdb_probe_expect(id == 3, "id 按 1,2,3 顺序分配");
    }
    face_db_get_info(&info);
    fdb_probe_expect(info.count == 3, "count == 3");

    /* ---- ④ 重名被挡 ---- */
    fdb_probe_make_feat(feat, FDB_PROBE_DIM, 3);
    err = face_db_enroll("张三", feat, FDB_PROBE_DIM, 3, NULL);
    fdb_probe_expect(err == ESP_ERR_INVALID_STATE, "重名录入被挡（INVALID_STATE）");

    /* ---- ⑤ 查库：正交特征应精确命中"自己" ---- */
    fdb_probe_make_feat(feat, FDB_PROBE_DIM, 1);        /* 李四的特征（第 1 维） */
    at = face_db_find_best(feat, FDB_PROBE_DIM, 0.6f, &mch);
    fdb_probe_expect(at >= 0 && mch.matched && strcmp(mch.person.name, "李四") == 0,
                     "find_best 命中李四");
    ESP_LOGI(TAG, "  -> score=%.4f name=%s", (double)mch.score, mch.person.name);

    fdb_probe_make_feat(feat, FDB_PROBE_DIM, 100);      /* 库外特征：与库内全部正交 */
    at = face_db_find_best(feat, FDB_PROBE_DIM, 0.6f, &mch);
    fdb_probe_expect(at >= 0 && !mch.matched, "库外特征不误报（低于阈值判为访客）");
    ESP_LOGI(TAG, "  -> 最高分 = %.4f（期望 0.0）", (double)mch.score);

    /* ---- ⑥ 补录到满 10 人，第 11 人必须被拒（R9）---- */
    for (i = 3; i < FACE_DB_MAX; i++) {
        fdb_probe_make_feat(feat, FDB_PROBE_DIM, i);
        snprintf(name, sizeof(name), "P%02d", i);
        err = face_db_enroll(name, feat, FDB_PROBE_DIM, 3, &id);
        if (err != ESP_OK) {
            break;
        }
    }
    fdb_probe_expect(err == ESP_OK, "补录到满 10 人");
    face_db_get_info(&info);
    fdb_probe_expect(info.count == FACE_DB_MAX && info.free_slots == 0,
                     "满库状态正确（10/10，free_slots=0）");

    fdb_probe_make_feat(feat, FDB_PROBE_DIM, 200);
    err = face_db_enroll("第11人", feat, FDB_PROBE_DIM, 3, NULL);
    fdb_probe_expect(err == ESP_ERR_NO_MEM, "第 11 人被明确拒绝（NO_MEM，不静默覆盖）");

    /* ---- ⑦ 打卡目录名（这是 punch_service 唯一的耦合点，先锁住格式）---- */
    err = face_db_attendance_dir(1, "张三", dirbuf, sizeof(dirbuf));
    fdb_probe_expect(err == ESP_OK && strcmp(dirbuf, "01_张三") == 0, "目录名 == 01_张三");
    err = face_db_attendance_dir(FACE_DB_GUEST_ID, NULL, dirbuf, sizeof(dirbuf));
    fdb_probe_expect(err == ESP_OK && strcmp(dirbuf, "99_访客") == 0, "访客目录名 == 99_访客");
    err = face_db_attendance_dir(2, "a/b:c*?", dirbuf, sizeof(dirbuf));
    fdb_probe_expect(err == ESP_OK && strcmp(dirbuf, "02_a_b_c__") == 0,
                     "FATFS 非法字符被替换为 '_'");

    /* ---- ⑧ "模拟重启"：deinit → 重新 init → 数据必须原样还在 ---- */
    face_db_deinit();
    err = face_db_init_at(FDB_PROBE_ROOT);
    fdb_probe_expect(err == ESP_OK, "重新 init 成功（模拟重启）");
    face_db_get_info(&info);
    fdb_probe_expect(info.count == FACE_DB_MAX, "重载后 10 人仍在（验双份世代号）");
    fdb_probe_expect(info.feat_dim == FDB_PROBE_DIM, "重载后 dim 正确");

    err = face_db_find_by_name("李四", &p);
    fdb_probe_expect(err == ESP_OK && p.id == 2, "重载后按名查到李四，id 稳定 == 2");

    /* ---- ⑨ 删除也要落盘 ---- */
    err = face_db_delete(2);
    fdb_probe_expect(err == ESP_OK, "删除李四成功");
    face_db_deinit();
    err = face_db_init_at(FDB_PROBE_ROOT);
    face_db_get_info(&info);
    fdb_probe_expect(err == ESP_OK && info.count == FACE_DB_MAX - 1,
                     "删除结果已落盘（重载后 9 人）");
    err = face_db_find_by_name("李四", &p);
    fdb_probe_expect(err == ESP_ERR_NOT_FOUND, "李四确实没了");

summary:
    face_db_deinit();
    fdb_probe_cleanup();

    /* ★★ 关键收尾：把库**还原到开机时的真根目录**。
     *   本自检为了不污染真库，把实例切到了 FDB_PROBE_ROOT（face_db_init_at 会
     *   先 deinit 掉当前实例），如果就这样收工，全局实例就一直空着/指向自检目录 ——
     *   而 UI（SYS → Face DB 页 / CAM 页 ENROLL 态）读的是这个全局实例，
     *   结果就是"库明明有数据，页面却显示 0/10 或 not ready"。
     *   所以这里补一次 face_db_init()（等价于 app_main 开机那次；SD 缺失时它
     *   会干净地失败，不影响任何功能）。 */
    face_db_init();

    if (s_fdb_probe_fail == 0) {
        ESP_LOGI(TAG, "============ S8-1 PROBE PASS（face_db 存储层 OK）============");
        ESP_LOGI(TAG, "  -> 可继续 S8-1 的 UI 部分：MODE 按钮 + 库管理页");
    } else {
        ESP_LOGE(TAG, "============ S8-1 PROBE FAIL：%d 项未过 ============", s_fdb_probe_fail);
        ESP_LOGE(TAG, "  -> 上面每行 [FAIL] 就是判据；把整段日志贴回来");
    }
    ESP_LOGI(TAG, "=========================================================");
}

#if FDB_PROBE_SEED_DEMO
/** 往**真库**写 3 条演示数据（仅为 UI 验收，见上面 FDB_PROBE_SEED_DEMO 的说明）。
 *  ★ 写之前不删库：若已有数据，重名会被 face_db_enroll 挡下（INVALID_STATE），
 *    那是**预期**行为，不是错误 —— 所以这里只把非预期错误当失败。 */
static void fdb_seed_demo(void)
{
    static const char *NAMES[3] = { "张三", "李四", "王五" };
    face_db_info_t info;
    float     feat[FACE_DB_DIM_MAX];
    char      name[FDB_PROBE_NAMEBUF];
    esp_err_t err;

    err = face_db_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[SEED] face_db_init failed: %s -> 没有 SD 卡就没法播种子，跳过",
                 esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < 3; i++) {
        fdb_probe_make_feat(feat, FDB_PROBE_DIM, i);
        snprintf(name, sizeof(name), "%s", NAMES[i]);
        err = face_db_enroll(name, feat, FDB_PROBE_DIM, 3, NULL);
        ESP_LOGW(TAG, "[SEED] enroll %s -> %s", name, esp_err_to_name(err));
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            break;                  /* INVALID_STATE = 重名（已存在），继续下一条 */
        }
    }

    face_db_get_info(&info);
    ESP_LOGW(TAG, "[SEED] 真库现在 %d/%d 人 —— 去 SYS -> Face DB (S8) 看列表，"
                  "验完请把 FDB_PROBE_SEED_DEMO 置 0 重烧", info.count, FACE_DB_MAX);
}
#endif /* FDB_PROBE_SEED_DEMO */

static void face_db_probe_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FDB_PROBE_DELAY_MS));
    face_db_probe_run();
#if FDB_PROBE_SEED_DEMO
    fdb_seed_demo();
#endif
    vTaskDelete(NULL);              /* ★ 最后一行，之后不能再碰栈上变量 */
}
#endif /* FACE_DB_PROBE_ENABLE */

/* ★ P1-4 配套验证 + P2-4 数据前提：全任务栈水位上报表。
 *
 * 一次性回答"哪些任务栈该缩、能缩多少"（方案 §4-P2-4：75,776 B 里可能藏着 20~30 KB）。
 *
 * ★ 手法说明（换过方案，别再试 uxTaskGetSystemState）：
 *   `uxTaskGetSystemState()` 需要 CONFIG_FREERTOS_USE_TRACE_FACILITY —— 本工程**未开**
 *   （`sdkconfig:2683`），而且它的 TaskStatus_t **没有"栈尺寸"字段**，
 *   打不出"建议尺寸"。所以改用：**静态登记表 + xTaskGetHandle()**。
 *   - `xTaskGetHandle()` 只要求 INCLUDE_xTaskGetHandle=1，IDF 默认就是 1
 *     （`freertos/FreeRTOSConfig.h:216`）⇒ **零 Kconfig 改动、零额外 RAM**。
 *   - 尺寸只能靠下面这张表（FreeRTOS 没有"查栈大小"的公开 API）。
 *   ★ **新增任务时必须同步这张表**（见 `工程规范/10_新增模块CheckList.md`），
 *     否则这里只会打出 hwm、给不出建议值。
 *
 * ★★ 判读口径（先读这段，这张表**很容易读反**，第一版就写反过）：
 *   `uxTaskGetStackHighWaterMark()` 返回的是「**历史最小剩余空闲**字节」，
 *   不是"用了多少"。所以：
 *       峰值用量  used = size - hwm     （hwm 越大 = 用得越少）
 *       安全下限  need = used × 1.5 向上取整到 512（方案 §4-P2-4 的公式，换算成用量口径）
 *   判据：
 *     · hwm ≤ size/8  ⇒ 余量已不足 1/8，标 "!! 偏紧" ⇒ 该**加**栈，不该缩。
 *     · size - need ≥ 2048 ⇒ 才值得缩（省不到 2 KB 不值得冒栈溢出的风险）。
 *     · 其余（余量 12.5%~25%）⇒ 不动。
 *
 * ★★ 加栈 / 回退的**唯一判据就是本报表**（机器判定），别在别处再手写一条阈值。
 *   本文件曾经在下面的 `bsp_display_cfg` 那段写过一条"hwm 掉到 4 KB 以下就回退"，
 *   那条是 **size = 12288 时**写的，等价于"峰值用量别超 8192"；栈缩到 9728 之后，
 *   同一条等价于"别超 5632"，而实测峰值 6576 ⇒ 照字面**永远**该回退 ——
 *   与"按 ×1.5 缩到 9728"这个决定直接打架。2026-09-28 实测 hwm=3152 时踩到这一点，
 *   两处手写阈值已改为指向本节。
 *   ★ 反例警告（第一版就是错的）：拿 hwm 去 ×1.5 当 need，会让"几乎没用栈"的任务
 *     （hwm≈size）算出 need>size 被误标"偏紧"，让"吃满栈"的任务（hwm≈0）算出
 *     need≈0 被推荐缩到 1536 —— 结论完全反了，照着改必崩。
 *   ★ need 只是**下限**：某些路径（开机、切页、异常分支）可能没被本次观测覆盖，
 *     实际落地的尺寸可以在此基础上再留余量，见 `s_task_reg` 里的逐项注释。
 * 这是脚手架：按水位改完尺寸、并连跑 30 min 不崩之后，把本表 + 本函数一起删掉。 */
typedef struct {
    const char *name;
    uint32_t    size;
} task_slot_t;

static const task_slot_t s_task_reg[] = {
    /* ★ 本表必须与真实创建处**逐字同步**，否则水位百分比/可缩建议全是错的。
     *   已核对的创建处：esp_lvgl_port（本文件覆盖 12288，实测峰值 6,576 B）、
     *   audio_service.c / net_time_service.c / camera_service.c（CAM_TASK_STACK）/
     *   led_service.c。
     *   ★ 2026-09-27 call/rec 阉割：record_service / udp_voice / link_manager 的
     *     任务已随功能删除，表中对应行移除。 */
    /* ★ 2026-09-26 按水位全面收缩（本次 [stack] 报告：可省 ~32 KB 内部 RAM）。
     *   只缩"跑过全路径"的任务。
     *   回退条件：回归后任一任务被判"偏紧"或出现栈溢出症状。 */
    { "taskLVGL",       9728 },   /* 峰值 6396→6576（2026-09-28 复测），need=9864 ⇒ 略欠 512 B */
    { "audio_task",      2048 },  /* 峰值 756 */
    { "rec_task",        4096 },  /* §40 本地录音：SD FATFS IO，内部栈（F35） */
    { "net_time",        8192 },  /* 峰值 4296，吃满一半，不动 */
    { "cam",             3584 },  /* 两次观测大者 2140（含同步写盘路径）×1.5 */
    { "led_task",        1536 },  /* 峰值 656 */
};

static void stack_report_dump(void)
{
    uint32_t total_size = 0, total_need = 0, total_savable = 0;

    for (size_t i = 0; i < sizeof(s_task_reg) / sizeof(s_task_reg[0]); i++) {
        const char *nm = s_task_reg[i].name;
        uint32_t    sz = s_task_reg[i].size;
        TaskHandle_t h = xTaskGetHandle(nm);

        total_size += sz;
        if (h == NULL) {
            ESP_LOGI(TAG, "[stack] %-15s size=%-6u not running", nm, (unsigned)sz);
            total_need += sz;      /* 未创建时按原尺寸计入，别虚报可省量 */
            continue;
        }

        /* 口径见函数头注释：used = size - hwm 才是"峰值用量"。 */
        uint32_t hwm  = (uint32_t)uxTaskGetStackHighWaterMark(h);
        uint32_t used = (sz > hwm) ? (sz - hwm) : 0;
        uint32_t need = (((used * 3u) / 2u) + 511u) / 512u * 512u;   /* ×1.5 向上取整 512 */
        if (need < 1024u) need = 1024u;

        char        vs[48] = "";
        const char *verdict = vs;
        if (hwm <= sz / 8u) {
            snprintf(vs, sizeof(vs), "  !! 偏紧 hwm=%u%%.%u 该加栈",
                     (unsigned)(hwm * 100u / sz),
                     (unsigned)((hwm * 1000u / sz) % 1000u / 100u));
        } else if (need + 2048u <= sz) {
            snprintf(vs, sizeof(vs), "  <-- 可缩到 %u (省 %u)", (unsigned)need,
                     (unsigned)(sz - need));
            total_savable += (sz - need);
        } else {
            verdict = "";
        }
        total_need += (need < sz) ? need : sz;
        ESP_LOGI(TAG, "[stack] %-15s size=%-6u used=%-6u hwm=%-6u need=%-6u%s",
                 nm, (unsigned)sz, (unsigned)used, (unsigned)hwm, (unsigned)need, verdict);
    }

    ESP_LOGI(TAG, "[stack] TOTAL size=%u B | by-watermark need=%u B | 可省=%u B | tasks=%u",
             (unsigned)total_size, (unsigned)total_need,
             (unsigned)total_savable, (unsigned)uxTaskGetNumberOfTasks());
}

static void stack_hwm_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t tick;
    /* 保留原有一行（判读历史日志时口径不变），全表每 30s 打一次以免刷屏。 */
    ESP_LOGI(TAG, "[mem] taskLVGL stack hwm=%u B | tasks=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)uxTaskGetNumberOfTasks());
    if ((++tick % 3u) == 0u) {
        stack_report_dump();
    }
}

void app_main(void)
{
    /* ★ P0-1 分步内存账单（docs/开发文档/内存优化可执行方案.md §4-P0-1）。
     * 每行给 "want | INT free | INT largest | DMA free | PSRAM free"；
     * 相邻两行相减 = 该步净消耗。★ 判够不够只看 largest，不看 free。
     * 账单确认后本段（所有 MEM_STAGE）可整段删除。 */
    MEM_STAGE(TAG, "boot");

    ESP_ERROR_CHECK(fs_service_mount());
    MEM_STAGE(TAG, "after fs_service_mount");

    /* ★ §50 全 CJK bin 字体（AI 对话页中文）：阶段 1 读文件进 PSRAM。
     *   F40 约束：~2.7MB 的 flash read 按 16KB 分块、每块关一次全局 cache，
     *   窗口内任何核的 PSRAM 访问都会 Cache error —— 必须在后续任务创建
     *   （LVGL/服务任务）之前做，此刻 app_main 串行、零并发，天然安全。 */
    ui_binfont_load();
    MEM_STAGE(TAG, "after ui_binfont_load");

    /* ★ SD 卡（可选介质）：**故意不用 ESP_ERROR_CHECK** —— 没插卡时只打一条 WARN，
     *   其余功能必须照常（方案 §9 阶段 S1 的验收判据之一）。
     *   位置刻意在 bsp_display_start() 之前：此刻内部 RAM 最空、PSRAM 尚未被切碎，
     *   32KB DMA bounce buffer 与 FATFS 工作区最容易分配成功（与 main 分支同序）。 */
    sdcard_service_mount();
    MEM_STAGE(TAG, "after sdcard_service_mount");

    /* ★ S8-1：人脸库（可选介质）—— **故意不用 ESP_ERROR_CHECK**：
     *   没插卡时只降级（is_ready()==false），人脸检测/预览/录像等其它功能必须照常，
     *   这是 face_db.h 文件头第 3 条红线（也对应 S8-1 的一条验收项）。
     *
     *   位置：紧接 sdcard_service_mount() 之后。
     *   · 库根目录 /sdcard/face_db 必须建在**已挂载的挂载点**下（face_db_init 会先
     *     stat 挂载点，不存在就直接降级，绝不尝试创建 "/sdcard" —— 那会在内部
     *     flash 上建出一个同名目录，后患无穷）；
     *   · 这一步只读约 600 B 索引 + 两个 mkdir，放在 UI 起来之前做，用户看不到抖动。
     *
     *   ★ 为什么不留给 UI 懒加载：库是**系统状态**（同 wifi/nvs），启动就该就位；
     *     而且 tab_camera 的 ENROLL 态每 50 ms 要问一次"库能不能用"，懒加载会让
     *     那条只读路径挂上"第一次可能触发初始化"的副作用。 */
    face_db_init();
    MEM_STAGE(TAG, "after face_db_init");

    /* ★ S8-2b：打卡服务的**状态复位**（幂等、零分配 —— 帧缓冲/JPEG 输出缓冲都在
     *   第一次真正打卡时才分配，见 punch_service.h）。
     *   为什么要在开机时调一次：冷却表（s_cool_us）与"最近一次结果"快照都是模块
     *   静态量，开机清零保证"刚上电不会因为残留状态拒绝第一次打卡"。
     *   ★ 识别引擎（face_recog_init）**不在这里加载**：MFN 是 1.3 MB 的模型，
     *     必须由 CAM 页按需触发（tab_camera 的 recog_ensure_init），否则没插卡/
     *     不进人脸功能的用户白白付一份加载时间与内存。 */
    punch_service_init();
    MEM_STAGE(TAG, "after punch_service_init");

    bsp_i2c_init();
    MEM_STAGE(TAG, "after bsp_i2c_init");

    /* ★ P1-4 + P2-4（2026-09-25 按实测收敛）：taskLVGL 任务栈 7168 -> 16384 -> **12288**。
     * 7168→16384 是修 F37（栈溢出，见 11_遇到的问题.md）；
     * 16384→12288 是 P2-4 "按水位缩栈"：含相机页在 LVGL 线程里跑
     * tab_camera::refresh() 逐像素翻转 + face_service_detect() 的 esp-dl 推理，
     * 峰值用量 6,576 B ⇒ 12288 留 5,712 B 余量（峰值的 1.87 倍）。
     * （原文这里还写了"实测 hwm≈9808 B"，与同句的峰值 6,576 B 自相矛盾
     *   —— 12288 减 9808 只有 2,480 B。那个数是相机页还没跑过时的快照，
     *   已删除，以 `[stack]` 报表为准。）
     * 方案 §4-P2-4 的公式（used×1.5 ⇒ 10240）更激进，这里**故意留一档**：
     * F37 的教训就是栈溢出不会提前告警。省 4,096 B 内部 RAM。
     * 回退条件：**以 `[stack]` 报表为准**（见 stack_report_dump 上方的"判读口径"一节），
     * 不要在这里另立阈值 —— 这条注释原来写的"hwm 掉到 4 KB 以下 ⇒ 回 16384"
     * 已于 2026-09-28 作废，原因见那一节。
     *
     * bsp_display_start() 内部就是组装下面这份默认 cfg（见 bsp_display.c:274-291），
     * 这里照抄后只覆盖 task_stack，其余行为不变。 */
    /* ★ P2-4 + 2026-09-26 二次水位收缩：taskLVGL 7168 -> 16384 -> 12288 -> **9728**。
     * 本次依据：FILES/VIDEO/相机全路径跑过后 hwm=5892（峰值 6,396 B），
     * need=6396×1.5=9728；与 P2-4 时代的峰值 6,576 B 基本一致（路径已覆盖）。
     * 回退条件：**同上，以 `[stack]` 报表为准**（原写的"hwm 掉到 4 KB 以下 ⇒ 回 12288"
     * 已作废：那句在 size=12288 时等价于"峰值别超 8192"，搬到 9728 后等价于"别超 5632"，
     * 而实测峰值 6576 ⇒ 照字面永远该回退，与本次缩栈的决定自相矛盾）。
     * 2026-09-28 实测：size=9728 / hwm=3152 / used=6576 ⇒ need=9864，
     * 比 9728 高 512 B（按 ×1.5 口径略欠），**但不会打 `!! 偏紧`**（阈值 size/8=1216），
     * 余量 32% ⇒ 按上面"判读口径"的机器判据**不需要动**。
     * 若想把 ×1.5 那点安全系数补足：把 taskLVGL 改成 10240（多花 512 B 内部 RAM）。
     * ⚠ S8-2 提醒：MFN 推理（实测 324 ms）**绝不能放进这条线程** —— 它既会卡掉 6.5 帧预览，
     *   也会让这个栈数再跳一截（S8-0 探针就是为它在 Core0 单开了 10240 栈）。
     *   ⇒ 识别必须由 cam_task 侧的 poll 驱动，与预览帧率解耦。 */
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
    /* 不能写进上面的指定初始化器 —— 整体初始化 .lvgl_port_cfg 后再覆盖子成员会触发
     * -Woverride-init。 */
    dsp_cfg.lvgl_port_cfg.task_stack = 9728;
    bsp_display_start_with_config(&dsp_cfg);
    MEM_STAGE(TAG, "after bsp_display_start");

    /* ★ §50 阶段 2：lv_init() 已在 bsp_display_start 内完成，纯 RAM 解析
     *   成 LVGL 运行时字体（dynamic_glyph_load=ON，位图按需现读）。失败时
     *   ui_binfont_get() 返 NULL，tab_ai 自动回退 cn_20。 */
    ui_binfont_attach();
    MEM_STAGE(TAG, "after ui_binfont_attach");

    bsp_display_lock(0);
    lv_timer_create(stack_hwm_cb, 10000, NULL);
    bsp_display_unlock();

    //bsp_display_brightness_set(50);
    fs_service_init();
    MEM_STAGE(TAG, "after fs_service_init");
    audio_service_init();
    MEM_STAGE(TAG, "after audio_service_init");
    rec_local_service_init();   /* ★ §40 本地录音（ext EXCLUSIVE mic 租借） */
    MEM_STAGE(TAG, "after rec_local_service_init");

    led_service_init();
    MEM_STAGE(TAG, "after led_service_init");
    button_service_init();
    MEM_STAGE(TAG, "after button_service_init");
    /* ★ 顺序有讲究：先注册 wifi 事件回调，再 wifi_manager_init()（后者内部会
     * esp_wifi_start() + auto_connect）。反过来写时，开机自动连已保存路由的
     * GOT_IP 会在 wifi_manager_init() 返回前就派发完，而那时 sys_cb 还是 NULL，
     * 通知被丢掉 ⇒ 校时/天气任务永不启动。 */
    wifi_manager_set_sys_cb(on_wifi_evt);
    MEM_STAGE(TAG, "before wifi_manager_init");
    wifi_manager_init();
    MEM_STAGE(TAG, "after wifi_manager_init");
    camera_service_init();
    MEM_STAGE(TAG, "after camera_service_init");
    /* ★★ 2026-09-28：取景器缓冲**不再开机预分配**，改为按需
     *   （viewfinder_acquire / viewfinder_release，由 VIDEO 页的 PREVIEW 开关
     *     与 FILES 页的预览/播放驱动）。
     *
     *   历史（2026-09-25 起）：这里曾**无条件**调 viewfinder_init() 拿齐 ~2.93 MB
     *   （出图 1,843,200 + staging 921,600 + 显示双缓冲 230,400 B），理由是
     *   "PSRAM 一旦被切碎，1.8 MB 这种大块运行中再也拿不回来"。
     *
     *   2026-09-28 用户拍板改为按需：这笔内存是本工程**最大的可让出块**，
     *   让出来正好够跑 dl 模型（MFN 加载峰值 2,115,950 B）。
     *   ⇒ 开机账单里这一步现在 diff 为 0；真正的申请/归还发生在
     *     **进页面/离开页面**那一刻，日志形如：
     *       `VFD: vfmem acquire` / `VFD: vfmem released`
     *     （各带一行 PSRAM free/largest 现场快照，见 mem_diag_snap）。
     *
     *   ★ 保留一行占位（diff=0）是为了让新旧日志能逐行对照、不至于以为"漏了一步"。 */
    MEM_STAGE(TAG, "after viewfinder_init (on-demand now, no alloc)");
    /* ★ S4 文件管理服务：预览/播放双显示缓冲（2x460.8 KB PSRAM）+ files 任务
     *   （4.6 KB 内部栈，做文件 IO 的任务栈必须内部 RAM，F35）。
     *   解码不在这里配 —— 复用 viewfinder 的单例引擎与 staging（S4 方案 §4.2）。
     *   ★ 失败不中断开机：FILES 页所有操作会在守卫处被拒（状态行报错）。 */
    file_mgr_init();
    MEM_STAGE(TAG, "after file_mgr_init");
    /* ★ S5 M2/M3：MP3 播放服务（esp_player 链路惰性建，首次 PLAY 才占内存）+
     *   小智对话服务（只建任务/队列；音频管线与网络会话在对话期才建立）。
     *   两者都不在此占 codec —— 音频出声一律经 audio_service ext 协议仲裁。 */
    media_player_service_init();
    MEM_STAGE(TAG, "after media_player_service_init");
    xiaozhi_service_init();
    MEM_STAGE(TAG, "after xiaozhi_service_init");
    /* ★ S2a/S2b：拍照与录像服务。两者都**不建常驻任务**
     *   （拍照/录像时才建一次性任务，收尾完即删），所以这里不占内部 RAM：
     *   - capture_service_init 只是把状态归零；
     *   - record_video_service_init 装上 PSRAM 版 idx 批缓冲分配器
     *     （必须在任何 avi_muxer_open 之前执行，故放这里而不是懒加载）。 */
    capture_service_init();
    record_video_service_init();
    MEM_STAGE(TAG, "after capture/record_video service init");
    /* 默认上电 STA 模式（自动连已保存路由）；UI 可切 AP 开热点 */

    /* UI framework */
    PageManager_Init();
    MEM_STAGE(TAG, "after PageManager_Init");
    page_splash_set_enter_cb(on_splash_enter);
    PageManager_Load(&Page_Splash);     /* stack root */
    MEM_STAGE(TAG, "after splash load");

    ESP_LOGI(TAG, "boot done, waiting swipe up");

#if PANEL_PROBE_ENABLE
    /* ★ 放在最后启动：不污染上面的开机分步内存账单（探针自己吃 16 KB 内部 RAM）。 */
    MEM_STAGE(TAG, "panelprobe: pre");
    panel_probe_start();
    MEM_STAGE(TAG, "panelprobe: post");
#endif

#if FACE_RECOG_PROBE_ENABLE
    /* ★ S8-0：MFN 可行性自检。一次性任务，延时 RECOG_PROBE_DELAY_MS 后跑一次即自删。
     *   ★ 钉在 **Core0** —— Core1 是 LVGL 与相机任务的地盘，自检绝不能卡 UI。
     *   ★ 创建任务本身只花几 KB 栈、不分配 PSRAM ⇒ 不影响上面那份开机内存账单；
     *     真正的 1.29 MB 连续 PSRAM 需求发生在 3 s 之后（那时各服务已就位）。 */
    BaseType_t pr = xTaskCreatePinnedToCore(face_recog_probe_task, "recog_probe",
                                            RECOG_PROBE_STACK, NULL, 4, NULL, 0);
    if (pr != pdPASS) {
        ESP_LOGE(TAG, "recog_probe task create failed");
    } else {
        ESP_LOGW(TAG, "[S8-0] MFN feasibility probe armed, fires in %d ms "
                      "(set FACE_RECOG_PROBE_ENABLE 0 after verification)",
                 RECOG_PROBE_DELAY_MS);
    }
#endif

#if FACE_DB_PROBE_ENABLE
    /* ★ S8-1：人脸库存储层自检。同样钉 Core0、一次性、延时错开。
     *   它**不加载任何模型**，只写 /sdcard/face_db_selftest 这个临时根目录，
     *   跑完自行清理 —— 所以与 S8-0 探针互不干扰，可以同时开。 */
    BaseType_t dr = xTaskCreatePinnedToCore(face_db_probe_task, "fdb_probe",
                                            FDB_PROBE_STACK, NULL, 4, NULL, 0);
    if (dr != pdPASS) {
        ESP_LOGE(TAG, "fdb_probe task create failed");
    } else {
        ESP_LOGW(TAG, "[S8-1] face_db self test armed, fires in %d ms "
                      "(set FACE_DB_PROBE_ENABLE 0 after verification)",
                 FDB_PROBE_DELAY_MS);
    }
#endif
}
