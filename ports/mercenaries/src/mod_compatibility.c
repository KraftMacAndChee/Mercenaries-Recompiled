#include "mod_compatibility.h"
#include "kernel/xbox_memory_layout.h"
#include <string.h>

static int expanded_world_properties;

void recomp_mod_compatibility_init(void)
{
    char path[MAX_PATH];
    int extended_graphics = 0;
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    expanded_world_properties = 0;
    if (length && length < MAX_PATH) {
        char *slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = '\0';
            if (strcat_s(path, MAX_PATH, "modcompatibility.ini") == 0) {
                extended_graphics = GetPrivateProfileIntA(
                    "Mods", "extended_graphics_memory", 0, path) == 1;
                expanded_world_properties = GetPrivateProfileIntA(
                    "Mods", "expanded_world_properties", 0, path) == 1;
            }
        }
    }
    xbox_SetExtendedGraphicsMemory(extended_graphics);
}

int recomp_mod_expanded_world_properties(void)
{
    return expanded_world_properties;
}
