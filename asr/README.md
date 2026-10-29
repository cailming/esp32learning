# FunASR ParaformerStreaming 服务（Intel macOS CPU）

这个目录给 `s3cam/mic_wake_command_test` 的“对话模式”提供 ASR 服务。

## 1. 环境

建议 Intel Mac 使用 Python 3.10 或 3.11：

```bash
cd esp32learning/asr
python3.11 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip
pip install -r requirements.txt
```

首次启动时 FunASR / ModelScope 会下载 `paraformer-zh-streaming` 模型。

## 2. 启动服务

```bash
uvicorn server:app --host 0.0.0.0 --port 8001
```

健康检查：

```bash
curl http://127.0.0.1:8001/health
```

期望返回类似：

```json
{"ok":true,"model":"paraformer-zh-streaming","device":"cpu","sample_rate":16000}
```

## 3. ASR HTTP 协议

ESP32 调用：

```text
POST /asr
Content-Type: application/octet-stream
X-Sample-Rate: 16000
X-Sample-Format: s16le

<body = 16kHz / 16-bit / mono / little-endian PCM>
```

返回：

```json
{
  "text": "今天天气怎么样",
  "sample_rate": 16000,
  "duration_ms": 1860
}
```

ESP32 上传的是已经由本地轻量 VAD 分好的一句话。服务端再按 ParaformerStreaming 的 chunk/cache 方式做流式模型推理。

## 4. ESP32 配置

进入：

```bash
idf.py menuconfig
```

在 `Voice services / uplink network` 中配置：

- `Enable Wi-Fi STA uplink for TTS / ASR`：打开
- `Uplink Wi-Fi SSID / password`：填写与 Mac 同一局域网的 Wi-Fi
- `FunASR recognition URL`：例如 `http://192.168.1.92:8001/asr`

然后：

```bash
idf.py build flash monitor
```

## 5. 使用流程

```text
你好小智
  -> 我在，有什么需要
进入对话模式
  -> TTS：进入对话模式
  -> ESP32 开始持续监听普通说话
  -> VAD 检测到一句话结束
  -> PCM 上传到 Mac /asr
  -> FunASR 返回文本
  -> 串口打印：ASR TEXT: ...
退出对话模式
  -> 回到 WakeNet 唤醒模式
```

对话模式中 MultiNet 不再执行“拍照/状态查询”等普通固定命令，避免正常对话被误触发；只保留“退出对话模式/退出语音”作为本地退出命令。

## 6. VAD 调整

如果环境噪声较大，可以提高 `Dialog VAD peak threshold`（默认 900）。
如果说话较轻却经常没有开始录音，可以降低它。

一句话结束静音默认 800 ms；可通过 `Silence duration that ends one utterance` 修改。
