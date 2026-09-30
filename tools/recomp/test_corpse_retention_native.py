"""Exercise lifted human hibernation and its corpse-policy restore path.

Scene/AI and spore storage are controlled dependencies. Property construction,
list insertion, boolean parsing, and the actor save/restore code are lifted C.
"""
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
patches = runpy.run_path(str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py"))["PATCHES"]
patch = next(p for p in patches if p.name == "Preserve human corpse cleanup policy during hibernation")
human = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c").read_text(encoding="utf-8")
properties = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0009.c").read_text(encoding="utf-8")

def function(source, address):
    return re.search(r"void sub_" + address + r"\(void\)\n\{.*?\n\}", source, re.S)[0]

fixed = function(human, "0004D390")
assert fixed.count(patch.after) == 1
baseline = fixed.replace(patch.after, patch.before)
assert baseline.count(patch.before) == 1
helpers = "\n".join(function(properties, a) for a in ("001EA730", "001EB010", "001EB080", "001EF710"))
restore = human[human.index("loc_00055F75: ;"):human.index("loc_00055F92: ;")]
restore = "static void restore_policy(void) { uint32_t ebp=0x1000;\n" + restore + "loc_00055F92: ; MEM8(ebp+0x6A4)=LO8(eax); }"

PRE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t mem[0x400000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
#define g_esp esp
#define MEM8(a) mem[(a)]
#define MEM16(a) (*(uint16_t *)(mem+(a)))
#define SMEM16(a) (*(int16_t *)(mem+(a)))
#define MEM32(a) (*(uint32_t *)(mem+(a)))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define PUSH32(s,v) do { uint32_t t=(v); s-=4; MEM32(s)=t; } while(0)
#define POP32(s,v) do { v=MEM32(s); s+=4; } while(0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
static uint32_t saved[32][2], saved_count;
static unsigned state, finalized;
static uint32_t lookup(uint32_t key) {
 for(unsigned i=0;i<saved_count;i++) if(saved[i][0]==key) return saved[i][1];
 return 0;
}
static void icall(uint32_t target) {
 switch(target) {
 case 1: case 2: esp+=4; return; /* pose borrowing/lending */
 case 3: eax=state==1;esp+=4;return; /* dead */
 case 4: eax=state==2;esp+=4;return; /* subdued */
 case 5: eax=0;esp+=4;return; /* no AI */
 case 6: eax=0;esp+=8;return; /* no authored scripted-use flag */
 case 7: { /* spore property lookup resolves interned hash to text */
  uint32_t value=lookup(MEM32(esp+4));
  assert(!value || value==0x4DB211E5u || value==0x0B069958u);
  eax=value ? (value==0x4DB211E5u ? 0x6000 : 0x6010) : 0;
  esp+=8;return;
 }
 default: abort();
 }
}
#define RECOMP_ICALL_SAFE(target,expected) do { icall(target); assert(esp==(expected)); } while(0)
static void sub_00041990(void){esp+=4;} /* parent hibernation */
static void sub_0004C780(void){finalized++;esp+=4;} /* pose finalization */
static void sub_00113700(void){eax=0x5000;esp+=4;} /* properties */
static void sub_001EDF40(void){ /* merge serialized properties into spore */
 uint32_t list=MEM32(esp+4);
 for(unsigned i=0;i<MEM16(list);i++) {
  uint32_t key=MEM32(list+4+i*8),value=MEM32(list+8+i*8),j=0;
  while(j<saved_count && saved[j][0]!=key) j++;
  assert(j<32); if(j==saved_count) saved_count++;
  saved[j][0]=key;saved[j][1]=value;
 }
 esp+=8;
}
static void sub_000443D0(void){MEM32(ecx+4)=MEM32(esp+4);esp+=8;}
static void sub_002370B8(void){abort();}
static void sub_00012280(void){abort();}
static void sub_001EAFB0(void){abort();}
"""
MAIN = r"""
static unsigned cases;
static void cycle(int allowed,int verifiable,int scripted,unsigned actor_state) {
 MEM8(0x1000+0x6A4)=allowed;MEM8(0x1000+0x808)=verifiable;
 MEM8(0x1000+0x800)=scripted;MEM8(0x1000+0x801)=1;
 MEM32(0x1000+0x804)=0xABCDEF;state=actor_state;finalized=0;
 MEM32(0x1000+0x6A8)=0x123456;MEM32(0x1000+0x6AC)=0x654321;
 ecx=0x1000;esp=0xF004;ebx=0xAABBCCDD;esi=0x12345678;edi=0x13579BDF;g_seh_ebp=0x11223344;
 sub_0004D390();
 assert(esp==0xF008 && ebx==0xAABBCCDD && esi==0x12345678 && edi==0x13579BDF && g_seh_ebp==0x11223344);
 assert(finalized==(actor_state!=0));
 assert(lookup(0xDA127965)==0x123456 && lookup(0x270A2F14)==0x654321);
 assert(lookup(0x84AF673C)==(verifiable?0x4DB211E5u:0x0B069958u));
 assert(lookup(0x9B9B84CF)==(state==1?0xB5DF2B27u:state==2?0x7FB82A73u:0x5B55AA06u));
 assert(lookup(0x20D5747D)==(FIXED?(allowed?0x4DB211E5u:0x0B069958u):0));
 /* A new actor defaults to true when the serialized property is absent. */
 MEM8(0x1000+0x6A4)=0xCC;ebx=0x5000;esp=0xF000;
 restore_policy();assert(esp==0xF000);
 assert(MEM8(0x1000+0x6A4)==(FIXED?allowed:1));
 cases++;
}
int main(void) {
 MEM32(0x1000)=0x3000;MEM32(0x3000+0x2C4)=1;MEM32(0x3000+0x2C8)=2;
 MEM32(0x3000+0x1CC)=3;MEM32(0x3000+0x318)=4;MEM32(0x3000+0xBC)=5;
 MEM32(0x5000)=0x5100;MEM32(0x5100+0x10)=6;MEM32(0x5100+0xC)=7;
 strcpy((char *)mem+0x6000,"true");strcpy((char *)mem+0x6010,"false");
 for(int a=0;a<2;a++) for(int v=0;v<2;v++) for(int s=0;s<2;s++) for(unsigned st=0;st<3;st++) {
  saved_count=0;
  for(int n=0;n<8;n++) cycle(a,v,s,st);
 }
 /* Mission protects a body, credits it, then permits normal cleanup. */
 saved_count=0;cycle(0,0,0,0);cycle(0,0,0,1);cycle(1,0,0,1);cycle(1,0,0,1);
 printf("PASS: %s; %u cycles; alive/dead/subdued, raw policy, script use, property merge, guest ABI\n",
  FIXED?"corpse retention survives hibernation":"baseline drops corpse retention",cases);
}
"""
with tempfile.TemporaryDirectory(prefix="corpse-retention-") as temp:
    directory = Path(temp)
    for label, body, enabled in (("baseline", baseline, 0), ("fixed", fixed, 1)):
        source, exe = directory / (label + ".c"), directory / (label + ".exe")
        source.write_text(f"#define FIXED {enabled}\n" + PRE + helpers + body + restore + MAIN, encoding="utf-8")
        subprocess.run(["C:/MinGW/bin/gcc.exe", "-O2", "-fno-strict-aliasing", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
