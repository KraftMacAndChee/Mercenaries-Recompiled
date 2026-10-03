/* Experimental per-triangle guest-depth interpolation. The geometry stage
 * forwards authored vertices unchanged and supplies flat depth-plane inputs.
 * Bind only around the selected draw; resolves and scaled lines keep their GS. */
#include "d3d8_triangle_depth.h"
#include "d3d8_compiler.h"
#include <stdio.h>
#include <string.h>
static ID3D11GeometryShader *triangle_shader;
static ID3D11Buffer *triangle_constants;
static BOOL triangle_checked;
static const char triangle_shader_source[] =
"struct V {float4 pos:SV_POSITION;float4 c0:COLOR0;float4 c1:COLOR1;"
"float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
"float fog:FOG;float size:PSIZE;float4 back0:TEXCOORD4;float4 back1:TEXCOORD5;"
"nointerpolation float4 p0:TEXCOORD6;nointerpolation float4 p1:TEXCOORD7;nointerpolation float4 p2:TEXCOORD8;};"
"cbuffer G:register(b0){float4 viewport;}"
"[maxvertexcount(3)] void main(triangle V input[3],inout TriangleStream<V> stream){"
"float4 p0=float4(input[0].p0.zw*viewport.xy+viewport.zw,input[0].p0.xy / input[0].pos.w);"
"float4 p1=float4(input[1].p0.zw*viewport.xy+viewport.zw,input[1].p0.xy / input[1].pos.w);"
"float4 p2=float4(input[2].p0.zw*viewport.xy+viewport.zw,input[2].p0.xy / input[2].pos.w);"
"for(uint i=0;i<3;i++){V v=input[i];v.p0=p0;v.p1=p1;v.p2=p2;stream.Append(v);}stream.RestartStrip();}";
void d3d8_triangle_depth_shutdown(void){
 if(triangle_shader)ID3D11GeometryShader_Release(triangle_shader);
 if(triangle_constants)ID3D11Buffer_Release(triangle_constants);
 triangle_shader=NULL;triangle_constants=NULL;triangle_checked=FALSE;
}
static BOOL prepare_triangle_pipeline(ID3D11Device *device){
 if(triangle_checked)return triangle_shader&&triangle_constants;
 triangle_checked=TRUE;ID3DBlob *blob=NULL,*errors=NULL;
 HRESULT hr=d3d8_compile_shader(triangle_shader_source,sizeof(triangle_shader_source)-1,"nv2a_triangle_depth",NULL,NULL,"main","gs_5_0",0,0,&blob,&errors);
 if(SUCCEEDED(hr))hr=ID3D11Device_CreateGeometryShader(device,ID3D10Blob_GetBufferPointer(blob),ID3D10Blob_GetBufferSize(blob),NULL,&triangle_shader);
 if(SUCCEEDED(hr)){D3D11_BUFFER_DESC d={0};d.ByteWidth=16;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr=ID3D11Device_CreateBuffer(device,&d,NULL,&triangle_constants);}
 if(FAILED(hr))fprintf(stderr,"D3D8: triangle-depth pipeline failed %08lX: %s\n",hr,errors?(char*)ID3D10Blob_GetBufferPointer(errors):"unknown");
 if(errors)ID3D10Blob_Release(errors);if(blob)ID3D10Blob_Release(blob);return SUCCEEDED(hr);
}
void d3d8_triangle_depth_begin(ID3D11Device *device,ID3D11DeviceContext *ctx,float width,float height,D3D8TriangleDepthScope *scope){
 memset(scope,0,sizeof(*scope));if(!device||!ctx||width<=0||height<=0||!prepare_triangle_pipeline(device))return;
 D3D11_VIEWPORT v;UINT n=1;ID3D11DeviceContext_RSGetViewports(ctx,&n,&v);if(!n)return;
 float params[4]={v.Width/width,v.Height/height,v.TopLeftX,v.TopLeftY};
 ID3D11DeviceContext_UpdateSubresource(ctx,(ID3D11Resource*)triangle_constants,0,NULL,params,0,0);
 ID3D11DeviceContext_GSGetShader(ctx,&scope->shader,NULL,NULL);ID3D11DeviceContext_GSGetConstantBuffers(ctx,0,1,&scope->constants);
 ID3D11DeviceContext_GSSetShader(ctx,triangle_shader,NULL,0);ID3D11DeviceContext_GSSetConstantBuffers(ctx,0,1,&triangle_constants);scope->active=TRUE;
}
void d3d8_triangle_depth_end(ID3D11DeviceContext *ctx,D3D8TriangleDepthScope *scope){
 if(!scope->active)return;ID3D11DeviceContext_GSSetShader(ctx,scope->shader,NULL,0);ID3D11DeviceContext_GSSetConstantBuffers(ctx,0,1,&scope->constants);
 if(scope->shader)ID3D11GeometryShader_Release(scope->shader);if(scope->constants)ID3D11Buffer_Release(scope->constants);memset(scope,0,sizeof(*scope));
}
