"""Find abrupt, screen-wide luminance or tint changes in a gameplay recording.

The detector works on consecutive, downscaled frames. Camera motion produces
large local differences with mixed signs, whereas a full-screen flash or tint
usually moves most pixels in a consistent RGB direction. This tool is kept
independent of Mercenaries runtime state so recordings from the recompiled
port and an emulator can be compared with the same measurements.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path

import cv2
import numpy as np


@dataclass
class FlashEvent:
    seconds: float
    frame: int
    score: float
    changed_fraction: float
    coherent_fraction: float
    mean_absolute_delta: float
    median_delta_bgr: tuple[float, float, float]
    mean_bgr: tuple[float, float, float]


def _triplet(values: np.ndarray) -> tuple[float, float, float]:
    return tuple(round(float(value), 3) for value in values)  # type: ignore[return-value]


def analyze(path: Path, start: float, end: float | None, width: int,
            minimum_score: float, cooldown_seconds: float) -> dict:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise RuntimeError(f"could not open {path}")
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    source_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    source_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    height = max(1, round(width * source_height / max(1, source_width)))
    capture.set(cv2.CAP_PROP_POS_MSEC, start * 1000.0)

    previous: np.ndarray | None = None
    events: list[FlashEvent] = []
    strongest: list[FlashEvent] = []
    processed = 0
    cooldown_frames = max(1, round(cooldown_seconds * fps))
    last_event_frame = -cooldown_frames

    while True:
        seconds = float(capture.get(cv2.CAP_PROP_POS_MSEC)) / 1000.0
        if end is not None and seconds >= end:
            break
        ok, frame = capture.read()
        if not ok:
            break
        small = cv2.resize(frame, (width, height), interpolation=cv2.INTER_AREA)
        current = small.astype(np.float32)
        if previous is not None:
            delta = current - previous
            magnitude = np.mean(np.abs(delta), axis=2)
            changed = magnitude >= 5.0
            changed_fraction = float(np.mean(changed))
            median_delta = np.median(delta.reshape(-1, 3), axis=0)
            dominant_sign = np.sign(median_delta)
            significant_channels = np.abs(median_delta) >= 1.0
            if np.any(significant_channels):
                direction_matches = np.ones(delta.shape[:2], dtype=bool)
                for channel in range(3):
                    if significant_channels[channel]:
                        direction_matches &= (
                            np.sign(delta[:, :, channel]) == dominant_sign[channel]
                        )
                coherent_fraction = float(np.mean(direction_matches & changed))
            else:
                coherent_fraction = 0.0
            mean_absolute_delta = float(np.mean(magnitude))
            score = (changed_fraction * coherent_fraction * 100.0 +
                     mean_absolute_delta * 0.08)
            event = FlashEvent(
                seconds=round(seconds, 6),
                frame=round(seconds * fps),
                score=round(score, 4),
                changed_fraction=round(changed_fraction, 5),
                coherent_fraction=round(coherent_fraction, 5),
                mean_absolute_delta=round(mean_absolute_delta, 4),
                median_delta_bgr=_triplet(median_delta),
                mean_bgr=_triplet(np.mean(current, axis=(0, 1))),
            )
            strongest.append(event)
            strongest.sort(key=lambda item: item.score, reverse=True)
            del strongest[40:]
            if (score >= minimum_score and
                    event.frame - last_event_frame >= cooldown_frames):
                events.append(event)
                last_event_frame = event.frame
        previous = current
        processed += 1

    capture.release()
    return {
        "path": str(path),
        "source_fps": fps,
        "start": start,
        "end": end,
        "analysis_size": [width, height],
        "processed_frames": processed,
        "minimum_score": minimum_score,
        "events": [asdict(event) for event in events],
        "strongest": [asdict(event) for event in strongest],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("video", type=Path)
    parser.add_argument("--start", type=float, default=0.0)
    parser.add_argument("--end", type=float)
    parser.add_argument("--width", type=int, default=160)
    parser.add_argument("--minimum-score", type=float, default=7.0)
    parser.add_argument("--cooldown", type=float, default=0.08)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.video, args.start, args.end, args.width,
                     args.minimum_score, args.cooldown)
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()