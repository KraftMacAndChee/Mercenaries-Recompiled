"""Compare the full lifted helicopter fire gate with the retail x86 executable.

External actor/terrain/property getters use identical deterministic fixtures.
The decision, pressure calculation, seat paths and ABI execute unmodified in
both engines. This does not assert that all targeting or combat AI is correct.
"""
from pathlib import Path
import faulthandler, itertools, re, shutil, struct, subprocess, tempfile
import pytest
from tools.recomp import config
ROOT=Path(__file__).resolve().parents[2]
AI,ACTOR,TARGET,TURRET,MINI,SEAT,VT,DATA=range(0x350000,0x358000,0x1000)
STACK,STOP=0x3e0000,0x390100
F=lambda x:struct.pack('<f',x)
U=lambda x:struct.pack('<I',x)


def map_oracle_memory(uc, memory):
    # Unicorn on Windows probes memory using handled SEH during allocation.
    # Python's fault handler otherwise prints a misleading fatal traceback.
    enabled=faulthandler.is_enabled()
    if enabled:faulthandler.disable()
    try:uc.mem_map(0,len(memory));uc.mem_write(0,bytes(memory))
    finally:
        if enabled:faulthandler.enable()


def test_retail_helicopter_fire_gate():
    ucmod=pytest.importorskip('unicorn')
    from unicorn import x86_const as r
    config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
    raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
    memory=bytearray(0x400000)
    for sec in config._SECTIONS:
        if sec.va+sec.raw_size<=0x340000:
            memory[sec.va:sec.va+sec.raw_size]=raw[sec.raw_addr:sec.raw_addr+sec.raw_size]
    def put(a,v):memory[a:a+4]=U(v)
    for obj in [AI,ACTOR,TARGET]:put(obj,VT)
    put(AI+0x10,ACTOR);put(TARGET+8,SEAT);put(SEAT+0x28,0x12345678)
    put(TURRET+0x27c,DATA);put(TURRET+0x28c,MINI)
    callbacks={4:0x390200,0x254:0x390300,0x208:0x390400,
               0xc4:0x390500,0x3c:0x390600,0xf4:0x390700}
    for slot,addr in callbacks.items():put(VT+slot,addr)
    # Executable getters: mov eax,[fixture];ret n / fld [fixture];ret n.
    def ret(n):return b'\xc2'+struct.pack('<H',n) if n else b'\xc3'
    def getter(addr,field,n=0,fp=False):
        code=(b'\xd9\x05' if fp else b'\xa1')+U(field)+ret(n)
        memory[addr:addr+len(code)]=code
    getter(0x390200,0x30d3b0)
    getter(0x390300,DATA+0x40,4);getter(0x390400,DATA+0x44)
    getter(0x162370,DATA+0x44,4);getter(0x1624b0,DATA+0x48)
    getter(0x325e0,DATA,4,True);getter(0x15b60,DATA+4,0,True)
    getter(0x390700,DATA+8,0,True)
    # A vector return writes to caller's output pointer and returns that pointer.
    for addr,src in [(0x390500,DATA+0x10),(0x390600,DATA+0x20)]:
        code=b'\x8b\x44\x24\x04'
        for i in range(3):code+=b'\x8b\x15'+U(src+4*i)+b'\x89\x50'+bytes([4*i])
        code+=ret(4);memory[addr:addr+len(code)]=code
    # Reset the x87 stack before each original invocation.
    code=b'\xdb\xe3\xe9'+struct.pack('<i',0x777f0-(0x390000+7))
    memory[0x390000:0x390000+len(code)]=code
    gen=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text()
    body=re.search(r'void sub_000777F0\(void\)\n\{.*?\n\}',gen,re.S)[0]
    pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) callback(a)
static void pushfp(float x){g_fp_stack[--g_fp_top&7u]=x;}
static void callback(uint32_t a){
 switch(a){
 case 0x390200:eax=MEM32(0x30d3b0);esp+=4;break;
 case 0x390300:eax=MEM32(0x357040);esp+=8;break;
 case 0x390400:eax=MEM32(0x357044);esp+=4;break;
 case 0x390500:case 0x390600:
  eax=MEM32(esp+4);memcpy(memory+eax,memory+(a==0x390500?0x357010:0x357020),12);esp+=8;break;
 case 0x390700:pushfp(MEMF(0x357008));esp+=4;break;
 default:abort();
 }
}
static void sub_00162370(void){eax=MEM32(0x357044);esp+=8;}
static void sub_001624B0(void){eax=MEM32(0x357048);esp+=4;}
static void sub_000325E0(void){pushfp(MEMF(0x357000));esp+=8;}
static void sub_00015B60(void){pushfp(MEMF(0x357004));esp+=4;}
"""
    main=r"""
