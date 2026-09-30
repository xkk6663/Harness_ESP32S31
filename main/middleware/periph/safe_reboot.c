/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file safe_reboot.c
 * @brief 有序软重启实现（§73.20，见头文件）。
 *
 * ★ 各步骤全部"尽力而为 + 不拦截"：任何一步失败都只是少收尾一项，
 *   绝不能让收尾本身变成新的卡死点（那比不收尾更糟）。
 */

#include <stdbool.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "bsp/esp32_s31_korvo_1.h"

#include "safe_reboot.h"
#include "middleware/camera/camera_service.h"
#include "middleware/periph/sdcard_service.h"

static const char *TAG = "SAFE_RB";

void safe_reboot_perform(const char *reason)
{
    ESP_LOGW(TAG, "safe reboot: %s", reason ? reason : "requested");

    /* ① 相机：停 DVP DMA（未在跑则无害返回）。camera_service_stop 幂等；
     *    在跑时同步等流退干净（内部有超时），不会永久阻塞。 */
    if (camera_service_is_running()) {
        esp_err_t err = camera_service_stop();
        ESP_LOGW(TAG, "camera stop: %s", esp_err_to_name(err));
    }

    /* ② SD：FATFS flush + 卸载（未挂载/没插卡则跳过）。
     *    这是裸 esp_restart 的最大确定性风险（写盘中断 = 卷脏）。 */
    sdcard_service_unmount();

    /* ③ Wi-Fi：射频干净下电（未 init/start 时返回错误码，忽略）。
     *    不做 esp_wifi_deinit——重启场景没必要，stop 足够。 */
    esp_err_t werr = esp_wifi_stop();
    ESP_LOGW(TAG, "wifi stop: %s", esp_err_to_name(werr));

    /* ④ RGB 屏待机：本板无 LCD_RST/背光 GPIO（都是 NC），DISP(GPIO38) 是
     *    唯一抓手。esp_lcd rgb 驱动的 off 极性 = 拉到 !disp_en_level（默认
     *    en_level=0 ⇒ 熄屏拉高）。让屏 IC 在 PCLK 消失前先进 standby，
     *    尽量贴近冷启动时序。（不保证对一切 RGB 屏 IC 有效，但无害。） */
    gpio_config_t io = {
        .pin_bit_mask = BIT64(BSP_LCD_RGB_DISP),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BSP_LCD_RGB_DISP, 1);
    ESP_LOGW(TAG, "LCD DISP -> standby");

    /* ⑤ 给日志/UART 发送一点时间，然后重启（本函数不返回） */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "rebooting now");
    esp_restart();
}
