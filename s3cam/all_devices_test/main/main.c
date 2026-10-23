/*
 * ESP32-S3-CAM Camera + MIC + Speaker 三设备并行综合测试
 *
 * 前提：camera_test、mic_test、speaker_test 已分别通过。
 *
 * 本程序重点验证：
 * 1. Camera、I2S0 RX、I2S1 TX 能否同时初始化；
 * 2. 三个 FreeRTOS 任务能否同时持续工作；
 * 3. 是否存在 GPIO / DMA / 内存 / 外设资源冲突；
 * 4. 在全部接线保持不动的情况下，ESP32-S3 是否稳定运行。
 *
 * 通过标准：
 * - Camera 连续抓取 30 帧成功；
 * - MIC 在综合测试期间持续读到非零 PCM 数据；
 * - Speaker I2S 数据持续写出成功；
 * - 无重启、崩溃和外设错误；
 * - 串口最终打印 "ALL DEVICES TEST PASS"。
 */

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "all_devices_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "all_test";

/* ============================================================
 * Camera GPIO
 * ============================================================ */
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D0     11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

/* ============================================================
 * MIC: I2S0 RX
 * ============================================================ */
#define MIC_I2S_PORT       I2S_NUM_0
#define MIC_I2S_BCLK       GPIO_NUM_42
#define MIC_I2S_WS         GPIO_NUM_41
#define MIC_I2S_DIN        GPIO_NUM_1
#define MIC_SAMPLE_RATE_HZ 16000
#define MIC_FRAMES_PER_READ 256
#define MIC_WORDS_PER_READ  (MIC_FRAMES_PER_READ * 2)

/* ============================================================
 * Speaker: I2S1 TX
 * ============================================================ */
#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_I2S_BCLK       GPIO_NUM_14
#define SPK_I2S_WS         GPIO_NUM_3
#define SPK_I2S_DOUT       GPIO_NUM_46
#define SPK_SAMPLE_RATE_HZ 16000
#define TONE_AMPLITUDE     2500
#define SPK_FRAMES_PER_BLOCK 256

/* ============================================================
 * 综合测试参数
 * ============================================================ */
#define CAMERA_TEST_FRAMES 30
#define CAMERA_WARMUP_FRAMES 2
#define TEST_TIMEOUT_MS    20000

static i2s_chan_handle_t s_mic_rx_chan;
static i2s_chan_handle_t s_spk_tx_chan;

static int32_t s_mic_samples[MIC_WORDS_PER_READ];
static int16_t s_spk_audio[SPK_FRAMES_PER_BLOCK * 2];

/*
 * 下面这些统计值只做测试状态汇总。
 * 每个字段只有一个任务负责写，主任务负责读，避免复杂同步逻辑。
 */
static volatile uint32_t s_camera_frames_ok = 0;
static volatile uint32_t s_mic_reads_ok = 0;
static volatile uint32_t s_speaker_blocks_ok = 0;

static volatile bool s_camera_done = false;
static volatile bool s_speaker_done = false;
static volatile bool s_mic_has_signal = false;
static volatile bool s_test_failed = false;
static volatile bool s_stop_test = false;

/* ============================================================
 * Camera 初始化
 * ============================================================ */
static esp_err_t init_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        /*
         * 与已经通过的 camera_test 保持一致：
         * QVGA + JPEG + 单个 DRAM framebuffer。
         * 综合测试先追求稳定，不追求高分辨率。
         */
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 15,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    return esp_camera_init(&config);
}

/* ============================================================
 * MIC 初始化：I2S0 RX
 * ============================================================ */
static esp_err_t init_mic(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_mic_rx_chan);
    if (err != ESP_OK) {
        return err;
    }

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

    err = i2s_channel_init_std_mode(s_mic_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_mic_rx_chan);
}

/* ============================================================
 * Speaker 初始化：I2S1 TX
 * ============================================================ */
