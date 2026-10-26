/*
 * ESP32-S3-CAM：BOOT 按键拍照 + 浏览器查看最新照片 + MJPEG 实时视频预览
 *
 * 当前版本把网页从 main.c 中拆了出去：
 *
 *   main.c
 *      ├── Camera
 *      ├── BOOT 按键
 *      ├── Wi-Fi SoftAP
 *      ├── HTTP API
 *      └── MJPEG 视频流
 *
 *   web/index.html
 *      └── HTML + CSS + JavaScript
 *
 * index.html 会在“编译阶段”通过 EMBED_TXTFILES 打包进固件，
 * 运行时不需要 SPIFFS / LittleFS，也不需要 SD 卡。
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "camera_boot_http_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "boot_photo";

/* ========================================================================
 * 1. Camera GPIO
 * ========================================================================
 * 下面这组 GPIO 已在前面的 camera_test 中实际验证通过。
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

/* 板载 BOOT：GPIO0，按下时为低电平。 */
#define BOOT_BUTTON_GPIO GPIO_NUM_0

/* ========================================================================
 * 2. Wi-Fi SoftAP
 * ======================================================================== */
#define AP_SSID     "S3CAM-Photo"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL  1

/* ========================================================================
 * 3. HTTP / MJPEG
 * ======================================================================== */
#define STREAM_PORT 81
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static const char *STREAM_BOUNDARY =
    "\r\n--" PART_BOUNDARY "\r\n";

static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n";

/* ========================================================================
 * 4. 编译进固件的 index.html
 * ========================================================================
 *
 * main/CMakeLists.txt 中：
 *
 *   EMBED_TXTFILES "web/index.html"
 *
 * ESP-IDF 会把这个文本文件直接打进固件，并由链接器生成符号：
 *
 *   _binary_web_index_html_start
 *
 * EMBED_TXTFILES 会在文本末尾自动补 '\0'，所以这里可以直接把它当 C 字符串使用。
 *
 * 注意：这个符号不是我们自己定义的变量，而是构建系统在链接阶段自动生成的。
 */
extern const uint8_t index_html_start[] asm("_binary_web_index_html_start");

/* ========================================================================
 * 5. 最近一次 BOOT 静态照片缓存
 * ======================================================================== */
static uint8_t *s_latest_jpeg = NULL;
static size_t s_latest_jpeg_len = 0;
static uint32_t s_photo_count = 0;

/*
 * s_photo_mutex：保护最近一张静态照片缓存。
 * s_camera_mutex：保护 Camera framebuffer 获取过程。
 */
static SemaphoreHandle_t s_photo_mutex = NULL;
static SemaphoreHandle_t s_camera_mutex = NULL;

/* ========================================================================
 * 6. 初始化 Camera
 * ======================================================================== */
static esp_err_t init_camera(void)
{
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

        /* 直接由 OV3660 输出 JPEG，避免 ESP32 再软件压缩。 */
        .pixel_format = PIXFORMAT_JPEG,

        /* 当前先使用已验证稳定的 QVGA 320x240。 */
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,

        /* 单 framebuffer，因此 BOOT 和 /video 必须通过 mutex 协调。 */
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);

    if (err == ESP_OK) {
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor != NULL) {
            ESP_LOGI(TAG,
                     "Camera init OK: PID=0x%04x VER=0x%02x",
                     sensor->id.PID,
                     sensor->id.VER);
        }
    }

    return err;
}

/* ========================================================================
 * 7. BOOT 按一次，拍一张静态 JPEG
 * ======================================================================== */
