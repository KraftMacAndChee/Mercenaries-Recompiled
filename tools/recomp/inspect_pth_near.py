import argparse
import pathlib
import re


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--xmin", type=float, required=True)
    parser.add_argument("--xmax", type=float, required=True)
    parser.add_argument("--zmin", type=float, required=True)
    parser.add_argument("--zmax", type=float, required=True)
    args = parser.parse_args()

    text = args.path.read_text(encoding="utf-8", errors="replace")
    for chunk in re.split(r'(?=Path\(")', text):
        match = re.match(r'Path\("([^"]+)', chunk)
        if match is None:
            continue
        points = [
            tuple(map(float, values))
            for values in re.findall(
                r"Position\(([-0-9.]+),\s*([-0-9.]+),\s*([-0-9.]+)\)",
                chunk,
            )
        ]
        nearby = [
            point
            for point in points
            if args.xmin < point[0] < args.xmax
            and args.zmin < point[2] < args.zmax
        ]
        if nearby:
            print(
                match.group(1),
                "nodes=", len(points),
                "first=", points[0],
                "last=", points[-1],
                "near=", nearby,
            )


if __name__ == "__main__":
    main()
