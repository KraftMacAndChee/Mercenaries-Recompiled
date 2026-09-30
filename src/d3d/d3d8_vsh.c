#include "kernel/preview_log.h"
/**
 * NV2A Vertex Shader Microcode to HLSL Translator - Implementation
 *
 * Translates NV2A 128-bit vertex shader microcode instructions into
 * HLSL vertex shader source, compiles them, and caches the results.
 *
 * The translation pipeline is:
 *   1. Parse: 128-bit instruction words -> NV2AVshInstruction structs
 *   2. Analyze: determine which input registers (v0-v15) are read
 *   3. Generate HLSL: emit HLSL code mapping NV2A ops to HLSL intrinsics
 *   4. Compile: D3DCompile -> ID3D11VertexShader
 *   5. Cache: hash microcode -> reuse compiled shader on subsequent draws
 *
 * The generated HLSL uses:
 *   - cbuffer at b1: 192 float4 constants (c0-c191)
 *   - cbuffer at b2: host surface size and Xbox depth range
 *   - Input semantics: ATTR0-ATTR15 mapped to v0-v15
 *   - Output semantics: SV_POSITION, COLOR0/1, TEXCOORD0-3, FOG, PSIZE
 */

#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "d3d8_shader_cache.h"
#include "d3d8_compiler.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>

#pragma comment(lib, "d3dcompiler.lib")

/* Vertex-shader diagnostics are immutable process-launch configuration. Shader
 * handles may be recreated frequently, so cache both present and absent values
 * instead of entering the CRT environment lock on every creation. This cache is
 * owned by the rendering thread, matching the shader translator itself. */
static const char *vsh_cached_getenv(const char *name)
{
    typedef struct VshEnvCacheEntry {
        const char *name;
        const char *value;
    } VshEnvCacheEntry;
    static VshEnvCacheEntry cache[16];
    uintptr_t key = (uintptr_t)name;
    size_t index = (size_t)((key >> 4u) ^ (key >> 12u)) & 15u;
    size_t probe;

    for (probe = 0u; probe < 16u; ++probe) {
        VshEnvCacheEntry *entry = &cache[(index + probe) & 15u];
        if (entry->name == name)
            return entry->value;
        if (entry->name == NULL) {
            entry->name = name;
            entry->value = getenv(name);
            return entry->value;
        }
    }
    return getenv(name);
}
/* ================================================================
 * NV2A Instruction Bit Field Extraction
 *
 * Each instruction is 128 bits stored as 4 DWORDs (word[0..3]).
 * The following macros extract individual fields.
 *
 * Bit layout follows the envytools / xemu conventions:
 *
 * word[0] bits:
 *   [3:0]   = (unused / type marker)
 *   [24:21] = MAC opcode
 *   [28:25] = ILU opcode
 *   [20:13] = Constant register index
 *   [12:9]  = Input register index (v0-v15)
 *   [8]     = Source A negate
 *   [7:6]   = Source A register type
 *   [5:2]   = Source A temp register index (high bits)
 *
 * word[1] bits:
 *   [31:26] = Source A temp reg index (low bit) + swizzle X,Y
 *   [25:24] = Source A swizzle Z
 *   [23:22] = Source A swizzle W
 *   [21]    = Source B negate
 *   [20:19] = Source B register type
 *   [18:15] = Source B temp register index
 *   [14:13] = Source B swizzle X
 *   [12:11] = Source B swizzle Y
 *   [10:9]  = Source B swizzle Z
 *   [8:7]   = Source B swizzle W
 *   [6]     = Source C negate
 *   [5:4]   = Source C register type
 *   [3:0]   = Source C temp register index (high bits)
 *
 * word[2] bits:
 *   [31:28] = Source C temp reg index (low bits)
 *   [27:26] = Source C swizzle X
 *   [25:24] = Source C swizzle Y
 *   [23:22] = Source C swizzle Z
 *   [21:20] = Source C swizzle W
 *   [19:16] = MAC dest temp register index
 *   [15:12] = MAC dest write mask
 *   [11:3]  = MAC dest output register + mux
 *   [2:0]   = ILU dest fields (high bits)
 *
 * word[3] bits:
 *   [31:28] = ILU dest temp register index
 *   [27:24] = ILU dest write mask
 *   [23:14] = ILU dest output register + mux
 *   [13]    = Relative addressing flag (a0.x)
 *   ...
 *   [0]     = Final instruction flag
 *
 * NOTE: The exact bit positions below are derived from the xemu
 * NV2A vertex shader decoder. Different references may number
 * bits differently (MSB-first vs LSB-first within the 128-bit
 * word). We use the convention where word[0] bit 0 is the LSB.
 * ================================================================ */

/*
 * We use a helper to extract arbitrary bit fields from the 128-bit
 * instruction. Bits are numbered 0..127 where bit 0 is word[0] bit 0.
 */
static inline uint32_t vsh_extract(const DWORD *insn, int start, int count)
{
    int word_idx = start / 32;
    int bit_ofs  = start % 32;
    uint32_t mask = (count == 32) ? 0xFFFFFFFF : ((1u << count) - 1);

    if (bit_ofs + count <= 32) {
        return (insn[word_idx] >> bit_ofs) & mask;
    }
    /* Field spans two words */
    uint32_t lo = insn[word_idx] >> bit_ofs;
    uint32_t hi = insn[word_idx + 1] << (32 - bit_ofs);
    return (lo | hi) & mask;
}

/*
 * NV2A instruction field positions (bit offsets within 128-bit instruction).
 *
 * These follow the xemu vsh_decode() ordering. The 128-bit instruction
 * is stored as DWORD[0] = bits [31:0], DWORD[1] = bits [63:32], etc.
 *
 * However, NV2A documentation typically describes the instruction in
 * big-endian bit order. We define fields from the xemu-style extraction
 * where the 128-bit word is treated as a single integer with bit 0 at
 * the LSB of word[0].
 */

/* Word 0 fields */
#define VSH_FIELD_ILU_OP_START      57
#define VSH_FIELD_ILU_OP_SIZE       3
#define VSH_FIELD_MAC_OP_START      53
#define VSH_FIELD_MAC_OP_SIZE       4
#define VSH_FIELD_CONST_IDX_START   45
#define VSH_FIELD_CONST_IDX_SIZE    8
#define VSH_FIELD_INPUT_IDX_START   41
#define VSH_FIELD_INPUT_IDX_SIZE    4

/* Source A (spans word 0 and word 1) */
#define VSH_FIELD_SRC_A_NEG_START   40
#define VSH_FIELD_SRC_A_NEG_SIZE    1
#define VSH_FIELD_SRC_A_TYPE_START  90
#define VSH_FIELD_SRC_A_TYPE_SIZE   2
#define VSH_FIELD_SRC_A_IDX_START   92
#define VSH_FIELD_SRC_A_IDX_SIZE    4
#define VSH_FIELD_SRC_A_SWZ_X_START 38  /* word 1 bit 6 = abs bit 38 */
#define VSH_FIELD_SRC_A_SWZ_X_SIZE  2
#define VSH_FIELD_SRC_A_SWZ_Y_START 36
#define VSH_FIELD_SRC_A_SWZ_Y_SIZE  2
#define VSH_FIELD_SRC_A_SWZ_Z_START 34
#define VSH_FIELD_SRC_A_SWZ_Z_SIZE  2
#define VSH_FIELD_SRC_A_SWZ_W_START 32
#define VSH_FIELD_SRC_A_SWZ_W_SIZE  2

/* Source B (word 1) */
#define VSH_FIELD_SRC_B_NEG_START   89
#define VSH_FIELD_SRC_B_NEG_SIZE    1
#define VSH_FIELD_SRC_B_TYPE_START  75
#define VSH_FIELD_SRC_B_TYPE_SIZE   2
#define VSH_FIELD_SRC_B_IDX_START   77
#define VSH_FIELD_SRC_B_IDX_SIZE    4
#define VSH_FIELD_SRC_B_SWZ_X_START 87
#define VSH_FIELD_SRC_B_SWZ_X_SIZE  2
#define VSH_FIELD_SRC_B_SWZ_Y_START 85
#define VSH_FIELD_SRC_B_SWZ_Y_SIZE  2
#define VSH_FIELD_SRC_B_SWZ_Z_START 83
#define VSH_FIELD_SRC_B_SWZ_Z_SIZE  2
#define VSH_FIELD_SRC_B_SWZ_W_START 81
#define VSH_FIELD_SRC_B_SWZ_W_SIZE  2

