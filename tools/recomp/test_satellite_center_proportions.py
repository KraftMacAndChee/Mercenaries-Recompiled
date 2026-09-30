"""Execute actual satellite call-site patches and check unaffected HUD arguments."""
from pathlib import Path
import re, runpy, shutil, subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[2]
ns = runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))
patches = ns['SATELLITE_CENTER_PATCHES']
class SatelliteCenterTests(unittest.TestCase):
    def test_scoped_reproducible_patches(self):
        source = (ROOT/'ports/mercenaries/src/recomp/gen/recomp_0004.c').read_text(encoding='utf-8-sig')
        # Diagnostics sit at post-call labels also used by these geometry patches.
        # Remove only the known opt-in checkpoints when reconstructing the baseline.
        for checkpoint in ns['PATCHES']:
            if checkpoint.name.startswith('satellite color checkpoint '):
                source=source.replace(checkpoint.after,checkpoint.before)
        baseline=source
        for p in patches: baseline=baseline.replace(p.after,p.before)
        modified=baseline
        for p in patches:
            self.assertEqual(modified.count(p.before),1,p.name)
            modified=modified.replace(p.before,p.after)
        pattern=r'void sub_000FA620\(void\)\n\{.*?\n\}'
        old=re.search(pattern,baseline,re.S); new=re.search(pattern,modified,re.S)
        self.assertEqual(baseline[:old.start()],modified[:new.start()])
        self.assertEqual(baseline[old.end():],modified[new.end():])
        restored=modified
        for p in patches:
            self.assertEqual(restored.count(p.after),1)
            restored=restored.replace(p.after,p.before)
        self.assertEqual(restored,baseline)
        self.assertEqual(len(patches),16)
        self.assertIn('Patch-Generated.py',(ROOT/'ports/mercenaries/scripts/Recompile.ps1').read_text(encoding='utf-8-sig'))
    def test_actual_call_arguments_all_aspects(self):
        options=(ROOT/'ports/mercenaries/src/recomp_options.c').read_text(encoding='utf-8-sig')
        # Compile only these production geometry functions, not adjacent option accessors.
        helpers='\n'.join(re.search(
            r'^(?:static )?float '+name+r'\([^\n]*\) \{.*?^\}', options, re.M | re.S).group(0)
            for name in ('satellite_center_horizontal_scale',
                         'recomp_options_satellite_center_x',
                         'recomp_options_satellite_center_width', 'recomp_options_ui_x'))
        fixture=r'''
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <assert.h>
static unsigned aspect;
static void recomp_options_presentation_aspect(uint32_t *w,uint32_t *h) {
    static const uint32_t ratios[4][2]={{4,3},{16,9},{16,10},{21,9}};
    *w=ratios[aspect][0]; *h=ratios[aspect][1];
}
'''+helpers+r'''
static uint32_t memory[256],esp,eax,ecx,esi,captured[8],count;
#define MEMF(a) (*(float *)((unsigned char *)memory+(a)))
#define PUSH32(s,v) ((s)-=4, memory[(s)/4]=(uint32_t)(v))
static void capture(unsigned n) { count=n; memcpy(captured,&memory[esp/4+1],n*4); }
static void sub_0020C980(void) { capture(8); }
static void sub_0020C160(void) { capture(2); }
static void sub_000FA110(void) { capture(4); }
static void reset(void) {
    memset(memory,0x55,sizeof(memory)); esp=512; eax=0x12345678; esi=0x98765432; ecx=0;
    MEMF(esp+24)=193.0f; MEMF(esp+28)=146.0f;
}
static float asfloat(uint32_t v) { float f; memcpy(&f,&v,4); return f; }
static void near(float a,float b) { assert(fabsf(a-b)<0.0001f); }
'''
        for i,p in enumerate(patches):
            fixture+=f'\nstatic void before_{i}(void) {{\n{p.before}\n}}\n'
            fixture+=f'\nstatic void after_{i}(void) {{\n{p.after}\n}}\n'
        fixture+='\nint main(void) {\n'
        for i,p in enumerate(patches):
            addr=p.name.split()[-1]
            allowed={'000FA6C1':[2],'000FA6E7':[0,2],'000FA76F':[0,2],'000FA736':[2]}.get(addr,[0])
            fixture+=f'''
    for(aspect=0;aspect<4;aspect++) {{
        uint32_t old[8],old_count,old_esp,old_ecx;
        reset(); before_{i}(); memcpy(old,captured,sizeof(old));
        old_count=count; old_esp=esp; old_ecx=ecx;
        reset(); after_{i}();
        assert(count==old_count && esp==old_esp && ecx==old_ecx);
        assert(eax==0x12345678 && esi==0x98765432);
        for(unsigned j=0;j<count;j++) {{
            if(aspect==0 || !({' || '.join('j=='+str(j) for j in allowed)}))
                assert(captured[j]==old[j]);
        }}
'''
            for j in allowed:
                if addr=='000FA6E7' and j==2:
                    expr='512.0f-recomp_options_satellite_center_x(asfloat(old[0]))'
                elif addr in ('000FA76F','000FA736') and j==2:
                    expr=f'recomp_options_satellite_center_width(asfloat(old[{j}]))'
                else: expr=f'recomp_options_satellite_center_x(asfloat(old[{j}]))'
                fixture+=f'        near(asfloat(captured[{j}]),{expr});\n'
            fixture+='    }\n'
        fixture+=r'''
    for(aspect=0;aspect<4;aspect++) {
        uint32_t w,h; recomp_options_presentation_aspect(&w,&h);
        float expansion=((float)w/h)/(4.0f/3.0f);
        near(recomp_options_satellite_center_x(256),256);
        near(recomp_options_satellite_center_width(193)*expansion,193);
        near((recomp_options_satellite_center_x(356.5f)-recomp_options_satellite_center_x(155.5f))*expansion,201);
        float left=recomp_options_satellite_center_x(159.5f);
        float right=recomp_options_satellite_center_x(352.5f);
        near(left+recomp_options_satellite_center_width(193),right);
        near(left,512-right);
    }
    aspect=1;
    near(recomp_options_satellite_center_x(159.5f),183.625f);
    near(recomp_options_satellite_center_width(193),144.75f);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='satellite-center-') as directory:
            path=Path(directory); (path/'test.c').write_text(fixture)
            compiler=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
            subprocess.run([compiler,'-std=c11','-fno-strict-aliasing',str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
            subprocess.run([str(path/'test.exe')],check=True)
if __name__=='__main__': unittest.main()
