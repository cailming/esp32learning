#include "shutter_wav_player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "shutter_wav";

/*
 * 来自：EMBED_FILES "shutter.wav"
 *
 * 为了避免带子目录路径时不同 ESP-IDF 版本的符号命名差异，
 * 当前把 shutter.wav 放在 main 组件根目录，因此符号固定为：
 *
 *   _binary_shutter_wav_start
 *   _binary_shutter_wav_end
 */
extern const uint8_t shutter_wav_start[] asm("_binary_shutter_wav_start");
extern const uint8_t shutter_wav_end[]   asm("_binary_shutter_wav_end");

#define OUTPUT_SAMPLE_RATE 16000U
#define OUTPUT_FRAMES_PER_BLOCK 128U
#define OUTPUT_GAIN_PERCENT 70

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

static bool fourcc_is(const uint8_t *p, const char a, const char b, const char c, const char d)
{
    return p[0] == (uint8_t)a
           && p[1] == (uint8_t)b
           && p[2] == (uint8_t)c
           && p[3] == (uint8_t)d;
}

/*
 * WAV 并不保证 fmt/data chunk 紧挨着，所以这里按 RIFF chunk 逐个扫描。
 * 只接受最适合本项目的标准 PCM：16-bit / mono。
 */
static esp_err_t parse_wav(const uint8_t *data, size_t size, wav_info_t *info)
{
    if (data == NULL || info == NULL || size < 12) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fourcc_is(data, 'R', 'I', 'F', 'F') || !fourcc_is(data + 8, 'W', 'A', 'V', 'E')) {
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

        /* RIFF chunk 按 2-byte 对齐。 */
        offset = payload + chunk_size + (chunk_size & 1U);
    }

    if (!have_fmt || !have_data) {
        ESP_LOGE(TAG, "Invalid WAV: fmt/data chunk missing");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported WAV format=%u, only PCM(1) is supported",
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

    if (info->sample_rate == 0 || info->pcm_size < 2) {
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
    const size_t output_frames =
        ((uint64_t)source_samples * OUTPUT_SAMPLE_RATE) / wav.sample_rate;

    ESP_LOGI(TAG,
             "Play embedded WAV: %u Hz mono 16-bit, source=%u samples, output=%u frames",
             (unsigned)wav.sample_rate,
             (unsigned)source_samples,
             (unsigned)output_frames);

    size_t out_index = 0;

    while (out_index < output_frames) {
        size_t frames = output_frames - out_index;
        if (frames > OUTPUT_FRAMES_PER_BLOCK) {
            frames = OUTPUT_FRAMES_PER_BLOCK;
        }

        for (size_t i = 0; i < frames; ++i) {
            const size_t global_out = out_index + i;

            /*
             * 用整数线性插值把源 WAV 重采样到 16kHz。
             *
             * source_position = global_out * source_rate / output_rate
             */
            const uint64_t numerator =
                (uint64_t)global_out * (uint64_t)wav.sample_rate;
            size_t source_index = (size_t)(numerator / OUTPUT_SAMPLE_RATE);
            uint32_t fraction = (uint32_t)(numerator % OUTPUT_SAMPLE_RATE);

            if (source_index >= source_samples) {
                source_index = source_samples - 1;
            }

            size_t next_index = source_index + 1;
            if (next_index >= source_samples) {
                next_index = source_index;
            }

            int32_t s0 = pcm16_at(wav.pcm, source_index);
            int32_t s1 = pcm16_at(wav.pcm, next_index);
            int32_t mixed =
                s0 + ((s1 - s0) * (int32_t)fraction) / (int32_t)OUTPUT_SAMPLE_RATE;

            int16_t sample = apply_gain(mixed);

            /* MAX98357A 当前按 stereo framing 发送，左右声道写相同样本。 */
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
            return err;
        }

        if (bytes_written != bytes_to_write) {
            ESP_LOGE(TAG,
                     "Partial I2S write: %u/%u bytes",
                     (unsigned)bytes_written,
                     (unsigned)bytes_to_write);
            return ESP_FAIL;
        }

        out_index += frames;
    }

    return ESP_OK;
}
