"""Render real scaled NV2A lines on WARP and verify width and draw-state restoration."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
fixture=r'''
#define COBJMACROS
#undef NDEBUG
#include <d3d11.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "d3d8_scaled_lines.h"
HRESULT d3d8_compile_shader(LPCVOID src,SIZE_T size,LPCSTR name,const D3D_SHADER_MACRO *defines,ID3DInclude *inc,LPCSTR entry,LPCSTR target,UINT f1,UINT f2,ID3DBlob **code,ID3DBlob **errors){return D3DCompile(src,size,name,defines,inc,entry,target,f1,f2,code,errors);}
static const char vs_source[]=
"cbuffer Endpoints:register(b0){float4 endpoints[2];}"
"struct V{float4 p:SV_POSITION;float4 c:COLOR0;float4 c1:COLOR1;"
"float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
"float f:FOG;float s:PSIZE;float4 b0:TEXCOORD4;float4 b1:TEXCOORD5;};"
"V main(uint id:SV_VertexID){V o=(V)0;o.p=endpoints[id];o.c=float4(1,0.25,0.5,1);"
"o.b0=o.c;return o;}";
static const char ps_source[]="float4 main(float4 p:SV_POSITION,float4 c:COLOR0):SV_Target{return c;}";
static ID3DBlob *compile(const char *src,const char *profile){
    ID3DBlob *blob=NULL,*errors=NULL;
    HRESULT hr=D3DCompile(src,strlen(src),"scaled_line_test",NULL,NULL,"main",profile,0,0,&blob,&errors);
    if(FAILED(hr)&&errors) fprintf(stderr,"%s\n",(char*)ID3D10Blob_GetBufferPointer(errors));
    if(errors) ID3D10Blob_Release(errors);
    assert(SUCCEEDED(hr));return blob;
}
int main(void){
    ID3D11Device *device=NULL; ID3D11DeviceContext *ctx=NULL;
    ID3D11Texture2D *target=NULL,*readback=NULL; ID3D11RenderTargetView *rtv=NULL;
    ID3D11VertexShader *vs=NULL; ID3D11PixelShader *ps=NULL;
    ID3D11Buffer *endpoints=NULL,*sentinel=NULL,*found_cb=NULL;
    ID3D11GeometryShader *found_gs=NULL; ID3D11RasterizerState *rs=NULL,*found_rs=NULL;
    D3D11_TEXTURE2D_DESC td={0}; D3D11_BUFFER_DESC bd={0};
    D3D11_RASTERIZER_DESC rd={0}; D3D11_VIEWPORT vp={0};
    D3D11_MAPPED_SUBRESOURCE mapped; ID3DBlob *blob; HRESULT hr;
    float clear[4]={0}; float points[8]; float widths[]={1,2,4,4.5f,5};
    hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&device,NULL,&ctx);
    assert(SUCCEEDED(hr));
    td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&target)));
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)target,NULL,&rtv)));
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&td,NULL,&readback)));
    bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&bd,NULL,&endpoints)));
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device,&bd,NULL,&sentinel)));
    rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_FRONT;rd.DepthClipEnable=TRUE;
    assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device,&rd,&rs)));
    blob=compile(vs_source,"vs_5_0");
    assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(blob),ID3D10Blob_GetBufferSize(blob),NULL,&vs)));
    ID3D10Blob_Release(blob);blob=compile(ps_source,"ps_5_0");
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(blob),ID3D10Blob_GetBufferSize(blob),NULL,&ps)));
    ID3D10Blob_Release(blob);
    vp.Width=vp.Height=64;vp.MaxDepth=1;
    ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
    ID3D11DeviceContext_RSSetState(ctx,rs);
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,NULL);
    ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(ctx,ps,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&endpoints);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx,0,1,&sentinel);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    for(int axis=0;axis<2;++axis) for(int k=0;k<5;++k){
        D3D8ScaledLineScope scope;unsigned count=0;
        float p[8]={-.5f,0,.5f,1,.5f,0,.5f,1};memcpy(points,p,sizeof(p));
        if(axis){points[0]=points[4]=0;points[1]=-.5f;points[5]=.5f;}
        ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)endpoints,0,NULL,points,0,0);
        ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,clear);
        d3d8_scaled_lines_begin(device,ctx,widths[k],&scope);
        assert(scope.active==(widths[k]>1));
        ID3D11DeviceContext_Draw(ctx,2,0);
        d3d8_scaled_lines_end(ctx,&scope);
        ID3D11DeviceContext_GSGetShader(ctx,&found_gs,NULL,NULL);assert(!found_gs);
        ID3D11DeviceContext_GSGetConstantBuffers(ctx,0,1,&found_cb);assert(found_cb==sentinel);ID3D11Buffer_Release(found_cb);
        ID3D11DeviceContext_RSGetState(ctx,&found_rs);assert(found_rs==rs);ID3D11RasterizerState_Release(found_rs);
        ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)readback,(ID3D11Resource*)target);
        assert(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&mapped)));
        for(unsigned n=0;n<64;++n){unsigned x=axis?n:32,y=axis?32:n;
            unsigned char *pixel=(unsigned char*)mapped.pData+y*mapped.RowPitch+x*4;
            if(pixel[0]){++count;if(!(pixel[0]==255&&pixel[1]==64&&pixel[2]==128&&pixel[3]==255))fprintf(stderr,"axis=%d width=%.1f rgba=%u,%u,%u,%u\n",axis,widths[k],pixel[0],pixel[1],pixel[2],pixel[3]);assert(pixel[0]==255&&pixel[1]==64&&pixel[2]==128&&pixel[3]==255);}
        }
        ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)readback,0);
        if(widths[k]==4.5f)assert(count==4||count==5);else assert(count==(unsigned)widths[k]);
        printf("axis=%d width=%.1f covered=%u state=restored\n",axis,widths[k],count);
    }
    ID3D11DeviceContext_ClearState(ctx);d3d8_scaled_lines_shutdown();
    ID3D11RasterizerState_Release(rs);ID3D11Buffer_Release(sentinel);ID3D11Buffer_Release(endpoints);
    ID3D11VertexShader_Release(vs);ID3D11PixelShader_Release(ps);ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);ID3D11Texture2D_Release(readback);
    ID3D11DeviceContext_Release(ctx);ID3D11Device_Release(device);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nv2a-scaled-lines-') as temp:
    temp=Path(temp); (temp/'test.c').write_text(fixture,encoding='utf-8')
    cmake=ROOT/'.venv/Scripts/cmake.exe'
    project=f"""cmake_minimum_required(VERSION 3.20)
project(scaled_line_test C)
set(CMAKE_C_STANDARD 11)
add_executable(scaled_line_test test.c "{(ROOT/'src/d3d/d3d8_scaled_lines.c').as_posix()}")
target_include_directories(scaled_line_test PRIVATE "{(ROOT/'src/d3d').as_posix()}")
target_link_libraries(scaled_line_test PRIVATE d3d11 d3dcompiler dxguid)
"""
    (temp/'CMakeLists.txt').write_text(project,encoding='utf-8')
    def run(arguments):
        result=subprocess.run(arguments,capture_output=True,text=True)
        if result.returncode:
            print(result.stdout); print(result.stderr)
        result.check_returncode()
        return result.stdout
    run([str(cmake),'-S',str(temp),'-B',str(temp/'build'),'-G','Visual Studio 17 2022',
         '-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
    run([str(cmake),'--build',str(temp/'build'),'--config','Release'])
    print(run([str(temp/'build/Release/scaled_line_test.exe')]),end='')
print('Scaled line WARP rendering regression passed')
