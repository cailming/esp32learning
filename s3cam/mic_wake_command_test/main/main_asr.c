/*
 * ESP32-S3-CAM：Camera + INMP441 + ESP-SR + MAX98357A + ChatTTS + FunASR
 *
 * 对话模式新增链路：
 *   INMP441 -> audio_feed_task -> 16kHz PCM16 mono
 *                            |-> ESP-SR AFE -> WakeNet / MultiNet
 *                            `-> StreamBuffer -> dialog_capture_task(VAD)
 *                                             -> ASR upload queue
 *                                             -> asr_upload_task
 *                                             -> Mac FunASR ParaformerStreaming
 *                                             -> JSON text -> ESP_LOGI
 *
 * 注意：I2S0 始终只有 audio_feed_task 一个读取者，ASR 只是复制 PCM，避免抢占麦克风。
 *
 * 内存策略：
 *   1. 保持原版已验证的 Camera -> ESP-SR -> Wi-Fi 启动顺序；
 *   2. 等 Wi-Fi/HTTP Server 成功后，再创建新增的 ASR runtime；
 *   3. ASR 32KB PCM StreamBuffer 的实际数据区明确放在 PSRAM，避免挤占 Wi-Fi 所需内部 RAM。
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
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "asr_client.h"
#include "camera_capture.h"
#include "photo_http_server.h"
#include "shutter_wav_player.h"
#include "tts_service.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "mic_wake_command_test 只用于 ESP32-S3，请先执行 idf.py set-target esp32s3"
#endif

static const char *TAG = "voice_cmd";

#define STARTUP_TTS_WAIT_MS 15000U
#define AFE_RESUME_GUARD_MS 80U
#define STARTUP_TTS_TEXT \
    "系统启动完成。我是小智。说你好小智唤醒我，可以拍照、查询状态或进入对话模式。"
#define WAKE_REPLY_TTS_TEXT "我在，有什么需要"
#define PHOTO_DONE_TTS_TEXT "拍照完成"

#define MIC_I2S_PORT     I2S_NUM_0
#define MIC_BCLK_GPIO    GPIO_NUM_42
#define MIC_WS_GPIO      GPIO_NUM_41
#define MIC_DATA_GPIO    GPIO_NUM_1
#define MIC_SAMPLE_RATE  16000
#define MIC_SAMPLE_SHIFT 14

#define SPK_I2S_PORT       I2S_NUM_1
#define SPK_BCLK_GPIO      GPIO_NUM_14
#define SPK_WS_GPIO        GPIO_NUM_3
#define SPK_DOUT_GPIO      GPIO_NUM_46
#define SPK_SAMPLE_RATE    16000

#define ASR_STREAM_BYTES              (32 * 1024)
#define ASR_UPLOAD_QUEUE_DEPTH         2
#define ASR_CAPTURE_READ_BYTES         1024
#define ASR_MIN_UTTERANCE_MS           250
#define ASR_TEXT_MAX                   1024

static i2s_chan_handle_t s_mic_rx = NULL;
static i2s_chan_handle_t s_speaker_tx = NULL;
static SemaphoreHandle_t s_speaker_mutex = NULL;
static volatile bool s_speaker_playing = false;

static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static volatile bool s_running = true;
static volatile bool s_detector_ready = false;

typedef enum {
    CMD_TAKE_PHOTO = 1,
    CMD_START_VIDEO,
    CMD_STOP_VIDEO,
    CMD_SOUND_ON,
    CMD_SOUND_OFF,
    CMD_QUERY_STATUS,
    CMD_ENTER_DIALOG_MODE,
    CMD_EXIT_DIALOG_MODE,
    CMD_EXIT_VOICE,
} voice_command_id_t;

typedef struct {
    int command_id;
    const char *pinyin;
    const char *display_name;
} voice_command_def_t;

static const voice_command_def_t s_commands[] = {
    {CMD_TAKE_PHOTO,        "pai zhao",                "拍照"},
    {CMD_START_VIDEO,       "kai shi lu xiang",        "开始录像"},
    {CMD_STOP_VIDEO,        "ting zhi lu xiang",       "停止录像"},
    {CMD_SOUND_ON,          "da kai sheng yin",        "打开声音"},
    {CMD_SOUND_OFF,         "guan bi sheng yin",       "关闭声音"},
    {CMD_QUERY_STATUS,      "zhuang tai cha xun",      "状态查询"},
    {CMD_ENTER_DIALOG_MODE, "jin ru dui hua mo shi",   "进入对话模式"},
    {CMD_EXIT_DIALOG_MODE,  "tui chu dui hua mo shi",  "退出对话模式"},
    {CMD_EXIT_VOICE,        "tui chu yu yin",          "退出语音"},
};

static bool s_video_recording = false;
static bool s_sound_enabled = true;
static volatile bool s_dialog_mode = false;
static volatile bool s_asr_exit_dialog_requested = false;
static uint32_t s_photo_success_count = 0;

/* ASR 对话流水线 */
static StreamBufferHandle_t s_asr_stream = NULL;
static QueueHandle_t s_asr_upload_queue = NULL;
static uint8_t *s_asr_stream_storage = NULL;
static StaticStreamBuffer_t s_asr_stream_struct;

