/*
 * ESP32-S3-CAM：真实 Camera 拍照 + INMP441 语音唤醒 + 中文命令 + WAV 快门音
 *
 * 当前完整“拍照”链路：
 *
 *   “你好小智”
 *       ↓
 *   WakeNet10 唤醒
 *       ↓
 *   “拍照”
 *       ↓
 *   MultiNet6 命令识别
 *       ↓
 *   camera_capture_one()
 *       ↓
 *   OV3660 真正抓取 QVGA JPEG
 *       ↓
 *   JPEG 校验并复制为“最新照片”
 *       ↓
 *   拍照成功后播放 shutter.wav
 *       ↓
 *   重新等待“你好小智”
 *
 * 已验证硬件接线：
 *
 *   Camera：板载 OV3660，GPIO 见 camera_capture.c
 *   INMP441 SCK/BCLK -> GPIO42
 *   INMP441 WS/LRCLK -> GPIO41
 *   INMP441 SD       -> GPIO1
 *   INMP441 L/R      -> GND（LEFT channel）
 *
 *   MAX98357A BCLK   -> GPIO14
 *   MAX98357A LRC    -> GPIO3
 *   MAX98357A DIN    -> GPIO46
 *   MAX98357A VIN    -> 5V
 *   MAX98357A GND    -> 公共 GND
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-SR：AFE / WakeNet / MultiNet */
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "camera_capture.h"
#include "shutter_wav_player.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "mic_wake_command_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "voice_cmd";

/* ========================================================================
 * 1. INMP441 / I2S0 RX 配置
 * ======================================================================== */
#define MIC_I2S_PORT     I2S_NUM_0
#define MIC_BCLK_GPIO    GPIO_NUM_42
#define MIC_WS_GPIO      GPIO_NUM_41
#define MIC_DATA_GPIO    GPIO_NUM_1
#define MIC_SAMPLE_RATE  16000
#define MIC_SAMPLE_SHIFT 14

static i2s_chan_handle_t s_mic_rx = NULL;

/* ========================================================================
 * 2. MAX98357A / I2S1 TX 配置
 * ========================================================================
 *
 * I2S1 默认保持 16kHz。播放真实快门 WAV 时，shutter_wav_player.c 会临时
 * 把 I2S1 时钟切换到 WAV 原生 44.1kHz，播放完成后再恢复为 16kHz。
 * MIC 使用独立 I2S0，因此不受影响。
 */
#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_BCLK_GPIO      GPIO_NUM_14
#define SPK_WS_GPIO        GPIO_NUM_3
#define SPK_DOUT_GPIO      GPIO_NUM_46
#define SPK_SAMPLE_RATE    16000

static i2s_chan_handle_t s_speaker_tx = NULL;

/*
 * Speaker 播放期间置 true。
 * audio_feed_task 仍持续读取 MIC，但向 AFE 喂 0，避免快门声回灌语音识别。
 */
static volatile bool s_speaker_playing = false;

/* ========================================================================
 * 3. ESP-SR 全局对象
 * ======================================================================== */
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static volatile bool s_running = true;

/* ========================================================================
 * 4. 语音命令定义
 * ======================================================================== */
typedef enum {
    CMD_TAKE_PHOTO = 1,
    CMD_START_VIDEO,
    CMD_STOP_VIDEO,
    CMD_SOUND_ON,
    CMD_SOUND_OFF,
    CMD_QUERY_STATUS,
    CMD_EXIT_VOICE,
} voice_command_id_t;

typedef struct {
    int command_id;
    const char *pinyin;
    const char *display_name;
} voice_command_def_t;

static const voice_command_def_t s_commands[] = {
    {CMD_TAKE_PHOTO,    "pai zhao",           "拍照"},
    {CMD_START_VIDEO,   "kai shi lu xiang",   "开始录像"},
    {CMD_STOP_VIDEO,    "ting zhi lu xiang",  "停止录像"},
    {CMD_SOUND_ON,      "da kai sheng yin",   "打开声音"},
    {CMD_SOUND_OFF,     "guan bi sheng yin",  "关闭声音"},
    {CMD_QUERY_STATUS,  "zhuang tai cha xun", "状态查询"},
    {CMD_EXIT_VOICE,    "tui chu yu yin",     "退出语音"},
};

