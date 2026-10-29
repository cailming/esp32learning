#include "tts_service.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tts_client.h"

static const char *TAG = "tts_service";

#define TTS_JOB_TEXT_MAX         160U
#define TTS_QUEUE_LENGTH           1U
#define TTS_SERVICE_TASK_STACK    (14U * 1024U)
#define TTS_SERVICE_TASK_PRIORITY  5
#define TTS_SERVICE_TASK_CORE      1
#define TTS_ACOUSTIC_GUARD_MS    120U
#define TTS_SERVER_URL_MAX       256U

#define OLD_WAKE_REPLY_TEXT       "我在，有什么需要"
#define OLD_WAKE_REPLY_TEXT_ALT   "我在，有什么需求"
#define SHORT_WAKE_REPLY_TEXT     "在呢"
#define CHINESE_FULL_STOP         "。"

typedef struct {
    char text[TTS_JOB_TEXT_MAX];
} tts_job_t;

static QueueHandle_t s_queue = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;
static SemaphoreHandle_t s_speaker_mutex = NULL;
static volatile bool *s_speaker_guard = NULL;
static char s_server_url[TTS_SERVER_URL_MAX] = {0};

static void release_speaker_guard(void)
{
    if (s_speaker_guard != NULL) {
        *s_speaker_guard = false;
    }
}

static bool text_ends_with(const char *text, const char *suffix)
{
    const size_t text_len = strlen(text);
    const size_t suffix_len = strlen(suffix);

    if (suffix_len > text_len) {
        return false;
    }

    return memcmp(text + text_len - suffix_len, suffix, suffix_len) == 0;
}

static bool has_terminal_punctuation(const char *text)
{
    static const char *const endings[] = {
        "。", "！", "？", "；", "……", ".", "!", "?", ";",
    };

    for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); ++i) {
        if (text_ends_with(text, endings[i])) {
            return true;
        }
    }

    return false;
}

/*
 * 统一整理真正送给 ChatTTS 的文本：
 * 1. 旧版唤醒回复“我在，有什么需要/需求”统一缩短成“在呢”；
 * 2. 没有句末标点时自动补“。”，让 ChatTTS 有明确的收句信息。
 *
 * 第二点主要改善短提示词（例如“在呢”“拍照完成”“进入对话模式”）的尾音，
 * 避免模型把最后一个字当作仍在句中，从而产生“突然截掉”的听感。
 */
static esp_err_t prepare_tts_text(const char *input,
                                  char *output,
                                  size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *source = input;
    if (strcmp(input, OLD_WAKE_REPLY_TEXT) == 0 ||
        strcmp(input, OLD_WAKE_REPLY_TEXT_ALT) == 0) {
        source = SHORT_WAKE_REPLY_TEXT;
    }

    const size_t source_len = strlen(source);
    const bool add_full_stop = !has_terminal_punctuation(source);
    const size_t suffix_len = add_full_stop ? strlen(CHINESE_FULL_STOP) : 0U;

    if (source_len + suffix_len + 1U > output_size) {
        ESP_LOGE(TAG,
                 "Prepared TTS text too long: source=%u suffix=%u capacity=%u",
                 (unsigned)source_len,
                 (unsigned)suffix_len,
                 (unsigned)output_size);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(output, source, source_len);
    size_t offset = source_len;

    if (add_full_stop) {
        memcpy(output + offset, CHINESE_FULL_STOP, suffix_len);
        offset += suffix_len;
    }

    output[offset] = '\0';
    return ESP_OK;
}

static void tts_service_task(void *arg)
{
    (void)arg;

    tts_job_t job = {0};

    ESP_LOGI(TAG, "TTS service task ready");

    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Dequeued TTS job: %s", job.text);

        if (s_speaker_mutex == NULL ||
            xSemaphoreTake(s_speaker_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Cannot lock Speaker mutex");
            release_speaker_guard();
            continue;
        }

        esp_err_t err = tts_client_play(s_tx_chan,
                                        s_server_url,
                                        job.text);

        /*
         * tts_client_play() 已经负责把 I2S 尾部静音并 disable TX。
         * 这里再留一点声学保护时间，让物理喇叭余响衰减后再恢复 MIC -> AFE。
         */
        vTaskDelay(pdMS_TO_TICKS(TTS_ACOUSTIC_GUARD_MS));

        xSemaphoreGive(s_speaker_mutex);
        release_speaker_guard();

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TTS job complete");
        } else {
            ESP_LOGE(TAG,
                     "TTS job failed: %s",
                     esp_err_to_name(err));
        }
    }
}

esp_err_t tts_service_start(i2s_chan_handle_t tx_chan,
                            const char *server_url,
                            SemaphoreHandle_t speaker_mutex,
                            volatile bool *speaker_guard)
{
    if (tx_chan == NULL || server_url == NULL ||
        speaker_mutex == NULL || speaker_guard == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_queue != NULL) {
        return ESP_OK;
    }

    const size_t url_len = strlen(server_url);
    if (url_len >= sizeof(s_server_url)) {
        ESP_LOGE(TAG, "TTS server URL too long");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(s_server_url, server_url, url_len + 1U);
    s_tx_chan = tx_chan;
    s_speaker_mutex = speaker_mutex;
    s_speaker_guard = speaker_guard;
    *s_speaker_guard = false;

    s_queue = xQueueCreate(TTS_QUEUE_LENGTH, sizeof(tts_job_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Create TTS queue failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(tts_service_task,
                                                 "tts_service",
                                                 TTS_SERVICE_TASK_STACK,
                                                 NULL,
                                                 TTS_SERVICE_TASK_PRIORITY,
                                                 NULL,
                                                 TTS_SERVICE_TASK_CORE);
    if (task_ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "Create TTS service task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "TTS service started: queue=%u task_core=%d task_prio=%d url=%s",
             (unsigned)TTS_QUEUE_LENGTH,
             TTS_SERVICE_TASK_CORE,
             TTS_SERVICE_TASK_PRIORITY,
             s_server_url[0] != '\0' ? s_server_url : "(not configured)");

    return ESP_OK;
}

esp_err_t tts_service_enqueue(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL || s_tx_chan == NULL ||
        s_speaker_mutex == NULL || s_speaker_guard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server_url[0] == '\0') {
        ESP_LOGW(TAG, "TTS server URL is empty; reject enqueue");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * guard 在“入队前”就置 true，而不是等 worker 真正开始播放后再置。
     * 这样 speech_detect_task 一旦投递成功，audio_feed_task 就会马上暂停向 AFE
     * feed，避免命令处理返回到 WakeNet 后又把即将播放的 TTS 回灌进去。
     */
    if (*s_speaker_guard) {
        ESP_LOGW(TAG, "Speaker/TTS is busy; reject new TTS job");
        return ESP_ERR_INVALID_STATE;
    }

    tts_job_t job = {0};
    esp_err_t prepare_err = prepare_tts_text(text, job.text, sizeof(job.text));
    if (prepare_err != ESP_OK) {
        return prepare_err;
    }

    if (strcmp(job.text, text) != 0) {
        ESP_LOGI(TAG, "TTS text normalized: '%s' -> '%s'", text, job.text);
    }

    *s_speaker_guard = true;

    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        *s_speaker_guard = false;
        ESP_LOGW(TAG, "TTS queue is full; drop job");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Queued TTS job asynchronously: %s", job.text);
    return ESP_OK;
}
