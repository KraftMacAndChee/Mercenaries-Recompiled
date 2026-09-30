"""Exercise OG Bugs persistence and the lifted managed-cue alias/reuse path."""
from pathlib import Path
import os
import re
import runpy
import shutil
import subprocess
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports/mercenaries"


class OriginalBugs(unittest.TestCase):
    def test_policy_and_managed_cue(self):
        compiler = os.environ.get("CC") or shutil.which("gcc")
        if not compiler and Path("C:/MinGW/bin/gcc.exe").is_file():
            compiler = "C:/MinGW/bin/gcc.exe"
        if not compiler:
            self.skipTest("Set CC to a GCC-compatible C compiler")
        source = generated_text_containing("void sub_001FF6E0(void)")
        body = re.search(r"void sub_001FF6E0\(void\)\n\{.*?\n\}", source, re.S)[0]
        patches = runpy.run_path(str(PORT / "scripts/Patch-Generated.py"))["PATCHES"]
        patch = next(p for p in patches if p.name == "Optional NW Mafia 2 managed music cue alias")
        self.assertIn(patch.after, body)
        fixture = r"""
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static char runtime[MAX_PATH];
static DWORD module_path(HMODULE m,LPSTR out,DWORD size) {
    (void)m; snprintf(out,size,"%s",runtime); return (DWORD)strlen(out);
}
#define GetModuleFileNameA module_path
#define _snprintf_s(out,size,count,...) snprintf(out,size,__VA_ARGS__)
#define strcpy_s(out,size,in) snprintf(out,size,"%s",in)
#define strcat_s(out,size,in) snprintf(out+strlen(out),size-strlen(out),"%s",in)
#include "recomp_options.c"
#include "recomp_original_bugs.c"
static unsigned char memory[0x700000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp;
static unsigned creates,plays,updates,lookups,notified;
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define g_esp esp
#define PUSH32(s,v) do { uint32_t value=(v); (s)-=4; MEM32(s)=value; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define RECOMP_ICALL_SAFE(target,saved) virtual_call(target)
#define CUE 0x11000u
#define CONFIG 0x12000u
#define VTABLE 0x13000u
#define HANDLE 0x1234u
static void recomp_xact_managed_checkpoint(uint32_t a,uint32_t b,uint32_t c,uint32_t d) {}
static void sub_001FE410(void) {
    eax=MEM32(esp+4)==HANDLE ? CUE : 0; esp+=8;
}
static void sub_001FF690(void) {
    lookups++;
    eax=MEM32(esp+4)==0xC69A677Bu ? CONFIG : 0; esp+=4;
}
static void sub_001FF060(void) {
    creates++;MEM32(MEM32(esp+4))=HANDLE;eax=CUE;esp+=8;
}
static void sub_001FEB90(void) { updates++;esp+=8; }
static void virtual_call(uint32_t target) {
    assert(target==1 || target==2);
    if(target==2) { plays++;MEM32(CUE+0x38)=2; }
    esp+=4;
}
static void changed(uint32_t bits) { notified|=bits; }
""" + body + r"""
static uint32_t play(uint32_t handle,uint32_t name) {
    esp=0x20000;ecx=0x111;ebx=0x222;esi=0x333;edi=0x444;
    MEM32(esp+4)=handle;MEM32(esp+8)=name;MEM32(esp+12)=0;
    sub_001FF6E0();
    assert(esp==0x20004 && ebx==0x222 && esi==0x333 && edi==0x444);
    return eax;
}
int main(int argc,char **argv) {
    assert(argc==2);snprintf(runtime,sizeof(runtime),"%s\\runtime.exe",argv[1]);
    MEM32(CUE)=VTABLE;MEM32(VTABLE)=1;MEM32(VTABLE+8)=2;
    MEM32(CONFIG+4)=0xC69A677Bu;
    recomp_options_set_apply_callback(changed);
    assert(recomp_options_og_bugs()==1);
    assert(!recomp_original_bug_fix_enabled(RECOMP_FIX_NW_MAFIA2_MUSIC));
    assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_OG_BUGS_HASH),"OG BUGS: ON"));
    assert(play(0,0xFD50E6A3u)==0 && creates==0 && plays==0);
    recomp_options_begin_edit();recomp_options_adjust(RECOMP_OPTIONS_OG_BUGS_HASH,1);
    assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_OG_BUGS_HASH),"OG BUGS: OFF"));
    assert(play(0,0xFD50E6A3u)==0); /* pending value cannot affect playback */
    recomp_options_cancel_edit();assert(!changed_options());
    recomp_options_adjust(RECOMP_OPTIONS_OG_BUGS_HASH,-1);
    assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_OG_BUGS);
    assert(notified==RECOMP_OPTIONS_CHANGE_OG_BUGS);
    assert(GetPrivateProfileIntA("RecompOptions","OGBugs",-1,g_options.path)==0);
    memset(&g_options,0,sizeof(g_options));assert(!recomp_options_og_bugs());
    assert(recomp_options_apply()==0);
    assert(recomp_original_bug_fix_enabled(RECOMP_FIX_NW_MAFIA2_MUSIC));
    assert(!recomp_original_bug_fix_enabled((recomp_original_bug_fix)99));
    assert(recomp_options_localization_hash(RECOMP_OPTIONS_OG_BUGS_HASH)==0x941EE5E8u);
    assert(play(0,0xFD50E6A3u)==HANDLE && creates==1 && plays==1);
    unsigned prior_lookups=lookups;
    for(unsigned i=0;i<120;i++)assert(play(HANDLE,0xFD50E6A3u)==HANDLE);
    assert(creates==1 && plays==1 && lookups==prior_lookups && updates==121);
    /* Existing real names reuse the same cue; unrelated missing names stay missing. */
    assert(play(HANDLE,0xC69A677Bu)==HANDLE);
    assert(play(0,0x12345678u)==0);
    assert(recomp_original_bug_cue_alias(0)==0);
    assert(recomp_original_bug_cue_alias(0x85E79DB6u)==0x85E79DB6u); /* main menu unchanged */
    assert(recomp_original_bug_cue_alias(0xC69A677Bu)==0xC69A677Bu);
    assert(recomp_original_bug_cue_alias(0x98B54A58u)==0x98B54A58u);
    recomp_options_begin_edit();recomp_options_adjust(RECOMP_OPTIONS_OG_BUGS_HASH,1);
    recomp_options_apply();
    assert(play(HANDLE,0xFD50E6A3u)==0); /* restored retail mismatch */
    assert(play(0,0xFD50E6A3u)==0);
    assert(play(HANDLE,0xC69A677Bu)==HANDLE);
    memset(&g_options,0,sizeof(g_options));assert(recomp_options_og_bugs()==1);
    puts("PASS: OG Bugs default/persistence/apply/cancel, cue resolution, handle reuse and retail behavior");
}
"""
        # Keep executable and INI on the same Windows drive; the real INI API is exercised.
        import tempfile
        with tempfile.TemporaryDirectory(prefix="merc-original-bugs-") as directory:
            folder = Path(directory)
            c = folder / "test.c"
            exe = folder / "test.exe"
            c.write_text(fixture, encoding="utf-8")
            subprocess.run([compiler,"-std=c11","-O1", "-I"+str(PORT/"src"),
                            "-I"+str(ROOT/"src/input"), "-I"+str(ROOT/"include"),
                            "-I"+str(ROOT/"src"),str(c),"-o",str(exe),"-lm"],check=True)
            subprocess.run([str(exe),str(folder)],check=True)


if __name__ == "__main__":
    unittest.main()
