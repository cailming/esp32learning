#include "tts_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "tts_client";

#define TTS_SAMPLE_RATE              24000U
#define HTTP_RX_BUFFER_SIZE           4096U
#define JSON_BODY_SIZE                 512U
#define TTS_URL_BUFFER_SIZE            256U
#define HTTP_TIMEOUT_MS              10000
#define TTS_SPEECH_SPEED                 3

/*
 * ChatTTS 的 HTTP 响应是 24kHz / mono / PCM16。
 *
 * 之前的实现是：
 *   HTTP read -> 立刻同步 i2s_channel_write -> 再 HTTP read
 *
 * i2s_channel_write() 是阻塞调用，一块 4096-byte mono PCM 约对应 85ms 音频。
 * 在这个阻塞期间网络虽然仍可由 TCP/IP 栈接收，但应用层不能持续 read；一旦下一
 * 批数据没有及时进入 socket，就会出现 I2S TX 断粮。MAX98357A 听起来会像较长的
 * “嘀嗒/机关枪”片段。
 *
 * 这里改成生产者/消费者：
 *   当前 speech_detect_task = HTTP producer
 *   tts_pcm_play task         = I2S consumer
 *
 * HTTP producer 把 mono PCM 先放入 PSRAM StreamBuffer；达到约 0.5 秒预缓冲后再
 * 启动 I2S consumer。RTX3090 生成速度约 3x realtime，因此后续 producer 应能持续
 * 跑在 consumer 前面，吸收 Wi-Fi/TCP 抖动。
 */
#define TTS_STREAM_STORAGE_BYTES    (48U * 1024U + 1U)
#define TTS_PREBUFFER_BYTES         24000U  /* 0.5s * 24000Hz * 2 bytes */
#define TTS_PLAY_TASK_STACK         (6U * 1024U)
#define TTS_PLAY_TASK_PRIORITY      6
#define TTS_PLAY_TASK_CORE          1
#define TTS_EMPTY_WAIT_MS           30U
#define TTS_UNDERRUN_SILENCE_MS     20U

/*
 * TTS 结束时仍保留已验证有效的尾部处理：先在 24kHz 下补静音，再 disable I2S1。
 * 这样不会重新出现“播完后持续机关枪尾音”。
 */
#define TTS_TAIL_SILENCE_MS         120U
#define TTS_DMA_DRAIN_GUARD_MS       80U
#define SILENCE_FRAMES_PER_BLOCK    256U

/*
 * 一次最多消费 HTTP_RX_BUFFER_SIZE bytes mono PCM；若前一块留下奇数字节，最多
 * 多拼 1 个 sample。I2S 使用 stereo framing，所以每个 mono sample 左右复制。
 */
static int16_t s_stereo_output[(HTTP_RX_BUFFER_SIZE / 2U + 1U) * 2U];
static int16_t s_silence_output[SILENCE_FRAMES_PER_BLOCK * 2U];

typedef struct {
    i2s_chan_handle_t tx_chan;
    StreamBufferHandle_t stream;
    TaskHandle_t owner_task;
    volatile bool producer_done;
    volatile esp_err_t producer_result;
    esp_err_t consumer_result;
    size_t pcm_bytes_played;
    uint32_t underrun_count;
} tts_playback_ctx_t;

static esp_err_t speaker_set_sample_rate(i2s_chan_handle_t tx_chan,
                                         uint32_t sample_rate)
{
    /* TTS 完成后 channel 保持 disabled；因此 INVALID_STATE 表示本来就停着。 */
    esp_err_t err = i2s_channel_disable(tx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG,
                 "i2s_channel_disable before %u Hz failed: %s",
                 (unsigned)sample_rate,
                 esp_err_to_name(err));
        return err;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "reconfig I2S clock to %u Hz failed: %s",
                 (unsigned)sample_rate,
                 esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_channel_enable at %u Hz failed: %s",
                 (unsigned)sample_rate,
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Speaker I2S clock -> %u Hz", (unsigned)sample_rate);
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
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0U) {
            return ESP_FAIL;
        }
        offset += written;
    }

    return ESP_OK;
}

