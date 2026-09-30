#define UNICODE
#define _UNICODE

#include <windows.h>
#include "app_icon.h"
#include "setup_art.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#define APP_TITLE L"Mercenaries Recompiled Setup"
#define GAME_EXE L"mercenaries_recomp.exe"
#define GAME_RELATIVE_DIR L"game_files\\mercenaries-retail"
#define EXTRACTOR_RELATIVE_PATH L"tools\\xdvdfs.exe"
#define INSTALL_MARKER L".mercenaries-recompiled-install-v1"
#define PATH_CAPACITY 32768
#define IDC_INSTALL 1003
#define IDC_QUIT 1004
#define WM_INSTALL_FINISHED (WM_APP + 1)
#define WM_INSTALL_PROGRESS (WM_APP + 2)

static const unsigned char k_retail_xbe_sha256[32] = {
    0xAA,0x08,0xEA,0x21,0xD9,0x52,0xAC,0x35,
    0xF4,0x9C,0x02,0xC7,0xE2,0xED,0x08,0xAA,
    0x25,0xAD,0x75,0x35,0xFB,0xBB,0xCC,0xC9,
    0x56,0x36,0x77,0x5F,0x37,0xBE,0x99,0xD7
};

typedef struct launcher_state {
    HWND window, action_button, close_button;
    wchar_t base_dir[PATH_CAPACITY];
    wchar_t game_dir[PATH_CAPACITY];
    wchar_t staging_dir[PATH_CAPACITY];
    wchar_t selected_iso[PATH_CAPACITY];
    wchar_t error[1024];
    BOOL installing, install_succeeded, cancelled;
    volatile LONG install_phase; /* 0 idle, 1 cancellable, 2 cancelling, 3 committing */
    setup_view view;
} launcher_state;

static launcher_state g_state;
static BOOL install_cancelled(void)
{
    return InterlockedCompareExchange(&g_state.install_phase, 0, 0) == 2;
}


static BOOL path_join(wchar_t *output, size_t capacity,
                      const wchar_t *left, const wchar_t *right)
{
    return _snwprintf_s(output, capacity, _TRUNCATE,
                        L"%s\\%s", left, right) >= 0;
}

static BOOL regular_nonempty_file(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return FALSE;
    return !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           (data.nFileSizeHigh != 0 || data.nFileSizeLow != 0);
}

static BOOL directory_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static BOOL hash_file_sha256(const wchar_t *path, unsigned char digest[32])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char *hash_object = NULL;
    unsigned char *buffer = NULL;
    DWORD object_size = 0, digest_size = 0, result_size = 0, bytes_read;
    NTSTATUS status;
    BOOL ok = FALSE;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;
    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                         NULL, 0);
    if (status < 0) goto cleanup;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               (PUCHAR)&object_size, sizeof(object_size),
                               &result_size, 0);
    if (status < 0) goto cleanup;
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                               (PUCHAR)&digest_size, sizeof(digest_size),
                               &result_size, 0);
    if (status < 0 || digest_size != 32) goto cleanup;
    hash_object = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, object_size);
    if (!hash_object) goto cleanup;
    buffer = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, 1024 * 1024);
    if (!buffer) goto cleanup;
    status = BCryptCreateHash(algorithm, &hash, hash_object, object_size,
                              NULL, 0, 0);
    if (status < 0) goto cleanup;
    do {
        if (install_cancelled() || !ReadFile(file, buffer, 1024 * 1024, &bytes_read, NULL))
            goto cleanup;
        if (bytes_read && BCryptHashData(hash, buffer, bytes_read, 0) < 0)
            goto cleanup;
    } while (bytes_read);
    if (BCryptFinishHash(hash, digest, 32, 0) < 0) goto cleanup;
    ok = TRUE;
cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    if (hash_object) HeapFree(GetProcessHeap(), 0, hash_object);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}


#define EXPECTED_RETAIL_FILE_COUNT 434ULL
#define EXPECTED_RETAIL_TOTAL_BYTES 2506857692ULL

