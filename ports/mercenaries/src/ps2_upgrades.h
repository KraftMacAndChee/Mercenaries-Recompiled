#ifndef MERCENARIES_PS2_UPGRADES_H
#define MERCENARIES_PS2_UPGRADES_H
#include <stdint.h>
uint32_t recomp_ps2_sound_bank(uint32_t original, uint32_t size_address);
uint32_t recomp_ps2_cue_index(uint32_t bank, uint32_t index);
uint32_t recomp_ps2_texture(uint32_t original);
void recomp_ps2_wave(uint32_t sound, uint32_t event);
uint32_t recomp_ps2_wave_data(uint32_t entry);
#endif
