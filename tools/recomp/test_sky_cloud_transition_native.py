"""Exercise the optional cloud/empty weather transition and original behavior."""
from pathlib import Path
import shutil, subprocess, tempfile, runpy
ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / 'ports/mercenaries'

def main():
    compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
    fixture = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "recomp_original_bugs.c"
static int original;
int recomp_options_og_bugs(void) { return original; }
static void check(unsigned a,unsigned b,float w,float expected,unsigned bound) {
    unsigned reset_a=123,reset_b=0;
    recomp_original_bug_cloud_params(&reset_a,&reset_b);
    float colors[38], before[38];
    for(unsigned i=0;i<38;i++)colors[i]=(float)i*.013f;
    memcpy(before,colors,sizeof colors);
    recomp_original_bug_sky_blend(w);
    recomp_original_bug_cloud_params(&a,&b);
    recomp_original_bug_cloud_colors(colors+1);
    if(bound)assert(a==bound && b==bound);
    for(unsigned i=0;i<38;i++) {
        float want=before[i];
        if(i>=1 && i<=36 && (i-1)%4==3)want*=expected;
        assert(fabsf(colors[i]-want)<1e-7f);
    }
}
int main(void) {
    const unsigned cloud=0x43404EB9u;
    for(unsigned i=0;i<=1000;i++) {
        float w=i/1000.f;
        check(0,cloud,w,w,cloud);check(cloud,0,w,1-w,cloud);
        check(cloud,cloud,w,1,cloud);check(0,0,w,0,0);
    }
    check(123,cloud,.8f,1,0);check(cloud,456,.8f,1,0);
    check(0,cloud,NAN,1,0);check(0,cloud,-.01f,1,0);check(0,cloud,1.01f,1,0);
    original=1;
    for(unsigned i=0;i<2;i++) {
        unsigned a=i?cloud:0,b=i?0:cloud,oa=a,ob=b;
        recomp_original_bug_sky_blend(.8f);recomp_original_bug_cloud_params(&a,&b);
        assert(a==oa && b==ob);check(a,b,.8f,1,0);
    }
    original=0;check(0,cloud,.86f,.86f,cloud);
    /* Turning back before a fade completes must not snap to full coverage. */
    unsigned a=0,b=cloud;float colors[36];
    recomp_original_bug_sky_blend(.8f);recomp_original_bug_cloud_params(&a,&b);
    a=cloud;b=cloud;recomp_original_bug_sky_blend(1);recomp_original_bug_cloud_params(&a,&b);
    for(unsigned i=0;i<36;i++)colors[i]=1;
    recomp_original_bug_cloud_colors(colors);assert(fabsf(colors[3]-.8f)<1e-6f);
    a=cloud;b=cloud;recomp_original_bug_sky_blend(.5f);recomp_original_bug_cloud_params(&a,&b);
    for(unsigned i=0;i<36;i++)colors[i]=1;
    recomp_original_bug_cloud_colors(colors);assert(fabsf(colors[3]-.9f)<1e-6f);
    /* A reversal after the engine selected the empty outgoing texture must
     * retain the partially visible layer until it fades away. */
    check(cloud,0,.8f,.2f,cloud);
    a=0;b=0;recomp_original_bug_sky_blend(1);recomp_original_bug_cloud_params(&a,&b);
    assert(a==cloud && b==cloud);
    for(unsigned i=0;i<36;i++)colors[i]=1;
    recomp_original_bug_cloud_colors(colors);assert(fabsf(colors[3]-.2f)<1e-6f);
    a=0;b=0;recomp_original_bug_sky_blend(.5f);recomp_original_bug_cloud_params(&a,&b);
    for(unsigned i=0;i<36;i++)colors[i]=1;
    recomp_original_bug_cloud_colors(colors);assert(fabsf(colors[3]-.1f)<1e-6f);
    a=0;b=0;recomp_original_bug_sky_blend(0);recomp_original_bug_cloud_params(&a,&b);
    assert(a==0 && b==0);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as td:
        c=Path(td)/'test.c';exe=Path(td)/'test.exe';c.write_text(fixture)
        subprocess.run([compiler,'-std=c11','-O2','-I'+str(PORT/'src'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    patches=runpy.run_path(str(PORT/'scripts/Patch-Generated.py'))['PATCHES']
    generated=(PORT/'src/recomp/gen/recomp_0006.c').read_text()
    for patch in patches:
        if patch.name.startswith('Optional cloud transition'):
            assert generated.count(patch.after)==1,patch.name
    print('Cloud fade: 4,004 transition cases; original policy, unsupported assets, invalid blends and color bounds pass.')
if __name__=='__main__':main()