/* Source C (word 1/2 boundary) */
#define VSH_FIELD_SRC_C_NEG_START   74
#define VSH_FIELD_SRC_C_NEG_SIZE    1
#define VSH_FIELD_SRC_C_TYPE_START  124
#define VSH_FIELD_SRC_C_TYPE_SIZE   2
#define VSH_FIELD_SRC_C_IDX_START   62
#define VSH_FIELD_SRC_C_IDX_SIZE    4
#define VSH_FIELD_SRC_C_SWZ_X_START 72
#define VSH_FIELD_SRC_C_SWZ_X_SIZE  2
#define VSH_FIELD_SRC_C_SWZ_Y_START 70
#define VSH_FIELD_SRC_C_SWZ_Y_SIZE  2
#define VSH_FIELD_SRC_C_SWZ_Z_START 68
#define VSH_FIELD_SRC_C_SWZ_Z_SIZE  2
#define VSH_FIELD_SRC_C_SWZ_W_START 66
#define VSH_FIELD_SRC_C_SWZ_W_SIZE  2

/* MAC destination (word 2) */
#define VSH_FIELD_MAC_DST_TEMP_START   116
#define VSH_FIELD_MAC_DST_TEMP_SIZE    4
#define VSH_FIELD_MAC_DST_MASK_START   120
#define VSH_FIELD_MAC_DST_MASK_SIZE    4
#define VSH_FIELD_MAC_DST_OUT_START    99
#define VSH_FIELD_MAC_DST_OUT_SIZE     8  /* mux field for output reg select */

/* ILU destination (word 3) */
#define VSH_FIELD_ILU_DST_TEMP_START   116
#define VSH_FIELD_ILU_DST_TEMP_SIZE    4
#define VSH_FIELD_ILU_DST_MASK_START   112
#define VSH_FIELD_ILU_DST_MASK_SIZE    4
#define VSH_FIELD_ILU_DST_OUT_START    99
#define VSH_FIELD_ILU_DST_OUT_SIZE     8
#define VSH_FIELD_OUTPUT_MASK_START    108
#define VSH_FIELD_OUTPUT_MASK_SIZE     4
#define VSH_FIELD_OUTPUT_ORB_START     107
#define VSH_FIELD_OUTPUT_MUX_START     98

/* Misc flags */
#define VSH_FIELD_REL_ADDR_START   97
#define VSH_FIELD_REL_ADDR_SIZE    1
#define VSH_FIELD_FINAL_START      0    /* bit 0 of word 3, but stored at bit 96 abs */
#define VSH_FIELD_FINAL_BIT        96   /* Actually stored at a specific position */

/* ================================================================
 * Module State
 * ================================================================ */

/* Stored shader programs */
static NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];
static int g_vsh_slot_count = 0;

/* Constant registers (192 float4) */
static NV2AVSConstants g_vsh_constants;
static BOOL g_vsh_constants_dirty = TRUE;
static DXGI_FORMAT g_vsh_input_formats[NV2A_VS_MAX_INPUTS];
static UINT g_vsh_input_offsets[NV2A_VS_MAX_INPUTS];
static UINT g_vsh_input_components[NV2A_VS_MAX_INPUTS];
static uint32_t g_vsh_input_bgra_mask;
static float g_vsh_inline_values[NV2A_VS_MAX_INPUTS][4];
static uint32_t g_vsh_input_layout_key;

/* D3D11 constant buffer for VS constants */
static ID3D11Buffer *g_vsh_cb = NULL;
static ID3D11Buffer *g_vsh_host_cb = NULL;

typedef struct NV2AVSHostConstants {
    float surface_width;
    float surface_height;
    float depth_max;
    float padding;
    float inline_values[NV2A_VS_MAX_INPUTS][4];
} NV2AVSHostConstants;

static NV2AVSHostConstants g_vsh_host_constants;
static BOOL g_vsh_host_constants_valid = FALSE;

/* Shader cache: maps microcode hash to compiled shader + input layout */
typedef struct {
    uint32_t            hash;
    int                 in_use;
    ID3D11VertexShader *vs;
    ID3DBlob           *vs_blob;      /* Bytecode for input layout creation */
    ID3D11InputLayout  *layouts[16];  /* Cached layouts per input mask subset */
    uint32_t            layout_keys[16];
    int                 layout_count;
    uint16_t            inputs_read;  /* Which v registers are read */
} VshCacheEntry;

static VshCacheEntry g_vsh_cache[NV2A_VS_CACHE_SIZE];
static void (*g_vsh_warmup_callback)(void);
void d3d8_vsh_set_warmup(void (*callback)(void)) { g_vsh_warmup_callback = callback; }


/* ================================================================
 * Hash Function (FNV-1a)
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193;
    }
    return h;
}

/* ================================================================
 * Microcode Parser
 * ================================================================ */

static void parse_source(const DWORD *insn,
                          int neg_start, int type_start, uint32_t reg_idx,
                          int swz_x_start, int swz_y_start,
                          int swz_z_start, int swz_w_start,
                          int input_index, int const_index,
                          NV2AVshSrcOperand *src)
{
    uint32_t reg_type = vsh_extract(insn, type_start, 2);

    src->negate   = vsh_extract(insn, neg_start, 1);
    src->swizzle.x = (uint8_t)vsh_extract(insn, swz_x_start, 2);
    src->swizzle.y = (uint8_t)vsh_extract(insn, swz_y_start, 2);
    src->swizzle.z = (uint8_t)vsh_extract(insn, swz_z_start, 2);
    src->swizzle.w = (uint8_t)vsh_extract(insn, swz_w_start, 2);
    src->rel_addr = 0;

    switch (reg_type) {
    case 1: /* Temp register */
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = (int)reg_idx;
        break;
    case 2: /* Input register v# */
        src->reg_type  = NV2A_VSH_REG_INPUT;
        src->reg_index = input_index;
        break;
    case 3: /* Constant register c# */
        src->reg_type  = NV2A_VSH_REG_CONST;
        src->reg_index = const_index;
        break;
    default:
        /* Treat as temp */
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = 0;
        break;
    }
}

static NV2AVshOutputReg decode_output_mux(uint32_t mux_val)
{
    /* The output register mux field encodes which output register.
     * The low nibble gives the output type. */
    uint32_t out_idx = mux_val & 0xF;
    switch (out_idx) {
    case 0:  return NV2A_VSH_OUT_POS;
    case 3:  return NV2A_VSH_OUT_D0;
    case 4:  return NV2A_VSH_OUT_D1;
    case 5:  return NV2A_VSH_OUT_FOG;
    case 6:  return NV2A_VSH_OUT_PTS;
    case 7:  return NV2A_VSH_OUT_B0;
    case 8:  return NV2A_VSH_OUT_B1;
    case 9:  return NV2A_VSH_OUT_T0;
    case 10: return NV2A_VSH_OUT_T1;
    case 11: return NV2A_VSH_OUT_T2;
    case 12: return NV2A_VSH_OUT_T3;
    default: return NV2A_VSH_OUT_NONE;
    }
}

