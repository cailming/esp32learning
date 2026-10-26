# ESP32 ChatTTS 流式 TTS Server

这个目录提供一个运行在 macOS / PC 上的 Python TTS 服务。

目标链路：

```text
ESP32-S3
  │
  │ POST /tts/stream
  │ {"text":"设备正常，当前未录像。"}
  ▼
Python FastAPI
  │
  ▼
ChatTTS
  │
  │ 24kHz / 16bit / mono / PCM
  │ HTTP chunked stream
  ▼
ESP32-S3
  │
  │ 边接收边写 I2S
  ▼
MAX98357A -> Speaker
```

ESP32 不需要保存完整 WAV，也不需要等待整句话合成完成后再播放。
ChatTTS 每产生一段音频，HTTP 服务就立即把 PCM 字节发送给 ESP32。

---

## 1. 当前接口

### 健康检查

```http
GET /health
```

示例：

```bash
curl http://127.0.0.1:8000/health
```

返回示例：

```json
{
  "ok": true,
  "engine": "ChatTTS",
  "device": "cpu",
  "busy": false,
  "speaker_seed": 2222,
  "audio": {
    "format": "pcm_s16le",
    "sample_rate": 24000,
    "channels": 1,
    "bits_per_sample": 16
  }
}
```

### 流式 TTS

推荐接口：

```http
POST /tts/stream
Content-Type: application/json
```

`/tts` 是同一个接口的简写别名。

最小请求：

```json
{
  "text": "设备正常，当前未录像。"
}
```

完整参数示例：

```json
{
  "text": "设备正常，当前未录像，声音已开启。",
  "lang": "zh",
  "speed": 5,
  "temperature": 0.3,
  "top_p": 0.7,
  "top_k": 20,
  "skip_refine_text": false,
  "refine_prompt": ""
}
```

当前服务针对设备状态播报等短文本优化，单次 `text` 最大 300 个字符。

---

## 2. HTTP Response 音频格式

Body **不是 JSON，也不是 WAV 文件**。

Body 从第一个字节开始就是裸 PCM：

```text
format          : signed PCM little-endian (pcm_s16le)
sample rate     : 24000 Hz
bits per sample : 16
channels        : 1 (mono)
```

响应头同时携带：

```text
X-Audio-Format: pcm_s16le
X-Sample-Rate: 24000
X-Channels: 1
X-Bits-Per-Sample: 16
X-TTS-Engine: ChatTTS
X-Request-Id: xxxxxxxx
```

没有 `Content-Length` 是正常的，因为这是流式响应。

ChatTTS 当前原生音频是 24kHz，因此服务端第一版保持原生 24kHz，不在流式 chunk 之间做 24k -> 16k 的独立重采样，避免 chunk 边界产生不连续或爆音。

ESP32 播放 TTS 时可以临时把 MAX98357A 的 I2S 时钟切到 24kHz，播完后再恢复项目原来的 16kHz；这和当前项目播放 44.1kHz 快门 WAV 时临时调整 I2S sample rate 的思路一致。

---

## 3. macOS 安装

建议使用独立 Python 3.11 环境。

### conda

```bash
cd esp32learning/ttsserver

conda create -n chattts python=3.11 -y
conda activate chattts

pip install -r requirements.txt
```

`ChatTTS==0.2.5` 会安装对应的 PyTorch 等依赖。

第一次启动时 ChatTTS 默认会从 Hugging Face 下载模型，因此第一次启动明显比后续启动慢。

---

## 4. 启动服务

进入目录：

```bash
cd esp32learning/ttsserver
conda activate chattts
```

启动：

```bash
python -m uvicorn app:app \
  --host 0.0.0.0 \
  --port 8000
```

看到类似日志表示服务准备完成：

```text
Loading ChatTTS model: source=huggingface compile=False
ChatTTS ready: device=cpu speaker_seed=2222 native_pcm=24000Hz/16bit/mono
```

检查：

```bash
curl http://127.0.0.1:8000/health
```

---

## 5. 在 Mac 上先验证流式接口

仓库提供：

```text
test_client.py
```

执行：

```bash
python test_client.py \
  --text "设备正常，当前未录像，声音已开启。"
```

客户端会按照 ESP32 将来相同的方式消费 HTTP PCM stream，但为了方便 Mac 测试，会把收到的裸 PCM 包装成：

```text
tts_test.wav
```

播放：

```bash
afplay tts_test.wav
```

如果这一步能正常听到声音，说明：

```text
文本 -> ChatTTS -> 流式 HTTP -> PCM
```

服务端链路已经正常。

---

## 6. ESP32 请求格式

ESP32 将来发送：

