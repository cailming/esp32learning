#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 将一段 16kHz / 16-bit / mono PCM 发送到 ASR 服务。
 *
 * @param server_url 例如 http://192.168.1.92:8001/asr
 * @param pcm        PCM16LE 样本
 * @param samples    样本数量，不是字节数
 * @param text       返回识别文本
 * @param text_size  text 缓冲区大小
 */
esp_err_t asr_client_recognize(const char *server_url,
                               const int16_t *pcm,
                               size_t samples,
                               char *text,
                               size_t text_size);

#ifdef __cplusplus
}
#endif
