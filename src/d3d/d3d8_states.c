/**
 * D3D8 Render State → D3D11 State Object Translation
 *
 * Converts D3D8 render state values into D3D11 state objects:
 *   - Blend state (alpha blending, color write mask)
 *   - Depth-stencil state (z-test, z-write, stencil)
 *   - Rasterizer state (cull mode, fill mode)
 *   - Sampler state (texture filtering, addressing)
 *
 * State objects are cached and recreated only when dirty.
 */

#include "d3d8_internal.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Cached D3D11 state objects
 * ================================================================ */

static ID3D11BlendState        *g_blend_state = NULL;
static ID3D11DepthStencilState *g_ds_state = NULL;
static ID3D11RasterizerState   *g_raster_state = NULL;
static ID3D11SamplerState      *g_sampler_states[4] = { NULL, NULL, NULL, NULL };
static D3D11_SAMPLER_DESC       g_last_sampler_desc[4];
static BOOL                     g_last_sampler_valid[4];
static BOOL                     g_force_anisotropic_16x;

/* D3D11 state objects are immutable.  Guest rendering commonly alternates
 * between a small collection of descriptors, so retaining only the most
 * recently used object turns every A/B/A transition into another driver
 * Create*State call.  Keep exact descriptor-keyed process-lifetime caches;
 * the descriptors are zero-initialized before construction, making bytewise
 * comparison deterministic.  A bounded transient fallback preserves correct
 * behavior even if an unusual title exceeds the cache capacity. */
#define D3D8_STATE_CACHE_CAPACITY 1024u

typedef struct D3D8BlendCacheEntry {
    D3D11_BLEND_DESC desc;
    ID3D11BlendState *state;
} D3D8BlendCacheEntry;
typedef struct D3D8DepthStencilCacheEntry {
    D3D11_DEPTH_STENCIL_DESC desc;
    ID3D11DepthStencilState *state;
} D3D8DepthStencilCacheEntry;
typedef struct D3D8RasterizerCacheEntry {
    D3D11_RASTERIZER_DESC desc;
    ID3D11RasterizerState *state;
} D3D8RasterizerCacheEntry;
typedef struct D3D8SamplerCacheEntry {
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState *state;
} D3D8SamplerCacheEntry;

static D3D8BlendCacheEntry g_blend_cache[D3D8_STATE_CACHE_CAPACITY];
static D3D8DepthStencilCacheEntry g_ds_cache[D3D8_STATE_CACHE_CAPACITY];
static D3D8RasterizerCacheEntry g_raster_cache[D3D8_STATE_CACHE_CAPACITY];
static D3D8SamplerCacheEntry g_sampler_cache[D3D8_STATE_CACHE_CAPACITY];
static UINT g_blend_cache_count;
static UINT g_ds_cache_count;
static UINT g_raster_cache_count;
static UINT g_sampler_cache_count;
static BOOL g_blend_state_transient;
static BOOL g_ds_state_transient;
static BOOL g_raster_state_transient;
static BOOL g_sampler_state_transient[4];

void d3d8_SetForceAnisotropic16x(BOOL enabled)
{
    const BOOL next = enabled != FALSE;
    UINT stage;
    if (g_force_anisotropic_16x == next)
        return;
    g_force_anisotropic_16x = next;
    /* The override is part of the effective sampler descriptor. Invalidate
     * every cached stage so an Apply operation takes effect on the next draw
     * instead of waiting for the guest to change its texture state. */
    for (stage = 0u; stage < 4u; ++stage) {
        g_last_sampler_valid[stage] = FALSE;
        if (g_sampler_state_transient[stage] && g_sampler_states[stage]) {
            ID3D11SamplerState_Release(g_sampler_states[stage]);
        }
        g_sampler_states[stage] = NULL;
        g_sampler_state_transient[stage] = FALSE;
    }
}

/* Exact descriptors avoid collisions between distinct guest render states. */
static D3D11_BLEND_DESC g_last_blend_desc;
static D3D11_DEPTH_STENCIL_DESC g_last_ds_desc;
static D3D11_RASTERIZER_DESC g_last_raster_desc;
static BOOL g_last_blend_valid;
static BOOL g_last_ds_valid;
static BOOL g_last_raster_valid;
static BOOL g_scissor_enable = FALSE;
static BOOL g_line_smooth_enable = FALSE;
static BOOL g_depth_clip_enable = TRUE;
static D3D11_RECT g_scissor_rect;