void d3d8_vsh_parse(const DWORD *microcode, int num_insns,
                     NV2AVshProgram *program)
{
    int i;
    memset(program, 0, sizeof(*program));
    program->inputs_read = 0;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    for (i = 0; i < num_insns; i++) {
        const DWORD *insn = &microcode[i * 4];
        NV2AVshInstruction *inst = &program->insns[i];

        /* Extract opcodes */
        inst->mac_op = (NV2AVshMacOp)vsh_extract(insn, VSH_FIELD_MAC_OP_START,
                                                   VSH_FIELD_MAC_OP_SIZE);
        inst->ilu_op = (NV2AVshIluOp)vsh_extract(insn, VSH_FIELD_ILU_OP_START,
                                                   VSH_FIELD_ILU_OP_SIZE);

        /* Shared constant and input register indices */
        inst->const_index = (int)vsh_extract(insn, VSH_FIELD_CONST_IDX_START,
                                              VSH_FIELD_CONST_IDX_SIZE);
        inst->input_index = (int)vsh_extract(insn, VSH_FIELD_INPUT_IDX_START,
                                              VSH_FIELD_INPUT_IDX_SIZE);

        /* Clamp indices to valid ranges */
        if (inst->const_index >= NV2A_VS_MAX_CONSTANTS)
            inst->const_index = 0;
        if (inst->input_index >= NV2A_VS_MAX_INPUTS)
            inst->input_index = 0;

        /* Parse source operands A, B, C */
        parse_source(insn,
                     VSH_FIELD_SRC_A_NEG_START, VSH_FIELD_SRC_A_TYPE_START,
                     vsh_extract(insn, VSH_FIELD_SRC_A_IDX_START,
                                 VSH_FIELD_SRC_A_IDX_SIZE),
                     VSH_FIELD_SRC_A_SWZ_X_START, VSH_FIELD_SRC_A_SWZ_Y_START,
                     VSH_FIELD_SRC_A_SWZ_Z_START, VSH_FIELD_SRC_A_SWZ_W_START,
                     inst->input_index, inst->const_index,
                     &inst->mac_src[0]);

        parse_source(insn,
                     VSH_FIELD_SRC_B_NEG_START, VSH_FIELD_SRC_B_TYPE_START,
                     vsh_extract(insn, VSH_FIELD_SRC_B_IDX_START,
                                 VSH_FIELD_SRC_B_IDX_SIZE),
                     VSH_FIELD_SRC_B_SWZ_X_START, VSH_FIELD_SRC_B_SWZ_Y_START,
                     VSH_FIELD_SRC_B_SWZ_Z_START, VSH_FIELD_SRC_B_SWZ_W_START,
                     inst->input_index, inst->const_index,
                     &inst->mac_src[1]);

        parse_source(insn,
                     VSH_FIELD_SRC_C_NEG_START, VSH_FIELD_SRC_C_TYPE_START,
                     (vsh_extract(insn, 64, 2) << 2) |
                         vsh_extract(insn, 126, 2),
                     VSH_FIELD_SRC_C_SWZ_X_START, VSH_FIELD_SRC_C_SWZ_Y_START,
                     VSH_FIELD_SRC_C_SWZ_Z_START, VSH_FIELD_SRC_C_SWZ_W_START,
                     inst->input_index, inst->const_index,
                     &inst->mac_src[2]);

        /* ILU source = source C */
        inst->ilu_src = inst->mac_src[2];

        /* Check for relative addressing */
        {
            uint32_t rel = vsh_extract(insn, VSH_FIELD_REL_ADDR_START,
                                        VSH_FIELD_REL_ADDR_SIZE);
            if (rel) {
                /* Mark const-type sources as relatively addressed */
                int s;
                for (s = 0; s < 3; s++) {
                    if (inst->mac_src[s].reg_type == NV2A_VSH_REG_CONST)
                        inst->mac_src[s].rel_addr = 1;
                }
                if (inst->ilu_src.reg_type == NV2A_VSH_REG_CONST)
                    inst->ilu_src.rel_addr = 1;
            }
        }

        /* Xemu's output fields share one temp register and independently
         * mask MAC, ILU, and output writes. */
        {
            uint32_t temp_idx = vsh_extract(insn, VSH_FIELD_MAC_DST_TEMP_START,
                                             VSH_FIELD_MAC_DST_TEMP_SIZE);
            uint32_t mac_mask = vsh_extract(insn, VSH_FIELD_MAC_DST_MASK_START,
                                             VSH_FIELD_MAC_DST_MASK_SIZE);
            uint32_t ilu_mask = vsh_extract(insn, VSH_FIELD_ILU_DST_MASK_START,
                                             VSH_FIELD_ILU_DST_MASK_SIZE);
            uint32_t out_mask = vsh_extract(insn, VSH_FIELD_OUTPUT_MASK_START,
                                             VSH_FIELD_OUTPUT_MASK_SIZE);
            uint32_t out_addr = vsh_extract(insn, VSH_FIELD_MAC_DST_OUT_START,
                                             VSH_FIELD_MAC_DST_OUT_SIZE);
            uint32_t out_mux = vsh_extract(insn, VSH_FIELD_OUTPUT_MUX_START, 1);
            uint32_t out_orb = vsh_extract(insn, VSH_FIELD_OUTPUT_ORB_START, 1);

            inst->mac_dst.temp_reg = -1;
            inst->mac_dst.write_mask = 0;
            inst->mac_dst.output_reg = NV2A_VSH_OUT_NONE;
            inst->mac_dst.output_write_mask = 0;
            inst->ilu_dst.temp_reg = -1;
            inst->ilu_dst.write_mask = 0;
            inst->ilu_dst.output_reg = NV2A_VSH_OUT_NONE;
            inst->ilu_dst.output_write_mask = 0;

            if (inst->mac_op != NV2A_VSH_MAC_NOP) {
                inst->mac_dst.temp_reg = (int)temp_idx;
                inst->mac_dst.write_mask = (uint8_t)mac_mask;
            }
            if (inst->ilu_op != NV2A_VSH_ILU_NOP) {
                inst->ilu_dst.temp_reg =
                    inst->mac_op != NV2A_VSH_MAC_NOP ? 1 : (int)temp_idx;
                inst->ilu_dst.write_mask = (uint8_t)ilu_mask;
            }
            if (out_mask && out_orb) {
                NV2AVshDstOperand *dst = out_mux ? &inst->ilu_dst :
                                                   &inst->mac_dst;
                dst->output_reg = decode_output_mux(out_addr);
                dst->output_write_mask = (uint8_t)out_mask;
            }
        }
        /* Final instruction flag (bit 0 of word 3) */
        inst->is_final = (insn[3] & 1) ? 1 : 0;

        /* Track input register usage */
        {
            int s;
            for (s = 0; s < 3; s++) {
                if (inst->mac_src[s].reg_type == NV2A_VSH_REG_INPUT)
                    program->inputs_read |= (1u << inst->mac_src[s].reg_index);
            }
            if (inst->ilu_src.reg_type == NV2A_VSH_REG_INPUT)
                program->inputs_read |= (1u << inst->ilu_src.reg_index);
        }

        program->length = i + 1;

        /* Stop at final instruction */
        if (inst->is_final)
            break;
    }
}

/* ================================================================
 * HLSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    if (size > 0) buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    if (sb->pos >= sb->size - 1) return;
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, remaining, fmt, ap);
    va_end(ap);
    if (n > 0 && n < remaining)
        sb->pos += n;
    else if (n >= remaining)
        sb->pos = sb->size - 1;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index == 12)
            sb_append(sb, "R12"); /* oPos alias */
        else
            sb_append(sb, "R%d", src->reg_index);
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr)
            sb_append(sb, "c[a0 + %d]", src->reg_index);
        else
            sb_append(sb, "c[%d]", src->reg_index);
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an HLSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit a destination assignment (temp and/or output register write).
 *
 * The NV2A can write to both a temp register and an output register
 * simultaneously from the same operation. Evaluate the result once before
 * either write, since the expression can read the temporary destination.
 *
 * @param dst     The destination operand
 * @param rhs     The HLSL expression to assign (right-hand side)
 */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst,
                              const char *rhs)
{
    const int dual_write = dst->temp_reg >= 0 && dst->write_mask != 0 &&
        dst->output_reg != NV2A_VSH_OUT_NONE && dst->output_write_mask != 0;
    if (dual_write) {
        sb_append(sb, "    { float4 result = (%s);\n", rhs);
        rhs = "result";
    }
    /* Write to temp register if valid */
    if (dst->temp_reg >= 0 && dst->write_mask != 0) {
        if (dst->temp_reg == 12)
            sb_append(sb, "    R12");
        else
            sb_append(sb, "    R%d", dst->temp_reg);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = (%s)", rhs);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
    }

    /* Write to output register if specified */
    if (dst->output_reg != NV2A_VSH_OUT_NONE &&
        dst->output_write_mask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            sb_append(sb, "    %s", name);
            emit_write_mask(sb, dst->output_write_mask);
            sb_append(sb, " = (%s)", rhs);
            emit_write_mask(sb, dst->output_write_mask);
            sb_append(sb, ";\n");
        }
    }
    if (dual_write) sb_append(sb, "    }\n");
}

/**
 * Emit the HLSL for one MAC operation.
 */
