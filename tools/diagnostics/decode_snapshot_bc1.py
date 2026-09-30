"""Decode a bounded BC1 texture from an offline guest snapshot for inspection.

This does not read or modify a running process or any game asset.
"""
import argparse
from pathlib import Path
import struct
import numpy as np


def decode_bc1(data, width, height):
    if width <= 0 or height <= 0 or width > 8192 or height > 8192:
        raise ValueError('Invalid texture dimensions')
    bw, bh = (width + 3)//4, (height + 3)//4
    if len(data) != bw*bh*8:
        raise ValueError('Incorrect BC1 byte count')
    blocks = np.frombuffer(data, dtype=np.dtype([('c0','<u2'),('c1','<u2'),('idx','<u4')]))
    palette = np.zeros((len(blocks),4,4), dtype=np.uint16)
    for i in range(2):
        color=blocks['c'+str(i)].astype(np.uint16)
        r, g, b = color>>11, (color>>5)&63, color&31
        palette[:,i,0]=(r<<3)|(r>>2)
        palette[:,i,1]=(g<<2)|(g>>4)
        palette[:,i,2]=(b<<3)|(b>>2)
        palette[:,i,3]=255
    opaque=blocks['c0']>blocks['c1']
    palette[:,2]=(2*palette[:,0]+palette[:,1])//3
    palette[:,3]=(palette[:,0]+2*palette[:,1])//3
    palette[~opaque,2]=(palette[~opaque,0]+palette[~opaque,1])//2
    palette[~opaque,3]=0
    indices=(blocks['idx'][:,None]>>(np.arange(16,dtype=np.uint32)*2))&3
    pixels=palette[np.arange(len(blocks))[:,None],indices].astype(np.uint8)
    return pixels.reshape(bh,bw,4,4,4).transpose(0,2,1,3,4).reshape(bh*4,bw*4,4)[:height,:width]


def write_bmp(path, rgba):
    height,width,_=rgba.shape
    pixels=rgba[:,:, [2,1,0,3]].copy().tobytes()
    header=struct.pack('<2sIHHI',b'BM',54+len(pixels),0,0,54)
    header+=struct.pack('<IiiHHIIiiII',40,width,-height,1,32,0,len(pixels),0,0,0,0)
    with path.open('xb') as stream:
        stream.write(header+pixels)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot',type=Path)
    parser.add_argument('offset',type=lambda s:int(s,0))
    parser.add_argument('width',type=int)
    parser.add_argument('height',type=int)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    size=((args.width+3)//4)*((args.height+3)//4)*8
    if args.offset<0 or size<=0 or args.offset+size>args.snapshot.stat().st_size:
        parser.error('Texture lies outside snapshot')
    with args.snapshot.open('rb') as stream:
        stream.seek(args.offset);data=stream.read(size)
    rgba=decode_bc1(data,args.width,args.height)
    write_bmp(args.output,rgba)
    print(f'{args.output}: {args.width}x{args.height}, {size} BC1 bytes at {args.offset:#x}')


if __name__=='__main__':main()
