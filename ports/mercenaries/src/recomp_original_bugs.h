#ifndef MERCENARIES_RECOMP_ORIGINAL_BUGS_H
#define MERCENARIES_RECOMP_ORIGINAL_BUGS_H

#include <stdint.h>

/* Only confirmed original-game bugs belong here. Port correctness fixes do not
 * depend on this policy. Add each optional correction explicitly. */
typedef enum recomp_original_bug_fix {
    RECOMP_FIX_NW_MAFIA2_MUSIC,
    RECOMP_FIX_DATELINE_TYPING_SOUND,
    RECOMP_FIX_SKY_CLOUD_TRANSITION,
    RECOMP_FIX_JENNIFER_ALT_BACKPACK
} recomp_original_bug_fix;

int recomp_original_bug_fix_enabled(recomp_original_bug_fix fix);
uint32_t recomp_original_bug_cue_alias(uint32_t name);
uint32_t recomp_original_bug_dateline_chars(uint32_t previous);

uint32_t recomp_original_bug_accessory_show_slot(uint32_t player_model, uint32_t accessory_model);

void recomp_original_bug_sky_blend(float outgoing_weight);
void recomp_original_bug_cloud_params(volatile uint32_t *incoming, volatile uint32_t *outgoing);
void recomp_original_bug_cloud_colors(volatile float *rgba);

#endif
