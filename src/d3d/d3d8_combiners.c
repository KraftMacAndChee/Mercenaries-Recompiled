#include "kernel/preview_log.h"
/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders compiled for D3D11. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test and fog
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of compiled ID3D11PixelShader objects. Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. It checks
 *    if state is dirty, rebuilds/looks up the shader, uploads constants,
 *    and binds everything to the D3D11 pipeline.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include "d3d8_shader_cache.h"
#include "d3d8_compiler.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;

/** Dirty flag - set when any PS render state changes. */
static BOOL g_dirty = TRUE;
static BOOL g_external_state = FALSE;
/* The cache owns this shader.  It remains valid until the combiner structure
 * changes; constant colors are deliberately not shader identity. */
static ID3D11PixelShader *g_current_shader = NULL;

/** PS constant buffer and the last value successfully uploaded to it. */
static ID3D11Buffer *g_combiner_cb = NULL;
static NV2APSConstants g_last_constants;
static BOOL g_last_constants_valid = FALSE;
static UINT64 g_constant_prepare_count;
static UINT64 g_constant_upload_count;
static UINT64 g_constant_map_failure_count;
static ULONGLONG g_constant_trace_start_ms;
static int g_constant_trace_enabled = -1;
static float g_texture_scale[NV2A_MAX_TEXTURES][4] = {
    { 1.0f, 1.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f }
};
static int g_polygon_offset_enabled;
static float g_polygon_offset[4];
static float g_flare_grid_ratio[4];
static UINT g_color_key_mode_bits;
static UINT g_color_key[NV2A_MAX_TEXTURES];
static UINT g_color_key_mask[NV2A_MAX_TEXTURES] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    ID3D11PixelShader  *shader;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;
static void (*g_warmup_callback)(void);
void d3d8_combiners_set_warmup(void (*callback)(void))
{
    g_warmup_callback = callback;
}


/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash_update(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    const size_t constant_begin = offsetof(NV2ACombinerState, c0);
    const size_t structure_tail = offsetof(NV2ACombinerState, tex_mode);
    uint32_t hash = 0x811C9DC5u;

    /* C0/C1 are draw-time constants uploaded through NV2APSConstants.  They
     * affect pixel values, but never the generated HLSL structure.  Including
     * animated constants in the cache key caused a fresh D3DCompile for nearly
     * every lit object/frame, eventually thrashing the 128-entry cache.  Hash
     * the two structural spans directly instead of copying and clearing the
     * large state object on every draw. */
    hash = fnv1a_hash_update(hash, state, constant_begin);
    return fnv1a_hash_update(
        hash, (const uint8_t *)state + structure_tail,
        sizeof(*state) - structure_tail);
}

uint32_t d3d8_combiners_debug_state_hash(void)
{
    return combiner_state_hash(&g_combiner_state);
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    const size_t constant_begin = offsetof(NV2ACombinerState, c0);
    const size_t structure_tail = offsetof(NV2ACombinerState, tex_mode);

    return memcmp(a, b, constant_begin) == 0 &&
           memcmp((const uint8_t *)a + structure_tail,
                  (const uint8_t *)b + structure_tail,
                  sizeof(*a) - structure_tail) == 0;
}

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is packed as 8 bits in the render state DWORDs:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel replicate
 *   [7:5] input mapping mode (NV2AInputMapping value)
 * ================================================================ */

static void parse_combiner_input(DWORD packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/**
 * Parse a 32-bit input register DWORD containing 4 packed inputs.
 * Layout: [31:24]=A [23:16]=B [15:8]=C [7:0]=D
 */
static void parse_four_inputs(DWORD dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[3]); /* D */
}

/**
 * Parse a 32-bit output configuration DWORD for one channel.
 *
 * Output DWORD layout:
 *   [3:0]   AB destination register
 *   [7:4]   CD destination register
 *   [11:8]  SUM destination register
 *   [12]    CD dot product flag
 *   [13]    AB dot product flag
 *   [14]    mux_sum flag (mux instead of sum)
 *   [17:15] output mapping (scale/bias)
 *   Bits 18-31 are reserved/unused.
 */
static void parse_output(DWORD dword, NV2ACombinerOutput *output)
{
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
    output->cd_blue_to_alpha = (dword >> 18) & 1;
    output->ab_blue_to_alpha = (dword >> 19) & 1;
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    memset(state, 0, sizeof(*state));

    /* Bits [3:0]: number of active combiner stages (1-8) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;

    /* Bits [8:23]: texture mode per stage (4 bits each) */
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);

    /* Bits [24:31]: dot mapping and other flags */
    state->flags = (token >> 24) & 0xFF;

    /* Parse per-stage inputs and outputs from render states */
    d3d8_combiners_from_render_states(rs, state);

    /* Preserve the token-derived fields (from_render_states may overwrite) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);
    state->flags = (token >> 24) & 0xFF;
}

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    int i;
    DWORD combiner_count = rs[D3DRS_PSCOMBINERCOUNT];
    DWORD dot_mapping = rs[D3DRS_PSDOTMAPPING];

    /* Stage zero is always ZERO_TO_ONE. Stages one through three select
     * the NV2A dot-product input conversion from successive nibbles. */
    state->dot_map[0] = 0;
    state->dot_map[1] = (dot_mapping >> 0) & 0xf;
    state->dot_map[2] = (dot_mapping >> 4) & 0xf;
    state->dot_map[3] = (dot_mapping >> 8) & 0xf;

    /*
     * If called standalone (not from parse_token), read combiner count
     * from D3DRS_PSCOMBINERCOUNT render state.
     *
     * D3DRS_PSCOMBINERCOUNT layout:
     *   [7:0]   number of stages
     *   [8]     mux uses R0.a MSB (otherwise LSB)
     *   [12]    unique C0 per stage (otherwise shared C0[0])
     *   [16]    unique C1 per stage (otherwise shared C1[0])
     */
    if (state->num_stages == 0) {
        state->num_stages = combiner_count & 0xF;
        if (state->num_stages < 1) state->num_stages = 1;
        if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
            state->num_stages = NV2A_MAX_COMBINER_STAGES;
    }
    state->flags = combiner_count >> 8;

    /* Parse stage inputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_four_inputs(rs[D3DRS_PSRGBINPUTS0 + i],
                          state->stages[i].rgb_input);
        parse_four_inputs(rs[D3DRS_PSALPHAINPUTS0 + i],
                          state->stages[i].alpha_input);
    }

    /* Parse stage outputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_output(rs[D3DRS_PSRGBOUTPUTS0 + i],
                     &state->stages[i].rgb_output);
        parse_output(rs[D3DRS_PSALPHAOUTPUTS0 + i],
                     &state->stages[i].alpha_output);
    }

    /*
     * Parse final combiner inputs.
     *
     * D3DRS_PSFINALCOMBINERINPUTSABCD packs inputs A,B,C,D as 8 bits each:
     *   [7:0]=A  [15:8]=B  [23:16]=C  [31:24]=D
     *
     * D3DRS_PSFINALCOMBINERINPUTSEFG packs E,F,G:
     *   [7:0]=E  [15:8]=F  [23:16]=G  [31:24]=reserved
     */
    {
        DWORD abcd = rs[D3DRS_PSFINALCOMBINERINPUTSABCD];
        DWORD efg  = rs[D3DRS_PSFINALCOMBINERINPUTSEFG];

        NV2ACombinerInput parsed_efg[4];
        parse_four_inputs(abcd, state->final_input);
        parse_four_inputs(efg, parsed_efg);
        state->final_input[4] = parsed_efg[0];
        state->final_input[5] = parsed_efg[1];
        state->final_input[6] = parsed_efg[2];
        state->final_clamp_sum = (efg & 0x80u) != 0;
        state->final_inv_v1 = (efg & 0x40u) != 0;
        state->final_inv_r0 = (efg & 0x20u) != 0;
    }

    /* Per-stage constant colors */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }

    /* Final combiner uses the constants from the last active stage, or
     * can use its own - for now, store them separately. Games typically
     * share them with the last stage. */
    state->final_c0 = state->c0[state->num_stages > 0 ? state->num_stages - 1 : 0];
    state->final_c1 = state->c1[state->num_stages > 0 ? state->num_stages - 1 : 0];

    /* Read texture modes from render state if not already set by token */
    if (state->tex_mode[0] == 0 && state->tex_mode[1] == 0 &&
        state->tex_mode[2] == 0 && state->tex_mode[3] == 0) {
        DWORD tm = rs[D3DRS_PSTEXTUREMODES];
        state->tex_mode[0] = (NV2ATextureMode)((tm >>  0) & 0x1F);
        state->tex_mode[1] = (NV2ATextureMode)((tm >>  5) & 0x1F);
        state->tex_mode[2] = (NV2ATextureMode)((tm >> 10) & 0x1F);
        state->tex_mode[3] = (NV2ATextureMode)((tm >> 15) & 0x1F);
        state->input_tex[0] = -1;
        state->input_tex[1] = 0;
        state->input_tex[2] = (rs[D3DRS_PSINPUTTEXTURE] >> 16) & 0xF;
        state->input_tex[3] = (rs[D3DRS_PSINPUTTEXTURE] >> 20) & 0xF;
    }
}

