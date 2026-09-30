/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator
 *
 * The Xbox NV2A GPU uses "register combiners" rather than traditional
 * pixel shaders. Games configure up to 8 general combiner stages plus
 * one final combiner stage. Each general combiner stage performs
 * independent RGB and alpha math on a register file that includes
 * texture samples, interpolated vertex colors, constants, and results
 * from previous stages.
 *
 * Register combiner pipeline overview:
 *
 *   Texture fetch -> [Stage 0] -> [Stage 1] -> ... -> [Stage N-1] -> [Final Combiner] -> output
 *
 * Each general combiner stage computes:
 *   AB = map(A) * map(B)      (per-component multiply)
 *   CD = map(C) * map(D)      (per-component multiply)
 *   output = AB + CD           (or AB dot CD if dot product flag set)
 *   output = scale_bias(output) (optional output mapping)
 *
 * RGB and alpha paths are fully independent per stage - different
 * inputs, different output registers, different flags.
 *
 * The final combiner computes:
 *   result.rgb = A*B + (1-A)*C + D
 *   result.a   = G.a
 * where A,B,C,D,E,F,G each select from the register file.
 * The product E*F is available as a special register (EF_PROD).
 * The sum V1+R0 is also available (V1R0_SUM / SPARE0).
 *
 * This module translates the combiner configuration into HLSL source,
 * compiles it to a D3D11 pixel shader, and caches the result.
 *
 * References:
 *   - NV_register_combiners / NV_register_combiners2 GL extensions
 *   - Xbox SDK D3D pixel shader documentation
 *   - xemu NV2A pgraph register combiner implementation
 */

#ifndef XBOXRECOMP_D3D8_COMBINERS_H
#define XBOXRECOMP_D3D8_COMBINERS_H

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NV2A Register Combiner Enumerations
 * ================================================================ */

/**
 * Input register selectors.
 *
 * These identify which register in the NV2A register file is read
 * as input to a combiner stage. The register file is shared across
 * all stages; writes from stage N are visible to stage N+1.
 */
typedef enum NV2ACombinerRegister {
    NV2A_REG_ZERO       = 0,   /* Constant zero (reads 0.0) */
    NV2A_REG_C0         = 1,   /* Per-stage constant color 0 */
    NV2A_REG_C1         = 2,   /* Per-stage constant color 1 */
    NV2A_REG_FOG        = 3,   /* Fog factor (interpolated) */
    NV2A_REG_V0         = 4,   /* Primary color (diffuse) */
    NV2A_REG_V1         = 5,   /* Secondary color (specular) */
    /* 6, 7 reserved */
    NV2A_REG_T0         = 8,   /* Texture 0 sample result */
    NV2A_REG_T1         = 9,   /* Texture 1 sample result */
    NV2A_REG_T2         = 10,  /* Texture 2 sample result */
    NV2A_REG_T3         = 11,  /* Texture 3 sample result */
    NV2A_REG_R0         = 12,  /* Temporary register 0 (also SPARE0) */
    NV2A_REG_R1         = 13,  /* Temporary register 1 (also SPARE1) */
    /* Final combiner only: */
    NV2A_REG_V1R0_SUM   = 14,  /* V1+R0 sum (final combiner) */
    NV2A_REG_EF_PROD    = 15,  /* E*F product (final combiner) */
    NV2A_REG_COUNT       = 16,
} NV2ACombinerRegister;

/* Aliases used in documentation and comments */
#define NV2A_REG_SPARE0  NV2A_REG_R0
#define NV2A_REG_SPARE1  NV2A_REG_R1

/**
 * Input mapping modes.
 *
 * Applied to the selected register value before it enters the
 * multiply. These implement common math operations as a pre-step.
 *
 * All inputs are clamped to [0,1] after texture sampling / interpolation,
 * then these mappings produce the final value fed to the multiplier:
 *
 *   UNSIGNED_IDENTITY : x        (passthrough, range [0,1])
 *   UNSIGNED_INVERT   : 1 - x    (complement, range [0,1])
 *   EXPAND_NORMAL     : 2x - 1   (expand to [-1,1])
 *   EXPAND_NEGATE     : 1 - 2x   (expand + negate)
 *   HALFBIAS_NORMAL   : x - 0.5  (shift to [-0.5,0.5])
 *   HALFBIAS_NEGATE   : 0.5 - x  (shift + negate)
 *   SIGNED_IDENTITY   : x        (signed passthrough, allows negative)
 *   SIGNED_NEGATE     : -x       (negate)
 */
