"""Exercise embedded replacements, bank scoping and rollback in isolated guest memory."""
from pathlib import Path
import os, subprocess, tempfile, sys
ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''#include <assert.h>
#include <stdio.h>
#include "ps2_upgrades.c"
static unsigned char memory[0x4000000];
ptrdiff_t g_xbox_mem_offset;
static int enabled;
static uint32_t heap=0x100000, registrations;
uint32_t recomp_title_heap_allocate(uint32_t n) { uint32_t p=heap; heap=(heap+n+15)&~15u; assert(heap<sizeof(memory)); return p; }
int recomp_options_ps2_upgrades(void) { return enabled; }
void xbox_preview_log_event(const char *tag,const char *fmt,...) {}
void xbox_preview_log_sample(const char *tag,const char *fmt,...) {}
int pgraph_d3d11_register_world_texture(uint32_t o,uint32_t w,uint32_t h,const uint32_t *p) { assert(o && w==256 && h==256 && p); ++registrations; return 1; }
static void prepare(const char *bank,const char *wavebank,unsigned index) {
 memset(memory+0x10000,0,0x1000);
 write32(0x10044,0x10100); write32(0x10114,0x10200); write32(0x10204,0x10300);
 strcpy((char*)memory+0x10328,bank);
 write32(0x10574,0x10600);write32(0x1061c,0x10700);
 strcpy((char*)memory+0x10708,wavebank);write32(0x10556,index);
 write32(0x10560,0xac445);write32(0x10564,1234);write32(0x10568,10512);
}
/* An optional directory of retail XSBs exercises the real bank conversion.
 * These files are supplied by the local game installation, never committed. */
