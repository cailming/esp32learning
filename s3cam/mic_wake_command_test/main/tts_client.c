#include "tts_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tts_client";

#define TTS_SAMPLE_RATE             24000U
#define SPEAKER_IDLE_SAMPLE_RATE    16000U
#define HTTP_RX_BUFFER_SIZE         2048U
#define JSON_BODY_SIZE              512U
#define TTS_URL_BUFFER_SIZE         256U
#define HTTP_TIMEOUT_MS             10000

/*
 * TTS 结束后不能立刻把 I2S1 从 24kHz 切回 16kHz。
 * i2s_channel_write() 返回时，最后一批样本可能仍在 DMA 中等待真正送完。
 * 如果此时 disable -> reconfig -> enable，DMA 中残留的非零语音数据可能在
 * 新时钟下继续/重复输出，MAX98357A 听起来就会像“嘀嗒/机关枪”。
 *
 * 因此结束时先在 24kHz 下写入一段全 0 PCM，让 DMA 尾部被静音覆盖，
 * 再额外等待一小段时间确保这些 0 真正移出 I2S，最后才恢复 16kHz。
 */
#define TTS_TAIL_SILENCE_MS         120U
#define TTS_DMA_DRAIN_GUARD_MS       80U
#define SILENCE_FRAMES_PER_BLOCK    256U

/*
 * HTTP 一次最多读取 2048 bytes mono PCM，也就是 1024 个 int16 sample。
 * 如果上一次 read 恰好以奇数字节结束，下一次最多会多拼出 1 个 sample，
 * 所以这里预留 1025 frame。I2S 使用 stereo framing，因此 *2。
 */
static int16_t s_stereo_output[(HTTP_RX_BUFFER_SIZE / 2U + 1U) * 2U];

/* 静态区自动清零，专门用于 TTS 尾部把 DMA 内容刷成静音。 */
static int16_t s_silence_output[SILENCE_FRAMES_PER_BLOCK * 2U];

static esp_err_t speaker_set_sample_rate(i2s_chan_handle_t tx_chan,
                                         uint32_t sample_rate)
{
    esp_err_t err = i2s_channel_disable(tx_chan);
    if (err != ESP_OK) {
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
        /* 尽量恢复 enable，避免后续 Speaker 完全无声。 */
        i2s_channel_enable(tx_chan);
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
        if (written == 0) {
            return ESP_FAIL;
        }
        offset += written;
    }

    return ESP_OK;
}

/*
 * 在仍保持 24kHz 的情况下向 I2S1 写足够长的 stereo silence。
 * 这不是给 TTS 音频“补静音文件”，而是专门用来覆盖 DMA 尾部缓存。
 */
static esp_err_t speaker_flush_tts_tail(i2s_chan_handle_t tx_chan)
{
    const size_t total_frames =
        ((size_t)TTS_SAMPLE_RATE * TTS_TAIL_SILENCE_MS) / 1000U;
    size_t frames_left = total_frames;

    ESP_LOGI(TAG,
             "Flush TTS tail with %u ms silence before clock restore",
             (unsigned)TTS_TAIL_SILENCE_MS);

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
            ESP_LOGE(TAG,
                     "write TTS tail silence failed: %s",
                     esp_err_to_name(err));
            return err;
        }

        frames_left -= frames;
    }

    /*
     * i2s_channel_write() 只保证数据交给驱动/DMA，不代表最后一个 sample 已经
     * 从引脚发完。再等一段 guard，让 DMA 中剩余的静音真正移出。
     */
    vTaskDelay(pdMS_TO_TICKS(TTS_DMA_DRAIN_GUARD_MS));

    ESP_LOGI(TAG,
             "TTS tail drained: silence=%u ms guard=%u ms",
             (unsigned)TTS_TAIL_SILENCE_MS,
             (unsigned)TTS_DMA_DRAIN_GUARD_MS);

    return ESP_OK;
}

/*
 * menuconfig 中粘贴 URL 时，尤其从浏览器/聊天窗口复制，前后偶尔会带：
 *   - 普通空格 / Tab / CR / LF
 *   - UTF-8 NBSP（C2 A0，不换行空格）
 *
 * esp_http_client 的 URL parser 对这些前缀非常严格，会直接报：
 *   Error parse url
 *   Failed to set URL
 *
 * 因此这里先做一层小型规范化：
 *   1. 跳过开头的 ASCII 空白和 UTF-8 NBSP；
 *   2. 从 http:// 或 https:// 开始复制；
 *   3. 遇到结尾空白即停止；
 *   4. 再检查 scheme，避免把坏配置误报成 ESP_ERR_NO_MEM。
 */