/* ================================================================
 * D3D8 → D3D11 enum translation
 * ================================================================ */

static D3D11_BLEND d3d8_to_d3d11_blend(DWORD d3d8blend)
{
    switch (d3d8blend) {
    case D3DBLEND_ZERO:         return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:          return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:     return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return D3D11_BLEND_SRC_ALPHA_SAT;
    default:                    return D3D11_BLEND_ONE;
    }
}

/* D3D8 applies one component-wise factor to RGBA. D3D11 describes RGB and
 * alpha separately and rejects color-valued factors in SrcBlendAlpha and
 * DestBlendAlpha. Preserve D3D8's alpha-channel result by translating the
 * alpha component of each factor explicitly. */
static D3D11_BLEND d3d8_to_d3d11_blend_alpha(DWORD d3d8blend)
{
    switch (d3d8blend) {
    case D3DBLEND_ZERO:         return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:          return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:
    case D3DBLEND_SRCALPHA:     return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCCOLOR:
    case D3DBLEND_INVSRCALPHA:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTCOLOR:
    case D3DBLEND_DESTALPHA:    return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTCOLOR:
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    /* The alpha component of source-alpha-saturate is one. */
    case D3DBLEND_SRCALPHASAT:  return D3D11_BLEND_ONE;
    default:                    return D3D11_BLEND_ONE;
    }
}
static D3D11_COMPARISON_FUNC d3d8_to_d3d11_cmp(DWORD d3d8cmp)
{
    switch (d3d8cmp) {
    case D3DCMP_NEVER:        return D3D11_COMPARISON_NEVER;
    case D3DCMP_LESS:         return D3D11_COMPARISON_LESS;
    case D3DCMP_EQUAL:        return D3D11_COMPARISON_EQUAL;
    case D3DCMP_LESSEQUAL:    return D3D11_COMPARISON_LESS_EQUAL;
    case D3DCMP_GREATER:      return D3D11_COMPARISON_GREATER;
    case D3DCMP_NOTEQUAL:     return D3D11_COMPARISON_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
    case D3DCMP_ALWAYS:       return D3D11_COMPARISON_ALWAYS;
    default:                  return D3D11_COMPARISON_LESS_EQUAL;
    }
}

static D3D11_STENCIL_OP d3d8_to_d3d11_stencilop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_STENCIL_OP_KEEP;
    case 2: return D3D11_STENCIL_OP_ZERO;
    case 3: return D3D11_STENCIL_OP_REPLACE;
    case 4: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: return D3D11_STENCIL_OP_INVERT;
    case 7: return D3D11_STENCIL_OP_INCR;
    case 8: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_BLEND_OP d3d8_to_d3d11_blendop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_BLEND_OP_ADD;
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

/* ================================================================
 * State object creation
 * ================================================================ */

