"""Compare captured 48 kHz stereo PCM with a source WAV at candidate speeds."""

from __future__ import annotations

import argparse
import wave
from pathlib import Path

import numpy as np


def read_wav_mono(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as wav_file:
        if wav_file.getsampwidth() != 2:
            raise ValueError("only signed 16-bit PCM WAV files are supported")
        channels = wav_file.getnchannels()
        rate = wav_file.getframerate()
        samples = np.frombuffer(
            wav_file.readframes(wav_file.getnframes()), dtype="<i2"
        )
    return samples.reshape(-1, channels).astype(np.float64).mean(axis=1), rate


def read_raw_stereo_mono(path: Path) -> np.ndarray:
    samples = np.fromfile(path, dtype="<i2")
    samples = samples[: samples.size - samples.size % 2]
    return samples.reshape(-1, 2).astype(np.float64).mean(axis=1)


def resample_for_speed(
    source: np.ndarray,
    source_rate: int,
    output_rate: int,
    speed: float,
    seconds: float,
) -> np.ndarray:
    input_count = min(source.size, int(round(seconds * source_rate)))
    output_count = int(np.floor(input_count * output_rate / source_rate / speed))
    positions = np.arange(output_count, dtype=np.float64) * source_rate * speed / output_rate
    return np.interp(positions, np.arange(input_count), source[:input_count])


def best_normalized_match(search: np.ndarray, template: np.ndarray) -> tuple[float, int]:
    search = search.astype(np.float64) - np.mean(search)
    template = template.astype(np.float64) - np.mean(template)
    if template.size > search.size:
        raise ValueError("template is longer than search interval")

    fft_size = 1 << (search.size + template.size - 1).bit_length()
    correlation = np.fft.irfft(
        np.fft.rfft(search, fft_size) * np.fft.rfft(template[::-1], fft_size),
        fft_size,
    )
    correlation = correlation[template.size - 1 : search.size]

    cumulative = np.concatenate(([0.0], np.cumsum(search * search)))
    energy = cumulative[template.size :] - cumulative[: -template.size]
    denominator = np.sqrt(np.maximum(energy, 1e-30) * np.dot(template, template))
    normalized = correlation / denominator
    index = int(np.argmax(np.abs(normalized)))
    return float(normalized[index]), index


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("--start", type=float, default=21.0)
    parser.add_argument("--end", type=float, default=28.0)
    parser.add_argument("--source-start", type=float, default=0.0)
    parser.add_argument("--template-seconds", type=float, default=0.8)
    parser.add_argument("--speed", type=float, action="append",
                        help="Check only these speed ratios; repeat for multiple values")
    args = parser.parse_args()

    output_rate = 48_000
    if args.start < 0 or args.end <= args.start or args.template_seconds <= 0:
        parser.error('invalid capture/template interval')
    if args.speed and any(not np.isfinite(speed) or speed <= 0 for speed in args.speed):
        parser.error('speed ratios must be finite and positive')
    # Long mission captures can exceed hundreds of megabytes. Read only the
    # requested interval instead of allocating several full-duration copies.
    first_frame = int(args.start * output_rate)
    frame_count = int(args.end * output_rate) - first_frame
    samples = np.fromfile(args.capture, dtype='<i2', count=frame_count * 2,
                          offset=first_frame * 4)
    samples = samples[:samples.size - samples.size % 2]
    search = samples.reshape(-1, 2).astype(np.float64).mean(axis=1)
    source, source_rate = read_wav_mono(args.source)
    source = source[int(args.source_start * source_rate) :]

    speeds = np.unique(
        np.concatenate(
            (
                np.arange(0.5, 2.501, 0.05),
                np.array([1.0, 2.176959, 1.0 / 2.176959]),
            )
        )
    )
    if args.speed:
        speeds = np.unique(args.speed)
    matches: list[tuple[float, float, float, int]] = []
    for speed in speeds:
        template = resample_for_speed(
            source, source_rate, output_rate, float(speed), args.template_seconds
        )
        score, offset = best_normalized_match(search, template)
        matches.append((abs(score), score, float(speed), offset))

    print(
        f"capture={args.capture} source={args.source} "
        f"source_rate={source_rate} search={args.start:.3f}-{args.end:.3f}s"
    )
    for _, score, speed, offset in sorted(matches, reverse=True)[:12]:
        print(
            f"speed={speed:.6f} correlation={score:+.6f} "
            f"capture_time={args.start + offset / output_rate:.6f}s"
        )


if __name__ == "__main__":
    main()