static BOOL scan_install_tree(const wchar_t *directory,
                              uint64_t *file_count, uint64_t *total_bytes)
{
    wchar_t search[PATH_CAPACITY], child[PATH_CAPACITY];
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (!path_join(search, ARRAYSIZE(search), directory, L"*")) return FALSE;
    find = FindFirstFileW(search, &data);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        uint64_t size;
        if (install_cancelled()) { FindClose(find); return FALSE; }
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
            continue;
        if (!path_join(child, ARRAYSIZE(child), directory, data.cFileName)) {
            FindClose(find);
            return FALSE;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            FindClose(find);
            return FALSE;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!scan_install_tree(child, file_count, total_bytes)) {
                FindClose(find);
                return FALSE;
            }
            continue;
        }
        if (!wcscmp(data.cFileName, INSTALL_MARKER) ||
            !_wcsicmp(data.cFileName, L"xbox_kernel.log"))
            continue;
        size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        ++*file_count;
        *total_bytes += size;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return GetLastError() == ERROR_NO_MORE_FILES;
}

static BOOL validate_install(const wchar_t *game_dir, BOOL require_marker,
                             wchar_t *reason, size_t reason_capacity)
{
    static const wchar_t *required_files[] = {
        L"default.xbe", L"DATAxbox\\assets.dsk", L"DATAxbox\\english.dsk",
        L"DATAxbox\\locked.dsk", L"DATAxbox\\streamed.dsk",
        L"DATAxbox\\MOVIES\\attract.xmv"
    };
    static const wchar_t *required_directories[] = {
        L"DATAxbox\\SOUND", L"DATAxbox\\NW", L"DATAxbox\\HUD"
    };
    wchar_t path[PATH_CAPACITY];
    unsigned char digest[32];
    size_t index;
    uint64_t file_count = 0, total_bytes = 0;

    if (!directory_exists(game_dir)) {
        _snwprintf_s(reason, reason_capacity, _TRUNCATE,
                     L"Game files have not been installed yet.");
        return FALSE;
    }
    if (require_marker) {
        if (!path_join(path, ARRAYSIZE(path), game_dir, INSTALL_MARKER) ||
            !regular_nonempty_file(path)) {
            _snwprintf_s(reason, reason_capacity, _TRUNCATE,
                         L"The previous installation did not complete.");
            return FALSE;
        }
        if (reason_capacity) reason[0] = L'\0';
        return TRUE;
    }
    for (index = 0; index < ARRAYSIZE(required_files); ++index) {
        if (!path_join(path, ARRAYSIZE(path), game_dir, required_files[index]) ||
            !regular_nonempty_file(path)) {
            _snwprintf_s(reason, reason_capacity, _TRUNCATE,
                         L"A required game file is missing: %s",
                         required_files[index]);
            return FALSE;
        }
    }
    for (index = 0; index < ARRAYSIZE(required_directories); ++index) {
        if (!path_join(path, ARRAYSIZE(path), game_dir,
                       required_directories[index]) ||
            !directory_exists(path)) {
            _snwprintf_s(reason, reason_capacity, _TRUNCATE,
                         L"A required game directory is missing: %s",
                         required_directories[index]);
            return FALSE;
        }
    }
    if (!scan_install_tree(game_dir, &file_count, &total_bytes) ||
        file_count != EXPECTED_RETAIL_FILE_COUNT ||
        total_bytes != EXPECTED_RETAIL_TOTAL_BYTES) {
        _snwprintf_s(reason, reason_capacity, _TRUNCATE,
                     L"The installed game-file inventory is incomplete or damaged.");
        return FALSE;
    }
    if (!path_join(path, ARRAYSIZE(path), game_dir, L"default.xbe") ||
        !hash_file_sha256(path, digest) ||
        memcmp(digest, k_retail_xbe_sha256, sizeof(digest))) {
        _snwprintf_s(reason, reason_capacity, _TRUNCATE,
                     L"default.xbe is not the supported retail Mercenaries release.");
        return FALSE;
    }
    if (reason_capacity) reason[0] = L'\0';
    return TRUE;
}

