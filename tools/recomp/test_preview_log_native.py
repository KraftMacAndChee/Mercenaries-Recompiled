"""Native queue contention, retention, crash flush and write-failure tests."""
from pathlib import Path
import hashlib, subprocess, tempfile, re
ROOT=Path(__file__).resolve().parents[2]
SOURCE=r'''
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static volatile LONG reject_writes;
static BOOL test_write(HANDLE h,const void *p,DWORD n,DWORD *done,OVERLAPPED *o) {
 if(reject_writes){*done=0;SetLastError(ERROR_DISK_FULL);return FALSE;}
 return WriteFile(h,p,n,done,o);
}
#define WriteFile test_write
#define PREVIEW_LOG_FILE_LIMIT (32u*1024u)
#include "preview_log.c"
#undef WriteFile
static DWORD WINAPI spam(void *unused) {
 (void)unused;for(unsigned i=0;i<100000;i++)xbox_preview_log_event("stress","repeated warning %u",i);return 0;
}
int wmain(int argc,wchar_t **argv) {
 assert(argc==3);
 assert(owned_name(L"preview-0000000000000001-00000001-0000.log"));
 assert(!owned_name(L"preview-notes.log"));assert(!owned_name(L"preview-0000000000000001-00000001-0000.log.extra"));
 assert(!xbox_preview_log_init(argv[2]));
 _putenv_s("MERCENARIES_DISABLE_PREVIEW_LOGS","1");assert(!xbox_preview_log_init(argv[1]));_putenv_s("MERCENARIES_DISABLE_PREVIEW_LOGS","");
 CreateDirectoryW(argv[1],NULL);
 wchar_t path[MAX_PATH];HANDLE active=NULL;
 for(unsigned i=1;i<=22;i++) {
  swprintf(path,MAX_PATH,L"%ls\\preview-%016llX-00000001-0000.log",argv[1],(unsigned long long)i);
  HANDLE h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);assert(h!=INVALID_HANDLE_VALUE);
  if(i==1) active=h;else CloseHandle(h);
 }
 assert(xbox_preview_log_init(argv[1]));
 LARGE_INTEGER a,b,f;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 HANDLE threads[4];for(unsigned i=0;i<4;i++)threads[i]=CreateThread(NULL,0,spam,NULL,0,NULL);
 assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
 QueryPerformanceCounter(&b);printf("400000 concurrent warning attempts: %.3f ms\n",1000.0*(b.QuadPart-a.QuadPart)/f.QuadPart);
 for(unsigned i=0;i<4;i++)CloseHandle(threads[i]);
 char formats[64][32],payload[650];memset(payload,'x',sizeof(payload)-1);payload[sizeof(payload)-1]=0;
 for(unsigned i=0;i<64;i++)sprintf(formats[i],"record-%u %%s",i);
 for(unsigned lap=0;lap<4;lap++) {Sleep(1100);for(unsigned i=0;i<64;i++){xbox_preview_log_event("rotation",formats[i],payload);Sleep(2);}}
 Sleep(1100);
 /* Three separate stalls with one format must survive within the same second. */
 for(unsigned i=0;i<3;i++) xbox_preview_log_sample("timing-test","operation=%u elapsed_us=%u",i,40000+i);
 xbox_preview_log_event("scene","marker before injected crash");
 xbox_preview_log_event("USER-MARKER-F8","marker before injected crash");
 char too_long[17000];memset(too_long,'z',sizeof(too_long)-1);too_long[sizeof(too_long)-1]=0;
 xbox_preview_log_set_crash_context(too_long);assert(crash_context_length==16383);
 xbox_preview_log_set_crash_context(NULL);assert(!crash_context_length);
 xbox_preview_log_set_crash_context("[HAVOK-ENTITY-HISTORY] preserved before immediate crash\n");
 xbox_preview_log_crash(0xc0000005,(uintptr_t)GetModuleHandleW(NULL)+0x1234,0x204060,1,2,3,4);
 xbox_preview_log_shutdown();
 swprintf(path,MAX_PATH,L"%ls\\preview-0000000000000001-00000001-0000.log",argv[1]);assert(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES);
 CloseHandle(active);
 assert(xbox_preview_log_init(argv[1]));assert(!crash_context_length);InterlockedExchange(&reject_writes,1);
 xbox_preview_log_event("failure","synthetic disk full");Sleep(1200);
 assert(!xbox_preview_log_enabled());xbox_preview_log_shutdown();
 puts("PASS: contention, throttling, rotation, active-file preservation, crash record, disk-full shutdown");return 0;
}
'''
# Execute the production key handler with window-system side effects stubbed.
window_source=(ROOT/'ports/mercenaries/src/main.c').read_text(encoding='utf-8')
handler=re.search(r'static LRESULT CALLBACK host_window_proc\(.*?\n\}',window_source,re.S)[0]
handler=handler.replace('DefWindowProcA','fixture_default').replace('DestroyWindow','fixture_destroy').replace('PostQuitMessage','fixture_quit')
window_fixture=r"""
static unsigned marker_calls, menu_calls;
static void recomp_dev_menu_toggle(void){++menu_calls;}
static int recomp_controls_message(HWND w,UINT m,WPARAM p,LPARAM l){return 0;}
static void preview_snapshot(const char *why){assert(!strcmp(why,"USER-MARKER-F8"));++marker_calls;}
static LRESULT fixture_default(HWND h,UINT m,WPARAM w,LPARAM l){return 0;}
static void fixture_destroy(HWND h){}
static void fixture_quit(int code){}
"""+handler
SOURCE=SOURCE.replace('static DWORD WINAPI spam',window_fixture+'\nstatic DWORD WINAPI spam')
SOURCE=SOURCE.replace(' assert(argc==3);',r""" assert(argc==3);
 host_window_proc(NULL,WM_KEYDOWN,VK_F8,0);assert(marker_calls==1);
 host_window_proc(NULL,WM_KEYDOWN,VK_F8,1L<<30);assert(marker_calls==1);
 host_window_proc(NULL,WM_KEYUP,VK_F8,0);assert(marker_calls==1);
 host_window_proc(NULL,WM_KEYDOWN,VK_F9,0);assert(marker_calls==1 && menu_calls==1);
 host_window_proc(NULL,WM_KEYDOWN,VK_F9,1L<<30);assert(menu_calls==1);
 host_window_proc(NULL,WM_KEYUP,VK_F9,0);assert(menu_calls==1);
""")
with tempfile.TemporaryDirectory(prefix='merc-preview-log-') as directory:
 p=Path(directory);logs=p/'logs';logs.mkdir();(logs/'preview-notes.log').write_text('preserve me')
 (p/'test.c').write_text(SOURCE,encoding='utf-8')
 (p/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(preview_log_test C)
add_executable(preview_log_test test.c)
target_include_directories(preview_log_test PRIVATE "{(ROOT/'src/kernel').as_posix()}")
target_link_libraries(preview_log_test PRIVATE bcrypt psapi)
target_compile_options(preview_log_test PRIVATE /UNDEBUG)
''')
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 def run(args):
  result=subprocess.run(args,capture_output=True,text=True,timeout=60)
  if result.returncode:print(result.stdout);print(result.stderr)
  result.check_returncode();return result.stdout
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 exe=p/'build/Release/preview_log_test.exe'
 print(run([str(exe),str(logs),str(p/'missing-parent/logs')]))
 records=[f for f in logs.glob('preview-*.log') if f.name!='preview-notes.log']
 assert len(records)<=16,len(records)
 assert max(f.stat().st_size for f in records)<=32768
 new_parts=[f for f in records if int(f.name[8:24],16)>22 and not f.name.endswith('-9999.log') and f.stat().st_size]
 assert len(new_parts)>=2, 'Rotation must actually occur'
 text='\n'.join(f.read_text() for f in records)
 assert 'exe_sha256='+hashlib.sha256(exe.read_bytes()).hexdigest().upper() in text
 for i in range(3):assert f'[timing-test] operation={i} elapsed_us={40000+i}' in text
 assert '[USER-MARKER-F8]' in text
 assert 'exception=C0000005 native_rva=1234 guest_pc=00204060' in text
 assert '[HAVOK-ENTITY-HISTORY] preserved before immediate crash' in text
 assert 'clean shutdown' in text
 assert (logs/'preview-notes.log').read_text()=='preserve me'
 # The oldest file was protected during retention, then can be pruned after close.
 print(f'PASS: {len(records)} retained files; largest {max(f.stat().st_size for f in records)} bytes; executable hash, marker and crash fields verified')