```http
POST http://<MAC_IP>:8000/tts/stream
Content-Type: application/json

{"text":"设备正常，当前未录像。"}
```

收到 HTTP 200 后：

```text
读取一小块 HTTP body
        ↓
得到 PCM bytes
        ↓
mono int16 sample
        ↓
复制成 L/R stereo
        ↓
i2s_channel_write()
        ↓
继续读下一块
```

这样 ESP32 不需要在 PSRAM 中保存整句话。

### 非常重要：TCP/HTTP block != PCM sample block

虽然服务器每次 `yield` 的 PCM 数据长度一定是偶数，但是 TCP/IP 和 `esp_http_client_read()` **不保证**一次 read 刚好对应一次服务端 yield。

因此 ESP32 可能收到：

```text
read #1: 2047 bytes
read #2: 2048 bytes
```

PCM16 每个 mono sample 是 2 bytes，所以当一次 read 得到奇数字节时，ESP32 必须：

```text
保留最后 1 byte
        ↓
下一次 read 时放到新数据最前面
        ↓
重新组成完整 int16 sample
```

不能直接把每一次 HTTP read 强制 cast 成 `int16_t *`。

---

## 7. ESP32 Mono -> MAX98357A Stereo

服务端返回 mono：

```text
sample0 sample1 sample2 ...
```

当前项目 MAX98357A 使用 stereo I2S，因此 ESP32 可以把每个样本复制到左右声道：

```c
int16_t mono = ...;

stereo[i * 2]     = mono;  // LEFT
stereo[i * 2 + 1] = mono;  // RIGHT
```

再通过：

```c
i2s_channel_write(...)
```

发送。

这样无论 MAX98357A 当前选取左声道还是右声道都能正常播放。

---

## 8. 当前服务端日志

每次请求有独立 `request_id`：

```text
TTS a1b2c3d4 queued: chars=17 ...
TTS a1b2c3d4 inference start: queue_wait=0.001s
TTS a1b2c3d4 first PCM chunk: latency=1.234s bytes=24000
TTS a1b2c3d4 finished: chunks=3 bytes=118400 elapsed=3.421s
```

这些日志后面可用于判断：

```text
ChatTTS 首包慢
还是
Wi-Fi 传输慢
还是
ESP32 I2S 播放慢
```

---

## 9. 固定音色

服务启动时只生成一次 speaker embedding，并在后续所有请求中复用。

默认：

```text
CHATTTS_SPEAKER_SEED=2222
```

因此多次“状态查询”不会每次随机换一个声音。

可以在启动前修改：

```bash
export CHATTTS_SPEAKER_SEED=4099
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

换 seed 后需要重启服务。

---

## 10. 可调环境变量

```text
CHATTTS_SOURCE=huggingface
CHATTTS_SPEAKER_SEED=2222
CHATTTS_COMPILE=0
CHATTTS_STREAM_BATCH=24
CHATTTS_STREAM_SPEED=12000
CHATTTS_PASS_FIRST_N_BATCHES=2
LOG_LEVEL=INFO
```

第一阶段建议保持默认值，等 ESP32 流式播放跑通以后，再针对首包时延调整 streaming 参数。

---

## 11. 当前网络联调方式

如果 ESP32 继续使用当前项目的 SoftAP：

```text
SSID: S3CAM-Photo
ESP32: 192.168.4.1
```

Mac 也连接 `S3CAM-Photo` 后，先查看 Mac 在这个 Wi-Fi 下获得的 IP。

macOS 常见命令：

```bash
ipconfig getifaddr en0
```

假设返回：

```text
192.168.4.2
```

那么 ESP32 的 TTS URL 就是：

```text
http://192.168.4.2:8000/tts/stream
```

注意 Mac 的 DHCP 地址以后可能变化。第一阶段为了联调可以先写死，后续再考虑固定 DHCP 地址、mDNS 或改成 ESP32 STA 模式。

---

## 12. 并发策略

当前服务器故意把 ChatTTS inference 串行化：

```text
ESP32 request A -> inference
ESP32 request B -> wait
```

这是因为当前目标是一个 ESP32 设备的交互式播报，不是公共高并发 TTS 服务。

串行化可以显著减少 PyTorch / ChatTTS 同时推理导致的内存竞争和不稳定。

---

## 13. ChatTTS License 提醒

ChatTTS 代码与模型有各自的许可证约束；公开模型目前主要定位于研究/非商业用途。

如果后续这个 ESP32 项目进入正式商用产品阶段，需要在选型阶段重新确认 ChatTTS 当时的模型许可证是否满足商业使用要求。这个目录目前按开发、学习和原型验证用途设计。
