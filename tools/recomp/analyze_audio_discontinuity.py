"""Measure clipping, discontinuities, and silence gaps in PCM WAV audio."""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path)
    parser.add_argument("--window-ms", type=float, default=10.0)
    parser.add_argument("--silence-rms", type=float, default=40.0)
    parser.add_argument("--raw-rate", type=int)
    parser.add_argument("--raw-channels", type=int, default=2)
    parser.add_argument("--start-seconds", type=float, default=0.0)
    parser.add_argument("--duration-seconds", type=float)
    args = parser.parse_args()

    if args.raw_rate:
        channels = args.raw_channels
        rate = args.raw_rate
        audio = np.fromfile(args.wav, dtype="<i2")
    else:
        with wave.open(str(args.wav), "rb") as stream:
            channels = stream.getnchannels()
            rate = stream.getframerate()
            width = stream.getsampwidth()
            frames = stream.getnframes()
            if width != 2:
                raise RuntimeError(
                    f"expected signed 16-bit PCM, got {width * 8}-bit")
            audio = np.frombuffer(stream.readframes(frames), dtype="<i2")
    audio = audio.reshape(-1, channels).astype(np.int32)
    start_frame = max(0, round(args.start_seconds * rate))
    end_frame = len(audio)
    if args.duration_seconds is not None:
        end_frame = min(
            end_frame,
            start_frame + max(0, round(args.duration_seconds * rate)),
        )
    audio = audio[start_frame:end_frame]
    deltas = np.abs(np.diff(audio, axis=0))
    window_frames = max(1, round(rate * args.window_ms / 1000.0))
    usable = len(audio) - len(audio) % window_frames
    windows = audio[:usable].reshape(-1, window_frames, channels)
    rms = np.sqrt(np.mean(windows.astype(np.float64) ** 2, axis=(1, 2)))
    silent = rms <= args.silence_rms
    silent_runs: list[int] = []
    run = 0
    for value in silent:
        if value:
            run += 1
        elif run:
            silent_runs.append(run)
            run = 0
    if run:
        silent_runs.append(run)
    result = {
        "path": str(args.wav),
        "sample_rate": rate,
        "channels": channels,
        "start_seconds": start_frame / rate,
        "duration_seconds": len(audio) / rate,
        "peak_absolute": int(np.max(np.abs(audio), initial=0)),
        "clipped_samples": int(np.count_nonzero(np.abs(audio) >= 32760)),
        "maximum_sample_delta": int(np.max(deltas, initial=0)),
        "deltas_over_12000": int(np.count_nonzero(deltas > 12000)),
        "deltas_over_20000": int(np.count_nonzero(deltas > 20000)),
        "median_window_rms": float(np.median(rms)) if len(rms) else 0.0,
        "silent_window_fraction": float(np.mean(silent)) if len(silent) else 0.0,
        "longest_silence_ms": max(silent_runs, default=0) * args.window_ms,
        "silence_runs_at_least_100ms": sum(
            run * args.window_ms >= 100.0 for run in silent_runs),
        "silence_runs_at_least_500ms": sum(
            run * args.window_ms >= 500.0 for run in silent_runs),
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()