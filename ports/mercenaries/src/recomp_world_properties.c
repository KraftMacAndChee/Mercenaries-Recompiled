/* Permanent world properties use eight-byte key/value records. The retail
 * 40,000-record array ends immediately before the PPD object array. Large
 * modded worlds need a separate allocation, not writes into that neighbor.
 */
#include "kernel/xbox_memory_layout.h"
#include "mod_compatibility.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RETAIL_PROPERTY_BASE 0x004434E0u
#define RETAIL_PROPERTY_COUNT 40000u
#define MOD_PROPERTY_COUNT 262144u
#define PROPERTY_STRIDE 8u

static uint32_t mod_property_base;

static void property_pool_fail(const char *reason, uint32_t value)
{
    fprintf(stderr, "[WORLD-PROPERTIES] %s: %u (0x%08X)\n", reason, value, value);
    fflush(stderr);
    exit(86);
}

uint32_t recomp_world_property_address(uint32_t index)
{
    uint32_t capacity = RETAIL_PROPERTY_COUNT;
    uint32_t base = RETAIL_PROPERTY_BASE;
    if (recomp_mod_expanded_world_properties()) {
        capacity = MOD_PROPERTY_COUNT;
        if (!mod_property_base) {
            mod_property_base = xbox_HeapAlloc(capacity * PROPERTY_STRIDE, 16u);
            if (!mod_property_base)
                property_pool_fail("cannot allocate mod property pool", capacity);
            fprintf(stderr, "[WORLD-PROPERTIES] mod pool=%08X capacity=%u\n",
                    mod_property_base, capacity);
        }
        base = mod_property_base;
    }
    /* Empty objects can point one past the last record. Writes cannot. */
    if (index > capacity)
        property_pool_fail("property index exceeds capacity", index);
    return base + index * PROPERTY_STRIDE;
}

void recomp_world_property_check_write(uint32_t address)
{
    uint32_t base = recomp_world_property_address(0u);
    uint32_t capacity = recomp_mod_expanded_world_properties() ?
                        MOD_PROPERTY_COUNT : RETAIL_PROPERTY_COUNT;
    if (address < base || (address - base) % PROPERTY_STRIDE ||
        (address - base) / PROPERTY_STRIDE >= capacity)
        property_pool_fail("property write outside pool", address);
}
