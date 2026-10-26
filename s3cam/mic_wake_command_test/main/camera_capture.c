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
static uint32_t s_latest_fingerprint = 0;
static uint32_t s_sequence = 0;
static bool s_initialized = false;

/*
 * 沿用 camera_boot_http_test 已经验证过的 JPEG 判断方式：
 *
 *   format == PIXFORMAT_JPEG
 *   buf != NULL
 *   len >= 4
 *   开头为 JPEG SOI：FF D8
 */
static bool jpeg_is_valid(const camera_fb_t *fb)
{
    return fb != NULL
           && fb->format == PIXFORMAT_JPEG
           && fb->buf != NULL
           && fb->len >= 4
           && fb->buf[0] == 0xFF
           && fb->buf[1] == 0xD8;
}

/*
 * 对整张 JPEG 计算一个轻量内容指纹。
 *
 * 目的不是加密，而是让串口日志可以明确回答：
 * “Camera 抓到的这一帧、内存里保存的这一帧、HTTP 发出去的这一帧，
 *  到底是不是同一份 JPEG？”
 */
static uint32_t jpeg_fingerprint(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;  // FNV-1a offset basis

    if (data == NULL) {
        return 0;
    }

    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }

    return hash;
}

static uint8_t *allocate_jpeg_buffer(size_t size)
{
    /* 优先放 PSRAM，避免占用内部 DRAM；没有 PSRAM 时回退普通内存。 */
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

        /* OV3660 直接输出 JPEG，避免 ESP32 软件压缩。 */
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 1,

        /*
         * VGA JPEG framebuffer 必须放 PSRAM。
         *
         * 之前这里使用 CAMERA_FB_IN_DRAM。320x240 时还能勉强工作，切到
         * 640x480 后会吃掉大量内部 DRAM，导致 Wi-Fi 初始化时静态 RX buffer
         * 只能申请到 9/10，最终 esp_wifi_init() 返回 ESP_ERR_NO_MEM。
         *
         * 当前 sdkconfig 已启用 PSRAM，因此把 Camera framebuffer 搬到 PSRAM，
         * 把内部 DRAM 留给 ESP-SR、Wi-Fi、HTTP Server 和 FreeRTOS task。
         */
        .fb_location = CAMERA_FB_IN_PSRAM,
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
                 "Camera ready: PID=0x%04x VER=0x%02x, frame_size=VGA, JPEG quality=12, fb_count=1, fb=PSRAM, grab=WHEN_EMPTY",
                 sensor->id.PID,
                 sensor->id.VER);
    } else {
        ESP_LOGW(TAG, "Camera initialized but sensor descriptor is NULL");
    }

    ESP_LOGI(TAG,
             "Memory after Camera init: internal=%u bytes, largest_internal=%u bytes, psram=%u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    /*
     * 上电预热只负责丢掉初始化阶段可能不稳定的第一帧。
     * 注意：由于 grab_mode=CAMERA_GRAB_WHEN_EMPTY，驱动后续仍可能提前把一帧
     * 填进唯一 framebuffer。所以每次正式拍照还会再做一次“丢弃缓存帧”。
     */
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup != NULL) {
        uint32_t warmup_fp = jpeg_fingerprint(warmup->buf, warmup->len);
        ESP_LOGI(TAG,
                 "INIT warm-up discard: %ux%u, %u bytes, fp=%08lx",
                 (unsigned)warmup->width,
                 (unsigned)warmup->height,
                 (unsigned)warmup->len,
                 (unsigned long)warmup_fp);
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

    /*
     * 非常关键：先主动取走并丢弃当前 framebuffer 中可能已经缓存的旧帧。
     *
     * Espressif 对 CAMERA_GRAB_WHEN_EMPTY 的说明明确指出：
     * “first fb_count frames might be old”。当前 fb_count=1，因此如果直接把
     * 第一次 esp_camera_fb_get() 当成“本次拍照”，就可能出现整整慢一拍：
     * 第一次命令拿到旧帧，归还以后驱动才抓新帧；第二次命令才看到上一轮画面。
     *
     * 这里保持 fb_count=1（避免增加内存），用“丢一帧 + 正式抓一帧”解决。
     */
    ESP_LOGI(TAG, "CAPTURE STEP 1/2: flush possible buffered frame...");

    uint32_t discarded_fp = 0;
    camera_fb_t *discarded = esp_camera_fb_get();
    if (discarded != NULL) {
        discarded_fp = jpeg_fingerprint(discarded->buf, discarded->len);
        ESP_LOGI(TAG,
                 "CAPTURE STEP 1/2: discarded frame %ux%u, %u bytes, fp=%08lx, valid=%s",
                 (unsigned)discarded->width,
                 (unsigned)discarded->height,
                 (unsigned)discarded->len,
                 (unsigned long)discarded_fp,
                 jpeg_is_valid(discarded) ? "YES" : "NO");
        esp_camera_fb_return(discarded);
    } else {
        /* 丢弃帧失败不直接判本次拍照失败，继续尝试真正抓一帧。 */
        ESP_LOGW(TAG, "CAPTURE STEP 1/2: no buffered frame returned; continue fresh capture");
    }

    ESP_LOGI(TAG, "CAPTURE STEP 2/2: waiting for fresh frame...");

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "CAPTURE STEP 2/2 failed: esp_camera_fb_get returned NULL");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    uint8_t *new_jpeg = NULL;
    const uint32_t fresh_fp = jpeg_fingerprint(fb->buf, fb->len);

    ESP_LOGI(TAG,
             "CAPTURE STEP 2/2: fresh frame %ux%u, %u bytes, fp=%08lx, valid=%s",
             (unsigned)fb->width,
             (unsigned)fb->height,
             (unsigned)fb->len,
             (unsigned long)fresh_fp,
             jpeg_is_valid(fb) ? "YES" : "NO");

    if (discarded_fp != 0 && fresh_fp == discarded_fp) {
        ESP_LOGW(TAG,
                 "CAPTURE diagnostic: discarded and fresh fingerprint are identical (%08lx). "
                 "If the scene did not move this can be normal; if scene changed, please keep this log.",
                 (unsigned long)fresh_fp);
    }

    if (!jpeg_is_valid(fb)) {
        ESP_LOGE(TAG,
                 "Capture failed: invalid fresh JPEG, format=%d %ux%u len=%u fp=%08lx",
                 (int)fb->format,
                 (unsigned)fb->width,
                 (unsigned)fb->height,
                 (unsigned)fb->len,
                 (unsigned long)fresh_fp);
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }

    /*
     * fb->buf 属于 Camera 驱动，esp_camera_fb_return() 以后不能继续使用。
     * 因此复制一份，作为业务层真正的“最新照片”。
     */
    new_jpeg = allocate_jpeg_buffer(fb->len);
    if (new_jpeg == NULL) {
        ESP_LOGE(TAG, "Capture failed: no memory for JPEG copy (%u bytes)",
                 (unsigned)fb->len);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    memcpy(new_jpeg, fb->buf, fb->len);

    const size_t new_size = fb->len;
    const uint16_t new_width = (uint16_t)fb->width;
    const uint16_t new_height = (uint16_t)fb->height;

    esp_camera_fb_return(fb);
    fb = NULL;

    /*
     * 成功抓拍并成功复制后才替换上一张照片。
     * 如果任何步骤失败，上一张有效照片仍保留。
     */
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    uint8_t *old_jpeg = s_latest_jpeg;
    const uint32_t old_fp = s_latest_fingerprint;

    s_latest_jpeg = new_jpeg;
    s_latest_jpeg_size = new_size;
    s_latest_width = new_width;
    s_latest_height = new_height;
    s_latest_fingerprint = fresh_fp;
    ++s_sequence;

    if (info != NULL) {
        info->sequence = s_sequence;
        info->width = s_latest_width;
        info->height = s_latest_height;
        info->jpeg_size = s_latest_jpeg_size;
        info->fingerprint = s_latest_fingerprint;
    }

    const uint32_t sequence = s_sequence;
    xSemaphoreGive(s_photo_mutex);

    free(old_jpeg);
    new_jpeg = NULL;

    ESP_LOGI(TAG,
             "LATEST PHOTO REPLACED: #%lu %ux%u %u bytes fp=%08lx (previous fp=%08lx)",
             (unsigned long)sequence,
             (unsigned)new_width,
             (unsigned)new_height,
             (unsigned)new_size,
             (unsigned long)fresh_fp,
             (unsigned long)old_fp);

out:
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
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

esp_err_t camera_capture_copy_latest(uint8_t **jpeg_copy,
                                     camera_photo_info_t *info)
{
    if (jpeg_copy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *jpeg_copy = NULL;

    if (s_photo_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    if (s_latest_jpeg == NULL || s_latest_jpeg_size == 0) {
        xSemaphoreGive(s_photo_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *copy = allocate_jpeg_buffer(s_latest_jpeg_size);
    if (copy == NULL) {
        xSemaphoreGive(s_photo_mutex);
        return ESP_ERR_NO_MEM;
    }

    memcpy(copy, s_latest_jpeg, s_latest_jpeg_size);

    if (info != NULL) {
        info->sequence = s_sequence;
        info->width = s_latest_width;
        info->height = s_latest_height;
        info->jpeg_size = s_latest_jpeg_size;
        info->fingerprint = s_latest_fingerprint;
    }

    xSemaphoreGive(s_photo_mutex);

    *jpeg_copy = copy;
    return ESP_OK;
}