static void emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst,
                        int paired, int instruction_index)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->mac_op) {
    case NV2A_VSH_MAC_NOP:
        return;

    case NV2A_VSH_MAC_MOV:
        /* dst = A */
        emit_source(&expr, &inst->mac_src[0], 0);
        break;

    case NV2A_VSH_MAC_MUL:
        /* dst = A * B */
        sb_append(&expr, "nv2a_mul(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ADD:
        /* dst = A + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAD:
        /* dst = A * B + C */
        sb_append(&expr, "(nv2a_mul(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ") + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".xyz).xxxx");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "dot(float4(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, 1.0), ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w) */
        sb_append(&expr, "float4(1.0, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".y * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".y, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".z, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".w)");
        break;

    case NV2A_VSH_MAC_MIN:
        sb_append(&expr, "min(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAX:
        sb_append(&expr, "max(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_SLT:
        /* dst = (A < B) ? 1.0 : 0.0
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a)
         * Equivalent to: step(a, b) where a < b yields 1
         * Using explicit form for clarity: */
        sb_append(&expr, "(1.0 - step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_SGE:
        /* dst = (A >= B) ? 1.0 : 0.0
         * step(edge, x) returns 1 if x >= edge, 0 otherwise */
        sb_append(&expr, "step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ARL:
        /* Match Xemu/NV2A address-register conversion. Bone indices often
         * arrive through normalized vertex attributes; their round trip can
         * produce 16.999... for an intended 17. Add the hardware-compatible
         * bias before floor so skinning selects the correct matrix palette. */
        if (paired)
            sb_append(sb, "    int _pairedAddr%d = (int)floor(",
                      instruction_index);
        else
            sb_append(sb, "    a0 = (int)floor(");
        emit_source(sb, &inst->mac_src[0], 0);
        sb_append(sb, ".x + 0.001);\n");
        return; /* No destination register write */

    default:
        return;
    }

    if (paired) {
        NV2AVshDstOperand output_only = inst->mac_dst;
        char paired_name[32];
        snprintf(paired_name, sizeof(paired_name), "_pairedMac%d",
                 instruction_index);
        sb_append(sb, "    float4 %s = %s;\n", paired_name, expr_buf);
        output_only.temp_reg = -1;
        output_only.write_mask = 0;
        emit_dest_assign(sb, &output_only, paired_name);
    } else {
        emit_dest_assign(sb, &inst->mac_dst, expr_buf);
    }
}

static void emit_paired_mac_commit(StrBuf *sb,
                                   const NV2AVshInstruction *inst,
                                   int instruction_index)
{
    const NV2AVshDstOperand *dst = &inst->mac_dst;

    if (inst->mac_op == NV2A_VSH_MAC_ARL) {
        sb_append(sb, "    a0 = _pairedAddr%d;\n", instruction_index);
        return;
    }
    /* Xemu/NV2A: a paired ILU always owns R1. A simultaneous MAC whose
     * encoded temporary destination is R1 has that temporary write masked. */
    if (dst->temp_reg < 0 || dst->temp_reg == 1 || dst->write_mask == 0)
        return;
    if (dst->temp_reg == 12)
        sb_append(sb, "    R12");
    else
        sb_append(sb, "    R%d", dst->temp_reg);
    emit_write_mask(sb, dst->write_mask);
    sb_append(sb, " = (_pairedMac%d)", instruction_index);
    emit_write_mask(sb, dst->write_mask);
    sb_append(sb, ";\n");
}

/**
 * Emit the HLSL for one ILU operation.
 */
static void emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_NOP:
        return;

    case NV2A_VSH_ILU_MOV:
        /* dst = C */
        emit_source(&expr, &inst->ilu_src, 0);
        break;

    case NV2A_VSH_ILU_RCP:
        /* dst = (1.0 / C.x).xxxx */
        sb_append(&expr, "(1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RCC:
        /* Xemu/NV2A RCC preserves the reciprocal's sign while clamping its
         * magnitude to [2^-64, 2^64]. */
        sb_append(&expr, "nv2a_rcc(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = (1.0 / sqrt(abs(C.x))).xxxx */
        sb_append(&expr, "rsqrt(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")).xxxx");
        break;

    case NV2A_VSH_ILU_EXP:
        /* NV2A EXP is component-specific rather than a scalar splat:
         *   x = 2^floor(C.x), y = frac(C.x), z = 2^C.x, w = 1.
         * Particle shaders commonly write only .y and use it to interpolate
         * adjacent constant-table entries. */
        sb_append(&expr, "nv2a_exp(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_ILU_LOG: {
        /* dst = log2(abs(C.x)).xxxx
         * Guard against log2(0) which is -inf on NV2A -> clamp to large negative */
        sb_append(&expr, "log2(max(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.175494e-38)).xxxx");
        break;
    }

    case NV2A_VSH_ILU_LIT: {
        /* NV2A LIT instruction:
         *   dst.x = 1.0
         *   dst.y = max(src.x, 0.0)
         *   dst.z = (src.x > 0) ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0
         *   dst.w = 1.0
         *
         * We emit a helper call. The lit() HLSL intrinsic has similar but
         * not identical semantics, so we use an inline expansion. */
        sb_append(&expr, "float4(1.0, max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x, 0.0), (");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x > 0.0) ? exp2(clamp(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".w, -128.0, 128.0) * log2(max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".y, 0.0) + 1e-30)) : 0.0, 1.0)");
        break;
    }

    default:
        return;
    }

    emit_dest_assign(sb, &inst->ilu_dst, expr_buf);
}

/**
 * Map NV2A input register index to a D3D11 input semantic.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position
 *   v1  = Blend weight
 *   v2  = Normal
 *   v3  = Diffuse color
 *   v4  = Specular color
 *   v5  = Fog coordinate
 *   v6  = Point size / back diffuse
 *   v7  = Back specular
 *   v8  = Texture coord 0
 *   v9  = Texture coord 1
 *   v10 = Texture coord 2
 *   v11 = Texture coord 3
 *   v12-v15 = Additional attributes
 *
 * We use generic ATTR semantics so the input layout can match any
 * vertex buffer format at bind time.
 */
static const char *input_semantic_name(int reg_index)
{
    /* All inputs use the generic TEXCOORD semantic with unique indices
     * to avoid mismatches. The input layout will map them correctly. */
    (void)reg_index;
    return "ATTR";
}

