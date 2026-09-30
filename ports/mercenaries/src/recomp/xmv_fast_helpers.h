#ifndef MERCENARIES_XMV_FAST_HELPERS_H
#define MERCENARIES_XMV_FAST_HELPERS_H

/* Mercenaries XMV hot leaf helpers. */
/* These original routines are tiny, but translated calls cost more than their
 * useful work and occur tens of thousands of times per decoded frame. */
static __forceinline void recomp_xmv_get_bit_fast(void)
{
    const uint32_t reader = g_ecx;
    uint32_t available = MEM32(reader + 4u);
    if (available != 0u) {
        --available;
        MEM32(reader + 4u) = available;
        g_ecx = available;
        g_eax = (MEM32(reader) >> (available & 31u)) & 1u;
    } else {
        uint32_t cursor = MEM32(reader + 8u);
        const uint32_t word = MEM32(cursor);
        cursor += 4u;
        MEM32(reader) = word;
        MEM32(reader + 8u) = cursor;
        MEM32(reader + 4u) = 31u;
        g_ecx = cursor;
        g_edx = word;
        g_eax = word >> 31u;
    }
    g_esp += 4u;
}

static __forceinline void recomp_xmv_vlc_fast(void)
{
    const uint32_t reader = g_ecx;
    uint32_t table = g_edx;
    uint32_t cursor = MEM32(reader + 8u);
    uint32_t available = MEM32(reader + 4u);
    uint32_t word = MEM32(reader);
    uint32_t entry = (uint32_t)MEM16(table);
    uint32_t result;

    for (;;) {
        const uint32_t width = entry & 0xFu;
        uint32_t shift;
        uint32_t code;
        if ((int32_t)available <= 0) {
            word = MEM32(cursor);
            cursor += 4u;
            available += 32u;
        }
        if ((int8_t)available >= (int8_t)width) {
            shift = (available - width) & 31u;
            code = word >> shift;
        } else {
            code = (word << 16u) | (uint32_t)MEM16(cursor + 2u);
            shift = (available - width + 16u) & 31u;
            code >>= shift;
        }
        shift = (32u - width) & 31u;
        code = (code << shift) >> shift;
        table += ((code + 1u) << 1u);
        entry = (uint32_t)MEM16(table);
        result = entry >> 4u;
        if ((entry & 0xFu) != 0u) {
            available -= entry & 0xFu;
            break;
        }
        table += result << 1u;
        entry = (uint32_t)MEM16(table);
        result = entry >> 4u;
        available -= result;
    }
    if ((int32_t)available <= 0) {
        word = MEM32(cursor);
        cursor += 4u;
        available += 32u;
    }
    MEM32(reader + 8u) = cursor;
    MEM32(reader + 4u) = available;
    MEM32(reader) = word;
    g_ecx = reader;
    g_edx = word;
    g_eax = result;
    g_esp += 4u;
}

static __forceinline void recomp_xmv_zero32_fast(void)
{
    memset((void *)XBOX_PTR(g_ecx), 0, 32u);
    g_mm0 = 0u;
    g_esp += 4u;
}

static __forceinline void recomp_xmv_zero128_fast(void)
{
    memset((void *)XBOX_PTR(g_ecx), 0, 128u);
    g_mm0 = 0u;
    g_esp += 4u;
}

static __forceinline void recomp_xmv_zero256_fast(void)
{
    const uint32_t destination = g_ecx;
    memset((void *)XBOX_PTR(destination), 0, 256u);
    memset(g_xmm0, 0, sizeof(g_xmm0));
    g_ecx = destination + 128u;
    g_esp += 4u;
}

#endif /* MERCENARIES_XMV_FAST_HELPERS_H */
