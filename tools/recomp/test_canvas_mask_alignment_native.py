"""Verify retail mask-copy selection and scaled D3D11 raster coverage."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
start=s.index('    if (hash == 0x1730DD1Au && g_pg.blend_enable &&')
block=s[start:s.index('    d3d8_vsh_set_constant(0, constant_data, NV2A_VERTEXSHADER_CONSTANTS);',start)]
fixture=r"""#define COBJMACROS
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#define NV2A_VERTEXSHADER_CONSTANTS 192
typedef struct {unsigned width,height,logical_width,logical_height;int gpu_drawn;unsigned depth_alias_source_offset;} GuestColorSurface;
static GuestColorSurface target,sample;
static int missing;
static struct {int blend_enable;unsigned blend_sfactor,blend_dfactor;GuestColorSurface *bound_color;uint32_t vsh_constants[192][4];} g_pg;
static unsigned resolved_texture_offset(unsigned i){return 123;}
static GuestColorSurface *find_guest_color_surface(unsigned o){return missing?NULL:&sample;}
static float bias(unsigned hash){uint32_t constant_snapshot[192][4];const float *constant_data=(const float*)g_pg.vsh_constants;
"""+block+r"""
float v;memcpy(&v,&constant_data[28*4],4);assert(g_pg.vsh_constants[28][0]==0x3F080000);return v;}
static ID3DBlob *compile(const char *code,const char *profile){ID3DBlob *b=NULL,*e=NULL;HRESULT hr=D3DCompile(code,strlen(code),NULL,NULL,NULL,"main",profile,0,0,&b,&e);if(FAILED(hr)&&e)puts((char*)ID3D10Blob_GetBufferPointer(e));assert(SUCCEEDED(hr));if(e)ID3D10Blob_Release(e);return b;}
int main(void){
 target=(GuestColorSurface){1440,1080,640,480,1,0};sample=target;g_pg.bound_color=&target;g_pg.blend_enable=1;g_pg.blend_sfactor=0x304;g_pg.blend_dfactor=0x305;g_pg.vsh_constants[28][0]=g_pg.vsh_constants[28][1]=0x3F080000;
 assert(bias(0x1730DD1A)==0.03125f);assert(bias(0)==0.53125f);
 g_pg.blend_sfactor=0x302;assert(bias(0x1730DD1A)==0.53125f);g_pg.blend_sfactor=0x304;
 missing=1;assert(bias(0x1730DD1A)==0.53125f);missing=0;
 sample.depth_alias_source_offset=123;assert(bias(0x1730DD1A)==0.53125f);sample.depth_alias_source_offset=0;
 sample.logical_width=320;assert(bias(0x1730DD1A)==0.53125f);sample.logical_width=640;
 ID3D11Device *dev;ID3D11DeviceContext *ctx;D3D_FEATURE_LEVEL feature;assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&dev,&feature,&ctx)));
 const char *v="cbuffer C:register(b0){float bias;float3 pad;} float4 main(uint i:SV_VertexID):SV_Position{float2 p=float2((i==1||i==2||i==4)?619:522,(i==2||i==4||i==5)?177:49);p=trunc((p+bias)*16)/16;return float4(2*p.x/640-1,1-2*p.y/480,0,1);}";
 const char *p="float4 main():SV_Target{return float4(1,0,0,1);}";
 ID3DBlob *b=compile(v,"vs_4_0");ID3D11VertexShader *vs;assert(SUCCEEDED(ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&vs)));ID3D10Blob_Release(b);
 b=compile(p,"ps_4_0");ID3D11PixelShader *ps;assert(SUCCEEDED(ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ps)));ID3D10Blob_Release(b);
 D3D11_BUFFER_DESC cbd={0};cbd.ByteWidth=16;cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ID3D11Buffer *cb;assert(SUCCEEDED(ID3D11Device_CreateBuffer(dev,&cbd,NULL,&cb)));
 D3D11_BLEND_DESC bd={0};bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=D3D11_BLEND_DEST_ALPHA;bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_DEST_ALPHA;bd.RenderTarget[0].BlendOp=bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;bd.RenderTarget[0].RenderTargetWriteMask=7;ID3D11BlendState *blend;assert(SUCCEEDED(ID3D11Device_CreateBlendState(dev,&bd,&blend)));
 D3D11_RASTERIZER_DESC rd={0};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ID3D11RasterizerState *rs;assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(dev,&rd,&rs)));
 ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);ID3D11DeviceContext_PSSetShader(ctx,ps,NULL,0);ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&cb);ID3D11DeviceContext_RSSetState(ctx,rs);ID3D11DeviceContext_OMSetBlendState(ctx,blend,NULL,~0u);ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 unsigned widths[]={640,960,1440,1920,2880};
 for(unsigned n=0;n<5;n++){
  unsigned w=widths[n],h=w*3/4;target.width=sample.width=w;target.height=sample.height=h;
  unsigned l=522*w/640,t=49*h/480,r=(619*w+639)/640,bt=(177*h+479)/480;
  unsigned char *pixels=malloc((size_t)w*h*4);assert(pixels);
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){unsigned char *q=pixels+((size_t)y*w+x)*4;q[0]=q[1]=q[2]=255;q[3]=(x>=l&&x<r&&y>=t&&y<bt)?0:255;if(x>w*55/64&&x<w*56/64&&y>h/5&&y<h/4)q[3]=255;}
  D3D11_TEXTURE2D_DESC td={0};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *tex,*staging;ID3D11RenderTargetView *rtv;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&td,NULL,&tex)));assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)tex,NULL,&rtv)));td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&td,NULL,&staging)));
  D3D11_VIEWPORT vp={0,0,(float)w,(float)h,0,1};ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,NULL);
  for(int fixed=0;fixed<2;fixed++){
   float values[4]={fixed?bias(0x1730DD1A):0.53125f,0,0,0};ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)cb,0,NULL,values,0,0);ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)tex,0,NULL,pixels,w*4,0);ID3D11DeviceContext_Draw(ctx,6,0);ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)staging,(ID3D11Resource*)tex);
   D3D11_MAPPED_SUBRESOURCE m;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&m)));unsigned outside=0,inside=0;
   for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){unsigned char *q=(unsigned char*)m.pData+y*m.RowPitch+x*4;if(q[1]!=255){if(x<l||x>=r||y<t||y>=bt)outside++;else inside++;}}
   ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)staging,0);assert(inside>0);if(fixed||w==640)assert(outside==0);else if(w>=1440)assert(outside>0);printf("%ux%u fixed=%d outside-mask=%u inside-mask=%u\n",w,h,fixed,outside,inside);
  }
  ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);ID3D11RenderTargetView_Release(rtv);ID3D11Texture2D_Release(tex);ID3D11Texture2D_Release(staging);free(pixels);
 }
 ID3D11DeviceContext_ClearState(ctx);ID3D11Buffer_Release(cb);ID3D11RasterizerState_Release(rs);ID3D11BlendState_Release(blend);ID3D11VertexShader_Release(vs);ID3D11PixelShader_Release(ps);ID3D11DeviceContext_Release(ctx);ID3D11Device_Release(dev);puts("PASS: native coverage preserved; scaled composites respect mask bounds; unrelated shaders, blends, packed depth and sizes unchanged.");return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='canvas-mask-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture)
 (p/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(canvas_test C)\nadd_executable(canvas_test test.c)\ntarget_link_libraries(canvas_test PRIVATE d3d11 d3dcompiler dxguid)\n')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 def run(args):
  r=subprocess.run(args,capture_output=True,text=True)
  if r.returncode:print(r.stdout);print(r.stderr)
  r.check_returncode();return r.stdout
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 print(run([str(p/'build/Release/canvas_test.exe')]))
