/* camera_service.c - OV3660 DVP preview via V4L2 */
#include "camera_service.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_video_device.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "linux/videodev2.h"

#define TAG "CAM"
#define CAM_W   240
#define CAM_H   240
#define BUF_CNT 2

static int  s_fd = -1;
static void *s_buf[BUF_CNT] = {0};
static bool s_running = false;
static bool s_bsp_started = false;
static TaskHandle_t s_task = NULL;
static camera_frame_cb_t s_cb = NULL;
static void *s_user = NULL;

static void cam_task(void *arg)
{
    struct v4l2_buffer buf;
    while (s_running) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF fail errno=%d", errno);
            break;
        }
        if (s_cb && (buf.flags & V4L2_BUF_FLAG_DONE)) {
            s_cb((const uint8_t *)s_buf[buf.index], CAM_W, CAM_H, s_user);
        }
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF fail");
            break;
        }
    }
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_service_init(void)
{
    /* Lazy: bsp_camera_start called on first camera_service_start() */
    ESP_LOGI(TAG, "lazy init (hardware starts on first START)");
    return ESP_OK;
}

esp_err_t camera_service_start(camera_frame_cb_t cb, void *user)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_cb = cb; s_user = user;

    if (!s_bsp_started) {
        ESP_LOGI(TAG, "bsp_camera_start...");
        bsp_camera_cfg_t cfg = {0};
        esp_err_t err = bsp_camera_start(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bsp_camera_start fail=%d", err);
            return err;
        }
        s_bsp_started = true;
    }

    s_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDONLY);
    if (s_fd < 0) { ESP_LOGE(TAG, "open fail errno=%d", errno); return ESP_FAIL; }

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = CAM_W,
        .fmt.pix.height = CAM_H,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
    };
    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) { ESP_LOGE(TAG, "S_FMT fail"); goto err; }
    ESP_LOGI(TAG, "fmt %ux%u", fmt.fmt.pix.width, fmt.fmt.pix.height);

    struct v4l2_requestbuffers req = {
        .count = BUF_CNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) { ESP_LOGE(TAG, "REQBUFS fail"); goto err; }

    for (int i = 0; i < BUF_CNT; i++) {
        struct v4l2_buffer b = {.type=V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory=V4L2_MEMORY_MMAP, .index=i};
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) goto err;
        s_buf[i] = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        if (s_buf[i] == MAP_FAILED) goto err;
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) goto err;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) { ESP_LOGE(TAG, "STREAMON fail"); goto err; }

    s_running = true;
    xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 5, &s_task, 1);
    ESP_LOGI(TAG, "stream on");
    return ESP_OK;
err:
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
    return ESP_FAIL;
}

esp_err_t camera_service_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    if (s_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_task = NULL;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < BUF_CNT; i++) {
        if (s_buf[i]) { munmap(s_buf[i], 0); s_buf[i] = NULL; }
    }
    close(s_fd); s_fd = -1;
    ESP_LOGI(TAG, "stream off");
    return ESP_OK;
}

bool camera_service_is_running(void) { return s_running; }