static esp_err_t init_speaker(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_spk_tx_chan, NULL);
    if (err != ESP_OK) {
        return err;
    }

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

    err = i2s_channel_init_std_mode(s_spk_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_spk_tx_chan);
}

/* ============================================================
 * Camera Task
 * ============================================================ */
static void camera_task(void *arg)
{
    /*
     * 初始化刚完成时可能偶尔遇到 NO-SOI 的预热帧。
     * 单项测试中已经看到后续帧完全稳定，所以综合测试先丢弃 2 帧预热帧。
     */
    for (int i = 0; i < CAMERA_WARMUP_FRAMES && !s_stop_test; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb != NULL) {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    for (int i = 1; i <= CAMERA_TEST_FRAMES && !s_stop_test; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGE(TAG, "[CAM] FAIL: frame %d returned NULL", i);
            s_test_failed = true;
            break;
        }

        bool valid = (fb->buf != NULL) &&
                     (fb->len > 0) &&
                     (fb->width > 0) &&
                     (fb->height > 0);

        if (!valid) {
            ESP_LOGE(TAG, "[CAM] FAIL: invalid frame %d", i);
            esp_camera_fb_return(fb);
            s_test_failed = true;
            break;
        }

        ++s_camera_frames_ok;

        /* 每 5 帧打印一次，避免日志过多影响并行测试。 */
        if (i == 1 || (i % 5) == 0 || i == CAMERA_TEST_FRAMES) {
            ESP_LOGI(TAG,
                     "[CAM] frame %02d/%d: %ux%u len=%u OK",
                     i,
                     CAMERA_TEST_FRAMES,
                     (unsigned)fb->width,
                     (unsigned)fb->height,
                     (unsigned)fb->len);
        }

        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_camera_done = true;
    ESP_LOGI(TAG, "[CAM] task finished: %u/%d frames OK",
             (unsigned)s_camera_frames_ok,
             CAMERA_TEST_FRAMES);
    vTaskDelete(NULL);
}

/* ============================================================
 * MIC Task
 * ============================================================ */
static void mic_task(void *arg)
{
    while (!s_stop_test) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_mic_rx_chan,
            s_mic_samples,
            sizeof(s_mic_samples),
            &bytes_read,
            1000);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[MIC] FAIL: i2s_channel_read(): %s", esp_err_to_name(err));
            s_test_failed = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t word_count = bytes_read / sizeof(int32_t);
        if (word_count < 2) {
            continue;
        }

        int32_t min_l = INT32_MAX;
        int32_t max_l = INT32_MIN;
        int32_t min_r = INT32_MAX;
        int32_t max_r = INT32_MIN;

        for (size_t i = 0; i + 1 < word_count; i += 2) {
            int32_t left = s_mic_samples[i];
            int32_t right = s_mic_samples[i + 1];

            if (left < min_l) min_l = left;
            if (left > max_l) max_l = left;
            if (right < min_r) min_r = right;
            if (right > max_r) max_r = right;
        }

        uint64_t span_l = (uint64_t)((int64_t)max_l - (int64_t)min_l);
        uint64_t span_r = (uint64_t)((int64_t)max_r - (int64_t)min_r);
        uint64_t active_span = (span_l >= span_r) ? span_l : span_r;

        ++s_mic_reads_ok;

        /*
         * 单项测试已经验证 LEFT 是有效声道。
         * 综合测试只需要确认并发运行时仍持续收到非零 PCM。
         */
        if (active_span > 1000ULL) {
            s_mic_has_signal = true;
        }

        if ((s_mic_reads_ok % 10U) == 0U) {
            ESP_LOGI(TAG,
                     "[MIC] reads=%u L_span=%" PRIu64 " R_span=%" PRIu64 " active=%" PRIu64,
                     (unsigned)s_mic_reads_ok,
                     span_l,
                     span_r,
                     active_span);
        }
    }

    ESP_LOGI(TAG, "[MIC] task finished: %u reads OK, signal=%s",
             (unsigned)s_mic_reads_ok,
             s_mic_has_signal ? "YES" : "NO");
    vTaskDelete(NULL);
}

