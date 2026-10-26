#include "photo_http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_capture.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "photo_http";

#define AP_SSID       "S3CAM-Photo"
#define AP_PASSWORD   "12345678"
#define AP_CHANNEL    1
#define AP_MAX_CLIENT 2

/*
 * main/CMakeLists.txt 使用：
 *
 *   EMBED_TXTFILES "index.html"
 *
 * 为了避免不同 ESP-IDF 版本对“带子目录的嵌入资源”生成不同 linker symbol，
 * index.html 和 shutter.wav 一样都放在组件根目录。
 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");

static bool s_started = false;
static httpd_handle_t s_server = NULL;

/* HTTP 端再独立算一次 JPEG 指纹，用来核对复制出来的数据是否和 Camera 保存的一致。 */
static uint32_t jpeg_fingerprint(const uint8_t *data, size_t size)
{
    uint32_t hash = 2166136261u;

    if (data == NULL) {
        return 0;
    }

    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }

    return hash;
}

static void log_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "MEM %s: internal=%u, largest_internal=%u, dma=%u, psram=%u bytes",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t init_nvs_for_wifi(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase before Wi-Fi init");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t start_wifi_ap(void)
{
    ESP_RETURN_ON_ERROR(init_nvs_for_wifi(), TAG, "NVS init failed");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return ESP_FAIL;
    }

    /*
     * Wi-Fi 的静态 RX buffer 主要必须从内部 RAM / DMA-capable RAM 申请。
     * 如果这里 internal / largest_internal 太低，就会出现：
     *   Expected to init 10 rx buffer, actual is 9
     *   esp_wifi_init failed -> ESP_ERR_NO_MEM
     */
    log_memory("before esp_wifi_init");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        log_memory("after esp_wifi_init FAILED");
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }

    log_memory("after esp_wifi_init OK");

    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.ap.ssid,
            AP_SSID,
            sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password,
            AP_PASSWORD,
            sizeof(wifi_config.ap.password));

    wifi_config.ap.ssid_len = strlen(AP_SSID);
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CLIENT;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.capable = true;
    wifi_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP),
                        TAG,
                        "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config),
                        TAG,
                        "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    ESP_LOGI(TAG, "Wi-Fi SoftAP started");
    ESP_LOGI(TAG, "SSID     : %s", AP_SSID);
    ESP_LOGI(TAG, "Password : %s", AP_PASSWORD);
    ESP_LOGI(TAG, "Web      : http://192.168.4.1/");
    ESP_LOGI(TAG, "Photo    : http://192.168.4.1/photo.jpg");

    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    return httpd_resp_send(req,
                           (const char *)index_html_start,
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    size_t jpeg_size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t sequence = 0;

    const uint8_t *latest = camera_capture_latest(&jpeg_size,
                                                   &width,
                                                   &height,
                                                   &sequence);

    char json[160];
    int len = snprintf(json,
                       sizeof(json),
                       "{\"has_photo\":%s,\"sequence\":%lu,\"width\":%u,\"height\":%u,\"bytes\":%u}",
                       latest != NULL ? "true" : "false",
                       (unsigned long)sequence,
                       (unsigned)width,
                       (unsigned)height,
                       (unsigned)jpeg_size);

    if (len < 0 || len >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req,
                       "Cache-Control",
                       "no-store, no-cache, must-revalidate");

    return httpd_resp_send(req, json, len);
}

static esp_err_t photo_handler(httpd_req_t *req)
{
    uint8_t *jpeg_copy = NULL;
    camera_photo_info_t info = {0};

    ESP_LOGI(TAG, "HTTP /photo.jpg: request received");

    esp_err_t err = camera_capture_copy_latest(&jpeg_copy, &info);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "HTTP /photo.jpg: no photo available yet");
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req,
                                  "还没有照片，请先说：你好小智，然后说：拍照。");
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP /photo.jpg: copy latest failed: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to copy latest photo");
        return err;
    }

    const uint32_t copy_fp = jpeg_fingerprint(jpeg_copy, info.jpeg_size);

    ESP_LOGI(TAG,
             "HTTP /photo.jpg: prepared #%lu %ux%u %u bytes, stored_fp=%08lx copy_fp=%08lx",
             (unsigned long)info.sequence,
             (unsigned)info.width,
             (unsigned)info.height,
             (unsigned)info.jpeg_size,
             (unsigned long)info.fingerprint,
             (unsigned long)copy_fp);

    if (copy_fp != info.fingerprint) {
        ESP_LOGE(TAG,
                 "HTTP /photo.jpg: FINGERPRINT MISMATCH! stored=%08lx copy=%08lx",
                 (unsigned long)info.fingerprint,
                 (unsigned long)copy_fp);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req,
                       "Cache-Control",
                       "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    err = httpd_resp_send(req,
                          (const char *)jpeg_copy,
                          info.jpeg_size);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "HTTP /photo.jpg: SEND OK #%lu fp=%08lx",
                 (unsigned long)info.sequence,
                 (unsigned long)copy_fp);
    } else {
        ESP_LOGE(TAG,
                 "HTTP /photo.jpg: SEND FAILED #%lu fp=%08lx err=%s",
                 (unsigned long)info.sequence,
                 (unsigned long)copy_fp,
                 esp_err_to_name(err));
    }

    free(jpeg_copy);
    return err;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 6;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config),
                        TAG,
                        "httpd_start failed");

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t photo_uri = {
        .uri = "/photo.jpg",
        .method = HTTP_GET,
        .handler = photo_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root_uri),
                        TAG,
                        "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_uri),
                        TAG,
                        "register /status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &photo_uri),
                        TAG,
                        "register /photo.jpg failed");

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t photo_http_server_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(start_wifi_ap(), TAG, "SoftAP start failed");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "HTTP server start failed");

    s_started = true;

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "PHOTO WEB READY");
    ESP_LOGI(TAG, "1) Connect Wi-Fi: %s", AP_SSID);
    ESP_LOGI(TAG, "2) Password      : %s", AP_PASSWORD);
    ESP_LOGI(TAG, "3) Open browser  : http://192.168.4.1/");
    ESP_LOGI(TAG, "====================================================");

    return ESP_OK;
}
