#ifndef MERCENARIES_MOD_COMPATIBILITY_H
#define MERCENARIES_MOD_COMPATIBILITY_H

#include <stdint.h>

/* Read once before memory initialization; settings take effect on restart. */
void recomp_mod_compatibility_init(void);
int recomp_mod_expanded_world_properties(void);

/* Unmapped identifiers retain retail behavior. */
int recomp_mod_support_faction(uint32_t key);
int recomp_mod_impact_stun(uint32_t key);
int recomp_mod_impact_stun_enabled(void);
int recomp_mod_extra_grenade_slot(void);
int recomp_mod_faction_support(void);

#endif
