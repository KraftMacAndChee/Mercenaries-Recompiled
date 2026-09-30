"""Slow I/O remains observable after startup; diagnostics preserve read errors."""
from pathlib import Path
import shutil, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
def main():
    source=(ROOT/'src/kernel/kernel_file.c').read_text(encoding='utf-8')
    start=source.index('    read_error = result ? ERROR_SUCCESS : GetLastError();')
    end=source.index('    if (result || GetLastError() == ERROR_HANDLE_EOF)',start)
    body=source[start:end]
    prelude=r'''#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#define FILE_NAME_NORMALIZED 0
static unsigned events, fake_us;
static void fake_counter(LARGE_INTEGER *v) { v->QuadPart=fake_us; }
static DWORD fake_path(HANDLE h,WCHAR*p,DWORD n,DWORD flags) {
 (void)h;(void)p;(void)n;(void)flags;SetLastError(ERROR_INVALID_HANDLE);return 0;
}
#define QueryPerformanceCounter fake_counter
#define GetFinalPathNameByHandleW fake_path
#define wcscpy_s(p,n,s) wcscpy(p,s)
#define xbox_preview_log_sample(...) (++events)
static unsigned traced_reads;
static void read_trace(BOOL result,unsigned us) {
 int trace_reads=1;DWORD read_error,bytes_read=32;BOOL cache_handled=0;
 LARGE_INTEGER trace_start={0},trace_end={0},trace_frequency={.QuadPart=1000000};
 HANDLE FileHandle=0;ULONG Length=32;PLARGE_INTEGER ByteOffset=0;
 unsigned g_recomp_current_func=0;fake_us=us;
'''
    harness=r'''
}
int main(void) {
 for(unsigned i=0;i<5000;++i)read_trace(TRUE,10);
 if(events!=16){fprintf(stderr,"Normal reads flooded trace: %u",events);return 1;}
 read_trace(TRUE,40000);
 if(events!=17){fputs("Late 40ms read was lost",stderr);return 2;}
 SetLastError(ERROR_HANDLE_EOF);read_trace(FALSE,10);
 if(events!=18||GetLastError()!=ERROR_HANDLE_EOF){fputs("Diagnostic changed EOF",stderr);return 3;}
 SetLastError(ERROR_ACCESS_DENIED);read_trace(FALSE,10);
 if(events!=19||GetLastError()!=ERROR_ACCESS_DENIED)return 4;
 puts("PASS: late slow reads recorded; fast reads quiet; I/O errors preserved");return 0;
}
'''
    compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
    with tempfile.TemporaryDirectory(prefix='mercs-read-trace-') as tmp:
        c=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
        c.write_text(prelude+body+harness,encoding='utf-8')
        subprocess.run([compiler,'-std=c11','-O2',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':main()
