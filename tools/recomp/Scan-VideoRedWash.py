"""Locate sustained full-frame red washes in a gameplay recording."""

from __future__ import annotations

import argparse

import cv2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("video")
    parser.add_argument("--step", type=int, default=3)
    args = parser.parse_args()

    capture = cv2.VideoCapture(args.video)
    fps = capture.get(cv2.CAP_PROP_FPS)
    frame_index = -1
    active: list[tuple[float, float, float, float]] = []
    events: list[list[tuple[float, float, float, float]]] = []
    while True:
        ok, frame = capture.read()
        frame_index += 1
        if not ok:
            break
        if frame_index % args.step:
            continue
        sample = cv2.resize(frame, (64, 36))
        blue, green, red = sample.reshape(-1, 3).mean(axis=0)
        timestamp = frame_index / fps
        red_wash = red >= 35.0 and red >= green * 1.65 and red >= blue * 1.65
        if red_wash:
            active.append((timestamp, float(red), float(green), float(blue)))
        elif active:
            events.append(active)
            active = []
    if active:
        events.append(active)

    minimum_samples = max(1, int(0.20 * fps / args.step))
    for event in events:
        if len(event) < minimum_samples:
            continue
        peak = max(event, key=lambda item: item[1] - max(item[2], item[3]))
        print(
            f"{event[0][0]:.3f}..{event[-1][0]:.3f}s "
            f"duration={event[-1][0] - event[0][0]:.3f}s "
            f"peak={peak[0]:.3f}s rgb={peak[1]:.2f}/{peak[2]:.2f}/{peak[3]:.2f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