typedef enum NV2AInputMapping {
    NV2A_MAP_UNSIGNED_IDENTITY = 0,
    NV2A_MAP_UNSIGNED_INVERT   = 1,
    NV2A_MAP_EXPAND_NORMAL     = 2,
    NV2A_MAP_EXPAND_NEGATE     = 3,
    NV2A_MAP_HALFBIAS_NORMAL   = 4,
    NV2A_MAP_HALFBIAS_NEGATE   = 5,
    NV2A_MAP_SIGNED_IDENTITY   = 6,
    NV2A_MAP_SIGNED_NEGATE     = 7,
    NV2A_MAP_COUNT             = 8,
} NV2AInputMapping;

/**
 * Output scale/bias modes.
 *
 * Applied to the stage output (AB+CD or AB.CD) before writing
 * to the destination register.
 */
typedef enum NV2AOutputMapping {
    NV2A_OUT_IDENTITY           = 0,  /* x */
    NV2A_OUT_BIAS               = 1,  /* x - 0.5 */
    NV2A_OUT_SHIFTLEFT_1        = 2,  /* x * 2 */
    NV2A_OUT_SHIFTLEFT_1_BIAS   = 3,  /* (x - 0.5) * 2 */
    NV2A_OUT_SHIFTLEFT_2        = 4,  /* x * 4 */
    NV2A_OUT_SHIFTRIGHT_1       = 5,  /* x / 2 */
    NV2A_OUT_COUNT              = 6,
} NV2AOutputMapping;

/**
 * Texture addressing modes per stage.
 * Encoded in the pixel shader DWORD token.
 */
typedef enum NV2ATextureMode {
    NV2A_TEXMODE_NONE       = 0x00,
    NV2A_TEXMODE_2D         = 0x01,
    NV2A_TEXMODE_3D         = 0x02,
    NV2A_TEXMODE_CUBEMAP    = 0x03,
    NV2A_TEXMODE_PASSTHRU   = 0x04,
    NV2A_TEXMODE_DOT_ST     = 0x09,
    NV2A_TEXMODE_DPNDNT_AR  = 0x0F,
    NV2A_TEXMODE_DPNDNT_GB  = 0x10,
    NV2A_TEXMODE_DOTPRODUCT = 0x11,
} NV2ATextureMode;

/* ================================================================
 * Combiner Stage Input / Output Descriptors
 * ================================================================ */

/**
 * A single combiner input selection.
 *
 * Packed in hardware as 8 bits:
 *   [3:0] register   - which register to read (NV2ACombinerRegister)
 *   [4]   alpha_rep   - 0: use full RGBA, 1: replicate alpha to all channels
 *   [7:5] mapping    - input mapping mode (NV2AInputMapping)
 */
typedef struct NV2ACombinerInput {
    NV2ACombinerRegister reg;       /* Source register */
    int                  alpha_rep; /* 1 = replicate alpha to RGB */
    NV2AInputMapping     mapping;   /* Input mapping function */
} NV2ACombinerInput;

/**
 * Output configuration for one channel (RGB or alpha) of a stage.
 *
 * Packed in hardware output word:
 *   [3:0]  ab_dst       - destination register for AB product
 *   [7:4]  cd_dst       - destination register for CD product
 *   [11:8] sum_dst      - destination register for AB+CD sum
 *   [12]   cd_dot       - 1: CD uses dot product instead of multiply
 *   [13]   ab_dot       - 1: AB uses dot product instead of multiply
 *   [14]   mux_sum      - 1: mux instead of sum (R0.a selects AB or CD)
 *   [17:15] output_map  - output scale/bias (NV2AOutputMapping)
 *   [18]   ab_cd_mux    - unused alias, same as mux_sum
 *
 * The "dot product" flag means AB = dot3(A, B) instead of A * B
 * component-wise. This is the key to bump mapping on NV2A.
 */
typedef struct NV2ACombinerOutput {
    NV2ACombinerRegister ab_dst;     /* Where to write A*B (or 0=discard) */
    NV2ACombinerRegister cd_dst;     /* Where to write C*D (or 0=discard) */
    NV2ACombinerRegister sum_dst;    /* Where to write AB+CD (or 0=discard) */
    int                  ab_dot;     /* 1 = dot product for AB */
    int                  cd_dot;     /* 1 = dot product for CD */
    int                  mux_sum;    /* 1 = mux(R0.a, AB, CD) instead of AB+CD */
    int                  ab_blue_to_alpha;
    int                  cd_blue_to_alpha;
    NV2AOutputMapping    output_map; /* Scale/bias applied to results */
} NV2ACombinerOutput;

