"""Execute retail head/vertical-aim clamp blocks against source StepToValue."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class HeadAimSmoothingTests(unittest.TestCase):
    def test_real_clamps_and_old_increasing_branch_negative_control(self):
        source=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0001.c').read_text(encoding='utf-8')
        head=source[source.index('loc_00062600: ;'):source.index('loc_00062634: ;')]
        head=head[head.index('    if ('):]+ '\nloc_00062634: ;\nreturn xmm0;\n}'
        vertical=source[source.index('loc_00062830: ;'):source.index('loc_0006287D: ;')]
        vertical=vertical[vertical.index('    MEMF(esp + 4) = xmm0;'):]+ '\nloc_0006287D: ;\nassert(slot==xmm0);return xmm0;\n}'
        prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define recomp_xmm_copy(a,b) memcpy(a,b,sizeof(a))
#define MEMF(a) slot
static float reference(float n,float target,float step){
 if(n<target){n+=step;if(n>target)n=target;}
 else if(n>target){n-=step;if(n<target)n=target;}
 return n;
}
'''
        def wrapper(name):
            return f'\nstatic float {name}(float n,float t,float s){{\nfloat xmm0v[4]={{n}},xmm1v[4]={{t}},xmm2v[4]={{s}};float slot=n;\n'
        # The old merged-COMISS path tested the reversed operands after addition.
        broken=head.replace('(xmm0 <= xmm1))) goto loc_00062634;',
                            '(xmm1 <= xmm0))) goto loc_00062634;',1)
        tail=r'''
int main(void){
 const float steps[]={0.0f,0.00001f,0.01f,0.04f,0.13333333f,0.16666667f,1.0f,10.0f};
 unsigned cases=0;
 for(int a=-25;a<=25;a++)for(int b=-25;b<=25;b++)for(unsigned i=0;i<8;i++){
  float n=a/10.0f,t=b/10.0f,s=steps[i],want=reference(n,t,s);
  float h=head(n,t,s),v=vertical(n,t,s);
  assert(!memcmp(&h,&want,4));assert(!memcmp(&v,&want,4));cases+=2;
 }
 for(unsigned which=0;which<2;which++){
  float (*fn)(float,float,float)=which?head:vertical;float n=-2.0f;
  for(unsigned i=0;i<360;i++){float next=fn(n,0.25f,5.0f/60.0f);
   assert(next>=n && next<=0.25f);n=next;}assert(n==0.25f);
  for(unsigned i=0;i<360;i++){float next=fn(n,-0.5f,8.0f/60.0f);
   assert(next<=n && next>=-0.5f);n=next;}assert(n==-0.5f);
 }
 assert(broken_head(-1.0f,1.0f,0.1f)!=reference(-1.0f,1.0f,0.1f));
 assert(broken_head(0.9f,1.0f,0.5f)>1.0f);
 printf("%u source-equivalent clamp cases; convergence and old-branch negative control pass\n",cases);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-head-clamp-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prefix+wrapper('head')+head+wrapper('vertical')+vertical+wrapper('broken_head')+broken+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            run=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
            print(run.stdout.decode().strip())


if __name__=='__main__':unittest.main()
