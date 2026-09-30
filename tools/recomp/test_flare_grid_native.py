"""D3D11 WARP regression: authored 8/4/2 flare visibility remains scale invariant."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/d3d/d3d8_combiners.c').read_text(encoding='utf-8')
generator=s[s.index('#define EMIT(fmt, ...'):s.index('#undef EMIT')]
fixture=r'''
#define COBJMACROS
#undef NDEBUG
#include <d3d11.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "d3d8_combiners.h"
'''+generator+r'''
#undef EMIT
static ID3D11Device *dev;static ID3D11DeviceContext *ctx;static ID3D11VertexShader *vs;static ID3D11PixelShader *ps[4];static ID3D11Buffer *cb,*vc;
typedef struct {ID3D11Texture2D *tex;ID3D11RenderTargetView *rtv;ID3D11ShaderResourceView *srv;unsigned w,h;} Surface;
static ID3DBlob *compile(const char *s,const char *profile){ID3DBlob *b=NULL,*err=NULL;HRESULT hr=D3DCompile(s,strlen(s),"flare_grid",NULL,NULL,"main",profile,0,0,&b,&err);if(FAILED(hr)&&err)puts((char*)ID3D10Blob_GetBufferPointer(err));if(err)ID3D10Blob_Release(err);assert(SUCCEEDED(hr));return b;}
static Surface surface(unsigned w,unsigned h){Surface s={0};D3D11_TEXTURE2D_DESC d={0};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;s.w=w;s.h=h;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&d,NULL,&s.tex)));assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)s.tex,NULL,&s.rtv)));assert(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)s.tex,NULL,&s.srv)));return s;}
static void release(Surface *s){ID3D11ShaderResourceView_Release(s->srv);ID3D11RenderTargetView_Release(s->rtv);ID3D11Texture2D_Release(s->tex);}
static void draw(Surface *dst,Surface *src,unsigned mode,float size,float destY,float uvSize,float uvY){
 float v[12]={size,destY,dst->w==1?1:32,0,0,uvY,uvSize,uvSize,0,0,0,0};
 NV2APSConstants c={0};c.tex_scale[0][0]=c.tex_scale[0][1]=1.f/32;c.flare_grid_ratio[0]=32.f/dst->w;c.flare_grid_ratio[1]=32.f/dst->h;
 ID3D11ShaderResourceView *none=NULL;ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&none);
 ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&dst->rtv,NULL);D3D11_VIEWPORT vp={0};vp.Width=(float)dst->w;vp.Height=(float)dst->h;vp.MaxDepth=1;ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
 const float zero[4]={0};ID3D11DeviceContext_ClearRenderTargetView(ctx,dst->rtv,zero);
 ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)cb,0,NULL,&c,0,0);ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)vc,0,NULL,v,0,0);
 ID3D11DeviceContext_PSSetShader(ctx,ps[mode],NULL,0);ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&src->srv);ID3D11DeviceContext_Draw(ctx,6,0);
}
static unsigned read(Surface *s){ID3D11Texture2D *r=NULL;D3D11_TEXTURE2D_DESC d;D3D11_MAPPED_SUBRESOURCE m;ID3D11Texture2D_GetDesc(s->tex,&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=0;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&d,NULL,&r)));ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)r,(ID3D11Resource*)s->tex);assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)r,0,D3D11_MAP_READ,0,&m)));unsigned a=((unsigned char*)m.pData)[3];ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)r,0);ID3D11Texture2D_Release(r);return a;}
int main(void){
 assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&dev,NULL,&ctx)));
 const char *vsh="cbuffer V:register(b0){float4 outRect;float4 uvRect;float4 unused;} struct O{float4 pos:SV_POSITION;float4 c0:COLOR0;float4 c1:COLOR1;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float f:FOG;}; O main(uint i:SV_VertexID){float2 p[6]={float2(0,0),float2(0,1),float2(1,1),float2(0,0),float2(1,1),float2(1,0)};O o=(O)0;float2 xy=p[i]*outRect.x+float2(0,outRect.y);o.pos=float4(xy.x/outRect.z*2-1,1-xy.y/outRect.z*2,0,1);o.t0=float4(uvRect.xy+p[i]*uvRect.zw,0,1);return o;}";
 ID3DBlob *b=compile(vsh,"vs_5_0");assert(SUCCEEDED(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&vs)));ID3D10Blob_Release(b);
 for(unsigned i=0;i<4;i++){NV2ACombinerState s={0};char h[16384];s.tex_mode[0]=NV2A_TEXMODE_2D;s.final_input[3].reg=NV2A_REG_T0;s.final_input[6].reg=NV2A_REG_T0;s.final_input[6].alpha_rep=1;s.flare_grid=i;assert(d3d8_combiners_generate_hlsl(&s,h,sizeof(h))>0);b=compile(h,"ps_5_0");assert(SUCCEEDED(ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ps[i])));ID3D10Blob_Release(b);}
 D3D11_BUFFER_DESC bd={0};bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.ByteWidth=sizeof(NV2APSConstants);assert(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,NULL,&cb)));bd.ByteWidth=48;assert(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&bd,NULL,&vc)));
 ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&vc);ID3D11DeviceContext_PSSetConstantBuffers(ctx,0,1,&cb);ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_RASTERIZER_DESC rd={0};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ID3D11RasterizerState *rs=NULL;assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(dev,&rd,&rs)));ID3D11DeviceContext_RSSetState(ctx,rs);
 D3D11_SAMPLER_DESC sd={0};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ID3D11SamplerState *sam=NULL;assert(SUCCEEDED(ID3D11Device_CreateSamplerState(dev,&sd,&sam)));ID3D11DeviceContext_PSSetSamplers(ctx,0,1,&sam);
 unsigned native=0,negative=0;const float scales[]={1,1.5,2,3,4.5};
 for(unsigned pattern=0;pattern<3;pattern++)for(unsigned k=0;k<5;k++)for(unsigned aa=1;aa<=2;aa++)for(unsigned fix=0;fix<=1;fix++){
  unsigned w=(unsigned)(32*scales[k]),h=w;Surface source=surface(w*aa,h),a=surface(w,h),b=surface(w,h),result=surface(1,1);
  uint32_t *pixels=malloc(source.w*source.h*4);for(unsigned y=0;y<source.h;y++)for(unsigned x=0;x<source.w;x++){float gx=(x+.5f)/source.w*32,gy=(y+.5f)/source.h*32;unsigned on=pattern==1?0:pattern==2?1:!(gx>=12&&gx<20&&gy>=12&&gy<20);pixels[y*source.w+x]=on?0xffffffff:0;}
  ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)source.tex,0,NULL,pixels,source.w*4,0);free(pixels);
  draw(&a,&source,fix?3:0,8,0,32,0);draw(&b,&a,fix?3:0,4,0,8,0);draw(&a,&b,fix?3:0,2,16,4,0);
  /* Sample center of the final 2x2 visibility tile, as retail Emit does. */
  float v[12]={1,0,1,0,1,17,0,0,0,0,0,0};NV2APSConstants c={0};c.tex_scale[0][0]=c.tex_scale[0][1]=1.f/32;
  ID3D11ShaderResourceView *none=NULL;ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&none);ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&result.rtv,NULL);D3D11_VIEWPORT vp={0};vp.Width=vp.Height=vp.MaxDepth=1;ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)cb,0,NULL,&c,0,0);ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)vc,0,NULL,v,0,0);ID3D11DeviceContext_PSSetShader(ctx,ps[fix],NULL,0);ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&a.srv);ID3D11DeviceContext_Draw(ctx,6,0);
  unsigned value=read(&result);printf("pattern=%u scale=%.1f aa=%u fixed=%u alpha=%u\n",pattern,scales[k],aa,fix,value);
  if(k==0&&aa==1&&!fix){native=value;if(pattern==0)assert(native>200);else assert(native==(pattern==1?0:255));}
  if(fix)assert(abs((int)value-(int)native)<=2);else if(abs((int)value-(int)native)>24)negative++;
  ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&none);ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);release(&source);release(&a);release(&b);release(&result);
 }
 assert(negative>0);puts("PASS: native 8/4/2 mask filtering, fractional scales and horizontal AA; old scaled path fails negative control");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='flare-grid-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture,encoding='utf-8');(p/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(flare_grid C)
set(CMAKE_C_STANDARD 11)
add_executable(flare_grid test.c)
target_include_directories(flare_grid PRIVATE "{(ROOT/'src/d3d').as_posix()}")
target_link_libraries(flare_grid PRIVATE d3d11 d3dcompiler dxguid)
''',encoding='utf-8')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 for args in ([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'],[cmake,'--build',str(p/'build'),'--config','Release'],[str(p/'build/Release/flare_grid.exe')]):
  r=subprocess.run(args,capture_output=True,text=True)
  if r.returncode or len(args)==1:print(r.stdout);print(r.stderr)
  r.check_returncode()
