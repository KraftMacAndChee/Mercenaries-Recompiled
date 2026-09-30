"""Exercise lifted vehicle soft-reset convergence with controlled actor/trig dependencies."""
from pathlib import Path
import re,sys,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing

def body(n):
    return re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',generated_text_containing('void '+n+'(void)'),re.S)[0]
mainbody=body('sub_0009C610')
old='loc_0009C725: ;\n    fp_push(MEMF(esp + 0x50)); /* fld float */'
new='loc_0009C725: ;\n    fp_push(_recomp_reset_raw_angle); /* finish from remaining angle, not frame step */'
mainbody=mainbody.replace(new,old)
assert old in mainbody
helpers='\n'.join(body(n) for n in ['sub_001F8790','sub_001F8860','sub_001F8730','sub_001F9E40'])
raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes();config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
init=[]
for a in sorted(set(int(x,16) for x in re.findall(r'(?:MEMF\(|recomp_xmm_loadss\(\w+, )0x([0-9A-F]+)',helpers+mainbody))):
    o=config.va_to_file_offset(a);assert o is not None
    init.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[o:o+4],"little"):X}u;')
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) test_icall(a)
static void test_icall(uint32_t t){
 if(t==1){eax=8;esp+=4;}
 else{assert(t==2);uint32_t out=MEM32(esp+4);MEMF(out)=0;MEMF(out+4)=0;MEMF(out+8)=-1;eax=out;esp+=8;}
}
static void sub_001EC220(void){eax=0x20000;esp+=4;}
static void sub_001FA000(void){g_fp_stack[--g_fp_top&7u]=atan2f(MEMF(esp+4),MEMF(esp+8));esp+=4;}
static void sub_001F9DE0(void){uint32_t a=MEM32(esp+4),b=MEM32(esp+8);float angle=MEMF(esp+12);MEMF(a)=sinf(angle);MEMF(b)=cosf(angle);esp+=4;}
static float last_raw,last_step;
void recomp_vehicle_recenter_checkpoint(uint32_t state,float max,float raw,float step){last_raw=raw;last_step=step;}
'''
main=r'''
#define STATE 0x10000
#define STACK 0x3e0000
#define OUT 0x22000
static void step(float x,float y,float z,float dt){
 ecx=STATE;esp=STACK;esi=0x12345678;edi=0x23456789;ebx=0x3456789a;g_seh_ebp=0x456789ab;
 MEM32(esp+4)=OUT;MEMF(esp+8)=x;MEMF(esp+12)=y;MEMF(esp+16)=z;MEMF(esp+20)=dt;
 sub_0009C610();
 assert(esp==STACK+24&&esi==0x12345678&&edi==0x23456789&&ebx==0x3456789a&&g_fp_top==0);
 assert(g_seh_ebp==0x456789ab);assert(fabsf(MEMF(OUT+4)-y)<1e-6f);
 assert(fabsf(MEMF(OUT)*MEMF(OUT)+MEMF(OUT+8)*MEMF(OUT+8)-(x*x+z*z))<.0002f);
}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
INIT
MEM32(0x20000)=0x21000;MEM32(0x21004)=1;MEM32(0x21058)=2;MEM32(0x30d358)=0xffffffffu;MEM32(0x30d354)=8;
unsigned cases=0,shorts=0;
float times[]={0,.00001f,.0001f,.0005f,.001f,1.f/120,1.f/60,1.f/30,.1f};
for(unsigned i=0;i<721;i++)for(unsigned t=0;t<sizeof(times)/sizeof(times[0]);t++){
 float a=(i-360.f)*3.14159265f/360.f;
 MEM8(STATE+0x15c)=1;step(10*sinf(a),3,10*cosf(a),times[t]);
 int finish=times[t]>0&&fabsf(FIXED?last_raw:last_step)<.005f;
 assert(MEM8(STATE+0x15c)==!finish);cases++;
 if(times[t]>0&&fabsf(last_step)<.005f&&fabsf(last_raw)>=.005f)shorts++;
}
MEM8(STATE+0x15c)=1;step(0,3,-10,.0001f);
assert(MEM8(STATE+0x15c)==FIXED);
for(unsigned i=0;i<120;i++)step(MEMF(OUT),3,MEMF(OUT+8),1.f/60);
assert(FIXED?MEMF(OUT+8)>9.99f:MEMF(OUT+8)<-9.99f);
printf("PASS: %s; %u heading/timestep/ABI cases; %u short-update cases; front-start convergence %s\n",FIXED?"corrected remaining-angle finish":"baseline frame-step finish",cases,shorts,FIXED?"reaches rear":"prematurely stops at front");
}
'''.replace('INIT','\n'.join(init))
with tempfile.TemporaryDirectory(prefix='vehicle-recenter-') as d:
    p=Path(d)
    for fixed in [0,1]:
        source=p/f'test{fixed}.c';exe=p/f'test{fixed}.exe'
        source.write_text(f'#define FIXED {fixed}\n'+pre+helpers+mainbody.replace(old,new if fixed else old)+main)
        subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-msse2','-mfpmath=sse','-fno-strict-aliasing',str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
