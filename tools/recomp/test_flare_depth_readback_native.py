"""Execute the bounded depth-readback diagnostic with a padded GPU fixture.

Mocks readback transport only. Real D3D11 integration still requires a live run.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DepthReadbackTests(unittest.TestCase):
    def test_bounds_format_and_read_only_statistics(self):
        source = (ROOT / 'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        function = re.search(r'static void debug_trace_flare_probe_depth\(.*?\n\}', source, re.S)[0]
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
typedef long HRESULT;
typedef struct { int unused; } ID3D11Device, ID3D11DeviceContext, ID3D11Resource, ID3D11Texture2D;
typedef struct {
 uint32_t Width, Height, MipLevels, ArraySize, Format;
 struct {uint32_t Count,Quality;} SampleDesc;
 uint32_t Usage,BindFlags,CPUAccessFlags,MiscFlags;
} D3D11_TEXTURE2D_DESC;
typedef struct {void *pData;uint32_t RowPitch;} D3D11_MAPPED_SUBRESOURCE;
typedef struct {ID3D11Texture2D *texture;uint32_t offset,format;} GuestDepthSurface;
#define MAX_PATH 260
static const char *pgraph_cached_getenv(const char *name){assert(!strcmp(name,"MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX"));return "probe";}
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
#define NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 2
#define DXGI_FORMAT_D24_UNORM_S8_UINT 45
#define DXGI_FORMAT_R24G8_TYPELESS 44
#define D3D11_USAGE_STAGING 3
#define D3D11_CPU_ACCESS_READ 0x20000
#define D3D11_MAP_READ 1
static ID3D11Device device;
static ID3D11DeviceContext context;
static ID3D11Texture2D original, staging;
static GuestDepthSurface depth = {&original,0x1c5a000,2};
static struct {uint32_t logical_width,logical_height;} color={8,8};
static struct {GuestDepthSurface *bound_depth;__typeof__(color) *bound_color;int depth_test,depth_write;
 uint32_t depth_func;float immediate_vertices[4][1][4];} g_pg;
static D3D11_TEXTURE2D_DESC description;
static uint32_t pixels[8][10], before[8][10];
static int mode, creates,copies,maps,unmaps,releases;
static ID3D11Device *d3d8_GetD3D11Device(void){return &device;}
static ID3D11DeviceContext *d3d8_GetD3D11Context(void){return &context;}
static void ID3D11Texture2D_GetDesc(ID3D11Texture2D *p,D3D11_TEXTURE2D_DESC *out){assert(p==&original);*out=description;}
static HRESULT ID3D11Device_CreateTexture2D(ID3D11Device *d,const D3D11_TEXTURE2D_DESC *desc,void *data,ID3D11Texture2D **out){
 assert(d==&device && !data && desc->Usage==3 && !desc->BindFlags && desc->CPUAccessFlags==0x20000);
 ++creates; if(mode==9)return -1;*out=&staging;return 0;
}
static void ID3D11DeviceContext_CopyResource(ID3D11DeviceContext *c,ID3D11Resource *dst,ID3D11Resource *src){
 assert(c==&context && dst==(ID3D11Resource*)&staging && src==(ID3D11Resource*)&original);++copies;
}
static HRESULT ID3D11DeviceContext_Map(ID3D11DeviceContext *c,ID3D11Resource *r,unsigned sub,unsigned access,unsigned flags,D3D11_MAPPED_SUBRESOURCE *out){
 assert(c==&context && r==(ID3D11Resource*)&staging && !sub && access==1 && !flags);
 ++maps;if(mode==10)return -1;out->pData=pixels;out->RowPitch=sizeof(pixels[0]);return 0;
}
static void ID3D11DeviceContext_Unmap(ID3D11DeviceContext *c,ID3D11Resource *r,unsigned sub){assert(c==&context && !sub && r==(ID3D11Resource*)&staging);++unmaps;}
static void ID3D11Texture2D_Release(ID3D11Texture2D *p){assert(p==&staging);++releases;}
'''
        harness = r'''
int main(int argc,char **argv){
 assert(argc==2);mode=atoi(argv[1]);
 description.Width=description.Height=8;description.Format=45;description.SampleDesc.Count=1;
 g_pg.bound_depth=&depth;g_pg.bound_color=&color;g_pg.depth_test=1;g_pg.depth_func=4;
 const float xy[4][2]={{1,2},{1,6},{5,6},{5,2}};
 for(int i=0;i<4;++i){g_pg.immediate_vertices[i][0][0]=xy[i][0];g_pg.immediate_vertices[i][0][1]=xy[i][1];g_pg.immediate_vertices[i][0][2]=150;}
 for(int y=0;y<8;++y)for(int x=0;x<10;++x)pixels[y][x]=0xaa000000u+(x%2?200:100);
 memcpy(before,pixels,sizeof(pixels));
 if(mode==1)g_pg.depth_test=0;
 if(mode==2)g_pg.depth_write=1;
 if(mode==3)depth.format=1;
 if(mode==4)description.Width=2049;
 if(mode==5)description.SampleDesc.Count=4;
 if(mode==6)g_pg.immediate_vertices[1][0][2]=151;
 if(mode==7)g_pg.immediate_vertices[1][0][0]=NAN;
 if(mode==8)for(int i=0;i<4;++i)g_pg.immediate_vertices[i][0][0]+=20;
 if(mode==11){g_pg.immediate_vertices[0][0][0]=-4;g_pg.immediate_vertices[1][0][0]=-4;}
 if(mode==12)description.Format=44;
 if(mode==13){description.Format=44;color.logical_width=color.logical_height=4;for(int i=0;i<4;++i){g_pg.immediate_vertices[i][0][0]*=.5f;g_pg.immediate_vertices[i][0][1]*=.5f;}}
 if(mode==14){description.Format=44;color.logical_width=4;}
 debug_trace_flare_probe_depth(7,4);
 assert(!memcmp(before,pixels,sizeof(pixels)));
 if(mode>=1 && mode<=8)assert(!creates && !copies && !maps && !releases);
 else if(mode==9)assert(creates==1 && !copies && !releases);
 else if(mode==10)assert(creates==1 && copies==1 && maps==1 && !unmaps && releases==1);
 else assert(creates==1 && copies==1 && maps==1 && unmaps==1 && releases==1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mercs-flare-depth-') as temp:
            c = Path(temp) / 'test.c'; exe = Path(temp) / 'test.exe'
            for negative in (False, True):
                code = function.replace('row[x] & 0xFFFFFFu','row[x]') if negative else function
                c.write_text(prelude + code + harness, encoding='utf-8')
                build = subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O0',str(c),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                for mode in ([0] if negative else range(15)):
                    csv = Path(temp) / 'probe-07-depth.csv'
                    if csv.exists(): csv.unlink()
                    run = subprocess.run([str(exe),str(mode)],capture_output=True,text=True,timeout=5,cwd=temp)
                    self.assertEqual(run.returncode,0,run.stderr)
                    if mode in range(1,11):
                        self.assertFalse(csv.exists())
                    else:
                        rows=csv.read_text().splitlines()
                        self.assertEqual(rows[0],'x,y,scene_depth,probe_depth')
                        self.assertEqual(len(rows)-1,24 if mode==14 else 20 if mode==11 else 16)
                        if not negative:
                            self.assertTrue(all(r.split(',')[2] in ('100','200') for r in rows[1:]))
                    if negative:
                        self.assertNotIn('scene_z=100..200 lequal=8/16',run.stderr)
                    elif mode in (0,12,13):
                        self.assertIn('rect=1,2,5,6 probe_z=150 scene_z=100..200 lequal=8/16',run.stderr)
                    elif mode==14:
                        self.assertIn('rect=2,2,8,6 probe_z=150 scene_z=100..200 lequal=12/24',run.stderr)
                    elif mode==11:
                        self.assertIn('rect=0,2,5,6 probe_z=150 scene_z=100..200 lequal=8/20',run.stderr)
                    elif mode==10:
                        self.assertIn('readback-failed=',run.stderr)
                    else:
                        self.assertEqual(run.stderr,'')


if __name__ == '__main__':
    unittest.main()
