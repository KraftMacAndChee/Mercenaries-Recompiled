"""Compare retail faction queries/events with the original attitude thresholds.
No replacement faction policy is installed by this test.
"""
from pathlib import Path
import re,sys,subprocess,tempfile,struct
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
names=['sub_00095B70','sub_00095EE0','sub_0010F470','sub_0010F4B0']
bodies={}
for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
 if p.name=='recomp_dispatch.c':continue
 s=p.read_text(encoding='utf8')
 for n in names:
  m=re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',s,re.S)
  if m:bodies[n]=m[0]
assert len(bodies)==4
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
code='\n'.join(bodies[n] for n in names)
addresses=set(int(v,16) for v in re.findall(r'MEM(?:32|F)\(0x([0-9A-Fa-f]+)\)',code))|set(range(0x2E6C7C,0x2E6C7C+20,4))
addresses.update(int(v,16) for v in re.findall(r"recomp_xmm_loadss\([^,]+, 0x([0-9A-Fa-f]+)\)",code))
init=[]
for a in addresses:
 off=config.va_to_file_offset(a);assert off is not None
 init.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[off:off+4],"little"):X}u;')
prelude='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''
harness=r'''
enum {STACK=0x3e0000,EVENT=0x20000,REL=0x323338+0x1c};
static unsigned expected(float f){const float t[]={-.6f,-.2f,.2f,1.f};unsigned i=0;f=fminf(1.f,fmaxf(-1.f,f));while(i<3&&t[i]<f)i++;return i;}
static unsigned attitude(float f){esp=STACK;MEMF(esp+4)=f;sub_00095EE0();assert(esp==STACK+8);return eax;}
static void init_event(unsigned a,unsigned b){esp=STACK;ecx=EVENT;esi=0x11223344;MEM32(esp+4)=0x77777;MEM32(esp+8)=a;MEM32(esp+12)=b;sub_0010F470();assert(esp==STACK+16&&esi==0x11223344&&g_fp_top==0);}
static unsigned changed(void){esp=STACK;ecx=EVENT;esi=0x11223344;MEMF(esp+4)=1.f/60;sub_0010F4B0();assert(esp==STACK+8&&esi==0x11223344&&g_fp_top==0);return LO8(eax);}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
'''+''.join(init)+r'''
 const float boundaries[]={-2,-1,-.600001f,-.6f,-.599999f,-.200001f,-.2f,-.199999f,.199999f,.2f,.200001f,1,2};
 for(unsigned i=0;i<sizeof(boundaries)/sizeof(boundaries[0]);i++){unsigned actual=attitude(boundaries[i]);if(actual!=expected(boundaries[i]))fprintf(stderr,"relation %.9g actual %u expected %u\n",boundaries[i],actual,expected(boundaries[i]));assert(actual==expected(boundaries[i]));}
 for(int i=-2048;i<=2048;i++)assert(attitude(i/1024.f)==expected(i/1024.f));
 const float values[]={-.9f,-.4f,0,.7f};
 for(unsigned a=0;a<8;a++)for(unsigned b=0;b<8;b++)for(unsigned x=0;x<4;x++)for(unsigned y=0;y<4;y++){
  MEMF(REL+(a*8+b)*4)=values[x];init_event(a,b);assert(!changed());
  MEMF(REL+(a*8+b)*4)=values[y];assert(changed()==(x!=y));
  init_event(a,b);assert(!changed());
 }
 puts("PASS: 4110 standing values and 1024 faction-pair transitions/re-registration, preserved stack and x87 state");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='faction-native-') as temp:
 d=Path(temp);(d/'fixture.c').write_text(prelude+code+harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(d/'fixture.c'),'-o',str(d/'test.exe')],check=True)
 subprocess.run([str(d/'test.exe')],check=True)
