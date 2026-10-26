#include "camera_capture.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "camera_capture";

/*
 * 当前 ESP32-S3-CAM 板载 Camera GPIO。
 * 这组参数已经在 s3cam/camera_test 中通过 30 帧连续抓拍测试。
 */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D0     11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

static SemaphoreHandle_t s_photo_mutex = NULL;
static uint8_t *s_latest_jpeg = NULL;
static size_t s_latest_jpeg_size = 0;
static uint16_t s_latest_width = 0;
static uint16_t s_latest_height = 0;
static uint32_t s_sequence = 0;
static bool s_initialized = false;

static bool jpeg_has_valid_markers(const camera_fb_t *fb)
{
    if (fb == NULL || fb->buf == NULL || fb->len < 4) {
        return false;
    }

    /* JPEG SOI = FF D8；EOI = FF D9。 */
    return fb->buf[0] == 0xFF
           && fb->buf[1] == 0xD8
           && fb->buf[fb->len - 2] == 0xFF
           && fb->buf[fb->len - 1] == 0xD9;
}

static uint8_t *allocate_jpeg_buffer(size_t size)
{
    /* 优先放 PSRAM，避免占用宝贵的内部 DRAM；没有 PSRAM 时再回退普通内存。 */
    uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = malloc(size);
    }
    return buffer;
}

esp_err_t camera_capture_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (s_photo_mutex == NULL) {
        s_photo_mutex = xSemaphoreCreateMutex();
        if (s_photo_mutex == NULL) {
            ESP_LOGE(TAG, "Create photo mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        ESP_LOGI(TAG,
                 "Camera ready: PID=0x%04x VER=0x%02x, QVGA JPEG quality=%d",
                 sensor->id.PID,
                 sensor->id.VER,
                 12);
    } else {
        ESP_LOGW(TAG, "Camera initialized but sensor descriptor is NULL");
    }

    /*
     * 某些 Camera 上电后的第一帧可能不稳定。
     * 这里预热抓取一帧并立即归还，正式“拍照”从下一帧开始。
     */
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup != NULL) {
        ESP_LOGI(TAG,
                 "Camera warm-up frame: %ux%u, %u bytes",
                 (unsigned)warmup->width,
                 (unsigned)warmup->height,
                 (unsigned)warmup->len);
        esp_camera_fb_return(warmup);
    } else {
        ESP_LOGW(TAG, "Camera warm-up frame failed; continue anyway");
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t camera_capture_one(camera_photo_info_t *info)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "esp_camera_fb_get returned NULL");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    uint8_t *new_jpeg = NULL;

    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "Unexpected frame format=%d, require JPEG", (int)fb->format);
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }

    if (!jpeg_has_valid_markers(fb)) {
        ESP_LOGE(TAG,
                 "Invalid JPEG markers: %ux%u, len=%u",
                 (unsigned)fb->width,
                 (unsigned)fb->height,
                 (unsigned)fb->len);
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }

    new_jpeg = allocate_jpeg_buffer(fb->len);
    if (new_jpeg == NULL) {
        ESP_LOGE(TAG, "No memory for JPEG copy: %u bytes", (unsigned)fb->len);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    memcpy(new_jpeg, fb->buf, fb->len);

    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    uint8_t *old_jpeg = s_latest_jpeg;
    s_latest_jpeg = new_jpeg;
    s_latest_jpeg_size = fb->len;
    s_latest_width = (uint16_t)fb->width;
    s_latest_height = (uint16_t)fb->height;
    ++s_sequence;

    if (info != NULL) {
        info->sequence = s_sequence;
        info->width = s_latest_width;
        info->height = s_latest_height;
        info->jpeg_size = s_latest_jpeg_size;
    }

    xSemaphoreGive(s_photo_mutex);

    free(old_jpeg);
    new_jpeg = NULL;

    ESP_LOGI(TAG,
             "PHOTO CAPTURED: #%lu %ux%u JPEG %u bytes",
             (unsigned long)s_sequence,
             (unsigned)s_latest_width,
             (unsigned)s_latest_height,
             (unsigned)s_latest_jpeg_size);

out:
    esp_camera_fb_return(fb);
    free(new_jpeg);
    return err;
}

const uint8_t *camera_capture_latest(size_t *size,
                                     uint16_t *width,
                                     uint16_t *height,
                                     uint32_t *sequence)
{
    if (s_photo_mutex == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    const uint8_t *result = s_latest_jpeg;
    if (size != NULL) {
        *size = s_latest_jpeg_size;
    }
    if (width != NULL) {
        *width = s_latest_width;
    }
    if (height != NULL) {
        *height = s_latest_height;
    }
    if (sequence != NULL) {
        *sequence = s_sequence;
    }

    xSemaphoreGive(s_photo_mutex);
    return result;
}
