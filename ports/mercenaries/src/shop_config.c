/* The shop can exceed PblConfig's shared 512-line index before reaching its
 * 128-item capacity. Give this one parser a file-sized index, including its
 * terminating section, so it cannot overwrite the following engine globals.
 */
#include "kernel/xbox_memory_layout.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;

static uint32_t *shop_word(uint32_t address)
{
    return (uint32_t *)((uintptr_t)address + g_xbox_mem_offset);
}

void recomp_shop_config_storage(uint32_t config, uint32_t data, uint32_t size)
{
    const unsigned char *text = (const unsigned char *)shop_word(data);
    /* Each CR or LF can end a line. Counting CRLF twice is harmless and also
     * covers blank lines, comments, an empty file and a final unterminated line.
     */
    uint64_t capacity = 2;
    for (uint32_t i = 0; i < size; ++i)
        if (text[i] == '\r' || text[i] == '\n') ++capacity;
    if (capacity <= 512) return;
    if (capacity > UINT32_MAX / 8u) {
        fprintf(stderr, "[SHOP] Configuration line index is too large\n");
        exit(86);
    }
    uint32_t storage = xbox_HeapAlloc((uint32_t)capacity * 8u, 16u);
    if (!storage) {
        fprintf(stderr, "[SHOP] Cannot allocate configuration line index\n");
        exit(86);
    }
    *shop_word(config + 0x10) = (uint32_t)capacity;
    *shop_word(config + 0x14) = storage;
}

void recomp_shop_config_release(uint32_t config)
{
    uint32_t storage = *shop_word(config + 0x14);
    if (storage != 0x00641F30u) xbox_HeapFree(storage);
    *shop_word(config + 0x10) = 512;
    *shop_word(config + 0x14) = 0x00641F30u;
}
