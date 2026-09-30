"""Find a raw stereo PCM excerpt inside another capture at candidate speeds."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def read_mono(path: Path) -> np.ndarray:
    samples = np.fromfile(path, dtype="<i2")
    samples = samples[: samples.size - samples.size % 2]
    return samples.reshape(-1, 2).astype(np.float64).mean(axis=1)


def resample(samples: np.ndarray, factor: float) -> np.ndarray:
    positions = np.arange(0.0, samples.size, factor, dtype=np.float64)
    return np.interp(positions, np.arange(samples.size), samples)


def normalized_correlation(search: np.ndarray, template: np.ndarray) -> tuple[float, int]:
    search = search - np.mean(search)
    template = template - np.mean(template)
    fft_size = 1 << (search.size + template.size - 1).bit_length()
    correlation = np.fft.irfft(
        np.fft.rfft(search, fft_size) * np.fft.rfft(template[::-1], fft_size),
        fft_size,
    )[template.size - 1 : search.size]
    cumulative = np.concatenate(([0.0], np.cumsum(search * search)))
    energy = cumulative[template.size :] - cumulative[: -template.size]
    denominator = np.sqrt(np.maximum(energy, 1e-30) * np.dot(template, template))
    normalized = correlation / denominator
    index = int(np.argmax(np.abs(normalized)))
    return float(normalized[index]), index


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("search", type=Path)
    parser.add_argument("template", type=Path)
    parser.add_argument("--template-start", type=float, required=True)
    parser.add_argument("--template-seconds", type=float, default=3.0)
    parser.add_argument("--rate", type=int, default=48_000)
    parser.add_argument("--analysis-rate", type=int, default=4_000)
    parser.add_argument("--min-speed", type=float, default=0.75)
    parser.add_argument("--max-speed", type=float, default=1.25)
    parser.add_argument("--speed-step", type=float, default=0.025)
    args = parser.parse_args()

    stride = args.rate // args.analysis_rate
    search = read_mono(args.search)[::stride]
    source = read_mono(args.template)
    start = int(round(args.template_start * args.rate))
    count = int(round(args.template_seconds * args.rate))
    source = source[start : start + count][::stride]

    results: list[tuple[float, float, float, int]] = []
    speeds = np.arange(
        args.min_speed,
        args.max_speed + args.speed_step / 2.0,
        args.speed_step,
    )
    for speed in speeds:
        candidate = resample(source, float(speed))
        score, index = normalized_correlation(search, candidate)
        results.append((abs(score), score, float(speed), index))

    for _, score, speed, index in sorted(results, reverse=True)[:12]:
        print(
            f"speed={speed:.6f} correlation={score:+.6f} "
            f"search_time={index / args.analysis_rate:.6f}s"
        )


if __name__ == "__main__":
    main()