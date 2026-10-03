#include "mod_compatibility.h"
#include "kernel/xbox_memory_layout.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int expanded_world_properties;
static int faction_support, impact_stun_ammo, extra_grenade_slot;

enum { MOD_MAPPING_LIMIT = 512, MOD_SECTION_SIZE = 65536 };
typedef struct { uint32_t key; int value; } mod_mapping;
static mod_mapping support[MOD_MAPPING_LIMIT], impact[MOD_MAPPING_LIMIT];
static unsigned support_count, impact_count;

static uint32_t identifier_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    if (!*text) return 0;
    while (*text) hash = (hash ^ ((unsigned char)*text++ | 32u)) * 16777619u;
    return hash;
}

static void config_error(const char *section, const char *entry)
{
    fprintf(stderr, "[MOD-COMPATIBILITY] Invalid [%s] entry: %s\n", section, entry);
    exit(86);
}

static int faction_number(const char *name)
{
    static const char *names[] = {"", "player", "nk", "sk", "mafia", "china", "allies", "civilian"};
    for (int i = 2; i <= 7; ++i)
        if (!_stricmp(name, names[i])) return i;
    return 0;
}

static void read_mappings(const char *path, const char *section,
                          mod_mapping *entries, unsigned *count, int factions)
{
    char *buffer = (char *)malloc(MOD_SECTION_SIZE);
    if (!buffer) config_error(section, "out of memory");
    DWORD size = GetPrivateProfileSectionA(section, buffer, MOD_SECTION_SIZE, path);
    if (size >= MOD_SECTION_SIZE - 2) config_error(section, "section too large");
    for (char *entry = buffer; *entry; entry += strlen(entry) + 1) {
        char *equals = strchr(entry, '=');
        if (!equals || equals == entry) config_error(section, entry);
        size_t length = (size_t)(equals - entry);
        if (length >= 256) config_error(section, "identifier too long");
        char key[256];
        memcpy(key, entry, length); key[length] = 0;
        for (size_t i = 0; i < length; ++i)
            if ((unsigned char)key[i] <= 32 || (unsigned char)key[i] >= 127)
                config_error(section, "identifiers must be printable ASCII without spaces");
        int value = factions ? faction_number(equals + 1) :
                    (!strcmp(equals + 1, "1") ? 1 : !strcmp(equals + 1, "0") ? 0 : -1);
        if ((factions && !value) || value < 0) config_error(section, entry);
        uint32_t hash = identifier_hash(key);
        for (unsigned i = 0; i < *count; ++i)
            if (entries[i].key == hash) config_error(section, "duplicate identifier/hash");
        if (*count == MOD_MAPPING_LIMIT) config_error(section, "too many entries");
        entries[*count].key = hash; entries[*count].value = value; ++*count;
    }
    free(buffer);
}

int recomp_mod_support_faction(uint32_t key)
{
    if (faction_support)
        for (unsigned i = 0; i < support_count; ++i)
            if (support[i].key == key) return support[i].value;
    return 0;
}

int recomp_mod_impact_stun(uint32_t key)
{
    if (impact_stun_ammo)
        for (unsigned i = 0; i < impact_count; ++i)
            if (impact[i].key == key) return impact[i].value;
    return 0;
}

int recomp_mod_impact_stun_enabled(void) { return impact_stun_ammo; }

int recomp_mod_extra_grenade_slot(void) { return extra_grenade_slot; }
int recomp_mod_faction_support(void) { return faction_support; }


void recomp_mod_compatibility_init(void)
{
    char path[MAX_PATH];
    int extended_graphics = 0;
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    expanded_world_properties = 0;
    faction_support = impact_stun_ammo = extra_grenade_slot = 0;
    support_count = impact_count = 0;
    if (length && length < MAX_PATH) {
        char *slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = '\0';
            if (strcat_s(path, MAX_PATH, "modcompatibility.ini") == 0) {
                extended_graphics = GetPrivateProfileIntA(
                    "Mods", "extended_graphics_memory", 0, path) == 1;
                expanded_world_properties = GetPrivateProfileIntA(
                    "Mods", "expanded_world_properties", 0, path) == 1;
                faction_support = GetPrivateProfileIntA("Mods", "faction_support", 0, path) == 1;
                impact_stun_ammo = GetPrivateProfileIntA("Mods", "impact_stun_ammo", 0, path) == 1;
                extra_grenade_slot = GetPrivateProfileIntA("Mods", "extra_grenade_slot", 0, path) == 1;
                if (faction_support)
                    read_mappings(path, "SupportFactions", support, &support_count, 1);
                if (impact_stun_ammo)
                    read_mappings(path, "ImpactStunAmmo", impact, &impact_count, 0);
            }
        }
    }
    xbox_SetExtendedGraphicsMemory(extended_graphics);
}

int recomp_mod_expanded_world_properties(void)
{
    return expanded_world_properties;
}
