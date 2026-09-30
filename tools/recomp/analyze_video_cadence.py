"""Measure duplicate-frame bursts in a cropped screen recording.

This is intentionally independent of container timestamps: it measures the
frames a user actually saw after a desktop recorder sampled the game window.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np


def analyze(path: Path, start: float, end: float,
            crop: tuple[int, int, int, int], threshold: float) -> dict:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise RuntimeError(f"could not open {path}")
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    capture.set(cv2.CAP_PROP_POS_MSEC, start * 1000.0)
    previous = None
    differences: list[float] = []
    duplicate_runs: list[int] = []
    duplicate_run = 0
    frames = 0
    x, y, width, height = crop
    while True:
        timestamp = float(capture.get(cv2.CAP_PROP_POS_MSEC)) / 1000.0
        if timestamp >= end:
            break
        ok, frame = capture.read()
        if not ok:
            break
        frame = frame[y:y + height, x:x + width]
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.resize(gray, (160, 120), interpolation=cv2.INTER_AREA)
        if previous is not None:
            difference = float(np.mean(cv2.absdiff(gray, previous)))
            differences.append(difference)
            if difference <= threshold:
                duplicate_run += 1
            elif duplicate_run:
                duplicate_runs.append(duplicate_run)
                duplicate_run = 0
        previous = gray
        frames += 1
    capture.release()
    if duplicate_run:
        duplicate_runs.append(duplicate_run)
    duplicate_frames = sum(duplicate_runs)
    return {
        "path": str(path),
        "start": start,
        "end": end,
        "source_fps": fps,
        "frames": frames,
        "threshold": threshold,
        "mean_difference": float(np.mean(differences)) if differences else 0.0,
        "median_difference": float(np.median(differences)) if differences else 0.0,
        "duplicate_frames": duplicate_frames,
        "duplicate_fraction": duplicate_frames / max(1, len(differences)),
        "duplicate_runs": len(duplicate_runs),
        "longest_duplicate_run_frames": max(duplicate_runs, default=0),
        "longest_duplicate_run_ms":
            max(duplicate_runs, default=0) * 1000.0 / max(fps, 1.0),
        "runs_at_least_250ms": sum(
            run / max(fps, 1.0) >= 0.250 for run in duplicate_runs),
        "runs_at_least_500ms": sum(
            run / max(fps, 1.0) >= 0.500 for run in duplicate_runs),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("video", type=Path)
    parser.add_argument("--start", type=float, required=True)
    parser.add_argument("--end", type=float, required=True)
    parser.add_argument("--crop", required=True, help="x,y,width,height")
    parser.add_argument("--threshold", type=float, default=0.35)
    args = parser.parse_args()
    crop = tuple(int(value) for value in args.crop.split(","))
    if len(crop) != 4:
        parser.error("--crop requires x,y,width,height")
    print(json.dumps(analyze(args.video, args.start, args.end, crop,
                             args.threshold), indent=2))


if __name__ == "__main__":
    main()