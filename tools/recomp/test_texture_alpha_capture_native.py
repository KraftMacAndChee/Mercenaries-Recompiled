"""Verify the production BMP writer's RGB/alpha views, pitch and row order."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class AlphaCaptureTests(unittest.TestCase):
    def test_rgb_unchanged_and_alpha_grayscale(self):
        source=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
        start=source.index('static void d3d8_write_backbuffer_bmp(')
        code=source[start:source.index('\n}\n',start)+3]
        prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
typedef struct { uint32_t Width,Height; } D3D11_TEXTURE2D_DESC;
typedef struct { void *pData; uint32_t RowPitch; } D3D11_MAPPED_SUBRESOURCE;
'''
        harness=r'''
int main(int argc,char **argv) {
    uint8_t pixels[24]={1,2,3,4,5,6,7,8,99,99,99,99,
                        9,10,11,12,13,14,15,16,99,99,99,99};
    D3D11_TEXTURE2D_DESC d={2,2};
    D3D11_MAPPED_SUBRESOURCE m={pixels,12};
    if(argc!=3) return 1;
    d3d8_write_backbuffer_bmp(argv[1],&d,&m,0);
    d3d8_write_backbuffer_bmp(argv[2],&d,&m,1);
    return 0;
}
'''
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-alpha-') as temp:
            directory=Path(temp); c=directory/'capture.c'; exe=directory/'capture.exe'
            c.write_text(prelude+code+harness,encoding='utf-8')
            result=subprocess.run([compiler,str(c),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            rgb=directory/'rgb.bmp'; alpha=directory/'alpha.bmp'
            subprocess.run([str(exe),str(rgb),str(alpha)],check=True,capture_output=True)
            normal=rgb.read_bytes(); mask=alpha.read_bytes()
            self.assertEqual(normal[:54],mask[:54])
            self.assertEqual(struct.unpack_from('<I',mask,2)[0],70)
            self.assertEqual(normal[54:],bytes([11,10,9,15,14,13,0,0,3,2,1,7,6,5,0,0]))
            self.assertEqual(mask[54:],bytes([12,12,12,16,16,16,0,0,4,4,4,8,8,8,0,0]))


if __name__=='__main__':
    unittest.main()