static bool s_video_recording = false;
static bool s_sound_enabled = true;
static uint32_t s_photo_success_count = 0;

/* ========================================================================
 * 5. 通用工具函数
 * ======================================================================== */
static int16_t mic_sample_to_s16(int32_t sample)
{
    int32_t value = sample >> MIC_SAMPLE_SHIFT;

    if (value > INT16_MAX) {
        value = INT16_MAX;
    } else if (value < INT16_MIN) {
        value = INT16_MIN;
    }

    return (int16_t)value;
}

static const char *command_name_from_id(int command_id)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i) {
        if (s_commands[i].command_id == command_id) {
            return s_commands[i].display_name;
        }
    }
    return "未知命令";
}

/* ========================================================================
 * 6. MAX98357A / Speaker 初始化
 * ======================================================================== */
static esp_err_t init_speaker_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT,
                                                            I2S_ROLE_MASTER);

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_speaker_tx, NULL),
                        TAG,
                        "speaker i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_speaker_tx, &std_cfg),
                        TAG,
                        "speaker i2s_channel_init_std_mode failed");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_speaker_tx),
                        TAG,
                        "speaker i2s_channel_enable failed");

    ESP_LOGI(TAG,
             "Speaker I2S ready: 16kHz idle, BCLK=%d, LRC=%d, DOUT=%d",
             SPK_BCLK_GPIO,
             SPK_WS_GPIO,
             SPK_DOUT_GPIO);

    return ESP_OK;
}

/* 真正的 Camera 拍照成功以后才调用此函数播放确认音。 */
static esp_err_t play_camera_shutter_sound(void)
{
    if (!s_sound_enabled) {
        ESP_LOGI(TAG, "SHUTTER WAV: 当前声音已关闭，拍照成功但不播放声音");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SHUTTER WAV: photo success -> play once");
    s_speaker_playing = true;

    esp_err_t err = shutter_wav_play(s_speaker_tx);

    /* 留 120ms 声学保护时间，让 Speaker 尾音衰减。 */
    vTaskDelay(pdMS_TO_TICKS(120));
    s_speaker_playing = false;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHUTTER WAV failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ========================================================================
 * 7. 业务命令执行
 * ========================================================================
 * 返回 true：本命令结束后立即退出 MultiNet，重新等待唤醒词。
 */
static bool execute_voice_command(int command_id)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "COMMAND DETECTED: id=%d, name=%s",
             command_id,
             command_name_from_id(command_id));

    switch (command_id) {
    case CMD_TAKE_PHOTO: {
        camera_photo_info_t photo = {0};

        ESP_LOGI(TAG, "ACTION: Camera 开始正式拍照...");

        esp_err_t capture_err = camera_capture_one(&photo);
        if (capture_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "ACTION: 拍照失败，不播放快门音: %s (0x%x)",
                     esp_err_to_name(capture_err),
                     (unsigned)capture_err);
            ESP_LOGI(TAG, "ACTION: 本次拍照命令结束，重新等待唤醒词");
            ESP_LOGI(TAG, "====================================================");
            return true;
        }

        /*
         * camera_capture_one() 成功后，JPEG 已经复制并保留为“最新照片”。
         * sequence 只在真正拍照成功后递增，因此它就是成功拍照计数。
         */
        s_photo_success_count = photo.sequence;

        ESP_LOGI(TAG,
                 "ACTION: PHOTO OK #%lu, %ux%u, JPEG=%u bytes",
                 (unsigned long)photo.sequence,
                 (unsigned)photo.width,
                 (unsigned)photo.height,
                 (unsigned)photo.jpeg_size);

        if (play_camera_shutter_sound() != ESP_OK) {
            ESP_LOGW(TAG,
                     "ACTION: 照片已经拍摄成功，只是快门音播放失败");
        }

        ESP_LOGI(TAG, "ACTION: 最新 JPEG 已保留在内存中，可供后续 HTTP/上传使用");
        ESP_LOGI(TAG, "ACTION: 本次拍照完成，重新等待唤醒词");
        ESP_LOGI(TAG, "====================================================");
        return true;
    }

    case CMD_START_VIDEO:
        s_video_recording = true;
        ESP_LOGI(TAG, "ACTION: 录像状态 -> START（当前仍是状态占位，尚未接 Camera 录像）");
        break;

    case CMD_STOP_VIDEO:
        s_video_recording = false;
        ESP_LOGI(TAG, "ACTION: 录像状态 -> STOP");
        break;

    case CMD_SOUND_ON:
        s_sound_enabled = true;
        ESP_LOGI(TAG, "ACTION: 声音状态 -> ON");
        break;

    case CMD_SOUND_OFF:
        s_sound_enabled = false;
        ESP_LOGI(TAG, "ACTION: 声音状态 -> OFF（仍会真实拍照，只是不播放快门音）");
        break;

    case CMD_QUERY_STATUS: {
        size_t latest_size = 0;
        uint16_t latest_width = 0;
        uint16_t latest_height = 0;
        uint32_t latest_sequence = 0;
        const uint8_t *latest = camera_capture_latest(&latest_size,
                                                       &latest_width,
                                                       &latest_height,
                                                       &latest_sequence);

        ESP_LOGI(TAG,
                 "ACTION: STATUS video=%s, sound=%s, photos=%lu, latest=%s",
                 s_video_recording ? "ON" : "OFF",
                 s_sound_enabled ? "ON" : "OFF",
                 (unsigned long)s_photo_success_count,
                 latest != NULL ? "YES" : "NO");

        if (latest != NULL) {
            ESP_LOGI(TAG,
                     "LATEST PHOTO: #%lu %ux%u JPEG=%u bytes",
                     (unsigned long)latest_sequence,
                     (unsigned)latest_width,
                     (unsigned)latest_height,
                     (unsigned)latest_size);
        }
        break;
    }

    case CMD_EXIT_VOICE:
        ESP_LOGI(TAG, "ACTION: 退出命令模式，重新等待唤醒词");
        ESP_LOGI(TAG, "====================================================");
        return true;

    default:
        ESP_LOGW(TAG, "ACTION: 未定义 command_id=%d", command_id);
        break;
    }

    ESP_LOGI(TAG, "====================================================");
    return false;
}

