/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sdcard_service.c
 * @brief SD 卡挂载服务（SDIO 4-bit，挂载到 /sdcard）
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

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

/* SDMMC 驱动在「目标缓冲区未 DMA 对齐」的读路径上，会现场
 * heap_caps_malloc(..., MALLOC_CAP_DMA) 一块 bounce buffer（见 IDF
 * components/sdmmc/sdmmc_cmd.c:allocate_dma_buf）。内部 DMA 堆在
 * BT/LCD/WiFi/音频解码器都起来之后会被切得很碎，一旦连一个扇区都凑不出，
 * 就报 "allocate_dma_buf: not enough mem, err=0x101" → sdmmc_read_blocks
 * failed → 上层 GMF 管线报 I/O error。
 *
 * 解法（IDF 官方 sdmmc 测试 Test D 的做法）：开机时预分配一块常驻的
 * DMA 对齐缓冲挂到 host.dma_aligned_buffer 上，驱动直接复用，不再每次现场分配。
 * 本板 CONFIG_SOC_SDMMC_PSRAM_DMA_CAPABLE=y，PSRAM 可被 SDMMC DMA 直访，
 * 所以这块缓冲放 PSRAM，不占一字节内部 RAM。
 *
 * 大小 = 扇区 512B × chunk(16 块) = 8KB；放大到 32KB 覆盖多块连续读。 */
#define SD_DMA_BOUNCE_SIZE   (32 * 1024)
static void *s_dma_bounce = NULL;

/* 挂一块常驻 DMA bounce buffer 到 host 上；失败只打日志不致命 */
static void sdcard_setup_dma_buffer(sdmmc_host_t *host)
{
    if (s_dma_bounce) {
        host->dma_aligned_buffer = s_dma_bounce;
        return;
    }

    /* 优先 PSRAM（本板支持 PSRAM-DMA 直访），退回内部 DMA 内存 */
    s_dma_bounce = heap_caps_malloc(SD_DMA_BOUNCE_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_dma_bounce) {
        s_dma_bounce = heap_caps_malloc(SD_DMA_BOUNCE_SIZE, MALLOC_CAP_DMA);
    }
    if (!s_dma_bounce) {
        ESP_LOGW(TAG, "no DMA bounce buffer; reads may fail under memory pressure");
        return;
    }

    host->dma_aligned_buffer = s_dma_bounce;
    ESP_LOGI(TAG, "DMA bounce buffer %uKB @ %p (internal free=%u, PSRAM free=%u)",
             (unsigned)(SD_DMA_BOUNCE_SIZE / 1024), s_dma_bounce,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

esp_err_t sdcard_service_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdcard_setup_dma_buffer(&host);

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD card to /sdcard ...");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s (no card?)", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted OK");
    return ESP_OK;
}

bool sdcard_service_is_mounted(void)
{
    return s_mounted;
}
