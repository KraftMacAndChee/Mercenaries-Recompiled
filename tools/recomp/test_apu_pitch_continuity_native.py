"""Check the production resampler's pitch and continuity against analytic tones."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]
class PitchContinuityTests(unittest.TestCase):
 def test_rates_blocks_reuse_and_runtime_pitch_changes(self):
  source=(ROOT/'src/apu/apu_shim.h').read_text()
  part=source[source.index('#define SRC_SINC_FASTEST'):source.index('/* Float-to-short conversion')]
  program=r'''
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d\n",__LINE__);return 1;} } while(0)
'''+part+r'''
typedef struct { unsigned long long cursor; int block,channels; float samples[64]; } Input;
static double wave(double x,int ch) {return 0.7*sin(x*0.071+ch*0.4);}
static long supply(void* p,float** out) {
 Input* i=p; for(int f=0;f<i->block;f++) for(int c=0;c<i->channels;c++) i->samples[f*i->channels+c]=(float)wave((double)(i->cursor+f),c);
 i->cursor+=i->block;*out=i->samples;return i->block;
}
int main(void) {
 const int pitches[]={-8192,-4597,-4096,-2048,0,2048,4096,8192};
 const int blocks[]={1,7,16,32}; const int requests[]={1,13,32,127}; unsigned cases=0;
 for(int channels=1;channels<=2;channels++) for(unsigned b=0;b<4;b++) for(unsigned q=0;q<4;q++) {
  Input in={0};in.channels=channels;in.block=blocks[b];int error=0;
  SRC_STATE* s=src_callback_new(supply,SRC_SINC_FASTEST,channels,&error,&in);CHECK(s&&!error);
  double expected=0;
  for(unsigned p=0;p<8;p++) {
   double ratio=1.0/pow(2.0,pitches[p]/4096.0);double step=1.0/ratio;
   for(int n=0;n<64;n++) {
    float out[254];int count=src_callback_read(s,ratio,requests[q],out);CHECK(count==requests[q]);
    for(int f=0;f<count;f++) {
     double whole=floor(expected),frac=expected-whole;
     for(int c=0;c<channels;c++) {double reference=wave(whole,c)+(wave(whole+1,c)-wave(whole,c))*frac;CHECK(fabs(out[f*channels+c]-reference)<0.00002);}
     expected+=step;
    }
   }
   cases++;
  }
  src_reset(s);in.cursor=0;float out[2];CHECK(src_callback_read(s,1.0,1,out)==1);CHECK(fabs(out[0])<1e-6);src_delete(s);
 }
 printf("%u pitch/chunk/channel cases passed, including live ratio changes and voice reuse\n",cases);return 0;
}
'''
  compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
  with tempfile.TemporaryDirectory(prefix='mercs-pitch-') as d:
   p=Path(d);(p/'test.c').write_text(program)
   subprocess.run([compiler,'-O2','-std=c11',str(p/'test.c'),'-lm','-o',str(p/'test.exe')],check=True)
   subprocess.run([str(p/'test.exe')],check=True)
if __name__=='__main__':unittest.main()
