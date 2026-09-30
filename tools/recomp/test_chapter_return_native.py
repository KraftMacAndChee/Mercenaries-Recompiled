"""Run the lifted spawn routine with stale/no saves, mission starts and Ace endings."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0007.c').read_text()
a=s.index('void sub_00183D40(void)');b=s.index('\n/**',a)
handler=s[a:b]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0005.c').read_text()
a=s.index('void sub_0011B700(void)');b=s.index('\n/**',a)
movie_handler=s[a:b]
a=s.index("void sub_0011B810(void)");b=s.index("\n/**",a)
reset_handler=s[a:b]
pre=r"""
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char memory[0x450000];
static uint32_t eax,ecx,edx,esp,esi,edi,g_seh_ebp;
static int accepted,has_save,parking,placed;static uint32_t requested;
static uint32_t movie_hash,map_hash;static int has_map=1,valid_script=1;
#define MEM8(a) memory[(a)]
#define CMP_NE(a,b) ((a)!=(b))
#define MEM32(a) (*(uint32_t*)(memory+(a)))
#define PUSH32(s,v) ((s)-=4,MEM32(s)=(v))
#define POP32(s,v) ((v)=MEM32(s),(s)+=4)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define g_esp esp
static void sub_00121BD0(void){
 assert(ecx==0x414150);uint32_t k=MEM32(esp+4);assert(k==0xb70a6569 || k==0x4c713ab3);
 eax=k==0xb70a6569?(has_save?0x12000:0):(accepted?0x13000:0);esp+=8;
}
static void sub_001F29F0(void){uint32_t p=MEM32(esp+4);assert(p==0x12000 || p==0x22000 || p==0x22010);eax=p==0x12000?0x11223344:p==0x22000?movie_hash:map_hash;esp+=4;}
static void sub_00113B60(void){esp+=4;}
static void sub_00112AD0(void){eax=MEM32(esp+8)==1?0x22000:0x22010;esp+=4;}
static void sub_001DC970(void){eax=has_map;esp+=4;}
static void sub_001DCC10(void){eax=has_map;esp+=4;}
static void sub_00178970(void){assert(MEM32(esp+4)==map_hash);MEM32(0x403970)=map_hash;esp+=4;}
static void sub_00112120(void){eax=valid_script?0x24000:0;esp+=4;}
static void sub_001EC270(void){requested=MEM32(esp+4);eax=0x1234;esp+=4;}
static void sub_001EC1D0(void){assert(MEM32(esp+4)==0x1234);eax=0x15000;esp+=4;}
static void sub_001ECA50(void){assert(ecx==0x15000);eax=MEM32(esp+4);esp+=8;}
static void sub_00011D80(void){esp+=8;}
static void sub_00066490(void){eax=0x16000;MEM32(eax+0x10)=0x17000;MEM32(0x17000)=0x18000;MEM32(0x18030)=0x19191;esp+=4;}
static void sub_00181400(void){++parking;esp+=4;}
#define RECOMP_ICALL_SAFE(target,saved) do{assert((target)==0x19191);++placed;esp+=8;}while(0)
"""
main=r"""
static void movie(uint32_t name,uint32_t map){
 movie_hash=name;map_hash=map;esp=0x21000;MEM32(esp+4)=0x26000;esi=0x7654;edi=0x3210;
 sub_0011B700();assert(esp==0x21004 && esi==0x7654 && edi==0x3210);
 if(valid_script)assert(MEM32(0x240a0)==name);
}
static void run(int mission,int save,uint32_t expected){
 accepted=mission;has_save=save;parking=placed=0;requested=0;esp=0x20004;ecx=0x19000;esi=0x8765;edi=0x4321;
 sub_00183D40();assert(esp==0x20008);assert(esi==0x8765 && edi==0x4321);
 assert(parking==mission);assert(placed==(expected!=0));assert(requested==expected);
}
static void province(uint32_t from,uint32_t to,int save,uint32_t expected){
 MEM32(0x403970)=from;movie_hash=map_hash=to;esp=0x21000;MEM32(esp+4)=0x26000;esi=0x7654;
 sub_0011B810();assert(esp==0x21004 && esi==0x7654);
 if(valid_script)assert(MEM8(0x24036)==1);
 run(0,save,expected);run(0,1,0x11223344);
}
int main(void){
 assert(location_hash("sw")==0x4a5220af);assert(location_hash("SaveGameLocation")==0xb70a6569);
 const char *movies[]={"CH1_C_UN","CH1_K_UN","CH1_C_CH","CH1_K_CH","CH1_C_RM","CH1_K_RM","CH1_C_SK","CH1_K_SK"};
 uint32_t an=location_hash("loc_hq-allies-respawn");
 run(0,1,0x11223344);run(0,0,0);run(1,1,0);
 for(unsigned i=0;i<8;++i)for(int save=0;save<=1;++save){
  movie(location_hash(movies[i]),0x4a5220af);run(0,save,an);
  run(0,1,0x11223344); /* one arrival only */
 }
 movie(location_hash("CH1_C_UN"),0x4a364a32);run(0,1,0x11223344);
 movie(location_hash("CH2_C_UN"),0x4a5220af);run(0,1,0x11223344);
 movie(location_hash("CH1_C_UN"),0x4a5220af);recomp_chapter_return_movie(0,0);run(0,1,0x11223344);
 movie(location_hash("CH1_C_UN"),0x4a5220af);run(1,1,0);run(0,1,0x11223344);
 has_map=0;movie(location_hash("CH1_C_UN"),0x4a5220af);run(0,1,0x11223344);has_map=1;
 valid_script=0;movie(location_hash("CH1_C_UN"),0x4a5220af);run(0,1,0x11223344);valid_script=1;
 const uint32_t sw=location_hash("sw"),nw=location_hash("nw");
 for(int save=0;save<=1;++save){province(sw,nw,save,0);province(nw,sw,save,0);}
 province(sw,sw,1,0x11223344);province(nw,nw,1,0x11223344);
 province(location_hash("aclubs"),sw,1,0x11223344);
 has_map=0;province(sw,nw,1,0x11223344);has_map=1;
 valid_script=0;province(sw,nw,1,0x11223344);valid_script=1;
 recomp_chapter_return_province(sw,nw);run(1,1,0);run(0,1,0x11223344);
 puts("Province travel: both directions retain authored arrival; saves/retries/other maps/invalid Lua and guest ABI preserved");
 puts("chapter return: all eight Clubs endings return to AN; saves, other chapters, mission parking and guest ABI preserved");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-chapter-return-') as folder:
 d=Path(folder);source=d/'test.c';exe=d/'test.exe'
 source.write_text(pre+(root/'ports/mercenaries/src/chapter_return.c').read_text()+movie_handler+reset_handler+handler+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(source),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
