#ifndef MERCENARIES_MOD_EXTENSIONS_H
#define MERCENARIES_MOD_EXTENSIONS_H
#include <stdint.h>
void recomp_mod_player_inventory_begin(uint32_t player);
void recomp_mod_player_inventory_end(uint32_t player);
uint32_t recomp_mod_secondary_base(uint32_t actor);
uint32_t recomp_mod_secondary_capacity(uint32_t actor);
int recomp_mod_weapon_allowed(uint32_t weapon, uint32_t actor);
int recomp_mod_weapon_fixed_price(uint32_t weapon);
int recomp_mod_pda_allowed(uint32_t shop, uint32_t index);
int recomp_mod_pda_fixed_price(uint32_t shop, uint32_t index);
int recomp_mod_pda_has_available_supplier(uint32_t shop);
int recomp_mod_projectile_impact_stun(uint32_t projectile);
void recomp_mod_projectile_init(uint32_t projectile);
#endif