static BOOL ensure_directory_tree(const wchar_t *path)
{
    wchar_t copy[PATH_CAPACITY], *cursor;
    if (wcslen(path) >= ARRAYSIZE(copy)) return FALSE;
    wcscpy_s(copy, ARRAYSIZE(copy), path);
    for (cursor = copy + 3; *cursor; ++cursor) {
        if (*cursor == L'\\') {
            *cursor = L'\0';
            if (!CreateDirectoryW(copy, NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
            *cursor = L'\\';
        }
    }
    return CreateDirectoryW(copy, NULL) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL remove_tree(const wchar_t *path)
{
    wchar_t search[PATH_CAPACITY], child[PATH_CAPACITY];
    WIN32_FIND_DATAW data;
    HANDLE find;
    if (!directory_exists(path)) return TRUE;
    if (!path_join(search, ARRAYSIZE(search), path, L"*")) return FALSE;
    find = FindFirstFileW(search, &data);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND && RemoveDirectoryW(path);
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
            continue;
        if (!path_join(child, ARRAYSIZE(child), path, data.cFileName)) {
            FindClose(find); return FALSE;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            FindClose(find); return FALSE;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!remove_tree(child)) { FindClose(find); return FALSE; }
        } else {
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(child)) { FindClose(find); return FALSE; }
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return RemoveDirectoryW(path);
}

static BOOL run_extractor(const wchar_t *extractor, const wchar_t *iso,
                          const wchar_t *destination, DWORD *exit_code)
{
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = { 0 };
    wchar_t command[PATH_CAPACITY * 3];
    DWORD wait_result;
    unsigned int last_percent = 5;
    BOOL ok;
    if (_snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
                     L"\"%s\" unpack \"%s\" \"%s\"",
                     extractor, iso, destination) < 0) return FALSE;
    ok = CreateProcessW(extractor, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, g_state.base_dir,
                        &startup, &process);
    if (!ok) return FALSE;
    do {
        wait_result = WaitForSingleObject(process.hProcess, 200);
        if (install_cancelled()) {
            /* Only our staging extractor is stopped; cleanup waits for its handles. */
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, INFINITE);
            *exit_code = ERROR_CANCELLED;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return FALSE;
        }
        if (wait_result == WAIT_TIMEOUT && g_state.window) {
            IO_COUNTERS counters;
            unsigned int percent;
            /* Do not recursively rescan the growing output tree here.  That
             * competes with xdvdfs for the same disk every polling interval
             * and can turn a short extraction into an I/O-bound crawl.  The
             * child process' write-transfer counter is constant-time to read
             * and tracks the bytes it has actually emitted.  A single full
             * inventory validation still runs after extraction completes. */
            if (GetProcessIoCounters(process.hProcess, &counters)) {
                percent = 5u + (unsigned int)(
                    (counters.WriteTransferCount * 85ULL) /
                    EXPECTED_RETAIL_TOTAL_BYTES);
                if (percent > 90) percent = 90;
                if (percent > last_percent) {
                    last_percent = percent;
                    PostMessageW(g_state.window, WM_INSTALL_PROGRESS,
                                 (WPARAM)percent, 0);
                }
            }
        }
    } while (wait_result == WAIT_TIMEOUT);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        ok = FALSE;
    } else {
        ok = GetExitCodeProcess(process.hProcess, exit_code);
    }
    if (g_state.window)
        PostMessageW(g_state.window, WM_INSTALL_PROGRESS, 90, 0);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}

static BOOL write_install_marker(const wchar_t *game_dir)
{
    static const char marker[] =
        "Mercenaries Recompiled asset install v1\r\n"
        "retail_xbe_sha256=AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7\r\n";
    wchar_t path[PATH_CAPACITY];
    HANDLE file;
    DWORD written;
    if (!path_join(path, ARRAYSIZE(path), game_dir, INSTALL_MARKER))
        return FALSE;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!WriteFile(file, marker, (DWORD)(sizeof(marker) - 1), &written, NULL) ||
        written != sizeof(marker) - 1 || !FlushFileBuffers(file)) {
        CloseHandle(file); DeleteFileW(path); return FALSE;
    }
    CloseHandle(file);
    return TRUE;
}

