#!/usr/bin/env python3
"""Build contact sheets and objective image metrics for ordered BMP captures."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("--glob", default="*.bmp")
    parser.add_argument("--output-prefix", required=True)
    parser.add_argument("--columns", type=int, default=16)
    parser.add_argument("--thumb-width", type=int, default=120)
    parser.add_argument("--keyframe-step", type=int, default=8)
    parser.add_argument("--start", type=int, default=1)
    parser.add_argument("--end", type=int)
    args = parser.parse_args()

    def capture_index(path: Path) -> int:
        parts = path.stem.split("-")
        return int(parts[-2]) if parts[-1].isdigit() and parts[-2].isdigit() else int(parts[-1])

    paths = sorted(args.capture_dir.glob(args.glob), key=capture_index)
    paths = paths[args.start - 1 : args.end]
    if not paths:
        raise SystemExit(f"no captures matched {args.glob!r}")

    images = [Image.open(path).convert("RGB") for path in paths]
    thumb_height = round(images[0].height * args.thumb_width / images[0].width)

    def make_sheet(selected: list[tuple[int, Image.Image]], output: Path) -> None:
        rows = (len(selected) + args.columns - 1) // args.columns
        sheet = Image.new("RGB", (args.columns * args.thumb_width, rows * thumb_height), "black")
        draw = ImageDraw.Draw(sheet)
        for slot, (index, source) in enumerate(selected):
            thumb = source.resize((args.thumb_width, thumb_height), Image.Resampling.LANCZOS)
            x = (slot % args.columns) * args.thumb_width
            y = (slot // args.columns) * thumb_height
            sheet.paste(thumb, (x, y))
            draw.text((x + 2, y + 2), str(index), fill="white", stroke_width=1, stroke_fill="black")
        sheet.save(output, quality=92)

    prefix = args.capture_dir / args.output_prefix
    make_sheet(list(enumerate(images, 1)), prefix.with_name(prefix.name + "-contact-sheet.jpg"))
    keyframes = [(index, image) for index, image in enumerate(images, 1) if index == 1 or index == len(images) or (index - 1) % args.keyframe_step == 0]
    make_sheet(keyframes, prefix.with_name(prefix.name + "-keyframes.jpg"))

    means: list[float] = []
    black_fractions: list[float] = []
    adjacent_differences: list[float] = []
    previous: np.ndarray | None = None
    for image in images:
        array = np.asarray(image, dtype=np.float32)
        means.append(float(array.mean()))
        black_fractions.append(float(np.all(array < 8.0, axis=2).mean()))
        if previous is not None:
            adjacent_differences.append(float(np.abs(array - previous).mean()))
        previous = array

    maximum_difference = max(adjacent_differences, default=0.0)
    metrics = {
        "capture_count": len(paths),
        "first_capture": paths[0].name,
        "last_capture": paths[-1].name,
        "mean_rgb_min": min(means),
        "mean_rgb_max": max(means),
        "black_fraction_min": min(black_fractions),
        "black_fraction_max": max(black_fractions),
        "max_adjacent_rgb_difference": maximum_difference,
        "max_adjacent_difference_after_frame": adjacent_differences.index(maximum_difference) + 1 if adjacent_differences else None,
    }
    metrics_path = prefix.with_name(prefix.name + "-metrics.json")
    metrics_path.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
