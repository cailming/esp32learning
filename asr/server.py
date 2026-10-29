"""FunASR ParaformerStreaming ASR server for macOS Intel CPU.

ESP32 sends 16 kHz / 16-bit / mono little-endian PCM to POST /asr.
The server runs ParaformerStreaming on CPU and returns JSON.

By default this server loads the already-downloaded local FunASR model from:
    /Users/apple/dev/umfplugin/src/model

The local path can be overridden with ASR_MODEL_PATH. Remote model download is
disabled by default so an accidentally missing local directory does not trigger
an online download. Set ASR_ALLOW_REMOTE_MODEL=1 only when remote fallback is
explicitly desired.
"""

from __future__ import annotations

import itertools
import math
import os
import threading
import time
from pathlib import Path
from typing import Any

import numpy as np
from fastapi import FastAPI, HTTPException, Request
from funasr import AutoModel

SAMPLE_RATE = int(os.getenv("ASR_SAMPLE_RATE", "16000"))
DEFAULT_LOCAL_MODEL_PATH = "/Users/apple/dev/umfplugin/src/model"
MODEL_PATH = Path(
    os.getenv("ASR_MODEL_PATH", DEFAULT_LOCAL_MODEL_PATH)
).expanduser().resolve()
REMOTE_MODEL_NAME = os.getenv("ASR_MODEL", "paraformer-zh-streaming")
ALLOW_REMOTE_MODEL = os.getenv("ASR_ALLOW_REMOTE_MODEL", "0").strip().lower() in {
    "1",
    "true",
    "yes",
    "on",
}

# ParaformerStreaming: [0, 10, 5], middle value 10 corresponds to 600 ms.
CHUNK_SIZE = [0, 10, 5]
ENCODER_CHUNK_LOOK_BACK = 4
DECODER_CHUNK_LOOK_BACK = 1
CHUNK_SAMPLES = CHUNK_SIZE[1] * 60 * SAMPLE_RATE // 1000

app = FastAPI(title="ESP32 FunASR Streaming Server", version="1.2")

_model_lock = threading.Lock()
_request_ids = itertools.count(1)


def log(message: str) -> None:
    print(message, flush=True)


def _resolve_model_source() -> tuple[str, str]:
    if MODEL_PATH.is_dir():
        return str(MODEL_PATH), "local"

    if ALLOW_REMOTE_MODEL:
        log(
            f"[ASR] local model directory not found: {MODEL_PATH}; "
            f"remote fallback enabled -> {REMOTE_MODEL_NAME}"
        )
        return REMOTE_MODEL_NAME, "remote"

    raise RuntimeError(
        "FunASR local model directory does not exist: "
        f"{MODEL_PATH}. Set ASR_MODEL_PATH to the correct local model directory. "
        "Remote download is disabled by default; set ASR_ALLOW_REMOTE_MODEL=1 "
        "only if you explicitly want online fallback."
    )


MODEL_SOURCE, MODEL_SOURCE_TYPE = _resolve_model_source()
log(f"[ASR] loading model source={MODEL_SOURCE_TYPE}: {MODEL_SOURCE}")
_model_load_started = time.perf_counter()
model = AutoModel(
    model=MODEL_SOURCE,
    device="cpu",
    disable_update=True,
    trust_remote_code=False,
)
log(
    "[ASR] model ready: "
    f"source={MODEL_SOURCE_TYPE}, "
    f"resolved_path={getattr(model, 'model_path', MODEL_SOURCE)}, "
    f"load={time.perf_counter() - _model_load_started:.3f}s"
)


def _extract_text(result: Any) -> str:
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


def _audio_stats(pcm: np.ndarray) -> tuple[float, float]:
    if pcm.size == 0:
        return 0.0, 0.0
    peak = float(np.max(np.abs(pcm)))
    rms = float(np.sqrt(np.mean(np.square(pcm), dtype=np.float64)))
    return peak, rms