/* ========================================================================
 * 8. 初始化 INMP441 I2S0 RX
 * ======================================================================== */
static esp_err_t init_mic_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT,
                                                            I2S_ROLE_MASTER);

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_mic_rx),
                        TAG,
                        "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx, &std_cfg),
                        TAG,
                        "i2s_channel_init_std_mode failed");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_mic_rx),
                        TAG,
                        "i2s_channel_enable failed");

    ESP_LOGI(TAG,
             "MIC I2S ready: 16kHz, BCLK=%d, WS=%d, DIN=%d, LEFT channel",
             MIC_BCLK_GPIO,
             MIC_WS_GPIO,
             MIC_DATA_GPIO);

    return ESP_OK;
}

static esp_err_t mic_read_exact(void *buffer, size_t bytes_to_read)
{
    size_t offset = 0;

    while (offset < bytes_to_read) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_mic_rx,
                                         (uint8_t *)buffer + offset,
                                         bytes_to_read - offset,
                                         &bytes_read,
                                         portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_read == 0) {
            return ESP_FAIL;
        }
        offset += bytes_read;
    }
    return ESP_OK;
}

/* ========================================================================
 * 9. Feed Task：I2S0 -> 16-bit mono -> ESP-SR AFE
 * ======================================================================== */
static void audio_feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;

    const int feed_samples = s_afe_handle->get_feed_chunksize(afe_data);
    const int feed_channels = s_afe_handle->get_feed_channel_num(afe_data);

    ESP_LOGI(TAG,
             "AFE feed format: samples=%d, channels=%d",
             feed_samples,
             feed_channels);

    assert(feed_channels == 1);

    int32_t *raw_stereo = malloc(feed_samples * 2 * sizeof(int32_t));
    int16_t *mono_s16 = malloc(feed_samples * sizeof(int16_t));

    if (raw_stereo == NULL || mono_s16 == NULL) {
        ESP_LOGE(TAG, "audio_feed_task malloc failed");
        free(raw_stereo);
        free(mono_s16);
        vTaskDelete(NULL);
        return;
    }

    uint32_t frame_count = 0;

    while (s_running) {
        const size_t raw_bytes = feed_samples * 2 * sizeof(int32_t);

        if (mic_read_exact(raw_stereo, raw_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed");
            break;
        }

        int16_t peak = 0;

        if (s_speaker_playing) {
            memset(mono_s16, 0, feed_samples * sizeof(int16_t));
        } else {
            for (int i = 0; i < feed_samples; ++i) {
                int16_t sample = mic_sample_to_s16(raw_stereo[i * 2]);
                mono_s16[i] = sample;

                int32_t abs_sample = sample >= 0 ? sample : -(int32_t)sample;
                if (abs_sample > peak) {
                    peak = (int16_t)abs_sample;
                }
            }
        }

        s_afe_handle->feed(afe_data, mono_s16);

        if ((++frame_count % 100) == 0) {
            ESP_LOGI(TAG,
                     "MIC alive: peak=%d%s",
                     peak,
                     s_speaker_playing ? " (speaker guard)" : "");
        }
    }

    free(raw_stereo);
    free(mono_s16);
    ESP_LOGW(TAG, "audio_feed_task exit");
    vTaskDelete(NULL);
}

