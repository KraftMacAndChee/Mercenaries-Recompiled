"""Verify menu-theme category routing without altering other XACT bank data."""
from pathlib import Path
import subprocess
import tempfile
import shutil

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''#include <assert.h>
#include <stdio.h>
#include "audio_bank_fixes.c"
static uint8_t memory[0x4000000], original[512];
ptrdiff_t g_xbox_mem_offset;
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint8_t *make_bank(void) {
    uint8_t *p = memory + 0x10000;
    memset(p, 0, 512);
    memcpy(p, "SDBK", 4); put16(p+4, 11); put32(p+20, 512);
    put16(p+28, 2); put16(p+30, 2); memcpy(p+40, "shell", 6);
    put16(p+58, 0); put32(p+60, 200); memcpy(p+200, "move", 5);
    put16(p+78, 1); put32(p+80, 220); memcpy(p+220, "shell_stream", 13);
    p[106] = p[126] = 2;
    memcpy(original, p, 512);
    return p;
}
static void unchanged(uint8_t *p, uint32_t size) {
    uint8_t before[512]; memcpy(before, p, 512);
    assert(!fix_menu_music_category(p, size)); assert(!memcmp(before,p,512));
}
int main(int argc, char **argv) {
    g_xbox_mem_offset = (ptrdiff_t)memory;
    uint8_t *p = make_bank();
    recomp_fix_menu_music_bank(0x10000,512);
    assert(p[126] == 0 && p[106] == 2);
    original[126] = 0; assert(!memcmp(original,p,512)); unchanged(p,512);
    p=make_bank(); p[40]='x'; unchanged(p,512);
    p=make_bank(); p[220]='x'; unchanged(p,512);
    p=make_bank(); p[126]=1; unchanged(p,512);
    p=make_bank(); put16(p+58,1); unchanged(p,512); /* Shared mod sound. */
    p=make_bank(); put32(p+60,220); unchanged(p,512); /* Duplicate cue name. */
    p=make_bank(); put16(p+78,2); unchanged(p,512);
    p=make_bank(); put32(p+80,0xfffffff0); unchanged(p,512);
    p=make_bank(); put32(p+80,506); unchanged(p,512);
    p=make_bank(); put16(p+30,65535); unchanged(p,512);
    p=make_bank(); put16(p+28,65535); unchanged(p,512);
    p=make_bank(); p[0]='x'; unchanged(p,512);
    p=make_bank(); put16(p+4,12); unchanged(p,512);
    p=make_bank(); put32(p+20,513); unchanged(p,512);
    p=make_bank(); unchanged(p,55); unchanged(p,511);
    recomp_fix_menu_music_bank(0,512);
    recomp_fix_menu_music_bank(0xfffffff0,512);
    recomp_fix_menu_music_bank(0x10000,0xffffffff);
    recomp_fix_menu_music_bank(0x3fffff0,512);
    g_xbox_mem_offset=0; recomp_fix_menu_music_bank(0x10000,512);
    assert(!memcmp(original,p,512));
    if(argc>1) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        uint8_t data[4096], copy[4096]; size_t n=fread(data,1,sizeof(data),f); fclose(f);
        assert(n==1228); memcpy(copy,data,n); assert(fix_menu_music_category(data,(uint32_t)n));
        unsigned changed=0;
        for(unsigned i=0;i<n;++i) if(data[i]!=copy[i]) {++changed;assert(i==646 && copy[i]==2 && data[i]==0);}
        assert(changed==1); assert(!fix_menu_music_category(data,(uint32_t)n));
    }
    puts("PASS: only shell_stream changes Interface to Music; effects, shared/modded definitions, malformed banks and invalid guest ranges preserved");
    return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="menu-music-category-") as td:
        source=Path(td)/"test.c"; exe=Path(td)/"test.exe"
        source.write_text(FIXTURE,encoding="utf-8")
        subprocess.run([shutil.which("gcc") or "C:/MinGW/bin/gcc.exe","-std=c11","-O2","-I",str(ROOT/"ports/mercenaries/src"),str(source),"-o",str(exe)],check=True)
        bank=ROOT/"game_files/mercenaries-retail/DATAxbox/SOUND/sfx/shell/shell.xsb"
        subprocess.run([str(exe)]+([str(bank)] if bank.exists() else []),check=True)

if __name__ == "__main__":
    main()
