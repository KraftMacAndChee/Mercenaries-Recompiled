"""Run the production CPU upload on WARP; reject oversized driver reads first."""
import argparse, shutil, subprocess, tempfile
from pathlib import Path


def main():
    root=Path(__file__).resolve().parents[2]
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=root/'src/d3d/d3d8_device.c');a=p.parse_args()
    s=a.source.read_text(encoding='utf-8');start=s.index('void d3d8_UploadFrameX8R8G8B8(')
    body=s[start:s.index('\nstatic void d3d8_write_backbuffer_bmp',start)]
    pre=r'''
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static struct {
 ID3D11Device *d3d11_device; ID3D11DeviceContext *d3d11_context;
 IDXGISwapChain *swap_chain; ID3D11Texture2D *cpu_frame_texture;
 ID3D11ShaderResourceView *cpu_frame_srv;
 ID3D11RenderTargetView *default_rtv; ID3D11DepthStencilView *default_dsv;
 UINT width,height;
} g_device_state;
static UINT source_width,source_height,uploads,copies;
static HRESULT test_get_buffer(IDXGISwapChain *chain,UINT index,REFIID iid,void **out) {
 D3D11_TEXTURE2D_DESC d={0};d.Width=g_device_state.width;d.Height=g_device_state.height;
 d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
 return ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,&d,NULL,(ID3D11Texture2D**)out);
}
#undef IDXGISwapChain_GetBuffer
#define IDXGISwapChain_GetBuffer test_get_buffer
static void checked_update(ID3D11DeviceContext *ctx,ID3D11Resource *resource,UINT sub,
 const D3D11_BOX *box,const void *data,UINT pitch,UINT depth) {
 D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc((ID3D11Texture2D*)resource,&d);
 UINT w=box?box->right-box->left:d.Width,h=box?box->bottom-box->top:d.Height;
 size_t available=(size_t)source_width*source_height*4;
 size_t requested=(size_t)(h-1)*pitch+w*4;
 if(pitch<w*4||requested>available) {
  fprintf(stderr,"Invalid CPU upload: source=%ux%u destination=%ux%u pitch=%u available=%zu requested=%zu\n",
   source_width,source_height,w,h,pitch,available,requested);exit(1);
 }
 CHECK(w==source_width && h==source_height);
 ID3D11DeviceContext_UpdateSubresource(ctx,resource,sub,box,data,pitch,depth);++uploads;
}
#undef ID3D11DeviceContext_UpdateSubresource
#define ID3D11DeviceContext_UpdateSubresource checked_update
static BOOL d3d8_CopyTextureToBackbuffer(ID3D11Texture2D *texture,ID3D11ShaderResourceView *srv,UINT w,UINT h) {
 D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc(texture,&d);
 CHECK(w==source_width&&h==source_height&&d.Width==w&&d.Height==h&&srv);
 d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ID3D11Texture2D *readback=NULL;CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,&d,NULL,&readback)));
 ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,(ID3D11Resource*)readback,(ID3D11Resource*)texture);
 D3D11_MAPPED_SUBRESOURCE m;CHECK(SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&m)));
 for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++){
  BYTE *pixel=(BYTE*)m.pData+(size_t)y*m.RowPitch+x*4;
  CHECK(pixel[0]==0xA5&&pixel[1]==(BYTE)y&&pixel[2]==(BYTE)x&&pixel[3]==255);
 }
 ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,(ID3D11Resource*)readback,0);
 ID3D11Texture2D_Release(readback);++copies;return TRUE;
}
'''
    main=r'''
int main(void) {
 D3D_FEATURE_LEVEL level;
 CHECK(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,
  &g_device_state.d3d11_device,&level,&g_device_state.d3d11_context)));
 g_device_state.swap_chain=(IDXGISwapChain*)1;
 const UINT sizes[][4]={{1920,1080,640,480},{3840,2160,640,480},{640,480,640,480},
  {320,240,640,480},{1920,1080,800,600},{3840,2160,640,480}};
 for(UINT n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++) {
  g_device_state.width=sizes[n][0];g_device_state.height=sizes[n][1];
  source_width=sizes[n][2];source_height=sizes[n][3];UINT pitch=source_width*4+32;
  BYTE *pixels=malloc((size_t)pitch*source_height);CHECK(pixels);
  memset(pixels,0xCD,(size_t)pitch*source_height);
  for(UINT y=0;y<source_height;y++)for(UINT x=0;x<source_width;x++){
   BYTE *p=pixels+(size_t)y*pitch+x*4;p[0]=(BYTE)x;p[1]=(BYTE)y;p[2]=0xA5;p[3]=0;
  }
  d3d8_UploadFrameX8R8G8B8(pixels,pitch,source_width,source_height);
  CHECK(uploads==n+1 && copies==n+1);
  d3d8_UploadFrameX8R8G8B8(pixels,source_width*4-1,source_width,source_height);
  d3d8_UploadFrameX8R8G8B8(pixels,pitch,0,source_height);
  d3d8_UploadFrameX8R8G8B8(pixels,pitch,source_width,0);
  d3d8_UploadFrameX8R8G8B8(pixels,UINT32_MAX,UINT32_MAX,1);
  CHECK(uploads==n+1 && copies==n+1);free(pixels);
 }
 ID3D11ShaderResourceView_Release(g_device_state.cpu_frame_srv);
 ID3D11Texture2D_Release(g_device_state.cpu_frame_texture);
 ID3D11DeviceContext_Release(g_device_state.d3d11_context);
 ID3D11Device_Release(g_device_state.d3d11_device);
 puts("PASS: 6 display/source sizes, GPU readback, padded rows and invalid dimensions");return 0;
}
'''
    compiler=shutil.which('cl')
    if not compiler: raise SystemExit('Run from an MSVC developer environment (D3D11 SDK required).')
    with tempfile.TemporaryDirectory(prefix='mercs-cpu-frame-') as folder:
     cfile=Path(folder)/'test.c';exe=Path(folder)/'test.exe'
     cfile.write_text(pre+body+main,encoding='utf-8')
     subprocess.run([compiler,'/nologo','/O2','/TC',str(cfile),'/Fo'+str(cfile.with_suffix('.obj')),'/Fe'+str(exe),'/link','d3d11.lib','dxguid.lib'],check=True)
     subprocess.run([str(exe)],check=True)


if __name__ == "__main__":
    main()
