"""Read-only comparison of a captured flare cell with bilinear source samples.

The color BMP and its grayscale alpha companion are emitted by the renderer.
This tool does not modify captures or prescribe a renderer correction.
"""
import argparse
from pathlib import Path
import struct
import numpy as np


def read_bmp(path):
    data = Path(path).read_bytes()
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height, planes, bits, compression = struct.unpack_from('<iiHHI', data, 18)
    if data[:2] != b'BM' or planes != 1 or bits != 24 or compression != 0 or width <= 0 or height == 0:
        raise ValueError('Expected an uncompressed 24-bit diagnostic BMP')
    pitch = (width * 3 + 3) & ~3
    pixels = np.frombuffer(data, dtype=np.uint8, offset=offset, count=abs(height)*pitch)
    pixels = pixels.reshape(abs(height), pitch)[:, :width*3].reshape(abs(height), width, 3)
    return (pixels[::-1] if height > 0 else pixels)[:, :, ::-1].astype(float)


def rgba(path):
    return np.concatenate((read_bmp(path), read_bmp(str(path)+'.alpha.bmp')[:, :, :1]), axis=2)


def bilinear(image, x, y):
    x = np.clip(x-.5, 0, image.shape[1]-1)
    y = np.clip(y-.5, 0, image.shape[0]-1)
    ix, iy = x.astype(int), y.astype(int)
    jx, jy = np.minimum(ix+1, image.shape[1]-1), np.minimum(iy+1, image.shape[0]-1)
    fx, fy = (x-ix)[..., None], (y-iy)[..., None]
    return ((image[iy, ix]*(1-fx)+image[iy, jx]*fx)*(1-fy)
            +(image[jy, ix]*(1-fx)+image[jy, jx]*fx)*fy)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--sample', type=Path, required=True)
    parser.add_argument('--cell', type=int, required=True)
    parser.add_argument('--rect', type=float, nargs=4, required=True)
    args = parser.parse_args()
    source, sample = rgba(args.source), rgba(args.sample)
    actual = sample[:8, args.cell*8:(args.cell+1)*8]
    if args.cell < 0 or actual.shape != (8,8,4):
        raise ValueError('Invalid 8x8 cell')
    x0,y0,x1,y1 = args.rect
    x,y = np.meshgrid(x0+(np.arange(8)+.5)*(x1-x0)/8,
                      y0+(np.arange(8)+.5)*(y1-y0)/8)
    print('actual RGBA mean:', actual.mean(axis=(0,1)).round(3).tolist())
    for scale in (.5,1,2):
        expected = bilinear(source,x*scale,y*scale)
        print('coordinate scale',scale,'RGBA mean:',expected.mean(axis=(0,1)).round(3).tolist(),
              'RGBA MAE:',np.abs(actual-expected).mean(axis=(0,1)).round(3).tolist())


if __name__ == '__main__':
    main()
