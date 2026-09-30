"""Audit original .xvu programs against the production depth matcher/compiler.
Read-only source assets; retains only metadata, hashes and validation results.
"""
from pathlib import Path
import argparse,subprocess,tempfile,json,struct,hashlib
ROOT=Path(__file__).resolve().parents[2]
a=argparse.ArgumentParser();a.add_argument('directory',type=Path);a.add_argument('--output',required=True,type=Path);args=a.parse_args()
paths=sorted(args.directory.glob('*.xvu'));assert paths
records=[]
for p in paths:
 b=p.read_bytes();count=len(b[4:])//16
 assert len(b)>=20 and (len(b)-4)%16==0 and struct.unpack_from('<H',b,2)[0]==count,(p,len(b))
 records.append(dict(name=p.name,sha256=hashlib.sha256(b).hexdigest(),instructions=count))
source=(ROOT/'src/d3d/d3d8_vsh.c').read_text();source=source[:source.index(' * Input Layout Management')];source=source[:source.rfind('/* ================================================================')]
fixture=r'''
#undef NDEBUG
#include <assert.h>
int main(int argc,char **argv){
 for(int i=1;i<argc;i++){
  FILE*f=fopen(argv[i],"rb");assert(f);unsigned char data[4+136*16];size_t n=fread(data,1,sizeof(data),f);assert(feof(f));fclose(f);assert(n>=20&&(n-4)%16==0);
  NV2AVshProgram p;d3d8_vsh_parse((const DWORD*)(data+4),(int)((n-4)/16),&p);
  VshProjectionDepth d=vsh_projection_depth(&p);
  unsigned rcc=0,projection_constants=0;
  for(int j=0;j<p.length;j++){
   NV2AVshInstruction *v=&p.insns[j];if(v->ilu_op==NV2A_VSH_ILU_RCC)rcc++;
   for(int k=0;k<3;k++)if(v->mac_src[k].reg_type==NV2A_VSH_REG_CONST&&(v->mac_src[k].reg_index==26||v->mac_src[k].reg_index==27))projection_constants++;
  }
  char hlsl[262144];int generated=d3d8_vsh_generate_hlsl(&p,hlsl,sizeof(hlsl));assert(generated>0);
  ID3DBlob *code=NULL,*errors=NULL;HRESULT hr=D3DCompile(hlsl,strlen(hlsl),argv[i],NULL,NULL,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
  if(FAILED(hr)){fprintf(stderr,"%s: %s\n",argv[i],errors?(char*)ID3D10Blob_GetBufferPointer(errors):"compile failed");return 2;}
  if(errors)ID3D10Blob_Release(errors);ID3D10Blob_Release(code);
  printf("%d %d %d %u %u\n",i,p.length,d.instruction,rcc,projection_constants);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vsh-catalog-') as temp:
 p=Path(temp);(p/'test.c').write_text(source+'\n'+fixture)
 (p/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(catalog C)
add_executable(catalog test.c)
target_include_directories(catalog PRIVATE "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/d3d" "{ROOT.as_posix()}/src/platform")
target_link_libraries(catalog PRIVATE d3d11 d3dcompiler dxguid)
''')
 def run(cmd):
  r=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace')
  if r.returncode:print(r.stdout);print(r.stderr)
  r.check_returncode();return r.stdout
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 for line in run([str(p/'build/Release/catalog.exe'),*map(str,paths)]).splitlines():
  if len(line.split())!=5:continue
  i,length,match,rcc,constants=map(int,line.split());records[i-1].update(parsed_instructions=length,centered_depth_instruction=match,rcc=rcc,projection_constant_reads=constants,compiled=True)
 assert all(r.get('compiled') for r in records)
result=dict(scope='Original authoring shader catalog, production parser/matcher and O3 HLSL compile. Does not prove every shader ships in retail or all live material/depth states.',records=records)
args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,indent=2))
print(json.dumps(dict(total=len(records),matched=sum(r['centered_depth_instruction']>=0 for r in records),unmatched=[r['name'] for r in records if r['centered_depth_instruction']<0]),indent=2))
