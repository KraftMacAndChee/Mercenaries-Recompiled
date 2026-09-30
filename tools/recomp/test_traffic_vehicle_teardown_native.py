"""Execute lifted vehicle teardown and traffic accounting, including ruin removal.

The virtual Deinit dependency checks the path was cleared before it can detach
again. No world actor or live game is modified by this native harness.
"""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
def body(n):
    for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
        s=p.read_text(encoding='utf-8')
        m=re.search(r'void sub_'+n+r'\(void\)\n\{.*?\n\}',s,re.S)
        if m:return m[0]
    raise AssertionError(n)
vehicle=body('00065120')
fix='    /* Ruin replacement also uses vehicle teardown; dead actors still count. */\n    if (MEM32(esi + 0x10) && !(MEMF(MEM32(esi + 0x10) + 0x98) > 0.0f))\n        SET_LO8(ecx, 1);\n'
assert fix in vehicle
# A previous two-patch approach reinserted the old provenance event on replay.
# Verify fresh and legacy snapshots converge through the actual transform.
import ast
patch_path=ROOT/'ports/mercenaries/scripts/Patch-Generated.py'
tree=ast.parse(patch_path.read_text(encoding='utf-8'))
fn=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='patch_traffic_vehicle_teardown')
ns={'re':re};exec(compile(ast.Module(body=[fn],type_ignores=[]),str(patch_path),'exec'),ns)
trace='    recomp_traffic_release_checkpoint(0x0006512Au, esi, LO8(ecx));\n'
legacy='    recomp_traffic_release_checkpoint(0x0006512Au, esi, MEM8(esp + 8) == 0);\n'
raw=vehicle.replace(fix,'').replace(trace,'')
for source in (raw,raw.replace('loc_0006512A: ;\n','loc_0006512A: ;\n'+legacy),vehicle,vehicle.replace('loc_0006512A: ;\n','loc_0006512A: ;\n'+legacy)):
    corrected=ns[fn.name](source)
    assert corrected==vehicle and ns[fn.name](corrected)==corrected
print('PASS: four generation layouts converge; correction is idempotent')
account='\n'.join(body(n) for n in ['00168E90','00168F00','00169E90'])
pre=r"""
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>
static unsigned char mem[0x400000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_esp;
static unsigned deinit_calls,release_calls;
static float xmm0v[4],xmm1v[4];
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define MEM32(a) (*(uint32_t*)(mem+(a)))
#define MEM8(a) mem[(a)]
#define MEMF(a) (*(float*)(mem+(a)))
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
#define PUSH32(s,v) do{uint32_t t=(v);s-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{v=MEM32(s);s+=4;}while(0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_S(a,b) ((int32_t)((a)&(b))<0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define RECOMP_ICALL_SAFE(a,b) deinit(a)
static void deinit(uint32_t address){
 assert(address==123&&ecx==0x5000&&MEM32(ecx+0x3c)==0);
 ++deinit_calls;esp+=4;
}
static void recomp_xmm_loadss(float*v,uint32_t a){v[0]=MEMF(a);v[1]=v[2]=v[3]=0;}
static void recomp_xmm_zero(float*v){memset(v,0,16);}
static void sub_00113700(void){eax=MEM32(ecx+8);esp+=4;}
static void sub_001ED370(void){eax=MEM32(MEM32(esp+4)+0x28);esp+=4;}
static void recomp_traffic_detach_checkpoint(uint32_t a,uint32_t b,uint32_t c,uint32_t d){assert(d<=1);}
static void recomp_traffic_release_checkpoint(uint32_t site,uint32_t ai,uint32_t dead){
 assert(site==0x6512a&&ai==0x5000&&dead<=1);++release_calls;
}
"""
main=r"""
#define MANAGER 0x3dc970
#define RECORD (MANAGER+0x18b58)
static void invoke(unsigned flag){
 ecx=0x5000;esp=0xf000;g_esp=esp;esi=0x12345678;edi=0x23456789;ebx=0x3456789a;
 MEM32(esp+4)=flag;sub_00065120();
 assert(esp==0xf008&&esi==0x12345678&&edi==0x23456789&&ebx==0x3456789a);
}
int main(void){
 unsigned cases=0,missed=0;
 float health[]={600,1,.000001f,0,-1,-93.1468887f,INFINITY,-INFINITY,NAN};
 unsigned flags[]={0,1,0x80,0xff};
 for(unsigned h=0;h<9;h++)for(unsigned flag=0;flag<4;flag++)
 for(unsigned actor=0;actor<2;actor++)for(unsigned path=0;path<2;path++)
 for(unsigned timer=0;timer<2;timer++)for(unsigned cooldown=0;cooldown<2;cooldown++){
 memset(mem,0,sizeof(mem));deinit_calls=release_calls=0;
 MEM32(0x5000)=0x5100;MEM32(0x5118)=123;MEM32(0x5010)=actor?0x6000:0;
 MEM32(0x503c)=path?0x9000:0;MEM32(0x6008)=0x7000;MEM32(0x7028)=100;
 MEMF(0x6098)=health[h];
 MEM32(MANAGER+0x18b54)=1;MEM32(RECORD)=0x9000;MEM32(RECORD+4)=0x2000;
 MEM32(RECORD+8)=0x4000;MEM32(RECORD+12)=1;MEM32(0x4004)=1;MEM32(0x226c)=1;
 MEM32(0x2008)=1;MEM32(0x200c)=100;MEM32(0x2084)=1;
 MEMF(0x2278)=cooldown?600:0;MEMF(RECORD+0x20)=timer?123:0;
 int destroyed=flags[flag]==0||(actor&&!(health[h]>0));
 int effective=FIXED?destroyed:flags[flag]==0;
 unsigned counted=path&&cooldown&&effective;
 invoke(flags[flag]);
 assert(deinit_calls==1&&release_calls==path&&MEM32(0x503c)==0);
 assert(MEM32(RECORD+12)==1-path&&MEM32(0x4004)==1-path&&MEM32(0x226c)==1-path);
 assert(MEM32(0x2084)==1-(path&&actor));
 assert(MEM32(RECORD+0x1c)==counted&&MEM32(0x2270)==counted);
 assert(MEMF(RECORD+0x20)==(timer?123:counted?600:0));
 if(path&&cooldown&&destroyed&&!effective)missed++;
 invoke(flags[flag]);
 assert(deinit_calls==2&&release_calls==path&&MEM32(RECORD+0x1c)==counted);
 assert(MEM32(RECORD+12)==1-path);cases++;
 }
 assert(FIXED?missed==0:missed==30);
 printf("%s: %u lifted teardown/accounting cases; missed cooldowns=%u; alive/dead/null, hibernation flags, timer preservation, one detach, ABI\n",FIXED?"fixed":"legacy",cases,missed);
}
"""
with tempfile.TemporaryDirectory(prefix='traffic-teardown-') as d:
    d=Path(d)
    for fixed in (0,1):
        source=pre+account+(vehicle if fixed else vehicle.replace(fix,''))+main
        (d/'test.c').write_text('#define FIXED '+str(fixed)+'\n'+source,encoding='utf-8')
        subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing',str(d/'test.c'),'-o',str(d/'test.exe')],check=True,timeout=30)
        subprocess.run([str(d/'test.exe')],check=True,timeout=30)