void d3d8_combiners_from_nv2a_registers(
    DWORD control, const DWORD rgb_inputs[8], const DWORD alpha_inputs[8],
    const DWORD rgb_outputs[8], const DWORD alpha_outputs[8],
    DWORD final_inputs_0, DWORD final_inputs_1,
    const DWORD factor0[8], const DWORD factor1[8],
    DWORD final_factor0, DWORD final_factor1, DWORD shader_stage_program,
    DWORD dot_mapping, DWORD other_stage_input, NV2ACombinerState *state)
{
    NV2ACombinerInput efg[4];
    int i;

    memset(state, 0, sizeof(*state));
    state->num_stages = control & 0xff;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    state->flags = control >> 8;
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; ++i) {
        parse_four_inputs(rgb_inputs[i], state->stages[i].rgb_input);
        parse_four_inputs(alpha_inputs[i], state->stages[i].alpha_input);
        parse_output(rgb_outputs[i], &state->stages[i].rgb_output);
        parse_output(alpha_outputs[i], &state->stages[i].alpha_output);
        state->c0[i] = factor0[i];
        state->c1[i] = factor1[i];
    }
    parse_four_inputs(final_inputs_0, state->final_input);
    parse_four_inputs(final_inputs_1, efg);
    state->final_input[4] = efg[0];
    state->final_input[5] = efg[1];
    state->final_input[6] = efg[2];
    state->final_clamp_sum = (final_inputs_1 & 0x80u) != 0;
    state->final_inv_v1 = (final_inputs_1 & 0x40u) != 0;
    state->final_inv_r0 = (final_inputs_1 & 0x20u) != 0;
    state->final_c0 = final_factor0;
    state->final_c1 = final_factor1;
    for (i = 0; i < NV2A_MAX_TEXTURES; ++i)
        state->tex_mode[i] = (NV2ATextureMode)
            ((shader_stage_program >> (i * 5)) & 0x1f);
    state->input_tex[0] = -1;
    state->input_tex[1] = 0;
    state->input_tex[2] = (other_stage_input >> 16) & 0xf;
    state->input_tex[3] = (other_stage_input >> 20) & 0xf;
    state->dot_map[0] = 0;
    state->dot_map[1] = (dot_mapping >> 0) & 0xf;
    state->dot_map[2] = (dot_mapping >> 4) & 0xf;
    state->dot_map[3] = (dot_mapping >> 8) & 0xf;
}

void d3d8_combiners_set_nv2a_state(const NV2ACombinerState *state)
{
    if (state) {
        NV2ACombinerState effective = *state;
        effective.polygon_offset = g_polygon_offset_enabled;
        state = &effective;
        if (!g_external_state ||
            !combiner_state_equal(&g_combiner_state, state))
            g_current_shader = NULL;
        memcpy(&g_combiner_state, state, sizeof(g_combiner_state));
        g_external_state = TRUE;
    } else {
        if (g_external_state)
            g_current_shader = NULL;
        g_external_state = FALSE;
    }
}

BOOL d3d8_combiners_requires_triangle_depth(void)
{
    return g_external_state && g_current_shader && g_combiner_state.guest_depth == 2;
}

void d3d8_combiners_set_flare_grid_ratio(float x, float y)
{
    g_flare_grid_ratio[0] = x;
    g_flare_grid_ratio[1] = y;
}

void d3d8_combiners_set_polygon_offset(BOOL enabled, float bias,
                                        float slope, float scale_x, float scale_y)
{
    g_polygon_offset_enabled = enabled != FALSE;
    g_polygon_offset[0] = enabled ? bias : 0.0f;
    g_polygon_offset[1] = enabled ? slope : 0.0f;
    g_polygon_offset[2] = scale_x;
    g_polygon_offset[3] = scale_y;
}

void d3d8_combiners_set_texture_scale(UINT stage, float u_scale,
                                      float v_scale)
{
    if (stage >= NV2A_MAX_TEXTURES)
        return;
    g_texture_scale[stage][0] = u_scale;
    g_texture_scale[stage][1] = v_scale;
    g_texture_scale[stage][3] = 1.0f;
}

void d3d8_combiners_set_texture_red_blue_swap(UINT stage, BOOL swap)
{
    if (stage >= NV2A_MAX_TEXTURES)
        return;
    /* tex_scale.z is otherwise unused. Keeping this as draw-time data avoids
     * multiplying the shader cache for a surface-storage interpretation. */
    g_texture_scale[stage][2] = swap ? 1.0f : 0.0f;
}

void d3d8_combiners_set_color_key(UINT stage, UINT mode, UINT color,
                                  UINT mask)
{
    if (stage >= NV2A_MAX_TEXTURES)
        return;
    g_color_key_mode_bits &= ~(3u << (stage * 2u));
    g_color_key_mode_bits |= (mode & 3u) << (stage * 2u);
    g_color_key[stage] = color;
    g_color_key_mask[stage] = mask;
}
/* ================================================================
 * HLSL Code Generation
 *
 * Strategy: build the shader string via snprintf into a large buffer.
 * Each section appends to a running offset. This is not the prettiest
 * approach but it's straightforward, debuggable, and has zero
 * external dependencies.
 *
 * Generated shader structure:
 *   1. Texture sampler declarations
 *   2. Constant buffer (matches NV2APSConstants layout)
 *   3. Input struct (SV_POSITION, COLOR0, COLOR1, TEXCOORD0-3)
 *   4. Input mapping helper function
 *   5. Output mapping helper function
 *   6. main():
 *      a. Initialize register file from inputs
 *      b. Execute each general combiner stage
 *      c. Execute final combiner
 *      d. Apply fog
 *      e. Apply alpha test
 *      f. Return result
 * ================================================================ */

/**
 * Emit HLSL to append a string to the output buffer.
 * Returns new offset, or -1 if buffer overflow.
 */
