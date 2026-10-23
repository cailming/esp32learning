/*
 * ESP32-S3-CAM：BOOT 按键拍照 + 浏览器查看最新照片
 *
 * 使用方式：
 * 1. ESP32-S3-CAM 上电后会创建 Wi-Fi 热点：S3CAM-Photo
 * 2. 电脑连接该热点，密码：12345678
 * 3. 浏览器打开：http://192.168.4.1
 * 4. 每按一次板载 BOOT 按钮（GPIO0），摄像头拍一张 JPEG
 * 5. 网页会自动检测新照片并刷新显示
 *
 * 注意：
 * - 麦克风、MAX98357A、喇叭可以继续保持原来的物理接线；
 * - 本程序不会初始化 MIC / Speaker 的 I2S GPIO；
 * - BOOT 按钮在 ESP32-S3 上同时也是下载启动相关按键，正常运行时短按即可；
 * - 不要在按住 BOOT 的同时复位/重新上电，否则可能进入下载模式。
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

/* -------------------- Camera GPIO：沿用已通过测试的配置 -------------------- */
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

/* ESP32-S3 板载 BOOT 键通常连接 GPIO0，按下时为低电平。 */
#define BOOT_BUTTON_GPIO GPIO_NUM_0

/* -------------------- Wi-Fi SoftAP -------------------- */
#define AP_SSID     "S3CAM-Photo"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL  1

/* -------------------- 最新一张 JPEG 的内存缓存 -------------------- */
static uint8_t *s_latest_jpeg = NULL;
static size_t s_latest_jpeg_len = 0;
static uint32_t s_photo_count = 0;
static SemaphoreHandle_t s_photo_mutex = NULL;

/*
 * 网页本身很简单：
 * - 每 500ms 请求一次 /status；
 * - 如果 photo_count 变化，则重新请求 /photo.jpg；
 * - URL 后加时间戳，避免浏览器缓存旧照片。
 */
static const char s_index_html[] =
    "<!doctype html>"
    "<html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-S3-CAM BOOT Photo</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;margin:24px;background:#f5f5f5;color:#222;}"
    ".card{max-width:720px;margin:auto;background:white;padding:20px;border-radius:14px;box-shadow:0 2px 12px #0002;}"
    "img{width:100%;height:auto;border-radius:10px;background:#ddd;margin-top:14px;}"
    ".hint{color:#555;line-height:1.6;}"
    ".status{font-weight:600;margin-top:12px;}"
    "</style></head><body><div class='card'>"
    "<h2>ESP32-S3-CAM BOOT 拍照测试</h2>"
    "<div class='hint'>每按一次开发板上的 BOOT 按钮，就拍摄一张照片。网页会自动显示最新照片。</div>"
    "<div id='status' class='status'>等待第一次拍照...</div>"
    "<img id='photo' style='display:none'>"
    "</div>"
    "<script>"
    "let lastCount=0;"
    "async function poll(){"
    " try{"
    "  const r=await fetch('/status?ts='+Date.now(),{cache:'no-store'});"
    "  const s=await r.json();"
    "  document.getElementById('status').textContent = s.count>0"
    "    ? ('已拍 '+s.count+' 张，最新 JPEG '+s.bytes+' bytes')"
    "    : '等待第一次拍照...';"
    "  if(s.count>0 && s.count!==lastCount){"
    "    lastCount=s.count;"
    "    const img=document.getElementById('photo');"
    "    img.src='/photo.jpg?n='+s.count+'&ts='+Date.now();"
    "    img.style.display='block';"
    "  }"
    " }catch(e){document.getElementById('status').textContent='与开发板通信失败';}"
    "}"
    "setInterval(poll,500);poll();"
    "</script></body></html>";

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
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,      /* 320 x 240，先用已验证稳定的配置 */
        .jpeg_quality = 12,                /* 数字越小质量越高；12 仍然比较节省内存 */
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor != NULL) {
            ESP_LOGI(TAG, "Camera init OK: PID=0x%04x VER=0x%02x", sensor->id.PID, sensor->id.VER);
        }
    }
    return err;
}