typedef struct {
    int16_t *pcm;
    size_t samples;
} asr_utterance_t;

static void log_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "MEM %s: internal=%u largest_internal=%u dma=%u psram=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static int16_t mic_sample_to_s16(int32_t sample)
{
    int32_t value = sample >> MIC_SAMPLE_SHIFT;
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}

static const char *command_name_from_id(int command_id)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i) {
        if (s_commands[i].command_id == command_id) return s_commands[i].display_name;
    }
    return "未知命令";
}

static bool is_exit_dialog_text(const char *text)
{
    if (text == NULL) return false;

    return strcmp(text, "退出对话模式") == 0 ||
           strcmp(text, "退出对话模式。") == 0 ||
           strcmp(text, "退出对话模式！") == 0 ||
           strcmp(text, "退出对话模式!") == 0 ||
           strcmp(text, "退出对话模式？") == 0 ||
           strcmp(text, "退出对话模式?") == 0;
}

static esp_err_t wait_uplink_ready(uint32_t timeout_ms)
{
#ifdef CONFIG_TTS_WIFI_STA_ENABLE
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - start) < timeout) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta != NULL) {
            esp_netif_ip_info_t ip = {0};
            if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0U) {
                ESP_LOGI(TAG, "uplink ready: STA has IPv4 address");
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ESP_ERR_TIMEOUT;
#else
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t init_speaker_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_speaker_tx, NULL), TAG, "speaker channel");

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_speaker_tx, &cfg), TAG, "speaker std mode");

    s_speaker_mutex = xSemaphoreCreateMutex();
    return s_speaker_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool speaker_lock(void)
{
    return s_speaker_mutex != NULL && xSemaphoreTake(s_speaker_mutex, portMAX_DELAY) == pdTRUE;
}

static void speaker_unlock(void)
{
    if (s_speaker_mutex != NULL) xSemaphoreGive(s_speaker_mutex);
}

static esp_err_t play_camera_shutter_sound(void)
{
    if (!s_sound_enabled) return ESP_OK;
    if (!speaker_lock()) return ESP_ERR_INVALID_STATE;

    s_speaker_playing = true;
    esp_err_t err = shutter_wav_play(s_speaker_tx);
    vTaskDelay(pdMS_TO_TICKS(120));
    s_speaker_playing = false;
    speaker_unlock();
    return err;
}

static esp_err_t queue_tts_text(const char *text)
{
    if (!s_sound_enabled) return ESP_OK;
    if (strlen(CONFIG_TTS_SERVER_URL) == 0U) return ESP_ERR_INVALID_STATE;
    return tts_service_enqueue(text);
}