#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/**
 * Get the HLSL variable name for a register in the NV2A register file.
 *
 * The register file is represented as local float4 variables in the
 * generated shader. This returns the name used in the HLSL code.
 */
static const char *reg_name(NV2ACombinerRegister reg)
{
    switch (reg) {
    case NV2A_REG_ZERO:     return "r_zero";
    case NV2A_REG_C0:       return "r_c0";
    case NV2A_REG_C1:       return "r_c1";
    case NV2A_REG_FOG:      return "r_fog";
    case NV2A_REG_V0:       return "r_v0";
    case NV2A_REG_V1:       return "r_v1";
    case NV2A_REG_T0:       return "r_t0";
    case NV2A_REG_T1:       return "r_t1";
    case NV2A_REG_T2:       return "r_t2";
    case NV2A_REG_T3:       return "r_t3";
    case NV2A_REG_R0:       return "r_r0";
    case NV2A_REG_R1:       return "r_r1";
    case NV2A_REG_EF_PROD:  return "r_ef";
    case NV2A_REG_V1R0_SUM: return "r_v1r0sum";
    default:                return "r_zero";
    }
}

/**
 * Emit HLSL expression for reading a combiner input.
 *
 * An input consists of:
 *   1. Register selection (which variable to read)
 *   2. Channel selection (full RGBA or alpha-replicated)
 *   3. Mapping function (how to transform the value)
 *
 * For alpha-replicate: .aaaa swizzle
 * For normal RGB read in RGB path: .rgb (or .rgba for alpha path)
 *
 * The mapping function is applied inline as an arithmetic expression.
 *
 * @param suffix  ".rgb" for RGB path, ".a" for alpha path (determines swizzle)
 */
static const char *stage_input_reg_name(NV2ACombinerRegister reg)
{
    switch (reg) {
    case NV2A_REG_V0: return "stage_v0";
    case NV2A_REG_V1: return "stage_v1";
    case NV2A_REG_T0: return "stage_t0";
    case NV2A_REG_T1: return "stage_t1";
    case NV2A_REG_T2: return "stage_t2";
    case NV2A_REG_T3: return "stage_t3";
    case NV2A_REG_R0: return "stage_r0";
    case NV2A_REG_R1: return "stage_r1";
    default:          return reg_name(reg);
    }
}

static void emit_mapped_input(char *buf, int bufsize, int *off,
                               const NV2ACombinerInput *input,
                               const char *suffix, int stage_idx)
{
    const char *rn;
    char swizzle[8];
    char base_expr[128];
    int n;

    rn = stage_idx >= 0 ? stage_input_reg_name(input->reg) :
                          reg_name(input->reg);

    /* r_c0/r_c1 are rebound at each general stage according to the
     * shared/unique flags. The final combiner always has dedicated C0/C1. */
    if (input->reg == NV2A_REG_C0) {
        snprintf(base_expr, sizeof(base_expr), stage_idx < 0 ? "fc0" : "r_c0");
    } else if (input->reg == NV2A_REG_C1) {
        snprintf(base_expr, sizeof(base_expr), stage_idx < 0 ? "fc1" : "r_c1");
    } else {
        snprintf(base_expr, sizeof(base_expr), "%s", rn);
    }

    /* Determine swizzle based on alpha replicate and target channel */
    if (strcmp(suffix, ".a") == 0) {
        /* The channel bit selects BLUE/ALPHA for the scalar alpha path. */
        snprintf(swizzle, sizeof(swizzle),
                 input->alpha_rep ? ".a" : ".b");
    } else if (input->alpha_rep) {
        /* The same bit selects RGB/replicated-alpha for the RGB path. */
        snprintf(swizzle, sizeof(swizzle), ".aaa");
    } else {
        snprintf(swizzle, sizeof(swizzle), "%s", suffix);
    }

    /* Build the full variable reference */
    char var_ref[160];
    snprintf(var_ref, sizeof(var_ref), "%s%s", base_expr, swizzle);

    /* Apply input mapping */
    switch (input->mapping) {
    case NV2A_MAP_UNSIGNED_IDENTITY:
        /* x - passthrough */
        n = snprintf(buf + *off, bufsize - *off, "max(%s, 0.0)", var_ref);
        break;
    case NV2A_MAP_UNSIGNED_INVERT:
        /* 1 - x, clamped to [0,1] */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - clamp(%s, 0.0, 1.0))", var_ref);
        break;
    case NV2A_MAP_EXPAND_NORMAL:
        /* 2x - 1 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(2.0 * max(%s, 0.0) - 1.0)", var_ref);
        break;
    case NV2A_MAP_EXPAND_NEGATE:
        /* 1 - 2x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - 2.0 * max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NORMAL:
        /* x - 0.5 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(max(%s, 0.0) - 0.5)", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NEGATE:
        /* 0.5 - x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(0.5 - max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_SIGNED_IDENTITY:
        /* x (allow negative values) */
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    case NV2A_MAP_SIGNED_NEGATE:
        /* -x */
        n = snprintf(buf + *off, bufsize - *off, "(-%s)", var_ref);
        break;
    default:
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    }
    if (n > 0) *off += n;
}

/**
 * Emit HLSL expression for the output mapping (scale/bias).
 */
static const char *output_map_prefix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return "(";
    case NV2A_OUT_SHIFTLEFT_1:      return "(";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return "((";
    case NV2A_OUT_SHIFTLEFT_2:      return "(";
    case NV2A_OUT_SHIFTRIGHT_1:     return "(";
    default:                        return "";
    }
}

