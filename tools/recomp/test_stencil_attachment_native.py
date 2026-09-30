"""Exercise maintained target selection and real D3D11 stencil-only shadow compositing."""
from pathlib import Path
import subprocess,tempfile,re
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text()
start=s.index('static GuestDepthSurface *prepare_draw_render_targets(')
body=s[start:s.index('\nstatic ID3D11ShaderResourceView *snapshot_bound_color_surface',start)]
clear=re.search(r'prepare_guest_render_targets\(\(flags & \(D3DCLEAR_ZBUFFER \| D3DCLEAR_STENCIL\)\) != 0u\);',s)
assert clear,'Stencil-only clears must request their attachment'
states=(ROOT/'src/d3d/d3d8_states.c').read_text(encoding='utf-8')
setter=states[states.index('void d3d8_states_set_depth_clamp('):]
assert 'rd.DepthClipEnable = g_depth_clip_enable;' in states
fixture=r'''#define COBJMACROS
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3dcompiler.h>
typedef struct {unsigned marker;} GuestDepthSurface;
typedef struct {unsigned depth_alias_source_offset;uint64_t depth_alias_source_serial;} GuestColorSurface;
static struct {unsigned surface_color_offset;int stencil_enable;GuestColorSurface *bound_color;} g_pg;
static GuestColorSurface color;
static GuestDepthSurface depth;
static int request,producer;
static ID3D11Device *device;
static ID3D11DeviceContext *ctx;
static ID3D11RenderTargetView *rtv;
static ID3D11DepthStencilView *dsv;
static GuestColorSurface *find_guest_color_surface(unsigned offset){return &color;}
static GuestDepthSurface *sampled_packed_depth_copy_source(GuestColorSurface *c){return producer?&depth:NULL;}
static void prepare_guest_render_targets(int want){request=want;g_pg.bound_color=&color;if(ctx)ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,want?dsv:NULL);}
''' + body + r'''
static BOOL g_depth_clip_enable=TRUE,g_last_raster_valid=TRUE;
''' + setter + r'''
#define D3DCLEAR_TARGET 1
#define D3DCLEAR_ZBUFFER 2
#define D3DCLEAR_STENCIL 4
static void clear_attachment(unsigned flags){
''' + clear.group() + r'''
}
static ID3DBlob *compile(const char *code,const char *profile){ID3DBlob *b=NULL,*e=NULL;HRESULT hr=D3DCompile(code,strlen(code),NULL,NULL,NULL,"main",profile,0,0,&b,&e);if(FAILED(hr)&&e)fprintf(stderr,"%s",(char*)ID3D10Blob_GetBufferPointer(e));assert(SUCCEEDED(hr));if(e)ID3D10Blob_Release(e);return b;}
int main(void){
 for(int z=0;z<2;z++)for(int st=0;st<2;st++)for(producer=0;producer<2;producer++){
  g_pg.stencil_enable=st;color.depth_alias_source_offset=123;color.depth_alias_source_serial=456;
  assert(prepare_draw_render_targets(z)==(producer?&depth:NULL));assert(request==(z||st));
  assert(color.depth_alias_source_offset==(producer?123:0));assert(color.depth_alias_source_serial==(producer?456:0));
 }
 producer=0;for(unsigned f=0;f<8;f++){clear_attachment(f);assert(request==!!(f&6));}
 D3D_FEATURE_LEVEL feature;assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&device,&feature,&ctx)));
 const char *v="float4 main(uint i:SV_VertexID):SV_Position{float2 p=float2((i<<1)&2,i&2);return float4(p*float2(2,-2)+float2(-1,1),0.5,1);}";
 const char *p="float4 main():SV_Target{return float4(0,0,0,0.5);}";
 ID3DBlob *b=compile(v,"vs_4_0");ID3D11VertexShader *vs;assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&vs)));ID3D10Blob_Release(b);
 b=compile(p,"ps_4_0");ID3D11PixelShader *ps;assert(SUCCEEDED(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ps)));ID3D10Blob_Release(b);
 ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);ID3D11DeviceContext_PSSetShader(ctx,ps,NULL,0);ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_TEXTURE2D_DESC td={0};td.Width=64;td.Height=32;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 ID3D11Texture2D *target,*stage,*zeta;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&target)));assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)target,NULL,&rtv)));
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&stage)));
 td.BindFlags=D3D11_BIND_DEPTH_STENCIL;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&zeta)));assert(SUCCEEDED(ID3D11Device_CreateDepthStencilView(device,(ID3D11Resource*)zeta,NULL,&dsv)));
 D3D11_VIEWPORT vp={0,0,64,32,0,1};ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
 D3D11_RASTERIZER_DESC rd={0};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;ID3D11RasterizerState *rs;assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device,&rd,&rs)));ID3D11DeviceContext_RSSetState(ctx,rs);
 D3D11_DEPTH_STENCIL_DESC ds={0};ds.DepthEnable=FALSE;ds.StencilEnable=TRUE;ds.StencilReadMask=ds.StencilWriteMask=255;ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ds.BackFace=ds.FrontFace;
 ID3D11DepthStencilState *write,*shadow;assert(SUCCEEDED(ID3D11Device_CreateDepthStencilState(device,&ds,&write)));ds.FrontFace.StencilFunc=D3D11_COMPARISON_LESS;ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;ds.BackFace=ds.FrontFace;assert(SUCCEEDED(ID3D11Device_CreateDepthStencilState(device,&ds,&shadow)));
 D3D11_BLEND_DESC bd={0};bd.RenderTarget[0].RenderTargetWriteMask=15;bd.RenderTarget[0].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=D3D11_BLEND_ZERO;bd.RenderTarget[0].DestBlend=D3D11_BLEND_SRC_ALPHA;bd.RenderTarget[0].BlendOp=bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ZERO;bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ONE;
 ID3D11BlendState *blend;assert(SUCCEEDED(ID3D11Device_CreateBlendState(device,&bd,&blend)));
 ID3D11VertexShader *far_vs;const char *far_code="float4 main(uint i:SV_VertexID):SV_Position{float2 p=float2((i<<1)&2,i&2);return float4(p*float2(2,-2)+float2(-1,1),2,1);}";
 b=compile(far_code,"vs_4_0");assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&far_vs)));ID3D10Blob_Release(b);
 for(int mode=0;mode<6;mode++){
  d3d8_states_set_depth_clamp(mode==4);assert(g_depth_clip_enable==(mode!=4));
  ID3D11RasterizerState_Release(rs);rd.DepthClipEnable=g_depth_clip_enable;assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device,&rd,&rs)));ID3D11DeviceContext_RSSetState(ctx,rs);
  ID3D11DeviceContext_VSSetShader(ctx,mode>=3?far_vs:vs,NULL,0);
  ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
  g_pg.stencil_enable=1;prepare_draw_render_targets(0);assert(request);
  D3D11_RECT left={0,0,32,32},all={0,0,64,32};ID3D11DeviceContext_RSSetScissorRects(ctx,1,&left);ID3D11DeviceContext_OMSetDepthStencilState(ctx,write,1);ID3D11DeviceContext_Draw(ctx,3,0);
  float white[4]={1,1,1,1};ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,white);
  if(mode==0)prepare_guest_render_targets(0); /* Old buggy binding: negative control. */
  else prepare_draw_render_targets(0);
  if(mode==2){clear_attachment(D3DCLEAR_STENCIL);assert(request);ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_STENCIL,1,0);}
  ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);ID3D11DeviceContext_RSSetScissorRects(ctx,1,&all);ID3D11DeviceContext_OMSetDepthStencilState(ctx,shadow,0);ID3D11DeviceContext_OMSetBlendState(ctx,blend,NULL,~0u);ID3D11DeviceContext_Draw(ctx,3,0);
  ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)stage,(ID3D11Resource*)target);D3D11_MAPPED_SUBRESOURCE map;assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)stage,0,D3D11_MAP_READ,0,&map)));
  for(unsigned y=0;y<32;y++)for(unsigned x=0;x<64;x++){unsigned char *pixel=(unsigned char*)map.pData+y*map.RowPitch+x*4;int expected=(mode==0||((mode==1||mode==4)&&x<32))?128:255;for(unsigned c=0;c<3;c++)assert(pixel[c]==expected);}
  ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)stage,0);
 }
 ID3D11DeviceContext_ClearState(ctx);ID3D11BlendState_Release(blend);ID3D11DepthStencilState_Release(write);ID3D11DepthStencilState_Release(shadow);ID3D11RasterizerState_Release(rs);ID3D11VertexShader_Release(far_vs);ID3D11VertexShader_Release(vs);ID3D11PixelShader_Release(ps);ID3D11RenderTargetView_Release(rtv);ID3D11DepthStencilView_Release(dsv);ID3D11Texture2D_Release(target);ID3D11Texture2D_Release(stage);ID3D11Texture2D_Release(zeta);ID3D11DeviceContext_Release(ctx);ID3D11Device_Release(device);
 puts("PASS: four depth/stencil combinations, alias lifetime, eight clear flags; D3D11 negative control darkens all pixels, fixed binding darkens only masked half, stencil-only clear removes all shadows; far caps survive requested clamp and clipping restores afterward");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mercs-stencil-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture)
 (p/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(stencil_test C)\nadd_executable(stencil_test test.c)\ntarget_link_libraries(stencil_test PRIVATE d3d11 d3dcompiler dxguid)\n')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 def run(args):
  r=subprocess.run(args,capture_output=True,text=True)
  if r.returncode:print(r.stdout);print(r.stderr)
  r.check_returncode();return r.stdout
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 print(run([str(p/'build/Release/stencil_test.exe')]))
