#include "dialog_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "dialog_client";

#define DIALOG_INPUT_SAMPLE_RATE       16000U
#define DIALOG_OUTPUT_SAMPLE_RATE      24000U
#define DIALOG_HTTP_TIMEOUT_MS         10000
#define DIALOG_UPLOAD_CHUNK_BYTES       2048U
#define DIALOG_RX_BUFFER_BYTES          1024U
#define DIALOG_STREAM_STORAGE_BYTES   (96U * 1024U + 1U)
#define DIALOG_PREBUFFER_BYTES         48000U
#define DIALOG_PLAY_TASK_STACK         (6U * 1024U)
#define DIALOG_PLAY_TASK_PRIORITY      6
#define DIALOG_PLAY_TASK_CORE          1
#define DIALOG_EMPTY_WAIT_MS           30U
#define DIALOG_UNDERRUN_SILENCE_MS     20U
#define DIALOG_TAIL_SILENCE_MS        120U
#define DIALOG_DMA_DRAIN_GUARD_MS      80U
#define DIALOG_ACOUSTIC_GUARD_MS      120U
#define DIALOG_ASR_TEXT_B64_BYTES      2048U
#define DIALOG_SAMPLE_RATE_HDR_BYTES     16U

typedef struct {
    i2s_chan_handle_t tx_chan;
    StreamBufferHandle_t stream;
    TaskHandle_t owner_task;
    volatile bool producer_done;
    volatile esp_err_t producer_result;
    esp_err_t consumer_result;
    size_t pcm_bytes_played;
    uint32_t underrun_count;
} dialog_playback_ctx_t;

typedef struct {
    char asr_text_b64[DIALOG_ASR_TEXT_B64_BYTES];
    char sample_rate[DIALOG_SAMPLE_RATE_HDR_BYTES];
} dialog_response_headers_t;

/*
 * ESP-IDF 6.1 默认不会保存 response headers；
 * esp_http_client_get_response_header() 只有在
 * CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS=y 时才会提供。
 *
 * 这里直接使用 HTTP_EVENT_ON_HEADER，把本业务需要的两个响应头拷贝出来，
 * 不依赖全局 SAVE_RESPONSE_HEADERS 配置，也避免额外的 HTTP client 内存开销。
 */
static esp_err_t dialog_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == NULL ||
        evt->header_key == NULL || evt->header_value == NULL) {
        return ESP_OK;
    }

    dialog_response_headers_t *headers = (dialog_response_headers_t *)evt->user_data;

    if (strcasecmp(evt->header_key, "X-ASR-Text-B64") == 0) {
        strlcpy(headers->asr_text_b64,
                evt->header_value,
                sizeof(headers->asr_text_b64));
    } else if (strcasecmp(evt->header_key, "X-Audio-Sample-Rate") == 0) {
        strlcpy(headers->sample_rate,
                evt->header_value,
                sizeof(headers->sample_rate));
    }

    return ESP_OK;
}

static esp_err_t i2s_write_all(i2s_chan_handle_t tx_chan,
                               const uint8_t *data,
                               size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_chan,
                                          data + offset,
                                          size - offset,
                                          &written,
                                          pdMS_TO_TICKS(2000));
        if (err != ESP_OK) return err;
        if (written == 0U) return ESP_FAIL;
        offset += written;
    }
    return ESP_OK;
}

static esp_err_t speaker_set_sample_rate(i2s_chan_handle_t tx_chan,
                                         uint32_t sample_rate)
{
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfig Speaker clock failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable Speaker I2S failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Speaker I2S clock -> %u Hz", (unsigned)sample_rate);
    return ESP_OK;
}