static BOOL launch_game(void)
{
    wchar_t executable[PATH_CAPACITY], command[PATH_CAPACITY * 2];
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = { 0 };
    if (!path_join(executable, ARRAYSIZE(executable),
                   g_state.base_dir, GAME_EXE) ||
        !regular_nonempty_file(executable)) {
        MessageBoxW(g_state.window,
                    L"The game executable is missing from this distribution.",
                    APP_TITLE, MB_OK | MB_ICONERROR);
        return FALSE;
    }
    if (_snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
                     L"\"%s\" \"%s\"", executable, g_state.game_dir) < 0)
        return FALSE;
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL,
                        g_state.base_dir, &startup, &process)) {
        MessageBoxW(g_state.window, L"Mercenaries could not be started.",
                    APP_TITLE, MB_OK | MB_ICONERROR);
        return FALSE;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return TRUE;
}

static DWORD WINAPI install_worker(void *unused)
{
    wchar_t extractor[PATH_CAPACITY], reason[1024], backup[PATH_CAPACITY];
    wchar_t staging_parent[PATH_CAPACITY], *separator;
    DWORD exit_code = 1;
    HANDLE install_lock = INVALID_HANDLE_VALUE;
    BOOL owns_staging = FALSE;
    wchar_t lock_path[PATH_CAPACITY];
    SYSTEMTIME now;
    (void)unused;
    if (!path_join(extractor, ARRAYSIZE(extractor), g_state.base_dir,
                   EXTRACTOR_RELATIVE_PATH) ||
        !regular_nonempty_file(extractor)) {
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                 L"The bundled xdvdfs extractor is missing from this distribution.");
        goto finished;
    }
    wcscpy_s(staging_parent, ARRAYSIZE(staging_parent), g_state.staging_dir);
    separator = wcsrchr(staging_parent, L'\\');
    if (!separator) goto finished;
    *separator = L'\0';
    if (!ensure_directory_tree(staging_parent) ||
        !path_join(lock_path, ARRAYSIZE(lock_path), staging_parent, L".mercenaries-install.lock")) {
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error), L"The installation directory could not be created.");
        goto finished;
    }
    install_lock=CreateFileW(lock_path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,
                             FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    if(install_lock==INVALID_HANDLE_VALUE) {
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                 L"Another installation is already running here, or this folder is not writable.");
        goto finished;
    }
    owns_staging=TRUE;
    if (!remove_tree(g_state.staging_dir)) {
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                 L"The staging directory could not be created beside the game.");
        goto finished;
    }
    if (!run_extractor(extractor, g_state.selected_iso,
                       g_state.staging_dir, &exit_code) || exit_code != 0) {
        _snwprintf_s(g_state.error, ARRAYSIZE(g_state.error), _TRUNCATE,
                     L"ISO extraction failed (exit code %lu).", exit_code);
        goto finished;
    }
    if (g_state.window) PostMessageW(g_state.window, WM_INSTALL_PROGRESS, 92, 0);
    if (!validate_install(g_state.staging_dir, FALSE,
                          reason, ARRAYSIZE(reason))) {
        _snwprintf_s(g_state.error, ARRAYSIZE(g_state.error), _TRUNCATE,
                     L"The selected ISO is not a supported complete retail image.\r\n\r\n%s",
                     reason);
        goto finished;
    }
    /* Atomically close the cancellation window before activating verified files. */
    if (InterlockedCompareExchange(&g_state.install_phase, 3, 1) == 2)
        goto finished;
    if (g_state.window) PostMessageW(g_state.window, WM_INSTALL_PROGRESS, 96, 0);
    if (!write_install_marker(g_state.staging_dir)) {
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                 L"The completed installation could not be marked valid.");
        goto finished;
    }
    if (g_state.window) PostMessageW(g_state.window, WM_INSTALL_PROGRESS, 98, 0);
    if (directory_exists(g_state.game_dir)) {
        GetLocalTime(&now);
        _snwprintf_s(backup, ARRAYSIZE(backup), _TRUNCATE,
                     L"%s.invalid-%04u%02u%02u-%02u%02u%02u",
                     g_state.game_dir, now.wYear, now.wMonth, now.wDay,
                     now.wHour, now.wMinute, now.wSecond);
        if (!MoveFileExW(g_state.game_dir, backup, MOVEFILE_WRITE_THROUGH)) {
            wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                     L"The incomplete old installation could not be moved aside.");
            goto finished;
        }
    }
    if (!MoveFileExW(g_state.staging_dir, g_state.game_dir,
                     MOVEFILE_WRITE_THROUGH)) {
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                 L"The verified game files could not be activated.");
        goto finished;
    }
    if (g_state.window) PostMessageW(g_state.window, WM_INSTALL_PROGRESS, 100, 0);
    g_state.install_succeeded = TRUE;
    g_state.error[0] = L'\0';
