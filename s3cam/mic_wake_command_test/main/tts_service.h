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
 *
 * 当前队列长度故意设为 1：设备语音提示是交互反馈，不应该无限堆积旧播报。
 * 如果 Speaker/TTS 已忙，返回 ESP_ERR_INVALID_STATE；队列发送失败返回超时。
 */
esp_err_t tts_service_enqueue(const char *text);

#ifdef __cplusplus
}
#endif