static BOOL input_format_is_sint(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32_SINT:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Recognize the ordinary screenspace perspective projection by data flow,
 * independent of shader hashes, register allocation, materials or game assets.
 * Keep its depth relative to depthMax until the pixel shader. Forming a roughly
 * 16-million-unit absolute Z first discards the fractional depth separating
 * distant windows from walls. Other outputs retain the original instruction
 * sequence; nonstandard/clamped projections use the original depth path. */
typedef struct VshDepthRef { int bank, reg, component; } VshDepthRef;
typedef struct VshProjectionDepth {
    int instruction;
    VshDepthRef camera_z, camera_w, scale, offset;
} VshProjectionDepth;

static VshDepthRef vsh_depth_ref(const NV2AVshSrcOperand *src, int component)
{
    const uint8_t swz[4] = {src->swizzle.x, src->swizzle.y,
                            src->swizzle.z, src->swizzle.w};
    VshDepthRef r = {src->reg_type, src->reg_index, swz[component]};
    if (src->negate || src->rel_addr) r.bank = -1;
    return r;
}
static int vsh_depth_same(VshDepthRef a, VshDepthRef b)
{ return a.bank >= 0 && a.bank == b.bank && a.reg == b.reg && a.component == b.component; }
static int vsh_depth_writes(const NV2AVshDstOperand *dst, VshDepthRef ref)
{
    unsigned bit = 8u >> ref.component;
    return (dst->temp_reg == ref.reg && (dst->write_mask & bit)) ||
           (ref.reg == 12 && dst->output_reg == NV2A_VSH_OUT_POS &&
            (dst->output_write_mask & bit));
}
/* The low bit identifies ILU. Ambiguous paired writes are rejected. */
static int vsh_depth_writer(const NV2AVshProgram *p, int before, VshDepthRef ref)
{
    int i;
    if (ref.bank != NV2A_VSH_REG_TEMP) return -1;
    for (i = before - 1; i >= 0; --i) {
        const NV2AVshInstruction *in = &p->insns[i];
        int mac = in->mac_op != NV2A_VSH_MAC_NOP && vsh_depth_writes(&in->mac_dst, ref);
        int ilu = in->ilu_op != NV2A_VSH_ILU_NOP && vsh_depth_writes(&in->ilu_dst, ref);
        if (mac && ilu) return -2;
        if (mac || ilu) return 2*i + ilu;
    }
    return -1;
}
static int vsh_depth_unchanged(const NV2AVshProgram *p, int from, int to, VshDepthRef r)
{
    int first = vsh_depth_writer(p, from, r);
    return r.bank == NV2A_VSH_REG_CONST ||
           (r.bank == NV2A_VSH_REG_TEMP && first != -2 &&
            first == vsh_depth_writer(p, to, r));
}
static VshProjectionDepth vsh_projection_depth(const NV2AVshProgram *p)
{
    VshProjectionDepth result = {0};
    VshDepthRef posz = {NV2A_VSH_REG_TEMP, 12, 2};
    int terminal = vsh_depth_writer(p, p->length, posz), order;
    result.instruction = -1;
    if (terminal < 0 || (terminal & 1)) return result;
    terminal /= 2;
    if (p->insns[terminal].mac_op != NV2A_VSH_MAC_MUL) return result;
    for (order = 0; order < 2; ++order) {
        const NV2AVshInstruction *last = &p->insns[terminal], *mad, *mul, *recip;
        VshDepthRef accum = vsh_depth_ref(&last->mac_src[order], 2);
        VshDepthRef inverse = vsh_depth_ref(&last->mac_src[1-order], 2);
        int a = vsh_depth_writer(p, terminal, accum);
        int r = vsh_depth_writer(p, terminal, inverse), m;
        VshDepthRef addend, camera_z, camera_w, scale, offset, lhs, rhs;
        if (a < 0 || (a & 1) || r < 0 || !(r & 1)) continue;
        a /= 2; r /= 2; mad = &p->insns[a]; recip = &p->insns[r];
        if (mad->mac_op != NV2A_VSH_MAC_MAD || recip->ilu_op != NV2A_VSH_ILU_RCC) continue;
        camera_z = vsh_depth_ref(&recip->ilu_src, 0);
        if (camera_z.bank != NV2A_VSH_REG_TEMP) continue;
        addend = vsh_depth_ref(&mad->mac_src[2], accum.component);
        m = vsh_depth_writer(p, a, addend);
        if (m < 0 || (m & 1)) continue;
        m /= 2; mul = &p->insns[m];
        if (mul->mac_op != NV2A_VSH_MAC_MUL) continue;
        lhs = vsh_depth_ref(&mul->mac_src[0], addend.component);
        rhs = vsh_depth_ref(&mul->mac_src[1], addend.component);
        if (vsh_depth_same(lhs, camera_z)) scale = rhs;
        else if (vsh_depth_same(rhs, camera_z)) scale = lhs;
        else continue;
        lhs = vsh_depth_ref(&mad->mac_src[0], accum.component);
        rhs = vsh_depth_ref(&mad->mac_src[1], accum.component);
        if (lhs.bank == NV2A_VSH_REG_CONST) { offset = lhs; camera_w = rhs; }
        else { offset = rhs; camera_w = lhs; }
        if (scale.bank != NV2A_VSH_REG_CONST || offset.bank != NV2A_VSH_REG_CONST ||
            camera_w.bank != NV2A_VSH_REG_TEMP) continue;
        if (!vsh_depth_unchanged(p, m, a, camera_z) ||
            !vsh_depth_unchanged(p, r, a, camera_z)) continue;
        /* Capture before MAD consumes camera W. Shaders can legitimately reuse
         * the camera register for lighting before their final oPos write. */
        result.instruction = a; result.camera_z = camera_z;
        result.camera_w = camera_w; result.scale = scale; result.offset = offset;
        break;
    }
    return result;
}

int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    uint16_t inputs = program->inputs_read;
    const VshProjectionDepth projection = vsh_projection_depth(program);

    sb_init(&sb, buf, bufsize);

    /* Constant buffer: 192 float4 constants */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "cbuffer VSH_Constants : register(b1) {\n"
        "    float4 c[%d];\n"
        "};\n"
        "cbuffer VSH_HostConstants : register(b2) {\n"
        "    float2 surfaceSize;\n"
        "    float depthMax;\n"
        "    float hostPadding;\n"
        "    float4 inlineValue[%d];\n"
        "};\n"
        "float nv2a_clamp_away_zero_inf(float v) {\n"
        "    const float lo = 5.42101086243e-20;\n"
        "    const float hi = 1.84467440737e19;\n"
        "    return (v > 0.0 || asuint(v) == 0u) ? clamp(v, lo, hi)\n"
        "                                          : clamp(v, -hi, -lo);\n"
        "}\n"
        "float nv2a_rcc(float v) {\n"
        "    return nv2a_clamp_away_zero_inf(1.0 / v);\n"
        "}\n"
        "float4 nv2a_nan_to_one(float4 v) {\n"
        "    return float4(isnan(v.x) ? 1.0 : v.x,\n"
        "                  isnan(v.y) ? 1.0 : v.y,\n"
        "                  isnan(v.z) ? 1.0 : v.z,\n"
        "                  isnan(v.w) ? 1.0 : v.w);\n"
        "}\n"
        "float4 nv2a_mul(float4 a, float4 b) {\n"
        "    float4 result = a * b;\n"
        "    float4 zeroComponents = sign(nv2a_nan_to_one(a)) *\n"
        "                            sign(nv2a_nan_to_one(b));\n"
        "    if (zeroComponents.x == 0.0) result.x = 0.0;\n"
        "    if (zeroComponents.y == 0.0) result.y = 0.0;\n"
        "    if (zeroComponents.z == 0.0) result.z = 0.0;\n"
        "    if (zeroComponents.w == 0.0) result.w = 0.0;\n"
        "    return result;\n"
        "}\n"
        "float4 nv2a_exp(float v) {\n"
        "    float whole = floor(v);\n"
        "    return float4(exp2(whole), v - whole, exp2(v), 1.0);\n"
        "}\n"
        "float4 nv2a_decompress_11_11_10(int cmp) {\n"
        "    int x = (cmp << 21) >> 21;\n"
        "    int y = (cmp << 10) >> 21;\n"
        "    int z = cmp >> 22;\n"
        "    return float4(max(-1.0, (float)x / 1023.0),\n"
        "                  max(-1.0, (float)y / 1023.0),\n"
        "                  max(-1.0, (float)z / 511.0), 1.0);\n"
        "}\n"
        "\n", NV2A_VS_MAX_CONSTANTS, NV2A_VS_MAX_INPUTS);

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if ((inputs & (1u << i)) &&
            g_vsh_input_formats[i] != DXGI_FORMAT_UNKNOWN) {
            sb_append(&sb, "    %s4 v%d : ATTR%d;\n",
                      input_format_is_sint(g_vsh_input_formats[i]) ? "int" :
                                                                    "float",
                      i, i);
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : FOG;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD4;\n"
        "    float4 oB1  : TEXCOORD5;\n"
        "    noperspective float4 guestDepth : TEXCOORD6;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i <= 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    if (projection.instruction >= 0)
        sb_append(&sb, "    float projectionDepth = 0.0;\n    bool projectionDepthValid = false;\n");

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Alias input registers for readability */
    sb_append(&sb, "    /* Input register aliases */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            if (g_vsh_input_formats[i] == DXGI_FORMAT_R32_SINT)
                sb_append(&sb,
                          "    float4 v%d = nv2a_decompress_11_11_10(input.v%d.x);\n",
                          i, i);
            else if (g_vsh_input_formats[i] != DXGI_FORMAT_UNKNOWN) {
                if (g_vsh_input_bgra_mask & (1u << i)) {
                    sb_append(&sb, "    float4 v%d = float4(input.v%d.zyxw);\n", i, i);
                    continue;
                }
                switch (g_vsh_input_components[i]) {
                case 1:
                    sb_append(&sb, "    float4 v%d = float4(input.v%d.x, 0.0, 0.0, 1.0);\n", i, i);
                    break;
                case 2:
                    sb_append(&sb, "    float4 v%d = float4(input.v%d.xy, 0.0, 1.0);\n", i, i);
                    break;
                case 3:
                    sb_append(&sb, "    float4 v%d = float4(input.v%d.xyz, 1.0);\n", i, i);
                    break;
                default:
                    sb_append(&sb, "    float4 v%d = float4(input.v%d);\n", i, i);
                    break;
                }
            }
            else
                sb_append(&sb, "    float4 v%d = inlineValue[%d];\n", i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables */
    sb_append(&sb,
        "    /* Output registers (initialized to zero) */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,1);\n"
        "    float4 oPts = float4(0,0,0,1);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];

        sb_append(&sb, "\n    /* Instruction %d */\n", i);

        if (i == projection.instruction) {
            sb_append(&sb,
                "    projectionDepthValid = R%d.%c == 1.0 && R%d.%c > 5.42101086243e-20 && R%d.%c < 1.84467440737e19;\n"
                "    if (projectionDepthValid) projectionDepth = (c[%d].%c - depthMax) + c[%d].%c / R%d.%c;\n",
                projection.camera_w.reg, "xyzw"[projection.camera_w.component],
                projection.camera_z.reg, "xyzw"[projection.camera_z.component],
                projection.camera_z.reg, "xyzw"[projection.camera_z.component],
                projection.scale.reg, "xyzw"[projection.scale.component],
                projection.offset.reg, "xyzw"[projection.offset.component],
                projection.camera_z.reg, "xyzw"[projection.camera_z.component]);
        }

        /* Paired MAC/ILU operations read their sources in parallel. Match
         * Xemu by delaying the MAC temporary-register commit until after the
         * ILU has consumed the instruction's pre-write register state. */
        {
            const int paired = inst->mac_op != NV2A_VSH_MAC_NOP &&
                               inst->ilu_op != NV2A_VSH_ILU_NOP;
            if (inst->mac_op != NV2A_VSH_MAC_NOP)
                emit_mac_op(&sb, inst, paired, i);
            if (inst->ilu_op != NV2A_VSH_ILU_NOP)
                emit_ilu_op(&sb, inst);
            if (paired)
                emit_paired_mac_commit(&sb, inst, i);
        }
    }

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Populate output structure */
    sb_append(&sb,
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    /* Match Xemu's programmable VSH epilogue: NV2A oPos is in\n"
        "     * screen coordinates with 24-bit depth, then the rasterizer\n"
        "     * converts it to D3D11 homogeneous clip space. */\n"
        "    /* Preserve projection precision for perspective geometry. Rounding\n"
        "     * screen XY without adjusting Z tilts the interpolated depth\n"
        "     * plane and makes closely layered decals intersect their walls.\n"
        "     * Screen-space UI keeps the original subpixel alignment. */\n"
        "    if (%s || oPos.w == 1.0) {\n"
        "    oPos.xy = trunc(oPos.xy * 16.0) / 16.0;\n"
        "    /* Preserve native D3D8 full-surface coverage when D3D11 scales\n"
        "     * the logical surface.  Half-pixel surface bounds describe the\n"
        "     * outer guest samples; without edge snapping an internal scale\n"
        "     * magnifies them into uncovered right/bottom strips. */\n"
        "    if (abs(oPos.x + 0.5) < 0.03125) oPos.x = 0.0;\n"
        "    if (abs(oPos.y + 0.5) < 0.03125) oPos.y = 0.0;\n"
        "    if (abs(oPos.x - (surfaceSize.x - 0.5)) < 0.03125) oPos.x = surfaceSize.x;\n"
        "    if (abs(oPos.y - (surfaceSize.y - 0.5)) < 0.03125) oPos.y = surfaceSize.y;\n"
        "    }\n"
        "    float2 guestScreen = oPos.xy;\n"
        "    oPos.w = nv2a_clamp_away_zero_inf(oPos.w);\n"
        "    oPos.x = (2.0 * oPos.x / surfaceSize.x - 1.0) * oPos.w;\n"
        "    oPos.y = (1.0 - 2.0 * oPos.y / surfaceSize.y) * oPos.w;\n"
        "    o.guestDepth = float4(oPos.z - depthMax, depthMax, guestScreen);\n"
        "    oPos.z = (oPos.z / depthMax) * oPos.w;\n"
        "    o.oPos = oPos;\n"
        "    o.oD0  = %s;\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(nv2a_nan_to_one(oD1));\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n"
        "    o.oFog = oFog.x;\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(nv2a_nan_to_one(oB0));\n"
        "    o.oB1  = saturate(nv2a_nan_to_one(oB1));\n"
        "%s"
        "    return o;\n"
        "}\n",
        vsh_cached_getenv("MERCENARIES_STRICT_SCREEN_ROUNDING") != NULL ?
            "true" : "false",
        vsh_cached_getenv("MERCENARIES_DEBUG_VSH_COLOR") != NULL &&
                (inputs & (1u << 3)) ?
            "float4(saturate(input.v3.x / 8.0), saturate(c[41].g), "
            "saturate(oD0.b), 1.0)" :
            "saturate(nv2a_nan_to_one(oD0))",
        projection.instruction >= 0 ?
            "    if (projectionDepthValid && isfinite(projectionDepth)) o.guestDepth.x = projectionDepth;\n" : "");

    return sb.pos;
}

/* ================================================================
 * Input Layout Management
 *
 * When using a programmable VS, we need an input layout that matches
 * the shader's declared inputs. The layout maps vertex buffer elements
 * to the ATTR# semantics declared in the generated HLSL.
 *
 * The mapping from NV2A v# registers to vertex data depends on the
 * game's vertex stream setup. We use a simple mapping:
 *
 *   v0  -> ATTR0  -> POSITION (float4, offset 0)
 *   v1  -> ATTR1  -> BLENDWEIGHT (float4)
 *   v2  -> ATTR2  -> NORMAL (float4)
 *   v3  -> ATTR3  -> DIFFUSE (float4 / D3DCOLOR)
 *   v4  -> ATTR4  -> SPECULAR (float4 / D3DCOLOR)
 *   v5  -> ATTR5  -> FOG (float4)
 *   v6  -> ATTR6  -> POINTSIZE / BACKDIFFUSE (float4)
 *   v7  -> ATTR7  -> BACKSPECULAR (float4)
 *   v8  -> ATTR8  -> TEXCOORD0 (float4)
 *   v9  -> ATTR9  -> TEXCOORD1 (float4)
 *   v10 -> ATTR10 -> TEXCOORD2 (float4)
 *   v11 -> ATTR11 -> TEXCOORD3 (float4)
 *   v12-v15 -> ATTR12-15 -> additional
 *
 * The actual format (float2/3/4, D3DCOLOR, etc.) is determined at
 * draw time from the active stream source FVF/stride. For now, we
 * create a layout assuming the standard Xbox vertex attribute mapping.
 * ================================================================ */


static ID3D11InputLayout *create_vsh_input_layout(
    uint16_t inputs_read, ID3DBlob *vs_blob)
{
    D3D11_INPUT_ELEMENT_DESC elems[NV2A_VS_MAX_INPUTS];
    UINT elem_count = 0;
    ID3D11InputLayout *layout = NULL;
    HRESULT hr;
    int i;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(inputs_read & (1u << i)) ||
            g_vsh_input_formats[i] == DXGI_FORMAT_UNKNOWN)
            continue;

        DXGI_FORMAT fmt = g_vsh_input_formats[i];

        elems[elem_count].SemanticName      = "ATTR";
        elems[elem_count].SemanticIndex      = (UINT)i;
        elems[elem_count].Format             = fmt;
        elems[elem_count].InputSlot          = 0;
        elems[elem_count].AlignedByteOffset  = g_vsh_input_offsets[i];
        elems[elem_count].InputSlotClass     = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;
    }

    if (elem_count == 0) return NULL;

    hr = ID3D11Device_CreateInputLayout(
        d3d8_GetD3D11Device(),
        elems, elem_count,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &layout);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateInputLayout failed: 0x%08lX\n", hr);
        return NULL;
    }

    return layout;
}

