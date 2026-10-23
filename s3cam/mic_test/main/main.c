/*
 * ESP32-S3-CAM I2S 数字麦克风独立测试
 *
 * 当前接线：
 *   MIC SCK  -> GPIO42
 *   MIC WS   -> GPIO41
 *   MIC SD   -> GPIO1
 *   MIC VDD  -> 3V3
 *   MIC GND  -> 公共 GND
 *   MIC L/R  -> 公共 GND
 *
 * MAX98357A 和喇叭保持连接，但本程序不会初始化 GPIO14 / GPIO3 / GPIO46。
 *
 * 测试方法：
 * 1. 烧录后先保持环境安静约 2 秒，建立噪声基线；
 * 2. 拍手或说话；
 * 3. 当声音幅度明显高于基线时打印 "MIC TEST PASS"。
 */

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "mic_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "mic_test";

#define MIC_I2S_PORT       I2S_NUM_0
#define MIC_I2S_BCLK       GPIO_NUM_42
#define MIC_I2S_WS         GPIO_NUM_41
#define MIC_I2S_DIN        GPIO_NUM_1
#define MIC_SAMPLE_RATE_HZ 16000

/* 每次读取 256 个立体声 frame = 512 个 int32_t word。 */
#define MIC_FRAMES_PER_READ 256
#define MIC_WORDS_PER_READ  (MIC_FRAMES_PER_READ * 2)

/* 先用前 15 个数据块建立安静环境基线。 */
#define BASELINE_BLOCKS 15

static i2s_chan_handle_t s_rx_chan;
static int32_t s_samples[MIC_WORDS_PER_READ];

static void print_level_bar(uint64_t span)
{
    /*
     * 这里只是为了让串口上更直观，不代表真正的 dB 值。
     * 32 位 I2S 麦克风的有效数据通常位于高位，因此按 2^22 粗略缩放。
     */
    int bars = (int)(span >> 22);
    if (bars < 1 && span > 0) {
        bars = 1;
    }
    if (bars > 40) {
        bars = 40;
    }

    printf("level: ");
    for (int i = 0; i < bars; ++i) {
        putchar('#');
    }
    putchar('\n');
}

static void init_i2s_mic(void)
{
    /*
     * 固定使用 I2S0 做 RX。
     * 后续做 MIC + Speaker 联调时，可以把 Speaker 放到 I2S1，保持结构清晰。
     */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    /*
     * 常见 INMP441 一类 I2S 麦克风使用 Philips I2S 格式，
     * 每个 slot 用 32 bit 接收最稳妥。
     *
     * 使用 STEREO 的原因：L/R 已接 GND，但不同麦克风模块/驱动组合下
     * 有效数据具体落在 LEFT 还是 RIGHT 更适合由程序直接观察，
     * 因此这里两个声道都接收，不在代码里武断写死。
     */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK,
            .ws = MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM I2S MIC TEST");
    ESP_LOGI(TAG, "MAX98357A / Speaker 保持接线，不需要拔线");
    ESP_LOGI(TAG, "MIC: BCLK=%d WS=%d DIN=%d",
             (int)MIC_I2S_BCLK, (int)MIC_I2S_WS, (int)MIC_I2S_DIN);
    ESP_LOGI(TAG, "========================================");

    init_i2s_mic();
    ESP_LOGI(TAG, "I2S RX initialized OK");
    ESP_LOGI(TAG, "请先保持安静约 2 秒，然后拍手或说话");

    uint64_t baseline_sum = 0;
    uint64_t baseline = 0;
    int baseline_count = 0;
    bool pass_reported = false;

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_rx_chan,
            s_samples,
            sizeof(s_samples),
            &bytes_read,
            1000);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MIC TEST FAIL: i2s_channel_read(): %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t word_count = bytes_read / sizeof(int32_t);
        if (word_count < 2) {
            ESP_LOGW(TAG, "No complete stereo frame received: %u bytes", (unsigned)bytes_read);
            continue;
        }

        int32_t min_l = INT32_MAX;
        int32_t max_l = INT32_MIN;
        int32_t min_r = INT32_MAX;
        int32_t max_r = INT32_MIN;

        /* 立体声数据按 L, R, L, R... 交错排列。 */
        for (size_t i = 0; i + 1 < word_count; i += 2) {
            int32_t left = s_samples[i];
            int32_t right = s_samples[i + 1];

            if (left < min_l) min_l = left;
            if (left > max_l) max_l = left;
            if (right < min_r) min_r = right;
            if (right > max_r) max_r = right;
        }

        uint64_t span_l = (uint64_t)((int64_t)max_l - (int64_t)min_l);
        uint64_t span_r = (uint64_t)((int64_t)max_r - (int64_t)min_r);
        uint64_t active_span = span_l >= span_r ? span_l : span_r;
        const char *active_channel = span_l >= span_r ? "LEFT" : "RIGHT";

        if (baseline_count < BASELINE_BLOCKS) {
            baseline_sum += active_span;
            ++baseline_count;

            ESP_LOGI(TAG,
                     "Baseline %02d/%d: L_span=%" PRIu64 " R_span=%" PRIu64 " active=%s",
                     baseline_count, BASELINE_BLOCKS,
                     span_l, span_r, active_channel);

            if (baseline_count == BASELINE_BLOCKS) {
                baseline = baseline_sum / BASELINE_BLOCKS;
                ESP_LOGI(TAG, "Quiet baseline = %" PRIu64, baseline);
                ESP_LOGI(TAG, "现在请拍手或正常说话");
            }
        } else {
            ESP_LOGI(TAG,
                     "Audio: L_span=%" PRIu64 " R_span=%" PRIu64
                     " active=%s span=%" PRIu64 " baseline=%" PRIu64,
                     span_l, span_r, active_channel, active_span, baseline);
            print_level_bar(active_span);

            /*
             * 判断条件不是音质指标，只用于快速验证“声音变化确实进入 ESP32”。
             * 要求当前幅度至少达到安静基线的约 3 倍，并留一点绝对裕量。
             */
            uint64_t trigger = baseline * 3ULL + 250000ULL;
            if (!pass_reported && active_span > trigger) {
                ESP_LOGI(TAG, "========================================");
                ESP_LOGI(TAG, "MIC TEST PASS: detected clear audio level change");
                ESP_LOGI(TAG, "Active channel: %s", active_channel);
                ESP_LOGI(TAG, "========================================");
                pass_reported = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
