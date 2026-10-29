#pragma once

#include <stdbool.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动独立的 TTS 服务任务。
 *
 * speech_detect_task 只负责把待播报文本放进队列；真正的 HTTP 请求、
 * ChatTTS PCM 接收和 Speaker 播放由 tts_service_task 异步完成。
 *
 * @param tx_chan 已初始化的 Speaker I2S TX channel。
 * @param server_url ChatTTS streaming URL；可以为空，但为空时 enqueue 会失败。
 * @param speaker_mutex TTS 与快门音共用的 Speaker 互斥锁。
 * @param speaker_guard 指向主程序的 Speaker guard。请求一入队就置 true，
 *                      从而立即暂停 MIC -> AFE feed，避免扬声器声音回灌识别。
 */
esp_err_t tts_service_start(i2s_chan_handle_t tx_chan,
                            const char *server_url,
                            SemaphoreHandle_t speaker_mutex,
                            volatile bool *speaker_guard);

/**
 * @brief 把一条 UTF-8 文本异步投递给 TTS worker。
 */
esp_err_t tts_service_enqueue(const char *text);

/**
 * @brief 取得已经由 tts_service_start() 注册的 Speaker 播放上下文。
 *
 * dialog_client 用它复用同一个 I2S TX、Speaker mutex 和 speaker guard，
 * 从而保证普通 TTS 提示音、快门音和 /dialog 返回语音不会互相抢占。
 */
esp_err_t tts_service_get_playback_context(i2s_chan_handle_t *tx_chan,
                                           SemaphoreHandle_t *speaker_mutex,
                                           volatile bool **speaker_guard);

#ifdef __cplusplus
}
#endif
