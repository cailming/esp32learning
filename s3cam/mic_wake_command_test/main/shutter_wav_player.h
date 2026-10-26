#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放固件中嵌入的真实相机快门 WAV。
 *
 * CMake 会把 sounds/shutter.wav 通过 EMBED_FILES 嵌入固件。
 * 当前素材为 44.1kHz / 16-bit / mono PCM WAV；本函数会在播放时
 * 重采样为当前 MAX98357A I2S1 使用的 16kHz / 16-bit / stereo。
 *
 * @param tx_chan 已经初始化并 enable 的 I2S TX channel。
 * @return ESP_OK 播放成功；其他值表示 WAV 格式或 I2S 写入失败。
 */
esp_err_t shutter_wav_play(i2s_chan_handle_t tx_chan);

#ifdef __cplusplus
}
#endif
