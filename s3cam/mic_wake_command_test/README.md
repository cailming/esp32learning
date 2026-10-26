# mic_wake_command_test

这个工程用于验证当前 ESP32-S3-CAM 的离线语音交互链路，并在识别到“拍照”命令后通过 MAX98357A + Speaker 播放**真实 WAV 快门音**。

```text
INMP441
   ↓
I2S0 RX
   ↓
16kHz / 16-bit / mono
   ↓
ESP-SR AFE
   ↓
WakeNet10（你好小智）
   ↓
MultiNet6 中文命令识别
   ↓
execute_voice_command()
   ↓
“拍照”
   ↓
embedded shutter.wav
   ↓
I2S1 TX → MAX98357A → Speaker
```

## 当前模型

```text
ESP-SR ^2.6.0
WakeNet10：你好小智
WakeNet10 模型：wn10_nihaoxiaozhi
MultiNet6：中文命令词
```

`sdkconfig.defaults`：

```text
CONFIG_SR_WN_WN10_NIHAOXIAOZHI=y
CONFIG_SR_MN_CN_MULTINET6_QUANT=y
```

## 硬件接线

所有模块继续保持现有接线，不需要为了这个测试拔 Camera、MIC 或 Speaker。

### INMP441

| INMP441 | ESP32-S3-CAM |
| --- | --- |
| SCK / BCLK | GPIO42 |
| WS / LRCLK | GPIO41 |
| SD | GPIO1 |
| VDD | 3V3 |
| GND | 公共 GND |
| L/R | 公共 GND |

MIC 使用：

```text
I2S0 / RX
16kHz
32-bit stereo framing
LEFT channel 有效
```

### MAX98357A

| MAX98357A | ESP32-S3-CAM |
| --- | --- |
| BCLK | GPIO14 |
| LRC | GPIO3 |
| DIN | GPIO46 |
| VIN | 5V |
| GND | 公共 GND |

Speaker：

```text
红线 -> MAX98357A +
黑线 -> MAX98357A -
```

注意：Speaker 黑线不要接 ESP32 GND。

Speaker 使用：

```text
I2S1 / TX
16kHz
16-bit stereo
LEFT / RIGHT 发送相同数据
```

## 支持的语音命令

唤醒词：

```text
你好小智
```

唤醒后可以说：

| Command ID | 实际说法 | MultiNet 拼音 | 动作 |
| ---: | --- | --- | --- |
| 1 | 拍照 | pai zhao | 拍照请求计数 + 播放一次真实 WAV 快门音 |
| 2 | 开始录像 | kai shi lu xiang | `video_recording=true` |
| 3 | 停止录像 | ting zhi lu xiang | `video_recording=false` |
| 4 | 打开声音 | da kai sheng yin | 打开快门音 |
| 5 | 关闭声音 | guan bi sheng yin | 关闭快门音 |
| 6 | 状态查询 | zhuang tai cha xun | 打印当前状态 |
| 7 | 退出语音 | tui chu yu yin | 重新等待唤醒词 |

Camera 目前还没有在这个工程里真正初始化，因此“拍照”现在的动作是：

```text
识别“拍照”
   ↓
photo_request_count++
   ↓
播放 embedded shutter.wav
   ↓
立即退出命令模式
   ↓
重新等待“你好小智”
```

这样可以避免快门声被 MIC 听到后再次误触发“拍照”。

后续把 Camera 合进来以后，建议调整成：

```text
识别“拍照”
   ↓
Camera 真正拍照成功
   ↓
播放 shutter.wav
```

这样快门声代表“照片已经成功拍下”。

## 真实 WAV 快门音

当前不再用程序实时合成三角波、正弦波或随机噪声。

工程使用 Mozilla / Firefox DevTools 的真实 `shutter.wav` 资源，固定到指定 commit：

```text
https://raw.githubusercontent.com/mozilla/gecko-dev/5836a062726f715fda621338a17b51aff30d0a8c/devtools/client/themes/audio/shutter.wav
```

第一次 CMake 配置时会自动下载到：

```text
main/sounds/shutter.wav
```

随后：

```cmake
EMBED_FILES "sounds/shutter.wav"
```

会把 WAV 原始字节直接嵌入固件。

源码中对应的 linker symbol 是：

```text
_binary_sounds_shutter_wav_start
_binary_sounds_shutter_wav_end
```

`shutter_wav_player.c` 会：

