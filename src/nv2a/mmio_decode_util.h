/*
 * mmio_decode_util.h - shared x86-64 MMIO fault instruction decoder
 *
 * The NV2A and APU/AC97 MMIO apertures are unmapped and faulted into a VEH
 * handler. The handler must emulate the exact instruction the host compiler
 * emitted for the guest access, then advance RIP.
 *
 * Upstream only had to support the instruction patterns MSVC generates. Clang
 * (used by the LLVM-MinGW cross build) chooses different, equally valid
 * encodings: it folds constants into memory-immediate read-modify-writes and
 * freely swaps ALU operand directions. This decoder covers the full ALU family
 * in both directions, the Group-1/3/5 immediate and unary forms, and the MOV
 * forms both compilers emit.
 *
 * The caller supplies:
 *   ctx        - the faulting thread CONTEXT
 *   ip         - instruction bytes at ctx->Rip
 *   offset     - MMIO byte offset within the aperture
 *   rd         - mmio_read(opaque, offset, size)
 *   wr         - mmio_write(opaque, offset, value, size)
 *   opaque     - state pointer passed through to rd/wr
 * It returns the instruction length (and performs the access) on success, or
 * -1 if the encoding is not recognised.
 *
 * Define MMIO_DECODE_PREFIX before including to namespace the symbols:
 *   #define MMIO_DECODE_PREFIX nv2a / apu
 */

#ifndef MMIO_DECODE_UTIL_H
#define MMIO_DECODE_UTIL_H

#include <stdint.h>
#include <stdbool.h>

/* Callers must provide these two token pasting helpers. */
#ifndef MMIO_DECODE_CONCAT
#define MMIO_DECODE_CONCAT_(a, b) a##b
#define MMIO_DECODE_CONCAT(a, b) MMIO_DECODE_CONCAT_(a, b)
#endif

typedef uint64_t (*mmio_decode_read_fn)(void *opaque, uint32_t offset,
                                        unsigned int size);
typedef void (*mmio_decode_write_fn)(void *opaque, uint32_t offset,
                                     uint64_t value, unsigned int size);

/* Decode ModRM + optional SIB + displacement, return operand byte length. */
static int MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
    const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;             /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/*
 * Apply a Group-1 (immediate) or ALU operation, updating flags and either the
 * destination register or MMIO memory. Returns the result value.
 */
static uint64_t MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _alu_apply)(
    PCONTEXT ctx, int subop, uint64_t a, uint64_t b, uint64_t carry_in,
    int bits, int *is_arith_out, int *is_sub_out)
{
    const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
    const uint64_t sign_bit = 1ULL << (bits - 1);
    uint64_t result;
    int is_arith = 1, is_sub = 0;

    switch (subop) {
    case 0: result = a + b; break;                   /* ADD */
    case 1: result = a | b; is_arith = 0; break;     /* OR  */
    case 2: result = a + b + carry_in; break;        /* ADC */
    case 3: result = a - b - carry_in; is_sub = 1; break; /* SBB */
    case 4: result = a & b; is_arith = 0; break;     /* AND */
    case 5: result = a - b; is_sub = 1; break;       /* SUB */
    case 6: result = a ^ b; is_arith = 0; break;     /* XOR */
    default: result = a - b; is_sub = 1; break;      /* CMP */
    }
    result &= mask;

    uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u); /* clear CF|ZF|SF|OF */
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
        if (!(low & 1)) flags |= 0x0004; /* PF */
    }
    ctx->EFlags = flags;

    if (is_arith_out) *is_arith_out = is_arith;
    if (is_sub_out) *is_sub_out = is_sub;
    return result;
}

/*
 * Decode and emulate the faulting instruction.
 * Returns the instruction length in bytes on success, or -1 if unrecognised.
 */
