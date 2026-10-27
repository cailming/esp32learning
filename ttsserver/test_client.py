"""Small desktop client used to verify the ChatTTS streaming API.

It consumes the same raw PCM stream that the ESP32 will consume, but writes it
into a WAV container so macOS can play it with `afplay`.

The client also measures request/header latency, first-audio latency and total
streaming time so server inference cost can be separated from network/client
cost.
"""

from __future__ import annotations

import argparse
import time
import wave

import requests


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:8000/tts/stream",
        help="TTS streaming endpoint",
    )
    parser.add_argument(
        "--text",
        default="设备正常，当前未录像，声音已开启。",
        help="Text sent to ChatTTS",
    )
    parser.add_argument("--output", default="tts_test.wav")
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=4096,
        help="HTTP client read chunk size",
    )
    parser.add_argument(
        "--skip-refine-text",
        action="store_true",
        help="Ask ChatTTS to skip refine-text stage for latency comparison",
    )
    args = parser.parse_args()

    print(f"POST {args.url}")
    print(f"text: {args.text}")
    print(f"skip_refine_text: {args.skip_refine_text}")

    request_started = time.perf_counter()
    first_chunk_at: float | None = None
    header_at: float | None = None

    with requests.post(
        args.url,
        json={
            "text": args.text,
            "lang": "zh",
            "skip_refine_text": args.skip_refine_text,
        },
        stream=True,
        timeout=(10, 300),
    ) as response:
        header_at = time.perf_counter()
        response.raise_for_status()

        sample_rate = int(response.headers.get("X-Sample-Rate", "24000"))
        channels = int(response.headers.get("X-Channels", "1"))
        bits = int(response.headers.get("X-Bits-Per-Sample", "16"))
        audio_format = response.headers.get("X-Audio-Format", "unknown")
        request_id = response.headers.get("X-Request-Id", "unknown")

        if bits != 16:
            raise RuntimeError(f"This test client only supports PCM16, got {bits} bits")

        print(
            f"stream: request_id={request_id} format={audio_format} "
            f"{sample_rate}Hz {channels}ch {bits}bit"
        )
        print(f"HTTP headers latency: {header_at - request_started:.3f}s")

        total = 0
        chunks = 0
        last_progress_at = header_at

        with wave.open(args.output, "wb") as wav:
            wav.setnchannels(channels)
            wav.setsampwidth(bits // 8)
            wav.setframerate(sample_rate)

            for chunk in response.iter_content(chunk_size=args.chunk_size):
                if not chunk:
                    continue

                now = time.perf_counter()
                if first_chunk_at is None:
                    first_chunk_at = now
                    print(
                        "first PCM latency: "
                        f"{first_chunk_at - request_started:.3f}s "
                        f"(after headers: {first_chunk_at - header_at:.3f}s)"
                    )

                wav.writeframesraw(chunk)
                total += len(chunk)
                chunks += 1

                # Long CPU inference can look like a hung client. Print a small
                # progress line roughly every 2 seconds while data is arriving.
                if now - last_progress_at >= 2.0:
                    received_audio = total / (
                        sample_rate * channels * (bits // 8)
                    )
                    print(
                        f"receiving: chunks={chunks} bytes={total} "
                        f"audio={received_audio:.2f}s "
                        f"wall={now - request_started:.2f}s"
                    )
                    last_progress_at = now

    finished = time.perf_counter()
    seconds = total / (sample_rate * channels * (bits // 8))
    total_elapsed = finished - request_started
    header_latency = (
        header_at - request_started if header_at is not None else 0.0
    )
    first_latency = (
        first_chunk_at - request_started if first_chunk_at is not None else -1.0
    )
    receive_after_first = (
        finished - first_chunk_at if first_chunk_at is not None else 0.0
    )
    client_rtf = total_elapsed / seconds if seconds > 0 else 0.0
    client_speed = 1.0 / client_rtf if client_rtf > 0 else 0.0

    print()
    print("========== CLIENT PERF ==========")
    print(f"request_id        : {request_id}")
    print(f"HTTP headers      : {header_latency:.3f}s")
    print(f"first PCM         : {first_latency:.3f}s")
    print(f"after first PCM   : {receive_after_first:.3f}s")
    print(f"total wall        : {total_elapsed:.3f}s")
    print(f"audio duration    : {seconds:.3f}s")
    print(f"client RTF        : {client_rtf:.3f}")
    print(f"client speed      : {client_speed:.2f}x realtime")
    print(f"HTTP chunks       : {chunks}")
    print(f"PCM bytes         : {total}")
    print("=================================")
    print(f"saved: {args.output}")
    print(f"macOS playback: afplay {args.output}")


if __name__ == "__main__":
    main()