/* ========================================================================
 * 10. MultiNet 命令表初始化
 * ======================================================================== */
static esp_err_t configure_multinet_commands(void)
{
    ESP_RETURN_ON_ERROR(esp_mn_commands_clear(),
                        TAG,
                        "esp_mn_commands_clear failed");

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i) {
        esp_err_t err = esp_mn_commands_add(s_commands[i].command_id,
                                            s_commands[i].pinyin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "Add command failed: id=%d, %s, pinyin=%s",
                     s_commands[i].command_id,
                     s_commands[i].display_name,
                     s_commands[i].pinyin);
            return err;
        }
    }

    esp_mn_error_t *mn_error = esp_mn_commands_update();
    if (mn_error != NULL) {
        ESP_LOGE(TAG, "MultiNet command update has invalid phrase(s)");
        return ESP_ERR_INVALID_ARG;
    }

    esp_mn_commands_print();
    esp_mn_active_commands_print();
    return ESP_OK;
}

/* ========================================================================
 * 11. Detect Task：WakeNet10 -> MultiNet6 -> Execute
 * ======================================================================== */
static void speech_detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;
    const int afe_fetch_samples = s_afe_handle->get_fetch_chunksize(afe_data);

    char *mn_name = esp_srmodel_filter(s_models,
                                       ESP_MN_PREFIX,
                                       ESP_MN_CHINESE);
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "No Chinese MultiNet model found in model partition");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    if (multinet == NULL) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        vTaskDelete(NULL);
        return;
    }

    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    if (model_data == NULL) {
        ESP_LOGE(TAG, "MultiNet create failed");
        vTaskDelete(NULL);
        return;
    }

    const int mn_samples = multinet->get_samp_chunksize(model_data);
    assert(mn_samples == afe_fetch_samples);

    if (configure_multinet_commands() != ESP_OK) {
        multinet->destroy(model_data);
        vTaskDelete(NULL);
        return;
    }

    bool command_mode = false;

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "READY: 等待唤醒词 -> 你好小智");
    ESP_LOGI(TAG, "唤醒后可以说：拍照 / 开始录像 / 状态查询 ...");
    ESP_LOGI(TAG, "拍照命令：Camera 真抓拍 JPEG -> 保存最新照片 -> 播放快门音");
    ESP_LOGI(TAG, "====================================================");

    while (s_running) {
        afe_fetch_result_t *result = s_afe_handle->fetch(afe_data);

        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch failed");
            break;
        }

        if (!command_mode && result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "****************************************************");
            ESP_LOGI(TAG, "WAKE WORD DETECTED: 你好小智");
            ESP_LOGI(TAG, "现在请说命令，例如：拍照 / 开始录像 / 状态查询");
            ESP_LOGI(TAG, "****************************************************");

            s_afe_handle->disable_wakenet(afe_data);
            multinet->clean(model_data);
            command_mode = true;
        }

        if (!command_mode) {
            continue;
        }

        esp_mn_state_t mn_state = multinet->detect(model_data, result->data);

        if (mn_state == ESP_MN_STATE_DETECTING) {
            continue;
        }

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet->get_results(model_data);

            if (mn_result != NULL && mn_result->num > 0) {
                const int command_id = mn_result->command_id[0];
                const int phrase_id = mn_result->phrase_id[0];
                const float probability = mn_result->prob[0];

                ESP_LOGI(TAG,
                         "TOP1: command_id=%d phrase_id=%d prob=%.3f text=%s",
                         command_id,
                         phrase_id,
                         probability,
                         mn_result->string != NULL ? mn_result->string : "(null)");

                bool exit_voice_mode = execute_voice_command(command_id);

                if (exit_voice_mode) {
                    multinet->clean(model_data);
                    command_mode = false;
                    s_afe_handle->enable_wakenet(afe_data);
                    ESP_LOGI(TAG, "重新等待唤醒词：你好小智");
                }
            }
            continue;
        }

        if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "COMMAND TIMEOUT: 命令窗口超时");
            multinet->clean(model_data);
            command_mode = false;
            s_afe_handle->enable_wakenet(afe_data);
            ESP_LOGI(TAG, "重新等待唤醒词：你好小智");
        }
    }

    multinet->destroy(model_data);
    ESP_LOGW(TAG, "speech_detect_task exit");
    vTaskDelete(NULL);
}

