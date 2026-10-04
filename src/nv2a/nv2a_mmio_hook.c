/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * Decodes x86-64 MOV instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include "nv2a_pgraph_d3d11.h"
#include "../kernel/xbox_memory_layout.h"
#include <stdio.h>

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  xbox_GetGraphicsMemorySize()
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;
#if defined(_WIN32)
static uint8_t *g_nv2a_vram_aperture = NULL;
#endif

/* Statistics */
static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;

/* Narrow bring-up telemetry for NV_USER_DMA_PUT (0xFD800040). */
volatile uint64_t g_nv2a_mmio_put_hits = 0;
volatile uintptr_t g_nv2a_mmio_put_last_rip_before = 0;
volatile uintptr_t g_nv2a_mmio_put_last_rip_after = 0;
volatile uint32_t g_nv2a_mmio_put_last_value = 0;

/* Global APU state pointer is declared in apu.h and referenced from main.c
 * (regardless of whether the MMIO hook is active). Keep its definition
 * outside the Win32 guard. */

#if defined(_WIN32)

/* ============================================================
 * x86-64 register access helpers
 * ============================================================ */

/* Map ModRM reg field (+ REX.R) to CONTEXT register pointer */
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

/* ============================================================
 * x86-64 instruction decoder (focused on MOV patterns)
 *
 * We only need to handle the patterns MSVC generates for
 * volatile memory access (MEM8/MEM16/MEM32 macros):
 *
 * Writes:
 *   89 /r      MOV r/m32, r32       (32-bit reg → memory)
 *   88 /r      MOV r/m8, r8         (8-bit reg → memory)
 *   66 89 /r   MOV r/m16, r16       (16-bit reg → memory)
 *   C7 /0 id   MOV r/m32, imm32     (32-bit immediate → memory)
 *   C6 /0 ib   MOV r/m8, imm8       (8-bit immediate → memory)
 *
 * Reads:
 *   8B /r      MOV r32, r/m32       (memory → 32-bit reg)
 *   8A /r      MOV r8, r/m8         (memory → 8-bit reg)
 *   66 8B /r   MOV r16, r/m16       (memory → 16-bit reg)
 *   0F B6 /r   MOVZX r32, r/m8      (zero-extend byte → 32-bit)
 *   0F B7 /r   MOVZX r32, r/m16     (zero-extend word → 32-bit)
 *
 * With REX prefixes for 64-bit register extension.
 * ============================================================ */

/* Decode ModRM + optional SIB + displacement, return instruction length */
static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1; /* modrm byte */

    if (mod == 3) {
        /* Register-direct, no memory access - shouldn't happen for MMIO */
        return len;
    }

    /* Check for SIB byte */
    if ((rm & 7) == 4) {
        len++; /* SIB byte */
    }

    /* Displacement */
    if (mod == 0) {
        if ((rm & 7) == 5) len += 4; /* disp32 (RIP-relative or [disp32]) */
    } else if (mod == 1) {
        len += 1; /* disp8 */
    } else if (mod == 2) {
        len += 4; /* disp32 */
    }

    return len;
}

/*
 * Try to decode and handle the faulting instruction.
 * Returns true if successfully handled, false if unrecognized.
 */