/* ============================================================
 * Speaker 波形和写入
 * ============================================================ */
static int16_t make_triangle_sample(uint32_t position, uint32_t period)
{
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

static bool speaker_write_frames(size_t frame_count)
{
    size_t bytes_to_write = frame_count * 2U * sizeof(int16_t);
    size_t bytes_written = 0;

    esp_err_t err = i2s_channel_write(
        s_spk_tx_chan,
        s_spk_audio,
        bytes_to_write,
        &bytes_written,
        1000);

    if (err != ESP_OK || bytes_written != bytes_to_write) {
        ESP_LOGE(TAG,
                 "[SPK] FAIL: write err=%s bytes=%u/%u",
                 esp_err_to_name(err),
                 (unsigned)bytes_written,
                 (unsigned)bytes_to_write);
        s_test_failed = true;
        return false;
    }

    ++s_speaker_blocks_ok;
    return true;
}

static bool speaker_play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    uint32_t period = SPK_SAMPLE_RATE_HZ / frequency_hz;
    if (period < 2U) {
        period = 2U;
    }

    size_t total_frames = ((size_t)SPK_SAMPLE_RATE_HZ * duration_ms) / 1000U;
    size_t sent_frames = 0;
    uint32_t phase = 0;

    while (sent_frames < total_frames && !s_stop_test) {
        size_t frames = total_frames - sent_frames;
        if (frames > SPK_FRAMES_PER_BLOCK) {
            frames = SPK_FRAMES_PER_BLOCK;
        }

        for (size_t i = 0; i < frames; ++i) {
            int16_t sample = make_triangle_sample(phase % period, period);
            s_spk_audio[i * 2] = sample;
            s_spk_audio[i * 2 + 1] = sample;
            ++phase;
        }

        if (!speaker_write_frames(frames)) {
            return false;
        }

        sent_frames += frames;
    }

    return true;
}

static bool speaker_play_silence(uint32_t duration_ms)
{
    memset(s_spk_audio, 0, sizeof(s_spk_audio));

    size_t total_frames = ((size_t)SPK_SAMPLE_RATE_HZ * duration_ms) / 1000U;
    size_t sent_frames = 0;

    while (sent_frames < total_frames && !s_stop_test) {
        size_t frames = total_frames - sent_frames;
        if (frames > SPK_FRAMES_PER_BLOCK) {
            frames = SPK_FRAMES_PER_BLOCK;
        }

        if (!speaker_write_frames(frames)) {
            return false;
        }

        sent_frames += frames;
    }

    return true;
}

/* ============================================================
 * Speaker Task
 * ============================================================ */
static void speaker_task(void *arg)
{
    for (int round = 1; round <= 3 && !s_stop_test; ++round) {
        ESP_LOGI(TAG, "[SPK] round %d/3: 440 Hz", round);
        if (!speaker_play_tone(440, 800)) {
            break;
        }

        if (!speaker_play_silence(400)) {
            break;
        }

        ESP_LOGI(TAG, "[SPK] round %d/3: 880 Hz", round);
        if (!speaker_play_tone(880, 800)) {
            break;
        }

        if (!speaker_play_silence(600)) {
            break;
        }
    }

    s_speaker_done = true;
    ESP_LOGI(TAG, "[SPK] task finished: %u I2S blocks OK",
             (unsigned)s_speaker_blocks_ok);
    vTaskDelete(NULL);
}

