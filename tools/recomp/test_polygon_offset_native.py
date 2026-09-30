"""Generate production combiner shaders and check biased depth on D3D11 WARP."""
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
#include <string.h>
#include <math.h>
#include "d3d8_combiners.h"
#include "d3d8_triangle_depth.h"
/* The fixture observes rendering; compiler diagnostics have no host log sink. */
void xbox_preview_log_event(const char *category,const char *format,...) {
 (void)category;(void)format;
}
'''+generator+r'''
#undef EMIT
static ID3DBlob *compile(const char *src,const char *profile){
 ID3DBlob *code=NULL,*errors=NULL;
 HRESULT hr=D3DCompile(src,strlen(src),"polygon_offset_test",NULL,NULL,"main",profile,0,0,&code,&errors);
 if(FAILED(hr)&&errors)fprintf(stderr,"%s\n",(char*)ID3D10Blob_GetBufferPointer(errors));
 if(errors)ID3D10Blob_Release(errors);assert(SUCCEEDED(hr));return code;
}
int main(void){
 ID3D11Device *device=NULL;ID3D11DeviceContext *ctx=NULL;
 ID3D11VertexShader *vs[2];ID3D11PixelShader *ps[6];ID3D11Buffer *cb=NULL;
 ID3D11DepthStencilState *ds=NULL;ID3D11RasterizerState *rs=NULL;
 D3D11_BUFFER_DESC bd={0};D3D11_DEPTH_STENCIL_DESC dd={0};D3D11_RASTERIZER_DESC rd={0};
 NV2ACombinerState state={0};NV2APSConstants constants={0};char hlsl[16384];
 assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&device,NULL,&ctx)));
 for(unsigned mode=0;mode<6;mode++){
  state.polygon_offset=mode&1;state.guest_depth=mode>>1;assert(d3d8_combiners_generate_hlsl(&state,hlsl,sizeof(hlsl))>0);
  assert((strstr(hlsl,"SV_Depth")!=NULL)==(mode!=0));
  ID3DBlob *b=compile(hlsl,"ps_5_0");
  assert(SUCCEEDED(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ps[mode])));ID3D10Blob_Release(b);
 }
 for(unsigned axis=0;axis<2;axis++){
  snprintf(hlsl,sizeof(hlsl),
   "struct V{float4 pos:SV_POSITION;float4 c0:COLOR0;float4 c1:COLOR1;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float f:FOG;float size:PSIZE;float4 b0:TEXCOORD4;float4 b1:TEXCOORD5;noperspective float4 guestDepth:TEXCOORD6;};"
   "V main(uint id:SV_VertexID){V o=(V)0;float2 p=id==0?float2(-1,-1):(id==1?float2(-1,3):float2(3,-1));float z=0.25+0.125*p.%s;float w=id==0?0.75:(id==1?17.0:187.5);o.guestDepth=float4(z*16777215.0-16777215.0,16777215.0,(p.x+1)*32,(1-p.y)*32);o.pos=float4(p*w,z*w,w);return o;}",axis?"y":"x");
  ID3DBlob *b=compile(hlsl,"vs_5_0");assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&vs[axis])));ID3D10Blob_Release(b);
 }
 bd.ByteWidth=sizeof(constants);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
 assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&bd,NULL,&cb)));
 dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;
 assert(SUCCEEDED(ID3D11Device_CreateDepthStencilState(device,&dd,&ds)));
 rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
 assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device,&rd,&rs)));
 ID3D11DeviceContext_RSSetState(ctx,rs);ID3D11DeviceContext_OMSetDepthStencilState(ctx,ds,0);
 ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 ID3D11DeviceContext_PSSetConstantBuffers(ctx,0,1,&cb);
 for(unsigned scale=1;scale<=3;scale++)for(unsigned aa=1;aa<=2;aa++){
  unsigned w=64*scale*aa,h=64*scale;
  ID3D11Texture2D *target=NULL,*readback=NULL;ID3D11DepthStencilView *dsv=NULL;
  D3D11_TEXTURE2D_DESC td={0};D3D11_VIEWPORT vp={0};D3D11_MAPPED_SUBRESOURCE map;
  td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_D32_FLOAT;
  td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&target)));
  assert(SUCCEEDED(ID3D11Device_CreateDepthStencilView(device,(ID3D11Resource*)target,NULL,&dsv)));
  td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&readback)));
  vp.Width=(float)w;vp.Height=(float)h;vp.MaxDepth=1;
  ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,dsv);
  for(unsigned axis=0;axis<2;axis++)for(unsigned enabled=0;enabled<6;enabled++)for(int sign=-1;sign<=1;sign+=2){
   constants.polygon_offset[0]=sign*0.01f;constants.polygon_offset[1]=sign*1.0f;
   constants.polygon_offset[2]=(float)w/64;constants.polygon_offset[3]=(float)h/64;
   ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)cb,0,NULL,&constants,0,0);
   ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH,1,0);
   ID3D11DeviceContext_VSSetShader(ctx,vs[axis],NULL,0);ID3D11DeviceContext_PSSetShader(ctx,ps[enabled],NULL,0);
   D3D8TriangleDepthScope scope={0};
   if(enabled>=4){d3d8_triangle_depth_begin(device,ctx,64,64,&scope);assert(scope.active);}
   ID3D11DeviceContext_Draw(ctx,3,0);
   d3d8_triangle_depth_end(ctx,&scope);
   ID3D11GeometryShader *restored=NULL;ID3D11DeviceContext_GSGetShader(ctx,&restored,NULL,NULL);assert(restored==NULL);
   ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)readback,(ID3D11Resource*)target);
   assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&map)));
   float actual=((float*)((unsigned char*)map.pData+(h/2)*map.RowPitch))[w/2];
   float expected=0.25f+(axis?-0.125f/h:0.125f/w)+((enabled&1)?sign*(0.01f+0.25f/64):0);
   if(fabsf(actual-expected)>0.000002f)fprintf(stderr,"scale=%u aa=%u axis=%u enabled=%u sign=%d actual=%g expected=%g\n",scale,aa,axis,enabled,sign,actual,expected);
   assert(fabsf(actual-expected)<0.000002f);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)readback,0);
  }
  ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
  ID3D11DepthStencilView_Release(dsv);ID3D11Texture2D_Release(target);ID3D11Texture2D_Release(readback);
 }
 d3d8_triangle_depth_shutdown();
 ID3D11DeviceContext_ClearState(ctx);for(unsigned i=0;i<2;i++){ID3D11VertexShader_Release(vs[i]);}for(unsigned i=0;i<6;i++){ID3D11PixelShader_Release(ps[i]);}
 ID3D11Buffer_Release(cb);ID3D11DepthStencilState_Release(ds);ID3D11RasterizerState_Release(rs);ID3D11DeviceContext_Release(ctx);ID3D11Device_Release(device);
 puts("PASS: production HLSL compiles; signed depth bias and slope at 1x/2x/3x resolution, anisotropic 2x AA, both axes, offset disabled, guest-depth interpolation with unequal W");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nv2a-polygon-offset-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture,encoding='utf-8')
 (p/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(polygon_offset_test C)
set(CMAKE_C_STANDARD 11)
add_executable(polygon_offset_test test.c "{(ROOT/'src/d3d/d3d8_triangle_depth.c').as_posix()}" "{(ROOT/'src/d3d/d3d8_compiler.c').as_posix()}")
target_include_directories(polygon_offset_test PRIVATE "{(ROOT/'src/d3d').as_posix()}" "{(ROOT/'src').as_posix()}")
target_link_libraries(polygon_offset_test PRIVATE d3d11 d3dcompiler dxguid)
''')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 def run(args):
  r=subprocess.run(args,capture_output=True,text=True)
  if r.returncode:print(r.stdout);print(r.stderr)
  r.check_returncode();return r.stdout
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 print(run([str(p/'build/Release/polygon_offset_test.exe')]))
