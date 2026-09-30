"""Exercise opt-in crash RAM capture without real process/file side effects."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define assert(c) do {if (!(c)) exit(1);} while(0)
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef size_t SIZE_T;
typedef intptr_t HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define GENERIC_WRITE 1
#define FILE_SHARE_READ 2
#define CREATE_NEW 3
#define FILE_ATTRIBUTE_NORMAL 4
static intptr_t g_xbox_mem_offset=0x10000;
static unsigned mode,created,reads,writes,closed;
static const char *capture_path="capture.bin";
static const char *fake_getenv(const char *name) {assert(!strcmp(name,"MERCENARIES_DUMP_CRASH_GUEST_PATH"));return capture_path;}
#define getenv fake_getenv
static LONG InterlockedCompareExchange(volatile LONG *p,LONG next,LONG expected) {LONG old=*p;if(old==expected)*p=next;return old;}
static HANDLE GetCurrentProcess(void) {return 42;}
static unsigned GetLastError(void) {return 80;}
static HANDLE CreateFileA(const char *path,DWORD access,DWORD share,void *security,DWORD disposition,DWORD flags,HANDLE other) {
 assert(!strcmp(path,capture_path)&&access==GENERIC_WRITE&&share==FILE_SHARE_READ);
 assert(disposition==CREATE_NEW&&flags==FILE_ATTRIBUTE_NORMAL);++created;
 return mode==2?INVALID_HANDLE_VALUE:123;
}
static int ReadProcessMemory(HANDLE process,const void *address,void *output,SIZE_T count,SIZE_T *copied) {
 assert(process==42&&count==65536&&(uintptr_t)address==0x10000+(uintptr_t)reads*65536);
 ++reads;if(mode==3&&reads==4){*copied=0;return 0;}
 memset(output,0x5A,count);*copied=count;return 1;
}
static int WriteFile(HANDLE file,const void *buffer,DWORD count,DWORD *written,void *overlapped) {
 assert(file==123&&count==65536&&((const unsigned char *)buffer)[count-1]==0x5A);
 ++writes;*written=count;return 1;
}
static int CloseHandle(HANDLE file) {assert(file==123);++closed;return 1;}
'''


class CrashCaptureTests(unittest.TestCase):
    def test_bounded_opt_in_read_only_capture_and_failure_paths(self):
        source = (ROOT / "ports/mercenaries/src/main.c").read_text(encoding="utf-8")
        start = source.index("static void capture_crash_guest_memory(void)")
        body = source[start:source.index("\nstatic LONG CALLBACK veh_handler", start)]
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required")
        harness = r'''
int main(int argc,char **argv) {
 mode=argc>1?(unsigned)atoi(argv[1]):0;
 if(mode==0)capture_path=NULL;
 capture_crash_guest_memory();
 if(mode==0) assert(!created&&!reads&&!writes&&!closed);
 else if(mode==2) assert(created==1&&!reads&&!writes&&!closed);
 else if(mode==3) assert(created==1&&reads==4&&writes==3&&closed==1);
 else assert(created==1&&reads==1024&&writes==1024&&closed==1);
 capture_crash_guest_memory();
 assert(created==(mode?1:0));
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="mercs-crash-capture-") as directory:
            c_path = Path(directory) / "check.c"
            exe_path = Path(directory) / "check.exe"
            c_path.write_text(PRELUDE+body+harness, encoding="utf-8")
            result = subprocess.run([compiler,"-std=c11",str(c_path),"-o",str(exe_path)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
            for mode in range(4):
                with self.subTest(mode=mode):
                    result=subprocess.run([str(exe_path),str(mode)],capture_output=True)
                    self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main()
