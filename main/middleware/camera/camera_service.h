/* camera_service.h - OV3660 preview service wrapper */
#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*camera_frame_cb_t)(const uint8_t *rgb565, uint32_t width, uint32_t height, void *user);

esp_err_t camera_service_init(void);
esp_err_t camera_service_start(camera_frame_cb_t cb, void *user);
esp_err_t camera_service_stop(void);
bool      camera_service_is_running(void);

#ifdef __cplusplus
}
#endif
