# mic_wake_command_test

这个工程用于验证当前 ESP32-S3-CAM 的离线语音交互链路，并在识别到“拍照”命令后通过 MAX98357A + Speaker 播放相机快门音。

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
I2S1 TX → MAX98357A → Speaker → “咔嚓”
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
| 1 | 拍照 | pai zhao | 拍照请求计数 + 播放“咔嚓”快门音 |
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
播放真实 Speaker 快门音
```

后续把 Camera 合进来以后，建议调整成：

```text
识别“拍照”
   ↓
Camera 真正拍照成功
   ↓
播放“咔嚓”
```

这样声音代表“照片已经成功拍下”。

## 快门音实现

当前没有使用 WAV 文件，而是在代码中实时合成两个很短的衰减噪声脉冲：

```text
“咔”  24ms 较强脉冲
  ↓
14ms 间隔
  ↓
“嚓”  48ms 较弱脉冲
  ↓
60ms 静音尾部
```

这样不需要 SPIFFS / LittleFS，也不需要额外音频文件。

快门音音量参数位于 `main.c`：

```c
#define SHUTTER_CLICK_AMPLITUDE 4000
#define SHUTTER_CLACK_AMPLITUDE 2800
```

如果觉得太响，可以减小；如果太轻，可以适当增大。

## 避免 Speaker 声音干扰语音识别

当前 AFE 配置是单麦克风：

```text
input_format = "M"
```

没有 Speaker playback reference，因此暂时没有使用真正的 AEC 回声消除。

播放快门音时采用一个简单保护：

```text
Speaker 播放期间
   ↓
INMP441 仍持续从 I2S0 读取
   ↓
但 audio_feed_task 向 ESP-SR AFE 喂 0
   ↓
快门声不会再次进入 WakeNet / MultiNet
```

播放结束后再自动恢复正常 MIC 数据。

## 编译

切换过 WakeNet 模型后，建议干净编译：

```bash
cd s3cam/mic_wake_command_test

rm -rf build
rm -f sdkconfig

idf.py set-target esp32s3
idf.py build
```

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
拍照命令会通过 MAX98357A 播放快门音
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

### 2. 拍照 + 快门音

说：

```text
拍照
```

期望串口：

```text
COMMAND DETECTED: id=1, name=拍照
ACTION: 拍照请求 #1
SHUTTER SOUND: 咔嚓
```

同时 Speaker 应实际听到一个很短的“咔嚓”声音。

### 3. 测试关闭声音

说：

```text
关闭声音
拍照
```

第二次“拍照”仍应识别成功并增加计数，但 Speaker 不播放：

```text
SHUTTER SOUND: 当前声音已关闭，跳过播放
```

然后说：

```text
打开声音
拍照
```

快门音恢复。

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
MAX98357A / Speaker 快门音
```

Camera 仍然保持物理连接，但这个工程暂未初始化 Camera。

下一步可以把已经验证过的 Camera 代码合进来，形成真正的：

```text
“你好小智”
   ↓
“拍照”
   ↓
Camera 拍照
   ↓
“咔嚓”
```
