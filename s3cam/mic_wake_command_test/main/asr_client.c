#include "asr_client.h"

#include <stdio.h>
#include <string.h>

#include "dialog_client.h"
#include "esp_log.h"
#include "tts_service.h"

static const char *TAG = "asr_client";

#define DIALOG_URL_MAX 256U

static esp_err_t build_dialog_url(const char *asr_url,
                                  char *dialog_url,
                                  size_t dialog_url_size)
{
    if (asr_url == NULL || asr_url[0] == '\0' ||
        dialog_url == NULL || dialog_url_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char asr_suffix[] = "/asr";
    const char dialog_suffix[] = "/dialog";
    const size_t url_len = strlen(asr_url);
    const size_t asr_suffix_len = sizeof(asr_suffix) - 1U;

    if (url_len >= asr_suffix_len &&
        strcmp(asr_url + url_len - asr_suffix_len, asr_suffix) == 0) {
        const size_t prefix_len = url_len - asr_suffix_len;
        const size_t required = prefix_len + sizeof(dialog_suffix);
        if (required > dialog_url_size) return ESP_ERR_INVALID_SIZE;

        memcpy(dialog_url, asr_url, prefix_len);
        memcpy(dialog_url + prefix_len,
               dialog_suffix,
               sizeof(dialog_suffix));
        return ESP_OK;
    }

    if (url_len >= sizeof(dialog_suffix) - 1U &&
        strcmp(asr_url + url_len - (sizeof(dialog_suffix) - 1U), dialog_suffix) == 0) {
        if (url_len + 1U > dialog_url_size) return ESP_ERR_INVALID_SIZE;
        memcpy(dialog_url, asr_url, url_len + 1U);
        return ESP_OK;
    }

    int written = snprintf(dialog_url,
                           dialog_url_size,
                           "%s%s",
                           asr_url,
                           (url_len > 0U && asr_url[url_len - 1U] == '/')
                               ? "dialog"
                               : "/dialog");
    if (written < 0 || (size_t)written >= dialog_url_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t asr_client_recognize(const char *server_url,
                               const int16_t *pcm,
                               size_t samples,
                               char *text,
                               size_t text_size)
{
    if (server_url == NULL || server_url[0] == '\0' ||
        pcm == NULL || samples == 0U ||
        text == NULL || text_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    char dialog_url[DIALOG_URL_MAX];
    esp_err_t err = build_dialog_url(server_url,
                                     dialog_url,
                                     sizeof(dialog_url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Build /dialog URL failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_chan_handle_t tx_chan = NULL;
    SemaphoreHandle_t speaker_mutex = NULL;
    volatile bool *speaker_guard = NULL;
    err = tts_service_get_playback_context(&tx_chan,
                                           &speaker_mutex,
                                           &speaker_guard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "TTS playback context unavailable: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "ASR dialog bridge: %s -> %s",
             server_url,
             dialog_url);

    /*
     * 从本轮 HTTP 请求开始就暂停 MIC -> AFE/ASR，而不是等真正播放 TTS 时才暂停。
     * 这样一次对话严格保持 LISTEN -> PROCESSING -> PLAYING -> LISTEN，避免服务端
     * 正在做 ASR/TTS 时用户的后续声音继续进入 VAD 队列，重新出现 queue full。
     */
    *speaker_guard = true;

    err = dialog_client_exchange(dialog_url,
                                 pcm,
                                 samples,
                                 tx_chan,
                                 speaker_mutex,
                                 speaker_guard,
                                 text,
                                 text_size);

    /* dialog_client 正常播放结束会自己清 guard；错误/204 路径由这里兜底恢复。 */
    *speaker_guard = false;
    return err;
}
