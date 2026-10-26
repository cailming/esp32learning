# mic_wake_command_test

当前工程已经形成完整的离线语音拍照测试链路：

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
保存为最新照片
   ↓
MAX98357A 播放真实 shutter.wav
   ↓
浏览器自动显示最新照片
```

## 当前功能

- WakeNet10 唤醒词：`你好小智`
- MultiNet6 中文离线命令识别
- INMP441 麦克风输入
- OV3660 真正 JPEG 拍照
- MAX98357A + Speaker 真实 WAV 快门音
- Wi-Fi SoftAP
- HTTP 查看最新照片

## Wi-Fi / 浏览器

开发板启动后会创建：

```text
SSID     : S3CAM-Photo
Password : 12345678
```

连接后打开：

```text
http://192.168.4.1/
```

页面每 500ms 查询一次最新照片状态。说：

```text
你好小智
拍照
```

拍照成功后，页面会自动切换为最新 JPEG，不需要手工刷新。

也可以直接访问：

```text
http://192.168.4.1/photo.jpg
```

接口：

```text
GET /          网页
GET /status    最新照片元数据 JSON
GET /photo.jpg 最新 JPEG
```

当前没有启用 MJPEG 实时视频流，目的是减少 Wi-Fi、Camera 和 ESP-SR 同时运行时的资源占用。

## 拍照语义

“咔嚓”只代表真实拍照成功：

```text
识别“拍照”
   ↓
camera_capture_one()
   ↓
JPEG 抓取和校验成功？
   ├─ 否 → 不播放快门音
   └─ 是
       ↓
保存最新 JPEG
       ↓
播放 shutter.wav
```

`关闭声音`以后仍然会真正拍照，只是不播放快门音。

## 最新照片内存策略

Camera framebuffer 在 `esp_camera_fb_return()` 后不能继续使用，因此程序会复制 JPEG，并只保留最新一张：

```text
照片 #1 -> 保存
照片 #2 -> 替换 #1
照片 #3 -> 替换 #2
```

JPEG 优先放 PSRAM；如果 PSRAM 分配失败则回退内部内存。

HTTP 发送 `/photo.jpg` 时会再复制一份线程安全快照，因此即使浏览器正在下载时又触发下一次语音拍照，也不会出现旧 JPEG 被释放导致的竞态。

## Camera

当前沿用已经验证通过的 ESP32-S3-CAM / OV3660 配置：

```text
分辨率 : QVGA 320x240
格式   : JPEG
quality: 12
XCLK   : 20MHz
fb_count: 1
framebuffer: DRAM
```

Camera GPIO：

```text
XCLK  15
SIOD   4
SIOC   5
D0    11
D1     9
D2     8
D3    10
D4    12
D5    18
D6    17
D7    16
VSYNC  6
HREF   7
PCLK  13
PWDN  -1
RESET -1
```

## INMP441

```text
I2S0 RX
BCLK GPIO42
WS   GPIO41
DIN  GPIO1
16kHz
LEFT channel
```

L/R 接 GND。

## MAX98357A

```text
I2S1 TX
BCLK GPIO14
LRC  GPIO3
DIN  GPIO46
VIN  5V
GND  公共 GND
```

Speaker：

```text
红线 -> MAX98357A +
黑线 -> MAX98357A -
```

Speaker 黑线不要接 ESP32 GND。

快门 WAV 原始采样率为 44.1kHz。播放时 I2S1 临时切到 WAV 原生采样率，播完恢复 16kHz；不再做 44.1kHz → 16kHz 的简易实时降采样。

## ESP-SR

`sdkconfig.defaults`：

```text
CONFIG_SR_WN_WN10_NIHAOXIAOZHI=y
CONFIG_SR_MN_CN_MULTINET6_QUANT=y
```

当前命令：

| ID | 语音 | 动作 |
|---:|---|---|
| 1 | 拍照 | 真拍 JPEG + 成功后快门音 |
| 2 | 开始录像 | 当前仍为状态占位 |
| 3 | 停止录像 | 当前仍为状态占位 |
| 4 | 打开声音 | 打开快门音 |
| 5 | 关闭声音 | 关闭快门音，但仍拍照 |
| 6 | 状态查询 | 串口打印最新照片状态 |
| 7 | 退出语音 | 回到等待唤醒词 |

“拍照”采用 one-shot 模式：执行完成后立即退出 MultiNet，重新等待下一次“你好小智”。

## 编译

这次增加了 Wi-Fi、HTTP Server、HTML 嵌入和新的 C 文件，建议清理 build：

```bash
cd ~/dev/esp32/esp32learning/s3cam/mic_wake_command_test

rm -rf build
idf.py build
```

如果新终端还没有 ESP-IDF 6.1 环境：

```bash
source /Users/apple/.espressif/tools/activate_idf_v6.1.sh
export PATH="/Users/apple/.espressif/tools/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH"
```

烧录：

```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 期望启动日志

重点观察：

```text
Camera ready: PID=0x3660 ...
MIC I2S ready ...
Speaker I2S ready ...
READY: 等待唤醒词 -> 你好小智
Wi-Fi SoftAP started
SSID     : S3CAM-Photo
Password : 12345678
Web      : http://192.168.4.1/
SYSTEM READY
```

然后：

```text
你好小智
拍照
```

成功时应看到类似：

```text
PHOTO CAPTURED: #1 320x240 JPEG ... bytes
ACTION: PHOTO OK #1 ...
SHUTTER WAV: photo success -> play once
重新等待唤醒词：你好小智
```

浏览器页面随后自动显示这张照片。
