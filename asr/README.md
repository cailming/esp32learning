# FunASR + ChatTTS 对话桥接服务（Intel macOS CPU）

这个目录给 `s3cam/mic_wake_command_test` 的“对话模式”提供两类接口：

- `/asr`：纯 ASR 调试接口，PCM -> 文本 JSON。
- `/dialog`：完整语音闭环，PCM -> FunASR -> 文本 -> ChatTTS -> PCM 返回 ESP32 播放。

当前 `/dialog` 的对话处理器是 **echo**：ASR 识别出的文字原样交给 ChatTTS。后续如果要接 LLM，只需要替换 `server.py` 中的 `dialog_process_text()`，ESP32 传输和播放链路不用再改。

当前默认直接使用已经下载到本机的 FunASR 模型，不再自动从网上下载。

默认模型目录：

```text
/Users/apple/dev/umfplugin/src/model
```

## 1. 环境

建议 Intel Mac 使用 Python 3.10 或 3.11：

```bash
cd esp32learning/asr
python3.11 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip
pip install -r requirements.txt
```

`/dialog` 会通过 HTTP 调用现有 ChatTTS 服务，因此 `requirements.txt` 中额外包含 `requests`。

## 2. 本地 FunASR 模型

服务默认加载：

```text
/Users/apple/dev/umfplugin/src/model
```

也可以覆盖：

```bash
export ASR_MODEL_PATH=/Users/apple/dev/umfplugin/src/model
```

如果目录不存在，服务会直接报错退出，**不会偷偷重新下载模型**。

如果以后确实允许远程 fallback：

```bash
export ASR_ALLOW_REMOTE_MODEL=1
export ASR_MODEL=paraformer-zh-streaming
```

> `ASR_MODEL_PATH` 必须指向能够被 `AutoModel(model=...)` 直接加载的实际模型根目录。

## 3. ChatTTS 上游配置

默认：

```text
http://192.168.1.92:8000/tts/stream
```

可以通过环境变量覆盖：

```bash
export ASR_TTS_SERVER_URL=http://192.168.1.92:8000/tts/stream
```

ChatTTS 响应必须是当前项目已经验证过的格式：

```text
24kHz / signed PCM16LE / mono
```

## 4. 启动

可以直接：

```bash
python server.py
```

等价于：

```bash
python -m uvicorn server:app --host 0.0.0.0 --port 8001
```

启动后应看到：

```text
[ASR] loading model source=local: /Users/apple/dev/umfplugin/src/model
[ASR] model ready: ...
[DIALOG] TTS upstream: http://192.168.1.92:8000/tts/stream
```

## 5. 健康检查

```bash
curl http://127.0.0.1:8001/health
```

除了 FunASR 信息，还会返回：

```json
{
  "dialog_tts_url": "http://192.168.1.92:8000/tts/stream",
  "dialog_tts_sample_rate": 24000,
  "dialog_processor": "echo"
}
```

## 6. `/asr`：纯识别接口

```text
POST /asr
Content-Type: application/octet-stream
X-Sample-Rate: 16000
X-Sample-Format: s16le

<body = 16kHz / PCM16LE / mono>
```

返回 JSON：

```json
{
  "text": "今天天气怎么样",
  "sample_rate": 16000,
  "duration_ms": 1860,
  "inference_ms": 320,
  "total_ms": 600,
  "rtf": 0.21
}
```

这个接口保留用于独立排查 ASR，不负责 TTS。

## 7. `/dialog`：Speech -> Text -> Speech

ESP32 发送：

```text
POST /dialog
Content-Type: application/octet-stream
X-Sample-Rate: 16000
X-Sample-Format: s16le

<body = 用户的一句话 PCM>
```

服务端流程：

```text
ESP32 PCM 16k
   -> FunASR
   -> ASR text
   -> dialog_process_text(text)
   -> 当前版本原文 echo
   -> ChatTTS /tts/stream
   -> 24k PCM
   -> HTTP body 流回 ESP32
```

HTTP 200 响应：

```text
X-ASR-Text-B64: <ASR UTF-8 文本的 Base64>
X-Audio-Sample-Rate: 24000
Content-Type: application/octet-stream

<body = 24kHz / PCM16LE / mono>
```

如果 ASR 结果为空，不调用 TTS，直接返回 HTTP 204。

## 8. ESP32 配置

进入：

```bash
idf.py menuconfig
```

在 `Voice services / uplink network` 中：

- 打开 `Enable Wi-Fi STA uplink for TTS / ASR`。
- 配好 Wi-Fi SSID / password。
- `FunASR recognition URL` 仍配置成 `/asr`，例如：

```text
http://192.168.1.103:8001/asr
```

ESP32 的 `asr_client` 会自动转换为：

```text
http://192.168.1.103:8001/dialog
```

这样 `/asr` 仍然可以保留给 curl / Python 单独调试。

然后：

```bash
idf.py build flash monitor
```

## 9. 当前对话流程

```text
你好小智
  -> 本地 WakeNet10

进入对话模式
  -> 本地 MultiNet
  -> TTS：进入对话模式

用户：速度还挺快
  -> ESP32 VAD 分句
  -> 16k PCM POST /dialog
  -> FunASR：速度还挺快
  -> echo processor：速度还挺快
  -> ChatTTS
  -> 24k PCM 流回 ESP32
  -> MAX98357A 播放“速度还挺快”
  -> 串口仍打印 ASR TEXT: 速度还挺快
```

对话模式中 MultiNet 不执行“拍照/状态查询”等普通固定命令，只保留退出命令作为本地兜底。

## 10. 后续接 LLM

第一版故意只做 echo，先验证完整语音闭环。稳定后只需把：

```python
def dialog_process_text(text: str) -> str:
    return text
```

替换成：

```text
ASR text -> LLM -> reply text
```

ChatTTS 和 ESP32 播放链路无需改变。

## 11. VAD 调整

环境噪声较大时，提高 `Dialog VAD peak threshold`（默认 900）；轻声经常触发不了时降低。

一句话结束静音默认 800 ms，可通过 `Silence duration that ends one utterance` 修改。