```text
读取固件内 WAV
   ↓
解析 RIFF / WAVE
   ↓
找到 fmt / data chunk
   ↓
读取 16-bit mono PCM
   ↓
重采样到 16kHz
   ↓
复制为 LEFT / RIGHT stereo
   ↓
I2S1 → MAX98357A → Speaker
```

当前播放音量在：

```c
#define OUTPUT_GAIN_PERCENT 70
```

如果实际听起来太响，可以把 70 调小；太轻则可以适当调大。

资源说明见：

```text
main/sounds/README.md
```

## 避免 Speaker 声音干扰语音识别

当前 AFE 配置仍然是单麦克风：

```text
input_format = "M"
```

没有 Speaker playback reference，因此暂时没有使用真正的 AEC 回声消除。

播放 WAV 时采用两层保护：

```text
1. 进入 MultiNet 命令模式时关闭 WakeNet

2. Speaker 播放期间：
   INMP441 仍持续读取 I2S0
   但 audio_feed_task 向 ESP-SR AFE 喂 0
```

WAV 播放完以后还保留约 120ms 声学尾音保护时间，然后恢复正常 MIC 数据。

另外，“拍照”是 one-shot 命令：

```text
你好小智
   ↓
拍照
   ↓
播放一次 WAV
   ↓
退出 MultiNet
   ↓
重新等待你好小智
```

因此一次唤醒只会执行一次拍照。

## 编译

这次增加了新的 C 文件和 `EMBED_FILES` 资源，建议先干净编译一次：

```bash
cd s3cam/mic_wake_command_test

rm -rf build

idf.py build
```

如果当前目录没有正确的 `sdkconfig`，再执行：

```bash
rm -f sdkconfig
idf.py set-target esp32s3
idf.py build
```

第一次编译新的 WAV 版本时需要能访问：

```text
raw.githubusercontent.com
```

因为 CMake 会下载一次 `shutter.wav`。下载成功后文件会保留在 `main/sounds/`，之后普通增量编译不需要再次下载。

如果新终端缺少 ESP-IDF 6.1 环境：

```bash
source /Users/apple/.espressif/tools/activate_idf_v6.1.sh
export PATH="/Users/apple/.espressif/tools/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH"
```

## 烧录并监控

```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

启动后应看到：

```text
MIC I2S ready: 16kHz, BCLK=42, WS=41, DIN=1, LEFT channel
Speaker I2S ready: 16kHz, BCLK=14, LRC=3, DOUT=46
READY: 等待唤醒词 -> 你好小智
拍照命令：播放一次真实 shutter.wav，然后重新等待唤醒
```

## 推荐测试步骤

### 1. 唤醒

说：

```text
你好小智
```

期望：

```text
WAKE WORD DETECTED: 你好小智
```

### 2. 拍照 + WAV 快门音

说：

```text
拍照
```

期望串口：

```text
COMMAND DETECTED: id=1, name=拍照
ACTION: 拍照请求 #1
SHUTTER WAV: play once
```

随后 `shutter_wav` 模块会打印类似：

```text
Play embedded WAV: 44100 Hz mono 16-bit, source=... samples, output=... frames
```

Speaker 应只播放**一次**真实相机快门声。

播放完成后应看到：

```text
ACTION: 本次拍照结束，重新等待唤醒词
重新等待唤醒词：你好小智
```

### 3. 再次拍照

必须重新说：

```text
你好小智
拍照
```

才会播放第二次快门声。

### 4. 测试关闭声音

先说：

```text
你好小智
关闭声音
```

然后重新唤醒并说：

```text
你好小智
拍照
```

拍照命令仍然会执行，但 Speaker 不播放：

```text
SHUTTER WAV: 当前声音已关闭，跳过播放
```

## Flash / PSRAM

当前工程按：

```text
Flash：16MB
WakeNet10 + MultiNet6
PSRAM：启用
```

`partitions.csv` 中包含独立 `model` 分区，用来存放 ESP-SR 模型。

## 当前测试边界

目前已经覆盖：

```text
麦克风采集
+
WakeNet10 唤醒
+
MultiNet6 命令识别
+
命令 ID 分发
+
真实 WAV 资源嵌入
+
WAV 解析 / 重采样
+
MAX98357A / Speaker 播放
```

Camera 仍保持物理连接，但这个工程暂未初始化 Camera。

下一步可以把已经验证过的 Camera 代码合进来，形成真正的：

```text
“你好小智”
   ↓
“拍照”
   ↓
Camera 拍照
   ↓
真实 shutter.wav
```
