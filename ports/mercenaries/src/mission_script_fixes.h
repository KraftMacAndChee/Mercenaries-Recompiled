#ifndef MERCENARIES_MISSION_SCRIPT_FIXES_H
#define MERCENARIES_MISSION_SCRIPT_FIXES_H
#include <stddef.h>
/* Returns an owned buffer, or NULL to keep the original. The caller frees it. */
char *recomp_patch_mission_script(const char *source, size_t length, size_t *out_length);
#endif
