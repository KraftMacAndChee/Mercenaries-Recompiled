"""Exercise the lifted damage/health-event chain through lethal HP and spore removal."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from generated_test_utils import generated_text_containing

ROOT=Path(__file__).resolve().parents[2]

class ActorHealth(unittest.TestCase):
    def test_damage_and_death_notification(self):
        compiler=os.environ.get("CC") or shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).is_file(): self.skipTest("Requires GCC (CC)")
        fixture=(ROOT / "tools/recomp/fixtures/actor_health_host.c").read_text()
        for name in ('0010FC40','0010F170','00110490','0003F950'):
            source=generated_text_containing('void sub_'+name+'(void)')
            fixture+=re.search(r'void sub_'+name+r'\(void\)\n\{.*?\n\}',source,re.S)[0]+'\n'
        fixture+=r"""
enum {E=0x10000,A=0x11000,S=0x12000,LINK=0x13000,END=0x13100,V=0x20000,STACK=0x300000};
int main(void){
 float damage[]={0,.5f,99,99.01f,100,150,10000};unsigned cases=0;
 for(unsigned i=0;i<7;i++)for(unsigned removed=0;removed<2;removed++){
  MEM32(0x30E794)=LINK;MEM32(LINK+8)=E;MEM32(LINK)=END;MEM32(END+8)=0;
  MEM32(E+0xA8)=1;MEM32(E+0x14)=0;MEM32(E+0x18)=E;
  MEM32(E+0x1C)=S;MEM32(E+0x20)=0x12345678;MEMF(E+0x24)=1;
  MEM8(E+0x28)=1;MEM8(E+0x29)=0;MEM32(A+8)=S;MEM32(S+0x28)=0x12345678;
  MEM32(A)=V;MEM32(V+0x208)=1;MEM32(V+0xBC)=1;MEM32(V+0xF0)=1;
  MEM32(V+0xAC)=2;MEM32(V+0x1AC)=3;MEM32(V+0x88)=4;
  MEMF(A+0x98)=100;MEMF(A+0x170)=100;MEM8(A+0x124)=0;MEM8(0x30EBA4)=1;
  MEMF(0x2DC08C)=1;MEMF(0x2DC098)=0;MEMF(0x2DC3DC)=100;
  g_fp_top=8;kills=updates=flashes=cancelled=0;
  esi=0xAAAA;edi=0xBBBB;ebx=0xCCCC;ecx=A;esp=STACK;MEMF(esp+4)=damage[i];
  sub_0003F950();
  assert(esp==STACK+8 && esi==0xAAAA && edi==0xBBBB && ebx==0xCCCC && ecx==A && g_fp_top==8);
  unsigned lethal=100-damage[i]<1;
  assert(MEMF(A+0x98)==(lethal?0:100-damage[i]));
  assert(kills==lethal && updates==!lethal && flashes==(damage[i]>=1));
  if(removed)MEM32(S+0x28)=0;
  ecx=E+0x18;esp=STACK;sub_00110490();
  assert(esp==STACK+8 && LO8(eax)==lethal && cancelled==(removed && !lethal));cases++;
 }
 printf("PASS: %u lifted damage/health-event cases, including lethal overkill and removal before polling\n",cases);
 return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="actor-health-") as folder:
            path=Path(folder);(path/'health.c').write_text(fixture,newline='\n')
            subprocess.run([compiler,'-std=c11','-O2',str(path/'health.c'),'-o',str(path/'health.exe'),'-lm'],check=True)
            subprocess.run([str(path/'health.exe')],check=True)

if __name__=='__main__': unittest.main()
