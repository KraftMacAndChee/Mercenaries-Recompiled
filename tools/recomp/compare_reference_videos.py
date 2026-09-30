import argparse
from pathlib import Path

import cv2
import numpy as np


def inspect_video(path: Path, output_dir: Path, columns: int = 4,
                  start: float | None = None, end: float | None = None,
                  interval: float | None = None,
                  crop: tuple[int, int, int, int] | None = None,
                  save_frames: bool = False) -> dict:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise RuntimeError(f"Could not open {path}")
    fps = capture.get(cv2.CAP_PROP_FPS)
    frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    duration = frame_count / fps if fps else 0.0
    last_time = max(0.0, duration - 1.0 / max(fps, 1.0))
    if interval is not None:
        first_time = max(0.0, start or 0.0)
        final_time = min(last_time, end if end is not None else last_time)
        times = np.arange(first_time, final_time + interval * 0.5, interval)
    else:
        sample_count = min(24, max(8, int(duration / 4.0) + 1))
        times = np.linspace(0.0, last_time, sample_count)
    frames = []
    metrics = []
    for seconds in times:
        capture.set(cv2.CAP_PROP_POS_MSEC, float(seconds * 1000.0))
        ok, frame = capture.read()
        if not ok:
            continue
        if crop is not None:
            x, y, crop_width, crop_height = crop
            frame = frame[y:y + crop_height, x:x + crop_width]
        if save_frames:
            frame_path = output_dir / f"{path.stem}-{seconds:07.2f}s.png"
            frame_path.parent.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(frame_path), frame)
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        metrics.append((float(seconds), float(cv2.Laplacian(gray, cv2.CV_64F).var())))
        resized = cv2.resize(frame, (480, 270), interpolation=cv2.INTER_AREA)
        cv2.rectangle(resized, (0, 0), (130, 28), (0, 0, 0), -1)
        cv2.putText(resized, f"{seconds:6.1f}s", (8, 20), cv2.FONT_HERSHEY_SIMPLEX,
                    0.55, (255, 255, 255), 1, cv2.LINE_AA)
        frames.append(resized)
    capture.release()
    rows = (len(frames) + columns - 1) // columns
    sheet = np.zeros((rows * 270, columns * 480, 3), dtype=np.uint8)
    for index, frame in enumerate(frames):
        row, column = divmod(index, columns)
        sheet[row * 270:(row + 1) * 270, column * 480:(column + 1) * 480] = frame
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / f"{path.stem}-contact-sheet.jpg"
    cv2.imwrite(str(output_path), sheet, [cv2.IMWRITE_JPEG_QUALITY, 92])
    return {
        "path": str(path), "width": width, "height": height, "fps": fps,
        "frames": frame_count, "duration": duration,
        "contact_sheet": str(output_path.resolve()), "sharpness_samples": metrics,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("videos", nargs="+", type=Path)
    parser.add_argument("--start", type=float)
    parser.add_argument("--end", type=float)
    parser.add_argument("--interval", type=float)
    parser.add_argument("--crop", help="x,y,width,height")
    parser.add_argument("--save-frames", action="store_true")
    args = parser.parse_args()
    crop = tuple(int(value) for value in args.crop.split(",")) if args.crop else None
    if crop is not None and len(crop) != 4:
        parser.error("--crop requires x,y,width,height")
    for video in args.videos:
        print(inspect_video(video, args.output_dir, start=args.start,
                            end=args.end, interval=args.interval, crop=crop,
                            save_frames=args.save_frames))


if __name__ == "__main__":
    main()