/* ============================================================
 * 主程序
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3-CAM ALL DEVICES CONCURRENT TEST");
    ESP_LOGI(TAG, "Camera + MIC(I2S0 RX) + Speaker(I2S1 TX)");
    ESP_LOGI(TAG, "所有接线保持不动，三个设备将真正并行运行");
    ESP_LOGI(TAG, "====================================================");

    /* 1. Camera */
    esp_err_t err = init_camera();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAIL: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        return;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        ESP_LOGI(TAG, "Camera init OK: PID=0x%04x VER=0x%02x",
                 sensor->id.PID,
                 sensor->id.VER);
    } else {
        ESP_LOGI(TAG, "Camera init OK");
    }

    /* 2. MIC */
    err = init_mic();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MIC init FAIL: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "MIC init OK: I2S0 RX BCLK=%d WS=%d DIN=%d",
             (int)MIC_I2S_BCLK,
             (int)MIC_I2S_WS,
             (int)MIC_I2S_DIN);

    /* 3. Speaker */
    err = init_speaker();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker init FAIL: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Speaker init OK: I2S1 TX BCLK=%d LRC=%d DOUT=%d",
             (int)SPK_I2S_BCLK,
             (int)SPK_I2S_WS,
             (int)SPK_I2S_DOUT);

    ESP_LOGI(TAG, "All three devices initialized OK. Starting concurrent tasks...");

    BaseType_t cam_task_ok = xTaskCreate(
        camera_task,
        "camera_test_task",
        4096,
        NULL,
        5,
        NULL);

    BaseType_t mic_task_ok = xTaskCreate(
        mic_task,
        "mic_test_task",
        4096,
        NULL,
        6,
        NULL);

    BaseType_t spk_task_ok = xTaskCreate(
        speaker_task,
        "speaker_test_task",
        4096,
        NULL,
        5,
        NULL);

    if (cam_task_ok != pdPASS || mic_task_ok != pdPASS || spk_task_ok != pdPASS) {
        ESP_LOGE(TAG, "FAIL: unable to create one or more test tasks");
        s_stop_test = true;
        return;
    }

    /*
     * 等待 Camera 和 Speaker 都完成。
     * MIC 在这段时间一直持续采样，所以三者存在真实并行重叠。
     */
    uint32_t waited_ms = 0;
    while (!(s_camera_done && s_speaker_done) && waited_ms < TEST_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }

    if (!(s_camera_done && s_speaker_done)) {
        ESP_LOGE(TAG, "FAIL: test timeout after %u ms", (unsigned)waited_ms);
        s_test_failed = true;
    }

    /* 停止 MIC 任务，并给它一点时间退出。 */
    s_stop_test = true;
    vTaskDelay(pdMS_TO_TICKS(300));

    bool camera_pass = (s_camera_frames_ok == CAMERA_TEST_FRAMES);
    bool mic_pass = (s_mic_reads_ok > 0U) && s_mic_has_signal;
    bool speaker_pass = (s_speaker_blocks_ok > 0U) && s_speaker_done;

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "CONCURRENT TEST SUMMARY");
    ESP_LOGI(TAG, "Camera : %s (%u/%d frames)",
             camera_pass ? "PASS" : "FAIL",
             (unsigned)s_camera_frames_ok,
             CAMERA_TEST_FRAMES);
    ESP_LOGI(TAG, "MIC    : %s (%u reads, signal=%s)",
             mic_pass ? "PASS" : "FAIL",
             (unsigned)s_mic_reads_ok,
             s_mic_has_signal ? "YES" : "NO");
    ESP_LOGI(TAG, "Speaker: %s (%u I2S blocks written)",
             speaker_pass ? "PASS" : "FAIL",
             (unsigned)s_speaker_blocks_ok);
    ESP_LOGI(TAG, "====================================================");

    if (!s_test_failed && camera_pass && mic_pass && speaker_pass) {
        ESP_LOGI(TAG, "ALL DEVICES TEST PASS");
        ESP_LOGI(TAG, "Camera + MIC + Speaker can run concurrently.");
        ESP_LOGI(TAG, "另外请人工确认测试期间确实听到了 440/880 Hz 两种音高。");
    } else {
        ESP_LOGE(TAG, "ALL DEVICES TEST FAIL");
    }

    ESP_LOGI(TAG, "====================================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