/* ================================================================
 * Shader Compilation and Caching
 * ================================================================ */

static VshCacheEntry *cache_lookup(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use)
            return NULL;
        if (g_vsh_cache[probe].hash == hash)
            return &g_vsh_cache[probe];
    }
    return NULL;
}

static VshCacheEntry *cache_insert(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;

    /* Find an empty slot or reuse the probed slot */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use) {
            g_vsh_cache[probe].hash   = hash;
            g_vsh_cache[probe].in_use = 1;
            return &g_vsh_cache[probe];
        }
    }

    /* Cache full: evict the first probed entry */
    {
        VshCacheEntry *evict = &g_vsh_cache[idx];
        int j;

        if (evict->vs)
            ID3D11VertexShader_Release(evict->vs);
        if (evict->vs_blob)
            ID3D10Blob_Release(evict->vs_blob);
        for (j = 0; j < evict->layout_count; j++) {
            if (evict->layouts[j])
                ID3D11InputLayout_Release(evict->layouts[j]);
        }
        memset(evict, 0, sizeof(*evict));
        evict->hash   = hash;
        evict->in_use = 1;
        return evict;
    }
}

/**
 * Compile a vertex shader from microcode.
 *
 * Parses, generates HLSL, compiles, and caches the result.
 * Returns the cache entry (with compiled VS and blob).
 */
