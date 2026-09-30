"""Execute simultaneous NV2A temporary/output writes on D3D11 WARP."""
from pathlib import Path
import subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parents[2]
s=(Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'src/d3d/d3d8_vsh.c').read_text();s=s[:s.index(' * Input Layout Management')];s=s[:s.rfind('/* ================================================================')]
fixture=r'''
#undef NDEBUG
#include <assert.h>
int main(void){
 ID3D11Device*d=NULL;ID3D11DeviceContext*c=NULL;assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&d,NULL,&c)));
 D3D11_BUFFER_DESC desc={0};desc.ByteWidth=32;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=16;ID3D11Buffer*out,*read;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d,&desc,NULL,&out)));ID3D11UnorderedAccessView*uav;assert(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(d,(ID3D11Resource*)out,NULL,&uav)));desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(ID3D11Device_CreateBuffer(d,&desc,NULL,&read)));
 const char*expr[]={"R4 * 0.125 + 0.6125","R4.wzyx * 0.5","(1.0 / R4.x).xxxx","R12.wxyz"};
 float expected[4][8]={{1.1125f,1.6125f,2.1125f,2.6125f,1.1125f,1.6125f,2.1125f,2.6125f},{8,8,12,2,0,6,4,0},{.25f,8,12,16,.25f,.25f,.25f,.25f},{16,8,12,16,16,4,8,12}};
 for(unsigned i=0;i<4;i++){
  char text[8192];StrBuf sb;sb_init(&sb,text,sizeof(text));sb_append(&sb,"RWStructuredBuffer<float4> resultBuffer:register(u0);\n#define R12 oPos\n[numthreads(1,1,1)] void main(){float4 R4=float4(4,8,12,16),oPos=R4,oT0=0;\n");
  NV2AVshDstOperand dst={0};dst.temp_reg=i==3?12:4;dst.write_mask=i==1?9:i>=2?8:15;dst.output_reg=NV2A_VSH_OUT_T0;dst.output_write_mask=i==1?6:15;emit_dest_assign(&sb,&dst,expr[i]);sb_append(&sb,"resultBuffer[0]=%s;resultBuffer[1]=oT0;}",i==3?"oPos":"R4");
  ID3DBlob*b=NULL,*e=NULL;HRESULT hr=D3DCompile(text,strlen(text),"dual_write",NULL,NULL,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e);if(FAILED(hr)&&e)fprintf(stderr,"%s",(char*)ID3D10Blob_GetBufferPointer(e));assert(SUCCEEDED(hr));ID3D11ComputeShader*cs;assert(SUCCEEDED(ID3D11Device_CreateComputeShader(d,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&cs)));ID3D11DeviceContext_CSSetShader(c,cs,NULL,0);ID3D11DeviceContext_CSSetUnorderedAccessViews(c,0,1,&uav,NULL);ID3D11DeviceContext_Dispatch(c,1,1,1);ID3D11DeviceContext_CopyResource(c,(ID3D11Resource*)read,(ID3D11Resource*)out);D3D11_MAPPED_SUBRESOURCE map;assert(SUCCEEDED(ID3D11DeviceContext_Map(c,(ID3D11Resource*)read,0,D3D11_MAP_READ,0,&map)));float*actual=map.pData;for(unsigned j=0;j<8;j++){if(fabsf(actual[j]-expected[i][j])>1e-6f){fprintf(stderr,"case %u component %u: got %g expected %g\n",i,j,actual[j],expected[i][j]);return 2;}}ID3D11DeviceContext_Unmap(c,(ID3D11Resource*)read,0);ID3D11ComputeShader_Release(cs);ID3D10Blob_Release(b);if(e)ID3D10Blob_Release(e);
 }
 ID3D11DeviceContext_ClearState(c);ID3D11UnorderedAccessView_Release(uav);ID3D11Buffer_Release(out);ID3D11Buffer_Release(read);ID3D11DeviceContext_Release(c);ID3D11Device_Release(d);puts("PASS: dual writes use pre-instruction sources for MAD, swizzled partial masks, reciprocal and R12 alias");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vsh-dual-write-') as td:
 p=Path(td);(p/'test.c').write_text(s+fixture)
 (p/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(dual C)
add_executable(dual test.c)
target_include_directories(dual PRIVATE "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/d3d" "{ROOT.as_posix()}/src/platform")
target_link_libraries(dual PRIVATE d3d11 d3dcompiler dxguid)
''')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 for cmd in [[cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'],[cmake,'--build',str(p/'build'),'--config','Release'],[str(p/'build/Release/dual.exe')]]:
  r=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace')
  if r.returncode:print(r.stdout);print(r.stderr)
  r.check_returncode()
 print(r.stdout)
