# mic_wake_command_test

这个工程现在已经从“语音拍照指令模拟”升级为**真实 Camera 拍照**。

完整链路：

```text
INMP441
   ↓
I2S0 RX / 16kHz
   ↓
ESP-SR AFE
   ↓
WakeNet10：你好小智
   ↓
MultiNet6：拍照
   ↓
OV3660 真正抓取 QVGA JPEG
   ↓
校验 JPEG
   ↓
复制并保留为“最新照片”
   ↓
拍照成功后播放真实 shutter.wav
   ↓
I2S1 → MAX98357A → Speaker
   ↓
重新等待“你好小智”
```

## 当前能力

- WakeNet10：`你好小智`
- MultiNet6：中文离线固定命令
- INMP441：持续语音输入
- OV3660：真实 JPEG 拍照
- MAX98357A：真实 WAV 快门音
- 最新一张 JPEG：保留在 PSRAM / 内存中，供后续 HTTP、上传或保存功能复用

## 模型配置

```text
ESP-SR ^2.6.0
WakeNet10：wn10_nihaoxiaozhi
MultiNet6：中文命令词
```

`sdkconfig.defaults`：

```text
CONFIG_SR_WN_WN10_NIHAOXIAOZHI=y
CONFIG_SR_MN_CN_MULTINET6_QUANT=y
```

## Camera

使用 `espressif/esp32-camera ^2.1.8`。

当前 Camera 配置沿用已经在 `s3cam/camera_test` 和 `camera_boot_http_test` 中验证通过的参数：

```text
Sensor     : OV3660
Pixel      : JPEG
Frame size : QVGA 320x240
JPEG quality: 12
XCLK       : 20MHz
Framebuffer: 1 个，DRAM
```

板载 Camera GPIO：

```text
XCLK  GPIO15
SIOD  GPIO4
SIOC  GPIO5
D0    GPIO11
D1    GPIO9
D2    GPIO8
D3    GPIO10
D4    GPIO12
D5    GPIO18
D6    GPIO17
D7    GPIO16
VSYNC GPIO6
HREF  GPIO7
PCLK  GPIO13
PWDN  -1
RESET -1
```

Camera 相关正式实现位于：

```text
main/camera_capture.c
main/camera_capture.h
```

## “拍照”命令的正式行为

现在说：

```text
你好小智
拍照
```

程序执行：

```text
camera_capture_one()
   ↓
esp_camera_fb_get()
   ↓
检查 PIXFORMAT_JPEG + JPEG SOI(FF D8)
   ↓
复制 Camera framebuffer
   ↓
保存为最新 JPEG
   ↓
成功序号 +1
   ↓
播放 shutter.wav
```

只有真正拍照成功以后才会播放快门声。

如果 Camera 抓拍失败：

```text
不增加成功拍照计数
不替换上一张有效照片
不播放快门音
重新等待“你好小智”
```

因此“咔嚓”现在代表：**照片已经真实拍摄并保存到内存。**

## 最新照片缓存

Camera 驱动返回的 `fb->buf` 在：

```c
esp_camera_fb_return(fb);
```

之后不能继续使用，所以正式实现不会直接保存 framebuffer 指针，而是复制一份 JPEG。

内存策略：

```text
优先 PSRAM
   ↓ 失败
回退内部内存
```

模块始终只保存**最新一张**照片：

```text
第 1 张成功 → 保存 #1
第 2 张成功 → 释放 #1，保存 #2
第 3 张成功 → 释放 #2，保存 #3
```

这样不会因为连续拍照无限占用内存。

后续可以直接基于：

```c
camera_capture_latest(...)
```

增加：

- HTTP `/photo.jpg`
- 手机 APP 获取照片
- Wi-Fi / BLE 业务上传
- SD 卡保存
- 图片识别

## 硬件接线

### INMP441

| INMP441 | ESP32-S3-CAM |
| --- | --- |
| SCK / BCLK | GPIO42 |
| WS / LRCLK | GPIO41 |
| SD | GPIO1 |
| VDD | 3V3 |
| GND | 公共 GND |
| L/R | GND |

```text
I2S0 RX
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

Speaker 黑线不要接 ESP32 GND。

## 语音命令

| ID | 说法 | MultiNet 拼音 | 当前动作 |
| ---: | --- | --- | --- |
| 1 | 拍照 | `pai zhao` | **真实 Camera 拍 JPEG，成功后播放快门 WAV** |
| 2 | 开始录像 | `kai shi lu xiang` | 当前仍为录像状态占位 |
| 3 | 停止录像 | `ting zhi lu xiang` | 停止录像状态 |
| 4 | 打开声音 | `da kai sheng yin` | 打开快门音 |
| 5 | 关闭声音 | `guan bi sheng yin` | 关闭快门音；仍会真实拍照 |
| 6 | 状态查询 | `zhuang tai cha xun` | 打印拍照数及最新照片信息 |
| 7 | 退出语音 | `tui chu yu yin` | 返回等待唤醒 |

## 快门 WAV

使用真实 `shutter.wav`，构建时下载到：

```text
main/shutter.wav
```

通过：

```cmake
EMBED_FILES "shutter.wav"
```

嵌入固件。

播放器位于：

```text
main/shutter_wav_player.c
```

当前策略：

```text
WAV 44.1kHz mono PCM
   ↓
I2S1 临时切到 44.1kHz
   ↓
原采样率播放，不做 44.1kHz→16kHz 简易降采样
   ↓
播放完成
   ↓
I2S1 恢复 16kHz idle
```

这样避免之前高频瞬态降采样后出现类似“机关枪”的失真。

## 防止 Speaker 回灌语音识别

播放快门音期间：

```text
INMP441 仍持续读取
   ↓
audio_feed_task 向 AFE 临时喂 0
   ↓
快门音不进入 WakeNet / MultiNet
```

另外，“拍照”仍然采用 one-shot：

```text
你好小智
   ↓
拍照
   ↓
真实抓拍 + 快门音
   ↓
立即退出 MultiNet
   ↓
重新等待你好小智
```

## 编译

本次新增了 `esp32-camera` 依赖和新的 Camera 源文件，建议干净编译：

```bash
cd ~/dev/esp32/esp32learning/s3cam/mic_wake_command_test

git pull
rm -rf build
idf.py build
```

如果依赖锁文件需要更新，ESP-IDF Component Manager 会在配置阶段处理。

烧录：

```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 正常启动日志重点

应该能看到类似：

```text
Camera ready: PID=0x3660 ... QVGA JPEG quality=12
Camera warm-up frame: 320x240, ... bytes
MIC I2S ready: 16kHz ...
Speaker I2S ready: 16kHz idle ...
READY: 等待唤醒词 -> 你好小智
```

## 正常拍照日志

说：

```text
你好小智
拍照
```

期望：

```text
WAKE WORD DETECTED: 你好小智
COMMAND DETECTED: id=1, name=拍照
ACTION: Camera 开始正式拍照...
PHOTO CAPTURED: #1 320x240 JPEG ... bytes
ACTION: PHOTO OK #1, 320x240, JPEG=... bytes
SHUTTER WAV: photo success -> play once
ACTION: 最新 JPEG 已保留在内存中，可供后续 HTTP/上传使用
重新等待唤醒词：你好小智
```

如果 Camera 失败，则应该看到：

```text
ACTION: 拍照失败，不播放快门音: ...
```

不会伪装成拍照成功。
