/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdcard_service.c
 * @brief 中间层：SD 卡挂载（SDMMC 4-bit → FATFS /sdcard）
 *
 * ---------------------------------------------------------------------------
 * ★★★ 动这个文件前必须先读这段（2026-09-25 实施时踩过，差点搞坏人脸检测）
 *
 * 【1】`CONFIG_BSP_SD_MOUNT_POINT` 在本工程**必须保持 "/littlefs"**，绝不要"修正"成 /sdcard！
 *
 *     它不是笔误，而是被**故意复用**成"文件系统根"的开关：
 *
 *       sdkconfig:4237  CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD=y
 *       sdkconfig:4240  CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR="models"
 *       sdkconfig:4008  CONFIG_BSP_SD_MOUNT_POINT="/littlefs"          ← 就是这里
 *
 *     于是：
 *       · 组件侧 `human_face_detect.cpp:23`：
 *             path(CONFIG_BSP_SD_MOUNT_POINT) / CONFIG_..._SDCARD_DIR / model_name
 *         = "/littlefs" / "models" / "xxx.espdl"
 *         （组件在 `human_face_detect.cpp:10-13` 有一句
 *           `#if !defined(CONFIG_BSP_SD_MOUNT_POINT) #define ... "/sdcard" #endif`。
 *           注意它是 **`!defined` 保护的** —— 因为 sdkconfig 里**定义了**它，
 *           所以那句重定义**根本不生效**，组件用的是 "/littlefs"。）
 *       · 本工程侧 `face_service.cpp:53`：
 *             #define FACE_MODEL_DIR (CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_..._SDCARD_DIR)
 *         = "/littlefs/models"
 *
 *     而 `littlefs_content/models/` 正好有那 4 份 `.espdl`（合计 1,186,432 B），
 *     经 `littlefs_create_partition_image(storage littlefs_content)` 编进 storage 分区、
 *     挂载在 `/littlefs` ⇒ **模型路径与文件位置严丝合缝**。
 *
 *     ⇒ 若把 BSP_SD_MOUNT_POINT 改成 "/sdcard"：模型路径变成 `/sdcard/models/xxx.espdl`
 *       而文件仍在 littlefs ⇒ **三档人脸模型全部加载失败，FACE 功能彻底失效**。
 *
 * 【2】正因为 [1]，**不能**用 BSP 的 `bsp_sdcard_sdmmc_mount()` —— 它内部写死
 *     `esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, ...)`（`bsp_storage.c:141`），
 *     会试图把 FATFS 挂到 `/littlefs`，与本工程的 LittleFS **同点注册、必然失败**。
 *     ⇒ 这里只能自己调 `esp_vfs_fat_sdmmc_mount(SDCARD_MNT_PATH, ...)`（SDCARD_MNT_PATH = "/sdcard"），
 *       并**手动补上** BSP 封装里那一步 SD 电源使能（见 [3]）。
 *
 * 【3】main 分支 `middleware/system/sdcard_service.c` 的 API 相同，但有两处隐患，勿回退：
 *
 *     ① **漏了 SD 卡上电**。BSP 的挂载路径里有这一步（`bsp_storage.c:139`）：
 *            BSP_ERROR_CHECK_RETURN_ERR(bsp_feature_enable(BSP_FEATURE_SD, true));
 *        它做的是 `gpio_set_level(BSP_SD_EN(GPIO39), !enable)` —— 低有效。
 *        （`bsp_feature_en.c:19-29`）—— 即 SD 的电源使能。
 *        main 版没调它，能挂上大概率是"依赖 GPIO39 上电默认低"的侥幸。
 *        ⇒ 本文件显式调用（见 mount 内 ★ 标注）。
 *
 *     ② 32KB 常驻 DMA bounce buffer **必须保留**（那是 F23 的修复）：
 *        SDMMC 驱动在"目标缓冲未 DMA 对齐"的读路径上会**现场** malloc 一块 DMA 内存
 *        （IDF components/sdmmc/sdmmc_cmd.c 的 allocate_dma_buf）。开机一段时间后
 *        内部 DMA 堆被 BT/LCD/WiFi/音频解码器切碎 ⇒ 连一个扇区都凑不出 ⇒
 *            E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
 *            E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
 *        解法（IDF 官方 sdmmc 测试 Test D 的做法）：开机预分配一块常驻 DMA 对齐缓冲挂到
 *        `host.dma_aligned_buffer`，驱动直接复用、不再每次现场分配。
 *        本板 `SOC_SDMMC_PSRAM_DMA_CAPABLE=y` ⇒ 优先放 PSRAM，**不占内部 RAM**。
 * ---------------------------------------------------------------------------
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "bsp/esp-bsp.h"

#include "sdcard_service.h"

static const char *TAG = "SDCARD";

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

/* 挂载用 host/slot 放 static：生命周期覆盖整个运行期，杜绝"栈变量逃逸"的疑虑。 */
static sdmmc_host_t        s_host;
static sdmmc_slot_config_t s_slot;