static VshCacheEntry *compile_shader(const DWORD *microcode, int num_insns,
                                      uint32_t hash)
{
    NV2AVshProgram program;
    char hlsl_buf[16384];  /* 16KB should be enough for any VS */
    int hlsl_len;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;
    VshCacheEntry *entry;
    uint64_t source_hash;
    int loaded_from_cache = 0;
    const int trace_timing =
        vsh_cached_getenv("MERCENARIES_TRACE_SHADER_TIMING") != NULL;
    const ULONGLONG timing_begin = GetTickCount64();
    ULONGLONG timing_generated = timing_begin;
    ULONGLONG timing_loaded = timing_begin;
    ULONGLONG timing_created;

    /* Bounded, opt-in structural capture for extending startup preparation.
     * No host bytecode, constants, or mutable guest pointers are persisted. */
    if (vsh_cached_getenv("MERCENARIES_TRACE_VSH_WARMUP") != NULL) {
        static unsigned captures;
        if (captures++ < 128u) {
            fprintf(stderr, "[VSH-WARMUP] {\"hash\":\"%08X\",\"microcode\":[", hash);
            for (int i=0;i<num_insns*4;++i) fprintf(stderr,"%s%lu",i?",":"",(unsigned long)microcode[i]);
            fprintf(stderr,"],\"formats\":[");
            for (unsigned i=0;i<16;++i) fprintf(stderr,"%s%u",i?",":"",g_vsh_input_formats[i]);
            fprintf(stderr,"],\"offsets\":[");
            for (unsigned i=0;i<16;++i) fprintf(stderr,"%s%u",i?",":"",g_vsh_input_offsets[i]);
            fprintf(stderr,"],\"components\":[");
            for (unsigned i=0;i<16;++i) fprintf(stderr,"%s%u",i?",":"",g_vsh_input_components[i]);
            fprintf(stderr,"],\"bgra_mask\":%u}\n",g_vsh_input_bgra_mask);
        }
    }

    /* Parse microcode */
    d3d8_vsh_parse(microcode, num_insns, &program);

    /* Generate HLSL */
    hlsl_len = d3d8_vsh_generate_hlsl(&program, hlsl_buf, sizeof(hlsl_buf));
    if (hlsl_len <= 0) {
        fprintf(stderr, "D3D8 VSH: HLSL generation failed\n");
        return NULL;
    }
    if (trace_timing)
        timing_generated = GetTickCount64();
    {
        const char *trace_hash =
            vsh_cached_getenv("MERCENARIES_TRACE_VSH_HLSL_HASH");
        const char *trace_raw_hash =
            vsh_cached_getenv("MERCENARIES_TRACE_VSH_HLSL_RAW_HASH");
        const uint32_t raw_hash =
            fnv1a_hash(microcode, (size_t)num_insns * 4 * sizeof(DWORD));
        if (vsh_cached_getenv("MERCENARIES_TRACE_VSH_HLSL") != NULL ||
            (trace_hash != NULL &&
             (uint32_t)strtoul(trace_hash, NULL, 16) == hash) ||
            (trace_raw_hash != NULL &&
             (uint32_t)strtoul(trace_raw_hash, NULL, 16) == raw_hash))
            fprintf(stderr,
                    "--- Generated HLSL hash=%08X raw=%08X ---\n%s\n--- End ---\n",
                    hash, raw_hash, hlsl_buf);
    }

    /* Restore bytecode generated by this exact translator output when
     * available. Constants remain dynamic and are not part of this cache. */
    source_hash = d3d8_shader_source_hash(hlsl_buf, (size_t)hlsl_len,
                                          "vs_5_0");
    hr = d3d8_shader_cache_load("vs", source_hash, &code);
    if (trace_timing)
        timing_loaded = GetTickCount64();
    if (hr == S_OK) {
        loaded_from_cache = 1;
        fprintf(stderr,
                "D3D8 VSH: Disk cache hit (hash 0x%08X, %d insns)\n",
                hash, num_insns);
    } else {
        fprintf(stderr,
                "D3D8 VSH: Compiling shader (hash 0x%08X, raw 0x%08X, %d insns, %d HLSL bytes)\n",
                hash,
                fnv1a_hash(microcode,
                           (size_t)num_insns * 4 * sizeof(DWORD)),
                num_insns, hlsl_len);
        hr = d3d8_compile_shader(hlsl_buf, (SIZE_T)hlsl_len, "nv2a_vsh",
                        NULL, NULL, "main", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &code, &errors);
    }
    if (FAILED(hr)) {
        xbox_preview_log_event("shader-error", "vertex compile HRESULT=%08lX %.500s",hr, errors ? (char*)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        fprintf(stderr, "D3D8 VSH: Compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End ---\n", hlsl_buf);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    if (trace_timing) timing_loaded = GetTickCount64();
    if (!loaded_from_cache)
        d3d8_shader_cache_store("vs", source_hash, code);

    /* Insert into cache */
    entry = cache_insert(hash);
    if (!entry) {
        ID3D10Blob_Release(code);
        return NULL;
    }

    /* Create D3D11 vertex shader */
    hr = ID3D11Device_CreateVertexShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &entry->vs);
    timing_created = GetTickCount64();

    if (FAILED(hr)) {
        xbox_preview_log_event("shader-error", "CreateVertexShader HRESULT=%08lX",hr);
        fprintf(stderr, "D3D8 VSH: CreateVertexShader failed: 0x%08lX\n", hr);
        ID3D10Blob_Release(code);
        entry->in_use = 0;
        if (loaded_from_cache)
            d3d8_shader_cache_invalidate("vs", source_hash);
        return NULL;
    }

    if (timing_created - timing_begin >= 8u)
        xbox_preview_log_sample("shader-build", "kind=vs hash=%08X cache=%d elapsed_ms=%llu",
            hash, loaded_from_cache, (unsigned long long)(timing_created - timing_begin));

    entry->vs_blob     = code;
    entry->inputs_read = program.inputs_read;
    entry->layout_count = 0;

    fprintf(stderr, "D3D8 VSH: Compiled shader (hash 0x%08X, %d insns, inputs 0x%04X)\n",
            hash, program.length, program.inputs_read);
    if (trace_timing) {
        fprintf(stderr,
                "[SHADER-TIMING] kind=vs hash=%08X cache=%d generate_ms=%llu "
                "load_or_compile_ms=%llu create_ms=%llu total_ms=%llu\n",
                hash, loaded_from_cache,
                (unsigned long long)(timing_generated - timing_begin),
                (unsigned long long)(timing_loaded - timing_generated),
                (unsigned long long)(timing_created - timing_loaded),
                (unsigned long long)(timing_created - timing_begin));
        fflush(stderr);
    }

    return entry;
}

/**
 * Get the input layout for a cache entry.
 * Creates and caches the layout on first request per input mask.
 */
static ID3D11InputLayout *get_cached_layout(VshCacheEntry *entry)
{
    uint16_t mask = entry->inputs_read;
    int i;

    /* Check if we already created a layout for this mask */
    for (i = 0; i < entry->layout_count; i++) {
        if (entry->layout_keys[i] == g_vsh_input_layout_key)
            return entry->layouts[i];
    }

    /* Create new layout */
    if (entry->layout_count >= 16)
        return entry->layouts[0]; /* Fallback to first */

    ID3D11InputLayout *layout = create_vsh_input_layout(mask, entry->vs_blob);
    entry->layouts[entry->layout_count]      = layout;
    entry->layout_keys[entry->layout_count] = g_vsh_input_layout_key;
    entry->layout_count++;

    return layout;
}

/* ================================================================
 * Public API Implementation
 * ================================================================ */

HRESULT d3d8_vsh_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    memset(g_vsh_input_formats, 0, sizeof(g_vsh_input_formats));
    memset(g_vsh_input_offsets, 0, sizeof(g_vsh_input_offsets));
    memset(g_vsh_input_components, 0, sizeof(g_vsh_input_components));
    memset(g_vsh_inline_values, 0, sizeof(g_vsh_inline_values));
    memset(&g_vsh_host_constants, 0, sizeof(g_vsh_host_constants));
    g_vsh_input_bgra_mask = 0;
    g_vsh_input_layout_key = 0;
    g_vsh_slot_count = 0;
    g_vsh_constants_dirty = TRUE;
    g_vsh_host_constants_valid = FALSE;

    /* Create the constant buffer for VS constants (192 * float4 = 3072 bytes) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth      = sizeof(NV2AVSConstants);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vsh_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Failed to create constant buffer: 0x%08lX\n", hr);
        return hr;
    }

    cbd.ByteWidth = sizeof(NV2AVSHostConstants);
    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL,
                                   &g_vsh_host_cb);
    if (FAILED(hr)) {
        fprintf(stderr,
                "D3D8 VSH: Failed to create host constant buffer: 0x%08lX\n",
                hr);
        ID3D11Buffer_Release(g_vsh_cb);
        g_vsh_cb = NULL;
        return hr;
    }

    fprintf(stderr, "D3D8 VSH: Vertex shader translator initialized\n");
    if (g_vsh_warmup_callback) g_vsh_warmup_callback();
    return S_OK;
}

void d3d8_vsh_shutdown(void)
{
    int i, j;

    /* Release all cached shaders and layouts */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        VshCacheEntry *e = &g_vsh_cache[i];
        if (!e->in_use) continue;
        if (e->vs)      ID3D11VertexShader_Release(e->vs);
        if (e->vs_blob) ID3D10Blob_Release(e->vs_blob);
        for (j = 0; j < e->layout_count; j++) {
            if (e->layouts[j])
                ID3D11InputLayout_Release(e->layouts[j]);
        }
    }
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));

    if (g_vsh_cb) {
        ID3D11Buffer_Release(g_vsh_cb);
        g_vsh_cb = NULL;
    }
    if (g_vsh_host_cb) {
        ID3D11Buffer_Release(g_vsh_host_cb);
        g_vsh_host_cb = NULL;
    }
    g_vsh_host_constants_valid = FALSE;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    g_vsh_slot_count = 0;
}

HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle)
{
    int slot;

    if (!microcode || num_insns <= 0 || !out_handle)
        return E_INVALIDARG;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    /* Find a free slot */
    slot = -1;
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        if (!g_vsh_slots[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "D3D8 VSH: No free shader slots\n");
        return E_OUTOFMEMORY;
    }

    /* Store microcode (deferred compilation) */
    memcpy(g_vsh_slots[slot].microcode, microcode,
           (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].microcode_hash =
        fnv1a_hash(microcode, (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].length = num_insns;
    g_vsh_slots[slot].in_use = 1;
    g_vsh_slot_count++;

    /* Generate handle: slot index + 0x10000 to distinguish from FVF codes.
     * Xbox D3D8 uses handles with the high bit set (> 0xFFFF). */
    *out_handle = (DWORD)(slot + 0x10000);

    if (vsh_cached_getenv("MERCENARIES_TRACE_VSH_HANDLES") != NULL) {
        fprintf(stderr,
                "D3D8 VSH: Created shader handle 0x%lX (%d instructions)\n",
                *out_handle, num_insns);
    }

    return S_OK;
}

HRESULT d3d8_vsh_delete_shader(DWORD handle)
{
    int slot;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;

    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return E_INVALIDARG;

    if (g_vsh_slots[slot].in_use) {
        g_vsh_slots[slot].in_use = 0;
        g_vsh_slot_count--;
    }

    return S_OK;
}

void d3d8_vsh_set_constant(int start_reg, const float *data, int count)
{
    int end_reg;
    size_t byte_count;

    if (!data || start_reg < 0)
        return;

    end_reg = start_reg + count;
    if (end_reg > NV2A_VS_MAX_CONSTANTS)
        end_reg = NV2A_VS_MAX_CONSTANTS;
    if (end_reg <= start_reg)
        return;

    byte_count = (size_t)(end_reg - start_reg) * 4u * sizeof(float);
    /* Guest constants are copied as exact IEEE-754 bits. Avoid a dynamic
     * D3D11 map only when the complete requested range is byte-identical;
     * NaNs and signed zero therefore retain their hardware-visible payload. */
    if (memcmp(&g_vsh_constants.c[start_reg][0], data, byte_count) != 0) {
        memcpy(&g_vsh_constants.c[start_reg][0], data, byte_count);
        g_vsh_constants_dirty = TRUE;
    }
}

void d3d8_vsh_set_input_layout(
    const DXGI_FORMAT formats[NV2A_VS_MAX_INPUTS],
    const UINT offsets[NV2A_VS_MAX_INPUTS],
    const UINT components[NV2A_VS_MAX_INPUTS],
    uint32_t bgra_mask)
{
    uint32_t hash = 2166136261u;
    int i;

    memcpy(g_vsh_input_formats, formats, sizeof(g_vsh_input_formats));
    memcpy(g_vsh_input_offsets, offsets, sizeof(g_vsh_input_offsets));
    memcpy(g_vsh_input_components, components,
           sizeof(g_vsh_input_components));
    g_vsh_input_bgra_mask = bgra_mask;
    for (i = 0; i < NV2A_VS_MAX_INPUTS; ++i) {
        hash ^= (uint32_t)formats[i];
        hash *= 16777619u;
        hash ^= offsets[i];
        hash *= 16777619u;
        hash ^= components[i];
        hash *= 16777619u;
    }
    hash ^= bgra_mask;
    hash *= 16777619u;
    g_vsh_input_layout_key = hash;
}

BOOL d3d8_vsh_prewarm(const NV2AVshWarmup *program)
{
    DXGI_FORMAT formats[NV2A_VS_MAX_INPUTS];
    UINT offsets[NV2A_VS_MAX_INPUTS], components[NV2A_VS_MAX_INPUTS];
    uint32_t bgra, layout_key, hash;
    VshCacheEntry *entry;
    BOOL ready;
    if (!program || program->length < 1 || program->length > NV2A_VS_MAX_INSTRUCTIONS)
        return FALSE;
    memcpy(formats, g_vsh_input_formats, sizeof(formats));
    memcpy(offsets, g_vsh_input_offsets, sizeof(offsets));
    memcpy(components, g_vsh_input_components, sizeof(components));
    bgra = g_vsh_input_bgra_mask;
    layout_key = g_vsh_input_layout_key;
    d3d8_vsh_set_input_layout(program->formats, program->offsets,
                             program->components, program->bgra_mask);
    hash = fnv1a_hash(program->microcode, (size_t)program->length * 4 * sizeof(DWORD));
    hash = (hash ^ g_vsh_input_layout_key) * 16777619u;
    entry = cache_lookup(hash);
    if (!entry) entry = compile_shader(program->microcode, program->length, hash);
    ready = entry && get_cached_layout(entry) != NULL;
    /* Prewarming must not alter guest handles, inline values, constants, or
     * active input declarations, including when compilation/layout fails. */
    memcpy(g_vsh_input_formats, formats, sizeof(formats));
    memcpy(g_vsh_input_offsets, offsets, sizeof(offsets));
    memcpy(g_vsh_input_components, components, sizeof(components));
    g_vsh_input_bgra_mask = bgra;
    g_vsh_input_layout_key = layout_key;
    return ready;
}

void d3d8_vsh_set_inline_values(
    const float values[NV2A_VS_MAX_INPUTS][4])
{
    if (memcmp(g_vsh_inline_values, values,
               sizeof(g_vsh_inline_values)) != 0) {
        memcpy(g_vsh_inline_values, values, sizeof(g_vsh_inline_values));
        g_vsh_host_constants_valid = FALSE;
    }
}

BOOL d3d8_vsh_is_programmable(DWORD handle)
{
    return (handle >= 0x10000) ? TRUE : FALSE;
}

BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    ID3D11DeviceContext *ctx;
    int slot;
    NV2AVshSlot *vsh;
    uint32_t hash;
    VshCacheEntry *entry;
    ID3D11InputLayout *layout;
    D3D11_MAPPED_SUBRESOURCE mapped;
    NV2AVSHostConstants host_constants;
    HRESULT hr;

    if (!d3d8_vsh_is_programmable(handle))
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx) return FALSE;

    /* Resolve handle to shader slot */
    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return FALSE;

    vsh = &g_vsh_slots[slot];
    if (!vsh->in_use)
        return FALSE;

    /* Handle bytecode is immutable; reuse the key computed at creation. */
    hash = vsh->microcode_hash;
    /* Input component types are part of the compiled HLSL signature. The
     * same NV2A program may be used with different vertex declarations. */
    hash ^= g_vsh_input_layout_key;
    hash *= 16777619u;

    /* Look up in cache */
    entry = cache_lookup(hash);
    if (!entry) {
        /* Cache miss: parse, generate HLSL, compile */
        entry = compile_shader(vsh->microcode, vsh->length, hash);
        if (!entry)
            return FALSE;
    }

    /* Bind the vertex shader */
    ID3D11DeviceContext_VSSetShader(ctx, entry->vs, NULL, 0);

    /* Bind the input layout */
    layout = get_cached_layout(entry);
    if (layout)
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);

    /* Update constant buffer if dirty */
    if (g_vsh_constants_dirty) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vsh_cb,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &g_vsh_constants, sizeof(g_vsh_constants));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vsh_cb, 0);
        }
        g_vsh_constants_dirty = FALSE;
    }

    /* Bind guest constants to b1. Xbox programmable shaders leave oPos in
     * screen space, so b2 supplies the host conversion parameters used by
     * the Xemu-matched HLSL epilogue. */
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &g_vsh_cb);
    host_constants.surface_width = (float)d3d8_GetRenderTargetWidth();
    host_constants.surface_height = (float)d3d8_GetRenderTargetHeight();
    host_constants.depth_max = 16777215.0f;
    host_constants.padding = 0.0f;
    memcpy(host_constants.inline_values, g_vsh_inline_values,
           sizeof(host_constants.inline_values));
    if (!g_vsh_host_constants_valid ||
        memcmp(&host_constants, &g_vsh_host_constants,
               sizeof(host_constants)) != 0) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vsh_host_cb,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &host_constants, sizeof(host_constants));
            ID3D11DeviceContext_Unmap(ctx,
                                      (ID3D11Resource *)g_vsh_host_cb, 0);
            g_vsh_host_constants = host_constants;
            g_vsh_host_constants_valid = TRUE;
        }
    }
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 2, 1, &g_vsh_host_cb);

    return TRUE;
}
