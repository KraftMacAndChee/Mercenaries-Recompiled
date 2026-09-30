"""Regression-test retail enemy memory against recorded expected results.

Exercises the existing expiry, refresh, eviction and personal-relation cases.
The current fixture is reproduced by executing the retail XBE under Unicorn;
see fixtures/enemy_memory/README.md for provenance and coverage limits.
Running this test needs the retail XBE/generated code.
"""
from pathlib import Path
import re,sys,subprocess,tempfile,struct,gzip,hashlib,json
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
FIXTURES=Path(__file__).resolve().parent/'fixtures/enemy_memory'
metadata=json.loads((FIXTURES/'manifest.json').read_text(encoding='utf-8'))
packed=(FIXTURES/'expected.bin.gz').read_bytes()
assert hashlib.sha256(packed).hexdigest()==metadata['compressed_sha256'], 'Expected-results archive hash mismatch'
expected=gzip.decompress(packed)
assert len(expected)==metadata['uncompressed_bytes']
assert hashlib.sha256(expected).hexdigest()==metadata['uncompressed_sha256'], 'Expected-results content hash mismatch'
assert expected[:8]==b'ENMEM001' and metadata['format']=='ENMEM001'
names=['sub_000641B0','sub_000641D0','sub_000642E0','sub_00064320','sub_00064360','sub_00095C00','sub_000664D0']
bodies={}
for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
 if p.name=='recomp_dispatch.c':continue
 s=p.read_text(encoding='utf8')
 for n in names:
  m=re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',s,re.S)
  if m:bodies[n]=m[0]
assert len(bodies)==len(names)
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
assert hashlib.sha256(raw).hexdigest()==metadata['retail_xbe_sha256'], 'Unsupported retail XBE for this fixture'
code='\n'.join('void '+n+'(void);' for n in names)+'\n'+'\n'.join(bodies[n] for n in names)
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
void oracle_open(const char*);void oracle_close(void);
void oracle_init(void);void oracle_update(float);float oracle_find(unsigned);
unsigned oracle_add(unsigned,float);unsigned oracle_remove(unsigned);void* oracle_state(void);
enum{STACK=0x3e0000,AI=0x10000,LIST=AI+0x434,ACTOR=0x20000,META=0x21000};
static void prepare(void){esp=STACK;ecx=LIST;esi=0x11223344;edi=0x22334455;ebx=0x33445566;g_seh_ebp=0x44556677;}
static void preserved(unsigned end){assert(esp==end&&esi==0x11223344&&edi==0x22334455&&ebx==0x33445566&&g_seh_ebp==0x44556677);}
static float result(void){float v=(float)g_fp_stack[g_fp_top&7u];g_fp_top++;assert(g_fp_top==0);return v;}
static void init_list(void){prepare();sub_000641B0();preserved(STACK+4);oracle_init();}
static unsigned add(unsigned id,float timeout){prepare();MEM32(esp+4)=id;MEMF(esp+8)=timeout;sub_00064360();preserved(STACK+12);unsigned r=LO8(eax);assert(r==oracle_add(id,timeout));return r;}
static void update(float dt){prepare();MEMF(esp+4)=dt;sub_000641D0();preserved(STACK+8);oracle_update(dt);}
static void remove_id(unsigned id){prepare();MEM32(esp+4)=id;sub_00064320();preserved(STACK+8);assert(LO8(eax)==oracle_remove(id));}
static float find(unsigned id){prepare();MEM32(esp+4)=id;sub_000642E0();preserved(STACK+8);float r=result();assert(fabsf(r-oracle_find(id))<.00002f);return r;}
static void compare(void){unsigned char* o=oracle_state();assert(fabsf(MEMF(LIST)-*(float*)o)<.00002f);for(unsigned i=0;i<4;i++){unsigned id=MEM32(LIST+4+i*8);assert(id==*(unsigned*)(o+4+i*8));if(id)assert(fabsf(MEMF(LIST+8+i*8)-*(float*)(o+8+i*8))<.00002f);}for(unsigned id=1;id<13;id++)find(id);}
static float relation(float f,unsigned id){MEM32(META+0x28)=id;prepare();ecx=AI;MEMF(esp+4)=f;MEM32(esp+8)=ACTOR;sub_000664D0();preserved(STACK+12);return result();}
static uint32_t seed=0x13572468;static uint32_t random_value(void){seed=1664525u*seed+1013904223u;return seed;}
int main(int argc,char**argv){assert(argc==2);oracle_open(argv[1]);g_xbox_mem_offset=(ptrdiff_t)memory;
'''+''.join(init)+r'''
MEM32(ACTOR+8)=META;
for(unsigned fps=30;fps<=120;fps*=2){init_list();assert(add(1,30));assert(!add(1,10));for(unsigned frame=0;frame<fps*31;frame++){update(1.f/fps);compare();}assert(find(1)==0);assert(relation(1,1)==1);}
init_list();for(unsigned id=1;id<=4;id++)add(id,id*10.f);add(5,50);compare();assert(find(1)==0);assert(find(5)==50);remove_id(3);compare();
for(unsigned step=0;step<100000;step++){unsigned r=random_value(),id=1+(r>>8)%12;switch(r%5){case 0:init_list();break;case 1:case 2:add(id,(random_value()%2400)/16.f);break;case 3:remove_id(id);break;default:update((random_value()%400)/64.f);break;}compare();}
init_list();add(7,30);
for(int i=-1024;i<=1024;i++){float f=i/1024.f;float half=.49f*(1.f+MEMF(0x2E6C7C));float expected=f>MEMF(0x2E6C7C)?f*half+(half-1.f):f;assert(fabsf(relation(f,7)-expected)<.000001f);assert(relation(f,8)==f);}
update(30.01f);assert(find(7)==0);for(int i=-1024;i<=1024;i++)assert(relation(i/1024.f,7)==i/1024.f);
oracle_close();puts("PASS: 100000 recorded-reference enemy-memory operations, 30/60/120 Hz expiry, eviction/refresh, 2049 personal-relation recovery values; preserved guest ABI");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='faction-memory-native-') as temp:
 d=Path(temp)
 (d/'fixture.c').write_text(prelude+code+harness)
 (d/'expected.bin').write_bytes(expected)
 flags=['-I',str(ROOT/'ports/mercenaries/src'),'-O1','-fno-strict-aliasing','-msse2','-mfpmath=sse']
 subprocess.run(['C:/MinGW/bin/gcc.exe',*flags,str(d/'fixture.c'),str(FIXTURES/'replay.c'),'-o',str(d/'test.exe')],check=True)
 subprocess.run([str(d/'test.exe'),str(d/'expected.bin')],check=True)
