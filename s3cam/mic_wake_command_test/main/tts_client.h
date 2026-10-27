#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 ChatTTS HTTP 服务获取 24kHz/16-bit/mono PCM，并播放到 I2S Speaker。
 *
 * 服务端接口：POST /tts/stream
 * 请求 JSON：{"text":"...","lang":"zh","skip_refine_text":true}
 * 响应 Body：raw PCM, signed 16-bit little-endian, mono, 24000 Hz。
 *
 * 当前实现内部会：
 *   1. POST 文本并读取 ChatTTS streaming PCM；
 *   2. 在 PSRAM 中创建约 48KB 的 FreeRTOS StreamBuffer；
 *   3. 先预缓冲约 0.5 秒 mono PCM；
 *   4. 创建独立 tts_pcm_play_task，把 HTTP producer 与 I2S consumer 解耦；
 *   5. 播放时把 mono sample 复制成 stereo framing，输出到 MAX98357A；
 *   6. 正确处理 HTTP read 的奇数字节边界；
 *   7. 若发生 underrun，主动补短静音而不是重复旧 DMA 数据；
 *   8. 播放结束后补尾部静音并直接 disable I2S TX，避免机关枪尾音。
 *
 * 注意：本函数本身仍是“同步等待本次 TTS 播放完成”的底层函数。
 * 上层业务不要直接从 speech_detect_task 调用；当前由 tts_service_task 调用，
 * speech_detect_task 只通过 tts_service_enqueue() 异步投递文字。
 *
 * @param tx_chan 已初始化的 I2S1 TX channel；可以处于 disabled 状态。
 * @param server_url 完整接口地址，例如 http://192.168.1.20:8000/tts/stream。
 * @param text 要播报的 UTF-8 文本。
 */
esp_err_t tts_client_play(i2s_chan_handle_t tx_chan,
                          const char *server_url,
                          const char *text);

#ifdef __cplusplus
}
#endif
