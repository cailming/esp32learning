"""ChatTTS HTTP streaming server for ESP32 devices.

Protocol:
    POST /tts/stream
    Content-Type: application/json
    {"text": "设备正常，当前未录像。", "skip_refine_text": true}

Response body:
    Raw PCM stream, signed 16-bit little-endian, mono, 24000 Hz.

The ESP32 can consume the HTTP response incrementally and write each received
PCM block to I2S without buffering the complete sentence in RAM.
"""

from __future__ import annotations

import logging
import lzma
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
import pybase16384 as b14
import torch
from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field


APP_NAME = "ESP32 ChatTTS Streaming Server"
CHAT_TTS_SAMPLE_RATE = 24000
PCM_BITS_PER_SAMPLE = 16
PCM_CHANNELS = 1
PCM_BYTES_PER_SAMPLE = PCM_BITS_PER_SAMPLE // 8
PCM_BYTES_PER_SECOND = (
    CHAT_TTS_SAMPLE_RATE * PCM_CHANNELS * PCM_BYTES_PER_SAMPLE
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_SOURCE = os.getenv("CHATTTS_SOURCE", "huggingface")
COMPILE_MODEL = os.getenv("CHATTTS_COMPILE", "0") == "1"

# 固定音色配置：
# - 默认读取 ttsserver/speakers/seed_492.pt。
# - 后续有新音色时，只要把 .pt 放进 speakers/，然后通过
#   CHATTTS_SPEAKER_FILE=seed_xxx.pt 即可切换，不需要改代码。
# - 也支持绝对路径，例如：
#   CHATTTS_SPEAKER_FILE=/home/gc/gcdev/ttsserver/speakers/seed_492.pt
# - 如果显式设置 CHATTTS_SPEAKER_FILE=""，则回退到原来的 seed 生成方式。
SPEAKER_DIR = os.path.abspath(
    os.path.expanduser(
        os.getenv("CHATTTS_SPEAKER_DIR", os.path.join(BASE_DIR, "speakers"))
    )
)
SPEAKER_FILE = os.getenv("CHATTTS_SPEAKER_FILE", "seed_492.pt").strip()
SPEAKER_SEED = int(os.getenv("CHATTTS_SPEAKER_SEED", "2222"))

# These defaults are tuned for the ESP32 low-latency device speech use case.
# RTX3090 measurements: batch=24, pass_first=0 -> ~0.17s warm first PCM,
# RTF ~0.33. Environment variables can still override them for experiments.
STREAM_BATCH = int(os.getenv("CHATTTS_STREAM_BATCH", "24"))
STREAM_SPEED = int(os.getenv("CHATTTS_STREAM_SPEED", "12000"))
PASS_FIRST_N_BATCHES = int(os.getenv("CHATTTS_PASS_FIRST_N_BATCHES", "0"))
LOG_CHUNKS = os.getenv("CHATTTS_LOG_CHUNKS", "0") == "1"

# The first real CUDA/CPU inference is slower than steady state. Warm up once
# during application startup so the first ESP32 request gets steady-state
# latency instead of paying lazy CUDA/PyTorch initialization cost.
WARMUP_ENABLED = os.getenv("CHATTTS_WARMUP", "1") == "1"
WARMUP_TEXT = os.getenv("CHATTTS_WARMUP_TEXT", "设备启动")

logging.basicConfig(
    level=os.getenv("LOG_LEVEL", "INFO").upper(),
    format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
)
logger = logging.getLogger("ttsserver")

_chat: ChatTTS.Chat | None = None
_speaker_embedding: str | None = None
_speaker_mode = "not-loaded"
_speaker_file_in_use: str | None = None
_warmup_ok = False
_warmup_elapsed = 0.0

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


def _encode_spk_emb(spk_emb: torch.Tensor) -> str:
    """Convert a saved speaker tensor to ChatTTS 0.2.5's string format."""
    if not isinstance(spk_emb, torch.Tensor):
        raise TypeError(
            f"Speaker .pt must contain a torch.Tensor, got {type(spk_emb).__name__}"
        )
    if spk_emb.numel() == 0:
        raise ValueError("Speaker .pt contains an empty tensor")

    with torch.no_grad():
        arr = spk_emb.to(dtype=torch.float16, device="cpu").numpy()
        encoded = b14.encode_to_string(
            lzma.compress(
                arr.tobytes(),
                format=lzma.FORMAT_RAW,
                filters=[
                    {
                        "id": lzma.FILTER_LZMA2,
                        "preset": 9 | lzma.PRESET_EXTREME,
                    }
                ],
            )
        )
        del arr
    return encoded


def _resolve_speaker_file() -> str | None:
    """Resolve CHATTTS_SPEAKER_FILE against CHATTTS_SPEAKER_DIR."""
    if not SPEAKER_FILE:
        return None

    path = os.path.expanduser(SPEAKER_FILE)
    if not os.path.isabs(path):
        path = os.path.join(SPEAKER_DIR, path)
    return os.path.abspath(path)


def _load_speaker(chat: ChatTTS.Chat) -> tuple[str, str, str | None]:
    """Load a fixed .pt speaker, or explicitly fall back to speaker seed mode."""
    speaker_path = _resolve_speaker_file()

    if speaker_path is not None:
        if not os.path.isfile(speaker_path):
            raise RuntimeError(
                "Configured speaker file does not exist: "
                f"{speaker_path}. Put the .pt file there, change "
                "CHATTTS_SPEAKER_FILE, or set CHATTTS_SPEAKER_FILE='' "
                "to use seed fallback mode."
            )

        logger.info("Loading fixed speaker embedding from %s", speaker_path)
        speaker_tensor = torch.load(speaker_path, map_location="cpu")
        speaker_embedding = _encode_spk_emb(speaker_tensor)
        logger.info("Fixed speaker loaded successfully: %s", speaker_path)
        return speaker_embedding, "file", speaker_path

    logger.warning(
        "CHATTTS_SPEAKER_FILE is empty; generating speaker from seed=%d",
        SPEAKER_SEED,
    )
    torch.manual_seed(SPEAKER_SEED)
    return chat.sample_random_speaker(), "seed", None


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

    request_started = time.perf_counter()
    inference_started: float | None = None
    first_chunk_at: float | None = None
    last_chunk_at: float | None = None
    total_bytes = 0
    total_chunks = 0

    logger.info(
        "TTS %s queued: chars=%d lang=%s speed=%d skip_refine=%s text=%r",
        request_id,
        len(req.text),
        req.lang,
        req.speed,
        req.skip_refine_text,
        req.text[:80],
    )

    try:
        with _inference_lock:
            inference_started = time.perf_counter()
            queue_wait = inference_started - request_started
            logger.info(
                "TTS %s inference start: queue_wait=%.3fs stream_batch=%d "
                "stream_speed=%d pass_first=%d",
                request_id,
                queue_wait,
                STREAM_BATCH,
                STREAM_SPEED,
                PASS_FIRST_N_BATCHES,
            )

            params_started = time.perf_counter()
            refine_params, infer_params = _build_infer_params(req)
            params_elapsed = time.perf_counter() - params_started

            # IMPORTANT: split_text=False keeps this request as one sequential
            # audio stream. ChatTTS' split-text streaming path can represent
            # multiple items as a batch; that is not suitable for a single raw
            # PCM wire stream consumed by an embedded speaker.
            generator_setup_started = time.perf_counter()
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
            generator_created = time.perf_counter()

            logger.info(
                "TTS %s generator ready: params=%.3fms setup=%.3fms",
                request_id,
                params_elapsed * 1000.0,
                (generator_created - generator_setup_started) * 1000.0,
            )

            for wav_chunk in wav_generator:
                convert_started = time.perf_counter()
                pcm = _float_audio_to_pcm16_bytes(wav_chunk)
                convert_elapsed = time.perf_counter() - convert_started
                if not pcm:
                    continue

                now = time.perf_counter()
                total_chunks += 1
                total_bytes += len(pcm)

                chunk_audio_seconds = len(pcm) / PCM_BYTES_PER_SECOND
                gap = 0.0 if last_chunk_at is None else now - last_chunk_at

                if first_chunk_at is None:
                    first_chunk_at = now
                    first_from_infer = first_chunk_at - inference_started
                    first_from_request = first_chunk_at - request_started
                    logger.info(
                        "TTS %s first PCM: request_latency=%.3fs "
                        "inference_latency=%.3fs bytes=%d audio=%.3fs",
                        request_id,
                        first_from_request,
                        first_from_infer,
                        len(pcm),
                        chunk_audio_seconds,
                    )

                if LOG_CHUNKS:
                    logger.info(
                        "TTS %s chunk=%d bytes=%d audio=%.3fs gap=%.3fs "
                        "pcm_convert=%.2fms",
                        request_id,
                        total_chunks,
                        len(pcm),
                        chunk_audio_seconds,
                        gap,
                        convert_elapsed * 1000.0,
                    )

                last_chunk_at = now
                yield pcm

    except GeneratorExit:
        logger.warning("TTS %s client disconnected", request_id)
        raise
    except Exception:
        logger.exception("TTS %s inference/stream failed", request_id)
        raise
    finally:
        finished = time.perf_counter()
        request_elapsed = finished - request_started
        audio_seconds = total_bytes / PCM_BYTES_PER_SECOND

        if inference_started is not None:
            inference_elapsed = finished - inference_started
        else:
            inference_elapsed = 0.0

        if first_chunk_at is not None:
            first_audio_latency = first_chunk_at - request_started
            post_first_elapsed = finished - first_chunk_at
        else:
            first_audio_latency = -1.0
            post_first_elapsed = 0.0

        rtf = (
            inference_elapsed / audio_seconds
            if audio_seconds > 0.0 and inference_elapsed > 0.0
            else 0.0
        )
        realtime_speed = (1.0 / rtf) if rtf > 0.0 else 0.0

        logger.info(
            "TTS %s PERF: audio=%.3fs request=%.3fs inference=%.3fs "
            "first_audio=%.3fs post_first=%.3fs RTF=%.3f speed=%.2fx "
            "chunks=%d bytes=%d",
            request_id,
            audio_seconds,
            request_elapsed,
            inference_elapsed,
            first_audio_latency,
            post_first_elapsed,
            rtf,
            realtime_speed,
            total_chunks,
            total_bytes,
        )


def _run_startup_warmup() -> tuple[bool, float]:
    """Run one tiny inference and discard PCM to warm CUDA/PyTorch caches."""
    if not WARMUP_ENABLED:
        logger.info("Startup warmup disabled by CHATTTS_WARMUP=0")
        return False, 0.0

    started = time.perf_counter()
    logger.info("Startup warmup begin: text=%r", WARMUP_TEXT)

    req = TTSRequest(
        text=WARMUP_TEXT,
        lang="zh",
        skip_refine_text=True,
    )

    total_bytes = 0
    try:
        for pcm in _pcm_stream("warmup", req):
            total_bytes += len(pcm)
    except Exception:
        elapsed = time.perf_counter() - started
        logger.exception("Startup warmup failed after %.3fs", elapsed)
        return False, elapsed

    elapsed = time.perf_counter() - started
    audio_seconds = total_bytes / PCM_BYTES_PER_SECOND
    logger.info(
        "Startup warmup complete: elapsed=%.3fs discarded_audio=%.3fs bytes=%d",
        elapsed,
        audio_seconds,
        total_bytes,
    )
    return True, elapsed


@asynccontextmanager
async def lifespan(_: FastAPI):
    global _chat, _speaker_embedding, _speaker_mode, _speaker_file_in_use
    global _warmup_ok, _warmup_elapsed

    startup_started = time.perf_counter()
    logger.info(
        "Loading ChatTTS model: source=%s compile=%s",
        MODEL_SOURCE,
        COMPILE_MODEL,
    )

    chat = ChatTTS.Chat()
    model_load_started = time.perf_counter()
    loaded = chat.load(source=MODEL_SOURCE, compile=COMPILE_MODEL)
    model_load_elapsed = time.perf_counter() - model_load_started
    if not loaded:
        raise RuntimeError(
            f"ChatTTS model load failed (source={MODEL_SOURCE!r}). "
            "Check network/model cache and server logs."
        )

    # Load the selected speaker once at startup and reuse it for every request.
    speaker_started = time.perf_counter()
    speaker_embedding, speaker_mode, speaker_path = _load_speaker(chat)
    speaker_elapsed = time.perf_counter() - speaker_started

    _chat = chat
    _speaker_embedding = speaker_embedding
    _speaker_mode = speaker_mode
    _speaker_file_in_use = speaker_path

    if speaker_mode == "file":
        logger.info(
            "ChatTTS model ready: device=%s speaker_file=%s native_pcm=%dHz/%dbit/mono",
            getattr(chat, "device", "unknown"),
            speaker_path,
            CHAT_TTS_SAMPLE_RATE,
            PCM_BITS_PER_SAMPLE,
        )
    else:
        logger.info(
            "ChatTTS model ready: device=%s speaker_seed=%d native_pcm=%dHz/%dbit/mono",
            getattr(chat, "device", "unknown"),
            SPEAKER_SEED,
            CHAT_TTS_SAMPLE_RATE,
            PCM_BITS_PER_SAMPLE,
        )

    _warmup_ok, _warmup_elapsed = _run_startup_warmup()

    startup_elapsed = time.perf_counter() - startup_started
    logger.info(
        "STARTUP PERF: model_load=%.3fs speaker_init=%.3fs warmup=%.3fs "
        "warmup_ok=%s total=%.3fs",
        model_load_elapsed,
        speaker_elapsed,
        _warmup_elapsed,
        _warmup_ok,
        startup_elapsed,
    )
    logger.info(
        "ChatTTS ready for ESP32: batch=%d speed=%d pass_first=%d",
        STREAM_BATCH,
        STREAM_SPEED,
        PASS_FIRST_N_BATCHES,
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
        _speaker_mode = "not-loaded"
        _speaker_file_in_use = None
        _warmup_ok = False
        _warmup_elapsed = 0.0


app = FastAPI(
    title=APP_NAME,
    version="0.4.0",
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
        # 保留 speaker_seed 字段兼容现有检查脚本；file 模式下它只是 fallback 值。
        "speaker_seed": SPEAKER_SEED,
        "speaker": {
            "mode": _speaker_mode,
            "file": _speaker_file_in_use,
            "configured_file": SPEAKER_FILE or None,
            "directory": SPEAKER_DIR,
            "fallback_seed": SPEAKER_SEED,
        },
        "warmup": {
            "enabled": WARMUP_ENABLED,
            "ok": _warmup_ok,
            "elapsed_seconds": round(_warmup_elapsed, 3),
        },
        "stream": {
            "batch": STREAM_BATCH,
            "speed": STREAM_SPEED,
            "pass_first_n_batches": PASS_FIRST_N_BATCHES,
            "log_chunks": LOG_CHUNKS,
        },
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
