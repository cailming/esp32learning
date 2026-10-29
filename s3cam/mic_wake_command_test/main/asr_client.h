#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 完成一次对话模式 speech-to-speech 请求。
 *
 * 为保持 main_asr.c 现有接口不变，参数仍叫 server_url，并继续配置成 /asr；
 * 实现内部会自动把末尾 /asr 转成 /dialog：
 *
 *   ESP32 PCM16 16k -> /dialog -> FunASR -> ChatTTS -> PCM16 24k -> Speaker
 *
 * 函数返回前会完成服务端返回语音的播放，同时把 ASR 文本写入 text，
 * 所以现有串口日志仍然可以打印 `ASR TEXT: ...`。
 *
 * @param server_url 例如 http://192.168.1.103:8001/asr
 * @param pcm        PCM16LE 样本
 * @param samples    样本数量，不是字节数
 * @param text       返回 ASR UTF-8 文本
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