/* ========================================================================
 * 12. app_main
 * ======================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3 VOICE + REAL CAMERA PHOTO + WAV SHUTTER");
    ESP_LOGI(TAG, "Camera  : OV3660 / QVGA JPEG / real capture");
    ESP_LOGI(TAG, "INMP441 : BCLK=42 WS=41 DIN=1 (I2S0 RX / LEFT)");
    ESP_LOGI(TAG, "Speaker : BCLK=14 LRC=3 DOUT=46 (I2S1 TX)");
    ESP_LOGI(TAG, "WakeNet10: 你好小智 (wn10_nihaoxiaozhi)");
    ESP_LOGI(TAG, "====================================================");

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free PSRAM before init: %u bytes", (unsigned)psram_free);

    if (psram_free == 0) {
        ESP_LOGW(TAG,
                 "当前没有可用 PSRAM；最新 JPEG 会回退到内部内存，"
                 "WakeNet + MultiNet 也可能受内存限制影响。");
    }

    /*
     * 先初始化三套硬件能力：
     *   Camera：板载 OV3660
     *   I2S0：INMP441 RX
     *   I2S1：MAX98357A TX
     */
    ESP_ERROR_CHECK(camera_capture_init());
    ESP_ERROR_CHECK(init_mic_i2s());
    ESP_ERROR_CHECK(init_speaker_i2s());

    /* 从 model 分区加载 WakeNet10 / MultiNet6。 */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init(model) failed");
        return;
    }

    /* 单麦克风输入，不提供 playback reference。 */
    afe_config_t *afe_config = afe_config_init("M",
                                               s_models,
                                               AFE_TYPE_SR,
                                               AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }

    afe_config_print(afe_config);

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (s_afe_handle == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        return;
    }

    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return;
    }

    BaseType_t feed_ok = xTaskCreatePinnedToCore(audio_feed_task,
                                                 "audio_feed",
                                                 8 * 1024,
                                                 s_afe_data,
                                                 5,
                                                 NULL,
                                                 0);

    BaseType_t detect_ok = xTaskCreatePinnedToCore(speech_detect_task,
                                                   "speech_detect",
                                                   10 * 1024,
                                                   s_afe_data,
                                                   5,
                                                   NULL,
                                                   1);

    if (feed_ok != pdPASS || detect_ok != pdPASS) {
        ESP_LOGE(TAG, "Create speech task failed");
        s_running = false;
        return;
    }
}
