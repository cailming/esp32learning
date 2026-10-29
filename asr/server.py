"""FunASR + ChatTTS dialog bridge for ESP32-S3.

Endpoints:
- GET  /health : health/config information
- POST /asr    : raw 16kHz PCM16LE mono -> JSON text
- POST /dialog : raw 16kHz PCM16LE mono -> ASR -> echo processor -> ChatTTS -> raw 24kHz PCM16LE mono

The first dialog version intentionally uses an echo processor: recognized text is
sent unchanged to ChatTTS.  Later an LLM can be inserted inside
``dialog_process_text`` without changing the ESP32 transport/playback path.
"""

from __future__ import annotations

import base64
import itertools
import math
import os
import threading
import time
from pathlib import Path
from typing import Any, Iterator

import numpy as np
import requests
from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import StreamingResponse
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

# ChatTTS service already used by the ESP32 project.
TTS_SERVER_URL = os.getenv(
    "ASR_TTS_SERVER_URL",
    "http://192.168.1.92:8000/tts/stream",
)
TTS_SAMPLE_RATE = int(os.getenv("ASR_TTS_SAMPLE_RATE", "24000"))
TTS_CONNECT_TIMEOUT = float(os.getenv("ASR_TTS_CONNECT_TIMEOUT", "3"))
TTS_READ_TIMEOUT = float(os.getenv("ASR_TTS_READ_TIMEOUT", "30"))
TTS_CHUNK_BYTES = int(os.getenv("ASR_TTS_CHUNK_BYTES", "4096"))

# ParaformerStreaming: [0, 10, 5], middle value 10 corresponds to 600 ms.
CHUNK_SIZE = [0, 10, 5]
ENCODER_CHUNK_LOOK_BACK = 4
DECODER_CHUNK_LOOK_BACK = 1
CHUNK_SAMPLES = CHUNK_SIZE[1] * 60 * SAMPLE_RATE // 1000

app = FastAPI(title="ESP32 FunASR Dialog Server", version="2.0")

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
log(f"[DIALOG] TTS upstream: {TTS_SERVER_URL}")


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


def _decode_pcm_request(raw: bytes, request: Request) -> tuple[np.ndarray, int, float, float]:
    content_type = request.headers.get("content-type", "")
    if "application/octet-stream" not in content_type:
        raise HTTPException(415, "expected application/octet-stream PCM")
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
    return pcm, duration_ms, peak, rms


def _recognize_request(pcm: np.ndarray, request_id: int) -> tuple[str, float]:
    lock_started = time.perf_counter()
    with _model_lock:
        lock_wait = time.perf_counter() - lock_started
        if lock_wait >= 0.001:
            log(f"[ASR #{request_id}] model lock wait={lock_wait:.3f}s")
        return recognize_streaming(pcm, request_id)


def dialog_process_text(text: str) -> str:
    """Dialog policy hook. Version 1 simply echoes ASR text back to TTS."""
    return text.strip()


def _tts_request(reply_text: str, request_id: int) -> requests.Response:
    payload = {
        "text": reply_text,
        "lang": "zh",
        "speed": 3,
        "skip_refine_text": True,
    }

    started = time.perf_counter()
    try:
        response = requests.post(
            TTS_SERVER_URL,
            json=payload,
            stream=True,
            timeout=(TTS_CONNECT_TIMEOUT, TTS_READ_TIMEOUT),
        )
    except requests.RequestException as exc:
        log(f"[DIALOG #{request_id}] TTS request failed: {exc}")
        raise HTTPException(502, f"TTS upstream request failed: {exc}") from exc

    setup_elapsed = time.perf_counter() - started
    if response.status_code != 200:
        try:
            detail = response.text[:500]
        finally:
            response.close()
        log(
            f"[DIALOG #{request_id}] TTS upstream HTTP {response.status_code}: "
            f"{detail!r}"
        )
        raise HTTPException(
            502,
            f"TTS upstream HTTP {response.status_code}: {detail}",
        )

    log(
        f"[DIALOG #{request_id}] TTS upstream ready in {setup_elapsed:.3f}s "
        f"content_type={response.headers.get('content-type', '')!r}"
    )
    return response


