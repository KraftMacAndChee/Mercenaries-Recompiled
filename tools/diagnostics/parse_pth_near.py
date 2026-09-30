"""List authored PTH nodes near a runtime X/Z coordinate."""

from __future__ import annotations

import argparse
import math
import re
from pathlib import Path


PATH_RE = re.compile(r'Path\("([^"]+)"\)\s*\{(.*?)(?=\nPath\("|\Z)', re.S)
POSITION_RE = re.compile(
    r"Position\(([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)\)"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("x", type=float)
    parser.add_argument("runtime_z", type=float)
    parser.add_argument("--radius", type=float, default=200.0)
    args = parser.parse_args()

    text = args.path.read_text(encoding="utf-8", errors="ignore")
    rows = []
    for match in PATH_RE.finditer(text):
        points = [tuple(map(float, item)) for item in POSITION_RE.findall(match.group(2))]
        if not points:
            continue
        nearest = min(
            math.hypot(x - args.x, -editor_z - args.runtime_z)
            for x, _y, editor_z in points
        )
        if nearest > args.radius:
            continue
        runtime_points = [(round(x, 3), round(-editor_z, 3)) for x, _y, editor_z in points]
        rows.append((nearest, match.group(1), runtime_points))

    for nearest, name, points in sorted(rows):
        print(f"{nearest:8.3f} {name} nodes={len(points)}")
        print("  " + " -> ".join(f"({x:.3f},{z:.3f})" for x, z in points))


if __name__ == "__main__":
    main()
