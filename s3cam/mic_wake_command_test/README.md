# mic_wake_command_test

这个工程用于验证当前 ESP32-S3-CAM + INMP441 的完整离线语音链路：

```text
INMP441
   ↓
I2S0 RX
   ↓
16kHz / 16-bit / mono
   ↓
ESP-SR AFE
   ↓
WakeNet（Hi 乐鑫）
   ↓
MultiNet6 中文命令识别
   ↓
execute_voice_command()
   ↓
串口 ACTION 日志 / 状态变化
```

## 为什么使用 ESP-SR

ESP32-S3 可以直接运行乐鑫官方 ESP-SR：

- AFE：音频前端；
- WakeNet：离线唤醒词；
- MultiNet：离线固定命令词识别。

这个测试不需要 Wi-Fi，也不需要云端 ASR。

当前工程使用：

```text
ESP-SR ^2.6.0
WakeNet9：Hi 乐鑫
MultiNet6：中文命令词
```

选择较新的 ESP-SR 是因为 2.5.0 起已包含 ESP-IDF 6.1 Kconfig 兼容修复。

## 硬件接线

保持现有所有硬件完整接线，不需要拔 Camera、MAX98357A 或 Speaker。

本程序只初始化 INMP441：

| INMP441 | ESP32-S3-CAM |
| --- | --- |
| SCK / BCLK | GPIO42 |
| WS / LRCLK | GPIO41 |
| SD | GPIO1 |
| VDD | 3V3 |
| GND | 公共 GND |
| L/R | 公共 GND |

前面的 `mic_test` 已经确认：

```text
LEFT  channel = 有效麦克风数据
RIGHT channel = 0
```

## 第一版支持的语音命令

唤醒词：

```text
Hi 乐鑫
```

唤醒成功后直接说中文：

| Command ID | 实际说法 | MultiNet 内部拼音 | 测试动作 |
| ---: | --- | --- | --- |
| 1 | 拍照 | pai zhao | `photo_request_count++` |
| 2 | 开始录像 | kai shi lu xiang | `video_recording=true` |
| 3 | 停止录像 | ting zhi lu xiang | `video_recording=false` |
| 4 | 打开声音 | da kai sheng yin | `sound_enabled=true` |
| 5 | 关闭声音 | guan bi sheng yin | `sound_enabled=false` |
| 6 | 状态查询 | zhuang tai cha xun | 打印当前状态 |
| 7 | 退出语音 | tui chu yu yin | 立即重新等待唤醒词 |

这里的“执行”目前是**测试级业务执行**：

- 命令已经由 MultiNet 识别成 `command_id`；
- `execute_voice_command()` 根据 ID 进入不同 `case`；
- 更新内存状态并输出明确的 `ACTION` 日志。

这一步通过以后，可以把对应 case 替换成真实动作，例如：

```text
“拍照”
   ↓
调用 Camera capture_one_photo()

“打开声音”
   ↓
启用 MAX98357A / 播放提示音
```

## Flash / PSRAM

当前板子前面的启动日志已经检测到：

```text
Flash physical size: 16MB
```

因此本工程配置为 16MB Flash，并专门创建 `model` 分区存放 ESP-SR 模型。

完整 WakeNet + MultiNet 对内存要求比普通 MIC 测试高很多，官方 ESP32-S3 speech-command 示例通常启用 PSRAM。本工程也暂按 ESP32-S3 Octal PSRAM 配置。

如果本板实际 PSRAM 类型不同，启动时可能在 PSRAM 初始化阶段报错。**遇到这种情况不要拆任何硬件线**，把启动日志发回来，再根据实际模组修改 PSRAM 配置。

## 分区表

`partitions.csv` 中包含：

```text
factory app : 2MB
model       : 6MB
```

ESP-SR 会把选中的 WakeNet / MultiNet 模型打包并烧录到 `model` 分区。

## 编译

第一次编译需要下载 ESP-SR Managed Component，所以电脑需要能访问 ESP Component Registry。

```bash
cd s3cam/mic_wake_command_test

rm -rf build
rm -f sdkconfig

idf.py set-target esp32s3
idf.py build
```

如果新终端里又出现：

```text
xtensa-esp32s3-elf-gcc not found
```

先恢复已经确认可用的 ESP-IDF 6.1 环境：

```bash
source /Users/apple/.espressif/tools/activate_idf_v6.1.sh
export PATH="/Users/apple/.espressif/tools/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH"
```

## 烧录并监控

```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 正常启动时重点观察

程序会依次完成：

```text
1. I2S0 / INMP441 初始化
2. ESP-SR model partition 加载
3. AFE 创建
4. audio_feed task 启动
5. speech_detect task 启动
6. 等待唤醒词
```

最终应看到类似：

```text
READY: 等待唤醒词 -> Hi 乐鑫
```

同时会周期性看到：

```text
MIC alive: peak=xxxx
```

说明 I2S 麦克风数据还在持续进入 AFE。

## 测试步骤

### 1. 唤醒

对着麦克风说：

```text
Hi 乐鑫
```

期望：

```text
WAKE WORD DETECTED: Hi 乐鑫
现在请说命令，例如：拍照 / 开始录像 / 状态查询
```

### 2. 说命令

例如说：

```text
拍照
```

期望看到类似：

```text
TOP1: command_id=1 ...
COMMAND DETECTED: id=1, name=拍照
ACTION: 拍照请求 #1
```

再说：

```text
开始录像
状态查询
```

期望：

```text
ACTION: 录像状态 -> START
ACTION: STATUS video=ON, sound=ON, photo_requests=1
```

### 3. 超时 / 退出

唤醒后约 6 秒没有有效命令，MultiNet 会超时并重新等待唤醒词。

也可以直接说：

```text
退出语音
```

立即回到：

```text
重新等待唤醒词：Hi 乐鑫
```

## 关于音频格式

INMP441 当前输出：

```text
16kHz
32-bit slot
stereo I2S framing
LEFT channel 有效
```

ESP-SR MultiNet 要求：

```text
16kHz
16-bit
mono
```

因此 `audio_feed_task()` 会把 LEFT `int32_t` 样本转换为 `int16_t`，再送入 AFE。

当前转换参数：

```c
#define MIC_SAMPLE_SHIFT 14
```

如果后面发现：

- `MIC alive peak` 经常接近 32767：说明可能太响/削顶，可改成 15 或 16；
- `peak` 长期非常小：可以尝试 13；

但第一轮测试先不要调整，先看实际日志。

## 当前测试边界

这个工程第一阶段只验证：

```text
麦克风采集
  +
唤醒词
  +
命令词
  +
命令 ID 分发
```

Camera / Speaker 仍保持物理连接，但程序暂时不初始化它们。

等这个测试通过后，再把它和已经通过的 Camera + Speaker 代码合起来，形成真正的：

```text
“Hi 乐鑫”
   ↓
“拍照”
   ↓
摄像头实际拍照

“打开声音”
   ↓
喇叭实际反馈
```
