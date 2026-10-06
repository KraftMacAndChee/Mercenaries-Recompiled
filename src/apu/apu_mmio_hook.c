/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu_mmio_hook.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The MMIO hook is a Win32-VEH x86-64 instruction decoder. On Linux the
 * equivalent goes through sigaction + ucontext_t (Stage 2 / main.c). For
 * now the whole body is Windows-only so apu_emu links on Debian. */
#if defined(_WIN32)
#include <windows.h>

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

#define AC97_MMIO_BASE 0xFEC00000u
#define AC97_MMIO_SIZE 0x00001000u
/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

typedef uint64_t (*mmio_read_fn)(void *opaque, uint32_t offset,
                                 unsigned int size);
typedef void (*mmio_write_fn)(void *opaque, uint32_t offset, uint64_t value,
                              unsigned int size);

typedef struct AC97HookState {
    uint8_t mixer[0x100];
    uint8_t nabm[0x80];
    uint8_t cas;
} AC97HookState;

static AC97HookState g_ac97;

static uint64_t mmio_load_le(const uint8_t *data, uint32_t offset,
                             unsigned int size, uint32_t capacity)
{
    uint64_t value = 0;
    unsigned int i;

    if (offset >= capacity || size > capacity - offset) return 0;
    for (i = 0; i < size; ++i)
        value |= (uint64_t)data[offset + i] << (i * 8);
    return value;
}

static void mmio_store_le(uint8_t *data, uint32_t offset, uint64_t value,
                          unsigned int size, uint32_t capacity)
{
    unsigned int i;

    if (offset >= capacity || size > capacity - offset) return;
    for (i = 0; i < size; ++i)
        data[offset + i] = (uint8_t)(value >> (i * 8));
}

static void ac97_mixer_store16(uint32_t offset, uint16_t value)
{
    mmio_store_le(g_ac97.mixer, offset, value, 2,
                  (uint32_t)sizeof(g_ac97.mixer));
}

static void ac97_reset_mixer(void)
{
    memset(g_ac97.mixer, 0, sizeof(g_ac97.mixer));
    ac97_mixer_store16(0x02, 0x8000); /* Master volume: muted */
    ac97_mixer_store16(0x18, 0x8808); /* PCM output volume */
    ac97_mixer_store16(0x1C, 0x8808); /* Record gain */
    ac97_mixer_store16(0x26, 0x000F); /* Codec sections ready */
    ac97_mixer_store16(0x28, 0x0809); /* Extended audio ID */
    ac97_mixer_store16(0x2A, 0x0009); /* Extended audio control */
    ac97_mixer_store16(0x2C, 0xBB80); /* Front DAC: 48000 Hz */
    ac97_mixer_store16(0x2E, 0xBB80); /* Surround DAC */
    ac97_mixer_store16(0x30, 0xBB80); /* LFE DAC */
    ac97_mixer_store16(0x32, 0xBB80); /* Stereo ADC */
    ac97_mixer_store16(0x34, 0xBB80); /* Microphone ADC */
    ac97_mixer_store16(0x7C, 0x8384); /* SigmaTel vendor ID 1 */
    ac97_mixer_store16(0x7E, 0x7600); /* SigmaTel vendor ID 2 */
}

void ac97_hook_init(void)
{
    unsigned int stream;

    memset(&g_ac97, 0, sizeof(g_ac97));
    ac97_reset_mixer();
    for (stream = 0; stream < 8; ++stream)
        g_ac97.nabm[stream * 16 + 6] = 1; /* SR_DCH */
    mmio_store_le(g_ac97.nabm, 0x30, 0x00000100u, 4,
                  (uint32_t)sizeof(g_ac97.nabm)); /* GS_S0CR */
}

static uint64_t ac97_mmio_read(void *opaque, uint32_t offset,
                               unsigned int size)
{
    uint32_t reg;
    uint64_t value;
    (void)opaque;

    if (offset < 0x100) {
        g_ac97.cas = 0;
        if (size != 2) return size == 1 ? 0xFFu : 0xFFFFFFFFu;
        return mmio_load_le(g_ac97.mixer, offset, size,
                            (uint32_t)sizeof(g_ac97.mixer));
    }
    reg = offset - 0x100;
    if (reg >= sizeof(g_ac97.nabm)) return 0;
    if (reg == 0x34 && size == 1) {
        value = g_ac97.cas;
        g_ac97.cas = 1;
        return value;
    }
    if (reg == 0x30 && size == 4) {
        value = mmio_load_le(g_ac97.nabm, reg, size,
                             (uint32_t)sizeof(g_ac97.nabm));
        return value | 0x00000100u; /* Primary codec ready */
    }
    return mmio_load_le(g_ac97.nabm, reg, size,
                        (uint32_t)sizeof(g_ac97.nabm));
}

