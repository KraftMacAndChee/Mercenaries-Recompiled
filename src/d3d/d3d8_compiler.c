/* Wine's built-in HLSL compiler does not implement all intrinsics used by
 * the NV2A translator. Load the SDK redistributable only on Wine. Native
 * Windows retains its existing imported D3DCompile function and DLL. */
#include "d3d8_compiler.h"
#include "kernel/preview_log.h"
#include <wchar.h>

static INIT_ONCE compiler_once = INIT_ONCE_STATIC_INIT;
static pD3DCompile selected_compiler = D3DCompile;

static BOOL CALLBACK select_compiler(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    WCHAR path[MAX_PATH];
    DWORD length;
    WCHAR *slash;
    HMODULE module;
    pD3DCompile compile;
    static const WCHAR suffix[] = L"compat\\d3dcompiler_47_native.dll";
    (void)once; (void)parameter; (void)context;
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        xbox_preview_log_event("shader-compiler", "native Windows system D3DCompile retained");
        return TRUE;
    }
    length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) goto unavailable;
    slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash + 1 - path) + sizeof(suffix)/sizeof(suffix[0]) > MAX_PATH)
        goto unavailable;
    wcscpy(slash + 1, suffix);
    /* A distinct basename prevents Wine from substituting its built-in DLL.
     * Absolute application-relative loading does not search the working dir. */
    module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) goto unavailable;
    compile = (pD3DCompile)GetProcAddress(module, "D3DCompile");
    if (!compile) { FreeLibrary(module); goto unavailable; }
    selected_compiler = compile; /* Module intentionally lives with the process. */
    xbox_preview_log_event("shader-compiler", "Wine SDK native D3DCompile loaded");
    return TRUE;
unavailable:
    xbox_preview_log_event("shader-compiler", "Wine compatibility compiler unavailable (error=%lu); using system compiler", GetLastError());
    return TRUE;
}

HRESULT d3d8_compile_shader(LPCVOID source, SIZE_T size, LPCSTR name,
    const D3D_SHADER_MACRO *defines, ID3DInclude *include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors)
{
    InitOnceExecuteOnce(&compiler_once, select_compiler, NULL, NULL);
    return selected_compiler(source, size, name, defines, include, entry,
                             target, flags1, flags2, code, errors);
}