static esp_err_t speaker_write_silence(i2s_chan_handle_t tx_chan,
                                       uint32_t duration_ms)
{
    size_t frames_left =
        ((size_t)TTS_SAMPLE_RATE * (size_t)duration_ms) / 1000U;

    while (frames_left > 0U) {
        size_t frames = frames_left;
        if (frames > SILENCE_FRAMES_PER_BLOCK) {
            frames = SILENCE_FRAMES_PER_BLOCK;
        }

        const size_t bytes = frames * 2U * sizeof(int16_t);
        esp_err_t err = i2s_write_all(tx_chan,
                                      (const uint8_t *)s_silence_output,
                                      bytes);
        if (err != ESP_OK) {
            return err;
        }
        frames_left -= frames;
    }

    return ESP_OK;
}

static esp_err_t speaker_flush_tts_tail(i2s_chan_handle_t tx_chan)
{
    ESP_LOGI(TAG,
             "Flush TTS tail with %u ms silence before stopping I2S",
             (unsigned)TTS_TAIL_SILENCE_MS);

    esp_err_t err = speaker_write_silence(tx_chan, TTS_TAIL_SILENCE_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "write TTS tail silence failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(TTS_DMA_DRAIN_GUARD_MS));

    ESP_LOGI(TAG,
             "TTS tail drained: silence=%u ms guard=%u ms",
             (unsigned)TTS_TAIL_SILENCE_MS,
             (unsigned)TTS_DMA_DRAIN_GUARD_MS);
    return ESP_OK;
}

static bool is_ascii_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

static const char *skip_leading_url_space(const char *p)
{
    while (*p != '\0') {
        const uint8_t c0 = (uint8_t)p[0];
        if (is_ascii_space(c0)) {
            ++p;
            continue;
        }
        if (c0 == 0xC2U && (uint8_t)p[1] == 0xA0U) {
            p += 2;
            continue;
        }
        break;
    }
    return p;
}

static esp_err_t normalize_server_url(const char *input,
                                      char *output,
                                      size_t output_size)
{
    if (input == NULL || output == NULL || output_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = skip_leading_url_space(input);
    if (strncmp(p, "http://", 7) != 0 && strncmp(p, "https://", 8) != 0) {
        ESP_LOGE(TAG,
                 "Invalid TTS URL: must start with http:// or https:// after trimming whitespace");
        return ESP_ERR_INVALID_ARG;
    }

    size_t n = 0;
    while (*p != '\0') {
        const uint8_t c0 = (uint8_t)p[0];
        if (is_ascii_space(c0)) {
            break;
        }
        if (c0 == 0xC2U && (uint8_t)p[1] == 0xA0U) {
            break;
        }
        if (n + 1U >= output_size) {
            ESP_LOGE(TAG,
                     "TTS URL is too long (max %u chars)",
                     (unsigned)(output_size - 1U));
            return ESP_ERR_INVALID_SIZE;
        }
        output[n++] = *p++;
    }

    output[n] = '\0';
    return n > 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static int append_json_escaped(char *dst,
                               size_t dst_size,
                               size_t *offset,
                               const char *src)
{
    for (const unsigned char *p = (const unsigned char *)src;
         *p != '\0';
         ++p) {
        const char *escape = NULL;
        char unicode_escape[7] = {0};

        switch (*p) {
        case '\"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default:
            if (*p < 0x20U) {
                snprintf(unicode_escape,
                         sizeof(unicode_escape),
                         "\\u%04x",
                         (unsigned)*p);
                escape = unicode_escape;
            }
            break;
        }

        if (escape != NULL) {
            const size_t len = strlen(escape);
            if (*offset + len >= dst_size) {
                return -1;
            }
            memcpy(dst + *offset, escape, len);
            *offset += len;
        } else {
            if (*offset + 1U >= dst_size) {
                return -1;
            }
            dst[(*offset)++] = (char)*p;
        }
    }

    return 0;
}

static esp_err_t build_request_json(const char *text,
                                    char *body,
                                    size_t body_size,
                                    size_t *body_len)
{
    static const char prefix[] = "{\"text\":\"";
    static const char suffix[] =
        "\",\"lang\":\"zh\",\"speed\":3,\"skip_refine_text\":true}";

    size_t offset = 0;
    if (sizeof(prefix) - 1U + sizeof(suffix) - 1U >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(body + offset, prefix, sizeof(prefix) - 1U);
    offset += sizeof(prefix) - 1U;

    if (append_json_escaped(body, body_size, &offset, text) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (offset + sizeof(suffix) > body_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(body + offset, suffix, sizeof(suffix) - 1U);
    offset += sizeof(suffix) - 1U;
    body[offset] = '\0';
    *body_len = offset;
    return ESP_OK;
}

static esp_err_t play_pcm_bytes(i2s_chan_handle_t tx_chan,
                                const uint8_t *data,
                                size_t size,
                                bool *pending_valid,
                                uint8_t *pending_byte,
                                size_t *pcm_bytes_played)
{
    size_t index = 0;
    size_t frames = 0;

    if (*pending_valid && size > 0U) {
        const int16_t sample =
            (int16_t)((uint16_t)(*pending_byte) | ((uint16_t)data[0] << 8));
        s_stereo_output[0] = sample;
        s_stereo_output[1] = sample;
        frames = 1;
        index = 1;
        *pending_valid = false;
        *pcm_bytes_played += 2U;
    }

    while (index + 1U < size) {
        const int16_t sample =
            (int16_t)((uint16_t)data[index] |
                      ((uint16_t)data[index + 1U] << 8));
        s_stereo_output[frames * 2U] = sample;
        s_stereo_output[frames * 2U + 1U] = sample;
        frames++;
        index += 2U;
        *pcm_bytes_played += 2U;
    }

    if (index < size) {
        *pending_byte = data[index];
        *pending_valid = true;
    }

    if (frames == 0U) {
        return ESP_OK;
    }

    return i2s_write_all(tx_chan,
                         (const uint8_t *)s_stereo_output,
                         frames * 2U * sizeof(int16_t));
}

static void tts_pcm_play_task(void *arg)
{
    tts_playback_ctx_t *ctx = (tts_playback_ctx_t *)arg;
    uint8_t pcm[HTTP_RX_BUFFER_SIZE];
    bool pending_valid = false;
    uint8_t pending_byte = 0;

    ctx->consumer_result = ESP_OK;

    for (;;) {
        size_t received = xStreamBufferReceive(ctx->stream,
                                               pcm,
                                               sizeof(pcm),
                                               pdMS_TO_TICKS(TTS_EMPTY_WAIT_MS));

        if (received == 0U) {
            if (ctx->producer_done &&
                xStreamBufferBytesAvailable(ctx->stream) == 0U) {
                break;
            }

            /*
             * 理论上 0.5 秒预缓冲 + 3090 的 3x realtime 足以避免这里发生。
             * 若仍发生，不重复旧 DMA 内容，而是主动写一小段静音并计数。
             */
            ctx->underrun_count++;
            ESP_LOGW(TAG,
                     "TTS playback underrun #%lu: insert %u ms silence",
                     (unsigned long)ctx->underrun_count,
                     (unsigned)TTS_UNDERRUN_SILENCE_MS);

            esp_err_t silence_err =
                speaker_write_silence(ctx->tx_chan, TTS_UNDERRUN_SILENCE_MS);
            if (silence_err != ESP_OK) {
                ctx->consumer_result = silence_err;
                break;
            }
            continue;
        }

        esp_err_t err = play_pcm_bytes(ctx->tx_chan,
                                       pcm,
                                       received,
                                       &pending_valid,
                                       &pending_byte,
                                       &ctx->pcm_bytes_played);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "I2S PCM playback failed: %s",
                     esp_err_to_name(err));
            ctx->consumer_result = err;
            break;
        }
    }

    if (ctx->consumer_result == ESP_OK && pending_valid) {
        ESP_LOGE(TAG, "PCM stream ended with one dangling byte");
        ctx->consumer_result = ESP_ERR_INVALID_SIZE;
    }

    xTaskNotifyGive(ctx->owner_task);
    vTaskDelete(NULL);
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
        if (sent == 0U) {
            return ESP_ERR_TIMEOUT;
        }
        offset += sent;
    }
    return ESP_OK;
}

esp_err_t tts_client_play(i2s_chan_handle_t tx_chan,
                          const char *server_url,
                          const char *text)
{
    if (tx_chan == NULL || server_url == NULL || text == NULL ||
        server_url[0] == '\0' || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized_url[TTS_URL_BUFFER_SIZE];
    esp_err_t err = normalize_server_url(server_url,
                                         normalized_url,
                                         sizeof(normalized_url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS server URL config is invalid");
        return err;
    }

    char body[JSON_BODY_SIZE];
    size_t body_len = 0;
    err = build_request_json(text, body, sizeof(body), &body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS request JSON too large");
        return err;
    }

    ESP_LOGI(TAG, "POST normalized URL: '%s'", normalized_url);
    ESP_LOGI(TAG, "TTS text: %s", text);
    ESP_LOGI(TAG, "TTS speech speed: %d (lower = slower)", TTS_SPEECH_SPEED);

    esp_http_client_config_t config = {
        .url = normalized_url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_RX_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG,
                 "esp_http_client_init failed for URL '%s'",
                 normalized_url);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *stream_storage = NULL;
    StreamBufferHandle_t stream = NULL;
    StaticStreamBuffer_t stream_struct;
    tts_playback_ctx_t playback = {0};
    bool playback_task_started = false;
    bool speaker_started = false;
    uint8_t rx_buffer[HTTP_RX_BUFFER_SIZE];
    size_t http_bytes_received = 0;
    int64_t request_started_us = esp_timer_get_time();
    int64_t first_pcm_us = 0;
    int64_t playback_started_us = 0;

    err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_http_client_set_header(client,
                                     "Content-Type",
                                     "application/json");
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    size_t body_offset = 0;
    while (body_offset < body_len) {
        int written = esp_http_client_write(client,
                                            body + body_offset,
                                            (int)(body_len - body_offset));
        if (written <= 0) {
            ESP_LOGE(TAG, "HTTP request body write failed: %d", written);
            err = ESP_FAIL;
            goto cleanup;
        }
        body_offset += (size_t)written;
    }

    int64_t headers = esp_http_client_fetch_headers(client);
    if (headers < 0) {
        ESP_LOGE(TAG,
                 "HTTP fetch headers failed: %lld",
                 (long long)headers);
        err = ESP_FAIL;
        goto cleanup;
    }

    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "TTS server returned HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    stream_storage = heap_caps_malloc(TTS_STREAM_STORAGE_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream_storage == NULL) {
        ESP_LOGE(TAG,
                 "Cannot allocate %u-byte TTS PSRAM stream buffer",
                 (unsigned)TTS_STREAM_STORAGE_BYTES);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    memset(&stream_struct, 0, sizeof(stream_struct));
    stream = xStreamBufferCreateStatic(TTS_STREAM_STORAGE_BYTES,
                                       1,
                                       stream_storage,
                                       &stream_struct);
    if (stream == NULL) {
        ESP_LOGE(TAG, "xStreamBufferCreateStatic failed");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /*
     * 先积累约 0.5 秒 PCM，再启动 I2S。这样第一批模型输出之后即使网络短暂抖动，
     * consumer 也有足够余量继续播。
     */
    bool http_eof = false;
    while (xStreamBufferBytesAvailable(stream) < TTS_PREBUFFER_BYTES) {
        int read_len = esp_http_client_read(client,
                                            (char *)rx_buffer,
                                            sizeof(rx_buffer));
        if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP PCM prebuffer read failed: %d", read_len);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            http_eof = true;
            break;
        }

        if (first_pcm_us == 0) {
            first_pcm_us = esp_timer_get_time();
            ESP_LOGI(TAG,
                     "First PCM received: %.3fs, bytes=%d",
                     (first_pcm_us - request_started_us) / 1000000.0,
                     read_len);
        }

        http_bytes_received += (size_t)read_len;
        err = stream_send_all(stream, rx_buffer, (size_t)read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TTS prebuffer send failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }

    const size_t prebuffer_bytes = xStreamBufferBytesAvailable(stream);
    if (prebuffer_bytes == 0U) {
        ESP_LOGE(TAG, "TTS server returned an empty PCM body");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "TTS prebuffer ready: %u bytes = %.3fs audio",
             (unsigned)prebuffer_bytes,
             prebuffer_bytes / (24000.0 * 2.0));

    err = speaker_set_sample_rate(tx_chan, TTS_SAMPLE_RATE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    speaker_started = true;
    playback_started_us = esp_timer_get_time();

    playback.tx_chan = tx_chan;
    playback.stream = stream;
    playback.owner_task = xTaskGetCurrentTaskHandle();
    playback.producer_done = http_eof;
    playback.producer_result = ESP_OK;
    playback.consumer_result = ESP_OK;

    BaseType_t task_ok = xTaskCreatePinnedToCore(tts_pcm_play_task,
                                                 "tts_pcm_play",
                                                 TTS_PLAY_TASK_STACK,
                                                 &playback,
                                                 TTS_PLAY_TASK_PRIORITY,
                                                 NULL,
                                                 TTS_PLAY_TASK_CORE);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "create tts_pcm_play task failed");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    playback_task_started = true;

    /* producer：继续尽可能快地从 HTTP 读取并塞进 PSRAM buffer。 */
    if (!http_eof) {
        while (true) {
            int read_len = esp_http_client_read(client,
                                                (char *)rx_buffer,
                                                sizeof(rx_buffer));
            if (read_len == -ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP PCM read failed: %d", read_len);
                playback.producer_result = ESP_FAIL;
                err = ESP_FAIL;
                break;
            }
            if (read_len == 0) {
                break;
            }

            http_bytes_received += (size_t)read_len;
            esp_err_t send_err =
                stream_send_all(stream, rx_buffer, (size_t)read_len);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "TTS stream buffer send failed: %s",
                         esp_err_to_name(send_err));
                playback.producer_result = send_err;
                err = send_err;
                break;
            }
        }
    }

    playback.producer_done = true;

    /* 等 consumer 把 StreamBuffer 中剩余 PCM 全部播放完。 */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    playback_task_started = false;

    if (err == ESP_OK && playback.producer_result != ESP_OK) {
        err = playback.producer_result;
    }
    if (err == ESP_OK && playback.consumer_result != ESP_OK) {
        err = playback.consumer_result;
    }

cleanup:
    /*
     * 如果错误发生在 consumer 已启动之后，先告诉它 producer 不会再写，并等待它退出，
     * 再释放 StreamBuffer/PSRAM，避免 task 访问已经释放的内存。
     */
    if (playback_task_started) {
        playback.producer_done = true;
        if (playback.producer_result == ESP_OK && err != ESP_OK) {
            playback.producer_result = err;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        playback_task_started = false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (speaker_started) {
        esp_err_t tail_err = speaker_flush_tts_tail(tx_chan);
        if (err == ESP_OK && tail_err != ESP_OK) {
            err = tail_err;
        }

        esp_err_t stop_err = i2s_channel_disable(tx_chan);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG,
                     "stop Speaker I2S after TTS failed: %s",
                     esp_err_to_name(stop_err));
            if (err == ESP_OK) {
                err = stop_err;
            }
        } else {
            ESP_LOGI(TAG, "Speaker I2S disabled after TTS");
        }
    }

    if (stream != NULL) {
        vStreamBufferDelete(stream);
    }
    if (stream_storage != NULL) {
        heap_caps_free(stream_storage);
    }

    const int64_t finished_us = esp_timer_get_time();
    const double elapsed =
        (finished_us - request_started_us) / 1000000.0;
    const double audio_seconds =
        playback.pcm_bytes_played / (24000.0 * 2.0);
    const double playback_start_latency =
        playback_started_us > 0
            ? (playback_started_us - request_started_us) / 1000000.0
            : 0.0;

    ESP_LOGI(TAG,
             "TTS finished: err=%s HTTP=%u PCM=%u audio=%.3fs wall=%.3fs start=%.3fs underruns=%lu",
             esp_err_to_name(err),
             (unsigned)http_bytes_received,
             (unsigned)playback.pcm_bytes_played,
             audio_seconds,
             elapsed,
             playback_start_latency,
             (unsigned long)playback.underrun_count);

    return err;
}
