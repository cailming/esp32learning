"""Small desktop client used to verify the ChatTTS streaming API.

It consumes the same raw PCM stream that the ESP32 will consume, but writes it
into a WAV container so macOS can play it with `afplay`.
"""

from __future__ import annotations

import argparse
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
    args = parser.parse_args()

    print(f"POST {args.url}")
    print(f"text: {args.text}")

    with requests.post(
        args.url,
        json={"text": args.text, "lang": "zh"},
        stream=True,
        timeout=(10, 300),
    ) as response:
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

        total = 0
        with wave.open(args.output, "wb") as wav:
            wav.setnchannels(channels)
            wav.setsampwidth(bits // 8)
            wav.setframerate(sample_rate)

            for chunk in response.iter_content(chunk_size=4096):
                if not chunk:
                    continue
                wav.writeframesraw(chunk)
                total += len(chunk)

    seconds = total / (sample_rate * channels * (bits // 8))
    print(f"saved: {args.output}")
    print(f"pcm bytes: {total}, duration: {seconds:.2f}s")
    print(f"macOS playback: afplay {args.output}")


if __name__ == "__main__":
    main()