static bool execute_voice_command(int command_id)
{
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "COMMAND DETECTED: id=%d name=%s", command_id, command_name_from_id(command_id));

    switch (command_id) {
    case CMD_TAKE_PHOTO: {
        camera_photo_info_t photo = {0};
        esp_err_t err = camera_capture_one(&photo);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "拍照失败: %s", esp_err_to_name(err));
            return true;
        }
        s_photo_success_count = photo.sequence;
        ESP_LOGI(TAG, "PHOTO OK #%lu %ux%u JPEG=%u bytes",
                 (unsigned long)photo.sequence,
                 (unsigned)photo.width,
                 (unsigned)photo.height,
                 (unsigned)photo.jpeg_size);
        if (play_camera_shutter_sound() != ESP_OK) ESP_LOGW(TAG, "快门音播放失败");
        queue_tts_text(PHOTO_DONE_TTS_TEXT);
        return true;
    }

    case CMD_START_VIDEO:
        s_video_recording = true;
        ESP_LOGI(TAG, "录像状态 -> START（占位）");
        break;

    case CMD_STOP_VIDEO:
        s_video_recording = false;
        ESP_LOGI(TAG, "录像状态 -> STOP");
        break;

    case CMD_SOUND_ON:
        s_sound_enabled = true;
        ESP_LOGI(TAG, "声音 -> ON");
        break;

    case CMD_SOUND_OFF:
        s_sound_enabled = false;
        ESP_LOGI(TAG, "声音 -> OFF");
        break;

    case CMD_QUERY_STATUS: {
        ESP_LOGI(TAG, "STATUS video=%s sound=%s dialog=%s photos=%lu",
                 s_video_recording ? "ON" : "OFF",
                 s_sound_enabled ? "ON" : "OFF",
                 s_dialog_mode ? "ON" : "OFF",
                 (unsigned long)s_photo_success_count);
        char status[128];
        snprintf(status, sizeof(status), "设备正常，当前%s，声音%s，%s。",
                 s_video_recording ? "正在录像" : "未录像",
                 s_sound_enabled ? "已开启" : "已关闭",
                 s_dialog_mode ? "已进入对话模式" : "未进入对话模式");
        queue_tts_text(status);
        return true;
    }

    case CMD_ENTER_DIALOG_MODE:
        if (s_asr_stream == NULL || s_asr_upload_queue == NULL) {
            ESP_LOGE(TAG, "DIALOG MODE unavailable: ASR runtime 未成功启动");
            queue_tts_text("对话模式暂不可用");
            return true;
        }
        if (!s_dialog_mode) {
            xStreamBufferReset(s_asr_stream);
            s_dialog_mode = true;
            ESP_LOGI(TAG, "DIALOG MODE -> ON：开始采集用户语音并发送 ASR");
            queue_tts_text("进入对话模式");
        } else {
            queue_tts_text("已在对话模式");
        }
        ESP_LOGI(TAG, "ASR endpoint: %s", CONFIG_ASR_SERVER_URL);
        return false;

    case CMD_EXIT_DIALOG_MODE:
        if (s_dialog_mode) {
            s_dialog_mode = false;
            ESP_LOGI(TAG, "DIALOG MODE -> OFF");
            ESP_LOGI(TAG, "退出动作已完成，播放退出确认语音");
            queue_tts_text("退出对话模式");
        }
        return true;

    case CMD_EXIT_VOICE:
        if (s_dialog_mode) {
            s_dialog_mode = false;
            ESP_LOGI(TAG, "DIALOG MODE -> OFF (退出语音)");
            ESP_LOGI(TAG, "退出动作已完成，播放退出确认语音");
            queue_tts_text("退出对话模式");
        }
        return true;

    default:
        ESP_LOGW(TAG, "未知 command_id=%d", command_id);
        break;
    }

    return false;
}

static esp_err_t init_mic_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_mic_rx), TAG, "mic channel");

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DATA_GPIO,
            .invert_flags = {0},
        },
    };
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx, &cfg), TAG, "mic std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_mic_rx), TAG, "mic enable");
    return ESP_OK;
}

static esp_err_t mic_read_exact(void *buffer, size_t bytes)
{
    size_t offset = 0;
    while (offset < bytes) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_mic_rx,
                                         (uint8_t *)buffer + offset,
                                         bytes - offset,
                                         &got,
                                         portMAX_DELAY);
        if (err != ESP_OK) return err;
        if (got == 0) return ESP_FAIL;
        offset += got;
    }
    return ESP_OK;
}