finished:
    g_state.cancelled = install_cancelled();
    if (!g_state.install_succeeded && owns_staging && !remove_tree(g_state.staging_dir)) {
        g_state.cancelled = FALSE;
        wcscpy_s(g_state.error, ARRAYSIZE(g_state.error),
                 L"Temporary installation files could not be cleaned up. Please close other programs using them and retry.");
    }
    if(install_lock!=INVALID_HANDLE_VALUE) CloseHandle(install_lock);
    if (g_state.window) PostMessageW(g_state.window, WM_INSTALL_FINISHED, 0, 0);
    return 0;
}


static void refresh_setup(void)
{
    if (!g_state.window) return;
    SetWindowTextW(g_state.action_button, g_state.view.button);
    EnableWindow(g_state.close_button, !g_state.installing);
    EnableMenuItem(GetSystemMenu(g_state.window, FALSE), SC_CLOSE,
                   MF_BYCOMMAND | (g_state.installing ? MF_GRAYED : MF_ENABLED));
    EnableWindow(g_state.action_button, !g_state.view.cancelling &&
        InterlockedCompareExchange(&g_state.install_phase, 0, 0) != 3);
    InvalidateRect(g_state.window, NULL, FALSE);
    InvalidateRect(g_state.action_button, NULL, FALSE);
    InvalidateRect(g_state.close_button, NULL, FALSE);
}