static bool is_ascii_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static const char *skip_leading_url_space(const char *p)
{
    while (*p != '\0') {
        const uint8_t c0 = (uint8_t)p[0];

        if (is_ascii_space(c0)) {
            ++p;
            continue;
        }

        /* UTF-8 NBSP = C2 A0 */
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

        /* URL 本身不应包含 NBSP；把它视作结尾空白。 */
        if (c0 == 0xC2U && (uint8_t)p[1] == 0xA0U) {
            break;
        }

        if (n + 1U >= output_size) {
            ESP_LOGE(TAG, "TTS URL is too long (max %u chars)",
                     (unsigned)(output_size - 1U));
            return ESP_ERR_INVALID_SIZE;
        }

        output[n++] = *p++;
    }

    output[n] = '\0';

    if (n == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static int append_json_escaped(char *dst,
                               size_t dst_size,
                               size_t *offset,
                               const char *src)
{
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; ++p) {
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
            size_t len = strlen(escape);
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
        "\",\"lang\":\"zh\",\"skip_refine_text\":true}";

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
        int16_t sample = (int16_t)((uint16_t)(*pending_byte)
                                   | ((uint16_t)data[0] << 8));
        s_stereo_output[0] = sample;
        s_stereo_output[1] = sample;
        frames = 1;
        index = 1;
        *pending_valid = false;
        *pcm_bytes_played += 2U;
    }

    while (index + 1U < size) {
        int16_t sample = (int16_t)((uint16_t)data[index]
                                   | ((uint16_t)data[index + 1U] << 8));
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

    const size_t stereo_bytes = frames * 2U * sizeof(int16_t);
    return i2s_write_all(tx_chan,
                         (const uint8_t *)s_stereo_output,
                         stereo_bytes);
}

esp_err_t tts_client_play(i2s_chan_handle_t tx_chan,
                          const char *server_url,
                          const char *text)
{
    if (tx_chan == NULL || server_url == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (server_url[0] == '\0' || text[0] == '\0') {
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
    err = build_request_json(text,
                             body,
                             sizeof(body),
                             &body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS request JSON too large");
        return err;
    }

    ESP_LOGI(TAG, "POST normalized URL: '%s'", normalized_url);
    ESP_LOGI(TAG, "TTS text: %s", text);

    esp_http_client_config_t config = {
        .url = normalized_url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_RX_BUFFER_SIZE,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        /*
         * esp_http_client_init() 可能因为 URL 解析失败等多种原因返回 NULL，
         * 不能简单解释成“内存不足”。底层 HTTP_CLIENT 日志会给出更具体原因。
         */
        ESP_LOGE(TAG,
                 "esp_http_client_init failed for URL '%s'",
                 normalized_url);
        return ESP_ERR_INVALID_ARG;
    }

    bool speaker_clock_changed = false;
    bool pending_valid = false;
    uint8_t pending_byte = 0;
    uint8_t rx_buffer[HTTP_RX_BUFFER_SIZE];
    size_t pcm_bytes_played = 0;
    size_t http_bytes_received = 0;
    int64_t request_started_us = esp_timer_get_time();
    int64_t first_pcm_us = 0;

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
        ESP_LOGE(TAG, "HTTP fetch headers failed: %lld", (long long)headers);
        err = ESP_FAIL;
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "TTS server returned HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    err = speaker_set_sample_rate(tx_chan, TTS_SAMPLE_RATE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    speaker_clock_changed = true;

    while (true) {
        int read_len = esp_http_client_read(client,
                                            (char *)rx_buffer,
                                            sizeof(rx_buffer));

        if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP PCM read failed: %d", read_len);
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
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

        err = play_pcm_bytes(tx_chan,
                             rx_buffer,
                             (size_t)read_len,
                             &pending_valid,
                             &pending_byte,
                             &pcm_bytes_played);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S PCM playback failed: %s", esp_err_to_name(err));
            break;
        }
    }

    if (err == ESP_OK && pending_valid) {
        /*
         * PCM16 合法流最终必须是偶数字节。如果结尾还残留 1 byte，说明响应损坏；
         * 不把半个 sample 写进喇叭。
         */
        ESP_LOGE(TAG, "PCM stream ended with one dangling byte");
        err = ESP_ERR_INVALID_SIZE;
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (speaker_clock_changed) {
        /*
         * 关键：不要在最后一包语音刚写入 DMA 后立刻切时钟。
         * 先用全 0 静音把 DMA 尾部覆盖并等待排空，再恢复 16kHz。
         */
        esp_err_t tail_err = speaker_flush_tts_tail(tx_chan);
        if (err == ESP_OK && tail_err != ESP_OK) {
            err = tail_err;
        }

        esp_err_t restore_err = speaker_set_sample_rate(tx_chan,
                                                        SPEAKER_IDLE_SAMPLE_RATE);
        if (err == ESP_OK && restore_err != ESP_OK) {
            err = restore_err;
        }
    }

    const int64_t finished_us = esp_timer_get_time();
    const double elapsed = (finished_us - request_started_us) / 1000000.0;
    const double audio_seconds = pcm_bytes_played / (24000.0 * 2.0);

    ESP_LOGI(TAG,
             "TTS finished: err=%s HTTP=%u bytes PCM=%u bytes audio=%.3fs wall=%.3fs",
             esp_err_to_name(err),
             (unsigned)http_bytes_received,
             (unsigned)pcm_bytes_played,
             audio_seconds,
             elapsed);

    return err;
}
