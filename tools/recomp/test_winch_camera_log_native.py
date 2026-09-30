"""Native winch-camera history test: bounded logging and no guest mutations."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[2]

class WinchCameraLogTests(unittest.TestCase):
    def test_history_is_bounded_and_keeps_transient_collision(self):
        header = (ROOT/'ports/mercenaries/src/winch_camera_preview.h').as_posix()
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
typedef unsigned long long ULONGLONG;
static ULONGLONG tick=1000;
static unsigned enabled=1, records, reads;
static uint32_t g_edi=0x30000, g_eax=0x40000;
static unsigned char mem[0x20000], before[0x20000];
static char line[2048];
static uint32_t guest_u32(uint32_t a) {uint32_t v;assert(a+4<=sizeof(mem));reads++;memcpy(&v,mem+a,4);return v;}
static float guest_f32(uint32_t a) {float v;assert(a+4<=sizeof(mem));reads++;memcpy(&v,mem+a,4);return v;}
static void put(uint32_t a,float v){memcpy(mem+a,&v,4);}
static ULONGLONG GetTickCount64(void){return tick;}
static int xbox_preview_log_enabled(void){return enabled;}
static void xbox_preview_log_event(const char *category,const char *format,...){
 assert(!strcmp(category,"winch-camera"));records++;va_list a;va_start(a,format);vsnprintf(line,sizeof(line),format,a);va_end(a);
}
'''+f'#include "{header}"\n'+r'''
enum {S=0x10000,P=0x11000,H=0x12000,F=0x13000,D=0x14000};
static void frame(float collision){
 uint32_t bits;memcpy(&bits,&collision,4);memcpy(before,mem,sizeof(mem));
 preview_winch_pre(S,P);preview_winch_hit(0x9E1D5,S,H);preview_winch_post(0x9E1D5,S,F,D,bits);
 assert(!memcmp(before,mem,sizeof(mem)));
}
int main(void){
 uint32_t vt=0x2E6FD4;memcpy(mem+S,&vt,4);
 put(P,10);put(H,10);put(D,1);put(S+0xF8,10);put(S+0xFC,1);
 for(unsigned i=0;i<10000;i++)frame(10);assert(records==0);
 enabled=0;reads=0;preview_winch_pre(S,P);assert(reads==0);enabled=1;
 preview_winch_pre(0xffff,P);preview_winch_pre(0x4000000-0x180,P);assert(records==0);
 put(S+0x180,.5f);frame(10);assert(records==1);
 for(unsigned i=1;i<60;i++){tick=1000+i*16;frame(i==20?.1f:10);}
 assert(records==1);tick=2000;frame(10);assert(records==2);
 assert(strstr(line,"range=0.1..10") && strstr(line,"wanted=10") && strstr(line,"self_obstacle=00040000"));
 assert(strstr(line,"output=(10,0,0)"));puts(line);
 /* Only a matching result from the current call can produce a record. */
 tick=3000;preview_winch_pre(S,P);preview_winch_hit(0x123,S,H);
 preview_winch_post(0x9E1D5,S,F,D,0);assert(records==2);
 preview_winch_pre(S,P);preview_winch_hit(0x9E1D5,S+4,H);
 preview_winch_post(0x9E1D5,S,F,D,0);assert(records==2);
 put(S+0x180,0);tick=3100;frame(10);assert(records==3);
 tick=6000;frame(10);assert(records==3);
 puts("PASS: no guest writes; disabled/idle paths; one-second limit; short collision retained; call pairing; retraction expiry");
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='winch-camera-log-') as tmp:
            path=Path(tmp); (path/'test.c').write_text(source)
            subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
            subprocess.run([str(path/'test.exe')],check=True)

if __name__=='__main__':unittest.main()