/* ================================================================
 * Full Combiner State
 * ================================================================ */

/** Maximum number of general combiner stages (NV2A hardware limit). */
#define NV2A_MAX_COMBINER_STAGES 8

/** Maximum texture stages. */
#define NV2A_MAX_TEXTURES 4

/* D3DPS_COMBINERCOUNT flag values after the hardware control word is
 * shifted right by eight bits. These match Xemu's
 * PS_COMBINERCOUNTFLAGS definitions. */
#define NV2A_COMBINER_FLAG_MUX_MSB   0x0001u
#define NV2A_COMBINER_FLAG_UNIQUE_C0 0x0010u
#define NV2A_COMBINER_FLAG_UNIQUE_C1 0x0100u

/**
 * Complete NV2A register combiner configuration.
 *
 * This holds everything needed to generate an equivalent HLSL pixel
 * shader. Games set this up via SetPixelShader (DWORD token) or by
 * individually setting the D3DRS_PS* render states.
 */
typedef struct NV2ACombinerState {
    /* --- General combiner stages --- */
    int num_stages;  /* Active stage count (1-8) */

    struct {
        /* Four inputs per channel: A, B, C, D */
        NV2ACombinerInput  rgb_input[4];
        NV2ACombinerInput  alpha_input[4];
        NV2ACombinerOutput rgb_output;
        NV2ACombinerOutput alpha_output;
    } stages[NV2A_MAX_COMBINER_STAGES];

    /* --- Final combiner --- */
    NV2ACombinerInput final_input[7]; /* A, B, C, D, E, F, G */

    /* --- Per-stage constant colors --- */
    DWORD c0[NV2A_MAX_COMBINER_STAGES]; /* D3DCOLOR (ARGB) per stage */
    DWORD c1[NV2A_MAX_COMBINER_STAGES]; /* D3DCOLOR (ARGB) per stage */

    /* --- Final combiner constants --- */
    DWORD final_c0; /* Final combiner C0 (same as stage[final].c0) */
    DWORD final_c1; /* Final combiner C1 (same as stage[final].c1) */

    /* --- Texture modes --- */
    NV2ATextureMode tex_mode[NV2A_MAX_TEXTURES];
    int input_tex[NV2A_MAX_TEXTURES];
    int dot_map[NV2A_MAX_TEXTURES];

    /* --- Flags --- */
    DWORD flags;     /* Combiner count flags (MUX/unique constants). */
    int final_clamp_sum;
    int final_inv_v1;
    int final_inv_r0;
    int polygon_offset;
    int flare_grid; /* bit 0: guest-texel filter; bit 1: guest-pixel reduction */
    int screen_depth_stage; /* stage + 1: sample matching screen depth */
    int guest_depth; /* interpolate guest window Z before normalization */ /* Shader writes guest polygon-biased depth. */
} NV2ACombinerState;

/* ================================================================
 * PS Constant Buffer (HLSL layout - must be 16-byte aligned)
 *
 * This is uploaded to the GPU every draw call with current values
 * of the combiner constants, fog parameters, etc.
 * ================================================================ */

typedef struct NV2APSConstants {
    float c0[NV2A_MAX_COMBINER_STAGES][4]; /* Per-stage C0 (RGBA float) */
    float c1[NV2A_MAX_COMBINER_STAGES][4]; /* Per-stage C1 (RGBA float) */
    float final_c0[4];                      /* Final combiner C0 */
    float final_c1[4];                      /* Final combiner C1 */
    float fog_color[4];                     /* Fog color (from D3DRS_FOGCOLOR) */
    float tex_scale[NV2A_MAX_TEXTURES][4];  /* Normalize rectangle texture UVs */
    float alpha_ref;                        /* Normalized [0,1] alpha ref */
    UINT  alpha_func;                       /* D3DCMPFUNC enum value */
    UINT  alpha_test_enable;                /* 0 or 1 */
    UINT  fog_enable;                       /* 0 or 1 */
    UINT  alpha_kill_mask;                  /* texture-stage alpha-kill bits */
    UINT  _alpha_kill_pad[3];
    UINT  color_key_mode_bits;              /* 2-bit mode per texture stage */
    UINT  _color_key_pad[3];
    UINT  color_key[NV2A_MAX_TEXTURES];
    UINT  color_key_mask[NV2A_MAX_TEXTURES];
    float flare_grid_ratio[4]; /* guest/host output pixel ratios */
    float polygon_offset[4]; /* normalized bias, slope, host/guest x/y scale */
} NV2APSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the combiner translator system.
 * Allocates the PS constant buffer and sets up the shader cache.
 * Must be called after D3D11 device creation.
 */
