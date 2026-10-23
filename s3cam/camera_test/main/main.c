/*
 * ESP32-S3-CAM 摄像头独立测试
 *
 * 目标：
 * 1. 不拆除当前 MIC / MAX98357A / 喇叭接线；
 * 2. 程序只初始化板载摄像头；
 * 3. 连续抓取 30 帧，验证摄像头硬件和 Camera GPIO 是否正常。
 *
 * 通过标准：串口最终打印 "CAMERA TEST PASS"。
 */

#include <inttypes.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "camera_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "camera_test";

/*
 * 当前 ESP32-S3-CAM 板载摄像头 GPIO。
 * 这组引脚与当前外接音频引脚不冲突：
 *   MIC      : GPIO42 / GPIO41 / GPIO1
 *   Speaker  : GPIO14 / GPIO3 / GPIO46
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

#define TEST_FRAME_COUNT 30

static esp_err_t init_camera(void)
{
    /*
     * 为了把“硬件能不能正常工作”与 PSRAM 配置问题分开，
     * 第一版测试只抓 QVGA JPEG，并使用单个 DRAM framebuffer。
     * 这样即使暂时没有配置 PSRAM，也能完成最基础的摄像头验证。
     */
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
        .frame_size = FRAMESIZE_QVGA,      // 320 x 240，适合第一轮硬件测试
        .jpeg_quality = 15,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    return esp_camera_init(&config);
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM CAMERA TEST");
    ESP_LOGI(TAG, "MIC 和 MAX98357A 保持当前接线，不需要拔线");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Camera GPIO: XCLK=%d SIOD=%d SIOC=%d PCLK=%d VSYNC=%d HREF=%d",
             CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC,
             CAM_PIN_PCLK, CAM_PIN_VSYNC, CAM_PIN_HREF);

    esp_err_t err = init_camera();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA TEST FAIL: esp_camera_init() failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        ESP_LOGE(TAG, "先保留当前全部接线，记录这段日志，不要为了测试摄像头去拔 MIC/功放线");
        return;
    }

    ESP_LOGI(TAG, "Camera init OK");

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        ESP_LOGI(TAG, "Camera sensor detected: PID=0x%04x VER=0x%02x",
                 sensor->id.PID, sensor->id.VER);
    } else {
        ESP_LOGW(TAG, "Camera initialized, but sensor descriptor is NULL");
    }

    int ok_frames = 0;

    for (int i = 1; i <= TEST_FRAME_COUNT; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGE(TAG, "CAMERA TEST FAIL: frame #%d capture returned NULL", i);
            return;
        }

        bool valid = (fb->buf != NULL) && (fb->len > 0) && (fb->width > 0) && (fb->height > 0);

        ESP_LOGI(TAG,
                 "Frame %02d/%d: %ux%u, len=%u bytes, format=%d %s",
                 i,
                 TEST_FRAME_COUNT,
                 (unsigned)fb->width,
                 (unsigned)fb->height,
                 (unsigned)fb->len,
                 (int)fb->format,
                 valid ? "OK" : "INVALID");

        esp_camera_fb_return(fb);

        if (!valid) {
            ESP_LOGE(TAG, "CAMERA TEST FAIL: invalid frame data");
            return;
        }

        ++ok_frames;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "CAMERA TEST PASS: %d/%d frames captured successfully",
             ok_frames, TEST_FRAME_COUNT);
    ESP_LOGI(TAG, "========================================");

    /* 测试完成后保持程序运行，方便串口查看 PASS 结果。 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
