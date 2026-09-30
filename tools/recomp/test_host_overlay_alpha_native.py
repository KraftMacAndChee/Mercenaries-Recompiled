"""Render production host/video compositors on D3D11 WARP and read pixels."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
block=s[s.index('typedef struct D3D8PvideoConstants'):s.index('BOOL d3d8_CopyTextureToRenderTarget(')]
fixture=r"""
#define COBJMACROS
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#define d3d8_compile_shader D3DCompile
static unsigned g_presentation_aspect_width=16,g_presentation_aspect_height=9;
static struct {
 ID3D11Device *d3d11_device;ID3D11DeviceContext *d3d11_context;
 void *swap_chain;unsigned width,height;
 ID3D11RenderTargetView *default_rtv,*current_rtv;ID3D11DepthStencilView *current_dsv;
 ID3D11VertexShader *resolve_vs;ID3D11SamplerState *resolve_sampler;
 ID3D11PixelShader *pvideo_ps;ID3D11Buffer *pvideo_cb;
 ID3D11Texture2D *pvideo_texture;ID3D11ShaderResourceView *pvideo_srv;
 unsigned pvideo_width,pvideo_height;ID3D11BlendState *host_overlay_blend;
} g_device_state;
static BOOL d3d8_ensure_resolve_pipeline(void){return TRUE;}
"""+block+r"""
int main(void){
 ID3D11Device *dev;ID3D11DeviceContext *ctx;
 assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&dev,NULL,&ctx)));
 g_device_state.d3d11_device=dev;g_device_state.d3d11_context=ctx;g_device_state.swap_chain=(void*)1;
 const char*vs="struct O{float4 p:SV_POSITION;float2 uv:TEXCOORD0;}; O main(uint i:SV_VertexID){O o;o.uv=float2((i<<1)&2,i&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}";
 ID3DBlob *blob,*err=NULL;assert(SUCCEEDED(D3DCompile(vs,strlen(vs),NULL,NULL,NULL,"main","vs_5_0",0,0,&blob,&err)));
 assert(SUCCEEDED(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(blob),ID3D10Blob_GetBufferSize(blob),NULL,&g_device_state.resolve_vs)));ID3D10Blob_Release(blob);
 D3D11_SAMPLER_DESC sampler={0};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
 assert(SUCCEEDED(ID3D11Device_CreateSamplerState(dev,&sampler,&g_device_state.resolve_sampler)));
 for(unsigned scale=1;scale<=2;++scale){
  unsigned w=320*scale,h=180*scale;g_device_state.width=w;g_device_state.height=h;
  D3D11_TEXTURE2D_DESC td={0};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *target,*read;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&td,NULL,&target)));
  assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)target,NULL,&g_device_state.default_rtv)));g_device_state.current_rtv=g_device_state.default_rtv;
  td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.BindFlags=0;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&td,NULL,&read)));
  uint32_t pixels[]={0,0xffffffff,0x80808080,0xff000000};FLOAT clear[]={.2f,.4f,.6f,1};
  for(unsigned video=0;video<2;++video){
   ID3D11DeviceContext_ClearRenderTargetView(ctx,g_device_state.default_rtv,clear);
   if(video)assert(d3d8_CompositeVideoOverlay(pixels,2,2,8,0,0,1,1,1,1,2,2,4,4,NULL,0,0,FALSE,0));
   else assert(d3d8_CompositeHostOverlay(pixels,2,2,1,1,4,4));
   ID3D11BlendState *restored=NULL;ID3D11DeviceContext_OMGetBlendState(ctx,&restored,NULL,NULL);assert(!restored);
   ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)read,(ID3D11Resource*)target);
   D3D11_MAPPED_SUBRESOURCE m;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)read,0,D3D11_MAP_READ,0,&m)));
   for(unsigned i=0;i<4;++i){
    unsigned x=w*(3+(i%2)*2)/8,y=h*(3+(i/2)*2)/8;unsigned char*q=(unsigned char*)m.pData+y*m.RowPitch+x*4;
    for(unsigned c=0;c<3;++c){int expected=i==0?(video?0:51*(c+1)):i==1?255:i==2?(video?128:128+(int)(51*(c+1)*127/255.)):0;assert(abs((int)q[c]-expected)<=1);}
   }
   unsigned char*q=m.pData;assert(q[0]==51 && q[1]==102 && q[2]==153);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)read,0);
  }
  ID3D11Texture2D_Release(read);ID3D11RenderTargetView_Release(g_device_state.default_rtv);ID3D11Texture2D_Release(target);
 }
 puts("PASS: transparent/opaque/partial alpha pixels, original video opacity, two display sizes, restored blend state");
}
"""
with tempfile.TemporaryDirectory(prefix='host-overlay-alpha-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture)
 (p/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(overlay_test C)\nadd_executable(overlay_test test.c)\ntarget_link_libraries(overlay_test PRIVATE d3d11 d3dcompiler dxguid)\n')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 def run(args):
  r=subprocess.run(args,capture_output=True,text=True)
  if r.returncode:print(r.stdout);print(r.stderr)
  r.check_returncode();return r.stdout
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 print(run([str(p/'build/Release/overlay_test.exe')]))
