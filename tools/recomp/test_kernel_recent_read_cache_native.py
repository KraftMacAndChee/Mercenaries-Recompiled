"""Exercise production read-cache byte correctness and interleaved-bank I/O."""
from pathlib import Path
import shutil, subprocess, tempfile, os
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
/* Old MinGW headers lack InitOnce; this single-thread harness invokes the
 * production initializer once and uses real Windows critical sections. */
typedef int INIT_ONCE;typedef INIT_ONCE *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT 0
static void InitOnceExecuteOnce(PINIT_ONCE once,BOOL (CALLBACK *fn)(PINIT_ONCE,PVOID,PVOID*),PVOID p,PVOID *c){if(!*once){assert(fn(once,p,c));*once=1;}}
#define XBOX_FILE_NO_INTERMEDIATE_BUFFERING 8
static unsigned reads, version, fail_reads;
static uint64_t fetched_total;
static BYTE expected(uint64_t offset) { return (BYTE)((offset >> 12) + version); }
static BOOL test_read(HANDLE h,void *data,DWORD length,DWORD *fetched,OVERLAPPED *ov) {
 (void)h; ++reads;
 if(fail_reads){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
 uint64_t offset=((uint64_t)ov->OffsetHigh<<32)|ov->Offset, size=128u*1024u*1024u;
 *fetched=offset>=size?0:(DWORD)(size-offset<length?size-offset:length);
 BYTE *p=data;
 for(DWORD i=0;i<*fetched;){DWORD n=4096-(DWORD)((offset+i)&4095);if(n>*fetched-i)n=*fetched-i;memset(p+i,expected(offset+i),n);i+=n;}
 fetched_total+=*fetched;return TRUE;
}
#define ReadFile test_read
'''
POST=r'''
static BYTE output[256*1024];
static BOOL read_at(HANDLE h,uint64_t offset,DWORD length,DWORD *got,BOOL *handled) {
 LARGE_INTEGER o;o.QuadPart=offset;memset(output,0xCD,sizeof(output));
 BOOL ok=read_file_through_guest_cache(h,output,length,&o,got,handled);
 if(ok){for(DWORD i=0;i<*got;i++)assert(output[i]==expected(offset+i));assert(output[*got]==0xCD);}
 return ok;
}
int main(int argc,char **argv){
 (void)argv;
 HANDLE h=(HANDLE)1,h2=(HANDLE)2;DWORD got=0;BOOL handled=FALSE;
 register_read_cache_handle(h,8);
 uint64_t offsets[]={19333120,56328192,75431936};
 for(int pass=0;pass<12;pass++)for(int i=0;i<3;i++){
  assert(read_at(h,offsets[i],20480,&got,&handled)&&handled&&got==20480);
 }
 assert(reads==(argc>1?36u:3u));
 printf("interleaved: host_reads=%u fetched_bytes=%llu\n",reads,(unsigned long long)fetched_total);
 assert(read_at(h,offsets[0]+31,100,&got,&handled)&&got==100);
 /* A mutation must discard both levels, including contained requests. */
 ++version;invalidate_read_cache_for_handle(h);unsigned before=reads;
 assert(read_at(h,offsets[0],20480,&got,&handled)&&reads==before+1);
 register_read_cache_handle(h2,8);before=reads;
 assert(read_at(h2,offsets[0],20480,&got,&handled)&&reads==before+1);
 /* More entries than either tier can retain, then revisit an evicted one. */
 for(int i=0;i<24;i++)assert(read_at(h,(uint64_t)(i%12)*10*1024*1024+i*4096,20480,&got,&handled));
 assert(read_at(h,offsets[0],20480,&got,&handled));
 /* Partial reads/EOF remain partial and are not promoted as complete hits. */
 assert(read_at(h,128u*1024u*1024u-17,100,&got,&handled)&&got==17);
 assert(read_at(h,128u*1024u*1024u+1,100,&got,&handled)&&got==0);
 invalidate_read_cache_for_handle(h);fail_reads=1;
 assert(!read_at(h,0,100,&got,&handled)&&handled&&GetLastError()==ERROR_ACCESS_DENIED);
 fail_reads=0;assert(read_at(h,0,100,&got,&handled)&&got==100);
 cleanup_read_cache_for_handle(h);++version;register_read_cache_handle(h,8);
 assert(read_at(h,offsets[0],20480,&got,&handled));
 cleanup_read_cache_for_handle(h);cleanup_read_cache_for_handle(h2);
 assert(!read_at((HANDLE)3,0,100,&got,&handled)&&!handled);
 puts("PASS: exact bytes, contained reads, eviction, mutation, EOF, errors, handle isolation/reuse");
 return 0;
}
'''
def main():
 s=(ROOT/'src/kernel/kernel_file.c').read_text()
 body=s[s.index('#define XBOX_READ_CACHE_CONTEXTS'):s.index('/* Translate an Xbox OBJECT_ATTRIBUTES path')]
 cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
 with tempfile.TemporaryDirectory(prefix='recent-read-cache-') as t:
  c=Path(t)/'test.c';exe=c.with_suffix('.exe');c.write_text(PRE+body+POST)
  subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=30)
  subprocess.run([str(exe),'legacy'],check=True,timeout=30,env={**os.environ,'MERCENARIES_DISABLE_RECENT_READ_CACHE':'1'})
if __name__=='__main__':main()

