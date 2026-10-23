/*
 * ESP32-S3-CAM MAX98357A + 喇叭独立测试
 *
 * 当前接线：
 *   MAX98357A BCLK -> GPIO14
 *   MAX98357A LRC  -> GPIO3
 *   MAX98357A DIN  -> GPIO46
 *   MAX98357A VIN  -> 5V
 *   MAX98357A GND  -> 公共 GND
 *   MAX98357A SD / GAIN 暂不接
 *
 * 喇叭：
 *   红线 -> MAX98357A +
 *   黑线 -> MAX98357A -
 *
 * MIC 保持当前接线，但本程序不会初始化 GPIO42 / GPIO41 / GPIO1。
 */

#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "speaker_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "speaker_test";

#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_I2S_BCLK       GPIO_NUM_14
#define SPK_I2S_WS         GPIO_NUM_3
#define SPK_I2S_DOUT       GPIO_NUM_46
#define SPK_SAMPLE_RATE_HZ 16000

/* 音量刻意压低，先验证接线，避免第一次测试突然很响。 */
#define TONE_AMPLITUDE 2500
#define FRAMES_PER_BLOCK 256

static i2s_chan_handle_t s_tx_chan;
static int16_t s_audio[FRAMES_PER_BLOCK * 2];

static void init_i2s_speaker(void)
{
    /*
     * 固定使用 I2S1 做 TX，与 mic_test 规划的 I2S0/RX 分开。
     * 以后做 MIC + Speaker 联调时可以直接复用这个结构。
     */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_I2S_BCLK,
            .ws = SPK_I2S_WS,
            .dout = SPK_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
}

static int16_t make_triangle_sample(uint32_t position, uint32_t period)
{
    /*
     * 用三角波代替正弦波，避免为了一个硬件冒烟测试再引入浮点/三角函数。
     * 左右声道发送完全相同的数据，MAX98357A 无论当前模式取哪一边都能听到声音。
     */
    int32_t sample;

    if (position < period / 2U) {
        sample = -TONE_AMPLITUDE
                 + (int32_t)((4ULL * TONE_AMPLITUDE * position) / period);
    } else {
        sample = 3 * TONE_AMPLITUDE
                 - (int32_t)((4ULL * TONE_AMPLITUDE * position) / period);
    }

    return (int16_t)sample;
}

static void write_frames(size_t frame_count)
{
    size_t bytes_to_write = frame_count * 2U * sizeof(int16_t);
    size_t bytes_written = 0;

    esp_err_t err = i2s_channel_write(
        s_tx_chan,
        s_audio,
        bytes_to_write,
        &bytes_written,
        1000);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPEAKER TEST FAIL: i2s_channel_write(): %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(err);
    }

    if (bytes_written != bytes_to_write) {
        ESP_LOGW(TAG, "Partial I2S write: %u/%u bytes",
                 (unsigned)bytes_written, (unsigned)bytes_to_write);
    }
}

static void play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    uint32_t period = SPK_SAMPLE_RATE_HZ / frequency_hz;
    if (period < 2U) {
        period = 2U;
    }

    size_t total_frames = ((size_t)SPK_SAMPLE_RATE_HZ * duration_ms) / 1000U;
    size_t sent_frames = 0;
    uint32_t phase = 0;

    ESP_LOGI(TAG, "Play %u Hz for %u ms", (unsigned)frequency_hz, (unsigned)duration_ms);

    while (sent_frames < total_frames) {
        size_t frames = total_frames - sent_frames;
        if (frames > FRAMES_PER_BLOCK) {
            frames = FRAMES_PER_BLOCK;
        }

        for (size_t i = 0; i < frames; ++i) {
            int16_t sample = make_triangle_sample(phase % period, period);
            s_audio[i * 2] = sample;       // LEFT
            s_audio[i * 2 + 1] = sample;   // RIGHT
            ++phase;
        }

        write_frames(frames);
        sent_frames += frames;
    }
}

static void play_silence(uint32_t duration_ms)
{
    memset(s_audio, 0, sizeof(s_audio));

    size_t total_frames = ((size_t)SPK_SAMPLE_RATE_HZ * duration_ms) / 1000U;
    size_t sent_frames = 0;

    while (sent_frames < total_frames) {
        size_t frames = total_frames - sent_frames;
        if (frames > FRAMES_PER_BLOCK) {
            frames = FRAMES_PER_BLOCK;
        }

        write_frames(frames);
        sent_frames += frames;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM MAX98357A SPEAKER TEST");
    ESP_LOGI(TAG, "MIC 保持当前接线，不需要拔线");
    ESP_LOGI(TAG, "Speaker: BCLK=%d LRC=%d DOUT=%d",
             (int)SPK_I2S_BCLK, (int)SPK_I2S_WS, (int)SPK_I2S_DOUT);
    ESP_LOGI(TAG, "测试音量已刻意降低");
    ESP_LOGI(TAG, "========================================");

    init_i2s_speaker();
    ESP_LOGI(TAG, "I2S TX initialized OK");

    for (int round = 1; round <= 3; ++round) {
        ESP_LOGI(TAG, "Tone round %d/3", round);
        play_tone(440, 800);
        play_silence(400);
        play_tone(880, 800);
        play_silence(600);
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SPEAKER TEST PASS: I2S data sent successfully");
    ESP_LOGI(TAG, "请以实际听到 440Hz / 880Hz 两种音高作为最终硬件通过标准");
    ESP_LOGI(TAG, "========================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
