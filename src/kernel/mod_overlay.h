#ifndef XBOX_MOD_OVERLAY_H
#define XBOX_MOD_OVERLAY_H
#include <windows.h>
BOOL xbox_mod_overlay_init(void);
BOOL xbox_mod_overlay_path(const WCHAR *relative, WCHAR *output, DWORD capacity);
BOOL xbox_mod_overlay_readonly(const WCHAR *path);
void xbox_mod_overlay_find_data(HANDLE directory, WIN32_FIND_DATAW *data);
const WCHAR *xbox_mod_cache_root(void);
#endif