static esp_err_t speaker_write_silence(i2s_chan_handle_t tx_chan,
                                       uint32_t duration_ms)
{
    int16_t silence[128 * 2] = {0};
    size_t frames_left =
        ((size_t)DIALOG_OUTPUT_SAMPLE_RATE * (size_t)duration_ms) / 1000U;

    while (frames_left > 0U) {
        size_t frames = frames_left > 128U ? 128U : frames_left;
        esp_err_t err = i2s_write_all(tx_chan,
                                      (const uint8_t *)silence,
                                      frames * 2U * sizeof(int16_t));
        if (err != ESP_OK) return err;
        frames_left -= frames;
    }
    return ESP_OK;
}

static esp_err_t speaker_finish(i2s_chan_handle_t tx_chan)
{
    esp_err_t err = speaker_write_silence(tx_chan, DIALOG_TAIL_SILENCE_MS);
    vTaskDelay(pdMS_TO_TICKS(DIALOG_DMA_DRAIN_GUARD_MS));

    esp_err_t stop_err = i2s_channel_disable(tx_chan);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        if (err == ESP_OK) err = stop_err;
        ESP_LOGE(TAG, "disable Speaker I2S failed: %s", esp_err_to_name(stop_err));
    }
    return err;
}

static esp_err_t stream_send_all(StreamBufferHandle_t stream,
                                 const uint8_t *data,
                                 size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        size_t sent = xStreamBufferSend(stream,
                                        data + offset,
                                        size - offset,
                                        pdMS_TO_TICKS(2000));
        if (sent == 0U) return ESP_ERR_TIMEOUT;
        offset += sent;
    }
    return ESP_OK;
}

