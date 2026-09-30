"""Exercise lifted PDA routines; reproduce a dossier read consuming a future award."""
from pathlib import Path
import re,sys,subprocess,tempfile


def main():
    ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT));from tools.recomp import config
    temporary=tempfile.TemporaryDirectory(prefix='merc-intel-native-');case=Path(temporary.name)
    s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0003.c').read_text()
    names=['sub_000C8E80','sub_000CB120','sub_000CB070','sub_000CDD70','sub_000CDF00']
    bodies='\n'.join(re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',s,re.S)[0] for n in names)
    oldmail=re.search(r'void sub_000CDF00\(void\)\n\{.*?\n\}',s,re.S)[0]
    assert '/* Reading intel must not consume the capture animation. */' in oldmail
    oldmail=oldmail.replace('/* Reading intel must not consume the capture animation. */','MEM8(edx + edi + 0xDB48) = 1;')
    bodies += '\n'+oldmail.replace('sub_000CDF00','old_mail')
    xbe=ROOT/'game_files/mercenaries-retail/default.xbe';config.configure_from_xbe(str(xbe));raw=xbe.read_bytes();mem=bytearray(0x800000)
    for sec in config._SECTIONS:mem[sec.va:sec.va+sec.raw_size]=raw[sec.raw_addr:sec.raw_addr+sec.raw_size]
    # Retail GetCurrentCardIntelMail writes the transient shown flag at 0xCDF23.
    assert mem[0xCDF23:0xCDF2B] == bytes.fromhex('c6843a48db000001')
    (case/'retail-memory.bin').write_bytes(mem)
    prelude='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x800000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) do {esp+=8;}while(0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(a) do {fprintf(stderr,"unexpected tail %x\n",a);abort();}while(0)
enum{MODE=0x600000,VM=0x610000,STACK=0x7f0000};
static float saved[4];static unsigned suit;
void recomp_datapod_intel_checkpoint(uint32_t status,uint32_t stage){}
void sub_000CC840(void){eax=0x500000;esp+=8;}
void sub_000C9700(void){esp+=8;}
void sub_000C8F40(void){esp+=4;}
void sub_000C9BE0(void){esp+=4;}
void sub_000CBAF0(void){esp+=4;}
void sub_000CB040(void){eax=suit;esp+=4;}
void sub_000C8580(void){strcpy((char*)memory+MEM32(esp+8),"Clubs");esp+=12;}
void sub_002370B8(void){char* d=(char*)memory+MEM32(esp+4);char* f=(char*)memory+MEM32(esp+8);if(strchr(f,'%'))sprintf(d,f,(char*)memory+MEM32(esp+12),MEM32(esp+16));else strcpy(d,f);esp+=4;}
void sub_001F29F0(void){eax=0;const char* p=(char*)memory+MEM32(esp+4);if(strstr(p,"Diamonds"))eax=1;else if(strstr(p,"Spades"))eax=3;else if(strstr(p,"Hearts"))eax=2;esp+=4;}
void sub_00121E00(void){esp+=12;}
void sub_00121DA0(void){unsigned h=MEM32(esp+4);assert(h<4);saved[h]=MEMF(esp+8);esp+=12;}
void sub_00122000(void){unsigned h=MEM32(esp+4);assert(h<4);MEMF(MEM32(esp+8))=saved[h];eax=1;esp+=12;}
void sub_00152580(void){esp+=8;}
void sub_001FFA60(void){eax=1;esp+=4;}
void sub_002066F0(void){esp+=4;}
'''
    harness=r'''
static void prep(void){esp=STACK;ecx=MODE;esi=0x12345678;edi=0x23456789;ebx=0x3456789a;}
static void check(unsigned args){assert(esp==STACK+4+args&&esi==0x12345678&&edi==0x23456789&&ebx==0x3456789a);}
static void mail(unsigned rank,int old){MEM32(MODE+0x19C)=rank;prep();if(old)old_mail();else sub_000CDF00();check(0);assert(eax==0x500000);}
static void enter(void){prep();sub_000CDD70();check(0);}
static void leave(void){prep();sub_000CB070();check(0);}
static void tick(float dt){prep();MEMF(esp+4)=dt;sub_000CB120();check(4);}
int main(int argc,char**argv){g_xbox_mem_offset=(ptrdiff_t)memory;FILE* f=fopen(argv[1],"rb");assert(f);assert(fread(memory,1,sizeof(memory),f)==sizeof(memory));fclose(f);
for(unsigned outcome=2;outcome<=3;outcome++)for(suit=0;suit<4;suit++)for(unsigned fps=30;fps<=120;fps*=2){
memset(memory+MODE,0,0x20000);memset(saved,0,sizeof(saved));MEM32(MODE+0x24)=VM;
for(unsigned i=0;i<52;i++){MEM32(VM+0xDB40+i*40)=i/13;MEM32(VM+0xDB44+i*40)=i%13+2;MEM32(VM+0xDB4C+i*40)=1;}
enter();float expected=0;
/* Six previously read dossiers reproduce a zero bar, recovered by reload. */
for(unsigned rank=2;rank<=7;rank++)mail(rank,1);
for(unsigned rank=2;rank<=7;rank++)MEM32(VM+0xDB4C+(suit*13+rank-2)*40)=outcome;
for(unsigned frame=0;frame<fps*25;frame++)tick(1.f/fps);
assert(MEMF(MODE+0x3B8)==0);
/* Reload restores the flags from scribble; these reads were not persisted. */
for(unsigned rank=2;rank<=7;rank++)MEM8(VM+0xDB48+(suit*13+rank-2)*40)=0;
leave();enter();for(unsigned frame=0;frame<fps*25;frame++)tick(1.f/fps);
assert(MEMF(MODE+0x3B8)==27);
memset(saved,0,sizeof(saved));for(unsigned i=0;i<52;i++){MEM32(VM+0xDB4C+i*40)=1;MEM8(VM+0xDB48+i*40)=0;}enter();
/* Fixed dossier reads leave pending status untouched for free/captured cards. */
for(unsigned rank=2;rank<=14;rank++)mail(rank,0);
for(unsigned rank=2;rank<=14;rank++){
MEM32(VM+0xDB4C+(suit*13+rank-2)*40)=outcome;
mail(rank,0);assert(!MEM8(VM+0xDB48+(suit*13+rank-2)*40));
for(unsigned frame=0;frame<fps*6;frame++)tick(1.f/fps);
expected+=rank<11?rank:rank<14?30:0;
assert(saved[suit]==expected);
assert(fabsf(MEMF(MODE+0x3B8)-expected)<.001f);leave();enter();assert(fabsf(MEMF(MODE+0x3B8)-expected)<.001f);
}
/* Close during a partially filled face-card award: Exit commits once. */
MEM32(VM+0xDB4C+(suit*13+11-2)*40)=3;MEM8(VM+0xDB48+(suit*13+11-2)*40)=0;
saved[suit]=54;enter();for(unsigned frame=0;frame<fps*5/2;frame++)tick(1.f/fps);
assert(MEMF(MODE+0x3B8)>54 && MEMF(MODE+0x3B8)<84);leave();assert(saved[suit]==84);enter();for(unsigned frame=0;frame<fps*6;frame++)tick(1.f/fps);assert(MEMF(MODE+0x3B8)==84);
}puts("PASS: old getter reproduces empty 2-7 meter and reload catch-up; fixed capture/kill+dossier sequences, 4 suits, 30/60/120fps, interrupted awards, save/entry and guest ABI");}
'''
    (case/'fixture.c').write_text(prelude+'\n'.join('void '+n+'(void);' for n in names)+bodies+harness)
    subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(case/'fixture.c'),'-o',str(case/'test.exe')],check=True)
    subprocess.run([str(case/'test.exe'),str(case/'retail-memory.bin')],check=True)
    temporary.cleanup()
    # NG+ explicitly reuses this harness after the dossier regression passes.
    return {"prelude": prelude, "harness": harness, "bodies": bodies, "mem": mem}


if __name__ == "__main__":
    main()
