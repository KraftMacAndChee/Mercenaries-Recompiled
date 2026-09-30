"""Legacy INIs cannot re-enable world-distance extension; other settings survive."""
from pathlib import Path
import shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'ports/mercenaries/src/recomp_options.c'
HARNESS = r''' 
#include <assert.h>
int main(int argc, char **argv) {
    char path[MAX_PATH];
    build_config_path(path);
    assert(argc == 2);
    WritePrivateProfileStringA("RecompOptions", "FPSCap", NULL, path);
    WritePrivateProfileStringA("RecompOptions", "DrawDistance", argv[1], path);
    WritePrivateProfileStringA("RecompOptions", "NpcDrawDistance", "2", path);
    WritePrivateProfileStringA("RecompOptions", "60FPS", "0", path);
    recomp_options_init();
    assert(recomp_options_draw_distance() == 0);
    const float distances[] = {0.0f, 1.0f, 64.0f, 150.0f, 512.0f, 2000.0f};
    for (unsigned i=0; i<sizeof(distances)/sizeof(distances[0]); ++i)
        assert(recomp_options_scale_draw_distance(distances[i]) == distances[i]);
    recomp_options_begin_edit();
    assert(!recomp_options_adjust(RECOMP_OPTIONS_DISTANCE_HASH, 1));
    assert(!recomp_options_adjust(RECOMP_OPTIONS_DISTANCE_HASH, -1));
    assert(recomp_options_label(RECOMP_OPTIONS_DISTANCE_HASH) == NULL);
    assert(recomp_options_npc_draw_distance() == 0);
    assert(recomp_options_scale_npc_draw_distance(100.0f, 5) == 100.0f);
    assert(recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH, 1));
    assert(recomp_options_apply() == RECOMP_OPTIONS_CHANGE_FPS);
    assert(recomp_options_fps_cap() == 60);
    assert(GetPrivateProfileIntA("RecompOptions", "DrawDistance", 99, path) == 0);
    assert(GetPrivateProfileIntA("RecompOptions", "NpcDrawDistance", 99, path) == 0);
    assert(recomp_options_scale_draw_distance(150.0f) == 150.0f);
    return 0;
}
'''
def main():
    with tempfile.TemporaryDirectory() as d:
        c=Path(d)/'distance.c'; exe=c.with_suffix('.exe')
        c.write_text('#include <stdio.h>\n#include <string.h>\n#define _TRUNCATE ((size_t)-1)\n#define _snprintf_s(b,n,t,...) snprintf(b,n,__VA_ARGS__)\n#define strcpy_s(b,n,s) ((void)(n),strcpy(b,s))\n#define strcat_s(b,n,s) ((void)(n),strcat(b,s))\n#include "'+SOURCE.as_posix()+'"\n'+HARNESS)
        subprocess.run([shutil.which('gcc'), '-std=c11', '-O2', '-I'+str(ROOT/'src'), '-I'+str(ROOT/'src/input'), str(c), '-o', str(exe)], check=True)
        for value in ('0','1','2','3','999','-1'):
            subprocess.run([str(exe),value],check=True)
    print('PASS: six legacy INIs preserve authored distance, reject removed control, and retain independent NPC/FPS settings')
if __name__=='__main__': main()
