#include "mod_extensions.h"
#include "mod_compatibility.h"
#include "kernel/xbox_memory_layout.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST(type, address) (*(type *)((uintptr_t)(uint32_t)(address) + g_xbox_mem_offset))
#define U32(address) GUEST(uint32_t, address)
#define F32(address) GUEST(float, address)

enum {
    PLAYER_VTABLE = 0x2E32B8, SECONDARY_ARRAY = 0x7C0,
    WEAPON_AMMO_TEMPLATE = 0x1A4, FACTION_TABLE = 0x323338,
    PLAYER_FACTION = 1, MAFIA_FACTION = 4,
    STUN_EFFECT = 0xECA96359u
};

/* Only player initialization registers an expanded array. Shared human methods
 * keep their retail array for soldiers, including during player teardown. */
typedef struct inventory_storage {
    uint32_t owner, slots;
    struct inventory_storage *next;
} inventory_storage;
static inventory_storage *inventories;

static inventory_storage *inventory(uint32_t actor)
{
    for (inventory_storage *it = inventories; it; it = it->next)
        if (it->owner == actor) return it;
    return NULL;
}

void recomp_mod_player_inventory_begin(uint32_t player)
{
    if (!recomp_mod_extra_grenade_slot() || inventory(player)) return;
    inventory_storage *it = (inventory_storage *)calloc(1, sizeof(*it));
    uint32_t slots = xbox_HeapAlloc(3u * sizeof(uint32_t), 16u);
    if (!it || !slots) {
        fprintf(stderr, "[MOD-COMPATIBILITY] Cannot allocate player grenade slots\n");
        exit(86);
    }
    memset((void *)((uintptr_t)slots + g_xbox_mem_offset), 0, 12);
    it->owner = player; it->slots = slots; it->next = inventories; inventories = it;
}

void recomp_mod_player_inventory_end(uint32_t player)
{
    inventory_storage **link = &inventories;
    while (*link) {
        inventory_storage *it = *link;
        if (it->owner == player) {
            *link = it->next;
            xbox_HeapFree(it->slots);
            free(it);
            return;
        }
        link = &it->next;
    }
}

uint32_t recomp_mod_secondary_base(uint32_t actor)
{
    inventory_storage *it = inventory(actor);
    return it ? it->slots : actor + SECONDARY_ARRAY;
}

uint32_t recomp_mod_secondary_capacity(uint32_t actor)
{
    return inventory(actor) ? 3u : 2u;
}

static int weapon_faction(uint32_t weapon)
{
    return weapon ? recomp_mod_support_faction(U32(weapon + WEAPON_AMMO_TEMPLATE)) : 0;
}

static int faction_allowed(int faction)
{
    /* sub_00095EE0 classifies values <= the retail -0.6 threshold as hostile. */
    float standing = F32(FACTION_TABLE + 0x1C + (faction * 8 + PLAYER_FACTION) * 4);
    return standing > F32(0x2E6CE8);
}

int recomp_mod_weapon_allowed(uint32_t weapon, uint32_t actor)
{
    int faction = weapon_faction(weapon);
    if (!faction || !actor || U32(actor) != PLAYER_VTABLE) return 1;
    return faction_allowed(faction);
}

int recomp_mod_weapon_fixed_price(uint32_t weapon)
{
    int faction = weapon_faction(weapon);
    return faction && faction != MAFIA_FACTION;
}

static int pda_faction(uint32_t shop, uint32_t index)
{
    if (!recomp_mod_faction_support() || index >= U32(shop + 0x1794) || index >= 128u)
        return 0;
    uint32_t catalog_index = U32(shop + 0x1194 + index * 12);
    if (catalog_index >= U32(shop + 0x1190) || catalog_index >= 128u) return 0;
    return recomp_mod_support_faction(U32(shop + 0x198 + catalog_index * 32));
}

int recomp_mod_pda_allowed(uint32_t shop, uint32_t index)
{
    if (!recomp_mod_faction_support()) return 1;
    int faction = pda_faction(shop, index);
    return faction_allowed(faction ? faction : MAFIA_FACTION);
}

int recomp_mod_pda_fixed_price(uint32_t shop, uint32_t index)
{
    int faction = pda_faction(shop, index);
    return faction && faction != MAFIA_FACTION;
}

int recomp_mod_pda_has_available_supplier(uint32_t shop)
{
    if (!recomp_mod_faction_support()) return 0;
    uint32_t count = U32(shop + 0x1794);
    if (count > 128u) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        int faction = pda_faction(shop, i);
        if (faction && faction != MAFIA_FACTION && faction_allowed(faction)) return 1;
    }
    return 0;
}

int recomp_mod_projectile_impact_stun(uint32_t projectile)
{
    if (!recomp_mod_impact_stun_enabled()) return 0;
    /* Match the template Name as retail serialization does (0x1ED370):
     * Actor +8 -> PPD record +4 -> sorted inherited property tables.
     * The record's +0x28 identifies the instance, not its template. */
    uint32_t record = U32(projectile + 8);
    if (!record) return 0;
    uint32_t properties = U32(record + 4);
    for (unsigned depth = 0; properties && depth < 64; ++depth) {
        int low = 0, high = GUEST(int16_t, properties + 4) - 1;
        uint32_t entries = U32(properties + 8);
        while (low <= high) {
            int middle = low + (high - low) / 2;
            uint32_t key = U32(entries + middle * 8);
            if (key == 0x8D39BDE6u)
                return recomp_mod_impact_stun(U32(entries + middle * 8 + 4));
            if (key < 0x8D39BDE6u) low = middle + 1;
            else high = middle - 1;
        }
        properties = U32(properties);
    }
    return 0;
}

void recomp_mod_projectile_init(uint32_t projectile)
{
    if (!recomp_mod_projectile_impact_stun(projectile)) return;
    /* Retain the authored trajectory; use the retail stun effect and its
     * zero-damage payload instead of the launcher's explosive payload. */
    F32(projectile + 0x174) = F32(projectile + 0x178) = 0;
    F32(projectile + 0x17C) = F32(projectile + 0x180) = 0;
    F32(projectile + 0x18C) = F32(projectile + 0x190) = 0;
    U32(projectile + 0x1A0) = STUN_EFFECT;
    U32(projectile + 0x1A4) = STUN_EFFECT;
}
