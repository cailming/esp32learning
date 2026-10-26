/*
 * ESP32-S3-CAM：INMP441 语音唤醒 + 中文命令词识别 + 真实 WAV 快门音测试
 *
 * 已验证硬件接线：
 *
 *   INMP441 SCK/BCLK -> GPIO42
 *   INMP441 WS/LRCLK -> GPIO41
 *   INMP441 SD       -> GPIO1
 *   INMP441 L/R      -> GND（数据位于 LEFT channel）
 *
 *   MAX98357A BCLK   -> GPIO14
 *   MAX98357A LRC    -> GPIO3
 *   MAX98357A DIN    -> GPIO46
 *   MAX98357A VIN    -> 5V
 *   MAX98357A GND    -> 公共 GND
 *
 * 整体流程：
 *
 *   INMP441
 *      |
 *      | 32-bit stereo I2S（实际声音在 LEFT）
 *      v
 *   I2S0 RX
 *      |
 *      | 转换成 16kHz / 16-bit / mono
 *      v
 *   ESP-SR AFE
 *      |
 *      +--> WakeNet10：等待“你好小智”
 *      |
 *      +--> 唤醒后进入 MultiNet6 中文命令识别
 *               |
 *               v
 *         execute_voice_command()
 *               |
 *               +--> “拍照”
 *                       |
 *                       v
 *                 embedded shutter.wav
 *                       |
 *                       v
 *                 I2S1 -> MAX98357A -> Speaker
 *
 * 当前 Camera 动作仍然是测试级动作：只累计拍照请求次数；
 * 但快门声已经是真实 WAV 文件，不再由程序现场合成。
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

/*
 * INMP441 输出有效数据位于 32-bit I2S slot 的高位。
 * mic_test 已经验证 LEFT channel 有数据、RIGHT channel 为 0。
 *
 * ESP-SR 需要 16-bit PCM，因此这里把 int32_t 转成 int16_t。
 * 当前右移 14 位已经在前面的唤醒/命令测试中验证可用。
 */
#define MIC_SAMPLE_SHIFT 14

static i2s_chan_handle_t s_mic_rx = NULL;

/* ========================================================================
 * 2. MAX98357A / I2S1 TX 配置
 * ========================================================================
 *
 * 沿用 speaker_test 已经验证通过的接线和格式：
 *
 *   I2S1 TX
 *   16kHz / 16-bit / stereo framing
 *
 * shutter.wav 原始采样率不是 16kHz，shutter_wav_player.c 会在播放时
 * 做整数线性重采样，再把 mono 样本复制到 LEFT / RIGHT。
 */
#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_BCLK_GPIO      GPIO_NUM_14
#define SPK_WS_GPIO        GPIO_NUM_3
#define SPK_DOUT_GPIO      GPIO_NUM_46
#define SPK_SAMPLE_RATE    16000

static i2s_chan_handle_t s_speaker_tx = NULL;

/*
 * Speaker 播放期间置为 true。
 *
 * audio_feed_task 仍继续读取 I2S0，避免 RX 堵塞；
 * 但是送给 AFE 的数据会临时改成 0，防止设备自己的快门声回灌到
 * WakeNet / MultiNet，形成误识别。
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
 * 4. 测试命令定义
 * ========================================================================
 *
 * MultiNet6 中文命令使用拼音作为识别单元。
 * 用户实际说中文，程序配置里写对应拼音。
 */
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

/*
 * 测试状态：
 *
 * s_sound_enabled=true  -> “拍照”会播放 shutter.wav
 * s_sound_enabled=false -> “拍照”只执行命令，不播放声音
 */
static bool s_video_recording = false;
static bool s_sound_enabled = true;
static uint32_t s_photo_request_count = 0;

/* ========================================================================
 * 5. 通用工具函数
 * ======================================================================== */

/* 将 32-bit 麦克风样本转换为 ESP-SR 需要的 16-bit PCM。 */
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
             "Speaker I2S ready: 16kHz, BCLK=%d, LRC=%d, DOUT=%d",
             SPK_BCLK_GPIO,
             SPK_WS_GPIO,
             SPK_DOUT_GPIO);

    return ESP_OK;
}

/*
 * 播放真正的 WAV 快门声。
 *
 * 注意：这里不再生成三角波、正弦波或随机噪声。
 * shutter_wav_play() 直接读取编译进固件的 RIFF/WAV PCM 数据。
 */
