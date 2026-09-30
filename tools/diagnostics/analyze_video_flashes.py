#!/usr/bin/env python3
"""Find brief full-frame luminance/tint excursions in a gameplay recording.

This is a read-only diagnostic.  It decodes the source sequentially, compares
each frame's central-image colour statistics with a local temporal median, and
writes only a compact CSV plus JPEGs of the strongest separated candidates.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import cv2
import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--candidates", type=int, default=24)
    parser.add_argument("--separation-seconds", type=float, default=0.75)
    args = parser.parse_args()

    video = args.video.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)

    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise RuntimeError(f"Cannot open {video}")
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    expected = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    if not (fps > 0.0 and expected > 0):
        raise RuntimeError("Video reports invalid timing metadata")

    statistics: list[tuple[float, float, float, float]] = []
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        height, width = frame.shape[:2]
        # Ignore the outer five percent, where letterboxing/window borders can
        # otherwise look like a full-screen black flash after a mode change.
        image = frame[height // 20: height - height // 20,
                      width // 20: width - width // 20]
        blue, green, red, _ = cv2.mean(image)
        luminance = 0.0722 * blue + 0.7152 * green + 0.2126 * red
        statistics.append((blue, green, red, luminance))
    capture.release()

    values = np.asarray(statistics, dtype=np.float64)
    if len(values) < 5:
        raise RuntimeError("Too few decoded frames")
    channel_sum = np.maximum(values[:, :3].sum(axis=1, keepdims=True), 1.0)
    ratios = values[:, :3] / channel_sum
    radius = max(3, int(round(fps * 0.25)))
    window = radius * 2 + 1
    padded_ratios = np.pad(ratios, ((radius, radius), (0, 0)), mode="edge")
    padded_luma = np.pad(values[:, 3], (radius, radius), mode="edge")
    local_ratios = np.median(
        np.lib.stride_tricks.sliding_window_view(
            padded_ratios, window, axis=0), axis=2)
    local_luma = np.median(
        np.lib.stride_tricks.sliding_window_view(padded_luma, window), axis=1)

    colour_delta = np.linalg.norm(ratios - local_ratios, axis=1)
    luma_delta = np.abs(np.log((values[:, 3] + 1.0) / (local_luma + 1.0)))
    # Tint shifts dominate the score, while grey/black flashes remain visible
    # through the logarithmic luminance term.
    score = colour_delta * 400.0 + luma_delta * 8.0

    with (output / "frame-statistics.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(("frame", "seconds", "mean_b", "mean_g", "mean_r",
                         "mean_luma", "colour_delta", "luma_delta", "score"))
        for index, row in enumerate(values):
            writer.writerow((index, index / fps, *row,
                             colour_delta[index], luma_delta[index], score[index]))

    separation = max(1, int(round(fps * args.separation_seconds)))
    chosen: list[int] = []
    for index in np.argsort(score)[::-1]:
        frame_index = int(index)
        if all(abs(frame_index - prior) >= separation for prior in chosen):
            chosen.append(frame_index)
            if len(chosen) >= args.candidates:
                break
    chosen.sort()

    capture = cv2.VideoCapture(str(video))
    rows: list[tuple[int, float, float, str]] = []
    thumbnails: list[np.ndarray] = []
    for rank, frame_index in enumerate(chosen, 1):
        capture.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
        ok, frame = capture.read()
        if not ok:
            continue
        name = f"candidate-{rank:02d}-f{frame_index:06d}-{frame_index / fps:09.3f}s.jpg"
        cv2.imwrite(str(output / name), frame, [cv2.IMWRITE_JPEG_QUALITY, 90])
        thumb = cv2.resize(frame, (480, 270), interpolation=cv2.INTER_AREA)
        cv2.putText(thumb, f"{frame_index / fps:.3f}s  score={score[frame_index]:.2f}",
                    (8, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2,
                    cv2.LINE_AA)
        thumbnails.append(thumb)
        rows.append((frame_index, frame_index / fps, score[frame_index], name))
    capture.release()

    with (output / "candidates.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(("frame", "seconds", "score", "image"))
        writer.writerows(rows)
    if thumbnails:
        columns = 3
        blank = np.zeros_like(thumbnails[0])
        sheet_rows = []
        for start in range(0, len(thumbnails), columns):
            row = thumbnails[start:start + columns]
            row += [blank] * (columns - len(row))
            sheet_rows.append(np.hstack(row))
        cv2.imwrite(str(output / "contact-sheet.jpg"), np.vstack(sheet_rows),
                    [cv2.IMWRITE_JPEG_QUALITY, 92])

    print(f"decoded={len(values)} fps={fps:.6f} seconds={len(values) / fps:.3f}")
    for frame_index, seconds, candidate_score, name in rows:
        print(f"{seconds:9.3f}s frame={frame_index:6d} score={candidate_score:8.3f} {name}")


if __name__ == "__main__":
    main()
