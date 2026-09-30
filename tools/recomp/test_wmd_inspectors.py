"""Run the retail mission in pinned public Lua 5.0.3 with explicit host event/HUD fixtures.

Retail script contents are read from the user's disc assets, never embedded here.
The native preparation test also verifies unconditional application and the chunk fingerprint.
"""
from pathlib import Path
import json
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.diagnostics.inspect_retail_script import read_script

PORT = ROOT / "ports/mercenaries"


class InspectorMission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = ROOT / "game_files/mercenaries-retail/DATAxbox/assets.dsk"
        compiler = os.environ.get("CC") or shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not archive.is_file() or not Path(compiler).is_file():
            raise RuntimeError("Requires user retail assets and GCC (CC)")
        cls.temp = tempfile.TemporaryDirectory(prefix="wmd-inspectors-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        public_lua = ROOT / "third_party/lua-5.0.3"
        upstream = json.loads((public_lua / "UPSTREAM.json").read_text())
        for name, digest in upstream["files"].items():
            if hashlib.sha256((public_lua / name).read_bytes()).hexdigest() != digest:
                raise ValueError("Public Lua input changed: " + name)
        cls.lua = cls.work / "lua503.exe"
        subprocess.run([compiler, "-O1", "-w", "-I" + str(public_lua / "include"),
                        "-I" + str(public_lua / "src"),
                        str(public_lua / "src/lua/lua.c"),
                        *map(str, sorted((public_lua / "src").glob("*.c"))),
                        *map(str, sorted((public_lua / "src/lib").glob("*.c"))),
                        "-o", str(cls.lua)], check=True)
        evidence = json.loads((ROOT / "tools/recomp/fixtures/wmd-retail-evidence.json").read_text())
        if hashlib.sha256(archive.read_bytes()).hexdigest() != evidence["retail_assets_sha256"]:
            raise ValueError("Unsupported retail script archive")
        scripts = {}
        for name, expected in evidence["scripts"].items():
            source = read_script(archive, name)
            functions = {m.group(1): source[:m.start()].count("\n") + 1
                         for m in re.finditer(r"(?:local\s+)?function\s+([\w.:]+)\s*\(", source)}
            actual = {"decoded_sha256": hashlib.sha256(source.encode("ascii")).hexdigest(),
                      "decoded_bytes": len(source), "functions": functions}
            if actual != expected:
                raise ValueError("Retail mission contract changed: " + name)
            scripts[name] = source
        cls.original = scripts["nw_allies1"]
        for name in ("mission_objective", "mission_objectivedeliver"):
            (cls.work / (name + ".lua")).write_text(scripts[name], newline="\n")
        (cls.work / "retail.lua").write_bytes(cls.original.encode("ascii"))
        correction = "\n" + (PORT / "src/wmd_inspectors.lua").read_text()
        (cls.work / "wmd_inspectors_script.h").write_text(
            'static const char wmd_inspectors_script[] =\n' +
            '\n'.join(json.dumps(line) for line in correction.splitlines(keepends=True)) + ';\n')
        (cls.work / "prepare.c").write_text(r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mission_script_fixes.h"
#include "recomp_original_bugs.h"
/* Mission correctness must not consult the original-bug policy. */
int recomp_original_bug_fix_enabled(recomp_original_bug_fix fix) {
    assert(0 && "Inspector correction consulted OG Bugs");return 0;
}
int main(int argc,char **argv) {
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb");assert(f);
    char input[25000],original[25000];size_t n=fread(input,1,sizeof(input),f),out;
    fclose(f);memcpy(original,input,n);
    assert(n==24493);
    char *patch=recomp_patch_mission_script(input,n,&out);
    assert(patch && out>n && !memcmp(patch,input,n) && patch[out]==0);
    assert(!memcmp(input,original,n));
    f=fopen(argv[2],"wb");assert(f);assert(fwrite(patch,1,out,f)==out);fclose(f);free(patch);
    patch=recomp_patch_mission_script(input,n-1,&out);
    assert(patch && patch[n-1]=='\n' && patch[out]==0);free(patch);
    assert(!recomp_patch_mission_script(input,n-2,&out));
    input[0]^=1;assert(!recomp_patch_mission_script(input,n,&out));
    assert(!recomp_patch_mission_script(NULL,n,&out));
    puts("PASS: exact retail fingerprint, unconditional correction, source ownership, full and loader lengths");
    return 0;
}
""")
        cls.compiler = compiler
        subprocess.run([compiler, "-std=c11", "-O2", "-I", str(PORT / "src"), "-I", str(cls.work),
                        str(cls.work / "prepare.c"), str(PORT / "src/mission_script_fixes.c"),
                        "-o", str(cls.work / "prepare.exe")], check=True)
        subprocess.run([str(cls.work / "prepare.exe"), str(cls.work / "retail.lua"),
                        str(cls.work / "patched.lua")], check=True)

    def scenario(self, source, patched=True):
        # Same chunk scope lets assertions inspect mission locals without changing production APIs.
        mission = (self.work / ("patched.lua" if patched else "retail.lua")).read_text()
        setup = '\nmission_ObjectiveDeliver=setup_delivery\nTargetActorFollowingHero=noop\nreset_mission()\npost_InitialNotifications()\n'
        script = self.work / "scenario.lua"
        script.write_text(mission + '\nfunction run_scenario()\n' + setup + source +
                          '\nprint("SCENARIO PASSED")\nend\n', newline="\n")
        runner = self.work / "run.lua"
        runner.write_text("run_scenario()\n")
        # The public CLI executes one script; subsequent arguments are Lua's
        # arg table, not more chunks. Load every fixture explicitly in order.
        bootstrap = self.work / "bootstrap.lua"
        chunks = [ROOT / "tools/recomp/fixtures/wmd_inspectors_host.lua", script,
                  self.work / "mission_objective.lua",
                  self.work / "mission_objectivedeliver.lua", runner]
        bootstrap.write_text("\n".join("dofile(" + json.dumps(p.as_posix()) + ")" for p in chunks))
        result = subprocess.run([str(self.lua), str(bootstrap)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SCENARIO PASSED", result.stdout, result.stdout + result.stderr)

    def test_native_protected_loader_and_buffer_lifetime(self):
        from generated_test_utils import generated_text_containing
        generated = generated_text_containing("void sub_001DC520(void)")
        body = re.search(r"void sub_001DC520\(void\)\n\{.*?\n\}", generated, re.S)[0]
        manual = (PORT / "src/recomp_manual.c").read_text()
        bridge = re.search(r"uint32_t recomp_prepare_mission_script\(.*?\n\}", manual, re.S)[0]
        fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mission_script_fixes.h"
#include "recomp_original_bugs.h"
static unsigned char memory[0x4000000];
static uint32_t eax,ecx,edx,esi,edi,esp;
static int og_bugs,parse_error,run_error,nested,active,allocations,executions;
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define PUSH32(s,v) do {uint32_t value=(v);(s)-=4;MEM32(s)=value;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
static void *guest_ptr(uint32_t a){return memory+a;}
int recomp_original_bug_fix_enabled(recomp_original_bug_fix fix){assert(0 && "Mission fix must not consult OG Bugs");return !og_bugs;}
uint32_t recomp_title_heap_allocate(uint32_t size){assert(!active);active=1;allocations++;return 0x100000;}
void recomp_title_heap_free(uint32_t a){assert(a==0x100000 && active);active=0;}
static void recomp_shl_vm_checkpoint(uint32_t a,uint32_t b,uint32_t c,uint32_t d){}
static void recomp_lua_pcall_checkpoint(uint32_t a,uint32_t b,uint32_t c){}
void sub_001DC520(void);
static void sub_001DD610(void){
 uint32_t data=MEM32(esp+12),source=MEM32(data),size=MEM32(data+4);
 assert(MEM32(esp+8)==0x1DC460);
 if(nested){assert(source==0x30000 && size==4 && !active);}
 else {assert(source==0x100000 && size>24493 && active);assert(!memcmp(memory+source,memory+0x10000,24492));}
 eax=parse_error;esp+=4;
}
static void sub_001DD5A0(void){
 assert(!active);executions++;
 if(!nested){
  uint32_t outer=esp;nested=1;
  esp-=128;MEM32(esp+4)=0x90000;MEM32(esp+8)=0x30000;MEM32(esp+12)=4;
  sub_001DC520();assert(esp==outer-124 && MEM32(outer-120)==0x30000);
  esp=outer;nested=0;
 }
 eax=run_error;esp+=4;
}
static void sub_001DC4B0(void){assert(!active);esp+=4;}
""" + bridge + body + r"""
int main(int argc,char **argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);assert(fread(memory+0x10000,1,24493,f)==24493);fclose(f);
 for(og_bugs=0;og_bugs<2;og_bugs++)for(parse_error=0;parse_error<2;parse_error++)for(run_error=0;run_error<2;run_error++){
  allocations=executions=active=nested=0;
  esp=0x800000;esi=0x1111;edi=0x2222;
  MEM32(esp+4)=0x90000;MEM32(esp+8)=0x10000;MEM32(esp+12)=24492;MEM32(esp+16)=0;
  sub_001DC520();
  assert(esp==0x800004 && esi==0x1111 && edi==0x2222 && !active);
  assert(MEM32(0x800008)==0x10000 && MEM32(0x80000C)==24492);
  assert(allocations==1 && executions==(parse_error?0:2));
  assert(eax==(parse_error?parse_error:run_error));
 }
 puts("PASS: lifted loader preserves ABI and arguments; temporary freed before execution, including errors and nested loads");
 return 0;
}
"""
        (self.work / "loader.c").write_text(fixture, newline="\n")
        subprocess.run([self.compiler, "-std=c11", "-O2", "-I", str(PORT / "src"), "-I", str(self.work),
                        str(self.work / "loader.c"), str(PORT / "src/mission_script_fixes.c"),
                        "-o", str(self.work / "loader.exe")], check=True)
        subprocess.run([str(self.work / "loader.exe"), str(self.work / "retail.lua")], check=True)

    def test_retail_failure_and_correction(self):
        sequence = """
kill('a1_inspector1') kill('a1_inspector2')
assert(trays[tHUD.Inspectors][1].nValue==2)
StartInspection('a1_loc_site1','a1_inspector3')
assert(not mission_tTargets[Utility_GetActorAsInt("a1_inspector3")].destroyedHandle)
kill('a1_inspector3') kill('a1_inspector4')
"""
        self.scenario(sequence + "assert(nNumInspectors==2 and not failed)", patched=False)
        self.scenario(sequence + """
Recomp_WmdInspectorsPoll()
assert(nNumInspectors==0 and trays[tHUD.Inspectors][1].nValue==0 and failures==1)
Recomp_WmdInspectorsPoll()
OnInspectorDelivered('a1_inspector4',false,true,'Escort',nil,false)
assert(failures==1 and nNumInspectors==0)
""")

    def test_missing_damage_notification_and_living_actor(self):
        self.scenario("""
-- No engine callback is delivered (including an explosive kill with a removed spore).
dead.a1_inspector1=true removed.a1_inspector1=true
Recomp_WmdInspectorsPoll()
assert(nNumInspectors==3 and trays[tHUD.Inspectors][1].nValue==3)
OnInspectorDelivered('a1_inspector1',false,true,'Escort',nil,false)
assert(nNumInspectors==3)
-- A living inspector in a different animation is not a death.
Recomp_WmdInspectorsPoll() assert(nNumInspectors==3)
OnInspectorDelivered('player0',false,true,'Escort',nil,false)
OnInspectorDelivered(0,false,true,'Escort',nil,false)
assert(nNumInspectors==3)
""")

    def test_inspection_arrivals_and_stale_hud(self):
        self.scenario("""
StartInspection('a1_loc_site1','a1_inspector1')
OnInspecterAtPost('a1_inspector1','a1_loc_site1_i1',2,true,true)
StartInspection('a1_loc_site1','a1_inspector2')
OnInspecterAtPost('a1_inspector2','a1_loc_site1_i2',2,true,true)
OnInspecterAtPost('a1_inspector2','a1_loc_site1_i2',2,true,true)
assert(trays[tHUD[sCurrentSite]][1].nDelta==10)
dead.a1_inspector2=true Recomp_WmdInspectorsPoll()
assert(nNumInspectors==3 and trays[tHUD[sCurrentSite]][1].nDelta==5)
HUD_RemoveTrayDisplay(tHUD[sCurrentSite])
OnInspecterAtPost('a1_inspector3','a1_loc_site1_i3',2,true,true)
assert(nNumInspectors==3)
""")

    def test_three_inspections_and_normal_return(self):
        self.scenario("""
BlipSites()
for site=1,3 do
 local location='a1_loc_site'..site
 for i=1,4 do
  local name='a1_inspector'..i
  StartInspection(location,name)
  OnInspecterAtPost(name,location..'_i'..i,2,true,true)
 end
 assert(trays[tHUD[location]][1].nDelta==20)
 local late=OnInspecterAtPost
 OnInspectionComplete(tHUD[location],HUD.STATE.LIMIT)
 late('a1_inspector1',location..'_i1',2,true,true)
 OnInspectionComplete_UpdateHUD()
end
assert(mission_tObjectives.Return.targetsRemaining==4)
OnInspectorDeliveredToHQ('a1_inspector1',true,true,'Return',nil,true)
for i=1,4 do OnInspectorAtHQ('a1_inspector'..i,'a1_loc_inspectorexit',2,true,true) end
assert(nNumInspectors==4 and completed==1 and not failed)
""")

    def test_cancel_before_every_inspector_arrives(self):
        self.scenario("""
StartInspection('a1_loc_site1','a1_inspector1')
OnInspecterAtPost('a1_inspector1','a1_loc_site1_i1',2,true,true)
CancelInspection('a1_loc_site1')
OnInspecterAtPost('a1_inspector2','a1_loc_site1_i2',2,true,true)
assert(tHUD.a1_loc_site1==nil)
StartInspection('a1_loc_site1','a1_inspector2')
OnInspecterAtPost('a1_inspector2','a1_loc_site1_i2',2,true,true)
assert(trays[tHUD.a1_loc_site1][1].nDelta==5)
""")

    def test_hq_arrivals_are_not_deaths(self):
        self.scenario("""
OnInspectorDeliveredToHQ('a1_inspector1',true,true,'Return',nil,true)
for i=1,4 do
 local name='a1_inspector'..i
 InspectorEntersHQ(name)
 OnInspectorAtHQ(name,'a1_loc_inspectorexit',2,true,true)
 Recomp_WmdInspectorsPoll()
 OnInspectorAtHQDied(name)
end
assert(nNumInspectors==4 and nNumInspectorsInHQ==4 and completed==1 and not failed)
""")

    def test_hq_death_then_arrival(self):
        self.scenario("""
OnInspectorDeliveredToHQ('a1_inspector1',true,true,'Return',nil,true)
dead.a1_inspector1=true
OnInspectorAtHQDied('a1_inspector1')
OnInspectorAtHQDied('a1_inspector1')
OnInspectorAtHQ('a1_inspector1','a1_loc_inspectorexit',2,true,true)
assert(nNumInspectors==3 and nNumInspectorsInHQ==0)
for i=2,4 do OnInspectorAtHQ('a1_inspector'..i,'a1_loc_inspectorexit',2,true,true) end
assert(completed==1 and not failed)
""")

    def test_hq_all_dead_fails_without_completion(self):
        self.scenario("""
OnInspectorDeliveredToHQ('a1_inspector1',true,true,'Return',nil,true)
for i=1,4 do dead['a1_inspector'..i]=true end
Recomp_WmdInspectorsPoll()
assert(nNumInspectors==0 and failures==1 and completed==0)
""")

    def test_missed_return_death_updates_delivery_objective(self):
        self.scenario("""
mission_RemoveObjective('Escort')
setup_delivery{objectiveName='Return',callbackFunc=OnInspectorDeliveredToHQ,
 destLoc='a1_loc_return',actorTable={{actorName='a1_inspector1'},{actorName='a1_inspector2'},
 {actorName='a1_inspector3'},{actorName='a1_inspector4'}}}
mission_tObjectives.Return.targetsRemaining=1 -- the other three have reached the return area
removed.a1_inspector1=true dead.a1_inspector1=true
Recomp_WmdInspectorsPoll()
assert(nNumInspectors==3 and mission_tObjectives.Return.targetsRemaining==0)
TargetActorDestroyed('a1_inspector1') -- a queued old event cannot decrement it twice
assert(mission_tObjectives.Return.targetsRemaining==0)
for i=2,4 do OnInspectorAtHQ('a1_inspector'..i,'a1_loc_inspectorexit',2,true,true) end
assert(completed==1 and not failed)
""")

    def test_cleanup_cancels_monitor(self):
        self.scenario("""
pre_MissionCleanup()
for i=1,4 do dead['a1_inspector'..i]=true end
Recomp_WmdInspectorsPoll()
assert(nNumInspectors==4 and not failed)
for _,e in pairs(events) do assert(e[1]~='Recomp_WmdInspectorsPoll') end
""")

if __name__ == "__main__":
    unittest.main()
