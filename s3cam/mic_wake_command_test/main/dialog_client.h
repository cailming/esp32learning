#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 完成一次“用户语音 -> /dialog -> ASR 文本 -> TTS PCM -> Speaker”闭环。
 *
 * 请求：16kHz / PCM16LE / mono。
 * 响应：24kHz / PCM16LE / mono，HTTP body 流式播放到 MAX98357A。
 * 服务端同时通过 X-ASR-Text-B64 返回识别文本，本函数会解码到 text。
 *
 * @param server_url /dialog 完整 URL。
 * @param pcm 用户语音 PCM16。
 * @param samples PCM sample 数量。
 * @param tx_chan 已初始化但通常处于 disabled 的 Speaker I2S TX channel。
 * @param speaker_mutex 与 TTS/快门音共用的 Speaker mutex。
 * @param speaker_guard 主程序的扬声器保护标志；播放期间置 true，避免麦克风回灌。
 * @param text 输出 ASR UTF-8 文本。
 * @param text_size text 缓冲区大小。
 */
esp_err_t dialog_client_exchange(const char *server_url,
                                 const int16_t *pcm,
                                 size_t samples,
                                 i2s_chan_handle_t tx_chan,
                                 SemaphoreHandle_t speaker_mutex,
                                 volatile bool *speaker_guard,
                                 char *text,
                                 size_t text_size);

#ifdef __cplusplus
}
#endif
