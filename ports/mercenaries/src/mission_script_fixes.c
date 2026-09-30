#include "mission_script_fixes.h"
#include "wmd_inspectors_script.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

char *recomp_patch_mission_script(const char *source, size_t length, size_t *out_length)
{
    uint32_t hash = 2166136261u;
    size_t i;
    char *result;
    *out_length = length;
    /* RsLuaScript passes strlen - 1; accept the complete text as well.
     * Fingerprint the supported retail chunk, leaving mods and other missions alone. */
    if ((length != 24492u && length != 24493u) || !source)
        return NULL;
    for (i = 0; i < length; ++i)
        hash = (hash ^ (unsigned char)source[i]) * 16777619u;
    if (hash != (length == 24492u ? 0x518259E7u : 0x3D339017u))
        return NULL;
    result = (char *)malloc(length + sizeof(wmd_inspectors_script));
    if (!result)
        return NULL;
    memcpy(result, source, length);
    memcpy(result + length, wmd_inspectors_script, sizeof(wmd_inspectors_script));
    *out_length = length + sizeof(wmd_inspectors_script) - 1u;
    return result;
}
