/* NV2A lines remain one guest pixel wide when surfaces are upscaled.
 * Xemu scales rasterizer line width; D3D11 has no wide-line state, so expand
 * programmable NV2A lines after vertex shading. Bind only around the draw:
 * leaving a geometry shader active would corrupt later resolves and copies. */
#include "d3d8_scaled_lines.h"
#include "d3d8_compiler.h"
#include <stdio.h>
#include <string.h>

static ID3D11GeometryShader *line_shader;
static ID3D11Buffer *line_constants;
static ID3D11RasterizerState *line_rasterizer;
static D3D11_RASTERIZER_DESC line_rasterizer_desc;
static BOOL line_checked;
static const char line_shader_source[] =
    "struct NV2ALineVertex {\n"
    "    float4 position : SV_POSITION;\n"
    "    float4 color0 : COLOR0;\n"
    "    float4 color1 : COLOR1;\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float4 tc1 : TEXCOORD1;\n"
    "    float4 tc2 : TEXCOORD2;\n"
    "    float4 tc3 : TEXCOORD3;\n"
    "    float fog : FOG;\n"
    "    float point_size : PSIZE;\n"
    "    float4 back0 : TEXCOORD4;\n"
    "    float4 back1 : TEXCOORD5;\n"
    "};\n"
    "cbuffer LineConstants : register(b0) { float4 line_params; };\n"
    "NV2ALineVertex mix_vertex(NV2ALineVertex a, NV2ALineVertex b, float t) {\n"
    "    NV2ALineVertex o;\n"
    "    o.position=lerp(a.position, b.position, t);\n"
    "    o.color0=lerp(a.color0, b.color0, t);\n"
    "    o.color1=lerp(a.color1, b.color1, t);\n"
    "    o.tc0=lerp(a.tc0, b.tc0, t);\n"
    "    o.tc1=lerp(a.tc1, b.tc1, t);\n"
    "    o.tc2=lerp(a.tc2, b.tc2, t);\n"
    "    o.tc3=lerp(a.tc3, b.tc3, t);\n"
    "    o.fog=lerp(a.fog, b.fog, t);\n"
    "    o.point_size=lerp(a.point_size, b.point_size, t);\n"
    "    o.back0=lerp(a.back0, b.back0, t);\n"
    "    o.back1=lerp(a.back1, b.back1, t);\n"
    "    return o;\n"
    "}\n"
    "[maxvertexcount(4)]\n"
    "void main(line NV2ALineVertex input[2], inout TriangleStream<NV2ALineVertex> stream) {\n"
    "    NV2ALineVertex a=input[0], b=input[1], v;\n"
    "    // Clip at the eye before dividing; ordinary frustum clipping follows the GS.\n"
    "    const float epsilon=0.00001;\n"
    "    if (a.position.w < epsilon && b.position.w < epsilon) return;\n"
    "    if (a.position.w < epsilon)\n"
    "        a=mix_vertex(a,b,(epsilon-a.position.w)/(b.position.w-a.position.w));\n"
    "    if (b.position.w < epsilon)\n"
    "        b=mix_vertex(b,a,(epsilon-b.position.w)/(a.position.w-b.position.w));\n"
    "    float2 delta=(b.position.xy/b.position.w-a.position.xy/a.position.w)/line_params.xy;\n"
    "    float length2=dot(delta,delta);\n"
    "    if (length2 < 0.000001) return;\n"
    "    float2 offset=float2(-delta.y,delta.x)*rsqrt(length2)*line_params.xy*line_params.z;\n"
    "    v=a; v.position.xy-=offset*a.position.w; stream.Append(v);\n"
    "    v=b; v.position.xy-=offset*b.position.w; stream.Append(v);\n"
    "    v=a; v.position.xy+=offset*a.position.w; stream.Append(v);\n"
    "    v=b; v.position.xy+=offset*b.position.w; stream.Append(v);\n"
    "    stream.RestartStrip();\n"
    "}\n";

void d3d8_scaled_lines_shutdown(void)
{
    if (line_shader) ID3D11GeometryShader_Release(line_shader);
    if (line_constants) ID3D11Buffer_Release(line_constants);
    if (line_rasterizer) ID3D11RasterizerState_Release(line_rasterizer);
    line_shader=NULL; line_constants=NULL; line_rasterizer=NULL;
    line_checked=FALSE;
}

