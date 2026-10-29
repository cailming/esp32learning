#include "asr_client.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "asr_client";

#define ASR_RESPONSE_MAX 2048
/* Intel CPU 上 ParaformerStreaming 首轮推理可能明显慢于实时，调试阶段避免 20s 过早超时。 */
#define ASR_HTTP_TIMEOUT_MS 60000

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buffer_t *resp = (response_buffer_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && resp != NULL && evt->data_len > 0) {
        size_t available = resp->cap > resp->len ? resp->cap - resp->len - 1 : 0;
        size_t copy_len = (size_t)evt->data_len < available ? (size_t)evt->data_len : available;
        if (copy_len > 0) {
            memcpy(resp->buf + resp->len, evt->data, copy_len);
            resp->len += copy_len;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
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

    char response[ASR_RESPONSE_MAX] = {0};
    response_buffer_t resp = {
        .buf = response,
        .cap = sizeof(response),
        .len = 0,
    };

    esp_http_client_config_t config = {
        .url = server_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = ASR_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Sample-Format", "s16le");

    const int content_len = (int)(samples * sizeof(int16_t));
    const double audio_seconds = (double)samples / 16000.0;
    esp_http_client_set_post_field(client, (const char *)pcm, content_len);

    int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "ASR HTTP START: url=%s PCM=%d bytes audio=%.2fs timeout=%dms",
             server_url,
             content_len,
             audio_seconds,
             ASR_HTTP_TIMEOUT_MS);

    esp_err_t err = esp_http_client_perform(client);
    int64_t elapsed_us = esp_timer_get_time() - started_us;

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ASR HTTP FAIL: elapsed=%.2fs err=%s",
                 (double)elapsed_us / 1000000.0,
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,
             "ASR HTTP END: status=%d elapsed=%.2fs response_bytes=%u response=%s",
             status,
             (double)elapsed_us / 1000000.0,
             (unsigned)resp.len,
             response[0] != '\0' ? response : "(empty)");

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