static void audio_feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;
    const int feed_samples = s_afe_handle->get_feed_chunksize(afe_data);
    const int feed_channels = s_afe_handle->get_feed_channel_num(afe_data);
    assert(feed_channels == 1);

    int32_t *raw_stereo = malloc(feed_samples * 2 * sizeof(int32_t));
    int16_t *mono = malloc(feed_samples * sizeof(int16_t));
    if (raw_stereo == NULL || mono == NULL) {
        free(raw_stereo);
        free(mono);
        vTaskDelete(NULL);
        return;
    }

    uint32_t drop_count = 0;
    while (s_running) {
        size_t raw_bytes = feed_samples * 2 * sizeof(int32_t);
        if (mic_read_exact(raw_stereo, raw_bytes) != ESP_OK) break;
        if (!s_detector_ready) continue;

        for (int i = 0; i < feed_samples; ++i) {
            mono[i] = mic_sample_to_s16(raw_stereo[i * 2]);
        }

        /* 播放 TTS/快门音时既不送 ESP-SR，也不送 ASR，避免识别自己的声音。 */
        if (s_speaker_playing) continue;

        if (s_dialog_mode && s_asr_stream != NULL) {
            size_t wanted = feed_samples * sizeof(int16_t);
            size_t sent = xStreamBufferSend(s_asr_stream, mono, wanted, 0);
            if (sent != wanted && (++drop_count % 20U) == 1U) {
                ESP_LOGW(TAG, "ASR PCM stream full: dropped %u bytes", (unsigned)(wanted - sent));
            }
        }

        s_afe_handle->feed(afe_data, mono);
    }

    free(raw_stereo);
    free(mono);
    vTaskDelete(NULL);
}

static void free_utterance(asr_utterance_t *u)
{
    if (u != NULL && u->pcm != NULL) {
        heap_caps_free(u->pcm);
        u->pcm = NULL;
        u->samples = 0;
    }
}

