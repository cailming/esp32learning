"""FunASR ParaformerStreaming ASR server for macOS Intel CPU.

ESP32 sends 16 kHz / 16-bit / mono little-endian PCM to POST /asr.
The server runs ParaformerStreaming on CPU and returns JSON:
    {"text": "...", "sample_rate": 16000, "duration_ms": 1234}

The HTTP request contains one speech segment.  Inside the request the audio is
fed to ParaformerStreaming chunk-by-chunk with the model cache, so the model is
used in its streaming mode while keeping the ESP32 side simple and robust.
"""

from __future__ import annotations

import os
import threading
from typing import Any

import numpy as np
from fastapi import FastAPI, HTTPException, Request
from funasr import AutoModel

SAMPLE_RATE = int(os.getenv("ASR_SAMPLE_RATE", "16000"))
MODEL_NAME = os.getenv("ASR_MODEL", "paraformer-zh-streaming")
# FunASR streaming examples commonly use [0, 10, 5] at 60 ms encoder units.
CHUNK_SIZE = [0, 10, 5]
ENCODER_CHUNK_LOOK_BACK = 4
DECODER_CHUNK_LOOK_BACK = 1
# 10 * 60 ms = 600 ms of PCM per streaming call.
CHUNK_SAMPLES = 10 * 60 * SAMPLE_RATE // 1000

app = FastAPI(title="ESP32 FunASR Streaming Server", version="1.0")

# The model/cache pair is not shared across concurrent recognitions.  A lock is
# enough for the single-device development server and avoids CPU oversubscription
# on an Intel MacBook/Mac mini.
_model_lock = threading.Lock()
model = AutoModel(model=MODEL_NAME, device="cpu", disable_update=True)


def _extract_text(result: Any) -> str:
    """Normalize FunASR's list/dict result to one text string."""
    if result is None:
        return ""
    if isinstance(result, list):
        parts = []
        for item in result:
            text = _extract_text(item)
            if text:
                parts.append(text)
        return "".join(parts)
    if isinstance(result, dict):
        value = result.get("text", "")
        return value if isinstance(value, str) else str(value)
    return str(result)


def recognize_streaming(pcm: np.ndarray) -> str:
    """Run one utterance through ParaformerStreaming with a fresh cache."""
    cache: dict[str, Any] = {}
    final_text = ""

    for start in range(0, len(pcm), CHUNK_SAMPLES):
        chunk = pcm[start : start + CHUNK_SAMPLES]
        is_final = start + CHUNK_SAMPLES >= len(pcm)

        result = model.generate(
            input=chunk,
            cache=cache,
            is_final=is_final,
            chunk_size=CHUNK_SIZE,
            encoder_chunk_look_back=ENCODER_CHUNK_LOOK_BACK,
            decoder_chunk_look_back=DECODER_CHUNK_LOOK_BACK,
            disable_pbar=True,
        )
        text = _extract_text(result)
        if text:
            # ParaformerStreaming may return an incremental chunk.  Concatenating
            # non-empty chunks matches FunASR's streaming usage pattern.
            final_text += text

    return final_text.strip()


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "ok": True,
        "model": MODEL_NAME,
        "device": "cpu",
        "sample_rate": SAMPLE_RATE,
    }


@app.post("/asr")
async def asr(request: Request) -> dict[str, Any]:
    content_type = request.headers.get("content-type", "")
    if "application/octet-stream" not in content_type:
        raise HTTPException(415, "expected application/octet-stream PCM")

    raw = await request.body()
    if not raw:
        raise HTTPException(400, "empty PCM body")
    if len(raw) % 2 != 0:
        raise HTTPException(400, "PCM byte length must be even for int16 audio")

    requested_rate = int(request.headers.get("x-sample-rate", SAMPLE_RATE))
    if requested_rate != SAMPLE_RATE:
        raise HTTPException(
            400,
            f"sample rate must be {SAMPLE_RATE}, got {requested_rate}",
        )

    pcm = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    duration_ms = round(len(pcm) * 1000 / SAMPLE_RATE)

    with _model_lock:
        text = recognize_streaming(pcm)

    return {
        "text": text,
        "sample_rate": SAMPLE_RATE,
        "duration_ms": duration_ms,
    }
