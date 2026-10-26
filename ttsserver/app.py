"""ChatTTS HTTP streaming server for ESP32 devices.

Protocol:
    POST /tts/stream
    Content-Type: application/json
    {"text": "设备正常，当前未录像。"}

Response body:
    Raw PCM stream, signed 16-bit little-endian, mono, 24000 Hz.

The ESP32 can consume the HTTP response incrementally and write each received
PCM block to I2S without buffering the complete sentence in RAM.
"""

from __future__ import annotations

import logging
import os
import sys
import threading
import time
import uuid
from contextlib import asynccontextmanager
from typing import Generator, Literal

# ChatTTS' official macOS example enables this fallback. On Intel Macs the
# model will normally run on CPU; on Apple Silicon this also avoids some MPS
# operator failures.
if sys.platform == "darwin":
    os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

import ChatTTS
import numpy as np
import torch
from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field


APP_NAME = "ESP32 ChatTTS Streaming Server"
CHAT_TTS_SAMPLE_RATE = 24000
PCM_BITS_PER_SAMPLE = 16
PCM_CHANNELS = 1

MODEL_SOURCE = os.getenv("CHATTTS_SOURCE", "huggingface")
SPEAKER_SEED = int(os.getenv("CHATTTS_SPEAKER_SEED", "2222"))
COMPILE_MODEL = os.getenv("CHATTTS_COMPILE", "0") == "1"
STREAM_BATCH = int(os.getenv("CHATTTS_STREAM_BATCH", "24"))
STREAM_SPEED = int(os.getenv("CHATTTS_STREAM_SPEED", "12000"))
PASS_FIRST_N_BATCHES = int(os.getenv("CHATTTS_PASS_FIRST_N_BATCHES", "2"))

logging.basicConfig(
    level=os.getenv("LOG_LEVEL", "INFO").upper(),
    format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
)
logger = logging.getLogger("ttsserver")

_chat: ChatTTS.Chat | None = None
_speaker_embedding: str | None = None

# ChatTTS/PyTorch inference is serialized deliberately. The ESP32 use case is
# interactive device speech, not a high-concurrency public API. Serializing
# requests avoids two simultaneous inference jobs competing for CPU/GPU RAM.
_inference_lock = threading.Lock()


class TTSRequest(BaseModel):
    """Request sent by an ESP32 or another embedded client."""

    text: str = Field(
        ...,
        min_length=1,
        max_length=300,
        description="Text to synthesize. Optimized for short device prompts/status messages.",
    )
    lang: Literal["zh", "en"] = "zh"
    speed: int = Field(5, ge=1, le=9)
    temperature: float = Field(0.3, ge=0.05, le=1.0)
    top_p: float = Field(0.7, gt=0.0, le=1.0)
    top_k: int = Field(20, ge=1, le=100)
    skip_refine_text: bool = False
    refine_prompt: str = ""


def _float_audio_to_pcm16_bytes(wav: np.ndarray) -> bytes:
    """Convert ChatTTS float waveform to signed little-endian PCM16 bytes."""

    audio = np.asarray(wav, dtype=np.float32)
    if audio.size == 0:
        return b""

    # With split_text=False and one input string ChatTTS normally returns
    # shape (1, samples). Flatten defensively so the wire format stays mono.
    audio = audio.reshape(-1)
    audio = np.nan_to_num(audio, nan=0.0, posinf=1.0, neginf=-1.0)
    audio = np.clip(audio, -1.0, 1.0)

    pcm = (audio * 32767.0).astype("<i2", copy=False)
    return pcm.tobytes()


def _build_infer_params(req: TTSRequest) -> tuple:
    assert _speaker_embedding is not None

    refine = ChatTTS.Chat.RefineTextParams(
        prompt=req.refine_prompt,
        top_P=req.top_p,
        top_K=req.top_k,
        temperature=req.temperature,
        show_tqdm=False,
    )

    infer = ChatTTS.Chat.InferCodeParams(
        prompt=f"[speed_{req.speed}]",
        spk_emb=_speaker_embedding,
        temperature=req.temperature,
        top_P=req.top_p,
        top_K=req.top_k,
        show_tqdm=False,
        stream_batch=STREAM_BATCH,
        stream_speed=STREAM_SPEED,
        pass_first_n_batches=PASS_FIRST_N_BATCHES,
    )

    return refine, infer