static BOOL prepare_line_pipeline(ID3D11Device *device)
{
    ID3DBlob *blob=NULL, *errors=NULL;
    D3D11_BUFFER_DESC desc={0};
    HRESULT hr;
    if (line_checked) return line_shader && line_constants;
    line_checked=TRUE;
    hr=d3d8_compile_shader(line_shader_source,sizeof(line_shader_source)-1u,
        "nv2a_scaled_lines",NULL,NULL,"main","gs_5_0",0,0,&blob,&errors);
    if (FAILED(hr)) {
        fprintf(stderr,"D3D8: scaled line shader failed: %s\n",
            errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        goto done;
    }
    hr=ID3D11Device_CreateGeometryShader(device,ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob),NULL,&line_shader);
    if (FAILED(hr)) goto done;
    desc.ByteWidth=16u;
    desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    hr=ID3D11Device_CreateBuffer(device,&desc,NULL,&line_constants);
 done:
    if (errors) ID3D10Blob_Release(errors);
    if (blob) ID3D10Blob_Release(blob);
    if (FAILED(hr)) fprintf(stderr,"D3D8: scaled line pipeline unavailable: %08lX\n",hr);
    return SUCCEEDED(hr);
}

void d3d8_scaled_lines_begin(ID3D11Device *device, ID3D11DeviceContext *context,
                            float width, D3D8ScaledLineScope *scope)
{
    D3D11_VIEWPORT viewport;
    D3D11_RASTERIZER_DESC desc={0};
    ID3D11RasterizerState *replacement=NULL;
    UINT viewport_count=1u;
    float params[4];
    memset(scope,0,sizeof(*scope));
    if (!(width > 1.0f) || !device || !context || !prepare_line_pipeline(device)) return;
    ID3D11DeviceContext_RSGetViewports(context,&viewport_count,&viewport);
    if (!viewport_count || viewport.Width <= 0.0f || viewport.Height <= 0.0f) return;
    ID3D11DeviceContext_RSGetState(context,&scope->rasterizer);
    if (scope->rasterizer) ID3D11RasterizerState_GetDesc(scope->rasterizer,&desc);
    else { desc.FillMode=D3D11_FILL_SOLID; desc.DepthClipEnable=TRUE; }
    desc.FillMode=D3D11_FILL_SOLID;
    desc.CullMode=D3D11_CULL_NONE;
    desc.AntialiasedLineEnable=FALSE;
    if (!line_rasterizer || memcmp(&desc,&line_rasterizer_desc,sizeof(desc))) {
        if (FAILED(ID3D11Device_CreateRasterizerState(device,&desc,&replacement))) {
            if (scope->rasterizer) ID3D11RasterizerState_Release(scope->rasterizer);
            scope->rasterizer=NULL;
            return;
        }
        if (line_rasterizer) ID3D11RasterizerState_Release(line_rasterizer);
        line_rasterizer=replacement;
        line_rasterizer_desc=desc;
    }
    params[0]=2.0f/viewport.Width;
    params[1]=2.0f/viewport.Height;
    params[2]=width*0.5f;
    params[3]=0.0f;
    ID3D11DeviceContext_UpdateSubresource(context,(ID3D11Resource *)line_constants,
                                         0u,NULL,params,0u,0u);
    ID3D11DeviceContext_GSGetShader(context,&scope->shader,NULL,NULL);
    ID3D11DeviceContext_GSGetConstantBuffers(context,0u,1u,&scope->constants);
    ID3D11DeviceContext_GSSetShader(context,line_shader,NULL,0u);
    ID3D11DeviceContext_GSSetConstantBuffers(context,0u,1u,&line_constants);
    ID3D11DeviceContext_RSSetState(context,line_rasterizer);
    scope->active=TRUE;
}

void d3d8_scaled_lines_end(ID3D11DeviceContext *context, D3D8ScaledLineScope *scope)
{
    if (!scope->active) return;
    ID3D11DeviceContext_GSSetShader(context,scope->shader,NULL,0u);
    ID3D11DeviceContext_GSSetConstantBuffers(context,0u,1u,&scope->constants);
    ID3D11DeviceContext_RSSetState(context,scope->rasterizer);
    if (scope->shader) ID3D11GeometryShader_Release(scope->shader);
    if (scope->constants) ID3D11Buffer_Release(scope->constants);
    if (scope->rasterizer) ID3D11RasterizerState_Release(scope->rasterizer);
    memset(scope,0,sizeof(*scope));
}