def recognize_streaming(pcm: np.ndarray, request_id: int) -> tuple[str, float]:
    """Run one utterance through ParaformerStreaming and log every chunk."""
    cache: dict[str, Any] = {}
    final_text = ""
    total_chunks = math.ceil(len(pcm) / CHUNK_SAMPLES)
    infer_started = time.perf_counter()

    for chunk_index, start in enumerate(range(0, len(pcm), CHUNK_SAMPLES), start=1):
        chunk = pcm[start : start + CHUNK_SAMPLES]
        is_final = start + CHUNK_SAMPLES >= len(pcm)
        chunk_audio_ms = len(chunk) * 1000.0 / SAMPLE_RATE
        chunk_started = time.perf_counter()

        result = model.generate(
            input=chunk,
            cache=cache,
            is_final=is_final,
            chunk_size=CHUNK_SIZE,
            encoder_chunk_look_back=ENCODER_CHUNK_LOOK_BACK,
            decoder_chunk_look_back=DECODER_CHUNK_LOOK_BACK,
            disable_pbar=True,
        )

        chunk_elapsed = time.perf_counter() - chunk_started
        text = _extract_text(result)
        if text:
            final_text += text

        log(
            f"[ASR #{request_id}] chunk {chunk_index}/{total_chunks}: "
            f"audio={chunk_audio_ms:.0f}ms infer={chunk_elapsed:.3f}s "
            f"final={is_final} text={text!r}"
        )

    infer_elapsed = time.perf_counter() - infer_started
    return final_text.strip(), infer_elapsed


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "ok": True,
        "model": str(MODEL_SOURCE),
        "model_source": MODEL_SOURCE_TYPE,
        "model_path": str(getattr(model, "model_path", MODEL_SOURCE)),
        "device": "cpu",
        "sample_rate": SAMPLE_RATE,
        "chunk_samples": CHUNK_SAMPLES,
    }


@app.post("/asr")
async def asr(request: Request) -> dict[str, Any]:
    request_id = next(_request_ids)
    request_started = time.perf_counter()
    client = request.client.host if request.client is not None else "unknown"

    content_type = request.headers.get("content-type", "")
    if "application/octet-stream" not in content_type:
        raise HTTPException(415, "expected application/octet-stream PCM")

    raw = await request.body()
    if not raw:
        raise HTTPException(400, "empty PCM body")
    if len(raw) % 2 != 0:
        raise HTTPException(400, "PCM byte length must be even for int16 audio")

    try:
        requested_rate = int(request.headers.get("x-sample-rate", SAMPLE_RATE))
    except ValueError as exc:
        raise HTTPException(400, "invalid X-Sample-Rate header") from exc

    if requested_rate != SAMPLE_RATE:
        raise HTTPException(
            400,
            f"sample rate must be {SAMPLE_RATE}, got {requested_rate}",
        )

    pcm = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    duration_ms = round(len(pcm) * 1000 / SAMPLE_RATE)
    peak, rms = _audio_stats(pcm)

    log(
        f"[ASR #{request_id}] recv from={client}: bytes={len(raw)} "
        f"samples={len(pcm)} audio={duration_ms / 1000.0:.3f}s "
        f"peak={peak:.4f} rms={rms:.4f}"
    )

    lock_started = time.perf_counter()
    with _model_lock:
        lock_wait = time.perf_counter() - lock_started
        if lock_wait >= 0.001:
            log(f"[ASR #{request_id}] model lock wait={lock_wait:.3f}s")
        text, infer_elapsed = recognize_streaming(pcm, request_id)

    total_elapsed = time.perf_counter() - request_started
    audio_seconds = max(duration_ms / 1000.0, 0.001)
    rtf = infer_elapsed / audio_seconds

    log(
        f"[ASR #{request_id}] done: infer={infer_elapsed:.3f}s "
        f"total={total_elapsed:.3f}s RTF={rtf:.2f} text={text!r}"
    )

    return {
        "text": text,
        "sample_rate": SAMPLE_RATE,
        "duration_ms": duration_ms,
        "inference_ms": round(infer_elapsed * 1000),
        "total_ms": round(total_elapsed * 1000),
        "rtf": round(rtf, 3),
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8001)