static void update_blend_state(const DWORD *rs)
{
    D3D11_BLEND_DESC bd;
    HRESULT hr;
    UINT i;
    ID3D11BlendState *state = NULL;

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = d3d8_to_d3d11_blend(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlend = d3d8_to_d3d11_blend(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOp = d3d8_to_d3d11_blendop(
        rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    bd.RenderTarget[0].SrcBlendAlpha =
        d3d8_to_d3d11_blend_alpha(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlendAlpha =
        d3d8_to_d3d11_blend_alpha(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask =
        (UINT8)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);

    if (g_last_blend_valid && g_blend_state &&
        memcmp(&bd, &g_last_blend_desc, sizeof(bd)) == 0)
        return;
    if (g_blend_state_transient && g_blend_state) {
        ID3D11BlendState_Release(g_blend_state);
        g_blend_state = NULL;
        g_blend_state_transient = FALSE;
    }
    for (i = 0u; i < g_blend_cache_count; ++i) {
        if (memcmp(&bd, &g_blend_cache[i].desc, sizeof(bd)) == 0) {
            g_blend_state = g_blend_cache[i].state;
            g_last_blend_desc = bd;
            g_last_blend_valid = TRUE;
            return;
        }
    }
    g_last_blend_desc = bd;
    g_last_blend_valid = TRUE;

    hr = ID3D11Device_CreateBlendState(
        d3d8_GetD3D11Device(), &bd, &state);
    if (FAILED(hr)) {
        g_last_blend_valid = FALSE;
        fprintf(stderr,
                "D3D8: CreateBlendState failed: 0x%08lX "
                "enable=%u src=%u/%u dst=%u/%u op=%u write=%02X\n",
                hr, bd.RenderTarget[0].BlendEnable,
                bd.RenderTarget[0].SrcBlend,
                bd.RenderTarget[0].SrcBlendAlpha,
                bd.RenderTarget[0].DestBlend,
                bd.RenderTarget[0].DestBlendAlpha,
                bd.RenderTarget[0].BlendOp,
                bd.RenderTarget[0].RenderTargetWriteMask);
        return;
    }
    g_blend_state = state;
    if (g_blend_cache_count < D3D8_STATE_CACHE_CAPACITY) {
        g_blend_cache[g_blend_cache_count].desc = bd;
        g_blend_cache[g_blend_cache_count].state = state;
        ++g_blend_cache_count;
    } else {
        g_blend_state_transient = TRUE;
    }
}

static void update_depth_stencil_state(const DWORD *rs)
{
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;
    UINT i;
    ID3D11DepthStencilState *state = NULL;

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = rs[D3DRS_ZENABLE] ? TRUE : FALSE;
    dsd.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ?
        D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = d3d8_to_d3d11_cmp(rs[D3DRS_ZFUNC]);
    dsd.StencilEnable = rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    dsd.StencilReadMask = (UINT8)(rs[D3DRS_STENCILMASK] & 0xFF);
    dsd.StencilWriteMask = (UINT8)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);
    dsd.FrontFace.StencilFunc =
        d3d8_to_d3d11_cmp(rs[D3DRS_STENCILFUNC]);
    dsd.FrontFace.StencilFailOp =
        d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILFAIL]);
    dsd.FrontFace.StencilDepthFailOp =
        d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILZFAIL]);
    dsd.FrontFace.StencilPassOp =
        d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILPASS]);
    dsd.BackFace = dsd.FrontFace;

    if (g_last_ds_valid && g_ds_state &&
        memcmp(&dsd, &g_last_ds_desc, sizeof(dsd)) == 0)
        return;
    if (g_ds_state_transient && g_ds_state) {
        ID3D11DepthStencilState_Release(g_ds_state);
        g_ds_state = NULL;
        g_ds_state_transient = FALSE;
    }
    for (i = 0u; i < g_ds_cache_count; ++i) {
        if (memcmp(&dsd, &g_ds_cache[i].desc, sizeof(dsd)) == 0) {
            g_ds_state = g_ds_cache[i].state;
            g_last_ds_desc = dsd;
            g_last_ds_valid = TRUE;
            return;
        }
    }
    g_last_ds_desc = dsd;
    g_last_ds_valid = TRUE;

    hr = ID3D11Device_CreateDepthStencilState(
        d3d8_GetD3D11Device(), &dsd, &state);
    if (FAILED(hr)) {
        g_last_ds_valid = FALSE;
        fprintf(stderr,
                "D3D8: CreateDepthStencilState failed: 0x%08lX\n", hr);
        return;
    }
    g_ds_state = state;
    if (g_ds_cache_count < D3D8_STATE_CACHE_CAPACITY) {
        g_ds_cache[g_ds_cache_count].desc = dsd;
        g_ds_cache[g_ds_cache_count].state = state;
        ++g_ds_cache_count;
    } else {
        g_ds_state_transient = TRUE;
    }
}

