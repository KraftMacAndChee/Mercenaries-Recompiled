"""Native installer regression: real extraction, cancellation, close guards and commit."""
from pathlib import Path
import subprocess, shutil, sys
ROOT=Path(__file__).resolve().parents[2]
CASE=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else ROOT/'artifacts/diagnostics/launcher-art-20260919/native'
assert CASE.is_relative_to(ROOT/'artifacts/diagnostics'), 'Installer fixtures stay inside project diagnostics'
CASE.mkdir(parents=True,exist_ok=True)
port=ROOT/'ports/mercenaries'
source=r'''
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <assert.h>
#include <stdio.h>
static int errors;
static int test_message_box(HWND w,LPCWSTR text,LPCWSTR title,UINT flags) {
    (void)w;(void)title;(void)flags;errors++;fwprintf(stderr,L"Expected/test dialog: %s\n",text);return IDOK;
}
#define MessageBoxW test_message_box
#define wWinMain launcher_wWinMain
#include "setup_launcher.c"
#undef wWinMain
static unsigned progress_events,intermediate_progress,last_progress;
static int track_progress;
static void drain(void) {
    MSG msg;
    while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {
        if(track_progress && msg.message==WM_INSTALL_PROGRESS){
            assert(msg.wParam<=100 && msg.wParam>=last_progress);
            last_progress=(unsigned)msg.wParam;++progress_events;
            if(msg.wParam>5 && msg.wParam<90)++intermediate_progress;
        }
        if(msg.message!=WM_QUIT) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
}
static void await_install(void) {
    ULONGLONG start=GetTickCount64();
    while(g_state.installing && GetTickCount64()-start<120000) {drain();Sleep(10);}
    assert(!g_state.installing);
}
static void write_file(const wchar_t *p,const char *s) {
    FILE *f=_wfopen(p,L"wb");assert(f);fwrite(s,1,strlen(s),f);fclose(f);
}
int wmain(int argc,wchar_t **argv) {
    HINSTANCE instance=GetModuleHandleW(NULL);wchar_t file[PATH_CAPACITY],sentinel[PATH_CAPACITY],bad[PATH_CAPACITY];
    assert(argc==3 || argc==4);assert(setup_art_init(instance));
    wcscpy_s(g_state.base_dir,ARRAYSIZE(g_state.base_dir),argv[1]);
    assert(path_join(g_state.game_dir,ARRAYSIZE(g_state.game_dir),argv[1],GAME_RELATIVE_DIR));
    assert(path_join(g_state.staging_dir,ARRAYSIZE(g_state.staging_dir),argv[1],L"game_files\\.mercenaries-installing"));
    assert(ensure_directory_tree(g_state.game_dir));
    assert(path_join(sentinel,ARRAYSIZE(sentinel),g_state.game_dir,L"existing-install.txt"));
    write_file(sentinel,"preserve existing installation on cancel/failure");
    HWND w=create_setup_window(instance,L"");assert(w);
    RECT client,close_rect;GetClientRect(w,&client);HRGN outline=CreateRectRgn(0,0,0,0);
    assert(GetWindowRgn(w,outline)!=ERROR);
    assert(PtInRegion(outline,1,1) && !PtInRegion(outline,client.right-1,1));
    close_rect=setup_art_rect(client.right,client.bottom,TRUE);
    assert(PtInRegion(outline,(close_rect.left+close_rect.right)/2,(close_rect.top+close_rect.bottom)/2));
    DeleteObject(outline);
    assert(IsWindowEnabled(g_state.close_button));
    g_state.installing=TRUE;g_state.install_phase=1;g_state.view.busy=TRUE;refresh_setup();
    assert(!IsWindowEnabled(g_state.close_button));
    window_proc(w,WM_CLOSE,0,0);assert(IsWindow(w));
    window_proc(w,WM_SYSCOMMAND,SC_CLOSE,0);assert(IsWindow(w));
    assert(!window_proc(w,WM_QUERYENDSESSION,0,0));
    setup_action();assert(g_state.install_phase==2 && g_state.view.cancelling);
    assert(!IsWindowEnabled(g_state.action_button));
    g_state.cancelled=TRUE;window_proc(w,WM_INSTALL_FINISHED,0,0);
    assert(IsWindow(w)&&IsWindowEnabled(g_state.close_button));
    assert(!wcscmp(g_state.view.button,L"BROWSE..."));
    g_state.installing=TRUE;g_state.install_phase=3;refresh_setup();
    setup_action();assert(g_state.install_phase==3); /* Cannot cancel halfway through commit. */
    g_state.install_succeeded=TRUE;g_state.cancelled=FALSE;
    window_proc(w,WM_INSTALL_FINISHED,0,0);
    assert(IsWindow(w)&&g_state.view.completed&&g_state.view.progress==100);
    assert(!wcscmp(g_state.view.button,L"LAUNCH")); /* No automatic game launch. */
    g_state.install_succeeded=FALSE;
    if(argc==4 && !wcscmp(argv[3],L"--ui-only")){
        window_proc(w,WM_CLOSE,0,0);assert(!IsWindow(w));setup_art_shutdown();
        puts("PASS: clipped frame, close hit region, installation close/commit/cancel guards and success state");return 0;
    }
    /* Real bundled extractor cancellation, using only a disposable private root. */
    wcscpy_s(g_state.selected_iso,ARRAYSIZE(g_state.selected_iso),argv[2]);
    begin_install();assert(g_state.installing);
    ULONGLONG start=GetTickCount64();
    while(!directory_exists(g_state.staging_dir)&&GetTickCount64()-start<20000) {drain();Sleep(10);}
    assert(g_state.installing && directory_exists(g_state.staging_dir));
    setup_action();await_install();
    assert(g_state.cancelled&&!g_state.install_succeeded);
    assert(!directory_exists(g_state.staging_dir)&&regular_nonempty_file(sentinel));
    /* Invalid ISO must recover to Browse and preserve the previous destination. */
    assert(path_join(bad,ARRAYSIZE(bad),argv[1],L"invalid.iso"));write_file(bad,"not an Xbox ISO");
    wcscpy_s(g_state.selected_iso,ARRAYSIZE(g_state.selected_iso),bad);
    begin_install();await_install();
    assert(!g_state.cancelled&&!g_state.install_succeeded&&errors==1);
    assert(IsWindowEnabled(g_state.close_button)&&regular_nonempty_file(sentinel));
    /* Retry performs the full inventory/hash validation before activation. */
    track_progress=1;last_progress=progress_events=intermediate_progress=0;
    wcscpy_s(g_state.selected_iso,ARRAYSIZE(g_state.selected_iso),argv[2]);
    begin_install();await_install();
    assert(g_state.install_succeeded && g_state.view.progress==100);
    assert(progress_events>4 && intermediate_progress>0 && last_progress==100);
    assert(validate_install(g_state.game_dir,FALSE,g_state.error,ARRAYSIZE(g_state.error)));
    assert(!directory_exists(g_state.staging_dir));
    assert(!regular_nonempty_file(sentinel)); /* Invalid prior tree was moved aside, not deleted. */
    assert(IsWindow(w)&&IsWindowEnabled(g_state.close_button));
    assert(window_proc(w,WM_QUERYENDSESSION,0,0));
    window_proc(w,WM_CLOSE,0,0);assert(!IsWindow(w));
    setup_art_shutdown();
    puts("PASS: disabled X/Alt-F4/close/shutdown while busy; cancellation and commit guard; real extraction cancel/cleanup; invalid ISO recovery; full retail validation/activation; explicit Launch state; close re-enabled.");
    return 0;
}
'''
(CASE/'test.c').write_text(source)
(CASE/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(setup_test C CXX RC)
add_executable(setup_test test.c "{(port/'src/setup_art.cpp').as_posix()}" "{(port/'src/mod_loader.cpp').as_posix()}" "{(port/'src/mod_selector.cpp').as_posix()}" "{(port/'resources/app.rc').as_posix()}" "{(port/'resources/launcher.rc').as_posix()}")
set_property(TARGET setup_test PROPERTY CXX_STANDARD 17)
target_include_directories(setup_test PRIVATE "{(port/'src').as_posix()}" "{(port/'resources').as_posix()}")
target_compile_definitions(setup_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(setup_test PRIVATE /utf-8 /UNDEBUG)
target_link_libraries(setup_test PRIVATE gdiplus ole32 bcrypt comctl32 comdlg32 shell32)
target_link_options(setup_test PRIVATE /STACK:8388608,65536)
set_property(TARGET setup_test PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
''')
cmake=shutil.which('cmake') or str(ROOT/'.venv/Scripts/cmake.exe')
subprocess.run([str(cmake),'-S',str(CASE),'-B',str(CASE/'build'),'-G','Visual Studio 17 2022','-A','x64'],check=True)
subprocess.run([str(cmake),'--build',str(CASE/'build'),'--config','Release','--parallel','4'],check=True)
runtime=CASE/'runtime'
assert not runtime.exists(), 'Use a fresh disposable test directory'
(runtime/'tools').mkdir(parents=True)
shutil.copy2(port/'third_party/xdvdfs/xdvdfs.exe',runtime/'tools/xdvdfs.exe')
subprocess.run([str(CASE/'build/Release/setup_test.exe'),str(runtime),sys.argv[1]]+(["--ui-only"] if len(sys.argv)>3 and sys.argv[3]=="--ui-only" else []),check=True,timeout=180)