static void bank_conversion(const char *folder) {
 for (unsigned kind=0;kind<2;++kind) {
  char path[1024];snprintf(path,sizeof(path),"%s/%s.xsb",folder,upgrade_banks[kind].name);
  FILE *f=fopen(path,"rb");assert(f);
  uint32_t original=0x30000;unsigned size=(unsigned)fread(memory+original,1,2048,f);fclose(f);
  write32(0x2f000,size);uint32_t copy=recomp_ps2_sound_bank(original,0x2f000);
  assert(copy!=original && read32(0x2f000)>size && read32(copy+20)==read32(0x2f000));
  assert(read16(copy+30)==upgrade_banks[kind].retail_cues+1);
  write32(0x2e004,copy);enabled=0;assert(recomp_ps2_cue_index(0x2e000,2)==2);
  enabled=1;assert(recomp_ps2_cue_index(0x2e000,2)==upgrade_banks[kind].retail_cues);
  assert(recomp_ps2_cue_index(0x2e000,0)==0 && recomp_ps2_cue_index(0x2e000,1)==1);
  unsigned count=upgrade_banks[kind].retail_cues;
  uint32_t def=copy+56+(2*count+1)*20,table=copy+read32(def);
  assert(memory[def+8]==(kind?4:2));
  for(unsigned t=0;t<memory[def+8];++t){
   uint32_t rec=read32(table+4*t),p=copy+(rec>>8);assert(p>=copy && p<copy+read32(copy+20));
   unsigned plays=0;
   for(unsigned e=0;e<(rec&255);++e){
    if(memory[p]==0 || memory[p]==1){assert(read16(p+6)==0);++plays;
     if(kind && t>0)assert((read32(p)>>8)==100);
    }
    p+=8+memory[p+4];
   }
   assert(plays==1);
  }
  if(kind){
   uint32_t p=copy+(read32(table+12)>>8);assert(memory[p]==4 && read16(p+8)==0 && read16(p+10)==546);
   assert(memory[p+16]==0); /* Rifle layer has no volume fade. */
   uint32_t attack=copy+(read32(table)>>8);assert((int16_t)read16(attack+10)==-1024);
   assert(memory[attack+32+5]&4); /* Retail randomized attack retained. */
  }
  uint32_t before=heap;write32(0x2f000,size);assert(recomp_ps2_sound_bank(original,0x2f000)==copy && heap==before);
  memory[original+100]^=1;write32(0x2f000,size);assert(recomp_ps2_sound_bank(original,0x2f000)==original);
 }
}
static void private_track(unsigned kind,unsigned t) {
 upgrade_banks[kind].data=0x10300;
 write32(0x10010,0x10300+56+(2*upgrade_banks[kind].retail_cues+1)*20);
 write32(0x10034,0x10500-t*0x88);
}
int main(int argc,char **argv) {
 g_xbox_mem_offset=(ptrdiff_t)memory;
 if(argc>1)bank_conversion(argv[1]);
 prepare("w_dragun","w_dragun",0);enabled=1;
 recomp_ps2_wave(0x10000,0x10500);assert(read32(0x10564)==1234); /* Retail attack must remain. */
 private_track(1,3);recomp_ps2_wave(0x10000,0x10500);
 assert(waves[0].bytes==87040 && waves[0].rate==22050 && recomp_ps2_wave_data(0x1055c)==waves[0].data);
 enabled=0;assert(recomp_ps2_wave_data(0x1055c)==waves[0].data);
 prepare("w_dragun","w_dragun",0);private_track(1,3);recomp_ps2_wave(0x10000,0x10500);
 assert(recomp_ps2_wave_data(0x1055c)==waves[0].data); /* Prepared cue survives toggle. */
 enabled=1;prepare("base","w_dragun",0);recomp_ps2_wave(0x10000,0x10500);assert(read32(0x10564)==1234);
 prepare("w_dragun","w_dragun",1);recomp_ps2_wave(0x10000,0x10500);assert(read32(0x10564)==1234);
 prepare("w_autoca","combat",42);private_track(0,1);recomp_ps2_wave(0x10000,0x10500);assert(waves[1].bytes==34540 && recomp_ps2_wave_data(0x1055c)==waves[1].data);
 prepare("w_autoca","w_autoca",1);private_track(0,1);recomp_ps2_wave(0x10000,0x10500);assert(read32(0x10564)==1234);
 prepare("w_dragun","combat",33);private_track(1,0);recomp_ps2_wave(0x10000,0x10500);assert(read32(0x10564)==1234);
 prepare("w_dragun","combat",42);private_track(1,2);recomp_ps2_wave(0x10000,0x10500);assert(recomp_ps2_wave_data(0x1055c)==waves[1].data);
 prepare("w_20cann","combat",42);recomp_ps2_wave(0x10000,0x10500);assert(recomp_ps2_wave_data(0x1055c)==waves[1].data);
 assert(!waves[2].data && !waves[3].data); /* Unreferenced supplied variants never enter firing. */
 prepare("base","combat",65);recomp_ps2_wave(0x10000,0x10500);assert(read32(0x10564)==1234);
 recomp_ps2_wave(0,0xffffffff);assert(!recomp_ps2_wave_data(0xffffffff));
 uint32_t tex=0x20000;write32(tex+0x44,0x20100);
 assert(recomp_ps2_texture(tex)==tex);
 write32(tex+0x24,0xa9a66d3fu);enabled=0;assert(recomp_ps2_texture(tex)==tex);
 enabled=1;uint32_t ps2=recomp_ps2_texture(tex);assert(ps2!=tex && registrations==1);
 assert(read32(tex+0x44)==0x20100 && recomp_ps2_texture(tex)==ps2 && registrations==1);
 enabled=0;assert(recomp_ps2_texture(tex)==tex);
 puts("PASS: embedded WAVs, high-detail firing scope, reload/NPC isolation, active-voice lifetime, bounds and M1 toggle rollback");
}
'''
def main():
    with tempfile.TemporaryDirectory(prefix="merc-ps2-") as folder:
        d=Path(folder);(d/"test.c").write_text(SOURCE)
        env=os.environ.copy();env['PATH']='C:/msys64/mingw64/bin;'+env['PATH']
        subprocess.run(['C:/msys64/mingw64/bin/windres.exe','-I'+str(ROOT/'ports/mercenaries/resources'),str(ROOT/'ports/mercenaries/resources/ps2_upgrades.rc'),'-o',str(d/'assets.o')],check=True,env=env)
        subprocess.run(['C:/msys64/mingw64/bin/gcc.exe','-O2','-std=c11','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/kernel'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),str(d/'test.c'),str(d/'assets.o'),'-o',str(d/'test.exe')],check=True,env=env)
        subprocess.run([str(d/'test.exe'),*sys.argv[1:]],check=True,env=env)
if __name__=='__main__': main()
