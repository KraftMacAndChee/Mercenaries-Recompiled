#!/usr/bin/env python3
"""Extract a compact, timestamped contact sheet from a video interval."""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start", type=float, required=True)
    parser.add_argument("--end", type=float, required=True)
    parser.add_argument("--step", type=float, default=0.5)
    parser.add_argument("--columns", type=int, default=4)
    args = parser.parse_args()
    if not (0.0 <= args.start <= args.end and args.step > 0.0 and
            1 <= args.columns <= 8):
        raise ValueError("Invalid interval")

    capture = cv2.VideoCapture(str(args.video.resolve(strict=True)))
    if not capture.isOpened():
        raise RuntimeError("Cannot open video")
    frames: list[np.ndarray] = []
    timestamp = args.start
    while timestamp <= args.end + 1e-9:
        capture.set(cv2.CAP_PROP_POS_MSEC, timestamp * 1000.0)
        ok, frame = capture.read()
        if not ok:
            break
        thumb = cv2.resize(frame, (480, 270), interpolation=cv2.INTER_AREA)
        cv2.putText(thumb, f"{timestamp:.3f}s", (8, 25),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, (255, 255, 255), 2,
                    cv2.LINE_AA)
        frames.append(thumb)
        timestamp += args.step
    capture.release()
    if not frames:
        raise RuntimeError("No frames decoded")
    blank = np.zeros_like(frames[0])
    rows = []
    for start in range(0, len(frames), args.columns):
        row = frames[start:start + args.columns]
        row += [blank] * (args.columns - len(row))
        rows.append(np.hstack(row))
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(output), np.vstack(rows),
                       [cv2.IMWRITE_JPEG_QUALITY, 92]):
        raise RuntimeError(f"Failed to write {output}")
    print(output)


if __name__ == "__main__":
    main()