static void ac97_mmio_write(void *opaque, uint32_t offset, uint64_t value,
                            unsigned int size)
{
    uint32_t reg;
    (void)opaque;

    if (offset < 0x100) {
        g_ac97.cas = 0;
        if (size != 2) return;
        if (offset == 0) {
            ac97_reset_mixer();
        } else if (offset == 0x26) {
            uint16_t old_value = (uint16_t)mmio_load_le(
                g_ac97.mixer, offset, 2, (uint32_t)sizeof(g_ac97.mixer));
            value = ((uint16_t)value & (uint16_t)~0x800Fu) |
                    (old_value & 0x000Fu);
            ac97_mixer_store16(offset, (uint16_t)value);
        } else if (offset != 0x28 && offset != 0x7C && offset != 0x7E) {
            ac97_mixer_store16(offset, (uint16_t)value);
        }
        return;
    }

    reg = offset - 0x100;
    if (reg >= sizeof(g_ac97.nabm)) return;
    if (reg == 0x2C && size == 4) {
        /* xemu leaves warm/cold reset requests self-clearing. */
        if (!(value & 0x6u))
            mmio_store_le(g_ac97.nabm, reg, value & 0x3Fu, size,
                          (uint32_t)sizeof(g_ac97.nabm));
        return;
    }
    if (reg == 0x30 && size == 4) {
        uint32_t status = (uint32_t)mmio_load_le(
            g_ac97.nabm, reg, 4, (uint32_t)sizeof(g_ac97.nabm));
        status &= ~((uint32_t)value & 0x00008C01u);
        status |= 0x00000100u;
        mmio_store_le(g_ac97.nabm, reg, status, 4,
                      (uint32_t)sizeof(g_ac97.nabm));
        return;
    }
    if ((reg & 0x0Fu) == 0x0Bu && size == 1) {
        uint32_t status_reg = (reg & ~0x0Fu) + 6;
        if (value & 2) {
            memset(&g_ac97.nabm[reg & ~0x0Fu], 0, 16);
            g_ac97.nabm[status_reg] = 1;
        } else {
            g_ac97.nabm[reg] = (uint8_t)value & 0x1Fu;
            if (value & 1) g_ac97.nabm[status_reg] &= (uint8_t)~1u;
            else g_ac97.nabm[status_reg] |= 1u;
        }
        return;
    }
    if ((reg & 0x0Fu) == 6 && (size == 1 || size == 2)) {
        uint16_t status = (uint16_t)mmio_load_le(
            g_ac97.nabm, reg, 2, (uint32_t)sizeof(g_ac97.nabm));
        status &= (uint16_t)~((uint16_t)value & 0x001Cu);
        mmio_store_le(g_ac97.nabm, reg, status, 2,
                      (uint32_t)sizeof(g_ac97.nabm));
        return;
    }
    if ((reg & 0x0Fu) == 0 && size == 4) value &= ~3ULL;
    mmio_store_le(g_ac97.nabm, reg, value, size,
                  (uint32_t)sizeof(g_ac97.nabm));
}

static uint64_t apu_mmio_read(void *opaque, uint32_t offset,
                              unsigned int size)
{
    return mcpx_apu_mmio_read((MCPXAPUState *)opaque, offset, size);
}

static void apu_mmio_write(void *opaque, uint32_t offset, uint64_t value,
                           unsigned int size)
{
    mcpx_apu_mmio_write((MCPXAPUState *)opaque, offset, value, size);
}

/* ============================================================
 * x86-64 register access helpers (same as NV2A hook)
 * ============================================================ */

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1; /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ============================================================
 * Instruction decoder shared by the APU and AC97 MMIO ranges
 * ============================================================ */

#define MMIO_DECODE_PREFIX apu
#include "../nv2a/mmio_decode_util.h"

static bool mmio_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset,
                                   void *opaque, mmio_read_fn read_fn,
                                   mmio_write_fn write_fn, const char *tag)
{
    /* The shared decoder covers the same MOV/TEST/CMP forms plus the ALU and
     * group-immediate encodings Clang emits that the original MSVC-focused
     * body did not. Fall through to the legacy body only for what it rejects,
     * so the diagnostic message and failure accounting are preserved. */
    if (apu_decode_and_handle(ctx, mmio_offset, opaque, read_fn, write_fn) >= 0) {
        return true;
    }

    const uint8_t *ip = (const uint8_t *)ctx->Rip;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        write_fn(opaque, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m, imm32 (write: C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        write_fn(opaque, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (write: C6 /0) */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        write_fn(opaque, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = read_fn(opaque, mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = read_fn(opaque, mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        access_size = 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = read_fn(opaque, mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) - read */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = read_fn(opaque, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = read_fn(opaque, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = read_fn(opaque, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        write_fn(opaque, mmio_offset, mem_val | reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = read_fn(opaque, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        write_fn(opaque, mmio_offset, mem_val & reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[%s] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                tag, (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

int apu_hook_try_read32(uint32_t address, uint32_t *value)
{
    const uint32_t offset = address - APU_MMIO_BASE;
    if (!g_apu_state || !value || offset > APU_MMIO_SIZE - 4u || (offset & 3u))
        return 0;
    *value = (uint32_t)apu_mmio_read(g_apu_state, offset, 4u);
    ++g_apu_mmio_read_count;
    return 1;
}

int apu_hook_try_write32(uint32_t address, uint32_t value)
{
    const uint32_t offset = address - APU_MMIO_BASE;
    if (!g_apu_state || offset > APU_MMIO_SIZE - 4u || (offset & 3u))
        return 0;
    apu_mmio_write(g_apu_state, offset, value, 4u);
    ++g_apu_mmio_write_count;
    return 1;
}

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    (void)fault_addr;
    (void)is_write;
    if (!g_apu_state || mmio_offset >= APU_MMIO_SIZE) return false;
    return mmio_decode_and_handle(ctx, mmio_offset, g_apu_state,
                                  apu_mmio_read, apu_mmio_write, "APU");
}

bool ac97_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - AC97_MMIO_BASE;
    (void)fault_addr;
    (void)is_write;
    if (mmio_offset >= AC97_MMIO_SIZE) return false;
    return mmio_decode_and_handle(ctx, mmio_offset, &g_ac97,
                                  ac97_mmio_read, ac97_mmio_write, "AC97");
}

#endif /* _WIN32 */