int main(int argc,char **argv){
 FILE *f=fopen(argv[1],"rb");assert(f&&fread(memory,1,sizeof(memory),f)==sizeof(memory));fclose(f);
 g_xbox_mem_offset=(ptrdiff_t)memory;
 float x,y,splash,warning,skill;unsigned kind;
 while(scanf("%f %f %f %f %f %u",&x,&y,&splash,&warning,&skill,&kind)==6){
  MEMF(0x357000)=splash;MEMF(0x357004)=0;MEMF(0x357008)=skill;
  MEMF(0x357010)=x;MEMF(0x357014)=y;MEMF(0x357018)=0;
  MEMF(0x357020)=0;MEMF(0x357024)=y;MEMF(0x357028)=0;
  MEMF(0x35051c)=warning;MEM32(0x357040)=kind==1?0:0x353000;
  MEM32(0x35328c)=kind==2?0:0x354000;MEM32(0x3532d8)=kind>=3?2:0;
  MEM32(0x357044)=kind==3?0:0x355000;MEM32(0x357048)=kind==4?0:0x352000;
  MEM32(0x354004)=0;MEM8(0x354030)=0xa4;MEMF(0x354048)=-1;
  ecx=0x350000;esp=0x3e0000;MEM32(esp)=0x390100;MEM32(esp+4)=0;MEM32(esp+8)=0x352000;
  ebx=0x11223344;esi=0x22334455;edi=0x33445566;g_seh_ebp=0x44556677;g_fp_top=0;
  sub_000777F0();
  assert(esp==0x3e000c&&ebx==0x11223344&&esi==0x22334455&&edi==0x33445566&&g_fp_top==0);
  printf("%u %u %.9g %u\n",eax&255,MEM8(0x354030),(double)MEMF(0x354048),MEM32(0x354004));
 }
}
"""
    # Boundary distances and splash/friendly-fire/height/skill combinations,
    # including passive guns with/without a rider and unavailable components.
    cases=list(itertools.product([4.99,5,5.01,20,50,119.99,120,120.01],
                                [0,1,4,20],[0,1,1.01,10],[0,.2999,.3,.3001],[.1,.5,1],[0]))
    cases += [(30,20,0,0,.5,k) for k in range(1,6)]
    # Quantize both engines' parameters to their actual float inputs.
    cases=[tuple(struct.unpack('<f',F(v))[0] for v in c[:5])+(c[5],) for c in cases]
    with tempfile.TemporaryDirectory(prefix='merc-heli-oracle-') as td:
        td=Path(td);(td/'memory.bin').write_bytes(memory)
        (td/'test.c').write_text(pre+body+main)
        subprocess.run([shutil.which('gcc'),'-O2','-fno-strict-aliasing',str(td/'test.c'),'-o',str(td/'test.exe')],check=True)
        inputs=''.join(' '.join(map(str,c))+'\n' for c in cases)
        result=subprocess.run([str(td/'test.exe'),str(td/'memory.bin')],input=inputs,text=True,capture_output=True,check=True)
    rows=[line.split() for line in result.stdout.splitlines()]
    assert len(rows)==len(cases)
    # Ensure the fixture reaches real firing and withholding decisions.
    assert any(int(row[1])&1 for row in rows)
    assert any(not(int(row[1])&1) for row in rows)
    sample=rows[cases.index((50.0,20.0,0.0,0.0,0.5,0))]
    assert int(sample[1])&1 and float(sample[2])==pytest.approx(.25)
    uc=ucmod.Uc(ucmod.UC_ARCH_X86,ucmod.UC_MODE_32)
    map_oracle_memory(uc,memory)
    def wu(a,v):uc.mem_write(a,U(v))
    def wf(a,v):uc.mem_write(a,F(v))
    for c,row in zip(cases,rows):
        x,y,splash,warning,skill,kind=c
        for a,v in [(DATA,splash),(DATA+4,0),(DATA+8,skill),(DATA+0x10,x),(DATA+0x14,y),
                    (DATA+0x18,0),(DATA+0x20,0),(DATA+0x24,y),(DATA+0x28,0),(AI+0x51c,warning)]:wf(a,v)
        for a,v in [(DATA+0x40,0 if kind==1 else TURRET),(TURRET+0x28c,0 if kind==2 else MINI),
                    (TURRET+0x2d8,2 if kind>=3 else 0),(DATA+0x44,0 if kind==3 else SEAT),
                    (DATA+0x48,0 if kind==4 else TARGET),(MINI+4,0),(STACK,STOP),(STACK+4,0),(STACK+8,TARGET)]:wu(a,v)
        uc.mem_write(MINI+0x30,b'\xa4');wf(MINI+0x48,-1)
        for reg,v in [(r.UC_X86_REG_ECX,AI),(r.UC_X86_REG_ESP,STACK),(r.UC_X86_REG_EBX,0x11223344),
                      (r.UC_X86_REG_ESI,0x22334455),(r.UC_X86_REG_EDI,0x33445566),(r.UC_X86_REG_EBP,0x44556677)]:uc.reg_write(reg,v)
        uc.emu_start(0x390000,STOP,count=2000)
        assert uc.reg_read(r.UC_X86_REG_EIP)==STOP
        assert uc.reg_read(r.UC_X86_REG_ESP)==STACK+12
        actual=(uc.reg_read(r.UC_X86_REG_EAX)&255,uc.mem_read(MINI+0x30,1)[0],
                struct.unpack('<f',uc.mem_read(MINI+0x48,4))[0],struct.unpack('<I',uc.mem_read(MINI+4,4))[0])
        assert (int(row[0]),int(row[1]),int(row[3]))==(actual[0],actual[1],actual[3]),(c,row,actual)
        assert float(row[2])==pytest.approx(actual[2],abs=1e-6),(c,row,actual)
    print(f'{len(cases)} helicopter fire/pressure/seat cases match retail x86')


def test_shared_turret_schedule_and_tick_match_retail():
    ucmod=pytest.importorskip('unicorn')
    from unicorn import x86_const as r
    config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
    raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
    memory=bytearray(0x400000)
    for sec in config._SECTIONS:
        if sec.va+sec.raw_size<=0x340000:memory[sec.va:sec.va+sec.raw_size]=raw[sec.raw_addr:sec.raw_addr+sec.raw_size]
    # Only the random value is substituted, identically in both engines.
    memory[DATA:DATA+4]=F(.5)
    memory[0x12000:0x12007]=b'\xd9\x05'+U(DATA)+b'\xc3'
    gen=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0003.c').read_text()
    schedule=gen[gen.index('loc_00096BBA: ;'):gen.index('loc_00096C66: ;')]
    tick=gen[gen.index('loc_00096D38: ;'):gen.index('loc_00096D5B: ;')]
    pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <stdio.h>
#include <assert.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
unsigned char memory[0x400000];
#define fp_top() g_fp_stack[g_fp_top&7u]
#define fp_pop() (g_fp_top++)
static void sub_00012000(void){g_fp_stack[--g_fp_top&7u]=.5f;esp+=4;}
"""
    main=r"""
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb");assert(f&&fread(memory,1,sizeof(memory),f)==sizeof(memory));fclose(f);
 g_xbox_mem_offset=(ptrdiff_t)memory;
 unsigned mode,kind,shoot;float timer,relax,pressure,dt;
 while(scanf("%u %f %f %f %u %f %u",&mode,&timer,&relax,&pressure,&kind,&dt,&shoot)==7){
  esi=0x354000;edi=0x353000;esp=0x3e0000;g_fp_top=0;
  MEMF(esi+0x4c)=timer;MEMF(esi+0x50)=relax;MEMF(esi+0x48)=pressure;
  MEM32(edi+0x1c8)=kind;MEMF(esp+0x68)=dt;MEM8(esp+0x10)=shoot;
  if(mode==0)schedule();else tick();
  assert(esp==0x3e0000&&g_fp_top==0);
  printf("%.9g %.9g\n",(double)MEMF(esi+0x4c),(double)MEMF(esi+0x50));
 }
}
"""
    c=pre+'static void schedule(void){\n'+schedule+'\nloc_00096C66: return;}\nstatic void tick(void){\n'+tick+'\nloc_00096D63:return;}\n'+main
    cases=[(0,t,rel,p,k,0,0) for t,rel,p,k in itertools.product([-5,0,1],[-4,0,1],[0,.1,.5,1],[0,1,2,3])]
    cases += [(1,t,0,0,0,dt,shoot) for t,dt,shoot in itertools.product([-5,0,.001,1],[0,1/120,1/60,1/30,.1], [0,1])]
    inputs=''.join(' '.join(map(str,c))+'\n' for c in cases)
    with tempfile.TemporaryDirectory(prefix='merc-turret-oracle-') as td:
        td=Path(td);(td/'memory.bin').write_bytes(memory);(td/'test.c').write_text(c)
        subprocess.run([shutil.which('gcc'),'-O2','-fno-strict-aliasing',str(td/'test.c'),'-o',str(td/'test.exe')],check=True)
        result=subprocess.run([str(td/'test.exe'),str(td/'memory.bin')],input=inputs,text=True,capture_output=True,check=True)
    uc=ucmod.Uc(ucmod.UC_ARCH_X86,ucmod.UC_MODE_32);map_oracle_memory(uc,memory)
    rows=result.stdout.splitlines();assert len(rows)==len(cases)
    for case,row in zip(cases,rows):
        mode,t,rel,p,k,dt,shoot=case
        for a,v in [(MINI+0x4c,t),(MINI+0x50,rel),(MINI+0x48,p),(STACK+0x68,dt)]:uc.mem_write(a,F(v))
        uc.mem_write(TURRET+0x1c8,U(k));uc.mem_write(STACK+0x10,bytes([shoot]))
        for reg,val in [(r.UC_X86_REG_ESI,MINI),(r.UC_X86_REG_EDI,TURRET),(r.UC_X86_REG_ESP,STACK)]:uc.reg_write(reg,val)
        start,end=(0x96bba,0x96c66) if mode==0 else (0x96d38,0x96d63)
        uc.emu_start(start,end,count=400)
        assert uc.reg_read(r.UC_X86_REG_EIP)==end
        got=struct.unpack('<2f',uc.mem_read(MINI+0x4c,8))
        assert tuple(map(float,row.split()))==pytest.approx(got,abs=1e-6),(case,row,got)
    print(f'{len(cases)} shared turret schedule/tick cases match retail x86')
