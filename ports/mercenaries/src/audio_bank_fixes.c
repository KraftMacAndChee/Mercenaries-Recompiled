/* Correct the menu theme category in the loaded bank, before XACT creates
 * voices. Menu effects retain Interface; no game files are written back. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

static uint16_t bank_u16(const uint8_t *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t bank_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static int fix_menu_music_category(uint8_t *bank, uint32_t size)
{
    static const char theme[] = "shell_stream";
    uint32_t target = UINT32_MAX;
    if (size < 56 || memcmp(bank, "SDBK", 4) || bank_u16(bank + 4) != 11 ||
        bank_u32(bank + 20) != size || memcmp(bank + 40, "shell", 6))
        return 0;

    const uint32_t sounds = bank_u16(bank + 28);
    const uint32_t cues = bank_u16(bank + 30);
    const uint32_t table_end = 56 + (cues + sounds) * 20;
    if (table_end > size) return 0;

    for (uint32_t i = 0; i < cues; ++i) {
        const uint8_t *cue = bank + 56 + i * 20;
        const uint32_t name = bank_u32(cue + 4);
        if (name < table_end || name > size || sizeof(theme) > size - name ||
            memcmp(bank + name, theme, sizeof(theme)))
            continue;
        /* Ambiguous duplicate names or invalid sound references pass through. */
        if (target != UINT32_MAX || bank_u16(cue + 2) >= sounds) return 0;
        target = i;
    }
    if (target == UINT32_MAX) return 0;
    const uint32_t sound = bank_u16(bank + 56 + target * 20 + 2);
    for (uint32_t i = 0; i < cues; ++i)
        if (i != target && bank_u16(bank + 56 + i * 20 + 2) == sound)
            return 0; /* Do not change an effect sharing a modded definition. */

    uint8_t *category = bank + 56 + (cues + sound) * 20 + 10;
    if (*category != 2) return 0; /* Preserve other authored categories. */
    *category = 0; /* RedSoundSystem::eInterface -> eMusic */
    return 1;
}

void recomp_fix_menu_music_bank(uint32_t address, uint32_t size)
{
    if (!g_xbox_mem_offset || size > 0x04000000u || address < 0x10000u ||
        address > 0x04000000u - size)
        return;
    fix_menu_music_category((uint8_t *)((uintptr_t)address + g_xbox_mem_offset), size);
}