static esp_err_t capture_one_photo(void)
{
    ESP_LOGI(TAG, "BOOT pressed -> capturing photo...");

    /* 和实时视频流竞争 Camera，所以先取得独占锁。 */
    xSemaphoreTake(s_camera_mutex, portMAX_DELAY);

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        xSemaphoreGive(s_camera_mutex);
        ESP_LOGE(TAG, "Capture failed: esp_camera_fb_get() returned NULL");
        return ESP_FAIL;
    }

    /* JPEG SOI 标志通常为 FF D8。 */
    bool valid_jpeg =
        fb->format == PIXFORMAT_JPEG &&
        fb->buf != NULL &&
        fb->len >= 4 &&
        fb->buf[0] == 0xFF &&
        fb->buf[1] == 0xD8;

    if (!valid_jpeg) {
        ESP_LOGE(TAG,
                 "Capture failed: invalid JPEG frame, format=%d len=%u",
                 (int)fb->format,
                 (unsigned)fb->len);

        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);
        return ESP_FAIL;
    }

    /*
     * fb->buf 属于 Camera 驱动，esp_camera_fb_return() 后不能继续保存这个指针。
     * 所以这里 malloc 自己的内存并复制一份 JPEG。
     */
    uint8_t *new_jpeg = malloc(fb->len);
    if (new_jpeg == NULL) {
        ESP_LOGE(TAG, "Capture failed: malloc(%u) failed", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);
        return ESP_ERR_NO_MEM;
    }

    memcpy(new_jpeg, fb->buf, fb->len);

    size_t new_len = fb->len;
    unsigned width = fb->width;
    unsigned height = fb->height;

    /* JPEG 已复制，尽快归还 Camera framebuffer。 */
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_camera_mutex);

    /* 更新“最近一张 BOOT 照片”。 */
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    free(s_latest_jpeg);
    s_latest_jpeg = new_jpeg;
    s_latest_jpeg_len = new_len;
    ++s_photo_count;

    uint32_t count = s_photo_count;

    xSemaphoreGive(s_photo_mutex);

    ESP_LOGI(TAG,
             "PHOTO #" PRIu32 " OK: %ux%u, %u bytes",
             count,
             width,
             height,
             (unsigned)new_len);

    return ESP_OK;
}

/* ========================================================================
 * 8. BOOT GPIO
 * ======================================================================== */
static void init_boot_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
}

/*
 * BOOT 监听 Task：
 * - 检测 1 -> 0 下降沿；
 * - 35ms 软件消抖；
 * - 拍照后等待松手，避免长按连续拍照。
 */
static void boot_button_task(void *arg)
{
    (void)arg;

    int previous = gpio_get_level(BOOT_BUTTON_GPIO);

    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);

        if (previous == 1 && level == 0) {
            vTaskDelay(pdMS_TO_TICKS(35));

            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                capture_one_photo();

                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }

        previous = gpio_get_level(BOOT_BUTTON_GPIO);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ========================================================================
 * 9. 80 端口：网页 / 状态 / 静态照片
 * ======================================================================== */

/*
 * GET /
 *
 * 以前这里发送的是 main.c 中的 s_index_html。
 * 现在 HTML 已移到 web/index.html，并在编译时嵌入固件。
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /*
     * index_html_start 指向固件中的 index.html 文本。
     * 因为 EMBED_TXTFILES 自动补了 '\0'，可以使用 HTTPD_RESP_USE_STRLEN。
     */
    return httpd_resp_send(req,
                           (const char *)index_html_start,
                           HTTPD_RESP_USE_STRLEN);
}

/* GET /status：供 index.html 中的 JavaScript 每 500ms 轮询。 */
static esp_err_t status_handler(httpd_req_t *req)
{
    char json[96];
    uint32_t count;
    size_t bytes;

    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);
    count = s_photo_count;
    bytes = s_latest_jpeg_len;
    xSemaphoreGive(s_photo_mutex);

    int len = snprintf(json,
                       sizeof(json),
                       "{\"count\":%" PRIu32 ",\"bytes\":%u}",
                       count,
                       (unsigned)bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req,
                       "Cache-Control",
                       "no-store, no-cache, must-revalidate");

    return httpd_resp_send(req, json, len);
}