def _pcm_stream(request_id: str, req: TTSRequest) -> Generator[bytes, None, None]:
    """Run ChatTTS and yield PCM blocks as soon as ChatTTS produces them."""

    if _chat is None or not _chat.has_loaded():
        raise RuntimeError("ChatTTS model is not loaded")

    started = time.monotonic()
    first_chunk_at: float | None = None
    total_bytes = 0
    total_chunks = 0

    logger.info(
        "TTS %s queued: chars=%d lang=%s speed=%d text=%r",
        request_id,
        len(req.text),
        req.lang,
        req.speed,
        req.text[:80],
    )

    try:
        with _inference_lock:
            acquired = time.monotonic()
            logger.info(
                "TTS %s inference start: queue_wait=%.3fs",
                request_id,
                acquired - started,
            )

            refine_params, infer_params = _build_infer_params(req)

            # IMPORTANT: split_text=False keeps this request as one sequential
            # audio stream. ChatTTS' split-text streaming path can represent
            # multiple items as a batch; that is not suitable for a single raw
            # PCM wire stream consumed by an embedded speaker.
            wav_generator = _chat.infer(
                text=req.text,
                stream=True,
                lang=req.lang,
                skip_refine_text=req.skip_refine_text,
                split_text=False,
                do_text_normalization=True,
                do_homophone_replacement=True,
                params_refine_text=refine_params,
                params_infer_code=infer_params,
            )

            for wav_chunk in wav_generator:
                pcm = _float_audio_to_pcm16_bytes(wav_chunk)
                if not pcm:
                    continue

                now = time.monotonic()
                if first_chunk_at is None:
                    first_chunk_at = now
                    logger.info(
                        "TTS %s first PCM chunk: latency=%.3fs bytes=%d",
                        request_id,
                        first_chunk_at - started,
                        len(pcm),
                    )

                total_chunks += 1
                total_bytes += len(pcm)
                yield pcm

    except GeneratorExit:
        logger.warning("TTS %s client disconnected", request_id)
        raise
    except Exception:
        logger.exception("TTS %s inference/stream failed", request_id)
        raise
    finally:
        elapsed = time.monotonic() - started
        logger.info(
            "TTS %s finished: chunks=%d bytes=%d elapsed=%.3fs",
            request_id,
            total_chunks,
            total_bytes,
            elapsed,
        )


@asynccontextmanager
async def lifespan(_: FastAPI):
    global _chat, _speaker_embedding

    logger.info("Loading ChatTTS model: source=%s compile=%s", MODEL_SOURCE, COMPILE_MODEL)

    chat = ChatTTS.Chat()
    loaded = chat.load(source=MODEL_SOURCE, compile=COMPILE_MODEL)
    if not loaded:
        raise RuntimeError(
            f"ChatTTS model load failed (source={MODEL_SOURCE!r}). "
            "Check network/model cache and server logs."
        )

    # Sample one speaker embedding once at server startup. Reusing this
    # embedding makes the device voice stable across different TTS requests.
    torch.manual_seed(SPEAKER_SEED)
    speaker_embedding = chat.sample_random_speaker()

    _chat = chat
    _speaker_embedding = speaker_embedding

    logger.info(
        "ChatTTS ready: device=%s speaker_seed=%d native_pcm=%dHz/%dbit/mono",
        getattr(chat, "device", "unknown"),
        SPEAKER_SEED,
        CHAT_TTS_SAMPLE_RATE,
        PCM_BITS_PER_SAMPLE,
    )

    try:
        yield
    finally:
        logger.info("TTS server shutting down")
        if _chat is not None:
            try:
                _chat.unload()
            except Exception:
                logger.exception("ChatTTS unload failed")
        _chat = None
        _speaker_embedding = None


app = FastAPI(
    title=APP_NAME,
    version="0.1.0",
    lifespan=lifespan,
)


@app.get("/health")
def health() -> dict:
    loaded = _chat is not None and _chat.has_loaded()
    return {
        "ok": loaded,
        "engine": "ChatTTS",
        "model_source": MODEL_SOURCE,
        "device": str(getattr(_chat, "device", "not-loaded")),
        "busy": _inference_lock.locked(),
        "speaker_seed": SPEAKER_SEED,
        "audio": {
            "format": "pcm_s16le",
            "sample_rate": CHAT_TTS_SAMPLE_RATE,
            "channels": PCM_CHANNELS,
            "bits_per_sample": PCM_BITS_PER_SAMPLE,
        },
    }


@app.post("/tts/stream")
@app.post("/tts")
def tts_stream(req: TTSRequest) -> StreamingResponse:
    if _chat is None or not _chat.has_loaded():
        raise HTTPException(status_code=503, detail="ChatTTS model is not ready")

    text = req.text.strip()
    if not text:
        raise HTTPException(status_code=422, detail="text must not be blank")
    req.text = text

    request_id = uuid.uuid4().hex[:8]

    headers = {
        "Cache-Control": "no-store",
        "X-Request-Id": request_id,
        "X-TTS-Engine": "ChatTTS",
        "X-Audio-Format": "pcm_s16le",
        "X-Sample-Rate": str(CHAT_TTS_SAMPLE_RATE),
        "X-Channels": str(PCM_CHANNELS),
        "X-Bits-Per-Sample": str(PCM_BITS_PER_SAMPLE),
    }

    return StreamingResponse(
        _pcm_stream(request_id, req),
        media_type="application/octet-stream",
        headers=headers,
    )
