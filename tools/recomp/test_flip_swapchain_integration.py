"""Run production swap-chain creation/resize/Present against the Windows D3D11 runtime.

Uses a hidden private test window. Does not change the user's display mode.
Requires an MSVC developer command prompt (cl, Windows SDK, dxguid.lib).
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
typedef struct {UINT BackBufferCount,BackBufferWidth,BackBufferHeight;HWND hDeviceWindow;BOOL Windowed;} D3DPRESENT_PARAMETERS;
typedef struct {
 ID3D11Device *d3d11_device;ID3D11DeviceContext *d3d11_context;IDXGISwapChain *swap_chain;
 ID3D11RenderTargetView *default_rtv,*current_rtv;ID3D11DepthStencilView *default_dsv,*current_dsv;
 ID3D11Texture2D *default_depth;BOOL flip_model;UINT swap_chain_flags;HWND hwnd;
 UINT width,height,current_target_width,current_target_height,current_target_physical_width,current_target_physical_height;
} D3D8DeviceState;
static void (*g_host_overlay_callback)(void);
static D3D8DeviceState g_device_state;static BOOL g_pending_scanout_present;static unsigned submissions,errors,default_creates,explicit_failures;
static HRESULT test_create(IDXGIAdapter *adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,UINT version,const DXGI_SWAP_CHAIN_DESC *desc,IDXGISwapChain **chain,ID3D11Device **device,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context){
 assert(type==(adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE));
 if(desc->SwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD&&getenv("TEST_FAIL_FLIP_CREATION"))return E_INVALIDARG;
 if(!adapter)++default_creates;
 HRESULT hr=D3D11CreateDeviceAndSwapChain(adapter,type,software,flags,levels,count,version,desc,chain,device,level,context);
 /* Fail after allocating real objects to exercise cleanup before a retry. */
 if(adapter && getenv("TEST_FAIL_EXPLICIT_ADAPTER")){++explicit_failures;return E_FAIL;}
 return hr;
}
#define D3D11CreateDeviceAndSwapChain test_create
static const char *d3d8_cached_getenv(const char *n){return getenv(n);}
static void xbox_preview_log_event(const char *cat,const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);puts("");va_end(a);if(!strcmp(cat,"graphics-error"))++errors;}
static void record_frame_submission(void){++submissions;}
static void d3d8_BindRenderTargets(void *a,void *b,UINT w,UINT h){
 (void)a;(void)b;(void)w;(void)h;
 g_device_state.current_rtv=g_device_state.default_rtv;g_device_state.current_dsv=g_device_state.default_dsv;
 ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,1,&g_device_state.current_rtv,g_device_state.current_dsv);
}
'''
POST=r'''
static void check_frame(unsigned channel){
 D3D8DeviceState *s=&g_device_state;FLOAT color[4]={0,0,0,1};color[channel]=1;
 ID3D11Texture2D *back=NULL,*staging=NULL;D3D11_TEXTURE2D_DESC d;D3D11_MAPPED_SUBRESOURCE map;
 ID3D11DeviceContext_ClearRenderTargetView(s->d3d11_context,s->default_rtv,color);
 assert(SUCCEEDED(IDXGISwapChain_GetBuffer(s->swap_chain,0,&IID_ID3D11Texture2D,(void**)&back)));
 ID3D11Texture2D_GetDesc(back,&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
 assert(SUCCEEDED(ID3D11Device_CreateTexture2D(s->d3d11_device,&d,NULL,&staging)));
 ID3D11DeviceContext_CopyResource(s->d3d11_context,(ID3D11Resource*)staging,(ID3D11Resource*)back);
 assert(SUCCEEDED(ID3D11DeviceContext_Map(s->d3d11_context,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&map)));
 for(UINT y=0;y<d.Height;y+=d.Height-1)for(UINT x=0;x<d.Width;x+=d.Width-1){
  const BYTE *pixel=(const BYTE*)map.pData+y*map.RowPitch+x*4;for(UINT c=0;c<3;c++)assert(pixel[c]==(c==channel?255:0));
 }
 ID3D11DeviceContext_Unmap(s->d3d11_context,(ID3D11Resource*)staging,0);
 ID3D11Texture2D_Release(staging);ID3D11Texture2D_Release(back);
 assert(SUCCEEDED(preview_present(s->swap_chain,0,0)));
 ID3D11RenderTargetView *rt=NULL;ID3D11DepthStencilView *ds=NULL;
 ID3D11DeviceContext_OMGetRenderTargets(s->d3d11_context,1,&rt,&ds);
 assert(rt==s->current_rtv && ds==s->current_dsv);if(rt)ID3D11RenderTargetView_Release(rt);if(ds)ID3D11DepthStencilView_Release(ds);
}
int main(void){
 WNDCLASSA wc={0};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="MercsFlipIntegration";
 assert(RegisterClassA(&wc));HWND w=CreateWindowA(wc.lpszClassName,"Private presentation test",WS_OVERLAPPEDWINDOW,0,0,640,480,NULL,NULL,wc.hInstance,NULL);assert(w);
 D3DPRESENT_PARAMETERS pp={1,640,480,w,TRUE};D3D8DeviceState *s=&g_device_state;
 assert(SUCCEEDED(d3d11_create_device_and_swap_chain(s,&pp)));assert(SUCCEEDED(d3d11_create_render_targets(s)));
 if(getenv("TEST_FAIL_FLIP_CREATION")||getenv("MERCENARIES_DISABLE_FLIP_PRESENT"))assert(!s->flip_model);
 for(unsigned i=0;i<3;i++)check_frame(i);
 for(unsigned i=0;i<3;i++){
  UINT width=i==0?1280:i==1?1920:640,height=i==0?720:i==1?1080:480;
  assert(d3d8_ResizePresentation(width,height,FALSE));DXGI_SWAP_CHAIN_DESC desc;
  assert(SUCCEEDED(IDXGISwapChain_GetDesc(s->swap_chain,&desc)));
  assert(desc.BufferCount==(s->flip_model?2u:1u));assert(desc.Flags==s->swap_chain_flags);
  assert(desc.BufferDesc.Width==width&&desc.BufferDesc.Height==height);
  for(unsigned j=0;j<3;j++)check_frame(j);
 }
 if(getenv("TEST_FAIL_EXPLICIT_ADAPTER"))assert(default_creates==1 && explicit_failures>0);
 assert(submissions==12&&errors==0);puts("PASS: production creation, pixel contents, post-Present targets and repeated buffer resizing");
 ID3D11DeviceContext_ClearState(s->d3d11_context);ID3D11RenderTargetView_Release(s->default_rtv);ID3D11DepthStencilView_Release(s->default_dsv);ID3D11Texture2D_Release(s->default_depth);
 IDXGISwapChain_Release(s->swap_chain);ID3D11DeviceContext_Release(s->d3d11_context);ID3D11Device_Release(s->d3d11_device);DestroyWindow(w);return 0;
}
'''
def function(s,start,end):return s[s.index(start):s.index(end,s.index(start))]
def main():
 s=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
 block=function(s,'static HRESULT preview_present(', '\n\nstatic IDirect3DDevice8 g_device;')
 block+=function(s,'static BOOL d3d8_flip_presentation_requested(', 'static void d3d8_init_default_states(')
 with tempfile.TemporaryDirectory(prefix='flip-runtime-') as td:
  p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(PRE+block+POST)
  subprocess.run(['cl','/nologo','/O2','/TC',str(p),'/Fo'+str(p.with_suffix('.obj')),'/Fe'+str(exe),'/link','d3d11.lib','dxgi.lib','dxguid.lib','user32.lib'],check=True)
  for mode in ("legacy","flip","fallback","adapter-fallback"):
   e=os.environ.copy();e.pop('MERCENARIES_TEST_FLIP_PRESENT',None);e.pop('MERCENARIES_DISABLE_FLIP_PRESENT',None)
   e.pop('TEST_FAIL_FLIP_CREATION',None);e.pop('TEST_FAIL_EXPLICIT_ADAPTER',None)
   if mode=='legacy':e['MERCENARIES_DISABLE_FLIP_PRESENT']='1'
   if mode=='adapter-fallback':e['TEST_FAIL_EXPLICIT_ADAPTER']='1'
   if mode=='fallback':e['TEST_FAIL_FLIP_CREATION']='1'
   subprocess.run([str(exe)],env=e,check=True,timeout=30)
if __name__=='__main__':main()