static void update_rasterizer_state(const DWORD *rs)
{
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;
    UINT i;
    ID3D11RasterizerState *state = NULL;

    memset(&rd, 0, sizeof(rd));
    switch (rs[D3DRS_FILLMODE]) {
    case D3DFILL_POINT:     rd.FillMode = D3D11_FILL_WIREFRAME; break;
    case D3DFILL_WIREFRAME: rd.FillMode = D3D11_FILL_WIREFRAME; break;
    default:                rd.FillMode = D3D11_FILL_SOLID; break;
    }
    switch (rs[D3DRS_CULLMODE]) {
    case D3DCULL_NONE: rd.CullMode = D3D11_CULL_NONE; break;
    case D3DCULL_CW:   rd.CullMode = D3D11_CULL_FRONT; break;
    case D3DCULL_CCW:  rd.CullMode = D3D11_CULL_BACK; break;
    default:           rd.CullMode = D3D11_CULL_BACK; break;
    }
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = g_depth_clip_enable;
    rd.ScissorEnable = g_scissor_enable;
    rd.MultisampleEnable = FALSE;
    /* NV097_SET_LINE_SMOOTH_ENABLE maps to the D3D11 alpha-antialiased
     * line rasterizer when the target itself is not multisampled. */
    rd.AntialiasedLineEnable = g_line_smooth_enable;

    if (g_last_raster_valid && g_raster_state &&
        memcmp(&rd, &g_last_raster_desc, sizeof(rd)) == 0)
        return;
    if (g_raster_state_transient && g_raster_state) {
        ID3D11RasterizerState_Release(g_raster_state);
        g_raster_state = NULL;
        g_raster_state_transient = FALSE;
    }
    for (i = 0u; i < g_raster_cache_count; ++i) {
        if (memcmp(&rd, &g_raster_cache[i].desc, sizeof(rd)) == 0) {
            g_raster_state = g_raster_cache[i].state;
            g_last_raster_desc = rd;
            g_last_raster_valid = TRUE;
            return;
        }
    }
    g_last_raster_desc = rd;
    g_last_raster_valid = TRUE;

    hr = ID3D11Device_CreateRasterizerState(
        d3d8_GetD3D11Device(), &rd, &state);
    if (FAILED(hr)) {
        g_last_raster_valid = FALSE;
        fprintf(stderr,
                "D3D8: CreateRasterizerState failed: 0x%08lX\n", hr);
        return;
    }
    g_raster_state = state;
    if (g_raster_cache_count < D3D8_STATE_CACHE_CAPACITY) {
        g_raster_cache[g_raster_cache_count].desc = rd;
        g_raster_cache[g_raster_cache_count].state = state;
        ++g_raster_cache_count;
    } else {
        g_raster_state_transient = TRUE;
    }
}
/* ================================================================
 * Sampler state
 * ================================================================ */

