"""Run the flock core at different presentation rates and through pauses/stalls."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class Boids(unittest.TestCase):
    def test_fixed_step_and_stability(self):
        compiler = os.environ.get("CC") or shutil.which("gcc")
        if not compiler and Path("C:/MinGW/bin/gcc.exe").is_file():
            compiler = "C:/MinGW/bin/gcc.exe"
        if not compiler:
            self.skipTest("Set CC to a GCC-compatible C compiler")
        bridge = (ROOT/"ports/mercenaries/src/boids_guest.h").read_text()
        hash_function = re.search(r"static uint32_t boid_hash\([^\n]+\)\n\{.*?\n\}", bridge, re.S)[0]
        fixture = "#include <stdint.h>\n" + hash_function + r"""
#include "boids.h"
#include "boid_wings.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
int main(void){
 assert(boid_hash("global_seagull")==0x83EBB9F6u);
 assert(boid_hash("recomp_boid_00")==0x1937987Bu);
 boid_flock a,b,c;boid_vec center={-840,50,510};
 boids_init(&a,center,7);boids_init(&b,center,7);boids_init(&c,center,7);
 for(int i=0;i<600;i++)boids_advance(&a,1.f/30);
 for(int i=0;i<1200;i++)boids_advance(&b,1.f/60);
 for(int i=0;i<2400;i++)boids_advance(&c,1.f/120);
 assert(!memcmp(a.members,b.members,sizeof(a.members)));
 assert(!memcmp(a.members,c.members,sizeof(a.members)));
 boid_flock saved=a;assert(!boids_advance(&a,0));assert(!memcmp(&a,&saved,sizeof(a)));
 assert(!boids_advance(&a,NAN));assert(!boids_advance(&a,-1));
 for(int i=0;i<30000;i++)boids_advance(&a,1.f/30);
 for(int i=0;i<RECOMP_BOID_COUNT;i++){
  boid_vec p=a.members[i].position,v=a.members[i].velocity;
  assert(isfinite(p.x)&&isfinite(p.y)&&isfinite(p.z));
  assert(fabsf(p.x-center.x)<125 && fabsf(p.z-center.z)<125);
  assert(v.x*v.x+v.y*v.y+v.z*v.z<100.01f);
 }
 assert(boids_advance(&a,100)<9);
 for(unsigned rate=30;rate<=240;rate*=2){
  boid_wings w;boid_wings_init(&w,7,0);float phase=0;unsigned starts=0,ends=0,held=0;int was=0;
  for(unsigned frame=0;frame<rate*120;frame++){
   if(!was){phase+=1.f/rate;if(phase>1)phase-=1;}
   int glide=boid_wings_advance(&w,1.f/rate,phase);
   if(glide){assert(phase>=BOID_GLIDE_PHASE || was);phase=BOID_GLIDE_PHASE;held++;}
   if(glide&&!was)starts++;if(!glide&&was)ends++;was=glide;
  }
  assert(starts>=7&&ends>=7&&held>rate*40&&held<rate*90);
 }
 boid_wings wings;boid_wings_init(&wings,0,.7f);wings.flap_left=0;
 assert(!boid_wings_advance(&wings,.033f,.8f)); // Never hold wings-down.
 assert(!boid_wings_advance(&wings,.033f,.1f)); // Wrapped, still below raised pose.
 assert(boid_wings_advance(&wings,.033f,.3f));
 boid_wings paused=wings;assert(boid_wings_advance(&wings,0,.25f));assert(!memcmp(&paused,&wings,sizeof(wings)));
 assert(boid_wings_advance(&wings,NAN,.25f));assert(!memcmp(&paused,&wings,sizeof(wings)));
 puts("PASS: raised-wing-only glide entry, bounded holds, resume flapping, independent cadence, pause/invalid dt");
 puts("PASS: identical 30/60/120 FPS, pause/invalid dt, bounded stall, 1000-second stability");
}
"""
        with tempfile.TemporaryDirectory(prefix="merc-boids-") as directory:
            folder = Path(directory)
            source, exe = folder / "test.c", folder / "test.exe"
            source.write_text(fixture, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-O2", "-I"+str(ROOT/"ports/mercenaries/src"),
                            str(source), str(ROOT/"ports/mercenaries/src/boids.c"),
                            "-o", str(exe), "-lm"], check=True)
            subprocess.run([str(exe)], check=True)

    def test_guest_spawn_lifecycle(self):
        compiler = os.environ.get("CC") or shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="merc-boid-guest-") as directory:
            exe = Path(directory)/"test.exe"
            subprocess.run([compiler,"-std=c11","-O1","-I"+str(ROOT/"ports/mercenaries/src"),
                            str(ROOT/"tools/recomp/fixtures/boids_guest.c"),
                            str(ROOT/"ports/mercenaries/src/boids.c"),"-o",str(exe),"-lm"],check=True)
            subprocess.run([str(exe)],check=True)

    def test_retired_option_ignored_and_removed_on_save(self):
        compiler = os.environ.get("CC") or shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).is_file() and not shutil.which(compiler):
            self.skipTest("Set CC to a GCC-compatible C compiler")
        fixture = r"""
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char runtime[MAX_PATH];
static DWORD module_path(HMODULE m, LPSTR out, DWORD size) {
    snprintf(out,size,"%s",runtime); return (DWORD)strlen(out);
}
#define GetModuleFileNameA module_path
#define _snprintf_s(out,size,count,...) snprintf(out,size,__VA_ARGS__)
#define strcpy_s(out,size,in) snprintf(out,size,"%s",in)
#define strcat_s(out,size,in) snprintf(out+strlen(out),size-strlen(out),"%s",in)
#include "recomp_options.c"
int main(int argc, char **argv) {
    assert(argc==2); snprintf(runtime,sizeof(runtime),"%s\\runtime.exe",argv[1]);
    char ini[MAX_PATH]; snprintf(ini,sizeof(ini),"%s\\mercenaries_recomp.ini",argv[1]);
    assert(WritePrivateProfileStringA("RecompOptions","BoidSimulation","1",ini));
    recomp_options_init();
    /* The numeric slot now belongs to PS2 Upgrades, not the retired setting. */
    assert(strstr(recomp_options_label(RECOMP_OPTIONS_PS2_UPGRADES_HASH),"PS2 UPGRADES")!=NULL);
    assert(!recomp_options_ps2_upgrades());
    assert(!recomp_options_apply());
    recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH,1);
    assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_FPS);
    assert(GetPrivateProfileIntA("RecompOptions","BoidSimulation",-1,ini)==(UINT)-1);
    puts("PASS: retired boid setting ignored and removed when options are saved");
}
"""
        with tempfile.TemporaryDirectory(prefix="merc-boid-option-") as directory:
            folder = Path(directory)
            source, exe = folder/"test.c", folder/"test.exe"
            source.write_text(fixture,encoding="utf-8")
            subprocess.run([compiler,"-std=c11","-O1", "-I"+str(ROOT/"ports/mercenaries/src"),
                            "-I"+str(ROOT/"src/input"),"-I"+str(ROOT/"include"),"-I"+str(ROOT/"src"),
                            str(source),"-o",str(exe),"-lm"],check=True)
            subprocess.run([str(exe),str(folder)],check=True)

if __name__ == "__main__":
    unittest.main()
