/* face_service.cpp - esp-dl face detection wrapper, C ABI */
#include "face_service.h"
#include "human_face_detect.hpp"
#include "dl_image_define.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "FACE";

static HumanFaceDetect *s_detector = nullptr;
static SemaphoreHandle_t s_lock;
static bool s_running = false;
static uint8_t *s_scratch; /* RGB888 workspace, esp-dl wants RGB888 */

esp_err_t face_service_start(void)
{
    if (s_running) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "loading model...");
    int64_t t0 = esp_timer_get_time();
    /* ESPDet pico 224x224: small, fast, fits our 240x240 frame */
    s_detector = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, false);
    if (!s_detector) {
        ESP_LOGE(TAG, "detector alloc fail");
        return ESP_FAIL;
    }
    /* RGB888 scratch buffer 240*240*3 = 172KB */
    s_scratch = (uint8_t *)heap_caps_aligned_alloc(64, 240 * 240 * 3,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_scratch) {
        ESP_LOGE(TAG, "scratch alloc fail");
        delete s_detector; s_detector = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "model loaded in %lld ms, scratch=%p", (long long)((esp_timer_get_time()-t0)/1000), s_scratch);
    return ESP_OK;
}

esp_err_t face_service_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_detector) { delete s_detector; s_detector = nullptr; }
    if (s_scratch) { heap_caps_free(s_scratch); s_scratch = nullptr; }
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        face_box_t *out, int max)
{
    if (!s_running || !s_detector || !rgb565le || !out || max <= 0) return 0;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return 0;

    /* Convert RGB565LE -> RGB888 into scratch. esp-dl espdet wants RGB888. */
    uint8_t *dst = s_scratch;
    for (int i = 0; i < w * h; i++) {
        uint16_t p = rgb565le[i];
        dst[0] = ((p >> 11) & 0x1F) << 3;
        dst[1] = ((p >> 5) & 0x3F) << 2;
        dst[2] = (p & 0x1F) << 3;
        dst += 3;
    }

    dl::image::img_t img = {
        .data = s_scratch,
        .width = w,
        .height = h,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };

    int64_t t0 = esp_timer_get_time();
    auto &res = s_detector->run(img);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);

    int n = 0;
    for (auto &r : res) {
        if (n >= max) break;
        if (r.box.size() < 4) continue;
        out[n].x = r.box[0];
        out[n].y = r.box[1];
        out[n].w = r.box[2] - r.box[0];
        out[n].h = r.box[3] - r.box[1];
        out[n].score = r.score;
        n++;
    }
    ESP_LOGI(TAG, "detect %d faces in %d ms", n, ms);
    xSemaphoreGive(s_lock);
    return n;
}

bool face_service_is_running(void) { return s_running; }