static void submit_utterance(asr_utterance_t *u)
{
    if (u->pcm == NULL || u->samples == 0) return;
    if (xQueueSend(s_asr_upload_queue, u, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ASR upload queue full，丢弃本段语音");
        free_utterance(u);
    } else {
        ESP_LOGI(TAG, "ASR utterance queued: %.2f s",
                 (double)u->samples / (double)MIC_SAMPLE_RATE);
        u->pcm = NULL;
        u->samples = 0;
    }
}

static void dialog_capture_task(void *arg)
{
    (void)arg;
    uint8_t read_buf[ASR_CAPTURE_READ_BYTES];
    const size_t max_samples = (size_t)MIC_SAMPLE_RATE * CONFIG_ASR_MAX_UTTERANCE_MS / 1000U;
    const size_t min_samples = (size_t)MIC_SAMPLE_RATE * ASR_MIN_UTTERANCE_MS / 1000U;

    asr_utterance_t current = {0};
    bool speaking = false;
    uint32_t silence_ms = 0;

    while (s_running) {
        if (!s_dialog_mode) {
            free_utterance(&current);
            speaking = false;
            silence_ms = 0;
            if (s_asr_stream != NULL) xStreamBufferReset(s_asr_stream);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t got = xStreamBufferReceive(s_asr_stream,
                                          read_buf,
                                          sizeof(read_buf),
                                          pdMS_TO_TICKS(100));
        if (got < sizeof(int16_t)) continue;
        got &= ~(sizeof(int16_t) - 1U);

        int16_t *samples = (int16_t *)read_buf;
        size_t count = got / sizeof(int16_t);
        int32_t peak = 0;
        for (size_t i = 0; i < count; ++i) {
            int32_t v = samples[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }

        bool voice = peak >= CONFIG_ASR_VAD_PEAK_THRESHOLD;
        uint32_t frame_ms = (uint32_t)((count * 1000U) / MIC_SAMPLE_RATE);

        if (!speaking) {
            if (!voice) continue;

            current.pcm = heap_caps_malloc(max_samples * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (current.pcm == NULL) {
                current.pcm = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
            }
            if (current.pcm == NULL) {
                ESP_LOGE(TAG, "ASR utterance buffer malloc failed");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            current.samples = 0;
            speaking = true;
            silence_ms = 0;
            ESP_LOGI(TAG, "DIALOG VAD: speech start, peak=%ld", (long)peak);
        }

        size_t remain = max_samples - current.samples;
        size_t copy_count = count < remain ? count : remain;
        memcpy(current.pcm + current.samples, samples, copy_count * sizeof(int16_t));
        current.samples += copy_count;

        if (voice) silence_ms = 0;
        else silence_ms += frame_ms;

        bool max_reached = current.samples >= max_samples;
        bool silence_reached = silence_ms >= CONFIG_ASR_VAD_SILENCE_MS;
        if (max_reached || silence_reached) {
            if (current.samples >= min_samples) {
                ESP_LOGI(TAG, "DIALOG VAD: speech end, samples=%u silence=%lums",
                         (unsigned)current.samples,
                         (unsigned long)silence_ms);
                submit_utterance(&current);
            } else {
                ESP_LOGI(TAG, "DIALOG VAD: ignore short segment");
                free_utterance(&current);
            }
            speaking = false;
            silence_ms = 0;
        }
    }

    free_utterance(&current);
    vTaskDelete(NULL);
}

static void asr_upload_task(void *arg)
{
    (void)arg;
    asr_utterance_t u = {0};
    char text[ASR_TEXT_MAX];

    while (s_running) {
        if (xQueueReceive(s_asr_upload_queue, &u, portMAX_DELAY) != pdTRUE) continue;

        if (strlen(CONFIG_ASR_SERVER_URL) == 0U) {
            ESP_LOGW(TAG, "ASR server URL 未配置，丢弃语音");
            free_utterance(&u);
            continue;
        }

        esp_err_t err = asr_client_recognize(CONFIG_ASR_SERVER_URL,
                                             u.pcm,
                                             u.samples,
                                             text,
                                             sizeof(text));
        if (err == ESP_OK) {
            if (text[0] != '\0') {
                ESP_LOGI(TAG, "====================================================");
                ESP_LOGI(TAG, "ASR TEXT: %s", text);
                ESP_LOGI(TAG, "====================================================");

                if (s_dialog_mode && is_exit_dialog_text(text)) {
                    ESP_LOGI(TAG, "ASR CONTROL: 收到退出对话模式，交给本地状态机执行");
                    s_asr_exit_dialog_requested = true;
                }
            } else {
                ESP_LOGI(TAG, "ASR TEXT: (empty)");
            }
        } else {
            ESP_LOGE(TAG, "ASR request failed: %s", esp_err_to_name(err));
        }
        free_utterance(&u);
    }

    vTaskDelete(NULL);
}

static esp_err_t start_asr_runtime(void)
{
    if (s_asr_stream != NULL && s_asr_upload_queue != NULL) return ESP_OK;

    log_memory("before ASR runtime");

    /*
     * xStreamBufferCreate() 默认通过 FreeRTOS heap 申请数据区，容易占用内部 RAM。
     * 这里显式把 32KB PCM 环形缓冲区放进 8MB PSRAM，只把很小的控制块留在内部 RAM。
     */
    s_asr_stream_storage = heap_caps_malloc(ASR_STREAM_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_asr_stream_storage == NULL) {
        ESP_LOGE(TAG, "ASR PSRAM stream storage malloc failed (%u bytes)",
                 (unsigned)ASR_STREAM_BYTES);
        return ESP_ERR_NO_MEM;
    }

    s_asr_stream = xStreamBufferCreateStatic(ASR_STREAM_BYTES,
                                             1,
                                             s_asr_stream_storage,
                                             &s_asr_stream_struct);
    if (s_asr_stream == NULL) {
        ESP_LOGE(TAG, "ASR static StreamBuffer create failed");
        heap_caps_free(s_asr_stream_storage);
        s_asr_stream_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_asr_upload_queue = xQueueCreate(ASR_UPLOAD_QUEUE_DEPTH, sizeof(asr_utterance_t));
    if (s_asr_upload_queue == NULL) {
        ESP_LOGE(TAG, "ASR upload queue create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t capture_ok = xTaskCreatePinnedToCore(dialog_capture_task,
                                                    "dialog_capture",
                                                    4 * 1024,
                                                    NULL,
                                                    4,
                                                    NULL,
                                                    0);
    BaseType_t upload_ok = xTaskCreatePinnedToCore(asr_upload_task,
                                                   "asr_upload",
                                                   6 * 1024,
                                                   NULL,
                                                   3,
                                                   NULL,
                                                   0);
    if (capture_ok != pdPASS || upload_ok != pdPASS) {
        ESP_LOGE(TAG,
                 "Create ASR tasks failed: capture=%ld upload=%ld",
                 (long)capture_ok,
                 (long)upload_ok);
        return ESP_ERR_NO_MEM;
    }

    log_memory("after ASR runtime");
    ESP_LOGI(TAG, "ASR runtime ready: 32KB PCM stream in PSRAM");
    return ESP_OK;
}

static esp_err_t configure_multinet_commands(void)
{
    ESP_RETURN_ON_ERROR(esp_mn_commands_clear(), TAG, "clear commands");
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i) {
        ESP_RETURN_ON_ERROR(esp_mn_commands_add(s_commands[i].command_id,
                                                s_commands[i].pinyin),
                            TAG,
                            "add command");
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err != NULL) return ESP_ERR_INVALID_ARG;
    esp_mn_active_commands_print();
    return ESP_OK;
}

static void speech_detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;
    const int fetch_samples = s_afe_handle->get_fetch_chunksize(afe_data);

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "No Chinese MultiNet model");
        vTaskDelete(NULL);
        return;
    }

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    if (multinet == NULL) {
        vTaskDelete(NULL);
        return;
    }

    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    if (model_data == NULL) {
        vTaskDelete(NULL);
        return;
    }
    assert(multinet->get_samp_chunksize(model_data) == fetch_samples);

    if (configure_multinet_commands() != ESP_OK) {
        multinet->destroy(model_data);
        vTaskDelete(NULL);
        return;
    }

    bool command_mode = false;
    bool speaker_pause_seen = false;
    s_detector_ready = true;

    ESP_LOGI(TAG, "READY: 你好小智 -> 命令；进入对话模式 -> 任意语音送 FunASR");

    while (s_running) {
        if (s_asr_exit_dialog_requested) {
            s_asr_exit_dialog_requested = false;

            if (s_dialog_mode) {
                ESP_LOGI(TAG, "ASR CONTROL: 开始执行退出对话模式");
                s_dialog_mode = false;
                multinet->clean(model_data);
                command_mode = false;
                s_afe_handle->enable_wakenet(afe_data);
                if (s_asr_stream != NULL) {
                    xStreamBufferReset(s_asr_stream);
                }
                ESP_LOGI(TAG, "DIALOG MODE -> OFF");
                ESP_LOGI(TAG, "重新等待唤醒词：你好小智");
                ESP_LOGI(TAG, "退出动作已完成，播放退出确认语音");
                queue_tts_text("退出对话模式");
            }
            continue;
        }

        if (s_speaker_playing) {
            speaker_pause_seen = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (speaker_pause_seen) {
            vTaskDelay(pdMS_TO_TICKS(AFE_RESUME_GUARD_MS));
            speaker_pause_seen = false;
        }

        afe_fetch_result_t *result = s_afe_handle->fetch(afe_data);
        if (result == NULL) break;
        if (result->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s_speaker_playing) {
            speaker_pause_seen = true;
            continue;
        }

        if (!command_mode && result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "WAKE WORD DETECTED: 你好小智");
            s_afe_handle->disable_wakenet(afe_data);
            multinet->clean(model_data);
            command_mode = true;
            queue_tts_text(WAKE_REPLY_TTS_TEXT);
            continue;
        }

        if (!command_mode) continue;

        esp_mn_state_t state = multinet->detect(model_data, result->data);
        if (state == ESP_MN_STATE_DETECTING) continue;

        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = multinet->get_results(model_data);
            if (r != NULL && r->num > 0) {
                int command_id = r->command_id[0];
                ESP_LOGI(TAG, "MultiNet TOP1 id=%d prob=%.3f text=%s",
                         command_id,
                         r->prob[0],
                         r->string != NULL ? r->string : "(null)");

                /*
                 * 真正对话模式中，普通说话只交给 ASR。
                 * MultiNet 仍在后台保留“退出对话模式/退出语音”两个本地逃生命令。
                 */
                if (s_dialog_mode &&
                    command_id != CMD_EXIT_DIALOG_MODE &&
                    command_id != CMD_EXIT_VOICE) {
                    ESP_LOGI(TAG, "DIALOG MODE: 忽略本地固定命令匹配，交给 ASR 文本识别");
                    multinet->clean(model_data);
                    continue;
                }

                bool end_round = execute_voice_command(command_id);
                if (end_round) {
                    multinet->clean(model_data);
                    if (s_dialog_mode) {
                        command_mode = true;
                    } else {
                        command_mode = false;
                        s_afe_handle->enable_wakenet(afe_data);
                        ESP_LOGI(TAG, "重新等待唤醒词：你好小智");
                    }
                }
            }
            continue;
        }

        if (state == ESP_MN_STATE_TIMEOUT) {
            multinet->clean(model_data);
            if (s_dialog_mode) {
                command_mode = true;
            } else {
                command_mode = false;
                s_afe_handle->enable_wakenet(afe_data);
                ESP_LOGI(TAG, "COMMAND TIMEOUT -> 重新等待你好小智");
            }
        }
    }

    s_detector_ready = false;
    multinet->destroy(model_data);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "ESP32-S3 VOICE + CAMERA + ChatTTS + FunASR DIALOG");
    ESP_LOGI(TAG, "ASR: %s", strlen(CONFIG_ASR_SERVER_URL) ? CONFIG_ASR_SERVER_URL : "(not configured)");
    ESP_LOGI(TAG, "====================================================");

    log_memory("boot");

    ESP_ERROR_CHECK(camera_capture_init());
    ESP_ERROR_CHECK(init_mic_i2s());
    ESP_ERROR_CHECK(init_speaker_i2s());

    ESP_ERROR_CHECK(tts_service_start(s_speaker_tx,
                                      CONFIG_TTS_SERVER_URL,
                                      s_speaker_mutex,
                                      &s_speaker_playing));

    /*
     * 先完全恢复旧版已经验证过的 ESP-SR 启动路径。
     * 新增 ASR StreamBuffer/Queue/Task 暂时不创建，避免在 esp_wifi_init() 前抢内部 RAM。
     */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init(model) failed");
        return;
    }

    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }
    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (s_afe_handle == NULL) {
        afe_config_free(afe_cfg);
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
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

    log_memory("before Wi-Fi");

    /*
     * 关键：Wi-Fi 必须在新增 ASR runtime 之前先拿到它需要的内部 DMA/RX buffer。
     * 不再 ESP_ERROR_CHECK，避免内存异常时进入无限重启循环，保留完整错误日志。
     */
    esp_err_t photo_err = photo_http_server_start();
    if (photo_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "photo_http_server_start failed: %s (0x%x). Stop boot instead of reboot loop.",
                 esp_err_to_name(photo_err),
                 (unsigned)photo_err);
        log_memory("Wi-Fi FAILED");
        return;
    }

    log_memory("after Wi-Fi");

    /* Wi-Fi 成功以后才追加 ASR runtime。失败时保留原有 WakeNet/Camera/TTS 功能。 */
    esp_err_t asr_runtime_err = start_asr_runtime();
    if (asr_runtime_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ASR runtime disabled because memory allocation failed: %s",
                 esp_err_to_name(asr_runtime_err));
    }

    esp_err_t uplink = ESP_OK;
    if (strlen(CONFIG_TTS_SERVER_URL) > 0U || strlen(CONFIG_ASR_SERVER_URL) > 0U) {
        uplink = wait_uplink_ready(STARTUP_TTS_WAIT_MS);
        if (uplink != ESP_OK) {
            ESP_LOGW(TAG, "STA uplink not ready: %s", esp_err_to_name(uplink));
        }
    }

    ESP_LOGI(TAG, "SYSTEM READY");
    ESP_LOGI(TAG, "Voice : 你好小智 -> 固定命令");
    ESP_LOGI(TAG, "Dialog: 进入对话模式 -> VAD -> PCM -> FunASR -> 串口 ASR TEXT");
    ESP_LOGI(TAG, "Exit  : 对话模式里仍可说‘退出对话模式’");

    if (strlen(CONFIG_TTS_SERVER_URL) > 0U && uplink == ESP_OK) {
        queue_tts_text(STARTUP_TTS_TEXT);
    }
}