static int MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_and_handle)(
    PCONTEXT ctx, uint32_t mmio_offset, void *opaque,
    mmio_decode_read_fn read_fn, mmio_decode_write_fn write_fn)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||
                 b == 0x64 || b == 0x65) {
            prefix_len++; /* segment / FS / GS override; length only */
        }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    const int rex_w = has_rex && (rex & 0x08);
    const int rex_r = has_rex && (rex & 0x04);
    const int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4; /* default 32-bit */
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* ── ALU with a memory operand (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP) ── */
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
            const int modrm_len =
                MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                    opcode + 1, rex_b);
            const int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
            const int acs = size8 ? 1 : access_size;
            const int bits = acs * 8;
            const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
            const uint64_t mem =
                read_fn(opaque, mmio_offset, (unsigned int)acs) & mask;
            uint64_t *regp = ctx_reg64(ctx, reg);
            const uint64_t rv = regp ? (*regp & mask) : 0;
            const uint64_t a = reg_dest ? rv : mem;
            const uint64_t b = reg_dest ? mem : rv;
            const uint64_t carry_in = (ctx->EFlags & 0x0001u) ? 1u : 0u;
            const uint64_t result =
                MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _alu_apply)(
                    ctx, subop, a, b, carry_in, bits, NULL, NULL);

            if (subop != 7) { /* CMP only reads */
                if (reg_dest && regp) {
                    if (acs == 1)      *regp = (*regp & ~0xFFULL) | result;
                    else if (acs == 2) *regp = (*regp & ~0xFFFFULL) | result;
                    else               *regp = result; /* 32-bit zero-extends */
                } else {
                    write_fn(opaque, mmio_offset, result, (unsigned int)acs);
                }
            }
            ctx->Rip += prefix_len + 1 + modrm_len;
            return prefix_len + 1 + modrm_len;
        }
    }

    /* ── Group 1 immediate: r/m, imm (80/81/83) ── */
    if (opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83) {
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const int subop = (opcode[1] >> 3) & 7;
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
        const uint64_t a = read_fn(opaque, mmio_offset,
                                   (unsigned int)access_size) & mask;
        const uint64_t carry_in = (ctx->EFlags & 0x0001u) ? 1u : 0u;
        const uint64_t result =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _alu_apply)(
                ctx, subop, a, imm & mask, carry_in, bits, NULL, NULL);

        if (subop != 7) {
            write_fn(opaque, mmio_offset, result, (unsigned int)access_size);
        }
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        return prefix_len + 1 + modrm_len + imm_len;
    }

    /* ── Group 3: TEST/NOT/NEG/MUL/IMUL (F6/F7) ── */
    if (opcode[0] == 0xF6 || opcode[0] == 0xF7) {
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const int subop = (opcode[1] >> 3) & 7;
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        if (opcode[0] == 0xF6) access_size = 1;

        const int bits = access_size * 8;
        const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
        const uint64_t sign_bit = 1ULL << (bits - 1);

        if (subop == 0 || subop == 1) { /* TEST r/m, imm */
            const int imm_len = (opcode[0] == 0xF6) ? 1 : 4;
            const uint64_t imm = (opcode[0] == 0xF6)
                                     ? (uint64_t)*imm_ptr
                                     : (uint64_t)(*(const uint32_t *)imm_ptr);
            const uint64_t v =
                read_fn(opaque, mmio_offset, (unsigned int)access_size) & mask;
            const uint64_t r = v & (imm & mask);
            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
            if (r == 0) flags |= 0x0040;
            if (r & sign_bit) flags |= 0x0080;
            ctx->EFlags = flags;
            uint32_t low = (uint32_t)(r & 0xFF);
            low ^= low >> 4; low ^= low >> 2; low ^= low >> 1;
            if (!(low & 1)) ctx->EFlags |= 0x0004;
            ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
            return prefix_len + 1 + modrm_len + imm_len;
        }
        if (subop == 2 || subop == 3) { /* NOT / NEG */
            const uint64_t v =
                read_fn(opaque, mmio_offset, (unsigned int)access_size) & mask;
            const uint64_t r = (subop == 2) ? ((~v) & mask) : ((0 - v) & mask);
            if (subop == 3) {
                uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
                if (v != 0) flags |= 0x0001;
                if (v == sign_bit) flags |= 0x0800;
                if (r == 0) flags |= 0x0040;
                if (r & sign_bit) flags |= 0x0080;
                ctx->EFlags = flags;
            }
            write_fn(opaque, mmio_offset, r, (unsigned int)access_size);
            ctx->Rip += prefix_len + 1 + modrm_len;
            return prefix_len + 1 + modrm_len;
        }
        if (subop == 4 || subop == 5) { /* MUL / IMUL */
            const uint64_t v =
                read_fn(opaque, mmio_offset, (unsigned int)access_size) & mask;
            const uint64_t a = *ctx_reg64(ctx, 0) & mask;
            const uint64_t full = (subop == 4)
                                      ? (a * v)
                                      : (uint64_t)((int64_t)a * (int64_t)v);
            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
            if ((full >> bits) != 0) flags |= 0x0001;
            ctx->Rax = full & mask;
            *ctx_reg64(ctx, 2) = (full >> bits) & mask;
            ctx->EFlags = flags;
            ctx->Rip += prefix_len + 1 + modrm_len;
            return prefix_len + 1 + modrm_len;
        }
    }

    /* ── Group 5: INC/DEC r/m (FF /0, FF /1) ── */
    if (opcode[0] == 0xFF) {
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const int subop = (opcode[1] >> 3) & 7;
        if (subop == 0 || subop == 1) {
            const int bits = access_size * 8;
            const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
            const uint64_t sign_bit = 1ULL << (bits - 1);
            const uint64_t v =
                read_fn(opaque, mmio_offset, (unsigned int)access_size) & mask;
            const uint64_t r = ((subop == 0) ? (v + 1) : (v - 1)) & mask;
            uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0840u); /* ZF|SF|OF */
            if (r == 0) flags |= 0x0040;
            if (r & sign_bit) flags |= 0x0080;
            if ((r & sign_bit) != (v & sign_bit)) flags |= 0x0800;
            ctx->EFlags = flags;
            write_fn(opaque, mmio_offset, r, (unsigned int)access_size);
            ctx->Rip += prefix_len + 1 + modrm_len;
            return prefix_len + 1 + modrm_len;
        }
    }

    /* ── MOV r/m, r (write: 88/89) ── */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;
        write_fn(opaque, mmio_offset, val, (unsigned int)access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        return prefix_len + 1 + modrm_len;
    }

    /* ── MOV r, r/m (read: 8A/8B) ── */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        const uint64_t val =
            read_fn(opaque, mmio_offset, (unsigned int)access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        return prefix_len + 1 + modrm_len;
    }

    /* ── MOV r/m32, imm32 (C7 /0) ── */
    if (opcode[0] == 0xC7) {
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        const uint32_t imm = *(const uint32_t *)imm_ptr;
        write_fn(opaque, mmio_offset, imm, (unsigned int)access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        return prefix_len + 1 + modrm_len + 4;
    }

    /* ── MOV r/m8, imm8 (C6 /0) ── */
    if (opcode[0] == 0xC6) {
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const uint8_t imm = *(opcode + 1 + modrm_len);
        write_fn(opaque, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        return prefix_len + 1 + modrm_len + 1;
    }

    /* ── MOVZX/MOVSX r, r/m8/16 (0F B6/B7/BE/BF) ── */
    if (opcode[0] == 0x0F &&
        (opcode[1] == 0xB6 || opcode[1] == 0xB7 ||
         opcode[1] == 0xBE || opcode[1] == 0xBF)) {
        const int is8 = (opcode[1] == 0xB6 || opcode[1] == 0xBE);
        const int sign_extend = (opcode[1] == 0xBE || opcode[1] == 0xBF);
        const int acs = is8 ? 1 : 2;
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 2, rex_b);
        const int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = read_fn(opaque, mmio_offset, (unsigned int)acs);
        if (sign_extend) {
            if (is8) val = (uint64_t)(int64_t)(int8_t)val;
            else     val = (uint64_t)(int64_t)(int16_t)val;
        } else {
            val &= is8 ? 0xFF : 0xFFFF;
        }
        *ctx_reg64(ctx, reg) = val;
        ctx->Rip += prefix_len + 2 + modrm_len;
        return prefix_len + 2 + modrm_len;
    }

    /* ── TEST r/m, r (84/85) ── */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        const int modrm_len =
            MMIO_DECODE_CONCAT(MMIO_DECODE_PREFIX, _decode_modrm_len)(
                opcode + 1, rex_b);
        const int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        const uint64_t mask = (access_size >= 8)
                                  ? ~0ULL
                                  : ((1ULL << (access_size * 8)) - 1);
        const uint64_t mem_val =
            read_fn(opaque, mmio_offset, (unsigned int)access_size) & mask;
        const uint64_t reg_val = *ctx_reg64(ctx, reg) & mask;
        const uint64_t result = mem_val & reg_val;
        uint32_t flags = (uint32_t)(ctx->EFlags & ~0x0881u);
        if (result == 0) flags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) flags |= 0x0080;
        ctx->EFlags = flags;
        ctx->Rip += prefix_len + 1 + modrm_len;
        return prefix_len + 1 + modrm_len;
    }

    return -1;
}

#endif /* MMIO_DECODE_UTIL_H */