/* GET /photo.jpg：返回最近一次 BOOT 拍下来的静态 JPEG。 */
static esp_err_t photo_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);

    if (s_latest_jpeg == NULL || s_latest_jpeg_len == 0) {
        xSemaphoreGive(s_photo_mutex);

        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "还没有照片，请先按一次 BOOT 按钮。");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req,
                       "Cache-Control",
                       "no-store, no-cache, must-revalidate");

    /*
     * 发送期间保持照片锁，避免 BOOT Task 同时 free 当前 JPEG。
     */
    esp_err_t err = httpd_resp_send(req,
                                    (const char *)s_latest_jpeg,
                                    s_latest_jpeg_len);

    xSemaphoreGive(s_photo_mutex);
    return err;
}

/* ========================================================================
 * 10. 81 端口：MJPEG 实时视频
 * ========================================================================
 *
 * /video 会持续发送：
 *
 *   boundary
 *   JPEG #1
 *   boundary
 *   JPEG #2
 *   ...
 *
 * 浏览器把这个 multipart 长连接显示成实时视频。
 */
static esp_err_t video_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req,
                       "Cache-Control",
                       "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "MJPEG client connected: /video");

    /*
     * 视频流使用自己的 JPEG 缓存。
     * 每次从 Camera 抓到 JPEG 后先快速 memcpy，再立即归还 framebuffer。
     * 后续 Wi-Fi 即使发送较慢，也不会一直占着 Camera。
     */
    uint8_t *frame_copy = NULL;
    size_t frame_capacity = 0;
    char part_header[96];

    while (1) {
        size_t frame_len = 0;

        xSemaphoreTake(s_camera_mutex, portMAX_DELAY);

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            xSemaphoreGive(s_camera_mutex);
            ESP_LOGE(TAG, "Video capture failed: esp_camera_fb_get() returned NULL");
            res = ESP_FAIL;
            break;
        }

        bool valid_jpeg =
            fb->format == PIXFORMAT_JPEG &&
            fb->buf != NULL &&
            fb->len >= 4 &&
            fb->buf[0] == 0xFF &&
            fb->buf[1] == 0xD8;

        if (!valid_jpeg) {
            ESP_LOGW(TAG,
                     "Video skipped invalid JPEG frame: format=%d len=%u",
                     (int)fb->format,
                     (unsigned)fb->len);

            esp_camera_fb_return(fb);
            xSemaphoreGive(s_camera_mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* JPEG 大小每帧可能不同，容量不足时才 realloc。 */
        if (fb->len > frame_capacity) {
            uint8_t *new_buffer = realloc(frame_copy, fb->len);

            if (new_buffer == NULL) {
                ESP_LOGE(TAG,
                         "Video realloc(%u) failed",
                         (unsigned)fb->len);

                esp_camera_fb_return(fb);
                xSemaphoreGive(s_camera_mutex);
                res = ESP_ERR_NO_MEM;
                break;
            }

            frame_copy = new_buffer;
            frame_capacity = fb->len;
        }

        memcpy(frame_copy, fb->buf, fb->len);
        frame_len = fb->len;

        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);

        /* multipart：先 boundary。 */
        res = httpd_resp_send_chunk(req,
                                    STREAM_BOUNDARY,
                                    HTTPD_RESP_USE_STRLEN);
        if (res != ESP_OK) {
            break;
        }

        /* 再发送这一帧 JPEG 的 multipart header。 */
        int header_len = snprintf(part_header,
                                  sizeof(part_header),
                                  STREAM_PART,
                                  frame_len);

        if (header_len < 0 || header_len >= (int)sizeof(part_header)) {
            ESP_LOGE(TAG, "MJPEG part header too long");
            res = ESP_FAIL;
            break;
        }

        res = httpd_resp_send_chunk(req, part_header, header_len);
        if (res != ESP_OK) {
            break;
        }

        /* 最后发送 JPEG 二进制内容。 */
        res = httpd_resp_send_chunk(req,
                                    (const char *)frame_copy,
                                    frame_len);
        if (res != ESP_OK) {
            break;
        }

        /* 主动让出一点 CPU；实际帧率仍取决于 Camera/JPEG/Wi-Fi。 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    free(frame_copy);

    /* 结束 chunked response。客户端若已断开，这里失败也无妨。 */
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG,
             "MJPEG client disconnected: /video, result=%s",
             esp_err_to_name(res));

    return res;
}