static const char *output_map_suffix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return " - 0.5)";
    case NV2A_OUT_SHIFTLEFT_1:      return " * 2.0)";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return " - 0.5) * 2.0)";
    case NV2A_OUT_SHIFTLEFT_2:      return " * 4.0)";
    case NV2A_OUT_SHIFTRIGHT_1:     return " * 0.5)";
    default:                        return "";
    }
}

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    int off = 0;
    int i;

    /* ---- Texture samplers ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE &&
            state->tex_mode[i] != NV2A_TEXMODE_PASSTHRU) {
            if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
                EMIT("TextureCube  tex%d : register(t%d);\n", i, i);
            } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
                EMIT("Texture3D    tex%d : register(t%d);\n", i, i);
            } else {
                EMIT("Texture2D    tex%d : register(t%d);\n", i, i);
            }
            EMIT("SamplerState samp%d : register(s%d);\n", i, i);
        }
    }
    EMIT("\n");

    /* ---- Constant buffer ---- */
    EMIT("cbuffer CombinerCB : register(b0) {\n");
    EMIT("    float4 c0[8];\n");    /* Per-stage C0 */
    EMIT("    float4 c1[8];\n");    /* Per-stage C1 */
    EMIT("    float4 fc0;\n");      /* Final combiner C0 */
    EMIT("    float4 fc1;\n");      /* Final combiner C1 */
    EMIT("    float4 fog_color;\n");
    EMIT("    float4 tex_scale[4];\n");
    EMIT("    float  alpha_ref;\n");
    EMIT("    uint   alpha_func;\n");
    EMIT("    uint   alpha_test_enable;\n");
    EMIT("    uint   fog_enable;\n");
    EMIT("    uint   alpha_kill_mask;\n");
    EMIT("    uint3  _alpha_kill_pad;\n");
    EMIT("    uint   color_key_mode_bits;\n");
    EMIT("    uint3  _color_key_pad;\n");
    EMIT("    uint4  color_key;\n");
    EMIT("    uint4  color_key_mask;\n");
    EMIT("    float4 flare_grid_ratio;\n");
    EMIT("    float4 polygon_offset;\n");
    EMIT("};\n\n");

    /* ---- Input structure ---- */
    EMIT("struct PS_IN {\n");
    EMIT("    float4 pos     : SV_POSITION;\n");
    EMIT("    float4 color0  : COLOR0;\n");
    EMIT("    float4 color1  : COLOR1;\n");
    EMIT("    float4 tc0     : TEXCOORD0;\n");
    EMIT("    float4 tc1     : TEXCOORD1;\n");
    EMIT("    float4 tc2     : TEXCOORD2;\n");
    EMIT("    float4 tc3     : TEXCOORD3;\n");
    EMIT("    float  fog     : FOG;\n");
    if (state->guest_depth) {
        EMIT("    float size : PSIZE;\n");
        EMIT("    float4 back0 : TEXCOORD4;\n");
        EMIT("    float4 back1 : TEXCOORD5;\n");
    }
    if (state->guest_depth == 2) {
        EMIT("    nointerpolation float4 p0 : TEXCOORD6;\n");
        EMIT("    nointerpolation float4 p1 : TEXCOORD7;\n");
        EMIT("    nointerpolation float4 p2 : TEXCOORD8;\n");
    } else if (state->guest_depth) EMIT("    float4 guestDepth : TEXCOORD6;\n");
    EMIT("};\n\n");

    /* ---- Main function ---- */
    if (state->polygon_offset || state->guest_depth) {
        EMIT("float4 main(PS_IN input, out float depth : SV_Depth) : SV_TARGET {\n");
        if (state->guest_depth == 2) {
            EMIT("    float2 e1 = input.p1.xy - input.p0.xy, e2 = input.p2.xy - input.p0.xy;\n");
            EMIT("    float2 d = input.pos.xy - input.p0.xy;\n");
            EMIT("    precise float determinant = e1.x * e2.y - e1.y * e2.x;\n");
            EMIT("    float invdet = determinant != 0.0 ? 1.0 / determinant : 0.0;\n");
            EMIT("    precise float b1 = (d.x * e2.y - d.y * e2.x) * invdet;\n");
            EMIT("    precise float b2 = (e1.x * d.y - e1.y * d.x) * invdet;\n");
            EMIT("    precise float triangle_dz = b1 * (input.p1.z - input.p0.z) + b2 * (input.p2.z - input.p0.z);\n");
            EMIT("    float z = 1.0 + (input.p0.z + triangle_dz) / input.p0.w;\n");
        } else {
        EMIT("    float z = %s;\n", state->guest_depth ? "1.0 + input.guestDepth.x / input.guestDepth.y" : "input.pos.z");
        /* Homogeneous transport handles clipping; retain raster depth as a
         * fallback for non-finite or out-of-range guest shader output. */
        if (state->guest_depth)
            EMIT("    if (!isfinite(z) || z < 0.0 || z > 1.0) z = input.pos.z;\n");
        }
        if (state->polygon_offset) {
        /* Convert host-pixel derivatives back to guest-pixel slope. Separate
         * X/Y scales retain the NV2A 2x horizontal AA layout at every internal
         * resolution. Evaluate before texture alpha tests can discard lanes. */
        EMIT("    float2 dz = abs(float2(ddx(z), ddy(z))) * polygon_offset.zw;\n");
        EMIT("    depth = saturate(z + polygon_offset.x + polygon_offset.y * max(dz.x, dz.y));\n");
        } else { EMIT("    depth = saturate(z);\n"); }
    } else {
        EMIT("float4 main(PS_IN input) : SV_TARGET {\n");
    }

    /* Flare visibility reductions use an authored 8 -> 4 -> 2 pixel grid.
     * Retain those sample locations when the backing target is enlarged. */
    if (state->flare_grid & 2) {
        EMIT("    float2 grid_delta = (floor(input.pos.xy * flare_grid_ratio.xy) + 0.5) / flare_grid_ratio.xy - input.pos.xy;\n");
        EMIT("    input.tc0 += ddx(input.tc0) * grid_delta.x + ddy(input.tc0) * grid_delta.y;\n");
    }

    /* Initialize register file */
    EMIT("    /* Register file initialization */\n");
    EMIT("    float4 r_zero = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_c0   = c0[0];\n");
    EMIT("    float4 r_c1   = c1[0];\n");
    /* Xemu exposes the fog register as (fog color RGB, interpolated fog
     * factor A).  When fog is disabled, use a factor of one so final
     * combiner programs can consume FOG uniformly. */
    EMIT("    float4 r_fog  = float4(fog_color.rgb, fog_enable ? saturate(input.fog) : 1.0);\n");

    /* Vertex colors: Xbox D3DCOLOR is BGRA in memory, the vertex shader
     * should have already swizzled to RGBA. */
    EMIT("    float4 r_v0   = input.color0;\n");
    EMIT("    float4 r_v1   = input.color1;\n");

    /* Texture samples */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_mode[i] == NV2A_TEXMODE_NONE) {
            EMIT("    float4 r_t%d = float4(0, 0, 0, 1);\n", i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_PASSTHRU) {
            /* Xemu/NV2A PASSTHRU exposes the interpolated texture-coordinate
             * register directly; it does not sample a texture. Bump shaders
             * use this for tangent-space light and half vectors. */
            EMIT("    float4 r_t%d = input.tc%d;\n", i, i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_DPNDNT_GB) {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, r_t%d.gb);\n",
                 i, i, i, state->input_tex[i]);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_DPNDNT_AR) {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, r_t%d.ar);\n",
                 i, i, i, state->input_tex[i]);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_DOTPRODUCT ||
                   state->tex_mode[i] == NV2A_TEXMODE_DOT_ST) {
            int source = state->input_tex[i];

            /* NV097_SET_DOT_RGBMAPPING converts the sampled input before
             * DOTPRODUCT/DOT_ST. This is distinct from the combiner input
             * mappings and is required by Xbox post-processing filters. */
            switch (state->dot_map[i]) {
            case 1: /* MINUS1_TO_1_D3D */
                EMIT("    float3 dotmap%d = (r_t%d.rgb * 255.0 - 128.0) / 127.0;\n",
                     i, source);
                break;
            case 2: /* MINUS1_TO_1_GL */
                EMIT("    float3 dotmap%d_raw = r_t%d.rgb * 255.0;\n",
                     i, source);
                EMIT("    float3 dotmap%d = lerp((dotmap%d_raw + 0.5) / 127.5, "
                     "(dotmap%d_raw - 255.5) / 127.5, step(128.0, dotmap%d_raw));\n",
                     i, i, i, i);
                break;
            case 3: /* MINUS1_TO_1 */
                EMIT("    float3 dotmap%d_raw = r_t%d.rgb * 255.0;\n",
                     i, source);
                EMIT("    float3 dotmap%d = lerp(dotmap%d_raw / 127.0, "
                     "(dotmap%d_raw - 256.0) / 127.0, step(128.0, dotmap%d_raw));\n",
                     i, i, i, i);
                break;
            case 4: /* HILO_1 */
                EMIT("    float4 dotmap%d_raw = floor(saturate(r_t%d) * 255.0);\n",
                     i, source);
                EMIT("    float3 dotmap%d = float3((dotmap%d_raw.a * 256.0 + "
                     "dotmap%d_raw.r) / 65535.0, (dotmap%d_raw.g * 256.0 + "
                     "dotmap%d_raw.b) / 65535.0, 1.0);\n",
                     i, i, i, i, i);
                break;
            default: /* ZERO_TO_ONE and currently-reserved modes */
                EMIT("    float3 dotmap%d = r_t%d.rgb;\n", i, source);
                break;
            }
            EMIT("    float dot%d = dot(input.tc%d.xyz, dotmap%d);\n",
                 i, i, i);
            if (state->tex_mode[i] == NV2A_TEXMODE_DOTPRODUCT) {
            EMIT("    float4 r_t%d = float4(0, 0, 0, 0);\n", i);
            } else {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, "
                 "float2(dot%d, dot%d) * tex_scale[%d].xy);\n",
                 i, i, i, i - 1, i, i);
            }
        } else if (state->tex_mode[i] == NV2A_TEXMODE_2D) {
            /* Xemu uses textureProj(pT.xyw): NV2A PROJECT2D divides the
             * interpolated S/T coordinates by Q before sampling. */
            if (state->screen_depth_stage == i + 1) {
                /* Mercenaries' sky copies projected oPos.xy into its depth
                 * UVs. Perspective interpolation across a near-plane cut
                 * moves those UVs onto unrelated foreground pixels, leaving
                 * old framebuffer strips visible when the camera rolls.
                 * The guarded path binds a depth image with exactly the
                 * target's dimensions, so sample the corresponding pixel. */
                EMIT("    uint screen_w%d, screen_h%d;\n", i, i);
                EMIT("    tex%d.GetDimensions(screen_w%d, screen_h%d);\n", i, i, i);
                EMIT("    float4 r_t%d = tex%d.SampleLevel(samp%d, input.pos.xy / "
                     "float2(screen_w%d, screen_h%d), 0);\n", i, i, i, i, i);
            } else if (i == 0 && (state->flare_grid & 1)) {
                /* Bilinear filtering on guest texels, whose physical backing
                 * may occupy multiple host pixels. Four guest centers avoid
                 * accidentally reading only one subpixel of a 2x2 mask. */
                EMIT("    float2 guest_p = input.tc0.xy / (abs(input.tc0.w) > 1e-20 ? input.tc0.w : 1.0) - 0.5;\n");
                EMIT("    float2 guest_b = floor(guest_p), guest_f = frac(guest_p);\n");
                EMIT("    float4 grid00 = tex0.SampleLevel(samp0, (guest_b + float2(0.5,0.5)) * tex_scale[0].xy, 0);\n");
                EMIT("    float4 grid10 = tex0.SampleLevel(samp0, (guest_b + float2(1.5,0.5)) * tex_scale[0].xy, 0);\n");
                EMIT("    float4 grid01 = tex0.SampleLevel(samp0, (guest_b + float2(0.5,1.5)) * tex_scale[0].xy, 0);\n");
                EMIT("    float4 grid11 = tex0.SampleLevel(samp0, (guest_b + float2(1.5,1.5)) * tex_scale[0].xy, 0);\n");
                EMIT("    float4 r_t0 = lerp(lerp(grid00, grid10, guest_f.x), lerp(grid01, grid11, guest_f.x), guest_f.y);\n");
            } else {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, (input.tc%d.xy / "
                 "(abs(input.tc%d.w) > 1e-20 ? input.tc%d.w : 1.0)) * "
                 "tex_scale[%d].xy);\n",
                 i, i, i, i, i, i, i);
            }
        } else if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz);\n",
                 i, i, i, i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz / "
                 "(abs(input.tc%d.w) > 1e-20 ? input.tc%d.w : 1.0));\n",
                 i, i, i, i, i, i);
        } else {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xy);\n",
                 i, i, i, i);
        }
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE &&
            state->tex_mode[i] != NV2A_TEXMODE_PASSTHRU &&
            state->tex_mode[i] != NV2A_TEXMODE_DOTPRODUCT) {
            /* Packed Xbox Z24S8 aliases are stored in raw guest byte order.
             * A8R8G8B8 interprets that storage with R/B exchanged relative to
             * the R8G8B8A8 host SRV; A8B8G8R8 (the sky path) does not. */
            EMIT("    if (tex_scale[%d].z > 0.5) r_t%d = r_t%d.bgra;\n",
                 i, i, i);
        }
    }
    for (i = 0; i < NV2A_MAX_TEXTURES; ++i) {
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE &&
            state->tex_mode[i] != NV2A_TEXMODE_PASSTHRU &&
            state->tex_mode[i] != NV2A_TEXMODE_DOTPRODUCT) {
            EMIT("    if ((alpha_kill_mask & (1u << %d)) != 0u && "
                 "r_t%d.a == 0.0) discard;\n", i, i);
            EMIT("    { uint key_mode = (color_key_mode_bits >> %d) & 3u; "
                 "uint4 key_c = (uint4)(saturate(r_t%d) * 255.0 + 0.5); "
                 "uint sampled_key = (key_c.a << 24) | (key_c.r << 16) | "
                 "(key_c.g << 8) | key_c.b; "
                 "if (key_mode != 0u && ((sampled_key & color_key_mask[%d]) "
                 "== (color_key[%d] & color_key_mask[%d]))) { "
                 "if (key_mode == 3u) discard; "
                 "else if (key_mode == 1u) r_t%d.a = 0.0; "
                 "else r_t%d = float4(0, 0, 0, 0); } }\n",
                 i * 2, i, i, i, i, i, i);
        }
    }
    /* Temporary registers: R0 initialized to T0 (NV2A convention),
     * R1 initialized to zero */
    if (getenv("MERCENARIES_DEBUG_T0") != NULL)
        EMIT("    return r_t0;\n");
    if (getenv("MERCENARIES_DEBUG_T0_ALPHA") != NULL)
        EMIT("    return float4(r_t0.aaa, 1.0);\n");
    if (getenv("MERCENARIES_DEBUG_T1") != NULL)
        EMIT("    return r_t1;\n");
    if (getenv("MERCENARIES_DEBUG_T2_ALPHA") != NULL)
        EMIT("    return float4(r_t2.aaa, 1.0);\n");
    if (getenv("MERCENARIES_DEBUG_T2_CORNER") != NULL &&
        state->tex_mode[2] != NV2A_TEXMODE_NONE)
        EMIT("    return float4(tex2.Sample(samp2, float2(1.0, 1.0)).aaa, 1.0);\n");
    EMIT("    float4 r_r0 = float4(0, 0, 0, r_t0.a);\n");
    EMIT("    float4 r_r1 = float4(0, 0, 0, 0);\n\n");

    /* ---- General combiner stages ---- */
    for (i = 0; i < state->num_stages; i++) {
        const NV2ACombinerInput *rgb_in  = state->stages[i].rgb_input;
        const NV2ACombinerInput *alpha_in = state->stages[i].alpha_input;
        const NV2ACombinerOutput *rgb_out = &state->stages[i].rgb_output;
        const NV2ACombinerOutput *alpha_out = &state->stages[i].alpha_output;

        EMIT("    /* ---- Stage %d ---- */\n", i);
        EMIT("    {\n");

        /* Xbox combiners normally share stage-zero constants. They only
         * select the stage-indexed value when the corresponding UNIQUE flag
         * is set (Xemu psh.c get_var()). */
        EMIT("    r_c0 = c0[%d];\n",
             (state->flags & NV2A_COMBINER_FLAG_UNIQUE_C0) ? i : 0);
        EMIT("    r_c1 = c1[%d];\n",
             (state->flags & NV2A_COMBINER_FLAG_UNIQUE_C1) ? i : 0);
        /* General-combiner RGB and alpha halves read the same pre-stage
         * register file. Xemu generates both calculations before committing
         * either set of destinations; snapshot mutable inputs to preserve
         * that NV2A behavior while emitting the paths separately. */
        EMIT("        float4 stage_v0 = r_v0;\n");
        EMIT("        float4 stage_v1 = r_v1;\n");
        EMIT("        float4 stage_t0 = r_t0;\n");
        EMIT("        float4 stage_t1 = r_t1;\n");
        EMIT("        float4 stage_t2 = r_t2;\n");
        EMIT("        float4 stage_t3 = r_t3;\n");
        EMIT("        float4 stage_r0 = r_r0;\n");
        EMIT("        float4 stage_r1 = r_r1;\n");

        /*
         * RGB path: compute AB and CD products
         *
         * AB_rgb = map(A) * map(B)    (component-wise, or dot3 if ab_dot)
         * CD_rgb = map(C) * map(D)    (component-wise, or dot3 if cd_dot)
         */
        EMIT("    {\n");

        /* AB product */
        EMIT("        float3 a_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[0], ".rgb", i);
        EMIT(";\n");
        EMIT("        float3 b_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[1], ".rgb", i);
        EMIT(";\n");

        if (rgb_out->ab_dot) {
            EMIT("        float3 ab_rgb = float3(dot(a_rgb, b_rgb), "
                 "dot(a_rgb, b_rgb), dot(a_rgb, b_rgb));\n");
        } else {
            EMIT("        float3 ab_rgb = a_rgb * b_rgb;\n");
        }

        /* CD product */
        EMIT("        float3 c_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[2], ".rgb", i);
        EMIT(";\n");
        EMIT("        float3 d_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[3], ".rgb", i);
        EMIT(";\n");

        if (rgb_out->cd_dot) {
            EMIT("        float3 cd_rgb = float3(dot(c_rgb, d_rgb), "
                 "dot(c_rgb, d_rgb), dot(c_rgb, d_rgb));\n");
        } else {
            EMIT("        float3 cd_rgb = c_rgb * d_rgb;\n");
        }

        /* Sum or mux */
        if (rgb_out->mux_sum) {
            if (state->flags & 0x1u)
                EMIT("        float3 sum_rgb = (r_r0.a >= 0.5) ? cd_rgb : ab_rgb;\n");
            else
                EMIT("        float3 sum_rgb = (((uint)(r_r0.a * 255.0) & 1u) != 0u) ? cd_rgb : ab_rgb;\n");
        } else {
            EMIT("        float3 sum_rgb = ab_rgb + cd_rgb;\n");
        }

        /* Apply output mapping (scale/bias) */
        const char *omp = output_map_prefix(rgb_out->output_map);
        const char *oms = output_map_suffix(rgb_out->output_map);

        /* Write to destination registers */
        if (rgb_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = clamp(%sab_rgb%s, -1.0, 1.0);\n",
                 reg_name(rgb_out->ab_dst), omp, oms);
            if (rgb_out->ab_blue_to_alpha)
                EMIT("        %s.a = clamp(%sab_rgb.b%s, -1.0, 1.0);\n",
                     reg_name(rgb_out->ab_dst), omp, oms);
        }
        if (rgb_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = clamp(%scd_rgb%s, -1.0, 1.0);\n",
                 reg_name(rgb_out->cd_dst), omp, oms);
            if (rgb_out->cd_blue_to_alpha)
                EMIT("        %s.a = clamp(%scd_rgb.b%s, -1.0, 1.0);\n",
                     reg_name(rgb_out->cd_dst), omp, oms);
        }
        if (rgb_out->sum_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = clamp(%ssum_rgb%s, -1.0, 1.0);\n",
                 reg_name(rgb_out->sum_dst), omp, oms);
        }

        EMIT("    }\n");

        /*
         * Alpha path: same structure but scalar operations.
         * Uses .a swizzle for all reads/writes.
         */
        EMIT("    {\n");

        EMIT("        float a_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[0], ".a", i);
        EMIT(";\n");
        EMIT("        float b_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[1], ".a", i);
        EMIT(";\n");
        EMIT("        float ab_a = a_a * b_a;\n");

        EMIT("        float c_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[2], ".a", i);
        EMIT(";\n");
        EMIT("        float d_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[3], ".a", i);
        EMIT(";\n");
        EMIT("        float cd_a = c_a * d_a;\n");

        if (alpha_out->mux_sum) {
            if (state->flags & 0x1u)
                EMIT("        float sum_a = (r_r0.a >= 0.5) ? cd_a : ab_a;\n");
            else
                EMIT("        float sum_a = (((uint)(r_r0.a * 255.0) & 1u) != 0u) ? cd_a : ab_a;\n");
        } else {
            EMIT("        float sum_a = ab_a + cd_a;\n");
        }

        /* Alpha output mapping */
        omp = output_map_prefix(alpha_out->output_map);
        oms = output_map_suffix(alpha_out->output_map);

        if (alpha_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = clamp(%sab_a%s, -1.0, 1.0);\n",
                 reg_name(alpha_out->ab_dst), omp, oms);
        }
        if (alpha_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = clamp(%scd_a%s, -1.0, 1.0);\n",
                 reg_name(alpha_out->cd_dst), omp, oms);
        }
        if (alpha_out->sum_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = clamp(%ssum_a%s, -1.0, 1.0);\n",
                 reg_name(alpha_out->sum_dst), omp, oms);
        }

        EMIT("    }\n");
        EMIT("    }\n\n");
    }

    /* ---- Final combiner ----
     *
     * The NV2A final combiner computes:
     *   result.rgb = D + lerp(C, B, A)
     *              = D + A*B + (1-A)*C
     *   result.a   = G.a
     *
     * Additionally, E*F is computed and made available as the EF_PROD
     * register, and V1+R0 is available as V1R0_SUM. These are computed
     * BEFORE the final combiner reads its inputs.
     */
    EMIT("    /* ---- Final Combiner ---- */\n");

    /* Compute specials: EF product and V1R0 sum */
    EMIT("    float4 r_ef = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_v1r0sum = float4(0, 0, 0, 0);\n");

    /* E * F product */
    EMIT("    {\n");
    EMIT("        float4 e_val = float4(");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".rgb", -1);
    EMIT(", ");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".a", -1);
    EMIT(");\n");
    EMIT("        float4 f_val = float4(");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".rgb", -1);
    EMIT(", ");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".a", -1);
    EMIT(");\n");
    EMIT("        r_ef = e_val * f_val;\n");
    EMIT("    }\n");

    /* V1 + R0 final-combiner special, including complement flags. */
    EMIT("    r_v1r0sum = float4((%s + %s).rgb, 0.0);\n",
         state->final_inv_v1 ? "(1.0 - r_v1)" : "r_v1",
         state->final_inv_r0 ? "(1.0 - r_r0)" : "r_r0");
    if (state->final_clamp_sum)
        EMIT("    r_v1r0sum = saturate(r_v1r0sum);\n");
    EMIT("\n");

    /* Final combiner: result.rgb = D + A*B + (1-A)*C */
    /* Use last stage index for C0/C1 references in final combiner */
    {
        int fc_stage = -1;

        EMIT("    float4 result;\n");
        EMIT("    {\n");

        /* Read final combiner inputs A, B, C, D */
        EMIT("        float3 fc_a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[0], ".rgb", fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_b = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[1], ".rgb", fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_c = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[2], ".rgb", fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_d = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[3], ".rgb", fc_stage);
        EMIT(";\n");

        /* result.rgb = D + lerp(C, B, A) = D + A*B + (1-A)*C */
        EMIT("        result.rgb = saturate(fc_d + fc_a * fc_b + (1.0 - fc_a) * fc_c);\n");

        /* result.a = G.a */
        EMIT("        result.a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[6], ".a", fc_stage);
        EMIT(";\n");

        EMIT("    }\n\n");
    }

    /* ---- Alpha test ---- */
    EMIT("    /* Alpha test */\n");
    EMIT("    if (alpha_test_enable) {\n");
    EMIT("        int frag_alpha = (int)round(result.a * 255.0);\n");
    EMIT("        int alpha_ref_8 = (int)round(alpha_ref * 255.0);\n");
    EMIT("        bool alpha_pass = true;\n");
    EMIT("        if      (alpha_func == 1u) alpha_pass = false;\n");
    EMIT("        else if (alpha_func == 2u) alpha_pass = (frag_alpha <  alpha_ref_8);\n");
    EMIT("        else if (alpha_func == 3u) alpha_pass = (frag_alpha == alpha_ref_8);\n");
    EMIT("        else if (alpha_func == 4u) alpha_pass = (frag_alpha <= alpha_ref_8);\n");
    EMIT("        else if (alpha_func == 5u) alpha_pass = (frag_alpha >  alpha_ref_8);\n");
    EMIT("        else if (alpha_func == 6u) alpha_pass = (frag_alpha != alpha_ref_8);\n");
    EMIT("        else if (alpha_func == 7u) alpha_pass = (frag_alpha >= alpha_ref_8);\n");
    EMIT("        if (!alpha_pass) discard;\n");
    EMIT("    }\n\n");

    EMIT("    return result;\n");
    EMIT("}\n");

    return off;
}

#undef EMIT

/* ================================================================
 * Shader Compilation & Cache
 * ================================================================ */

static ID3D11PixelShader *compile_combiner_shader(const NV2ACombinerState *state)
{
    /* 16KB should be more than enough for any combiner shader */
    char hlsl[16384];
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    int len;
    uint64_t source_hash;
    const int trace_timing =
        getenv("MERCENARIES_TRACE_SHADER_TIMING") != NULL;
    const ULONGLONG timing_begin = GetTickCount64();
    ULONGLONG timing_generated = timing_begin;
    ULONGLONG timing_loaded = timing_begin;
    ULONGLONG timing_created;

    len = d3d8_combiners_generate_hlsl(state, hlsl, sizeof(hlsl));
    if (len < 0) {
        fprintf(stderr, "NV2A combiners: HLSL generation failed (buffer overflow)\n");
        return NULL;
    }
    if (trace_timing)
        timing_generated = GetTickCount64();

    source_hash = d3d8_shader_source_hash(hlsl, (size_t)len, "ps_5_0");
    {
        const char *trace_hash = getenv(
            "MERCENARIES_TRACE_COMBINER_HLSL_HASH");
        if (trace_hash != NULL &&
            (uint32_t)strtoul(trace_hash, NULL, 16) ==
                combiner_state_hash(state)) {
            fprintf(stderr,
                    "--- Generated combiner HLSL hash=%08X ---\n%s\n"
                    "--- End combiner HLSL ---\n",
                    combiner_state_hash(state), hlsl);
        }
    }
    hr = d3d8_shader_cache_load("ps", source_hash, &code);
    if (trace_timing)
        timing_loaded = GetTickCount64();
    if (hr == S_OK) {
        hr = ID3D11Device_CreatePixelShader(
            d3d8_GetD3D11Device(), ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &ps);
        timing_created = trace_timing ? GetTickCount64() : 0u;
        if (SUCCEEDED(hr)) {
            fprintf(stderr,
                    "NV2A combiners: Disk cache hit (hash 0x%08X)\n",
                    combiner_state_hash(state));
            if (trace_timing) {
                fprintf(stderr,
                        "[SHADER-TIMING] kind=ps hash=%08X cache=1 "
                        "generate_ms=%llu load_ms=%llu create_ms=%llu "
                        "total_ms=%llu\n",
                        combiner_state_hash(state),
                        (unsigned long long)(timing_generated - timing_begin),
                        (unsigned long long)(timing_loaded - timing_generated),
                        (unsigned long long)(timing_created - timing_loaded),
                        (unsigned long long)(timing_created - timing_begin));
                fflush(stderr);
            }
            ID3D10Blob_Release(code);
            return ps;
        }
        ID3D10Blob_Release(code);
        code = NULL;
        d3d8_shader_cache_invalidate("ps", source_hash);
    }
    fprintf(stderr,
            "NV2A combiners: Compiling shader (hash 0x%08X, %d HLSL bytes)\n",
            combiner_state_hash(state), len);
    hr = d3d8_compile_shader(hlsl, (SIZE_T)len, "ps_combiner", NULL, NULL,
                    "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        xbox_preview_log_event("shader-error", "pixel compile HRESULT=%08lX %.500s",hr, errors ? (char*)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        fprintf(stderr, "NV2A combiners: HLSL compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors)
                       : "unknown error");
        /* Dump the generated source for debugging */
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    d3d8_shader_cache_store("ps", source_hash, code);

    hr = ID3D11Device_CreatePixelShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &ps);
    ID3D10Blob_Release(code);

    if (FAILED(hr)) {
        xbox_preview_log_event("shader-error", "CreatePixelShader HRESULT=%08lX",hr);
        fprintf(stderr, "NV2A combiners: CreatePixelShader failed: 0x%08lX\n", hr);
        return NULL;
    }

    timing_created = GetTickCount64();
    if (timing_created - timing_begin >= 8u)
        xbox_preview_log_sample("shader-build", "kind=ps hash=%08X cache=0 elapsed_ms=%llu",
            combiner_state_hash(state), (unsigned long long)(timing_created - timing_begin));
    if (trace_timing) {
        fprintf(stderr, "[SHADER-TIMING] kind=ps hash=%08X cache=0 generate_ms=%llu load_ms=%llu compile_store_create_ms=%llu total_ms=%llu\n",
            combiner_state_hash(state),
            (unsigned long long)(timing_generated-timing_begin),
            (unsigned long long)(timing_loaded-timing_generated),
            (unsigned long long)(timing_created-timing_loaded),
            (unsigned long long)(timing_created-timing_begin));
        fflush(stderr);
    }
    return ps;
}

ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state)
{
    uint32_t hash = combiner_state_hash(state);
    uint32_t idx = hash & (COMBINER_CACHE_SIZE - 1);
    int probe;

    /* Linear probe lookup */
    for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) & (COMBINER_CACHE_SIZE - 1);
        CombinerCacheEntry *entry = &g_cache[slot];

        if (!entry->in_use) {
            /* Cache miss - compile and insert */
            ID3D11PixelShader *ps = compile_combiner_shader(state);
            if (!ps) return NULL;

            entry->in_use = TRUE;
            entry->hash = hash;
            memcpy(&entry->state, state, sizeof(NV2ACombinerState));
            entry->shader = ps;
            entry->last_used_frame = g_frame_counter;
            return ps;
        }

        if (entry->hash == hash && combiner_state_equal(&entry->state, state)) {
            /* Cache hit */
            entry->last_used_frame = g_frame_counter;
            return entry->shader;
        }
    }

    /*
     * Table is full - evict the least recently used entry.
     * This is rare in practice (most games use <20 configs).
     */
    {
        uint32_t lru_slot = idx;
        uint32_t lru_frame = UINT32_MAX;
        ID3D11PixelShader *ps;
        CombinerCacheEntry *entry;

        for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
            if (g_cache[probe].last_used_frame < lru_frame) {
                lru_frame = g_cache[probe].last_used_frame;
                lru_slot = probe;
            }
        }

        entry = &g_cache[lru_slot];
        if (entry->shader) {
            ID3D11PixelShader_Release(entry->shader);
        }

        ps = compile_combiner_shader(state);
        if (!ps) return NULL;

        entry->hash = hash;
        memcpy(&entry->state, state, sizeof(NV2ACombinerState));
        entry->shader = ps;
        entry->last_used_frame = g_frame_counter;
        return ps;
    }
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_combiners_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    g_ps_token = 0;
    g_dirty = TRUE;
    g_current_shader = NULL;
    g_frame_counter = 0;
    g_last_constants_valid = FALSE;
    g_current_shader = NULL;
    g_polygon_offset_enabled = 0;
    memset(g_polygon_offset, 0, sizeof(g_polygon_offset));
    g_constant_prepare_count = 0u;
    g_constant_upload_count = 0u;
    g_constant_map_failure_count = 0u;
    g_constant_trace_start_ms = 0u;
    g_constant_trace_enabled = -1;

    /* Create the PS constant buffer for combiner shaders.
     * Size must match NV2APSConstants, rounded up to 16-byte alignment. */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = (sizeof(NV2APSConstants) + 15) & ~15;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL,
                                   &g_combiner_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: Failed to create constant buffer: "
                "0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "NV2A combiners: Initialized (cache size=%d)\n",
            COMBINER_CACHE_SIZE);
    if (g_warmup_callback) g_warmup_callback();
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    int i;

    /* Release all cached shaders */
    for (i = 0; i < COMBINER_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && g_cache[i].shader) {
            ID3D11PixelShader_Release(g_cache[i].shader);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));

    if (g_combiner_cb) {
        ID3D11Buffer_Release(g_combiner_cb);
        g_combiner_cb = NULL;
    }
    g_last_constants_valid = FALSE;

    fprintf(stderr, "NV2A combiners: Shut down\n");
}

