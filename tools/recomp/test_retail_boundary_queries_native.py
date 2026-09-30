"""Execute retail boundary queries against independent polygon/distance oracles."""
from pathlib import Path
import re,runpy,shutil,subprocess,tempfile,unittest,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config

class BoundaryTests(unittest.TestCase):
 def test_vertical_sloped_concave_and_circle_queries(self):
  addresses=(0x1494E0,0x1496B0,0x149F60); found={}
  for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
   s=p.read_text(encoding='utf-8')
   for a in addresses:
    m=re.search(r'void sub_'+f'{a:08X}'+r'\(void\)\n\{.*?\n\}',s,re.S)
    if m: found[a]=m[0]
  self.assertEqual(set(found),set(addresses))
  patch=runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))['BOUNDARY_COMPARE_PATCHES'][0]
  # Both variants always execute: reverting the branch must make the oracle fail.
  old='\n'.join(found.values()).replace(patch.after,patch.before)
  self.assertEqual(old.count(patch.before),1)
  corrected=old.replace(patch.before,patch.after)
  raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
  config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
  constants=''
  for a in (0x2DC098,0x2DC08C,0x2DC3A0):
   off=config.va_to_file_offset(a);constants+=f'MEM32(0x{a:x})=0x{int.from_bytes(raw[off:off+4],"little"):x};'
  header='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <stdio.h>
#include <assert.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset; double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(va) ((void)0)
'''
  main=r'''
enum {BOUNDARY=0x10000,POINTS=0x11000,QUERY=0x12000,STACK=0x3e0000};
static float verts[8][2]; static unsigned count;
static int inside(double x,double z){
 int b=0;for(unsigned i=0,j=count-1;i<count;j=i++)
  if((verts[i][1]>z)!=(verts[j][1]>z) && x<(verts[j][0]-verts[i][0])*(z-verts[i][1])/(verts[j][1]-verts[i][1])+verts[i][0])b=!b;
 return b;
}
static int circle(double x,double z,double r){
 if(!inside(x,z))return 0;
 for(unsigned i=0,j=count-1;i<count;j=i++){
  double dx=verts[j][0]-verts[i][0],dz=verts[j][1]-verts[i][1];
  double t=((x-verts[i][0])*dx+(z-verts[i][1])*dz)/(dx*dx+dz*dz);
  if(t<0)t=0;if(t>1)t=1;
  double a=x-(verts[i][0]+t*dx),b=z-(verts[i][1]+t*dz);
  if(a*a+b*b<=r*r)return 0;
 }return 1;
}
static unsigned query(float x,float z,float r,unsigned type){
 MEMF(QUERY)=x;MEMF(QUERY+8)=z;
 esp=STACK;MEM32(esp)=0;MEM32(esp+4)=QUERY;MEMF(esp+8)=r;
 ecx=BOUNDARY;ebx=0x11111111;esi=0x22222222;edi=0x33333333;g_seh_ebp=0x44444444;
 if(type==0)sub_001496B0();else if(type==1)sub_00149F60();else sub_001494E0();
 assert(esp==STACK+(type==0?8:12));
 assert(ebx==0x11111111&&esi==0x22222222&&edi==0x33333333&&g_seh_ebp==0x44444444);
 return eax&255;
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 CONSTANTS
 const float shapes[3][8][2]={{{-10,-10},{10,-10},{10,10},{-10,10}},{{-11,-8},{9,-7},{12,9},{-7,11}},{{-10,-10},{10,-10},{10,-2},{-2,-2},{-2,10},{-10,10}}};
 unsigned failures=0,cases=0;
 for(unsigned shape=0;shape<3;shape++)for(unsigned reverse=0;reverse<2;reverse++){
  count=shape==2?6:4;MEM32(BOUNDARY+0x64)=count;MEM32(BOUNDARY+0x68)=POINTS;
  double minx=1e6,minz=1e6,maxx=-1e6,maxz=-1e6;
  for(unsigned i=0;i<count;i++){
   unsigned j=reverse?count-1-i:i;verts[i][0]=shapes[shape][j][0];verts[i][1]=shapes[shape][j][1];
   MEMF(POINTS+12*i)=verts[i][0];MEMF(POINTS+12*i+8)=verts[i][1];
   minx=fmin(minx,verts[i][0]);minz=fmin(minz,verts[i][1]);maxx=fmax(maxx,verts[i][0]);maxz=fmax(maxz,verts[i][1]);
  }
  MEMF(BOUNDARY+0x10)=minx;MEMF(BOUNDARY+0x14)=minz;MEMF(BOUNDARY+0x18)=maxx;MEMF(BOUNDARY+0x1c)=maxz;
  for(int ix=-14;ix<14;ix++)for(int iz=-14;iz<14;iz++){
   float x=ix+.3125f,z=iz+.1875f;
   for(unsigned type=0;type<3;type++)for(unsigned rad=0;rad<(type==0?1:3);rad++){
    float r=rad==0?.0625f:rad==1?1.25f:30.f;
    unsigned want=type==0?inside(x,z):type==1?circle(x,z,r):(x+r<minx||z+r<minz||x-r>maxx||z-r>maxz);
    unsigned got=query(x,z,r,type);cases++;
    if(got!=want){if(failures++<3)printf("shape%u reverse%u type%u p=%g,%g r=%g got%u want%u\n",shape,reverse,type,x,z,r,got,want);}
   }
  }
 }
 printf("%u queries, %u mismatches\n",cases,failures);return failures?1:0;
}
'''.replace('CONSTANTS',constants)
  with tempfile.TemporaryDirectory() as d:
   for name,body,want in (('negative-control',old,1),('corrected',corrected,0)):
    c=Path(d)/(name+'.c');exe=Path(d)/(name+'.exe');c.write_text(header+body+main,encoding='utf-8')
    build=subprocess.run([shutil.which('gcc'),'-O2',str(c),'-o',str(exe)],capture_output=True,text=True)
    self.assertEqual(build.returncode,0,build.stderr)
    result=subprocess.run([str(exe)],capture_output=True,text=True);print(name+': '+result.stdout.strip())
    self.assertEqual(result.returncode,want,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