static esp_err_t capture_one_photo(void)
{
    ESP_LOGI(TAG, "BOOT pressed -> capturing photo...");

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Capture failed: esp_camera_fb_get() returned NULL");
        return ESP_FAIL;
    }

    bool valid_jpeg =
        fb->format == PIXFORMAT_JPEG &&
        fb->buf != NULL &&
        fb->len >= 4 &&
        fb->buf[0] == 0xFF &&
        fb->buf[1] == 0xD8;

    if (!valid_jpeg) {
        ESP_LOGE(TAG, "Capture failed: invalid JPEG frame, format=%d len=%u",
                 (int)fb->format, (unsigned)fb->len);
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    /*
     * Camera framebuffer 后面必须还给驱动，所以不能直接把 fb->buf 保存下来。
     * 这里复制一份 JPEG 到普通堆内存，供 HTTP 服务长期读取。
     */
    uint8_t *new_jpeg = malloc(fb->len);
    if (new_jpeg == NULL) {
        ESP_LOGE(TAG, "Capture failed: malloc(%u) failed", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        return ESP_ERR_NO_MEM;
    }

    memcpy(new_jpeg, fb->buf, fb->len);
    size_t new_len = fb->len;
    unsigned width = fb->width;
    unsigned height = fb->height;

    esp_camera_fb_return(fb);

    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);
    free(s_latest_jpeg);
    s_latest_jpeg = new_jpeg;
    s_latest_jpeg_len = new_len;
    ++s_photo_count;
    uint32_t count = s_photo_count;
    xSemaphoreGive(s_photo_mutex);

    ESP_LOGI(TAG, "PHOTO #" PRIu32 " OK: %ux%u, %u bytes", count, width, height, (unsigned)new_len);
    ESP_LOGI(TAG, "电脑浏览器打开/保持 http://192.168.4.1 ，网页会自动刷新");

    return ESP_OK;
}

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

static void boot_button_task(void *arg)
{
    (void)arg;

    int previous = gpio_get_level(BOOT_BUTTON_GPIO);

    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);

        /* 检测 1 -> 0：BOOT 被按下。 */
        if (previous == 1 && level == 0) {
            /* 简单的软件消抖。 */
            vTaskDelay(pdMS_TO_TICKS(35));

            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                capture_one_photo();

                /* 等待用户松开，避免长按被识别成多次拍照。 */
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }

        previous = gpio_get_level(BOOT_BUTTON_GPIO);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[96];
    uint32_t count;
    size_t bytes;

    xSemaphoreTake(s_photo_mutex, portMAX_DELAY);
    count = s_photo_count;
    bytes = s_latest_jpeg_len;
    xSemaphoreGive(s_photo_mutex);

    int len = snprintf(json, sizeof(json),
                       "{\"count\":%" PRIu32 ",\"bytes\":%u}",
                       count, (unsigned)bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, json, len);
}

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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    /*
     * 发送期间保持互斥锁，避免 BOOT 拍新照片时把当前 JPEG 内存替换掉。
     * QVGA JPEG 很小，这个阻塞时间通常很短。
     */
    esp_err_t err = httpd_resp_send(req, (const char *)s_latest_jpeg, s_latest_jpeg_len);
    xSemaphoreGive(s_photo_mutex);
    return err;
}

static void start_http_server(void)
{
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

    ESP_LOGI(TAG, "HTTP server started: http://192.168.4.1");
}

static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, AP_PASSWORD, sizeof(wifi_config.ap.password));
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
    ESP_LOGI(TAG, "Browser  : http://192.168.4.1");
}

void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM BOOT BUTTON PHOTO TEST");
    ESP_LOGI(TAG, "BOOT(GPIO0) -> capture JPEG -> browser display");
    ESP_LOGI(TAG, "MIC / MAX98357A / Speaker 保持接线，不需要拔除");
    ESP_LOGI(TAG, "====================================================");

    /* Wi-Fi 依赖 NVS。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_photo_mutex = xSemaphoreCreateMutex();
    if (s_photo_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create photo mutex");
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
    ESP_LOGI(TAG, "3) 浏览器打开: http://192.168.4.1");
    ESP_LOGI(TAG, "4) 每短按一次 BOOT，就拍摄一张新照片");
    ESP_LOGI(TAG, "====================================================");

    xTaskCreate(boot_button_task, "boot_button_task", 4096, NULL, 5, NULL);
}
