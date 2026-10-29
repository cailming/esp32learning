#include "asr_client.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "asr_client";

#define ASR_RESPONSE_MAX 2048
/* 正常请求约 0.5~1.5 秒；5 秒足够容忍局域网抖动，同时避免单次请求卡死整个上传任务。 */
#define ASR_HTTP_TIMEOUT_MS 5000
/* PCM 分块写入 socket，避免一次性 set_post_field + perform 在较大 PSRAM buffer 上偶发长时间阻塞。 */
#define ASR_HTTP_WRITE_CHUNK 2048

static double elapsed_s(int64_t started_us)
{
    return (double)(esp_timer_get_time() - started_us) / 1000000.0;
}

esp_err_t asr_client_recognize(const char *server_url,
                               const int16_t *pcm,
                               size_t samples,
                               char *text,
                               size_t text_size)
{
    if (server_url == NULL || server_url[0] == '\0' || pcm == NULL || samples == 0 ||
        text == NULL || text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    text[0] = '\0';

    const int content_len = (int)(samples * sizeof(int16_t));
    const double audio_seconds = (double)samples / 16000.0;
    const int64_t total_started = esp_timer_get_time();

    esp_http_client_config_t config = {
        .url = server_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = ASR_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
        /* 每条 ASR 都是独立短请求；关闭 keep-alive，避免复用异常 TCP 连接。 */
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Sample-Format", "s16le");
    esp_http_client_set_header(client, "Connection", "close");

    ESP_LOGI(TAG,
             "ASR HTTP START: url=%s PCM=%d bytes audio=%.2fs timeout=%dms",
             server_url,
             content_len,
             audio_seconds,
             ASR_HTTP_TIMEOUT_MS);

    /*
     * 使用 ESP-IDF HTTP Stream API：
     *   open -> 多次 write -> fetch_headers -> read_response
     * 这样可以分别看到连接、上传、服务器处理/响应各阶段耗时。
     */
    int64_t stage_started = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ASR HTTP OPEN FAIL: elapsed=%.2fs err=%s",
                 elapsed_s(stage_started),
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    ESP_LOGI(TAG, "ASR HTTP OPEN OK: %.3fs", elapsed_s(stage_started));

    stage_started = esp_timer_get_time();
    const uint8_t *bytes = (const uint8_t *)pcm;
    int offset = 0;
    while (offset < content_len) {
        int remain = content_len - offset;
        int chunk = remain < ASR_HTTP_WRITE_CHUNK ? remain : ASR_HTTP_WRITE_CHUNK;
        int written = esp_http_client_write(client, (const char *)bytes + offset, chunk);
        if (written <= 0) {
            ESP_LOGE(TAG,
                     "ASR HTTP WRITE FAIL: offset=%d/%d elapsed=%.2fs",
                     offset,
                     content_len,
                     elapsed_s(stage_started));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        offset += written;
    }
    ESP_LOGI(TAG,
             "ASR HTTP WRITE OK: %d bytes in %.3fs",
             content_len,
             elapsed_s(stage_started));

    stage_started = esp_timer_get_time();
    int64_t response_content_len = esp_http_client_fetch_headers(client);
    if (response_content_len < 0) {
        ESP_LOGE(TAG,
                 "ASR HTTP FETCH HEADERS FAIL: value=%lld elapsed=%.2fs",
                 (long long)response_content_len,
                 elapsed_s(stage_started));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,
             "ASR HTTP HEADERS: status=%d content_length=%lld wait=%.3fs",
             status,
             (long long)response_content_len,
             elapsed_s(stage_started));

    char response[ASR_RESPONSE_MAX] = {0};
    stage_started = esp_timer_get_time();
    int read_len = esp_http_client_read_response(client, response, sizeof(response) - 1);
    if (read_len < 0) {
        ESP_LOGE(TAG,
                 "ASR HTTP READ FAIL: elapsed=%.2fs",
                 elapsed_s(stage_started));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_READ_TIMEOUT;
    }
    response[read_len] = '\0';

    ESP_LOGI(TAG,
             "ASR HTTP END: status=%d total=%.2fs read=%.3fs response_bytes=%d response=%s",
             status,
             elapsed_s(total_started),
             elapsed_s(stage_started),
             read_len,
             response[0] != '\0' ? response : "(empty)");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "ASR server HTTP %d: %s", status, response);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid JSON response: %s", response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *json_text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(json_text) || json_text->valuestring == NULL) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "ASR response has no text field: %s", response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(text, json_text->valuestring, text_size);
    cJSON_Delete(root);
    return ESP_OK;
}