def _iter_tts_pcm(response: requests.Response, request_id: int) -> Iterator[bytes]:
    total = 0
    started = time.perf_counter()
    first_chunk_at: float | None = None
    try:
        for chunk in response.iter_content(chunk_size=TTS_CHUNK_BYTES):
            if not chunk:
                continue
            if first_chunk_at is None:
                first_chunk_at = time.perf_counter()
                log(
                    f"[DIALOG #{request_id}] first TTS PCM after "
                    f"{first_chunk_at - started:.3f}s bytes={len(chunk)}"
                )
            total += len(chunk)
            yield chunk
    except requests.RequestException as exc:
        log(f"[DIALOG #{request_id}] TTS stream failed after {total} bytes: {exc}")
        raise
    finally:
        response.close()
        log(
            f"[DIALOG #{request_id}] TTS stream done: bytes={total} "
            f"stream={time.perf_counter() - started:.3f}s"
        )


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
        "dialog_tts_url": TTS_SERVER_URL,
        "dialog_tts_sample_rate": TTS_SAMPLE_RATE,
        "dialog_processor": "echo",
    }


@app.post("/asr")
async def asr(request: Request) -> dict[str, Any]:
    request_id = next(_request_ids)
    request_started = time.perf_counter()
    client = request.client.host if request.client is not None else "unknown"

    raw = await request.body()
    pcm, duration_ms, peak, rms = _decode_pcm_request(raw, request)

    log(
        f"[ASR #{request_id}] recv from={client}: bytes={len(raw)} "
        f"samples={len(pcm)} audio={duration_ms / 1000.0:.3f}s "
        f"peak={peak:.4f} rms={rms:.4f}"
    )

    text, infer_elapsed = _recognize_request(pcm, request_id)

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


@app.post("/dialog")
async def dialog(request: Request) -> Response:
    """ASR -> echo processor -> ChatTTS, returning raw 24k PCM to ESP32."""
    request_id = next(_request_ids)
    request_started = time.perf_counter()
    client = request.client.host if request.client is not None else "unknown"

    raw = await request.body()
    pcm, duration_ms, peak, rms = _decode_pcm_request(raw, request)

    log(
        f"[DIALOG #{request_id}] recv from={client}: bytes={len(raw)} "
        f"audio={duration_ms / 1000.0:.3f}s peak={peak:.4f} rms={rms:.4f}"
    )

    text, infer_elapsed = _recognize_request(pcm, request_id)
    reply_text = dialog_process_text(text)
    asr_text_b64 = base64.b64encode(text.encode("utf-8")).decode("ascii")
    reply_text_b64 = base64.b64encode(reply_text.encode("utf-8")).decode("ascii")

    log(
        f"[DIALOG #{request_id}] ASR={text!r} -> reply={reply_text!r} "
        f"infer={infer_elapsed:.3f}s"
    )

    common_headers = {
        "X-ASR-Text-B64": asr_text_b64,
        "X-Reply-Text-B64": reply_text_b64,
        "X-Audio-Sample-Rate": str(TTS_SAMPLE_RATE),
        "X-Audio-Format": "s16le",
        "X-Audio-Channels": "1",
        "X-Dialog-Processor": "echo",
    }

    if not reply_text:
        log(f"[DIALOG #{request_id}] empty ASR text -> 204, no TTS")
        return Response(status_code=204, headers=common_headers)

    tts_response = _tts_request(reply_text, request_id)

    log(
        f"[DIALOG #{request_id}] response headers ready: "
        f"total_before_stream={time.perf_counter() - request_started:.3f}s"
    )

    return StreamingResponse(
        _iter_tts_pcm(tts_response, request_id),
        status_code=200,
        media_type="application/octet-stream",
        headers=common_headers,
    )


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8001)
