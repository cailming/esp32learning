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
 * ESP-IDF 会生成：
 *   _binary_shutter_wav_start
 *   _binary_shutter_wav_end
 */
extern const uint8_t shutter_wav_start[] asm("_binary_shutter_wav_start");
extern const uint8_t shutter_wav_end[]   asm("_binary_shutter_wav_end");

/*
 * 当前真实 shutter.wav 的原始格式：44100 Hz / mono / 16-bit PCM。
 *
 * MIC 的 I2S0 仍固定使用 16kHz 给 ESP-SR；Speaker 使用独立的 I2S1。
 * 播放 WAV 前，我们把 I2S1 从平时的 16kHz 临时切到 44.1kHz，
 * 原样播放真实 PCM，结束后再恢复 16kHz。
 *
 * 这样完全避免 44.1kHz -> 16kHz 的实时降采样混叠，快门里的高频瞬态
 * 不会再被扭曲成爆裂/“机关枪”听感。
 */
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

/*
 * RIFF/WAV 的 fmt/data chunk 不保证紧挨着，因此逐个扫描 chunk。
 * 当前只接受本工程实际使用的标准 PCM：mono / 16-bit / 44.1kHz。
 */
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

        /* RIFF chunk 按 2-byte 对齐。 */
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

/*
 * 标准 I2S 的时钟只能在 channel 停止状态下重新配置。
 * 因此流程严格是：disable -> reconfig clock -> enable。
 */
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
        /* 尽量把 channel 恢复 enable，避免后续完全无声。 */
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

    /*
     * 关键修正：让 I2S1 跟随 WAV 的原始 44.1kHz，完全取消软件重采样。
     */
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

            /* WAV 是 mono；I2S 使用 stereo framing，左右复制同一个样本。 */
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

    /*
     * 播放完成后把 I2S1 恢复到工程原来的 16kHz 空闲配置。
     * 即使播放中途失败，也尽量执行恢复。
     */
    esp_err_t restore_err = speaker_set_sample_rate(tx_chan,
                                                    SPEAKER_IDLE_SAMPLE_RATE);
    if (err == ESP_OK && restore_err != ESP_OK) {
        err = restore_err;
    }

    return err;
}
