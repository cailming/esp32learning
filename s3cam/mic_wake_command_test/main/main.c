/*
 * ESP32-S3-CAM：INMP441 语音唤醒 + 中文命令词识别测试
 *
 * 硬件沿用已经通过 mic_test 的接线：
 *
 *   INMP441 SCK/BCLK -> GPIO42
 *   INMP441 WS/LRCLK -> GPIO41
 *   INMP441 SD       -> GPIO1
 *   INMP441 L/R      -> GND（数据位于 LEFT channel）
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
 *      +--> WakeNet：等待“Hi 乐鑫”
 *      |
 *      +--> 唤醒后进入 MultiNet6 中文命令识别
 *               |
 *               v
 *         execute_voice_command()
 *               |
 *               +--> 打印 ACTION / 更新测试状态
 *
 * 当前版本的“执行动作”是测试级动作：更新内存状态并打印串口日志。
 * 后续可以把 switch-case 中的动作直接替换成 Camera / Speaker 的真实业务调用。
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
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

#if !CONFIG_IDF_TARGET_ESP32S3
#error "mic_wake_command_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "voice_cmd";

/* ========================================================================
 * 1. INMP441 / I2S 配置
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
 * 这里把 int32_t 转成 int16_t 给 ESP-SR。
 * 右移 14 位相当于在取高位的同时保留一点增益；如果后续发现经常削顶，
 * 可以改成 15 或 16；如果声音过小，可以尝试 13。
 */
#define MIC_SAMPLE_SHIFT 14

static i2s_chan_handle_t s_mic_rx = NULL;

/* ========================================================================
 * 2. ESP-SR 全局对象
 * ======================================================================== */
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static volatile bool s_running = true;

/* ========================================================================
 * 3. 测试命令定义
 * ========================================================================
 *
 * MultiNet6 中文命令使用拼音作为识别单元。
 * 用户实际说中文，程序配置里写对应拼音。
 * Command ID 从 1 开始。
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
 * 这些变量只是用来证明“命令已经被解析并执行到业务分支”。
 * 后续接真实 Camera/Speaker 时，可以把这些状态替换成真实模块状态。
 */
static bool s_video_recording = false;
static bool s_sound_enabled = true;
static uint32_t s_photo_request_count = 0;

/* ========================================================================
 * 4. 工具函数
 * ======================================================================== */

/* 将 32-bit 麦克风样本转换成 ESP-SR 需要的 16-bit PCM，并做饱和保护。 */
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

/* 根据 command_id 找到方便人看的中文名字。 */
static const char *command_name_from_id(int command_id)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i) {
        if (s_commands[i].command_id == command_id) {
            return s_commands[i].display_name;
        }
    }

    return "未知命令";
}

/*
 * 业务命令执行入口。
 *
 * 返回 true 表示执行后希望马上退出“连续命令识别”状态，重新等待唤醒词。
 */
static bool execute_voice_command(int command_id)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "COMMAND DETECTED: id=%d, name=%s",
             command_id, command_name_from_id(command_id));

    switch (command_id) {
    case CMD_TAKE_PHOTO:
        ++s_photo_request_count;
        ESP_LOGI(TAG, "ACTION: 拍照请求 #%lu",
                 (unsigned long)s_photo_request_count);
        ESP_LOGI(TAG, "说明：当前语音测试暂不调用 Camera，后续可在这里接 capture_one_photo()。");
        break;

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
        ESP_LOGI(TAG, "ACTION: 声音状态 -> ON");
        break;

    case CMD_SOUND_OFF:
        s_sound_enabled = false;
        ESP_LOGI(TAG, "ACTION: 声音状态 -> OFF");
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
 * 5. 初始化 INMP441 I2S RX
 * ======================================================================== */
static esp_err_t init_mic_i2s(void)
{
    /* 创建 I2S0 RX channel。 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT,
                                                            I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_mic_rx),
                        TAG,
                        "i2s_new_channel failed");

    /*
     * 继续沿用已经通过 mic_test 的格式：
     * - 16kHz
     * - Philips I2S
     * - 32-bit stereo slot
     * - LEFT 为真实麦克风数据
     */
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
 * 6. Feed Task：I2S -> 16-bit mono -> AFE
 * ======================================================================== */
static void audio_feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;

    /* AFE 告诉我们：每次 feed 需要多少“单声道采样点”。 */
    const int feed_samples = s_afe_handle->get_feed_chunksize(afe_data);
    const int feed_channels = s_afe_handle->get_feed_channel_num(afe_data);

    ESP_LOGI(TAG,
             "AFE feed format: samples=%d, channels=%d",
             feed_samples,
             feed_channels);

    /* 本项目 afe_config_init("M", ...) 明确只有一个 microphone channel。 */
    assert(feed_channels == 1);

    /* I2S 是 32-bit stereo，所以每个时间点有 LEFT + RIGHT 两个 int32_t。 */
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

        for (int i = 0; i < feed_samples; ++i) {
            /* mic_test 已确认：偶数 slot（LEFT）是真实数据，RIGHT 为 0。 */
            int16_t sample = mic_sample_to_s16(raw_stereo[i * 2]);
            mono_s16[i] = sample;

            int32_t abs_sample = sample >= 0 ? sample : -(int32_t)sample;
            if (abs_sample > peak) {
                peak = (int16_t)abs_sample;
            }
        }

        /* 把一整帧 16-bit mono PCM 喂给 ESP-SR AFE。 */
        s_afe_handle->feed(afe_data, mono_s16);

        /* 每约 100 帧打印一次输入峰值，方便判断麦克风是否持续有数据。 */
        if ((++frame_count % 100) == 0) {
            ESP_LOGI(TAG, "MIC alive: peak=%d", peak);
        }
    }

    free(raw_stereo);
    free(mono_s16);

    ESP_LOGW(TAG, "audio_feed_task exit");
    vTaskDelete(NULL);
}