HRESULT d3d8_combiners_init(void);
/* Optional title-owned startup warm-up; called after the shader system is ready. */
void d3d8_combiners_set_warmup(void (*callback)(void));

/**
 * Shut down the combiner system.
 * Releases all cached pixel shaders and the constant buffer.
 */
void d3d8_combiners_shutdown(void);

/**
 * Parse an Xbox pixel shader DWORD token into combiner state.
 *
 * The token encodes the combiner count and texture modes.
 * The actual combiner stage inputs/outputs come from the
 * D3DRS_PS* render states which must already be set.
 *
 * @param token  The DWORD passed to SetPixelShader
 * @param rs     Pointer to the render state array (from d3d8_GetRenderStates)
 * @param state  Output: filled combiner state structure
 */
void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state);

/**
 * Build combiner state from individual render states.
 *
 * Called when games set PS render states directly rather than
 * using a pixel shader token.
 *
 * @param rs     Pointer to the render state array
 * @param state  Output: filled combiner state structure
 */
void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state);

/**
 * Generate HLSL pixel shader source from combiner state.
 *
 * @param state   The combiner configuration
 * @param buf     Output buffer for HLSL source
 * @param bufsize Size of output buffer in bytes
 * @return        Number of characters written (excluding null terminator),
 *                or -1 on error
 */
int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize);

/**
 * Get or create a compiled pixel shader for the given combiner state.
 *
 * Looks up the state in the shader cache. On cache miss, generates
 * HLSL, compiles it with D3DCompile, and caches the result.
 *
 * @param state  The combiner configuration
 * @return       Compiled pixel shader, or NULL on failure.
 *               The shader is owned by the cache - do NOT release it.
 */
ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state);

/**
 * Prepare for a draw call using register combiners.
 *
 * This is the main integration point. Call this instead of (or after)
 * d3d8_shaders_prepare_draw() when a combiner pixel shader is active.
 *
 * - Rebuilds combiner state from current render states if dirty
 * - Gets or compiles the matching pixel shader
 * - Updates the PS constant buffer with current C0/C1/fog values
 * - Binds the pixel shader and constant buffer to the pipeline
 *
 * @return TRUE if a combiner shader was bound, FALSE if falling back
 *         to the fixed-function pixel shader.
 */
BOOL d3d8_combiners_prepare_draw(void);

/**
 * Notify the combiner system that a PS-related render state changed.
 * This marks the combiner state as dirty so it will be rebuilt
 * on the next prepare_draw call.
 */
void d3d8_combiners_mark_dirty(void);

/**
 * Set the current pixel shader token.
 * Pass 0 to disable combiner shaders and revert to fixed-function.
 */
void d3d8_combiners_set_pixel_shader(DWORD token);
void d3d8_combiners_from_nv2a_registers(
    DWORD control, const DWORD rgb_inputs[8], const DWORD alpha_inputs[8],
    const DWORD rgb_outputs[8], const DWORD alpha_outputs[8],
    DWORD final_inputs_0, DWORD final_inputs_1,
    const DWORD factor0[8], const DWORD factor1[8],
    DWORD final_factor0, DWORD final_factor1, DWORD shader_stage_program,
    DWORD dot_mapping, DWORD other_stage_input, NV2ACombinerState *state);
void d3d8_combiners_set_nv2a_state(const NV2ACombinerState *state);
BOOL d3d8_combiners_requires_triangle_depth(void);
void d3d8_combiners_set_flare_grid_ratio(float x, float y);
void d3d8_combiners_set_polygon_offset(BOOL enabled, float bias, float slope, float scale_x, float scale_y);
void d3d8_combiners_set_texture_scale(UINT stage, float u_scale,
                                       float v_scale);
void d3d8_combiners_set_texture_red_blue_swap(UINT stage, BOOL swap);
void d3d8_combiners_set_color_key(UINT stage, UINT mode, UINT color,
                                  UINT mask);

/**
 * Get whether a combiner pixel shader is currently active.
 */
BOOL d3d8_combiners_active(void);

/** Read-only diagnostic identity for the currently bound combiner structure. */
uint32_t d3d8_combiners_debug_state_hash(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_COMBINERS_H */
