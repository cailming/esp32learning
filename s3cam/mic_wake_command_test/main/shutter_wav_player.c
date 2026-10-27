#include "shutter_wav_player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "shutter_wav";

extern const uint8_t shutter_wav_start[] asm("_binary_shutter_wav_start");
extern const uint8_t shutter_wav_end[]   asm("_binary_shutter_wav_end");

#define SHUTTER_SAMPLE_RATE        44100U
#define SPEAKER_IDLE_SAMPLE_RATE   16000U
#define OUTPUT_FRAMES_PER_BLOCK   128U
#define OUTPUT_GAIN_PERCENT       70

static int16_t s_output[OUTPUT_FRAMES_PER_BLOCK * 2U];

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t *pcm;
    size_t pcm_size;
} wav_info_t;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
           | ((uint32_t)p[1] << 8)
           | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static bool fourcc_is(const uint8_t *p,
                      const char a,
                      const char b,
                      const char c,
                      const char d)
{
    return p[0] == (uint8_t)a
           && p[1] == (uint8_t)b
           && p[2] == (uint8_t)c
           && p[3] == (uint8_t)d;
}

static esp_err_t parse_wav(const uint8_t *data, size_t size, wav_info_t *info)
{
    if (data == NULL || info == NULL || size < 12) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fourcc_is(data, 'R', 'I', 'F', 'F')
        || !fourcc_is(data + 8, 'W', 'A', 'V', 'E')) {
        ESP_LOGE(TAG, "Invalid WAV: RIFF/WAVE header not found");
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_fmt = false;
    bool have_data = false;
    size_t offset = 12;

    while (offset + 8 <= size) {
        const uint8_t *chunk = data + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t payload = offset + 8;

        if (payload + chunk_size > size) {
            ESP_LOGE(TAG, "Invalid WAV: chunk exceeds file size");
            return ESP_ERR_INVALID_SIZE;
        }

        if (fourcc_is(chunk, 'f', 'm', 't', ' ')) {
            if (chunk_size < 16) {
                return ESP_ERR_INVALID_SIZE;
            }

            info->audio_format = read_le16(data + payload + 0);
            info->channels = read_le16(data + payload + 2);
            info->sample_rate = read_le32(data + payload + 4);
            info->bits_per_sample = read_le16(data + payload + 14);
            have_fmt = true;
        } else if (fourcc_is(chunk, 'd', 'a', 't', 'a')) {
            info->pcm = data + payload;
            info->pcm_size = chunk_size;
            have_data = true;
        }

        offset = payload + chunk_size + (chunk_size & 1U);
    }

    if (!have_fmt || !have_data) {
        ESP_LOGE(TAG, "Invalid WAV: fmt/data chunk missing");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->audio_format != 1) {
        ESP_LOGE(TAG,
                 "Unsupported WAV format=%u, only PCM(1) is supported",
                 (unsigned)info->audio_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->channels != 1 || info->bits_per_sample != 16) {
        ESP_LOGE(TAG,
                 "Unsupported WAV: channels=%u bits=%u, require mono/16-bit",
                 (unsigned)info->channels,
                 (unsigned)info->bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->sample_rate != SHUTTER_SAMPLE_RATE) {
        ESP_LOGE(TAG,
                 "Unexpected WAV sample rate=%u, expected %u Hz",
                 (unsigned)info->sample_rate,
                 (unsigned)SHUTTER_SAMPLE_RATE);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->pcm_size < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static int16_t pcm16_at(const uint8_t *pcm, size_t sample_index)
{
    const uint8_t *p = pcm + sample_index * 2U;
    return (int16_t)read_le16(p);
}

static int16_t apply_gain(int32_t sample)
{
    sample = (sample * OUTPUT_GAIN_PERCENT) / 100;

    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }

    return (int16_t)sample;
}

static esp_err_t speaker_set_sample_rate(i2s_chan_handle_t tx_chan,
                                         uint32_t sample_rate)
{
    /*
     * ChatTTS 播放结束后会把 I2S1 保持 disabled，因此这里允许“已经 disabled”。
     */
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

esp_err_t shutter_wav_play(i2s_chan_handle_t tx_chan)
{
    if (tx_chan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t wav_size = (size_t)(shutter_wav_end - shutter_wav_start);
    wav_info_t wav = {0};

    esp_err_t err = parse_wav(shutter_wav_start, wav_size, &wav);
    if (err != ESP_OK) {
        return err;
    }

    const size_t source_samples = wav.pcm_size / sizeof(int16_t);

    ESP_LOGI(TAG,
             "Play embedded WAV natively: %u Hz mono 16-bit, %u samples",
             (unsigned)wav.sample_rate,
             (unsigned)source_samples);

    err = speaker_set_sample_rate(tx_chan, wav.sample_rate);
    if (err != ESP_OK) {
        return err;
    }

    size_t source_index = 0;

    while (source_index < source_samples) {
        size_t frames = source_samples - source_index;
        if (frames > OUTPUT_FRAMES_PER_BLOCK) {
            frames = OUTPUT_FRAMES_PER_BLOCK;
        }

        for (size_t i = 0; i < frames; ++i) {
            int16_t sample = apply_gain(
                pcm16_at(wav.pcm, source_index + i));

            s_output[i * 2U] = sample;
            s_output[i * 2U + 1U] = sample;
        }

        const size_t bytes_to_write = frames * 2U * sizeof(int16_t);
        size_t bytes_written = 0;

        err = i2s_channel_write(tx_chan,
                                s_output,
                                bytes_to_write,
                                &bytes_written,
                                pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            break;
        }

        if (bytes_written != bytes_to_write) {
            ESP_LOGE(TAG,
                     "Partial I2S write: %u/%u bytes",
                     (unsigned)bytes_written,
                     (unsigned)bytes_to_write);
            err = ESP_FAIL;
            break;
        }

        source_index += frames;
    }

    esp_err_t restore_err = speaker_set_sample_rate(tx_chan,
                                                    SPEAKER_IDLE_SAMPLE_RATE);
    if (err == ESP_OK && restore_err != ESP_OK) {
        err = restore_err;
    }

    return err;
}