/* ========================================================================
 * 7. MultiNet 命令表初始化
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

    /*
     * add/remove/modify 只是修改缓存；必须 update 才会真正应用到 MultiNet。
     * 成功时返回 NULL；非 NULL 表示存在不能解析的命令词。
     */
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
 * 8. Detect Task：WakeNet -> MultiNet -> Execute
 * ======================================================================== */
static void speech_detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;

    const int afe_fetch_samples = s_afe_handle->get_fetch_chunksize(afe_data);

    /* 从 model partition 中找到中文 MultiNet 模型。 */
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

    /* 6000ms：唤醒后如果长期没说有效命令，就回到等待唤醒状态。 */
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    if (model_data == NULL) {
        ESP_LOGE(TAG, "MultiNet create failed");
        vTaskDelete(NULL);
        return;
    }

    const int mn_samples = multinet->get_samp_chunksize(model_data);

    /* AFE fetch 出来的帧大小必须和 MultiNet detect 要求完全一致。 */
    assert(mn_samples == afe_fetch_samples);

    if (configure_multinet_commands() != ESP_OK) {
        multinet->destroy(model_data);
        vTaskDelete(NULL);
        return;
    }

    bool command_mode = false;

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "READY: 等待唤醒词 -> Hi 乐鑫");
    ESP_LOGI(TAG, "唤醒后可以连续说命令，约 6 秒无有效命令后自动退出命令模式");
    ESP_LOGI(TAG, "====================================================");

    while (s_running) {
        afe_fetch_result_t *result = s_afe_handle->fetch(afe_data);

        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch failed");
            break;
        }

        /*
         * 当前只有 1 个麦克风通道，因此 WAKENET_DETECTED 就可以直接认为唤醒成功。
         */
        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "****************************************************");
            ESP_LOGI(TAG, "WAKE WORD DETECTED: Hi 乐鑫");
            ESP_LOGI(TAG, "现在请说命令，例如：拍照 / 开始录像 / 状态查询");
            ESP_LOGI(TAG, "****************************************************");

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
                    s_afe_handle->enable_wakenet(afe_data);
                    multinet->clean(model_data);
                    command_mode = false;
                    ESP_LOGI(TAG, "重新等待唤醒词：Hi 乐鑫");
                }
            }

            /* 连续命令模式：识别一条后继续听下一条。 */
            continue;
        }

        if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "COMMAND TIMEOUT: 命令窗口超时");

            s_afe_handle->enable_wakenet(afe_data);
            multinet->clean(model_data);
            command_mode = false;

            ESP_LOGI(TAG, "重新等待唤醒词：Hi 乐鑫");
        }
    }

    multinet->destroy(model_data);
    ESP_LOGW(TAG, "speech_detect_task exit");
    vTaskDelete(NULL);
}

/* ========================================================================
 * 9. app_main
 * ======================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3 MIC WAKE + VOICE COMMAND TEST");
    ESP_LOGI(TAG, "INMP441: BCLK=42 WS=41 DIN=1 (LEFT)");
    ESP_LOGI(TAG, "WakeNet : Hi 乐鑫");
    ESP_LOGI(TAG, "MultiNet: Chinese offline command recognition");
    ESP_LOGI(TAG, "Camera / MAX98357A / Speaker 保持物理接线，不需要拔线");
    ESP_LOGI(TAG, "====================================================");

    /*
     * ESP-SR 完整 WakeNet + MultiNet 通常需要 PSRAM。
     * 这里打印当前可用 PSRAM，方便第一时间判断板子配置是否正确。
     */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free PSRAM before SR init: %u bytes", (unsigned)psram_free);

    if (psram_free == 0) {
        ESP_LOGW(TAG,
                 "当前没有可用 PSRAM；WakeNet + MultiNet 可能无法初始化。"
                 "如果后面失败，请先根据启动日志确认本板 PSRAM 类型/容量。");
    }

    ESP_ERROR_CHECK(init_mic_i2s());

    /*
     * ESP-SR 会从 partitions.csv 中 label 为 "model" 的分区加载 WakeNet / MultiNet。
     * 模型选择由 sdkconfig.defaults 中 CONFIG_SR_* 项决定。
     */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init(model) failed");
        return;
    }

    /*
     * input_format="M"：单麦克风输入。
     * 不提供 playback reference，因此当前 AFE 不做双通道 AEC 输入。
     */
    afe_config_t *afe_config = afe_config_init("M",
                                               s_models,
                                               AFE_TYPE_SR,
                                               AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }

    /* 打印最终 AFE 配置，调试识别率时很有用。 */
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

    /*
     * 两个核心任务分到两个 CPU Core：
     * Core 0：持续从 I2S 采音并 feed AFE
     * Core 1：fetch AFE 输出并做 WakeNet / MultiNet 推理
     */
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