static D3D11_TEXTURE_ADDRESS_MODE d3d8_to_d3d11_address(DWORD mode)
{
    switch (mode) {
    case D3DTADDRESS_WRAP:       return D3D11_TEXTURE_ADDRESS_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D11_TEXTURE_ADDRESS_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:                     return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static D3D11_FILTER d3d8_to_d3d11_filter(DWORD mag, DWORD min, DWORD mip)
{
    const BOOL mag_linear =
        mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC;
    const BOOL min_linear =
        min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC;
    const BOOL mip_linear = mip == D3DTEXF_LINEAR;

    if (mag == D3DTEXF_ANISOTROPIC || min == D3DTEXF_ANISOTROPIC)
        return D3D11_FILTER_ANISOTROPIC;

    switch ((min_linear ? 4u : 0u) |
            (mag_linear ? 2u : 0u) |
            (mip_linear ? 1u : 0u)) {
    case 0u: return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case 1u: return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    case 2u: return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case 3u: return D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    case 4u: return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    case 5u: return D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case 6u: return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case 7u: return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    default: return D3D11_FILTER_MIN_MAG_MIP_POINT;
    }
}
void d3d8_states_apply_sampler(DWORD stage)
{
    const DWORD *tss;
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;
    UINT i;
    ID3D11SamplerState *state = NULL;
    float lod_bias = 0.0f;
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();

    if (stage >= 4) return;
    tss = d3d8_GetTSS(stage);
    if (!tss) return;

    memset(&sd, 0, sizeof(sd));
    sd.Filter = d3d8_to_d3d11_filter(
        tss[D3DTSS_MAGFILTER],
        tss[D3DTSS_MINFILTER],
        tss[D3DTSS_MIPFILTER]);
    if (g_force_anisotropic_16x &&
        tss[D3DTSS_MINFILTER] != D3DTEXF_POINT)
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    sd.AddressV = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    if (g_force_anisotropic_16x && sd.Filter == D3D11_FILTER_ANISOTROPIC)
        sd.MaxAnisotropy = 16;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    /* D3DCOLOR and NV2A border registers are packed ARGB. D3D11 expects
     * normalized RGBA floats in the sampler descriptor, as Xemu does. */
    sd.BorderColor[0] = (FLOAT)((tss[D3DTSS_BORDERCOLOR] >> 16) & 0xFFu) / 255.0f;
    sd.BorderColor[1] = (FLOAT)((tss[D3DTSS_BORDERCOLOR] >> 8) & 0xFFu) / 255.0f;
    sd.BorderColor[2] = (FLOAT)(tss[D3DTSS_BORDERCOLOR] & 0xFFu) / 255.0f;
    sd.BorderColor[3] = (FLOAT)((tss[D3DTSS_BORDERCOLOR] >> 24) & 0xFFu) / 255.0f;
    memcpy(&lod_bias, &tss[D3DTSS_MIPMAPLODBIAS], sizeof(lod_bias));
    if (!(lod_bias >= -32.0f && lod_bias <= 32.0f)) lod_bias = 0.0f;
    sd.MipLODBias = lod_bias;
    sd.MinLOD = (FLOAT)tss[D3DTSS_MAXMIPLEVEL];
    sd.MaxLOD = tss[D3DTSS_MIPFILTER] == D3DTEXF_NONE ?
        sd.MinLOD : D3D11_FLOAT32_MAX;

    if (g_last_sampler_valid[stage] && g_sampler_states[stage] &&
        memcmp(&sd, &g_last_sampler_desc[stage], sizeof(sd)) == 0) {
        ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1,
                                         &g_sampler_states[stage]);
        return;
    }
    if (g_sampler_state_transient[stage] && g_sampler_states[stage]) {
        ID3D11SamplerState_Release(g_sampler_states[stage]);
        g_sampler_states[stage] = NULL;
        g_sampler_state_transient[stage] = FALSE;
    }
    for (i = 0u; i < g_sampler_cache_count; ++i) {
        if (memcmp(&sd, &g_sampler_cache[i].desc, sizeof(sd)) == 0) {
            g_sampler_states[stage] = g_sampler_cache[i].state;
            g_last_sampler_desc[stage] = sd;
            g_last_sampler_valid[stage] = TRUE;
            ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1,
                                             &g_sampler_states[stage]);
            return;
        }
    }
    hr = ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd,
                                         &state);
    if (SUCCEEDED(hr)) {
        g_sampler_states[stage] = state;
        g_last_sampler_desc[stage] = sd;
        g_last_sampler_valid[stage] = TRUE;
        if (g_sampler_cache_count < D3D8_STATE_CACHE_CAPACITY) {
            g_sampler_cache[g_sampler_cache_count].desc = sd;
            g_sampler_cache[g_sampler_cache_count].state = state;
            ++g_sampler_cache_count;
        } else {
            g_sampler_state_transient[stage] = TRUE;
        }
        ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1,
                                         &g_sampler_states[stage]);
    }
}

/* ================================================================
 * Apply all states before draw call
 * ================================================================ */

HRESULT d3d8_states_init(void)
{
    /* States are created on first apply */
    return S_OK;
}

