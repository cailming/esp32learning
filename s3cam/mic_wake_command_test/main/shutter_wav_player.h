#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放固件中嵌入的真实相机快门 WAV。
 *
 * CMake 会把 shutter.wav 通过 EMBED_FILES 嵌入固件。
 * 当前素材为 44.1kHz / 16-bit / mono PCM WAV。
 *
 * 播放时会临时把独立的 Speaker I2S1 时钟切换到 WAV 原生 44.1kHz，
 * 不再做 44.1kHz -> 16kHz 软件重采样；播放完成后再恢复 16kHz。
 * MIC 使用的是独立 I2S0，因此不会影响 ESP-SR 的 16kHz 采音。
 *
 * @param tx_chan 已经初始化并 enable 的 I2S TX channel。
 * @return ESP_OK 播放成功；其他值表示 WAV 格式或 I2S 写入失败。
 */
esp_err_t shutter_wav_play(i2s_chan_handle_t tx_chan);

#ifdef __cplusplus
}
#endif
