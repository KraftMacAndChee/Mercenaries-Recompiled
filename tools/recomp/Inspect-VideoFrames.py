"""Build a compact, timestamped contact sheet from a local test recording."""

from __future__ import annotations

import argparse
import base64
import sys

import cv2
import numpy as np


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("video")
    parser.add_argument("timestamps", nargs="+", type=float)
    parser.add_argument("--output")
    parser.add_argument("--tile-width", type=int, default=480)
    parser.add_argument("--quality", type=int, default=88)
    parser.add_argument("--crop", nargs=4, type=int,
                        metavar=("X", "Y", "WIDTH", "HEIGHT"))
    args = parser.parse_args()

    capture = cv2.VideoCapture(args.video)
    frames: list[np.ndarray] = []
    stats: list[tuple[float, float, float, float]] = []
    for timestamp in args.timestamps:
        capture.set(cv2.CAP_PROP_POS_MSEC, timestamp * 1000.0)
        ok, frame = capture.read()
        if not ok:
            print(f"could not read {timestamp:.3f}s", file=sys.stderr)
            continue
        blue, green, red = frame.reshape(-1, 3).mean(axis=0)
        stats.append((timestamp, float(red), float(green), float(blue)))
        if args.crop:
            x, y, width, height = args.crop
            frame = frame[y:y + height, x:x + width]
        tile_height = args.tile_width * 9 // 16
        frame = cv2.resize(frame, (args.tile_width, tile_height))
        cv2.rectangle(frame, (0, 0), (150, 27), (255, 255, 255), -1)
        cv2.putText(frame, f"{timestamp:.3f}s", (8, 20),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 1,
                    cv2.LINE_AA)
        frames.append(frame)

    if not frames:
        return 1
    rows = []
    for index in range(0, len(frames), 2):
        right = (frames[index + 1] if index + 1 < len(frames)
                 else np.zeros_like(frames[index]))
        rows.append(np.hstack((frames[index], right)))
    sheet = np.vstack(rows)
    ok, encoded = cv2.imencode(".jpg", sheet,
                               [cv2.IMWRITE_JPEG_QUALITY, args.quality])
    if not ok:
        return 1
    print("STATS", stats)
    if args.output:
        if not cv2.imwrite(args.output, sheet):
            return 1
        print("OUTPUT", args.output)
    else:
        print("IMAGE", base64.b64encode(encoded).decode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
