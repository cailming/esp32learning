# ESP32-S3-CAM ↔ RTX3090 ChatTTS 联调说明

## 1. 当前链路

```text
“你好小智”
    ↓
“状态查询”
    ↓
ESP32 生成状态文本
    ↓
HTTP POST /tts/stream
    ↓
Ubuntu + RTX3090 / ChatTTS
    ↓
24kHz / PCM16 / mono 流
    ↓
ESP32 边接收边播放
    ↓
I2S1 24kHz / stereo framing
    ↓
MAX98357A → 喇叭
```

当前状态播报示例：

```text
设备正常，当前未录像，声音已开启。
```

TTS 请求固定使用：

```json
{
  "text": "设备正常，当前未录像，声音已开启。",
  "lang": "zh",
  "skip_refine_text": true
}
```

## 2. ChatTTS Server 推荐配置

RTX3090 实测推荐：

```text
CHATTTS_STREAM_BATCH=24
CHATTTS_STREAM_SPEED=12000
CHATTTS_PASS_FIRST_N_BATCHES=0
```

现在 `ttsserver/app.py` 已经把 `pass_first_n_batches=0` 作为默认值，并默认执行一次 startup warm-up。

启动：

```bash
cd ttsserver
conda activate chattts
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

检查：

```bash
curl http://127.0.0.1:8000/health
```

RTX3090 应看到：

```json
"device": "cuda:0"
```

并确认：

```json
"stream": {
  "batch": 24,
  "speed": 12000,
  "pass_first_n_batches": 0
}
```

## 3. ESP32 网络模式

原工程只有：

```text
SoftAP: S3CAM-Photo
IP:     192.168.4.1
```

现在增加了可选 **AP+STA**：

- AP 继续保留，所以原拍照网页仍然是 `http://192.168.4.1/`。
- STA 用来连接现有局域网，从而访问 RTX3090 TTS Server。

## 4. ESP32 配置

进入工程：

```bash
cd s3cam/mic_wake_command_test
idf.py menuconfig
```

进入：

```text
ChatTTS / uplink network
```

配置：

```text
ChatTTS streaming URL
Enable Wi-Fi STA uplink in addition to S3CAM-Photo SoftAP
Uplink Wi-Fi SSID
Uplink Wi-Fi password
```

例如 RTX3090 的局域网 IP 是 `192.168.1.20`：

```text
ChatTTS streaming URL = http://192.168.1.20:8000/tts/stream
```

> 不要填写 `127.0.0.1`。对 ESP32 来说，`127.0.0.1` 指的是 ESP32 自己，而不是 Ubuntu。

## 5. Ubuntu 获取局域网 IP

Ubuntu 上可执行：

```bash
hostname -I
```

或：

```bash
ip addr
```

选择 ESP32 所在局域网能够访问的 IPv4 地址。

先从同一局域网另一台电脑验证：

```bash
curl http://<RTX3090-IP>:8000/health
```

如果本机 `127.0.0.1:8000` 正常、其他机器无法访问，检查 Ubuntu 防火墙：

```bash
sudo ufw status
```

必要时开放：

```bash
sudo ufw allow 8000/tcp
```

## 6. ESP32 构建与烧录

Intel Mac / ESP-IDF 6.1 环境：

```bash
source /Users/apple/.espressif/tools/activate_idf_v6.1.sh
export PATH="/Users/apple/.espressif/tools/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH"
```

然后：

```bash
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 7. 预期启动日志

启用 STA 后应该先看到：

```text
Wi-Fi SoftAP started
STA uplink enabled: connecting to <SSID>
STA uplink GOT IP: x.x.x.x
```

然后：

```text
SYSTEM READY
TTS : http://<RTX3090-IP>:8000/tts/stream
```

## 8. 状态查询预期日志

说：

```text
你好小智
状态查询
```

ESP32 应出现：

```text
COMMAND DETECTED: ... 状态查询
ACTION: STATUS ...
TTS: begin -> http://<RTX3090-IP>:8000/tts/stream
POST http://<RTX3090-IP>:8000/tts/stream
Speaker I2S clock -> 24000 Hz
First PCM received: ...
TTS finished: ...
Speaker I2S clock -> 16000 Hz
ACTION: 状态查询结束，重新等待唤醒词
```

同时喇叭应播报类似：

```text
设备正常，当前未录像，声音已开启。
```

## 9. Speaker / MIC 并发处理

快门音和 TTS 共用一个 Speaker mutex，避免同时修改 I2S1 时钟。

Speaker 播放期间：

- INMP441 / I2S0 仍持续读取；
- 但向 ESP-SR AFE 喂 0；
- 避免喇叭声音重新触发 WakeNet / MultiNet；
- 播放结束后保留 120ms 尾音保护，再恢复真实 MIC 音频。

## 10. PCM 流处理

ChatTTS Server 输出：

```text
24000 Hz
16-bit signed little-endian
mono
```

MAX98357A 当前 I2S 使用 stereo framing，因此 ESP32 会：

```text
mono sample
   ↓
复制为 L/R
   ↓
stereo I2S frame
```

另外 TCP/HTTP read 的边界不保证落在 16-bit sample 边界上，因此 `tts_client.c` 会保留一次 read 末尾可能出现的 1 个字节，并和下一次 read 的第 1 个字节重新组成完整 `int16_t` sample。