/* ================================================================
 * Draw Integration
 * ================================================================ */

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (g_external_state) {
        g_external_state = FALSE;
        g_dirty = TRUE;
        g_current_shader = NULL;
    }
    if (token != g_ps_token) {
        g_ps_token = token;
        g_dirty = TRUE;
        g_current_shader = NULL;
    }
}

BOOL d3d8_combiners_active(void)
{
    return g_external_state || g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
    if (!g_external_state)
        g_current_shader = NULL;
}

BOOL d3d8_combiners_prepare_draw(void)
{
    ID3D11DeviceContext *ctx;
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE mapped;
    NV2APSConstants constants;
    const DWORD *rs;
    HRESULT hr;
    BOOL constants_changed;
    int i;

    /* Not using combiner shaders - fall back to fixed-function */
    if (!g_external_state && g_ps_token == 0)
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx || !g_combiner_cb)
        return FALSE;

    rs = d3d8_GetRenderStates();

    /* Rebuild combiner state from token + render states if dirty */
    if (!g_external_state && g_dirty) {
        d3d8_combiners_parse_token(g_ps_token, rs, &g_combiner_state);
        g_dirty = FALSE;
        g_current_shader = NULL;
    }

    if (g_combiner_state.polygon_offset != g_polygon_offset_enabled) {
        g_combiner_state.polygon_offset = g_polygon_offset_enabled;
        g_current_shader = NULL;
    }

    /* Get or compile the pixel shader for this combiner state */
    ps = g_current_shader;
    if (!ps) {
        ps = d3d8_combiners_get_shader(&g_combiner_state);
        g_current_shader = ps;
    }
    if (!ps) {
        fprintf(stderr, "NV2A combiners: Failed to get shader, "
                "falling back to FFP\n");
        return FALSE;
    }

    /* Bind the combiner pixel shader */
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    /* Build an exact, padding-stable snapshot of the constants for this draw.
     * Xbox titles rewrite render state aggressively, but most successive draws
     * use identical values.  Mapping a D3D11 dynamic buffer unconditionally
     * introduces avoidable driver synchronization on every such draw. */
    memset(&constants, 0, sizeof(constants));

    /* Per-stage constants */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        d3dcolor_to_float4(g_combiner_state.c0[i], constants.c0[i]);
        d3dcolor_to_float4(g_combiner_state.c1[i], constants.c1[i]);
    }

    /* Final combiner constants */
    d3dcolor_to_float4(g_combiner_state.final_c0, constants.final_c0);
    d3dcolor_to_float4(g_combiner_state.final_c1, constants.final_c1);

    /* Fog color from render state */
    d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], constants.fog_color);
    memcpy(constants.tex_scale, g_texture_scale, sizeof(constants.tex_scale));
    memcpy(constants.flare_grid_ratio, g_flare_grid_ratio, sizeof(constants.flare_grid_ratio));
    memcpy(constants.polygon_offset, g_polygon_offset, sizeof(constants.polygon_offset));

    /* Alpha test parameters */
    constants.alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
    constants.alpha_func = rs[D3DRS_ALPHAFUNC];
    constants.alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
    constants.fog_enable = rs[D3DRS_FOGENABLE] ? 1 : 0;
    constants.alpha_kill_mask = 0u;
    for (i = 0; i < NV2A_MAX_TEXTURES; ++i) {
        const DWORD *tss = d3d8_GetTSS(i);
        if (tss && tss[D3DTSS_ALPHAKILL])
            constants.alpha_kill_mask |= 1u << i;
    }
    constants.color_key_mode_bits = g_color_key_mode_bits;
    memcpy(constants.color_key, g_color_key, sizeof(constants.color_key));
    memcpy(constants.color_key_mask, g_color_key_mask,
           sizeof(constants.color_key_mask));

    if (g_constant_trace_enabled < 0)
        g_constant_trace_enabled =
            getenv("MERCENARIES_TRACE_D3D_DRAW_PERF") != NULL;
    if (g_constant_trace_enabled) {
        const ULONGLONG now = GetTickCount64();
        ++g_constant_prepare_count;
        if (g_constant_trace_start_ms == 0u)
            g_constant_trace_start_ms = now;
    }

    constants_changed = !g_last_constants_valid ||
        memcmp(&constants, &g_last_constants, sizeof(constants)) != 0;
    if (constants_changed) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_combiner_cb,
                                    0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &constants, sizeof(constants));
            ID3D11DeviceContext_Unmap(ctx,
                                     (ID3D11Resource *)g_combiner_cb, 0);
            g_last_constants = constants;
            g_last_constants_valid = TRUE;
            if (g_constant_trace_enabled)
                ++g_constant_upload_count;
        } else {
            if (g_constant_trace_enabled)
                ++g_constant_map_failure_count;
        }
    }

    /* Reuse the existing renderer-performance opt-in for a low-volume cache
     * effectiveness counter.  No production preview enables this observer. */
    if (g_constant_trace_enabled) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_constant_trace_start_ms >= 1000u) {
            fprintf(stderr,
                    "[D3D-COMBINER-CB] prepares=%llu uploads=%llu "
                    "skips=%llu map_failures=%llu interval_ms=%llu\n",
                    (unsigned long long)g_constant_prepare_count,
                    (unsigned long long)g_constant_upload_count,
                    (unsigned long long)(g_constant_prepare_count -
                                         g_constant_upload_count -
                                         g_constant_map_failure_count),
                    (unsigned long long)g_constant_map_failure_count,
                    (unsigned long long)(now - g_constant_trace_start_ms));
            fflush(stderr);
            g_constant_prepare_count = 0u;
            g_constant_upload_count = 0u;
            g_constant_map_failure_count = 0u;
            g_constant_trace_start_ms = now;
        }
    }

    /* Bind the constant buffer to PS slot 0 */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_combiner_cb);

    /* Advance frame counter for LRU tracking */
    g_frame_counter++;

    return TRUE;
}
