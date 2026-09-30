#!/usr/bin/env python3
"""Locate mono WAV references in raw stereo PCM and estimate output gain."""

from __future__ import annotations

import argparse
import math
import wave
from pathlib import Path

import numpy as np


def read_wav_mono(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as wav:
        if wav.getsampwidth() != 2:
            raise ValueError(f"{path}: only 16-bit PCM is supported")
        rate = wav.getframerate()
        channels = wav.getnchannels()
        samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
    samples = samples.astype(np.float32).reshape(-1, channels).mean(axis=1)
    return samples / 32768.0, rate


def resample_linear(samples: np.ndarray, source_rate: int,
                    target_rate: int) -> np.ndarray:
    count = max(1, round(len(samples) * target_rate / source_rate))
    source_x = np.arange(len(samples), dtype=np.float64)
    target_x = np.arange(count, dtype=np.float64) * source_rate / target_rate
    return np.interp(target_x, source_x, samples).astype(np.float32)


def fft_valid_correlation(signal: np.ndarray,
                          reference: np.ndarray) -> tuple[int, float]:
    """Return the best valid start offset and normalized correlation."""
    reference = reference - reference.mean()
    signal = signal - signal.mean()
    fft_size = 1 << (len(signal) + len(reference) - 1).bit_length()
    spectrum = np.fft.rfft(signal, fft_size)
    reference_spectrum = np.fft.rfft(reference[::-1], fft_size)
    correlation = np.fft.irfft(spectrum * reference_spectrum, fft_size)
    valid = correlation[len(reference) - 1:len(signal)]
    squared = np.concatenate((np.zeros(1), np.cumsum(signal * signal,
                                                      dtype=np.float64)))
    window_energy = squared[len(reference):] - squared[:-len(reference)]
    denominator = np.sqrt(np.maximum(window_energy, 1e-20) *
                          max(float(np.dot(reference, reference)), 1e-20))
    normalized = valid / denominator
    offset = int(np.nanargmax(normalized))
    return offset, float(normalized[offset])


def fit_gain(output: np.ndarray, reference: np.ndarray) -> tuple[float, float, float, float]:
    reference = reference - reference.mean()
    output = output - output.mean(axis=0)
    energy = float(np.dot(reference, reference))
    gains = np.dot(reference, output) / max(energy, 1e-20)
    predicted = reference[:, None] * gains[None, :]
    residual = output - predicted
    explained = 1.0 - float(np.sum(residual * residual)) / max(
        float(np.sum(output * output)), 1e-20)
    source_rms = float(np.sqrt(np.mean(reference * reference)))
    output_rms = float(np.sqrt(np.mean(output * output)))
    return (float(np.sqrt(np.mean(gains * gains))), explained,
            source_rms, output_rms)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pcm", type=Path,
                        help="signed 16-bit little-endian 48 kHz stereo PCM")
    parser.add_argument("wav", nargs="+", type=Path)
    parser.add_argument("--search-rate", type=int, default=6000)
    args = parser.parse_args()

    raw = np.memmap(args.pcm, dtype="<i2", mode="r")
    stereo = raw[:len(raw) // 2 * 2].reshape(-1, 2)
    search_step = 48000 // args.search_rate
    if 48000 % args.search_rate:
        raise ValueError("search rate must divide 48000")
    search = stereo[::search_step].astype(np.float32).mean(axis=1) / 32768.0

    for wav_path in args.wav:
        source, source_rate = read_wav_mono(wav_path)
        reference_search = resample_linear(source, source_rate,
                                           args.search_rate)
        coarse_offset, score = fft_valid_correlation(search, reference_search)
        coarse_48k = coarse_offset * search_step
        reference_48k = resample_linear(source, source_rate, 48000)
        radius = search_step * 3
        best = None
        for offset in range(max(0, coarse_48k - radius),
                            min(len(stereo) - len(reference_48k),
                                coarse_48k + radius) + 1):
            segment = stereo[offset:offset + len(reference_48k)].astype(
                np.float32) / 32768.0
            gain, explained, source_rms, output_rms = fit_gain(
                segment, reference_48k)
            candidate = (explained, gain, offset, source_rms, output_rms)
            if best is None or candidate > best:
                best = candidate
        assert best is not None
        explained, gain, offset, source_rms, output_rms = best
        db = 20.0 * math.log10(max(gain, 1e-12))
        rms_ratio = output_rms / max(source_rms, 1e-12)
        rms_db = 20.0 * math.log10(max(rms_ratio, 1e-12))
        print(f"{wav_path.name}: time={offset / 48000.0:.3f}s "
              f"corr={score:.5f} gain={gain:.6f} ({db:.2f} dB) "
              f"explained={explained:.5f} rms_ratio={rms_ratio:.6f} "
              f"({rms_db:.2f} dB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