/* ========================================================================
 * 11. 启动两个 HTTP Server
 * ========================================================================
 *
 * 80：/, /status, /photo.jpg
 * 81：/video
 */
static void start_http_server(void)
{
    /* -------------------- 80 端口普通 Web Server -------------------- */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t photo_uri = {
        .uri = "/photo.jpg",
        .method = HTTP_GET,
        .handler = photo_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &photo_uri));

    ESP_LOGI(TAG, "Web server started : http://192.168.4.1");

    /* -------------------- 81 端口 MJPEG Server -------------------- */
    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = STREAM_PORT;

    /* 第二个 HTTP Server 的内部 ctrl_port 也必须错开。 */
    stream_config.ctrl_port += 1;
    stream_config.stack_size = 8192;
    stream_config.max_uri_handlers = 4;

    httpd_handle_t stream_server = NULL;
    ESP_ERROR_CHECK(httpd_start(&stream_server, &stream_config));

    httpd_uri_t video_uri = {
        .uri = "/video",
        .method = HTTP_GET,
        .handler = video_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(stream_server, &video_uri));

    ESP_LOGI(TAG,
             "Video server started: http://192.168.4.1:%d/video",
             STREAM_PORT);
}

/* ========================================================================
 * 12. 启动 Wi-Fi SoftAP
 * ======================================================================== */
static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.ap.ssid,
            AP_SSID,
            sizeof(wifi_config.ap.ssid));

    strlcpy((char *)wifi_config.ap.password,
            AP_PASSWORD,
            sizeof(wifi_config.ap.password));

    wifi_config.ap.ssid_len = strlen(AP_SSID);
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = 2;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    wifi_config.ap.pmf_cfg.capable = true;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started");
    ESP_LOGI(TAG, "SSID     : %s", AP_SSID);
    ESP_LOGI(TAG, "Password : %s", AP_PASSWORD);
    ESP_LOGI(TAG, "Web      : http://192.168.4.1");
    ESP_LOGI(TAG, "Video    : http://192.168.4.1:%d/video", STREAM_PORT);
}

/* ========================================================================
 * 13. 程序入口
 * ======================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM BOOT PHOTO + MJPEG VIDEO TEST");
    ESP_LOGI(TAG, "HTML source: embedded web/index.html");
    ESP_LOGI(TAG, "MIC / MAX98357A / Speaker 保持接线，不需要拔除");
    ESP_LOGI(TAG, "====================================================");

    /* Wi-Fi 驱动依赖 NVS。 */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    s_photo_mutex = xSemaphoreCreateMutex();
    s_camera_mutex = xSemaphoreCreateMutex();

    if (s_photo_mutex == NULL || s_camera_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    ESP_ERROR_CHECK(init_camera());
    init_boot_button();
    start_wifi_ap();
    start_http_server();

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "READY");
    ESP_LOGI(TAG, "1) 电脑连接 Wi-Fi: %s", AP_SSID);
    ESP_LOGI(TAG, "2) 密码: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "3) 浏览器主页: http://192.168.4.1");
    ESP_LOGI(TAG, "4) 实时视频URL: http://192.168.4.1:%d/video", STREAM_PORT);
    ESP_LOGI(TAG, "5) 每短按一次 BOOT，还会保存一张最新照片");
    ESP_LOGI(TAG, "====================================================");

    xTaskCreate(boot_button_task,
                "boot_button_task",
                4096,
                NULL,
                5,
                NULL);
}