static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;

    int prefix_len = 0;
    int has_66 = 0;     /* operand size override */
    int rex = 0;        /* REX prefix byte */
    int has_rex = 0;

    /* Parse prefixes */
    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) {
            has_66 = 1;
            prefix_len++;
        } else if (b == 0xF2 || b == 0xF3) {
            /* REP/REPNE prefix - skip */
            prefix_len++;
        } else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||
                   b == 0x64 || b == 0x65) {
            /* Segment overrides (and FS/GS). The fault address already
             * accounts for the effective address, so only the length matters. */
            prefix_len++;
        } else if (b >= 0x40 && b <= 0x4F) {
            /* REX prefix */
            rex = b;
            has_rex = 1;
            prefix_len++;
        } else {
            break;
        }
    }

    int rex_w = has_rex && (rex & 0x08); /* 64-bit operand */
    int rex_r = has_rex && (rex & 0x04); /* extends ModRM reg */
    int rex_b = has_rex && (rex & 0x01); /* extends ModRM r/m */

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4; /* default 32-bit */
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* ── ALU with a memory operand (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP) ──
     * Covers all eight operations in both operand directions and both
     * widths. Clang chooses these encodings freely; MSVC's patterns are
     * only a subset, so emulate the family generically. */
    {
        const uint8_t op0 = opcode[0];
        const int group = op0 & 0xF8;
        const int is_alu =
            (group == 0x00 || group == 0x08 || group == 0x10 || group == 0x18 ||
             group == 0x20 || group == 0x28 || group == 0x30 || group == 0x38) &&
            (op0 & 0x04) == 0;
        if (is_alu) {
            const int size8 = !(op0 & 0x01);
            const int reg_dest = (op0 & 0x02) != 0;
            const int subop = (op0 >> 3) & 7;
            const int modrm_len = decode_modrm_len(opcode + 1, rex_b);
            const int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
            const int acs = size8 ? 1 : access_size;
            const int bits = acs * 8;
            const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
            const uint64_t sign_bit = 1ULL << (bits - 1);
            const uint64_t mem =
                nv2a_mmio_read(nv2a, mmio_offset, acs) & mask;
            uint64_t *regp = ctx_reg64(ctx, reg);
            const uint64_t rv = regp ? (*regp & mask) : 0;
            const uint64_t a = reg_dest ? rv : mem;
            const uint64_t b = reg_dest ? mem : rv;
            const uint64_t carry_in = (ctx->EFlags & 0x0001u) ? 1u : 0u;
            uint64_t result;
            int is_arith = 1, is_sub = 0, is_cmp = 0;

            switch (subop) {
            case 0: result = a + b; break;                 /* ADD */
            case 1: result = a | b; is_arith = 0; break;   /* OR  */
            case 2: result = a + b + carry_in; break;      /* ADC */
            case 3: result = a - b - carry_in; is_sub = 1; break; /* SBB */
            case 4: result = a & b; is_arith = 0; break;   /* AND */
            case 5: result = a - b; is_sub = 1; break;     /* SUB */
            case 6: result = a ^ b; is_arith = 0; break;   /* XOR */
            default: result = a - b; is_sub = 1; is_cmp = 1; break; /* CMP */
            }
            result &= mask;

            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
            if (is_arith) {
                int cf = is_sub ? ((a < b + carry_in) ? 1 : 0)
                                : (int)(((a + b + carry_in) > mask) ? 1 : 0);
                int of = is_sub ? (int)(((a ^ b) & (a ^ result) & sign_bit) != 0)
                                : (int)(((a ^ result) & (b ^ result) & sign_bit) != 0);
                if (cf) flags |= 0x0001;
                if (of) flags |= 0x0800;
            }
            if (result == 0) flags |= 0x0040;
            if (result & sign_bit) flags |= 0x0080;
            {
                uint32_t low = (uint32_t)(result & 0xFF);
                low ^= low >> 4; low ^= low >> 2; low ^= low >> 1;
                if (!(low & 1)) flags |= 0x0004;
            }
            ctx->EFlags = flags;

            if (!is_cmp) {
                if (reg_dest && regp) {
                    if (acs == 1)       *regp = (*regp & ~0xFFULL) | result;
                    else if (acs == 2)  *regp = (*regp & ~0xFFFFULL) | result;
                    else                *regp = result; /* 32-bit zero-extends */
                    g_mmio_read_count++;
                } else {
                    nv2a_mmio_write(nv2a, mmio_offset, result, acs);
                    g_mmio_write_count++;
                }
            } else {
                g_mmio_read_count++;
            }
            ctx->Rip += prefix_len + 1 + modrm_len;
            return true;
        }
    }

    /* ── MOV r/m, r (write: 88/89) ── */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);

        /* Mask to access size */
        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;

        uintptr_t rip_before = (uintptr_t)ctx->Rip;
        nv2a_mmio_write(nv2a, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        if (mmio_offset == 0x00800040u) {
            ++g_nv2a_mmio_put_hits;
            g_nv2a_mmio_put_last_rip_before = rip_before;
            g_nv2a_mmio_put_last_rip_after = (uintptr_t)ctx->Rip;
            g_nv2a_mmio_put_last_value = (uint32_t)val;
        }
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r, r/m (read: 8A/8B) ── */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, access_size);

        uint64_t *dest = ctx_reg64(ctx, reg);
        if (access_size == 1) {
            *dest = (*dest & ~0xFFULL) | (val & 0xFF);
        } else if (access_size == 2) {
            *dest = (*dest & ~0xFFFFULL) | (val & 0xFFFF);
        } else if (access_size == 4) {
            *dest = val & 0xFFFFFFFF; /* 32-bit write zero-extends */
        } else {
            *dest = val;
        }

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOV r/m32, imm32 (C7 /0) ── */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        /* immediate follows modrm+sib+disp */
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint32_t imm = *(const uint32_t *)imm_ptr;
        int imm_len = (rex_w ? 4 : 4); /* still 32-bit imm even with REX.W */

        nv2a_mmio_write(nv2a, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r/m8, imm8 (C6 /0) ── */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);

        nv2a_mmio_write(nv2a, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOVZX r32, r/m8 (0F B6) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 1) & 0xFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val; /* zero-extend to 64-bit */

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOVZX r32, r/m16 (0F B7) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 2) & 0xFFFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val;

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── TEST r/m, r (84/85) - reads memory for flag comparison ── */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size == 1) { mem_val &= 0xFF; reg_val &= 0xFF; }
        else if (access_size == 2) { mem_val &= 0xFFFF; reg_val &= 0xFFFF; }
        else if (access_size == 4) { mem_val &= 0xFFFFFFFF; reg_val &= 0xFFFFFFFF; }

        uint64_t result = mem_val & reg_val;

        /* Update flags: ZF, SF, PF; clear OF, CF */
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800); /* CF, ZF, SF, OF */
        if (result == 0) ctx->EFlags |= 0x0040; /* ZF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── CMP r/m, r (38/39) or CMP r, r/m (3A/3B) ── */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        /* CMP r/m, r: compute r/m - r */
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800); /* CF, ZF, SF, OF */
        if (result == 0) ctx->EFlags |= 0x0040; /* ZF */
        if (mem_val < reg_val) ctx->EFlags |= 0x0001; /* CF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    if (opcode[0] == 0x3B || opcode[0] == 0x3A) {
        if (opcode[0] == 0x3A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        /* CMP r, r/m: compute r - r/m */
        uint64_t result = reg_val - mem_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (reg_val < mem_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── OR r/m, r (08/09) - read-modify-write ── */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val | reg_val;

        nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── AND r/m, r (20/21) ── */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;

        nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── Group 1 immediate: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m, imm ──
     * Clang folds a constant operand into a memory-immediate form (a guest
     * "or dword ptr [mmio], imm" becomes 83 /1 ib), where MSVC instead
     * loads the constant into a register and emits 09 /r. Handle the 8-bit
     * (0x80), 32/64-bit imm32 (0x81), and sign-extended imm8 (0x83) forms
     * so the MMIO read-modify-write is emulated rather than faulting. */
    if (opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int subop = (opcode[1] >> 3) & 7;
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint64_t imm;
        int imm_len;

        if (opcode[0] == 0x80) {
            access_size = 1;
            imm = (uint64_t)*imm_ptr;
            imm_len = 1;
        } else if (opcode[0] == 0x83) {
            imm = (uint64_t)(int64_t)(int8_t)*imm_ptr;
            imm_len = 1;
        } else {
            imm = (uint64_t)(*(const uint32_t *)imm_ptr);
            imm_len = 4;
            if (rex_w) imm = (uint64_t)(int64_t)(int32_t)imm;
        }

        const int bits = access_size * 8;
        const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
        const uint64_t sign_bit = 1ULL << (bits - 1);
        const uint64_t a = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
        const uint64_t b = imm & mask;
        const uint64_t carry_in = (ctx->EFlags & 0x0001u) ? 1u : 0u;
        uint64_t result;
        int is_arith = 1;
        int is_sub = 0;
        int is_cmp = 0;

        switch (subop) {
        case 0: result = a + b; break;                 /* ADD */
        case 1: result = a | b; is_arith = 0; break;   /* OR  */
        case 2: result = a + b + carry_in; break;      /* ADC */
        case 3: result = a - b - carry_in; is_sub = 1; break; /* SBB */
        case 4: result = a & b; is_arith = 0; break;   /* AND */
        case 5: result = a - b; is_sub = 1; break;     /* SUB */
        case 6: result = a ^ b; is_arith = 0; break;   /* XOR */
        default: result = a - b; is_sub = 1; is_cmp = 1; break; /* CMP */
        }
        result &= mask;

        uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u); /* CF|ZF|SF|OF */
        if (is_arith) {
            int cf = is_sub ? ((a < b + carry_in) ? 1 : 0)
                            : (int)(((a + b + carry_in) > mask) ? 1 : 0);
            int of = is_sub ? (int)(((a ^ b) & (a ^ result) & sign_bit) != 0)
                            : (int)(((a ^ result) & (b ^ result) & sign_bit) != 0);
            if (cf) flags |= 0x0001;
            if (of) flags |= 0x0800;
        }
        if (result == 0) flags |= 0x0040;
        if (result & sign_bit) flags |= 0x0080;
        ctx->EFlags = flags;
        if ((flags & 0x0040) == 0) {
            uint32_t low = (uint32_t)(result & 0xFF);
            low ^= low >> 4;
            low ^= low >> 2;
            low ^= low >> 1;
            if (!(low & 1)) ctx->EFlags |= 0x0004; /* PF */
        }

        if (!is_cmp) {
            nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
            g_mmio_write_count++;
        } else {
            g_mmio_read_count++;
        }
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        return true;
    }

    /* ── Group 3: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m (F6/F7) ──
     * TEST with an immediate (F7 /0) is the other constant-folded form
     * Clang emits for volatile MMIO that MSVC expresses through a
     * register. NOT/NEG/INC/DEC cover the remaining read-modify-writes. */
    if (opcode[0] == 0xF6 || opcode[0] == 0xF7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int subop = (opcode[1] >> 3) & 7;
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        if (opcode[0] == 0xF6) access_size = 1;

        const int bits = access_size * 8;
        const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
        const uint64_t sign_bit = 1ULL << (bits - 1);

        if (subop == 0) { /* TEST r/m, imm */
            int imm_len = (opcode[0] == 0xF6) ? 1 : 4;
            uint64_t imm = (opcode[0] == 0xF6)
                               ? (uint64_t)*imm_ptr
                               : (uint64_t)(*(const uint32_t *)imm_ptr);
            uint64_t v = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
            uint64_t r = v & (imm & mask);
            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
            if (r == 0) flags |= 0x0040;
            if (r & sign_bit) flags |= 0x0080;
            ctx->EFlags = flags;
            uint32_t low = (uint32_t)(r & 0xFF);
            low ^= low >> 4; low ^= low >> 2; low ^= low >> 1;
            if (!(low & 1)) ctx->EFlags |= 0x0004;
            ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
            g_mmio_read_count++;
            return true;
        }
        if (subop == 2 || subop == 3) { /* NOT / NEG */
            uint64_t v = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
            uint64_t r = (subop == 2) ? ((~v) & mask) : ((0 - v) & mask);
            if (subop == 3) {
                uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
                if (v != 0) flags |= 0x0001;
                if (v == sign_bit) flags |= 0x0800;
                if (r == 0) flags |= 0x0040;
                if (r & sign_bit) flags |= 0x0080;
                ctx->EFlags = flags;
            }
            nv2a_mmio_write(nv2a, mmio_offset, r, access_size);
            ctx->Rip += prefix_len + 1 + modrm_len;
            g_mmio_write_count++;
            return true;
        }
        if (subop == 4 || subop == 5) { /* MUL / IMUL r/m */
            uint64_t v = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
            uint64_t a = *ctx_reg64(ctx, 0) & mask;
            uint64_t full = (subop == 4) ? (a * v)
                                         : (uint64_t)((int64_t)a * (int64_t)v);
            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
            if ((full >> bits) != 0) flags |= 0x0001;
            ctx->Rax = full & mask;
            *ctx_reg64(ctx, 2) = (full >> bits) & mask;
            ctx->EFlags = flags;
            ctx->Rip += prefix_len + 1 + modrm_len;
            g_mmio_read_count++;
            return true;
        }
        /* DIV/IDIV on MMIO are not expected; fall through as unrecognized. */
    }

    /* ── Group 5: INC/DEC r/m (FF /0, FF /1) ── */
    if (opcode[0] == 0xFF) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int subop = (opcode[1] >> 3) & 7;
        if (subop == 0 || subop == 1) {
            const int bits = access_size * 8;
            const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
            const uint64_t sign_bit = 1ULL << (bits - 1);
            uint64_t v = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
            uint64_t r = ((subop == 0) ? (v + 1) : (v - 1)) & mask;
            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0840u); /* ZF|SF|OF */
            if (r == 0) flags |= 0x0040;
            if (r & sign_bit) flags |= 0x0080;
            if ((r & sign_bit) != (v & sign_bit)) flags |= 0x0800;
            ctx->EFlags = flags;
            nv2a_mmio_write(nv2a, mmio_offset, r, access_size);
            ctx->Rip += prefix_len + 1 + modrm_len;
            g_mmio_write_count++;
            return true;
        }
    }

    /* Unrecognized instruction */
    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API
 * ============================================================ */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset,
                    HANDLE system_memory_mapping)
{
    g_mem_offset = xbox_mem_offset;

    /* The Xbox has unified memory. PGRAPH reads guest physical RAM, while
     * D3D locks expose the same bytes through the 0xF0000000 aperture. */
    g_nv2a_vram = (uint8_t *)(uintptr_t)xbox_mem_offset;
    uint8_t *ramin_ptr = (uint8_t *)VirtualAlloc(
        NULL, NV2A_RAMIN_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_nv2a_vram || !ramin_ptr || !system_memory_mapping) {
        fprintf(stderr, "[NV2A] Unified-memory initialization failed!\n");
        return;
    }

    g_nv2a_vram_aperture = (uint8_t *)MapViewOfFileEx(
        system_memory_mapping, FILE_MAP_ALL_ACCESS, 0, 0, NV2A_VRAM_SIZE,
        (LPVOID)((uintptr_t)NV2A_VRAM_BASE + (uintptr_t)g_mem_offset));
    if (g_nv2a_vram_aperture !=
        (uint8_t *)((uintptr_t)NV2A_VRAM_BASE + (uintptr_t)g_mem_offset)) {
        fprintf(stderr,
                "[NV2A] Failed to alias Xbox VRAM aperture at %p "
                "(error %lu)\n",
                (void *)((uintptr_t)NV2A_VRAM_BASE +
                         (uintptr_t)g_mem_offset), GetLastError());
        return;
    }

    /* Initialize NV2A state machine with guest RAM as unified VRAM. */
    nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE,
                         ramin_ptr, NV2A_RAMIN_SIZE);

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void*)g_nv2a_vram, (void*)ramin_ptr);
}

bool nv2a_hook_service_scanout(void)
{
    return pgraph_d3d11_service_scanout() != 0;
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    /* Compute MMIO offset within NV2A register space */
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;

    return decode_and_handle(ctx, mmio_offset, is_write);
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{
    /* The configured VRAM aperture is eagerly mapped to the same section as host
     * NV2A VRAM. Faults here mean initialization failed; do not create an
     * incoherent private page that hides CPU writes from PGRAPH. */
    if (fault_xbox_va >= NV2A_VRAM_BASE &&
        fault_xbox_va < NV2A_VRAM_BASE + NV2A_VRAM_SIZE)
        return false;

    /* Preserve demand pages for the remaining framebuffer range,
     * which is outside the physical VRAM aperture. */
    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
}

#else /* !_WIN32 -- SIGSEGV-based MMIO trapping deferred to main.c port */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset,
                    HANDLE system_memory_mapping)
{ (void)xbox_mem_offset; (void)system_memory_mapping; }

bool nv2a_hook_service_scanout(void) { return false; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }

#endif /* _WIN32 */
