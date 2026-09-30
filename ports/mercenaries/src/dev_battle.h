#ifndef MERCENARIES_DEV_BATTLE_H
#define MERCENARIES_DEV_BATTLE_H
#include <stdint.h>
#include "dev_menu.h"
/* One immutable request crosses from the native panel to the game thread. */
#define DEV_SPAWN_INDEX_MASK 0x3ffu
#define DEV_SPAWN_TROOP 0x400u
#define DEV_SPAWN_FACTION_SHIFT 11u
#define DEV_SPAWN_CREW_SHIFT 14u
#define DEV_SPAWN_COUNT_SHIFT 16u
#define DEV_BATTLE_UNTARGETABLE 1u
#define DEV_BATTLE_PASSIVE 2u
unsigned recomp_dev_battle_flags(void);
void recomp_dev_battle_set(unsigned flags);
void recomp_dev_battle_refresh_player(void);
int recomp_dev_battle_protected(uint32_t ai, uint32_t target);
const DevVehicle *recomp_dev_troop_at(unsigned index);
unsigned recomp_dev_troop_count(void);
#endif
