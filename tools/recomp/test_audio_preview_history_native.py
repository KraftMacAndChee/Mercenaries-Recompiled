"""Run the actual XAudio2 submit path with deterministic queue/error fixtures."""
from pathlib import Path
import re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class AudioHistoryTests(unittest.TestCase):
 def test_submit_history_and_original_buffer_behavior(self):
  src=(ROOT/'src/apu/apu_xaudio2.c').read_text()
  globals=src[src.index('#define XA2_SAMPLE_RATE'):src.index('int xa2_init(void)')]
  submit=re.search(r'int xa2_submit_samples\(.*?\n\}',src,re.S)[0]
  fixture=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
typedef unsigned long long ULONGLONG;
typedef int32_t HRESULT;
typedef uint8_t BYTE;
typedef int IXAudio2;
typedef int IXAudio2MasteringVoice;
typedef int IXAudio2SourceVoice;
typedef struct {unsigned BuffersQueued;} XAUDIO2_VOICE_STATE;
typedef struct {unsigned AudioBytes;const BYTE *pAudioData;} XAUDIO2_BUFFER;
typedef struct {unsigned GlitchesSinceEngineStarted,ActiveSourceVoiceCount;} XAUDIO2_PERFORMANCE_DATA;
#define XAUDIO2_VOICE_NOSAMPLESPLAYED 0
#define FAILED(hr) ((hr)<0)
static unsigned queued=3,enabled=1,health,errors,perf_reads,submits,fail;
static ULONGLONG now=1000;
static char record[2048];
static int16_t input[512];
static ULONGLONG GetTickCount64(void){return now;}
static int xbox_preview_log_enabled(void){return enabled;}
static void IXAudio2_GetPerformanceData(IXAudio2 *unused,XAUDIO2_PERFORMANCE_DATA *p){(void)unused;perf_reads++;p->GlitchesSinceEngineStarted=7;p->ActiveSourceVoiceCount=1;}
static void xbox_preview_log_event(const char *category,const char *format,...){
 if(!strcmp(category,"audio-error")){errors++;return;}
 assert(!strcmp(category,"audio-health"));health++;va_list a;va_start(a,format);vsnprintf(record,sizeof(record),format,a);va_end(a);
}
static void IXAudio2SourceVoice_GetState(IXAudio2SourceVoice *unused,XAUDIO2_VOICE_STATE *s,unsigned flags){(void)unused;(void)flags;s->BuffersQueued=queued;}
static HRESULT IXAudio2SourceVoice_SubmitSourceBuffer(IXAudio2SourceVoice *unused,XAUDIO2_BUFFER *b,void *unused2){
 (void)unused;(void)unused2;assert(b->AudioBytes==sizeof(input));assert(!memcmp(b->pAudioData,input,sizeof(input)));submits++;return fail?-1:0;
}
'''+globals+submit+r'''
int main(void){
 g_xa2_initialized=1;g_xa2_source=(void*)1;g_xa2_trace=0;
 for(unsigned i=0;i<512;i++)input[i]=(int16_t)(i*57);
 assert(xa2_submit_samples(input,256)==1);assert(health==1&&submits==1);
 now=1005;queued=0;assert(xa2_submit_samples(input,256)==1);assert(g_xa2_underruns==1);
 now=1010;queued=8;assert(xa2_submit_samples(input,256)==0);assert(g_xa2_frames_dropped==1&&submits==2);
 now=1015;queued=2;fail=1;assert(xa2_submit_samples(input,256)==0);assert(g_xa2_submit_failures==1&&errors==1);
 fail=0;now=1090;assert(xa2_submit_samples(input,256)==1);
 for(now=1095;now<=2000;now+=5)assert(xa2_submit_samples(input,256)==1);
 assert(health==2&&perf_reads==2);assert(strstr(record,"underruns=1")&&strstr(record,"dropped=1")&&strstr(record,"submit_failures=1"));
 assert(strstr(record,"queue_range=0..8")&&strstr(record,"max_submit_gap_ms=75")&&strstr(record,"engine_glitches=7"));puts(record);
 enabled=0;unsigned before=perf_reads;for(unsigned i=0;i<10000;i++)xa2_preview_observe(3);assert(perf_reads==before);
 for(unsigned i=0;i<512;i++)assert(input[i]==(int16_t)(i*57));
 puts("PASS: actual submissions preserve PCM and queue behavior; transient starvation/drop/failure/gap retained; one-second sampling; disabled fast path");
 return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='audio-preview-test-') as tmp:
   p=Path(tmp);(p/'test.c').write_text(fixture)
   subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
   subprocess.run([str(p/'test.exe')],check=True)
if __name__=='__main__':unittest.main()
