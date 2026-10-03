/* Immutable launch-time file overlay. Directory placeholders expose the merged
 * namespace; file opens and enumeration metadata resolve to their real sources.
 */
#include "mod_overlay.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct
{
    WCHAR *relative, *source;
} mod_file;
static mod_file *entries;
static uint32_t entry_count;
static WCHAR namespace_root[MAX_PATH], cache_root[MAX_PATH];
static int compare(const void *a, const void *b)
{
    return _wcsicmp(((const mod_file *)a)->relative, ((const mod_file *)b)->relative);
}
static const mod_file *lookup(const WCHAR *relative)
{
    mod_file key = {(WCHAR *)relative, NULL};
    return bsearch(&key, entries, entry_count, sizeof(*entries), compare);
}
static BOOL safe_relative(const WCHAR *p)
{
    if (!*p || *p == L'\\' || *p == L'/' || wcschr(p, L':') || wcschr(p, L'/'))
        return FALSE;
    while (*p)
    {
        const WCHAR *end = wcschr(p, L'\\');
        size_t n = end ? (size_t)(end - p) : wcslen(p);
        if (!n || (n == 1 && p[0] == L'.') || (n == 2 && p[0] == L'.' && p[1] == L'.'))
            return FALSE;
        if (!end)
            break;
        p = end + 1;
    }
    return TRUE;
}
BOOL xbox_mod_overlay_init(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"MERCENARIES_MOD_OVERLAY", path, MAX_PATH);
    if (!n)
        return TRUE;
    if (n >= MAX_PATH || entries)
        return FALSE;
    FILE *f = _wfopen(path, L"rb");
    uint32_t magic, count;
    if (!f)
        return FALSE;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x31444f4d || fread(&count, 4, 1, f) != 1 ||
        !count || count > 1000000)
        goto error;
    entries = calloc(count, sizeof(*entries));
    if (!entries)
        goto error;
    entry_count = count;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t a, b;
        if (fread(&a, 4, 1, f) != 1 || fread(&b, 4, 1, f) != 1 || !a || !b || a >= MAX_PATH ||
            b >= MAX_PATH)
            goto error;
        entries[i].relative = calloc(a + 1, sizeof(WCHAR));
        entries[i].source = calloc(b + 1, sizeof(WCHAR));
        if (!entries[i].relative || !entries[i].source ||
            fread(entries[i].relative, 2, a, f) != a || fread(entries[i].source, 2, b, f) != b)
            goto error;
        if (wcslen(entries[i].relative) != a || wcslen(entries[i].source) != b ||
            !safe_relative(entries[i].relative))
            goto error;
    }
    if (fgetc(f) != EOF)
        goto error;
    qsort(entries, count, sizeof(*entries), compare);
    for (uint32_t i = 1; i < count; ++i)
        if (!compare(entries + i - 1, entries + i))
            goto error;
    WCHAR *slash = wcsrchr(path, L'\\');
    if (!slash)
        goto error;
    *slash = 0;
    if (swprintf_s(namespace_root, MAX_PATH, L"%s\\namespace", path) < 0)
        goto error;
    n = GetEnvironmentVariableW(L"MERCENARIES_MOD_CACHE", cache_root, MAX_PATH);
    if (!n || n >= MAX_PATH)
        goto error;
    fclose(f);
    return TRUE;
error:
    fclose(f);
    for (uint32_t i = 0; i < entry_count; ++i)
    {
        free(entries[i].relative);
        free(entries[i].source);
    }
    free(entries);
    entries = NULL;
    entry_count = 0;
    namespace_root[0] = cache_root[0] = 0;
    return FALSE;
}
const WCHAR *xbox_mod_cache_root(void) { return cache_root[0] ? cache_root : NULL; }
BOOL xbox_mod_overlay_path(const WCHAR *relative, WCHAR *output, DWORD capacity)
{
    if (!entries)
        return FALSE;
    while (*relative == L'\\')
        ++relative;
    // An active overlay must never fall back to the retail tree on invalid paths.
    if ((*relative && !safe_relative(relative)) || !capacity)
    {
        if (capacity)
            output[0] = 0;
        return TRUE;
    }
    const mod_file *file = lookup(relative);
    const size_t required =
        file ? wcslen(file->source) + 1 : wcslen(namespace_root) + wcslen(relative) + 2;
    if (required > capacity)
    {
        output[0] = 0;
        return TRUE;
    }
    if (file)
        wcscpy_s(output, capacity, file->source);
    else
        swprintf_s(output, capacity, L"%s\\%s", namespace_root, relative);
    return TRUE;
}
BOOL xbox_mod_overlay_readonly(const WCHAR *path)
{
    if (!entries)
        return FALSE;
    size_t n = wcslen(namespace_root);
    if (!_wcsnicmp(path, namespace_root, n) && (!path[n] || path[n] == L'\\'))
        return TRUE;
    for (uint32_t i = 0; i < entry_count; ++i)
        if (!_wcsicmp(path, entries[i].source))
            return TRUE;
    return FALSE;
}
void xbox_mod_overlay_find_data(HANDLE directory, WIN32_FIND_DATAW *data)
{
    if (!entries || (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return;
    WCHAR parent[MAX_PATH], relative[MAX_PATH];
    DWORD n = GetFinalPathNameByHandleW(directory, parent, MAX_PATH, FILE_NAME_NORMALIZED);
    if (!n || n >= MAX_PATH)
        return;
    WCHAR *p = parent;
    if (!wcsncmp(p, L"\\\\?\\", 4))
        p += 4;
    size_t root = wcslen(namespace_root);
    if (_wcsnicmp(p, namespace_root, root) || (p[root] && p[root] != L'\\'))
        return;
    p += root;
    if (*p == L'\\')
        ++p;
    if (swprintf_s(relative, MAX_PATH, *p ? L"%s\\%s" : L"%s%s", p, data->cFileName) < 0)
        return;
    const mod_file *file = lookup(relative);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (file && GetFileAttributesExW(file->source, GetFileExInfoStandard, &info))
    {
        data->dwFileAttributes = info.dwFileAttributes | FILE_ATTRIBUTE_READONLY;
        data->ftCreationTime = info.ftCreationTime;
        data->ftLastAccessTime = info.ftLastAccessTime;
        data->ftLastWriteTime = info.ftLastWriteTime;
        data->nFileSizeHigh = info.nFileSizeHigh;
        data->nFileSizeLow = info.nFileSizeLow;
    }
}