static esp_err_t play_camera_shutter_sound(void)
{
    if (!s_sound_enabled) {
        ESP_LOGI(TAG, "SHUTTER WAV: 当前声音已关闭，跳过播放");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SHUTTER WAV: play once");

    s_speaker_playing = true;

    esp_err_t err = shutter_wav_play(s_speaker_tx);

    /*
     * WAV 播完以后再留 120ms 声学保护时间。
     * Speaker 已经不再输出有效声音，但 MIC 仍暂时向 AFE 喂 0，
     * 等空气中的尾音衰减以后再恢复正常识别。
     */
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
 *
 * 返回 true：本命令执行完以后立即退出 MultiNet 命令模式，重新等待唤醒词。
 */
static bool execute_voice_command(int command_id)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "COMMAND DETECTED: id=%d, name=%s",
             command_id,
             command_name_from_id(command_id));

    switch (command_id) {
    case CMD_TAKE_PHOTO:
        ++s_photo_request_count;
        ESP_LOGI(TAG, "ACTION: 拍照请求 #%lu",
                 (unsigned long)s_photo_request_count);

        if (play_camera_shutter_sound() != ESP_OK) {
            ESP_LOGW(TAG, "ACTION: WAV 快门音播放失败，但拍照命令已经识别");
        }

        ESP_LOGI(TAG,
                 "说明：当前暂未调用 Camera；以后应在真正拍照成功后播放此 WAV。");
        ESP_LOGI(TAG, "ACTION: 本次拍照结束，重新等待唤醒词");
        ESP_LOGI(TAG, "====================================================");

        /*
         * 拍照采用 one-shot 模式。
         * 这样 Speaker 的快门声即使被 MIC 听到，也不会在同一个 MultiNet
         * 命令窗口里再次触发“拍照”，彻底避免连续自触发。
         */
        return true;

    case CMD_START_VIDEO:
        s_video_recording = true;
        ESP_LOGI(TAG, "ACTION: 录像状态 -> START");
        break;

    case CMD_STOP_VIDEO:
        s_video_recording = false;
        ESP_LOGI(TAG, "ACTION: 录像状态 -> STOP");
        break;

    case CMD_SOUND_ON:
        s_sound_enabled = true;
        ESP_LOGI(TAG, "ACTION: 声音状态 -> ON（拍照播放真实 WAV）");
        break;

    case CMD_SOUND_OFF:
        s_sound_enabled = false;
        ESP_LOGI(TAG, "ACTION: 声音状态 -> OFF（拍照静音）");
        break;

    case CMD_QUERY_STATUS:
        ESP_LOGI(TAG,
                 "ACTION: STATUS video=%s, sound=%s, photo_requests=%lu",
                 s_video_recording ? "ON" : "OFF",
                 s_sound_enabled ? "ON" : "OFF",
                 (unsigned long)s_photo_request_count);
        break;

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

/*
 * I2S read 理论上通常一次就会返回请求的数据量，但这里仍然写成“读满为止”，
 * 避免偶发 partial read 导致 AFE 收到半帧音频。
 */
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

    /* afe_config_init("M", ...) 明确只有一个 microphone channel。 */
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
            /*
             * Speaker 正在播放真实 WAV。
             * MIC 继续读取，但暂时不给 ESP-SR 真实声音。
             */
            memset(mono_s16, 0, feed_samples * sizeof(int16_t));
        } else {
            for (int i = 0; i < feed_samples; ++i) {
                /* mic_test 已确认：LEFT 为真实数据，RIGHT 为 0。 */
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

    /* add/remove/modify 后必须 update，命令表才会真正生效。 */
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

    /* 6000ms：唤醒后长期没有有效命令，就重新进入等待唤醒状态。 */
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
    ESP_LOGI(TAG, "拍照命令：播放一次真实 shutter.wav，然后重新等待唤醒");
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

            /*
             * 进入命令模式以后先关闭 WakeNet。
             * 这样命令识别阶段只由 MultiNet 工作，不会重复触发唤醒事件。
             */
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
    ESP_LOGI(TAG, "ESP32-S3 MIC WAKE + COMMAND + REAL WAV SHUTTER TEST");
    ESP_LOGI(TAG, "INMP441 : BCLK=42 WS=41 DIN=1 (I2S0 RX / LEFT)");
    ESP_LOGI(TAG, "Speaker : BCLK=14 LRC=3 DOUT=46 (I2S1 TX)");
    ESP_LOGI(TAG, "WakeNet10: 你好小智 (wn10_nihaoxiaozhi)");
    ESP_LOGI(TAG, "Shutter : embedded real shutter.wav");
    ESP_LOGI(TAG, "Camera / MIC / MAX98357A / Speaker 全部保持物理接线");
    ESP_LOGI(TAG, "====================================================");

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free PSRAM before SR init: %u bytes", (unsigned)psram_free);

    if (psram_free == 0) {
        ESP_LOGW(TAG,
                 "当前没有可用 PSRAM；WakeNet + MultiNet 可能无法初始化。"
                 "如果后面失败，请根据启动日志确认本板 PSRAM 类型/容量。");
    }

    /*
     * I2S0：INMP441 RX
     * I2S1：MAX98357A TX
     */
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