static void dialog_pcm_play_task(void *arg)
{
    dialog_playback_ctx_t *ctx = (dialog_playback_ctx_t *)arg;
    uint8_t pcm[DIALOG_RX_BUFFER_BYTES];
    int16_t stereo[(DIALOG_RX_BUFFER_BYTES / 2U + 1U) * 2U];
    bool pending_valid = false;
    uint8_t pending_byte = 0;

    ctx->consumer_result = ESP_OK;

    for (;;) {
        size_t received = xStreamBufferReceive(ctx->stream,
                                               pcm,
                                               sizeof(pcm),
                                               pdMS_TO_TICKS(DIALOG_EMPTY_WAIT_MS));
        if (received == 0U) {
            if (ctx->producer_done && xStreamBufferBytesAvailable(ctx->stream) == 0U) {
                break;
            }

            ctx->underrun_count++;
            ESP_LOGW(TAG,
                     "Dialog playback underrun #%lu: insert %u ms silence",
                     (unsigned long)ctx->underrun_count,
                     (unsigned)DIALOG_UNDERRUN_SILENCE_MS);
            esp_err_t silence_err =
                speaker_write_silence(ctx->tx_chan, DIALOG_UNDERRUN_SILENCE_MS);
            if (silence_err != ESP_OK) {
                ctx->consumer_result = silence_err;
                break;
            }
            continue;
        }

        size_t index = 0;
        size_t frames = 0;
        if (pending_valid && received > 0U) {
            int16_t sample =
                (int16_t)((uint16_t)pending_byte | ((uint16_t)pcm[0] << 8));
            stereo[0] = sample;
            stereo[1] = sample;
            frames = 1;
            index = 1;
            pending_valid = false;
            ctx->pcm_bytes_played += 2U;
        }

        while (index + 1U < received) {
            int16_t sample =
                (int16_t)((uint16_t)pcm[index] |
                          ((uint16_t)pcm[index + 1U] << 8));
            stereo[frames * 2U] = sample;
            stereo[frames * 2U + 1U] = sample;
            frames++;
            index += 2U;
            ctx->pcm_bytes_played += 2U;
        }

        if (index < received) {
            pending_byte = pcm[index];
            pending_valid = true;
        }

        if (frames > 0U) {
            esp_err_t err = i2s_write_all(ctx->tx_chan,
                                          (const uint8_t *)stereo,
                                          frames * 2U * sizeof(int16_t));
            if (err != ESP_OK) {
                ctx->consumer_result = err;
                break;
            }
        }
    }

    if (ctx->consumer_result == ESP_OK && pending_valid) {
        ESP_LOGE(TAG, "Dialog PCM ended with dangling byte");
        ctx->consumer_result = ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG,
             "dialog_pcm_play stack low-water=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    xTaskNotifyGive(ctx->owner_task);
    vTaskDelete(NULL);
}

static esp_err_t decode_b64_value(const char *encoded,
                                  const char *header_name,
                                  char *output,
                                  size_t output_size)
{
    if (output == NULL || output_size == 0U) return ESP_ERR_INVALID_ARG;
    output[0] = '\0';

    if (encoded == NULL || encoded[0] == '\0') {
        return ESP_OK;
    }

    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode((unsigned char *)output,
                                   output_size - 1U,
                                   &decoded_len,
                                   (const unsigned char *)encoded,
                                   strlen(encoded));
    if (rc != 0 || decoded_len >= output_size) {
        ESP_LOGE(TAG, "Decode %s failed: mbedtls=%d", header_name, rc);
        output[0] = '\0';
        return ESP_ERR_INVALID_RESPONSE;
    }

    output[decoded_len] = '\0';
    return ESP_OK;
}

static esp_err_t play_dialog_response_body(esp_http_client_handle_t client,
                                           i2s_chan_handle_t tx_chan)
{
    ESP_LOGI(TAG,
             "Entering dialog PCM receive, caller stack low-water=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    uint8_t *stream_storage = heap_caps_malloc(DIALOG_STREAM_STORAGE_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream_storage == NULL) {
        ESP_LOGE(TAG, "Cannot allocate dialog PSRAM stream buffer");
        return ESP_ERR_NO_MEM;
    }

    StaticStreamBuffer_t stream_struct = {0};
    StreamBufferHandle_t stream =
        xStreamBufferCreateStatic(DIALOG_STREAM_STORAGE_BYTES,
                                  1,
                                  stream_storage,
                                  &stream_struct);
    if (stream == NULL) {
        heap_caps_free(stream_storage);
        return ESP_ERR_NO_MEM;
    }

    uint8_t rx[DIALOG_RX_BUFFER_BYTES];
    dialog_playback_ctx_t playback = {0};
    bool playback_task_started = false;
    bool speaker_started = false;
    bool http_eof = false;
    size_t http_bytes = 0;
    esp_err_t err = ESP_OK;
    int64_t started_us = esp_timer_get_time();

    while (xStreamBufferBytesAvailable(stream) < DIALOG_PREBUFFER_BYTES) {
        int read_len = esp_http_client_read(client, (char *)rx, sizeof(rx));
        if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (read_len < 0) {
            ESP_LOGE(TAG, "Dialog PCM prebuffer read failed: %d", read_len);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            http_eof = true;
            break;
        }

        http_bytes += (size_t)read_len;
        err = stream_send_all(stream, rx, (size_t)read_len);
        if (err != ESP_OK) goto cleanup;
    }

    size_t prebuffer = xStreamBufferBytesAvailable(stream);
    if (prebuffer == 0U) {
        ESP_LOGW(TAG, "Dialog response contains no TTS PCM");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Dialog TTS prebuffer: %u bytes = %.3fs%s",
             (unsigned)prebuffer,
             prebuffer / (24000.0 * 2.0),
             http_eof ? " (full short utterance)" : "");

    err = speaker_set_sample_rate(tx_chan, DIALOG_OUTPUT_SAMPLE_RATE);
    if (err != ESP_OK) goto cleanup;
    speaker_started = true;

    playback.tx_chan = tx_chan;
    playback.stream = stream;
    playback.owner_task = xTaskGetCurrentTaskHandle();
    playback.producer_done = http_eof;
    playback.producer_result = ESP_OK;
    playback.consumer_result = ESP_OK;

    BaseType_t task_ok = xTaskCreatePinnedToCore(dialog_pcm_play_task,
                                                 "dialog_pcm_play",
                                                 DIALOG_PLAY_TASK_STACK,
                                                 &playback,
                                                 DIALOG_PLAY_TASK_PRIORITY,
                                                 NULL,
                                                 DIALOG_PLAY_TASK_CORE);
    if (task_ok != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    playback_task_started = true;

    if (!http_eof) {
        for (;;) {
            int read_len = esp_http_client_read(client, (char *)rx, sizeof(rx));
            if (read_len == -ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            if (read_len < 0) {
                playback.producer_result = ESP_FAIL;
                err = ESP_FAIL;
                break;
            }
            if (read_len == 0) break;

            http_bytes += (size_t)read_len;
            esp_err_t send_err = stream_send_all(stream, rx, (size_t)read_len);
            if (send_err != ESP_OK) {
                playback.producer_result = send_err;
                err = send_err;
                break;
            }
        }
    }

    playback.producer_done = true;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    playback_task_started = false;

    if (err == ESP_OK && playback.producer_result != ESP_OK) {
        err = playback.producer_result;
    }
    if (err == ESP_OK && playback.consumer_result != ESP_OK) {
        err = playback.consumer_result;
    }

cleanup:
    if (playback_task_started) {
        playback.producer_done = true;
        if (playback.producer_result == ESP_OK && err != ESP_OK) {
            playback.producer_result = err;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }

    if (speaker_started) {
        esp_err_t finish_err = speaker_finish(tx_chan);
        if (err == ESP_OK && finish_err != ESP_OK) err = finish_err;
    }

    vStreamBufferDelete(stream);
    heap_caps_free(stream_storage);

    ESP_LOGI(TAG,
             "Dialog TTS finished: err=%s HTTP=%u PCM=%u audio=%.3fs wall=%.3fs underruns=%lu stack_low_water=%u",
             esp_err_to_name(err),
             (unsigned)http_bytes,
             (unsigned)playback.pcm_bytes_played,
             playback.pcm_bytes_played / (24000.0 * 2.0),
             (esp_timer_get_time() - started_us) / 1000000.0,
             (unsigned long)playback.underrun_count,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return err;
}

esp_err_t dialog_client_exchange(const char *server_url,
                                 const int16_t *pcm,
                                 size_t samples,
                                 i2s_chan_handle_t tx_chan,
                                 SemaphoreHandle_t speaker_mutex,
                                 volatile bool *speaker_guard,
                                 char *text,
                                 size_t text_size)
{
    if (server_url == NULL || server_url[0] == '\0' || pcm == NULL || samples == 0U ||
        tx_chan == NULL || speaker_mutex == NULL || speaker_guard == NULL ||
        text == NULL || text_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    text[0] = '\0';
    const int content_len = (int)(samples * sizeof(int16_t));
    const double audio_seconds = (double)samples / DIALOG_INPUT_SAMPLE_RATE;

    /*
     * 这个结构里有约 2KB 的 Base64 header 缓冲。
     * 不能继续放在 asr_upload_task 的 6KB 栈上，否则进入 PCM 播放路径后很容易
     * 把任务栈压到危险区。优先放 PSRAM，失败时再退回普通 8-bit heap。
     */
    dialog_response_headers_t *response_headers =
        heap_caps_calloc(1,
                         sizeof(dialog_response_headers_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response_headers == NULL) {
        response_headers = heap_caps_calloc(1,
                                            sizeof(dialog_response_headers_t),
                                            MALLOC_CAP_8BIT);
    }
    if (response_headers == NULL) {
        ESP_LOGE(TAG, "Cannot allocate dialog response header cache");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "DIALOG stack low-water before HTTP=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    esp_http_client_config_t config = {
        .url = server_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DIALOG_HTTP_TIMEOUT_MS,
        .event_handler = dialog_http_event_handler,
        .user_data = response_headers,
        .buffer_size = DIALOG_RX_BUFFER_BYTES,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(response_headers);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Sample-Format", "s16le");

    int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "DIALOG HTTP START: url=%s PCM=%d bytes audio=%.2fs",
             server_url,
             content_len,
             audio_seconds);

    esp_err_t err = esp_http_client_open(client, content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DIALOG HTTP OPEN failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    size_t offset = 0;
    const uint8_t *bytes = (const uint8_t *)pcm;
    while (offset < (size_t)content_len) {
        size_t remaining = (size_t)content_len - offset;
        int chunk = (int)(remaining > DIALOG_UPLOAD_CHUNK_BYTES
                              ? DIALOG_UPLOAD_CHUNK_BYTES
                              : remaining);
        int written = esp_http_client_write(client,
                                            (const char *)bytes + offset,
                                            chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "DIALOG HTTP WRITE failed at %u/%d: %d",
                     (unsigned)offset, content_len, written);
            err = ESP_FAIL;
            goto cleanup;
        }
        offset += (size_t)written;
    }

    ESP_LOGI(TAG,
             "DIALOG HTTP WRITE OK: %d bytes in %.3fs",
             content_len,
             (esp_timer_get_time() - started_us) / 1000000.0);

    int64_t header_started_us = esp_timer_get_time();
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "DIALOG HTTP fetch headers failed: %lld",
                 (long long)content_length);
        err = ESP_FAIL;
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,
             "DIALOG HTTP HEADERS: status=%d content_length=%lld chunked=%s wait=%.3fs",
             status,
             (long long)content_length,
             esp_http_client_is_chunked_response(client) ? "yes" : "no",
             (esp_timer_get_time() - header_started_us) / 1000000.0);

    esp_err_t text_err = decode_b64_value(response_headers->asr_text_b64,
                                          "X-ASR-Text-B64",
                                          text,
                                          text_size);
    if (text_err != ESP_OK) {
        ESP_LOGW(TAG, "ASR text header unavailable: %s", esp_err_to_name(text_err));
    }
    ESP_LOGI(TAG, "DIALOG ASR TEXT: %s", text[0] != '\0' ? text : "(empty)");

    if (status == 204) {
        err = ESP_OK;
        goto cleanup;
    }
    if (status != 200) {
        char error_body[256] = {0};
        int n = esp_http_client_read_response(client, error_body, sizeof(error_body) - 1U);
        if (n > 0) error_body[n] = '\0';
        ESP_LOGE(TAG, "Dialog server HTTP %d: %s", status,
                 error_body[0] != '\0' ? error_body : "(no body)");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    if (response_headers->sample_rate[0] != '\0') {
        long sample_rate = strtol(response_headers->sample_rate, NULL, 10);
        if (sample_rate != (long)DIALOG_OUTPUT_SAMPLE_RATE) {
            ESP_LOGE(TAG, "Unexpected dialog PCM sample rate: %ld", sample_rate);
            err = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    ESP_LOGI(TAG,
             "DIALOG before Speaker mutex: stack low-water=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    if (xSemaphoreTake(speaker_mutex, portMAX_DELAY) != pdTRUE) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    ESP_LOGI(TAG, "DIALOG Speaker mutex acquired; start PCM response playback");

    *speaker_guard = true;
    err = play_dialog_response_body(client, tx_chan);
    vTaskDelay(pdMS_TO_TICKS(DIALOG_ACOUSTIC_GUARD_MS));
    *speaker_guard = false;
    xSemaphoreGive(speaker_mutex);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(response_headers);

    ESP_LOGI(TAG,
             "DIALOG HTTP END: err=%s total=%.3fs text=%s stack_low_water=%u",
             esp_err_to_name(err),
             (esp_timer_get_time() - started_us) / 1000000.0,
             text[0] != '\0' ? text : "(empty)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return err;
}