static void begin_install(void)
{
    HANDLE thread;
    if (g_state.installing || !regular_nonempty_file(g_state.selected_iso)) return;
    g_state.installing = TRUE;
    g_state.install_succeeded = g_state.cancelled = FALSE;
    g_state.error[0] = L'\0';
    InterlockedExchange(&g_state.install_phase, 1);
    g_state.view = (setup_view){L"INSTALLING GAME FILES, PLEASE WAIT...",L"CANCEL",5,TRUE,FALSE,FALSE};
    refresh_setup();
    thread = CreateThread(NULL, 8 * 1024 * 1024, install_worker, NULL,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        g_state.installing = FALSE;
        InterlockedExchange(&g_state.install_phase, 0);
        g_state.view = (setup_view){L"INSTALLATION COULD NOT START. PLEASE RETRY.",L"BROWSE...",0,FALSE,FALSE,FALSE};
        refresh_setup();
        MessageBoxW(g_state.window, L"The installation worker could not be started.",
                    APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(thread);
}

static void choose_iso(HWND owner)
{
    OPENFILENAMEW dialog = { 0 };
    wchar_t path[PATH_CAPACITY] = L"";
    if (g_state.installing) return;
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Xbox disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrTitle = L"Select your retail Mercenaries ISO";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        wcscpy_s(g_state.selected_iso, ARRAYSIZE(g_state.selected_iso), path);
        begin_install();
    }
}

static void setup_action(void)
{
    if (g_state.installing) {
        if (InterlockedCompareExchange(&g_state.install_phase, 2, 1) == 1) {
            g_state.view.cancelling = TRUE;
            g_state.view.headline = L"CANCELLING INSTALLATION, PLEASE WAIT...";
            g_state.view.button = L"WAIT...";
            refresh_setup();
        }
    } else if (g_state.install_succeeded) {
        if (launch_game()) DestroyWindow(g_state.window);
    } else choose_iso(g_state.window);
}

static void layout_setup(HWND window)
{
    RECT client, r;
    int i;
    GetClientRect(window, &client);
    /* Follow the supplied header's clipped corner instead of painting an
     * opaque rectangle over the desktop outside the frame. */
    if(client.right>0 && client.bottom>0){
        int cut=MulDiv(client.right,66,SETUP_DESIGN_WIDTH);
        POINT outline[5]={{0,0},{client.right-cut,0},{client.right,cut},
                          {client.right,client.bottom},{0,client.bottom}};
        HRGN region=CreatePolygonRgn(outline,5,WINDING);
        if(region && !SetWindowRgn(window,region,TRUE))DeleteObject(region);
    }
    for (i=0;i<2;i++) {
        r=setup_art_rect(client.right,client.bottom,i);
        MoveWindow(i?g_state.close_button:g_state.action_button,
                   r.left,r.top,r.right-r.left,r.bottom-r.top,TRUE);
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps; RECT r;
        HDC dc=BeginPaint(window,&ps);
        GetClientRect(window,&r);
        if(r.right>0 && r.bottom>0) setup_art_paint(dc,r.right,r.bottom,&g_state.view);
        EndPaint(window,&ps);return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item=(const DRAWITEMSTRUCT *)lparam;
        setup_art_button(item->hDC,item->rcItem.right-item->rcItem.left,
                         item->rcItem.bottom-item->rcItem.top,&g_state.view,
                         item->CtlID==IDC_QUIT,item->itemState);
        return TRUE;
    }
    case WM_SIZE: layout_setup(window);InvalidateRect(window,NULL,FALSE);return 0;
    case WM_DPICHANGED: {
        const RECT *r=(const RECT *)lparam;
        SetWindowPos(window,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,
                     SWP_NOZORDER|SWP_NOACTIVATE);return 0;
    }
    case WM_NCHITTEST: {
        POINT p={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};RECT r,button;
        ScreenToClient(window,&p);GetClientRect(window,&r);
        button=setup_art_rect(r.right,r.bottom,TRUE);
        if(PtInRect(&button,p)) return HTCLIENT;
        /* Only the framed header acts as the draggable title bar. */
        if(p.y<MulDiv(r.bottom,SETUP_HEADER_HEIGHT,SETUP_DESIGN_HEIGHT)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_COMMAND:
        if (HIWORD(wparam)==BN_CLICKED) {
            if(LOWORD(wparam)==IDC_INSTALL) {setup_action();return 0;}
            if(LOWORD(wparam)==IDC_QUIT) {SendMessageW(window,WM_CLOSE,0,0);return 0;}
        }
        break;
    case WM_KEYDOWN:
        if(wparam==VK_ESCAPE) {SendMessageW(window,WM_CLOSE,0,0);return 0;}
        break;
    case WM_SYSCOMMAND:
        if((wparam&0xfff0)==SC_CLOSE && g_state.installing) return 0;
        break;
    case WM_QUERYENDSESSION: return !g_state.installing;
    case WM_CLOSE:
        /* Covers X, Alt+F4, taskbar Close and accessibility activation. */
        if(!g_state.installing) DestroyWindow(window);
        return 0;
    case WM_INSTALL_PROGRESS:
        if(g_state.installing) {
            g_state.view.progress=(unsigned)(wparam>100?100:wparam);
            if(wparam>=96 && !g_state.view.cancelling) g_state.view.button=L"WAIT...";
            refresh_setup();
        }
        return 0;
    case WM_INSTALL_FINISHED:
        g_state.installing=FALSE;
        InterlockedExchange(&g_state.install_phase,0);
        if(g_state.install_succeeded)
            g_state.view=(setup_view){L"GAME FILES INSTALLED SUCCESSFULLY!",L"LAUNCH",100,FALSE,TRUE,FALSE};
        else if(g_state.cancelled)
            g_state.view=(setup_view){L"INSTALLATION CANCELLED. SELECT AN ISO TO RETRY.",L"BROWSE...",0,FALSE,FALSE,FALSE};
        else
            g_state.view=(setup_view){L"INSTALLATION FAILED. PLEASE SELECT A VALID ISO.",L"BROWSE...",0,FALSE,FALSE,FALSE};
        refresh_setup();
        SetFocus(g_state.action_button);
        if(!g_state.install_succeeded && !g_state.cancelled)
            MessageBoxW(window,g_state.error[0]?g_state.error:L"Installation failed.",
                        APP_TITLE,MB_OK|MB_ICONERROR);
        return 0;
    case WM_DESTROY: PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

static HWND create_setup_window(HINSTANCE instance, const wchar_t *reason)
{
    WNDCLASSEXW cls={sizeof(cls)};RECT work;int w,h;HWND window;
    (void)reason;
    cls.lpfnWndProc=window_proc;cls.hInstance=instance;
    cls.hCursor=LoadCursorW(NULL,IDC_ARROW);
    cls.hIcon=mercenaries_app_icon(instance,1);
    cls.hIconSm=mercenaries_app_icon(instance,0);
    cls.lpszClassName=L"MercenariesRecompiledSetupWindow";
    if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return NULL;
    SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    /* Approximately 1158 logical pixels, constrained to the work area. */
    {HDC dc=GetDC(NULL);int dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(NULL,dc);
     w=MulDiv(1158,dpi,96);}
    if(w>(work.right-work.left)*9/10)w=(work.right-work.left)*9/10;
    h=MulDiv(w,SETUP_DESIGN_HEIGHT,SETUP_DESIGN_WIDTH);
    if(h>(work.bottom-work.top)*9/10) {h=(work.bottom-work.top)*9/10;w=MulDiv(h,SETUP_DESIGN_WIDTH,SETUP_DESIGN_HEIGHT);}
    g_state.view=(setup_view){L"PLEASE SELECT A VALID MERCENARIES .ISO FILE",L"BROWSE...",0,FALSE,FALSE,FALSE};
    window=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_CONTROLPARENT,cls.lpszClassName,APP_TITLE,
        WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,
        work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,
        w,h,NULL,NULL,instance,NULL);
    if(!window)return NULL;
    g_state.window=window;
    g_state.action_button=CreateWindowExW(0,L"BUTTON",L"BROWSE...",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,window,
        (HMENU)(INT_PTR)IDC_INSTALL,instance,NULL);
    g_state.close_button=CreateWindowExW(0,L"BUTTON",L"Close",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,window,
        (HMENU)(INT_PTR)IDC_QUIT,instance,NULL);
    layout_setup(window);
    return window;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    wchar_t *command_line, int show_command)
{
    wchar_t module_path[PATH_CAPACITY], reason[1024], *separator;
    DWORD length;
    MSG message = { 0 };
    HWND window;
    INITCOMMONCONTROLSEX controls = {
        sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS
    };
    (void)previous; (void)command_line;
    /* Dynamic lookup keeps older Wine versions compatible. */
    { typedef BOOL (WINAPI *dpi_awareness_fn)(HANDLE);
      dpi_awareness_fn set_dpi=(dpi_awareness_fn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext");
      if(set_dpi) set_dpi((HANDLE)(INT_PTR)-4); else SetProcessDPIAware(); }
    ZeroMemory(&g_state, sizeof(g_state));
    length = GetModuleFileNameW(NULL, module_path, ARRAYSIZE(module_path));
    if (!length || length >= ARRAYSIZE(module_path)) return 1;
    separator = wcsrchr(module_path, L'\\');
    if (!separator) return 1;
    *separator = L'\0';
    wcscpy_s(g_state.base_dir, ARRAYSIZE(g_state.base_dir), module_path);
    if (!path_join(g_state.game_dir, ARRAYSIZE(g_state.game_dir),
                   g_state.base_dir, GAME_RELATIVE_DIR) ||
        !path_join(g_state.staging_dir, ARRAYSIZE(g_state.staging_dir),
                   g_state.base_dir, L"game_files\\.mercenaries-installing"))
        return 1;
    {
        int argument_count = 0;
        wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(),
                                                  &argument_count);
        if (arguments && argument_count >= 3 && !_wcsicmp(arguments[1],L"--preview-ui")) {
            wchar_t path[PATH_CAPACITY];int i;BOOL ok=setup_art_init(instance);
            const setup_view views[]={
                {L"PLEASE SELECT A VALID MERCENARIES .ISO FILE",L"BROWSE...",0,FALSE,FALSE,FALSE},
                {L"INSTALLING GAME FILES, PLEASE WAIT...",L"CANCEL",27,TRUE,FALSE,FALSE},
                {L"GAME FILES INSTALLED SUCCESSFULLY!",L"LAUNCH",100,FALSE,TRUE,FALSE},
                {L"CANCELLING INSTALLATION, PLEASE WAIT...",L"WAIT...",27,TRUE,FALSE,TRUE}
            };
            for(i=0;ok && i<4;i++) {
                _snwprintf_s(path,ARRAYSIZE(path),_TRUNCATE,L"%s\\\\state-%d.png",arguments[2],i);
                ok=setup_art_export(path,SETUP_DESIGN_WIDTH,SETUP_DESIGN_HEIGHT,&views[i]);
            }
            if(ok) {
                _snwprintf_s(path,ARRAYSIZE(path),_TRUNCATE,L"%s\\\\state-small.png",arguments[2]);
                ok=setup_art_export(path,960,MulDiv(960,SETUP_DESIGN_HEIGHT,SETUP_DESIGN_WIDTH),&views[0]);
            }
            setup_art_shutdown();LocalFree(arguments);return ok?0:5;
        }
        if (arguments && argument_count >= 2 &&
            (!_wcsicmp(arguments[1], L"--validate-only") ||
             !_wcsicmp(arguments[1], L"--validate-full"))) {
            BOOL full = !_wcsicmp(arguments[1], L"--validate-full");
            BOOL valid = validate_install(g_state.game_dir, !full,
                                          reason, ARRAYSIZE(reason));
            LocalFree(arguments);
            return valid ? 0 : 2;
        }
        if (arguments && argument_count >= 3 &&
            !_wcsicmp(arguments[1], L"--install-iso")) {
            BOOL no_launch = argument_count >= 4 &&
                             !_wcsicmp(arguments[3], L"--no-launch");
            wcscpy_s(g_state.selected_iso, ARRAYSIZE(g_state.selected_iso),
                     arguments[2]);
            LocalFree(arguments);
            install_worker(NULL);
            if (!g_state.install_succeeded) return 3;
            return no_launch || launch_game() ? 0 : 4;
        }
        if (arguments) LocalFree(arguments);
    }
    if (validate_install(g_state.game_dir, TRUE,
                         reason, ARRAYSIZE(reason)))
        return launch_game() ? 0 : 1;

    InitCommonControlsEx(&controls);
    if(!setup_art_init(instance)) {
        MessageBoxW(NULL,L"The setup artwork could not be loaded.",APP_TITLE,MB_OK|MB_ICONERROR);
        return 1;
    }
    window = create_setup_window(instance, reason);
    if (!window) {setup_art_shutdown();return 1;}
    ShowWindow(window, show_command);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE) {
            SendMessageW(window,WM_CLOSE,0,0);continue;
        }
        if(message.message==WM_KEYDOWN && message.wParam==VK_RETURN) {
            HWND focused=GetFocus();
            if(focused==g_state.close_button) SendMessageW(window,WM_CLOSE,0,0);
            else setup_action();
            continue;
        }
        if(!IsDialogMessageW(window,&message)) {
            TranslateMessage(&message);DispatchMessageW(&message);
        }
    }
    setup_art_shutdown();
    return (int)message.wParam;
}