/* ---- 常驻 DMA bounce buffer（见文件头 [3]②） ---- */
#define SD_DMA_BOUNCE_SIZE   (32 * 1024)
static void *s_dma_bounce = NULL;

static void sdcard_setup_dma_buffer(sdmmc_host_t *host)
{
    if (!s_dma_bounce) {
        /* 优先 PSRAM：本板 PSRAM 可被 SDMMC DMA 直访，等于零内部 RAM 开销 */
        s_dma_bounce = heap_caps_malloc(SD_DMA_BOUNCE_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_dma_bounce) {
            ESP_LOGW(TAG, "PSRAM DMA buffer unavailable, falling back to internal DMA");
            s_dma_bounce = heap_caps_malloc(SD_DMA_BOUNCE_SIZE, MALLOC_CAP_DMA);
        }
    }

    if (!s_dma_bounce) {
        /* 不致命：驱动会退化成"每次现场分配"，负载轻时通常也能跑 */
        ESP_LOGW(TAG, "no DMA bounce buffer; reads may fail under memory pressure");
        return;
    }

    host->dma_aligned_buffer = s_dma_bounce;
    ESP_LOGI(TAG, "DMA bounce buffer %uKB @ %p (INT free=%u largest=%u, PSRAM free=%u)",
             (unsigned)(SD_DMA_BOUNCE_SIZE / 1024), s_dma_bounce,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

esp_err_t sdcard_service_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    /* ① host：用 BSP 的默认值（SDMMC_HOST_DEFAULT），再覆盖一项 */
    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &s_host);

    /* BSP 只给 SDMMC_FREQ_DEFAULT(=20MHz)。录像要 ~1 MB/s、照片是百 KB 级，
     * 提到 HIGHSPEED 让 4-bit 总线跑满，把余量留给"边录边写"。 */
    s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdcard_setup_dma_buffer(&s_host);

    /* ② slot：BSP 已含 width=4 + 全部引脚（cmd/clk/d0-d3），不必自己填 */
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &s_slot);

    /* ③ ★ main 版漏掉的关键一步：SD 卡**电源使能**（GPIO39，低有效）。
     *    不用 bsp_sdcard_sdmmc_mount() 就必须自己补 —— 理由见文件头 [2]。
     *    失败不致命（继续尝试挂载；有些板子上电默认已使能）。 */
    esp_err_t feat = bsp_feature_enable(BSP_FEATURE_SD, true);
    if (feat != ESP_OK) {
        ESP_LOGW(TAG, "BSP_FEATURE_SD enable failed: %s", esp_err_to_name(feat));
    }

    /* ④ 挂载参数：★ 绝不 format —— SD 卡上是用户数据 */
    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files              = 8,      /* 同屏可能同时开：照片 + 录像 + 人脸库索引 */
        .allocation_unit_size   = 16 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD card to %s ...", SDCARD_MNT_PATH);

    /* ★ 挂载点用 SDCARD_MNT_PATH("/sdcard")，**不是** BSP_SD_MOUNT_POINT("/littlefs")。
     *   后者已被"模型路径"占用（文件头 [1]），不可挪作他用。 */
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MNT_PATH, &s_host, &s_slot, &mnt, &s_card);
    if (ret != ESP_OK) {
        /* 没插卡是最常见情况 —— 只告警，绝不让它阻塞启动 */
        ESP_LOGW(TAG, "SD mount failed: %s (no card?)", esp_err_to_name(ret));
        if (s_dma_bounce) {
            /* 失败就把 32KB 还回去，避免"没插卡也白占 32KB PSRAM" */
            heap_caps_free(s_dma_bounce);
            s_dma_bounce = NULL;
        }
        s_card = NULL;
        return ret;
    }

    if (s_card) {
        sdmmc_card_print_info(stdout, s_card);
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted OK at %s", SDCARD_MNT_PATH);
    return ESP_OK;
}

void sdcard_service_unmount(void)
{
    if (!s_mounted || !s_card) {
        return;   /* 未挂载（没插卡）：无事可做 */
    }
    /* ★ §73.20 有序软重启的核心一步：
     *  - f_unmount：把 FAT 脏缓冲全部回写、FATFS 关卷 —— 否则 esp_restart
     *    落在写盘事务中间 = 文件系统脏（轻则下次挂载慢/丢最后一条记录，
     *    重则 index.csv / face_db 索引半行）；
     *  - 卡回 Standby 态 —— 软重启后 CMD0 重新枚举不再撞"卡还在 busy"。
     * 失败不拦（马上就要重启了，尽力而为）；host 由 unmount 内部收尾。 */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_MNT_PATH, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed: %s (continuing, device is rebooting)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD card unmounted (flush OK)");
    }
    s_mounted = false;
    s_card = NULL;
}

bool sdcard_service_is_mounted(void)
{
    return s_mounted;
}
