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

    const size_t text_len = strlen(text);
    if (text_len >= TTS_JOB_TEXT_MAX) {
        ESP_LOGE(TAG,
                 "TTS text too long: %u bytes, max=%u",
                 (unsigned)text_len,
                 (unsigned)(TTS_JOB_TEXT_MAX - 1U));
        return ESP_ERR_INVALID_SIZE;
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
    memcpy(job.text, text, text_len + 1U);

    *s_speaker_guard = true;

    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        *s_speaker_guard = false;
        ESP_LOGW(TAG, "TTS queue is full; drop job");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Queued TTS job asynchronously: %s", job.text);
    return ESP_OK;
}
