#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 ChatTTS HTTP 服务获取 24kHz/16-bit/mono PCM，并实时播放到 I2S Speaker。
 *
 * 服务端接口：POST /tts/stream
 * 请求 JSON：{"text":"...","lang":"zh","skip_refine_text":true}
 * 响应 Body：raw PCM, signed 16-bit little-endian, mono, 24000 Hz。
 *
 * 本函数会：
 *   1. POST 文本；
 *   2. 收到 HTTP 200 后把 I2S TX 从 16kHz 临时切到 24kHz；
 *   3. 边读取 HTTP PCM 边 mono -> stereo 写入 MAX98357A；
 *   4. 正确处理 TCP/HTTP read 可能返回奇数字节的问题；
 *   5. 播放完成或失败后恢复 I2S TX 到 16kHz。
 *
 * @param tx_chan 已初始化并 enable 的 I2S1 TX channel。
 * @param server_url 完整接口地址，例如 http://192.168.1.20:8000/tts/stream。
 * @param text 要播报的 UTF-8 文本。
 */
esp_err_t tts_client_play(i2s_chan_handle_t tx_chan,
                          const char *server_url,
                          const char *text);

#ifdef __cplusplus
}
#endif
