# ESP32 ChatTTS 流式 TTS Server

这个目录提供一个运行在 Ubuntu RTX3090 / macOS / PC 上的 Python TTS 服务。

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

默认使用 `speakers/seed_492.pt` 时，返回中会包含类似：

```json
{
  "ok": true,
  "engine": "ChatTTS",
  "device": "cuda:0",
  "speaker": {
    "mode": "file",
    "file": "/home/gc/gcdev/ttsserver/speakers/seed_492.pt",
    "configured_file": "seed_492.pt",
    "directory": "/home/gc/gcdev/ttsserver/speakers",
    "fallback_seed": 2222
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

---

## 3. Python 环境

建议使用独立 Python 3.11 环境：

```bash
conda create -n chattts python=3.11 -y
conda activate chattts
pip install -r requirements.txt
```

不要与 ESP-IDF Python 环境混用。

---

## 4. Ubuntu RTX3090 启动

服务器目录例如：

```text
/home/gc/gcdev/ttsserver/
```

默认音色文件：

```text
/home/gc/gcdev/ttsserver/speakers/seed_492.pt
```

目录：

```text
ttsserver/
├── app.py
├── requirements.txt
├── test_client.py
└── speakers/
    └── seed_492.pt
```

默认情况下不需要设置任何音色环境变量，直接启动：

```bash
cd /home/gc/gcdev/ttsserver
conda activate chattts

python -m uvicorn app:app \
  --host 0.0.0.0 \
  --port 8000
```

程序会自动加载：

```text
speakers/seed_492.pt
```

启动日志应出现类似：

```text
Loading fixed speaker embedding from /home/gc/gcdev/ttsserver/speakers/seed_492.pt
Fixed speaker loaded successfully: /home/gc/gcdev/ttsserver/speakers/seed_492.pt
ChatTTS model ready: device=cuda:0 speaker_file=/home/gc/gcdev/ttsserver/speakers/seed_492.pt ...
```

---

## 5. 快速切换新的固定音色

以后如果新增：

```text
speakers/seed_1234.pt
```

不需要修改 `app.py`。

启动时指定文件名即可：

```bash
CHATTTS_SPEAKER_FILE=seed_1234.pt \
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

程序会自动在 `speakers/` 下查找：

```text
/home/gc/gcdev/ttsserver/speakers/seed_1234.pt
```

也支持直接传绝对路径：

```bash
CHATTTS_SPEAKER_FILE=/home/gc/gcdev/ttsserver/speakers/seed_1234.pt \
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

如果想把音色目录放到其他地方，也可以：

```bash
CHATTTS_SPEAKER_DIR=/data/chattts/speakers \
CHATTTS_SPEAKER_FILE=seed_1234.pt \
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

### 回退到旧的 seed 随机生成方式

如果临时不想使用 `.pt` 文件，可以显式把文件配置设为空：

```bash
CHATTTS_SPEAKER_FILE='' \
CHATTTS_SPEAKER_SEED=491 \
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

`.pt` 文件模式优先用于正式音色，因为它不会依赖不同机器重新随机生成 speaker embedding。

---

## 6. 在电脑上验证流式接口

仓库提供：

```text
test_client.py
```

执行：

```bash
python test_client.py \
  --text "你好，我是小智。设备连接成功。"
```

客户端会按照 ESP32 将来相同的方式消费 HTTP PCM stream，并保存为：

```text
tts_test.wav
```

Linux 上可用任意播放器播放；macOS 可以：

```bash
afplay tts_test.wav
```

如果能正常听到声音，说明：

```text
文本 -> ChatTTS -> 固定音色 -> 流式 HTTP -> PCM
```

服务端链路已经正常。

---

## 7. ESP32 请求格式

ESP32 发送：

```http
POST http://<TTS_SERVER_IP>:8000/tts/stream
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

ESP32 不需要在 PSRAM 中保存整句话。

### TCP/HTTP block != PCM sample block

虽然服务器每次 `yield` 的 PCM 数据长度一定是偶数，但是 TCP/IP 和 `esp_http_client_read()` 不保证一次 read 刚好对应一次服务端 yield。

PCM16 每个 mono sample 是 2 bytes，因此一次 read 得到奇数字节时必须保留最后 1 byte，与下一次 read 的第一个 byte 重新组成 int16 sample。

---

## 8. ESP32 Mono -> MAX98357A Stereo

服务端返回 mono：

```text
sample0 sample1 sample2 ...
```

当前项目 MAX98357A 使用 stereo I2S，因此 ESP32 可以把每个样本复制到左右声道：

```c
int16_t mono = ...;

stereo[i * 2]     = mono;
stereo[i * 2 + 1] = mono;
```

再通过 `i2s_channel_write()` 发送。

---

## 9. 可调环境变量

```text
CHATTTS_SOURCE=huggingface
CHATTTS_SPEAKER_DIR=<ttsserver>/speakers
CHATTTS_SPEAKER_FILE=seed_492.pt
CHATTTS_SPEAKER_SEED=2222        # 仅 file 为空时使用
CHATTTS_COMPILE=0
CHATTTS_STREAM_BATCH=24
CHATTTS_STREAM_SPEED=12000
CHATTTS_PASS_FIRST_N_BATCHES=0
CHATTTS_WARMUP=1
LOG_LEVEL=INFO
```

其中日常切换音色通常只需要改：

```text
CHATTTS_SPEAKER_FILE
```

---

## 10. 并发策略

当前服务器故意把 ChatTTS inference 串行化：

```text
ESP32 request A -> inference
ESP32 request B -> wait
```

当前目标是 ESP32 设备交互式播报，不是公共高并发 TTS 服务。串行化可以减少 PyTorch / ChatTTS 同时推理导致的 GPU 显存竞争和不稳定。

---

## 11. ChatTTS License 提醒

ChatTTS 代码与模型有各自的许可证约束；公开模型主要定位于研究/非商业用途。
如果后续进入正式商用产品阶段，需要重新确认当时的模型许可证是否满足商业使用要求。