void d3d8_states_shutdown(void)
{
    UINT i;
    if (g_blend_state_transient && g_blend_state)
        ID3D11BlendState_Release(g_blend_state);
    if (g_ds_state_transient && g_ds_state)
        ID3D11DepthStencilState_Release(g_ds_state);
    if (g_raster_state_transient && g_raster_state)
        ID3D11RasterizerState_Release(g_raster_state);
    for (i = 0u; i < 4u; ++i) {
        if (g_sampler_state_transient[i] && g_sampler_states[i])
            ID3D11SamplerState_Release(g_sampler_states[i]);
        g_sampler_states[i] = NULL;
        g_sampler_state_transient[i] = FALSE;
        g_last_sampler_valid[i] = FALSE;
    }
    for (i = 0u; i < g_blend_cache_count; ++i)
        ID3D11BlendState_Release(g_blend_cache[i].state);
    for (i = 0u; i < g_ds_cache_count; ++i)
        ID3D11DepthStencilState_Release(g_ds_cache[i].state);
    for (i = 0u; i < g_raster_cache_count; ++i)
        ID3D11RasterizerState_Release(g_raster_cache[i].state);
    for (i = 0u; i < g_sampler_cache_count; ++i)
        ID3D11SamplerState_Release(g_sampler_cache[i].state);
    g_blend_cache_count = 0u;
    g_ds_cache_count = 0u;
    g_raster_cache_count = 0u;
    g_sampler_cache_count = 0u;
    g_blend_state = NULL;
    g_ds_state = NULL;
    g_raster_state = NULL;
    g_blend_state_transient = FALSE;
    g_ds_state_transient = FALSE;
    g_raster_state_transient = FALSE;
    g_last_blend_valid = FALSE;
    g_last_ds_valid = FALSE;
    g_last_raster_valid = FALSE;
}

void d3d8_states_apply(void)
{
    const DWORD *rs = d3d8_GetRenderStates();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    float blend_factor[4] = { 1, 1, 1, 1 };

    if (!rs || !ctx) return;

    update_blend_state(rs);
    update_depth_stencil_state(rs);
    update_rasterizer_state(rs);

    if (g_blend_state)
        ID3D11DeviceContext_OMSetBlendState(ctx, g_blend_state, blend_factor, 0xFFFFFFFF);
    if (g_ds_state)
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_ds_state, rs[D3DRS_STENCILREF]);
    if (g_raster_state)
        ID3D11DeviceContext_RSSetState(ctx, g_raster_state);
    if (g_scissor_enable)
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &g_scissor_rect);

    /* Apply samplers for all 4 texture stages */
    {
        DWORD s;
        for (s = 0; s < 4; s++)
            d3d8_states_apply_sampler(s);
    }
}

void d3d8_states_debug_trace_blend(const char *label)
{
    const DWORD *rs = d3d8_GetRenderStates();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11BlendState *bound = NULL;
    D3D11_BLEND_DESC desc;
    FLOAT factor[4] = { 0, 0, 0, 0 };
    UINT mask = 0;

    if (!rs || !ctx)
        return;
    memset(&desc, 0, sizeof(desc));
    ID3D11DeviceContext_OMGetBlendState(ctx, &bound, factor, &mask);
    if (bound)
        ID3D11BlendState_GetDesc(bound, &desc);
    fprintf(stderr,
            "[D3D8-HOST-BLEND] %s rs=%u/%u/%u/%u bound=%p "
            "desc=%u/%u/%u op=%u mask=%02X sample=%08X\n",
            label ? label : "draw", rs[D3DRS_ALPHABLENDENABLE],
            rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_BLENDOP],
            (void *)bound, desc.RenderTarget[0].BlendEnable,
            desc.RenderTarget[0].SrcBlend, desc.RenderTarget[0].DestBlend,
            desc.RenderTarget[0].BlendOp,
            desc.RenderTarget[0].RenderTargetWriteMask, mask);
    if (bound)
        ID3D11BlendState_Release(bound);
}

void d3d8_states_set_scissor(BOOL enable, LONG left, LONG top,
                             LONG right, LONG bottom)
{
    g_scissor_enable = enable;
    g_scissor_rect.left = left;
    g_scissor_rect.top = top;
    g_scissor_rect.right = right;
    g_scissor_rect.bottom = bottom;
}

void d3d8_states_set_line_smooth(BOOL enable)
{
    enable = enable ? TRUE : FALSE;
    if (g_line_smooth_enable != enable) {
        g_line_smooth_enable = enable;
        g_last_raster_valid = FALSE;
    }
}

void d3d8_states_set_depth_clamp(BOOL enable)
{
    const BOOL clip = enable ? FALSE : TRUE;
    if (g_depth_clip_enable != clip) {
        g_depth_clip_enable = clip;
        g_last_raster_valid = FALSE;
    }
}
