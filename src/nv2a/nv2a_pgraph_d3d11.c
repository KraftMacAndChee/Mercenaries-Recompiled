/*
 * NV2A PGRAPH -> D3D11 Translator
 *
 * Translates NV2A push buffer methods into D3D8->D3D11 rendering calls.
 * Designed for Xbox static recompilation (xboxrecomp toolkit).
 *
 * Menu rendering profile (captured from xemu):
 *   - INLINE_ARRAY with 5-dword vertices (X, Y, U, V, Color)
 *   - TRIANGLE_STRIP topology
 *   - ~448 vertices per frame (~89 quads)
 *   - Textured 2D elements in 640x480 screen space
 */

#include "nv2a_pgraph_d3d11.h"
#include "nv2a_regs.h"
#include "nv2a_state.h"
#include "nv2a_image_blit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

/* Renderer diagnostics are process-launch configuration. Cache getenv results
 * by call-site string pointer so disabled probes stay cheap in method/draw hot
 * paths. Environment variables are never mutated after renderer startup. */
static const char *pgraph_cached_getenv(const char *name)
{
    typedef struct PgraphEnvCacheEntry {
        const char *name;
        const char *value;
    } PgraphEnvCacheEntry;
    static PgraphEnvCacheEntry cache[512];
    uintptr_t key = (uintptr_t)name;
    size_t index = (size_t)((key >> 4u) ^ (key >> 12u)) & 511u;
    size_t probe;

    for (probe = 0u; probe < 512u; ++probe) {
        PgraphEnvCacheEntry *entry = &cache[(index + probe) & 511u];
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

/* Pending resolves wait for a short quiet period so later HUD writes remain
 * part of the same scanout. Millisecond tick counts are too coarse here:
 * their 10-16ms granularity can starve scanout when a fast guest rewrites the
 * surface before the next tick. Use QPC for the idle fallback deadline;
 * normal 30/60 Hz scanout is consumed by the explicit guest flip. */
static ULONGLONG pgraph_scanout_clock_us(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    ULONGLONG ticks, hz;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0 || !QueryPerformanceCounter(&now))
        return GetTickCount64() * 1000u;
    ticks = (ULONGLONG)now.QuadPart; hz = (ULONGLONG)frequency.QuadPart;
    return (ticks / hz) * 1000000u + ((ticks % hz) * 1000000u) / hz;
}

/* NV2A programmable shaders have already divided screen depth by W. Preserve
 * that value for raster interpolation instead of multiplying/dividing it by W
 * again in host clip space (up to three additional D24 steps of error).
 * The opt-in gate retains same-camera comparisons; normal builds do no file I/O. */
static int pgraph_guest_depth_enabled(void)
{
    const char *enabled = pgraph_cached_getenv("MERCENARIES_TEST_GUEST_DEPTH");
    const char *gate;
    static DWORD last_check;
    static int gate_present;
    DWORD now;
    if (!enabled) return 1;
    gate = pgraph_cached_getenv("MERCENARIES_TEST_GUEST_DEPTH_GATE_FILE");
    if (!gate) return 1;
    now = GetTickCount();
    if (!last_check || now - last_check >= 250) {
        gate_present = GetFileAttributesA(gate) != INVALID_FILE_ATTRIBUTES;
        last_check = now;
    }
    return gate_present;
}

static int pgraph_test_precise_depth_enabled(void)
{
    const char *enabled = pgraph_cached_getenv("MERCENARIES_TEST_PRECISE_DEPTH");
    const char *gate;
    static DWORD last_check;
    static int gate_present;
    DWORD now;
    if (!enabled) return 0;
    gate = pgraph_cached_getenv("MERCENARIES_TEST_PRECISE_DEPTH_GATE_FILE");
    if (!gate) return 1;
    now = GetTickCount();
    if (!last_check || now - last_check >= 250) {
        gate_present = GetFileAttributesA(gate) != INVALID_FILE_ATTRIBUTES;
        last_check = now;
    }
    return gate_present;
}

/* D3D8 device - we include the full header for COM vtable access */
#include "../d3d/d3d8_internal.h"
#include "../d3d/d3d8_swizzle.h"
#include "../d3d/d3d8_vsh.h"
#include "../d3d/d3d8_combiners.h"
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);
extern volatile uint32_t g_mercenaries_gameplay_capture_active;
extern volatile uint32_t g_mercenaries_standup_complete;
extern volatile uint32_t g_mercenaries_roadblock_model_draw_active;
extern volatile uint32_t g_mercenaries_hq_briefing_active;
extern volatile uint32_t g_recomp_current_func;
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_recomp_recent_funcs[64];
extern volatile uint32_t g_recomp_recent_func_idx;
extern volatile uint32_t g_recomp_recent_game_funcs[256];
extern volatile uint32_t g_recomp_recent_game_func_idx;

/* Global.txd texture lookup */
/* Game-specific texture lookup - only available when GAME_HAS_FONT_ATLAS is defined */
#ifdef GAME_HAS_FONT_ATLAS
typedef struct { char name[24]; IDirect3DTexture8 *texture; uint32_t width, height, format; } TXD_Entry;
typedef struct { TXD_Entry entries[512]; int count; } TXD_Dict;
extern TXD_Dict g_global_txd;
extern int g_textures_loaded;
extern IDirect3DTexture8 *txd_find(const TXD_Dict *dict, const char *name);
#else
static int g_textures_loaded = 0;
#endif

/* Font atlas DXT5 data - game-specific, only available in burnout3 */
#ifdef GAME_HAS_FONT_ATLAS
#include "font_atlas_data.h"
#endif

/* Port-owned UI textures use reserved guest allocations, never a borrowed
 * game texture offset. Pixels remain owned by the caller for process lifetime.
 * Registration and use run on the guest render thread. */
typedef struct ExternalUiTexture {
    uint32_t offset, width, height, levels;
    const uint32_t *pixels;
    IDirect3DTexture8 *texture;
} ExternalUiTexture;
static ExternalUiTexture g_external_ui[1024];
static unsigned g_external_ui_count;
int pgraph_d3d11_register_ui_texture(uint32_t offset,uint32_t width,uint32_t height,const uint32_t *pixels)
{
    ExternalUiTexture *entry;
    unsigned i;
    if(!offset || !pixels || !width || !height || width>1024 || height>1024)return 0;
    for(i=0;i<g_external_ui_count;++i)if(g_external_ui[i].offset==offset)
        return g_external_ui[i].pixels==pixels;
    if(g_external_ui_count>=1024)return 0;
    entry=&g_external_ui[g_external_ui_count++];
    entry->offset=offset;entry->width=width;entry->height=height;entry->pixels=pixels;entry->levels=1;
    return 1;
}
int pgraph_d3d11_register_world_texture(uint32_t offset,uint32_t width,uint32_t height,const uint32_t *pixels)
{
    unsigned levels = 1, n = width > height ? width : height;
    if (!pgraph_d3d11_register_ui_texture(offset,width,height,pixels)) return 0;
    while (n > 1) { ++levels; n >>= 1; }
    for (unsigned i = 0; i < g_external_ui_count; ++i)
        if (g_external_ui[i].offset == offset) g_external_ui[i].levels = levels;
    return 1;
}
static IDirect3DTexture8 *external_ui_texture(IDirect3DDevice8 *dev,uint32_t offset,int *found)
{
    unsigned i,y;
    *found=0;
    for(i=0;i<g_external_ui_count;++i){
        ExternalUiTexture *entry=&g_external_ui[i];D3DLOCKED_RECT lock={0};
        if(entry->offset!=offset)continue;
        *found=1;
        if(!dev)return NULL;
        if(entry->texture)return entry->texture;
        if(dev->lpVtbl->CreateTexture(dev,entry->width,entry->height,entry->levels,0,
            D3DFMT_LIN_A8R8G8B8,0,&entry->texture)!=0 || !entry->texture)return NULL;
        if(entry->texture->lpVtbl->LockRect(entry->texture,0,&lock,NULL,0)!=0 || !lock.pBits){
            entry->texture->lpVtbl->Release(entry->texture);entry->texture=NULL;return NULL;
        }
        for(y=0;y<entry->height;++y)memcpy((uint8_t*)lock.pBits+y*lock.Pitch,
            entry->pixels+y*entry->width,entry->width*4);
        entry->texture->lpVtbl->UnlockRect(entry->texture,0);
        if (entry->levels > 1) {
            uint32_t w = entry->width, h = entry->height;
            uint32_t *mip = malloc((size_t)w * h * 4);
            if (!mip) { entry->texture->lpVtbl->Release(entry->texture); entry->texture=NULL; return NULL; }
            memcpy(mip, entry->pixels, (size_t)w * h * 4);
            for (unsigned level = 1; level < entry->levels; ++level) {
                uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
                for (uint32_t yy = 0; yy < nh; ++yy) for (uint32_t x = 0; x < nw; ++x) {
                    uint32_t result = 0;
                    for (unsigned shift = 0; shift < 32; shift += 8) {
                        unsigned sum = 0;
                        for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
                            uint32_t sx = x * 2 + dx, sy = yy * 2 + dy;
                            if (sx >= w) sx = w - 1;
                            if (sy >= h) sy = h - 1;
                            sum += (mip[sy * w + sx] >> shift) & 255;
                        }
                        result |= ((sum + 2) / 4) << shift;
                    }
                    mip[yy * nw + x] = result;
                }
                w = nw; h = nh;
                memset(&lock, 0, sizeof(lock));
                if (entry->texture->lpVtbl->LockRect(entry->texture,level,&lock,NULL,0)!=0 || !lock.pBits) {
                    free(mip); entry->texture->lpVtbl->Release(entry->texture); entry->texture=NULL; return NULL;
                }
                for (uint32_t yy = 0; yy < h; ++yy)
                    memcpy((uint8_t *)lock.pBits + yy * lock.Pitch, mip + yy * w, w * 4);
                entry->texture->lpVtbl->UnlockRect(entry->texture,level);
            }
            free(mip);
        }
        return entry->texture;
    }
    return NULL;
}

/* Create a D3D8 texture from raw DXT5 data */
static IDirect3DTexture8 *create_dxt5_texture(IDirect3DDevice8 *dev,
    uint32_t width, uint32_t height, const void *dxt5_data, uint32_t data_size)
{
    IDirect3DTexture8 *tex = NULL;
    /* D3DFMT_DXT5 = 0x35545844 ('DXT5') on Xbox, mapped to DXGI_FORMAT_BC3 in our layer.
     * Our d3d8 layer uses format code 0x0F for DXT5. */
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, width, height, 1,
        0 /*Usage*/, 0x0F /*DXT5*/, 0 /*D3DPOOL_DEFAULT*/, &tex);
    if (hr != 0 || !tex) {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to create font atlas texture: hr=0x%08X\n", hr);
        return NULL;
    }

    /* Lock and fill with DXT5 data */
    D3DLOCKED_RECT lr = {0};
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (hr == 0 && lr.pBits) {
        memcpy(lr.pBits, dxt5_data, data_size);
        tex->lpVtbl->UnlockRect(tex, 0);
        fprintf(stderr, "[PGRAPH-D3D11] Created font atlas: %ux%u DXT5 (%u bytes)\n",
                width, height, data_size);
    } else {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to lock font atlas: hr=0x%08X\n", hr);
    }
    return tex;
}

/* ======================================================================
 * NV2A method constants (from nv2a_regs.h, subset for translator)
 * ====================================================================== */

#ifndef NV097_SET_BEGIN_END
#define NV097_SET_BEGIN_END             0x17FC
#define NV097_INLINE_ARRAY              0x1818
#define NV097_CLEAR_SURFACE             0x01D0
#define NV097_SET_COLOR_CLEAR_VALUE     0x01D4
#define NV097_SET_CLEAR_RECT_HORIZONTAL 0x01D8
#define NV097_SET_CLEAR_RECT_VERTICAL   0x01DC

#define NV097_SET_DEPTH_TEST_ENABLE     0x0354
#define NV097_SET_BLEND_ENABLE          0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR    0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR    0x0348
#define NV097_SET_CULL_FACE_ENABLE      0x0308
#define NV097_SET_CULL_FACE             0x039C
#define NV097_SET_FRONT_FACE            0x03A0
#define NV097_SET_ALPHA_TEST_ENABLE     0x0300
#define NV097_SET_COLOR_MASK            0x0358
#define NV097_SET_SHADE_MODE            0x0368

#define NV097_SET_VIEWPORT_OFFSET       0x0A20
#define NV097_SET_VIEWPORT_SCALE        0x0AF0
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL 0x0204

#define NV097_SET_TEXTURE_OFFSET        0x1B00  /* +0x40 per stage */
#define NV097_SET_TEXTURE_FORMAT        0x1B04  /* +0x40 per stage */
#define NV097_SET_TEXTURE_CONTROL0      0x1B0C  /* +0x40 per stage */
#define NV097_SET_TEXTURE_CONTROL1      0x1B10  /* +0x40 per stage */
#define NV097_SET_TEXTURE_IMAGE_RECT    0x1B1C  /* +0x40 per stage */
#endif

/* NV2A draw modes -> D3D primitive types */
static int nv2a_draw_mode_to_d3d(uint32_t mode) {
    switch (mode) {
        case 1:  return D3DPT_POINTLIST;
        case 2:  return D3DPT_LINELIST;
        case 3:  return D3DPT_LINESTRIP;  /* LINE_LOOP -> LINE_STRIP */
        case 4:  return D3DPT_LINESTRIP;
        case 5:  return D3DPT_TRIANGLELIST;
        case 6:  return D3DPT_TRIANGLESTRIP;
        case 7:  return D3DPT_TRIANGLEFAN;
        case 8:  return D3DPT_TRIANGLELIST; /* QUADS -> TRI_LIST (needs conversion) */
        /* In fill mode an Xbox quad strip has the same vertex topology as a
         * host triangle strip: (0,1,2), (2,1,3), then the next vertex pair.
         * Treating it as the default triangle list drops every third vertex
         * into an unrelated triangle and can tear large terrain/effect strips. */
        case 9:  return D3DPT_TRIANGLESTRIP;
        /* Xbox POLYGON fill is a fan around vertex zero.  The old default
         * triangle-list mapping joined unrelated perimeter vertices, producing
         * the nested full-screen geometry seen by satellite targeting effects. */
        case 10: return D3DPT_TRIANGLEFAN;
        default: return D3DPT_TRIANGLELIST;
    }
}

/* NV2A blend factors -> D3D blend */
static uint32_t nv2a_blend_to_d3d(uint32_t nv) {
    switch (nv) {
        case NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO:
            return D3DBLEND_ZERO;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE:
            return D3DBLEND_ONE;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_COLOR:
            return D3DBLEND_SRCCOLOR;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_SRC_COLOR:
            return D3DBLEND_INVSRCCOLOR;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA:
            return D3DBLEND_SRCALPHA;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_SRC_ALPHA:
            return D3DBLEND_INVSRCALPHA;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_DST_ALPHA:
            return D3DBLEND_DESTALPHA;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_ALPHA:
            return D3DBLEND_INVDESTALPHA;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_DST_COLOR:
            return D3DBLEND_DESTCOLOR;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_COLOR:
            return D3DBLEND_INVDESTCOLOR;
        case NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA_SATURATE:
            return D3DBLEND_SRCALPHASAT;
        default:
            return D3DBLEND_ONE;
    }
}

/* NV2A blend equations map directly to the five D3D8/D3D11 operations.
 * The two signed Xbox extensions do not have native D3D11 equivalents;
 * xemu's host backends approximate them with the corresponding unsigned
 * reverse-subtract/add operation as well. */
static uint32_t nv2a_blend_equation_to_d3d(uint32_t nv)
{
    switch (nv) {
    case NV097_SET_BLEND_EQUATION_V_FUNC_SUBTRACT:
        return 2u /* D3DBLENDOP_SUBTRACT */;
    case NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT:
    case NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT_SIGNED:
        return 3u /* D3DBLENDOP_REVSUBTRACT */;
    case NV097_SET_BLEND_EQUATION_V_MIN:
        return 4u /* D3DBLENDOP_MIN */;
    case NV097_SET_BLEND_EQUATION_V_MAX:
        return 5u /* D3DBLENDOP_MAX */;
    case NV097_SET_BLEND_EQUATION_V_FUNC_ADD_SIGNED:
    case NV097_SET_BLEND_EQUATION_V_FUNC_ADD:
    default:
        return 1u /* D3DBLENDOP_ADD */;
    }
}

static uint32_t nv2a_texture_address_to_d3d(uint32_t nv)
{
    switch (nv) {
    case 1u: return D3DTADDRESS_WRAP;
    case 2u: return D3DTADDRESS_MIRROR;
    case 3u: return D3DTADDRESS_CLAMP;
    case 4u: return D3DTADDRESS_BORDER;
    /* NV2A mode 5 is OpenGL-style clamp, not D3D MIRRORONCE. Clamp to
     * edge is the closest D3D11 representation until border-half-texel
     * emulation is added. */
    case 5u: return D3DTADDRESS_CLAMP;
    default: return D3DTADDRESS_WRAP;
    }
}

static void apply_nv2a_texture_filter(IDirect3DDevice8 *dev, uint32_t stage,
                                      uint32_t filter, uint32_t control0,
                                      int linear)
{
    uint32_t min_filter = (filter >> 16) & 0x3Fu;
    const uint32_t mag_filter = (filter >> 24) & 0xFu;
    uint32_t host_min;
    uint32_t host_mip;
    uint32_t max_anisotropy =
        1u << ((control0 & NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY) >> 4);
    int32_t signed_bias = (int32_t)(filter & 0x1FFFu);
    float lod_bias;
    uint32_t lod_bias_bits;

    /* Report each requested convolution kernel once per stage, only in a
     * trace run. This establishes whether an edge-quality discrepancy uses
     * guest convolution filtering before changing ordinary sampling. */
    if (min_filter == 7u &&
        pgraph_cached_getenv("MERCENARIES_TRACE_TEXTURE_PERF")) {
        static uint32_t reported[4];
        uint32_t kernel = (filter >> 13) & 7u;
        if (stage < 4u && !(reported[stage] & (1u << kernel))) {
            reported[stage] |= 1u << kernel;
            fprintf(stderr, "[convolution-request] stage=%u kernel=%u filter=%08X control0=%08X linear=%d\n",
                    stage, kernel, filter, control0, linear);
        }
    }

    if (linear) {
        if (min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
            min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD)
            min_filter = NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0;
        else if (min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD ||
                 min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD)
            min_filter = NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0;
    }

    host_min = (min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
                min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
                min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD) ?
        D3DTEXF_POINT : D3DTEXF_LINEAR;
    if (min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD)
        host_mip = D3DTEXF_POINT;
    else if (min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD ||
             min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD)
        host_mip = D3DTEXF_LINEAR;
    else
        host_mip = D3DTEXF_NONE;

    /* Xemu forwards TEXCTL0.MAX_ANISOTROPY to the host sampler. D3D11
     * expresses anisotropy in the filter enum as well as MaxAnisotropy, so
     * promote linear minification when the guest requests more than 1x.
     * Leave point-filtered sprites point sampled. */
    if (max_anisotropy > 1u && host_min == D3DTEXF_LINEAR)
        host_min = D3DTEXF_ANISOTROPIC;

    if (signed_bias & 0x1000)
        signed_bias |= ~0x1FFF;
    lod_bias = (float)signed_bias / 256.0f;
    memcpy(&lod_bias_bits, &lod_bias, sizeof(lod_bias_bits));

    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MAGFILTER,
        mag_filter == 1u ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MINFILTER, host_min);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MIPFILTER, host_mip);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MIPMAPLODBIAS,
                                      lod_bias_bits);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MAXANISOTROPY,
                                      max_anisotropy);
    dev->lpVtbl->SetTextureStageState(
        dev, stage, D3DTSS_MAXMIPLEVEL,
        linear ? 0u :
            ((control0 & NV097_SET_TEXTURE_CONTROL0_MIN_LOD_CLAMP) >> 18));
}
static uint32_t nv2a_cull_to_d3d(int enabled, uint32_t cull_face,
                                  uint32_t front_face)
{
    int cull_ccw;

    if (!enabled || cull_face == 0x0408u)
        return D3DCULL_NONE;

    /* D3DCULL_* names the winding to discard.  The Xbox title selects
     * a front winding separately from whether it culls front or back. */
    cull_ccw = (front_face == 0x0901u);
    if (cull_face == 0x0405u)
        cull_ccw = !cull_ccw;
    return cull_ccw ? D3DCULL_CCW : D3DCULL_CW;
}

static uint32_t nv2a_stencil_op_to_d3d(uint32_t nv)
{
    switch (nv) {
    case NV097_SET_STENCIL_OP_V_KEEP:    return 1u;
    case NV097_SET_STENCIL_OP_V_ZERO:    return 2u;
    case NV097_SET_STENCIL_OP_V_REPLACE: return 3u;
    case NV097_SET_STENCIL_OP_V_INCRSAT: return 4u;
    case NV097_SET_STENCIL_OP_V_DECRSAT: return 5u;
    case NV097_SET_STENCIL_OP_V_INVERT:  return 6u;
    case NV097_SET_STENCIL_OP_V_INCR:    return 7u;
    case NV097_SET_STENCIL_OP_V_DECR:    return 8u;
    default:                             return 1u;
    }
}

/* ======================================================================
 * Translator State
 * ====================================================================== */

/* Inline vertex buffer - max 16K vertices per draw */
#define MAX_INLINE_VERTS 16384
#define MAX_INLINE_ELEMENTS (MAX_INLINE_VERTS * 4)
#define INLINE_VERT_DWORDS 11 /* X,Y; four UV pairs; Color */
#define MAX_GUEST_COLOR_SURFACES 32
#define MAX_GUEST_DEPTH_SURFACES 16
#define MAX_GUEST_TEXTURE_CACHE 256

/* RwIm2DVertex-compatible output vertex (28 bytes) */
typedef struct {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} OutputVertex;

typedef struct {
    float x, y, z, rhw;
    uint32_t color;
    float u0, v0;
    float u1, v1;
    float u2, v2;
    float u3, v3;
} ImmediateOutputVertex;

typedef struct {
    uint32_t offset;
    uint32_t pitch;
    uint32_t format;
    uint32_t anti_aliasing;
    uint32_t width;
    uint32_t height;
    uint32_t logical_width;
    uint32_t logical_height;
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    int valid;
    int drawn;
    int gpu_drawn;
    uint32_t draw_generation;
    uint32_t created_draw;
    uint64_t write_serial;
    uint64_t downloaded_serial;
    uint32_t depth_alias_source_offset;
    uint64_t depth_alias_source_serial;
    uint32_t resolve_source_width;
    uint32_t resolve_source_height;
    uint32_t resolve_source_pitch;
} GuestColorSurface;

typedef struct {
    uint32_t offset;
    uint32_t pitch;
    uint32_t format;
    uint32_t anti_aliasing;
    uint32_t width;
    uint32_t height;
    uint32_t logical_width;
    uint32_t logical_height;
    ID3D11Texture2D *texture;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *depth_srv;
    ID3D11ShaderResourceView *stencil_srv;
    int valid;
    int uniform_stencil_known;
    uint8_t uniform_stencil_value;
    uint8_t possible_stencil_bits;
    uint64_t color_alias_serial;
    uint64_t write_serial;
    uint64_t downloaded_serial;
} GuestDepthSurface;

typedef struct {
    IDirect3DTexture8 *texture;
    uint32_t offset;
    uint32_t format;
    uint32_t rect;
    uint32_t pitch;
    uint64_t source_hash;
    uint64_t last_use;
    uint32_t last_validation_generation;
    int source_hash_valid;
} GuestTextureCacheEntry;

static struct {
    /* Draw state */
    int in_draw;           /* Between BEGIN and END */
    uint32_t draw_mode;    /* NV2A draw mode (0=end, 6=tristrip, etc.) */
    int d3d_prim_type;     /* Translated D3D prim type */

    /* Inline vertex accumulator */
    uint32_t inline_data[MAX_INLINE_VERTS * INLINE_VERT_DWORDS];
    uint32_t inline_count; /* Number of dwords accumulated */
    int inline_array_mode;
    uint32_t inline_array_vertex_size;
    uint32_t inline_array_offsets[16];
    uint32_t vert_stride;  /* Dwords per vertex (auto-detected) */

    /* DRAW_ARRAYS state. xboxkrnl's D3D driver commonly keeps the stream
     * binding in its CPU-side cache and emits only BEGIN/DRAW_ARRAYS/END. */
    uint32_t draw_array_start;
    uint32_t draw_array_count;
    uint32_t inline_elements[MAX_INLINE_ELEMENTS];
    uint32_t inline_element_count;
    uint32_t guest_fvf;
    struct {
        uint32_t physical_offset;
        uint32_t stride;
    } stream[16];
    struct {
        uint32_t offset;
        uint32_t format;
        uint32_t dma_select;
    } vertex_array[16];
    uint32_t vertex_dma_a;
    uint32_t vertex_dma_a_base;
    uint32_t vertex_dma_b;
    uint32_t vertex_dma_b_base;
    uint32_t texture_dma_a;
    uint32_t texture_dma_a_base;
    uint32_t texture_dma_b;
    uint32_t texture_dma_b_base;

    /* Clear state */
    uint32_t clear_color;
    uint32_t clear_rect_h;  /* (width << 16) | x */
    uint32_t clear_rect_v;  /* (height << 16) | y */

    /* Render state cache */
    int depth_test;
    uint32_t depth_func;
    int depth_write;
    int blend_enable;
    uint32_t blend_sfactor;
    uint32_t blend_dfactor;
    uint32_t blend_equation;
    int cull_enable;
    uint32_t cull_face;
    uint32_t front_face;
    int alpha_test;
    uint32_t alpha_func;
    uint32_t alpha_ref;
    int fog_enable;
    uint32_t fog_color;
    int stencil_enable;
    int stencil_write_enable;
    uint32_t stencil_func;
    uint32_t stencil_ref;
    uint32_t stencil_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_fail;
    uint32_t stencil_zfail;
    uint32_t stencil_zpass;
    uint32_t control0;
    uint32_t window_clip_type;
    uint32_t window_clip_h[8];
    uint32_t window_clip_v[8];
    int window_clip_h_valid;
    int window_clip_v_valid;
    uint32_t point_params_enable;
    uint32_t point_smooth_enable;
    uint32_t line_smooth_enable;
    uint32_t polygon_offset_point_enable;
    uint32_t polygon_offset_line_enable;
    uint32_t polygon_offset_fill_enable;
    uint32_t polygon_offset_bias;
    uint32_t polygon_offset_factor;
    uint32_t front_polygon_mode;
    uint32_t dither_enable;
    uint32_t lighting_enable;
    uint32_t light_control;
    uint32_t fog_mode;
    uint32_t fog_gen_mode;
    uint32_t point_size;
    uint32_t point_params[8];
    uint32_t provoking_vertex;
    uint32_t eye_vector[3];
    uint32_t color_mask;
    uint32_t combiner_factor0[8];
    uint32_t combiner_factor1[8];
    uint32_t combiner_alpha_icw[8];
    uint32_t combiner_alpha_ocw[8];
    uint32_t combiner_color_icw[8];
    uint32_t combiner_color_ocw[8];
    uint32_t combiner_control;
    uint32_t combiner_final_inputs_0;
    uint32_t combiner_final_inputs_1;
    uint32_t combiner_final_factor[2];
    uint32_t shader_stage_program;
    uint32_t shader_dot_mapping;
    uint32_t shader_other_stage_input;
    int combiner_valid;
    uint32_t immediate_vertex_4ub[16];
    float immediate_vertex_attr[16][4];
    float immediate_vertices[MAX_INLINE_VERTS][NV2A_VS_MAX_INPUTS][4];
    uint32_t immediate_vertex_count;
    int immediate_buffer_mode;
    uint32_t immediate_texcoord_base;

    /* NV097 transform constants. xemu models SET_TRANSFORM_CONSTANT as an
     * eight-vector incrementing range selected by SET_TRANSFORM_CONSTANT_LOAD. */
    uint32_t transform_constant_load;
    struct {
        uint32_t serial;
        uint32_t draw;
        uint32_t method;
        uint32_t parameter;
        uint32_t load_before;
        uint32_t constant;
        uint32_t component;
        uint32_t is_load;
    } transform_constant_history[64];
    uint32_t transform_constant_history_serial;
    uint32_t vsh_constants[NV2A_VERTEXSHADER_CONSTANTS][4];
    uint8_t vsh_constants_dirty[NV2A_VERTEXSHADER_CONSTANTS];
    uint32_t transform_program_load;
    uint32_t transform_program_start;
    uint32_t transform_execution_mode;
    uint32_t clip_min;
    uint32_t clip_max;
    uint32_t zmin_max_control;
    uint32_t transform_program[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][4];
    DWORD host_vsh_handle;
    uint32_t host_vsh_hash;
    uint32_t transform_program_hash;
    uint32_t transform_program_length;
    int transform_program_key_valid;

    /* Viewport */
    float vp_offset[4];
    float vp_scale[4];
    uint32_t surface_clip_h;
    uint32_t surface_clip_v;

    /* Linear render target selected by the Xbox push buffer. */
    uint32_t surface_format;
    uint32_t surface_pitch;
    uint32_t surface_dma_color;
    uint32_t surface_dma_color_base;
    uint32_t surface_dma_zeta;
    uint32_t surface_dma_zeta_base;
    uint32_t surface_color_offset;
    uint32_t surface_zeta_offset;
    uint32_t zstencil_clear;
    uint32_t host_zeta_offset;
    int host_zeta_valid;
    int frame_had_draw;
    GuestColorSurface color_surfaces[MAX_GUEST_COLOR_SURFACES];
    GuestDepthSurface depth_surfaces[MAX_GUEST_DEPTH_SURFACES];
    GuestColorSurface *bound_color;
    GuestColorSurface *last_drawn_color;
    GuestColorSurface *last_resolved_color;
    GuestDepthSurface *bound_depth;
    ID3D11Texture2D *satellite_depth_sample_texture;
    ID3D11RenderTargetView *satellite_depth_sample_rtv;
    ID3D11ShaderResourceView *satellite_depth_sample_srv;
    uint32_t satellite_depth_sample_width;
    uint32_t satellite_depth_sample_height;
    uint32_t surface_generation;
    uint64_t surface_write_serial;
    uint32_t internal_width;
    uint32_t internal_height;
    uint32_t flip_read;
    uint32_t flip_write;
    uint32_t flip_modulo;
    int deferred_flip;
    GuestColorSurface *pending_scanout;
    ULONGLONG pending_scanout_write_us;

    /* Texture state per stage (4 stages) */
    struct {
        uint32_t offset;     /* NV2A VRAM offset (method 0x1B00) */
        uint32_t format;     /* Format register (method 0x1B04) */
        uint32_t address;
        uint32_t control0;
        uint32_t control1;
        uint32_t filter;
        uint32_t image_rect;
        uint32_t border_color; /* NV_PGRAPH_BORDERCOLORn / ARGB */
        int enabled;         /* Decoded from control0 bit 30 */
    } tex[4];
    uint32_t color_key[4];   /* NV097_SET_COLOR_KEY_COLOR stage values */

    /* Cached texture pointers */
    void *menu_texture;           /* IDirect3DTexture8* from Global.txd */
    IDirect3DTexture8 *font_atlas; /* Created from captured DXT5 data */
    IDirect3DTexture8 *guest_texture[4];
    uint32_t guest_texture_offset[4];
    uint32_t guest_texture_format[4];
    uint32_t guest_texture_rect[4];
    uint32_t guest_texture_pitch[4];
    uint64_t guest_texture_source_hash[4];
    int guest_texture_source_hash_valid[4];
    GuestTextureCacheEntry texture_cache[MAX_GUEST_TEXTURE_CACHE];
    uint64_t texture_cache_use_serial;
    int texture_lookup_done;

    /* Stats */
    PgraphD3D11Stats stats;

    /* Chyron scroll */
    float chyron_scroll_offset;  /* Pixels to shift X for chyron text */

    /* Init flag */
    int initialized;
} g_pg;

static uint32_t resolved_vertex_array_offset(uint32_t slot)
{
    const uint32_t dma_base = g_pg.vertex_array[slot].dma_select ?
        g_pg.vertex_dma_b_base : g_pg.vertex_dma_a_base;
    return (dma_base + g_pg.vertex_array[slot].offset) & 0x07FFFFFFu;
}

static uint32_t resolved_texture_offset(uint32_t stage)
{
    const uint32_t dma_base =
        (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA) ?
        g_pg.texture_dma_b_base : g_pg.texture_dma_a_base;
    return (dma_base + g_pg.tex[stage].offset) & 0x07FFFFFFu;
}

static GuestTextureCacheEntry *find_guest_texture_cache_entry(
    uint32_t offset, uint32_t format, uint32_t rect, uint32_t pitch)
{
    uint32_t index;
    for (index = 0u; index < MAX_GUEST_TEXTURE_CACHE; ++index) {
        GuestTextureCacheEntry *entry = &g_pg.texture_cache[index];
        if (entry->texture && entry->offset == offset &&
            entry->format == format && entry->rect == rect &&
            entry->pitch == pitch) {
            entry->last_use = ++g_pg.texture_cache_use_serial;
            return entry;
        }
    }
    return NULL;
}

static GuestTextureCacheEntry *allocate_guest_texture_cache_entry(
    uint32_t offset, uint32_t format, uint32_t rect, uint32_t pitch)
{
    GuestTextureCacheEntry *entry = NULL;
    uint32_t index;
    for (index = 0u; index < MAX_GUEST_TEXTURE_CACHE; ++index) {
        GuestTextureCacheEntry *candidate = &g_pg.texture_cache[index];
        if (!candidate->texture) {
            entry = candidate;
            break;
        }
        if (!entry || candidate->last_use < entry->last_use)
            entry = candidate;
    }
    if (!entry)
        return NULL;
    if (entry->texture)
        entry->texture->lpVtbl->Release(entry->texture);
    memset(entry, 0, sizeof(*entry));
    entry->offset = offset;
    entry->format = format;
    entry->rect = rect;
    entry->pitch = pitch;
    entry->last_use = ++g_pg.texture_cache_use_serial;
    return entry;
}

static uint32_t g_captured_resolve_pairs;
static uint32_t g_debug_gameplay_clear_index;
static int g_debug_captured_depth_pass;
static uint32_t g_debug_gameplay_array_index;
static int g_debug_gameplay_array_series_active;
static uint32_t g_debug_gameplay_draw_index;
static int g_debug_gameplay_draw_series_active;
static int g_debug_gameplay_draw_after_pending;
static uint32_t g_debug_gameplay_draw_after_index;
static char g_debug_gameplay_draw_after_path[MAX_PATH];
static uint32_t g_debug_humvee_transform_samples;
static int g_debug_humvee_repair_capture_pending;
static int g_debug_post_pda_resolve_trace_active;
static uint32_t g_debug_post_pda_resolve_trace_index;
static uint32_t g_debug_post_pda_present_trace_index;
static uint32_t g_debug_post_pda_scanout_trace_index;
static uint32_t g_debug_post_pda_flip_trace_index;
static uint32_t g_debug_post_pda_pending_write_index;
static uint32_t g_debug_surface_method_trace_count;

/* Optional one-shot arm for the existing per-draw capture series before
 * gameplay (for example, a particular menu-camera angle). Once armed, keep
 * counting clears so removing the file cannot leave one frame's series active
 * forever. This affects diagnostic selection only, never game/camera state. */
static int debug_draw_series_phase_active(void)
{
    static int initialized, armed, require_gate = -1;
    static const char *path;
    DWORD attributes;
    if (require_gate < 0)
        require_gate = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_DRAW_GATE_REQUIRED") != NULL;
    if (!require_gate && g_mercenaries_gameplay_capture_active != 0u)
        return 1;
    if (!initialized) {
        initialized = 1;
        path = pgraph_cached_getenv("MERCENARIES_CAPTURE_DRAW_GATE_FILE");
    }
    if (armed)
        return 1;
    if (!path || !*path)
        return 0;
    attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        armed = 1;
    return armed;
}

static void debug_capture_post_pda_pending_write(const char *kind)
{
    const char *prefix;
    char path[MAX_PATH];
    uint32_t index;

    if (!g_debug_post_pda_resolve_trace_active || !g_pg.pending_scanout ||
        g_pg.bound_color != g_pg.pending_scanout)
        return;
    prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_POST_PDA_PENDING_PREFIX");
    if (!prefix || !*prefix || g_debug_post_pda_pending_write_index >= 64u)
        return;
    index = g_debug_post_pda_pending_write_index++;
    snprintf(path, sizeof(path), "%s-%02u-%s.bmp", prefix, index,
             kind ? kind : "draw");
    fprintf(stderr,
            "[PGRAPH-POST-PDA-PENDING-WRITE] index=%u kind=%s draw=%u "
            "surface=%08X write=%llu prim=%d mode=%u vsh=%08X "
            "blend=%d/%04X/%04X depth=%d/%d/%u alpha=%d/%u "
            "tex0=%08X/%08X path=%s\n",
            index, kind ? kind : "draw", g_pg.stats.draw_calls + 1u,
            g_pg.bound_color->offset,
            (unsigned long long)g_pg.bound_color->write_serial,
            g_pg.d3d_prim_type, g_pg.draw_mode, g_pg.host_vsh_hash,
            g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
            g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
            g_pg.alpha_test, g_pg.alpha_func, g_pg.tex[0].offset,
            resolved_texture_offset(0), path);
    d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
}

void pgraph_d3d11_reset_gameplay_capture_series(void)
{
    g_debug_gameplay_clear_index = 0u;
    g_debug_captured_depth_pass = 0;
    g_debug_gameplay_array_index = 0u;
    g_debug_gameplay_draw_index = 0u;
    g_debug_gameplay_array_series_active = 0;
    g_debug_gameplay_draw_series_active = 0;
    g_debug_gameplay_draw_after_pending = 0;
    g_debug_humvee_transform_samples = 0u;
    g_debug_post_pda_resolve_trace_active =
        pgraph_cached_getenv("MERCENARIES_TRACE_POST_PDA_RESOLVE") != NULL;
    g_debug_post_pda_resolve_trace_index = 0u;
    g_debug_post_pda_present_trace_index = 0u;
    g_debug_post_pda_scanout_trace_index = 0u;
    g_debug_post_pda_flip_trace_index = 0u;
    g_debug_post_pda_pending_write_index = 0u;
    fprintf(stderr, "[PGRAPH-GAMEPLAY-CAPTURE-RESET]\n");
    fflush(stderr);
}
static uint32_t g_debug_depth_method_draw[8];
static uint32_t g_debug_depth_method_value[8];
static uint32_t g_debug_depth_method_serial;

static int pgraph_trace_depth_window_enabled(void)
{
    const char *minimum_env;
    const char *maximum_env;
    uint32_t minimum;
    uint32_t maximum;
    if (pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_METHODS") == NULL)
        return 0;
    minimum_env = pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_MIN");
    maximum_env = pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_MAX");
    minimum = minimum_env ? (uint32_t)strtoul(minimum_env, NULL, 10) : 0u;
    maximum = maximum_env ? (uint32_t)strtoul(maximum_env, NULL, 10) : UINT32_MAX;
    return g_pg.stats.draw_calls >= minimum && g_pg.stats.draw_calls < maximum;
}

static int pgraph_trace_draws_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = pgraph_cached_getenv("MERCENARIES_TRACE_PGRAPH_DRAWS");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return enabled;
}

static int pgraph_trace_textures_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = pgraph_cached_getenv("MERCENARIES_TRACE_PGRAPH_TEXTURES");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return enabled;
}
static int pgraph_trace_movie_texture_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = pgraph_cached_getenv(
            "MERCENARIES_TRACE_MOVIE_PGRAPH");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return enabled;
}

static int pgraph_texture_format_is_packed_yuv(uint32_t format)
{
    const uint32_t color =
        (format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    return color ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
           color ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8;
}

static void pgraph_trace_movie_texture_method(const char *name,
                                              int stage,
                                              uint32_t param)
{
    static uint32_t trace_count;
    if (!pgraph_trace_movie_texture_enabled() || stage != 0 ||
        trace_count >= 256u)
        return;
    if (strcmp(name, "format") != 0 &&
        !pgraph_texture_format_is_packed_yuv(g_pg.tex[0].format))
        return;
    if (strcmp(name, "format") == 0 &&
        !pgraph_texture_format_is_packed_yuv(param))
        return;
    fprintf(stderr,
            "[PGRAPH-MOVIE-METHOD] sample=%u draw=%llu name=%s "
            "param=%08X off=%08X fmt=%08X ctl0=%08X ctl1=%08X "
            "rect=%08X enabled=%d\n",
            ++trace_count, (unsigned long long)g_pg.stats.draw_calls, name,
            param, g_pg.tex[0].offset, g_pg.tex[0].format,
            g_pg.tex[0].control0, g_pg.tex[0].control1,
            g_pg.tex[0].image_rect, g_pg.tex[0].enabled);
}

static int pgraph_trace_present_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = pgraph_cached_getenv("MERCENARIES_TRACE_PGRAPH_PRESENT");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return enabled;
}

static int pgraph_trace_surface_textures_enabled(void)
{
    static int enabled = -1;
    static ULONGLONG trace_start_ms;
    const char *delay_text;
    ULONGLONG delay_ms;

    if (trace_start_ms == 0u)
        trace_start_ms = GetTickCount64();
    if (enabled < 0) {
        const char *value = pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_TEXTURES");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    if (!enabled)
        return 0;
    delay_text = pgraph_cached_getenv(
        "MERCENARIES_TRACE_SURFACE_DELAY_MS");
    delay_ms = delay_text && delay_text[0] != '\0' ?
        strtoull(delay_text, NULL, 0) : 0u;
    return GetTickCount64() - trace_start_ms >= delay_ms;
}
/* ======================================================================
 * Float/uint32 conversion
 * ====================================================================== */
static float u2f(uint32_t u) {
    union { float f; uint32_t i; } x;
    x.i = u;
    return x.f;
}

static uint32_t f2u(float f) {
    union { float f; uint32_t i; } x;
    x.f = f;
    return x.i;
}

static void apply_guest_window_clip(void)
{
    uint32_t xmin, xmax, ymin, ymax;
    const uint32_t surface_x = g_pg.surface_clip_h & 0xFFFFu;
    const uint32_t surface_y = g_pg.surface_clip_v & 0xFFFFu;
    const uint32_t surface_width = g_pg.surface_clip_h >> 16;
    const uint32_t surface_height = g_pg.surface_clip_v >> 16;
    uint32_t logical_width = 0u, logical_height = 0u;
    uint32_t physical_width = 0u, physical_height = 0u;

    if (pgraph_cached_getenv("MERCENARIES_DEBUG_DISABLE_WINDOW_CLIP") != NULL) {
        d3d8_states_set_scissor(FALSE, 0, 0, 0, 0);
        return;
    }

    /* The NV2A surface clip is a draw-time raster clip, not merely a hint
     * about render-target dimensions. Xemu applies it as the host scissor
     * independently of the window-clip regions. Xbox D3D8 SetScissors uses
     * SET_SURFACE_CLIP for its requested rectangle and leaves window clip 0
     * at the full target, so ignoring this state makes scoped canvas effects
     * (for example the PDA scanlines) escape their authored viewport. */
    if (surface_width == 0u || surface_height == 0u) {
        d3d8_states_set_scissor(FALSE, 0, 0, 0, 0);
        return;
    }

    xmin = surface_x;
    xmax = surface_x + surface_width;
    ymin = surface_y;
    ymax = surface_y + surface_height;

    /* A D3D11 scissor can exactly represent the intersection of the surface
     * clip and the common inclusive slot-0 window clip. Multiple inclusive
     * regions and exclusive window clips still require shader-side discard;
     * retain the mandatory surface clip in those cases. */
    if (g_pg.window_clip_h_valid && g_pg.window_clip_v_valid &&
        g_pg.window_clip_type == 0u) {
        const uint32_t window_xmin = g_pg.window_clip_h[0] & 0x0FFFu;
        const uint32_t window_xmax =
            ((g_pg.window_clip_h[0] >> 16) & 0x0FFFu) + 1u;
        const uint32_t window_ymin = g_pg.window_clip_v[0] & 0x0FFFu;
        const uint32_t window_ymax =
            ((g_pg.window_clip_v[0] >> 16) & 0x0FFFu) + 1u;
        if (window_xmin > xmin) xmin = window_xmin;
        if (window_xmax < xmax) xmax = window_xmax;
        if (window_ymin > ymin) ymin = window_ymin;
        if (window_ymax < ymax) ymax = window_ymax;
    }
    if (xmax < xmin) xmax = xmin;
    if (ymax < ymin) ymax = ymin;
    if (g_pg.bound_color) {
        logical_width = g_pg.bound_color->logical_width;
        logical_height = g_pg.bound_color->logical_height;
        physical_width = g_pg.bound_color->width;
        physical_height = g_pg.bound_color->height;
    }
    if (!logical_width || !logical_height ||
        !physical_width || !physical_height) {
        logical_width = physical_width = 1u;
        logical_height = physical_height = 1u;
    }
    d3d8_states_set_scissor(
        TRUE,
        (LONG)((uint64_t)xmin * physical_width / logical_width),
        (LONG)((uint64_t)ymin * physical_height / logical_height),
        (LONG)(((uint64_t)xmax * physical_width + logical_width - 1u) /
               logical_width),
        (LONG)(((uint64_t)ymax * physical_height + logical_height - 1u) /
               logical_height));
}

static void get_guest_surface_dimensions(uint32_t *logical_width, uint32_t *logical_height,
                                         uint32_t *physical_width, uint32_t *physical_height);

static void apply_guest_polygon_offset(void)
{
    uint32_t lw, lh, pw, ph;
    float bias, factor;
    int enabled = 0;
    if (g_pg.draw_mode >= 5u) {
        switch (g_pg.front_polygon_mode) {
        case NV097_SET_FRONT_POLYGON_MODE_V_POINT:
            enabled = g_pg.polygon_offset_point_enable != 0u;
            break;
        case NV097_SET_FRONT_POLYGON_MODE_V_LINE:
            enabled = g_pg.polygon_offset_line_enable != 0u;
            break;
        case 0u: /* Reset/default polygon mode is fill. */
        case NV097_SET_FRONT_POLYGON_MODE_V_FILL:
            enabled = g_pg.polygon_offset_fill_enable != 0u;
            break;
        }
    }
    memcpy(&bias, &g_pg.polygon_offset_bias, sizeof(bias));
    memcpy(&factor, &g_pg.polygon_offset_factor, sizeof(factor));
    if (!isfinite(bias) || !isfinite(factor)) enabled = 0;
    enabled = enabled && (bias != 0.0f || factor != 0.0f);
    get_guest_surface_dimensions(&lw, &lh, &pw, &ph);
    /* NV2A bias is measured in integer guest Z units. Host depth is [0,1]. */
    const uint32_t zeta = (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ZETA) >> 4;
    const float zmax = zeta == 1u ? 65535.0f : 16777215.0f;
    d3d8_combiners_set_polygon_offset(enabled, bias / zmax, factor,
        (float)pw / (float)lw, (float)ph / (float)lh);
    if (enabled && pgraph_cached_getenv("MERCENARIES_TRACE_POLYGON_OFFSET")) {
        static uint32_t samples;
        if (samples++ < 96u)
            fprintf(stderr, "[PGRAPH-POLYGON-OFFSET] mode=%u bias=%g factor=%g zeta=%u scale=%g,%g\n",
                g_pg.draw_mode, bias, factor, zeta, (double)pw/lw, (double)ph/lh);
    }
}

static void apply_guest_render_states(IDirect3DDevice8 *dev)
{
    uint32_t color_write_mask = 0u;

    /* NV2A stores B/G/R/A enables in bits 0/8/16/24, while D3D8 uses
     * the compact R/G/B/A mask 1/2/4/8.  Keeping the host default 0x0F
     * makes alpha-only effect passes (including rain) overwrite RGB. */
    if (g_pg.color_mask & NV097_SET_COLOR_MASK_RED_WRITE_ENABLE)
        color_write_mask |= 0x1u;
    if (g_pg.color_mask & NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE)
        color_write_mask |= 0x2u;
    if (g_pg.color_mask & NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE)
        color_write_mask |= 0x4u;
    if (g_pg.color_mask & NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE)
        color_write_mask |= 0x8u;
    dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE,
                                color_write_mask);

    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, g_pg.alpha_test);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC, g_pg.alpha_func);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF, g_pg.alpha_ref);
    dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, g_pg.fog_enable);
    dev->lpVtbl->SetRenderState(dev, D3DRS_FOGCOLOR, g_pg.fog_color);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE,
                                g_pg.stencil_enable);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFUNC, g_pg.stencil_func);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILREF, g_pg.stencil_ref);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILMASK, g_pg.stencil_mask);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILWRITEMASK,
        g_pg.stencil_write_enable ? g_pg.stencil_write_mask : 0u);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFAIL, g_pg.stencil_fail);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILZFAIL, g_pg.stencil_zfail);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILPASS, g_pg.stencil_zpass);
    apply_guest_window_clip();
    d3d8_states_set_line_smooth(g_pg.line_smooth_enable != 0u);
    /* Retail shadow volumes request depth clamping so their far caps remain
     * closed. Clipping those caps destroys the stencil volume count. */
    d3d8_states_set_depth_clamp(
        ((g_pg.zmin_max_control & NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_EN) >> 4) ==
        NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_EN_CLAMP);
    apply_guest_polygon_offset();
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void get_guest_surface_dimensions(uint32_t *logical_width,
                                         uint32_t *logical_height,
                                         uint32_t *physical_width,
                                         uint32_t *physical_height)
{
    uint32_t x = g_pg.surface_clip_h & 0xFFFFu;
    uint32_t y = g_pg.surface_clip_v & 0xFFFFu;
    uint32_t w = g_pg.surface_clip_h >> 16;
    uint32_t h = g_pg.surface_clip_v >> 16;

    if (!w) w = 640u;
    if (!h) h = 480u;
    w += x;
    h += y;
    if (!w || w > 4096u) w = 640u;
    if (!h || h > 4096u) h = 480u;
    *logical_width = w;
    *logical_height = h;
    *physical_width = (uint32_t)(((uint64_t)w * g_pg.internal_width + 639u) /
                                 640u);
    *physical_height = (uint32_t)(((uint64_t)h * g_pg.internal_height + 479u) /
                                  480u);
    switch ((g_pg.surface_format &
             NV097_SET_SURFACE_FORMAT_ANTI_ALIASING) >> 12) {
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2:
        *physical_width *= 2u;
        break;
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4:
        *physical_width *= 2u;
        *physical_height *= 2u;
        break;
    default:
        break;
    }
}

static void release_guest_color_surface(GuestColorSurface *surface)
{
    if (!surface) return;
    if (g_pg.last_drawn_color == surface) g_pg.last_drawn_color = NULL;
    if (g_pg.last_resolved_color == surface) g_pg.last_resolved_color = NULL;
    if (g_pg.pending_scanout == surface) g_pg.pending_scanout = NULL;
    if (surface->srv) ID3D11ShaderResourceView_Release(surface->srv);
    if (surface->rtv) ID3D11RenderTargetView_Release(surface->rtv);
    if (surface->texture) ID3D11Texture2D_Release(surface->texture);
    memset(surface, 0, sizeof(*surface));
}

static void release_guest_depth_surface(GuestDepthSurface *surface)
{
    if (!surface) return;
    if (surface->stencil_srv)
        ID3D11ShaderResourceView_Release(surface->stencil_srv);
    if (surface->depth_srv)
        ID3D11ShaderResourceView_Release(surface->depth_srv);
    if (surface->dsv) ID3D11DepthStencilView_Release(surface->dsv);
    if (surface->texture) ID3D11Texture2D_Release(surface->texture);
    memset(surface, 0, sizeof(*surface));
}

static void release_satellite_depth_sample(void)
{
    if (g_pg.satellite_depth_sample_srv)
        ID3D11ShaderResourceView_Release(
            g_pg.satellite_depth_sample_srv);
    if (g_pg.satellite_depth_sample_rtv)
        ID3D11RenderTargetView_Release(g_pg.satellite_depth_sample_rtv);
    if (g_pg.satellite_depth_sample_texture)
        ID3D11Texture2D_Release(g_pg.satellite_depth_sample_texture);
    g_pg.satellite_depth_sample_srv = NULL;
    g_pg.satellite_depth_sample_rtv = NULL;
    g_pg.satellite_depth_sample_texture = NULL;
    g_pg.satellite_depth_sample_width = 0u;
    g_pg.satellite_depth_sample_height = 0u;
}

static int create_guest_depth_resource(
    ID3D11Device *device, UINT width, UINT height, uint32_t format,
    const D3D11_SUBRESOURCE_DATA *initial_data,
    ID3D11Texture2D **texture, ID3D11DepthStencilView **dsv,
    ID3D11ShaderResourceView **depth_srv,
    ID3D11ShaderResourceView **stencil_srv)
{
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    HRESULT hr;

    if (!device || !texture || !dsv || !depth_srv || !stencil_srv)
        return 0;
    *texture = NULL;
    *dsv = NULL;
    *depth_srv = NULL;
    *stencil_srv = NULL;
    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1u;
    texture_desc.ArraySize = 1u;
    texture_desc.Format = format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ?
        DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R24G8_TYPELESS;
    texture_desc.SampleDesc.Count = 1u;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL |
        (format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ?
            D3D11_BIND_SHADER_RESOURCE : 0u);
    hr = ID3D11Device_CreateTexture2D(device, &texture_desc, initial_data,
                                      texture);
    if (FAILED(hr) || !*texture)
        goto fail;

    memset(&dsv_desc, 0, sizeof(dsv_desc));
    dsv_desc.Format = format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ?
        DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = ID3D11Device_CreateDepthStencilView(
        device, (ID3D11Resource *)*texture, &dsv_desc, dsv);
    if (FAILED(hr) || !*dsv)
        goto fail;
    if (format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) {
        memset(&srv_desc, 0, sizeof(srv_desc));
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1u;
        srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        hr = ID3D11Device_CreateShaderResourceView(
            device, (ID3D11Resource *)*texture, &srv_desc, depth_srv);
        if (FAILED(hr) || !*depth_srv)
            goto fail;
        srv_desc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        hr = ID3D11Device_CreateShaderResourceView(
            device, (ID3D11Resource *)*texture, &srv_desc, stencil_srv);
        if (FAILED(hr) || !*stencil_srv)
            goto fail;
    }
    return 1;

fail:
    if (*stencil_srv) ID3D11ShaderResourceView_Release(*stencil_srv);
    if (*depth_srv) ID3D11ShaderResourceView_Release(*depth_srv);
    if (*dsv) ID3D11DepthStencilView_Release(*dsv);
    if (*texture) ID3D11Texture2D_Release(*texture);
    *texture = NULL;
    *dsv = NULL;
    *depth_srv = NULL;
    *stencil_srv = NULL;
    return 0;
}
typedef struct {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t width;
    uint32_t height;
} GuestColorSurfaceMigration;

static void release_color_surface_migrations(
    GuestColorSurfaceMigration migrations[MAX_GUEST_COLOR_SURFACES])
{
    uint32_t i;

    for (i = 0u; i < MAX_GUEST_COLOR_SURFACES; ++i) {
        if (migrations[i].srv)
            ID3D11ShaderResourceView_Release(migrations[i].srv);
        if (migrations[i].rtv)
            ID3D11RenderTargetView_Release(migrations[i].rtv);
        if (migrations[i].texture)
            ID3D11Texture2D_Release(migrations[i].texture);
        memset(&migrations[i], 0, sizeof(migrations[i]));
    }
}

static int prepare_color_surface_migrations(
    uint32_t internal_width, uint32_t internal_height,
    GuestColorSurfaceMigration migrations[MAX_GUEST_COLOR_SURFACES])
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    uint32_t i;
    HRESULT hr = E_FAIL;

    if (!device)
        return 0;
    for (i = 0u; i < MAX_GUEST_COLOR_SURFACES; ++i) {
        GuestColorSurface *surface = &g_pg.color_surfaces[i];
        GuestColorSurfaceMigration *migration = &migrations[i];
        D3D11_TEXTURE2D_DESC desc;

        if (!surface->valid)
            continue;
        if (!surface->texture || !surface->rtv || !surface->srv)
            goto fail;
        migration->width = (uint32_t)(
            ((uint64_t)surface->logical_width * internal_width + 639u) /
            640u);
        migration->height = (uint32_t)(
            ((uint64_t)surface->logical_height * internal_height + 479u) /
            480u);
        if (surface->anti_aliasing ==
                NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2 ||
            surface->anti_aliasing ==
                NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
            migration->width *= 2u;
        if (surface->anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
            migration->height *= 2u;
        if (migration->width == surface->width &&
            migration->height == surface->height) {
            migration->width = migration->height = 0u;
            continue;
        }

        ID3D11Texture2D_GetDesc(surface->texture, &desc);
        desc.Width = migration->width;
        desc.Height = migration->height;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL,
                                          &migration->texture);
        if (FAILED(hr) || !migration->texture)
            goto fail;
        hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)migration->texture, NULL, &migration->rtv);
        if (FAILED(hr) || !migration->rtv)
            goto fail;
        hr = ID3D11Device_CreateShaderResourceView(device,
            (ID3D11Resource *)migration->texture, NULL, &migration->srv);
        if (FAILED(hr) || !migration->srv)
            goto fail;
        /* Render-target histories (weather tint, exposure and menu effects)
         * live only in the host surface cache. Releasing those surfaces and
         * recreating them from stale Xbox VRAM makes the missing history
         * alternate with newly drawn frames, producing the persistent
         * grey/brown/black flashing seen after an in-game scale change.
         * Resample every live color target before committing the new scale. */
        if (!d3d8_CopyTextureToRenderTarget(surface->srv, migration->rtv,
                                            migration->width,
                                            migration->height))
            goto fail;
    }
    return 1;

fail:
    fprintf(stderr,
            "[PGRAPH-D3D11] internal resolution migration failed at "
            "surface %u (hr=%08lX)\n", i, hr);
    release_color_surface_migrations(migrations);
    return 0;
}

void pgraph_d3d11_set_internal_resolution(uint32_t width, uint32_t height)
{
    GuestColorSurfaceMigration migrations[MAX_GUEST_COLOR_SURFACES];
    GuestColorSurface *old_bound_color;
    GuestDepthSurface *old_bound_depth;
    uint32_t i;

    if (width < 640u) width = 640u;
    if (height < 480u) height = 480u;
    if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        width = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        height = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (g_pg.internal_width == width && g_pg.internal_height == height)
        return;

    if (g_pg.initialized) {
        memset(migrations, 0, sizeof(migrations));
        pgraph_d3d11_flush();
        old_bound_color = g_pg.bound_color;
        old_bound_depth = g_pg.bound_depth;
        d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
        if (!prepare_color_surface_migrations(width, height, migrations)) {
            if (old_bound_color && old_bound_color->valid) {
                d3d8_BindRenderTargets(old_bound_color->rtv,
                    old_bound_depth && old_bound_depth->valid ?
                        old_bound_depth->dsv : NULL,
                    old_bound_color->width, old_bound_color->height);
                d3d8_SetLogicalRenderTargetSize(
                    old_bound_color->logical_width,
                    old_bound_color->logical_height);
            }
            return;
        }
        g_pg.bound_color = NULL;
        g_pg.bound_depth = NULL;
        for (i = 0u; i < MAX_GUEST_COLOR_SURFACES; ++i) {
            GuestColorSurface *surface = &g_pg.color_surfaces[i];
            GuestColorSurfaceMigration *migration = &migrations[i];

            if (!migration->texture)
                continue;
            ID3D11ShaderResourceView_Release(surface->srv);
            ID3D11RenderTargetView_Release(surface->rtv);
            ID3D11Texture2D_Release(surface->texture);
            surface->texture = migration->texture;
            surface->rtv = migration->rtv;
            surface->srv = migration->srv;
            surface->width = migration->width;
            surface->height = migration->height;
            memset(migration, 0, sizeof(*migration));
        }
        release_color_surface_migrations(migrations);
        release_satellite_depth_sample();
        for (i = 0u; i < MAX_GUEST_DEPTH_SURFACES; ++i)
            release_guest_depth_surface(&g_pg.depth_surfaces[i]);
        ++g_pg.surface_generation;
    }
    g_pg.internal_width = width;
    g_pg.internal_height = height;
    fprintf(stderr, "[PGRAPH-D3D11] internal resolution %ux%u\n", width,
            height);
}

static GuestColorSurface *find_guest_color_surface(uint32_t offset)
{
    uint32_t i;
    for (i = 0; i < MAX_GUEST_COLOR_SURFACES; ++i) {
        if (g_pg.color_surfaces[i].valid &&
            g_pg.color_surfaces[i].offset == offset)
            return &g_pg.color_surfaces[i];
    }
    return NULL;
}

static int guest_color_surface_aliases_bound(
    const GuestColorSurface *surface)
{
    const GuestColorSurface *bound = g_pg.bound_color;

    if (!surface || !bound)
        return 0;

    /* The guest can recreate a color-surface record for the same VRAM
     * allocation while the older record is still resident. Pointer identity
     * is therefore not a sufficient framebuffer-feedback test. Treat either
     * the guest allocation or the host texture as authoritative aliases so a
     * post-process pass never samples the RTV it is actively overwriting. */
    return surface == bound || surface->offset == bound->offset ||
           surface->texture == bound->texture;
}

static GuestDepthSurface *find_guest_depth_surface(uint32_t offset)
{
    uint32_t i;
    for (i = 0; i < MAX_GUEST_DEPTH_SURFACES; ++i) {
        if (g_pg.depth_surfaces[i].valid &&
            g_pg.depth_surfaces[i].offset == offset)
            return &g_pg.depth_surfaces[i];
    }
    return NULL;
}

static void initialize_guest_depth_backing_if_zero(uint32_t width,
                                                    uint32_t height,
                                                    uint32_t pitch,
                                                    uint32_t format)
{
    NV2AState *d = nv2a_get_state();
    const uint32_t bytes_per_pixel =
        format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ? 4u : 2u;
    const size_t row_bytes = (size_t)width * bytes_per_pixel;
    const size_t vram_size = d ? memory_region_size(d->vram) : 0u;
    uint32_t y;

    if (!d || !d->vram_ptr || !width || !height || pitch < row_bytes ||
        (uint64_t)g_pg.surface_zeta_offset +
            (uint64_t)(height - 1u) * pitch + row_bytes > vram_size)
        return;
    for (y = 0; y < height; ++y) {
        const uint8_t *row = d->vram_ptr + g_pg.surface_zeta_offset +
                             (size_t)y * pitch;
        size_t x;
        for (x = 0; x < row_bytes; ++x) {
            if (row[x] != 0u)
                return;
        }
    }
    for (y = 0; y < height; ++y) {
        uint8_t *row = d->vram_ptr + g_pg.surface_zeta_offset +
                       (size_t)y * pitch;
        uint32_t x;
        if (bytes_per_pixel == 4u) {
            for (x = 0; x < width; ++x)
                memcpy(row + (size_t)x * 4u, &g_pg.zstencil_clear, 4u);
        } else {
            const uint16_t value = (uint16_t)g_pg.zstencil_clear;
            for (x = 0; x < width; ++x)
                memcpy(row + (size_t)x * 2u, &value, 2u);
        }
    }
    fprintf(stderr,
            "[PGRAPH-ZETA] initialized zero backing off=%08X value=%08X\n",
            g_pg.surface_zeta_offset, g_pg.zstencil_clear);
}

static GuestColorSurface *get_guest_color_surface_at(
    uint32_t offset, uint32_t pitch, uint32_t format, uint32_t anti_aliasing,
    uint32_t logical_width, uint32_t logical_height,
    uint32_t width, uint32_t height)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    GuestColorSurface *surface;
    D3D11_TEXTURE2D_DESC desc;
    uint32_t i;
    HRESULT hr;

    if (!device) return NULL;
    surface = find_guest_color_surface(offset);
    /* Match Xemu's non-strict surface compatibility rule. A game may narrow
     * SET_SURFACE_CLIP for a pass while retaining the same VRAM allocation;
     * the existing larger backing remains compatible and must keep its GPU
     * contents. Recreating it on every clip-size change loses prior passes. */
    if (surface && surface->width >= width && surface->height >= height &&
        surface->pitch == pitch && surface->format == format &&
        surface->anti_aliasing == anti_aliasing)
        return surface;
    if (surface) {
        if (g_pg.bound_color == surface) {
            d3d8_BindRenderTargets(NULL, NULL, 0, 0);
            g_pg.bound_color = NULL;
            g_pg.bound_depth = NULL;
        }
        release_guest_color_surface(surface);
    } else {
        for (i = 0; i < MAX_GUEST_COLOR_SURFACES; ++i) {
            if (!g_pg.color_surfaces[i].valid) {
                surface = &g_pg.color_surfaces[i];
                break;
            }
        }
    }
    if (!surface) return NULL;

    memset(&desc, 0, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &surface->texture);
    if (FAILED(hr)) goto fail;
    hr = ID3D11Device_CreateRenderTargetView(device,
        (ID3D11Resource *)surface->texture, NULL, &surface->rtv);
    if (FAILED(hr)) goto fail;
    hr = ID3D11Device_CreateShaderResourceView(device,
        (ID3D11Resource *)surface->texture, NULL, &surface->srv);
    if (FAILED(hr)) goto fail;

    surface->offset = offset;
    surface->pitch = pitch;
    surface->format = format;
    surface->anti_aliasing = anti_aliasing;
    surface->width = width;
    surface->height = height;
    surface->logical_width = logical_width;
    surface->logical_height = logical_height;
    surface->created_draw = g_pg.stats.draw_calls;
    surface->valid = 1;
    fprintf(stderr,
            "[PGRAPH-SURFACE] color create off=%08X pitch=%u physical=%ux%u "
            "logical=%ux%u fmt=%u aa=%u draw=%u\n",
            surface->offset, surface->pitch, surface->width, surface->height,
            surface->logical_width, surface->logical_height, surface->format,
            surface->anti_aliasing, g_pg.stats.draw_calls);
    return surface;

fail:
    fprintf(stderr, "[PGRAPH-SURFACE] color create failed hr=%08lX\n", hr);
    release_guest_color_surface(surface);
    return NULL;
}

/* An explicit color copy into a known linear depth allocation must create
 * a full-sized color view before the first clipped RedCanvas draw. Inferring
 * that allocation from the radar clip (575x132) loses the 640x480 texture
 * identity and falls back to CPU depth bytes after a level changes buffers.
 * This creates storage only: depth contents are not substituted for color. */
static GuestColorSurface *get_image_blit_depth_destination(
    uint32_t offset, uint32_t pitch)
{
    GuestDepthSurface *depth = find_guest_depth_surface(offset);
    if (!depth || !depth->valid || depth->anti_aliasing != 0u ||
        depth->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        depth->pitch != pitch)
        return NULL;
    return get_guest_color_surface_at(offset, pitch,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 0u,
        depth->logical_width, depth->logical_height, depth->width, depth->height);
}

static GuestColorSurface *get_guest_color_surface(void)
{
    uint32_t width, height, logical_width, logical_height;
    get_guest_surface_dimensions(&logical_width, &logical_height, &width, &height);
    return get_guest_color_surface_at(g_pg.surface_color_offset,
        g_pg.surface_pitch & 0xFFFFu,
        g_pg.surface_format & NV097_SET_SURFACE_FORMAT_COLOR,
        (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ANTI_ALIASING) >> 12,
        logical_width, logical_height, width, height);
}

static GuestDepthSurface *get_guest_depth_surface(uint32_t width,
                                                   uint32_t height,
                                                   uint32_t logical_width,
                                                   uint32_t logical_height)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    GuestDepthSurface *surface;
    uint32_t pitch = g_pg.surface_pitch >> 16;
    uint32_t format =
        (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ZETA) >> 4;
    uint32_t anti_aliasing =
        (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ANTI_ALIASING) >> 12;
    uint32_t backing_width = logical_width;
    uint32_t backing_height = logical_height;
    uint32_t i;
    HRESULT hr;

    if (!device || !format || !g_pg.surface_zeta_offset) return NULL;

    surface = find_guest_depth_surface(g_pg.surface_zeta_offset);
    if (surface && surface->width >= width && surface->height >= height &&
        surface->pitch == pitch && surface->format == format &&
        surface->anti_aliasing == anti_aliasing)
        return surface;
    if (surface) {
        if (g_pg.bound_depth == surface) {
            d3d8_BindRenderTargets(NULL, NULL, 0, 0);
            g_pg.bound_color = NULL;
            g_pg.bound_depth = NULL;
        }
        release_guest_depth_surface(surface);
    } else {
        for (i = 0; i < MAX_GUEST_DEPTH_SURFACES; ++i) {
            if (!g_pg.depth_surfaces[i].valid) {
                surface = &g_pg.depth_surfaces[i];
                break;
            }
        }
    }
    if (!surface) return NULL;

    /* Xemu keeps one current SurfaceBinding for each VRAM address.  Only
     * when a new zeta surface is being created does it retire an incompatible
     * old color binding.  Do not repeat this for compatible zeta rebinds:
     * Mercenaries intentionally retains the 0x02980000 packed-color view and
     * samples it in later title/gameplay post-process passes.
     *
     * The color/zeta-same-target case also remains intact for our explicit
     * packed-depth copy path.  The transition this fixes is 0x01E34000 from
     * front-end color to gameplay zeta while color moves to 0x0240C000. */
    if (g_pg.surface_color_offset != g_pg.surface_zeta_offset) {
        GuestColorSurface *stale_color =
            find_guest_color_surface(g_pg.surface_zeta_offset);
        if (stale_color) {
            if (g_pg.bound_color == stale_color ||
                g_pg.bound_depth != NULL) {
                d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
                g_pg.bound_color = NULL;
                g_pg.bound_depth = NULL;
            }
            fprintf(stderr,
                    "[PGRAPH-SURFACE-EVICT] color off=%08X replaced-by=zeta "
                    "color-target=%08X\n",
                    stale_color->offset, g_pg.surface_color_offset);
            release_guest_color_surface(stale_color);
        }
    }

    if (!create_guest_depth_resource(
            device, width, height, format, NULL,
            &surface->texture, &surface->dsv,
            &surface->depth_srv, &surface->stencil_srv)) {
        hr = E_FAIL;
        goto fail;
    }
    if (context) {
        const UINT clear_flags = D3D11_CLEAR_DEPTH |
            (format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ?
                D3D11_CLEAR_STENCIL : 0u);
        ID3D11DeviceContext_ClearDepthStencilView(context, surface->dsv,
                                                   clear_flags, 1.0f, 0u);
    }
    if (anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2 ||
        anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
        backing_width *= 2u;
    if (anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
        backing_height *= 2u;
    initialize_guest_depth_backing_if_zero(backing_width, backing_height,
                                           pitch, format);

    surface->offset = g_pg.surface_zeta_offset;
    surface->pitch = pitch;
    surface->format = format;
    surface->anti_aliasing = anti_aliasing;
    surface->width = width;
    surface->height = height;
    surface->logical_width = logical_width;
    surface->logical_height = logical_height;
    surface->uniform_stencil_known =
        format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
    surface->uniform_stencil_value = 0u;
    surface->possible_stencil_bits = 0u;
    surface->valid = 1;
    fprintf(stderr,
            "[PGRAPH-SURFACE] depth create off=%08X pitch=%u %ux%u fmt=%u\n",
            surface->offset, surface->pitch, surface->width, surface->height,
            surface->format);
    return surface;

fail:
    fprintf(stderr, "[PGRAPH-SURFACE] depth create failed hr=%08lX\n", hr);
    release_guest_depth_surface(surface);
    return NULL;
}

static int upload_guest_depth_pixels(GuestDepthSurface *depth,
                                     const uint32_t *pixels,
                                     UINT row_bytes)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    D3D11_SUBRESOURCE_DATA initial_data;
    ID3D11Texture2D *replacement = NULL;
    ID3D11DepthStencilView *replacement_dsv = NULL;
    ID3D11ShaderResourceView *replacement_depth_srv = NULL;
    ID3D11ShaderResourceView *replacement_stencil_srv = NULL;
    const size_t pixel_count = depth ?
        (size_t)depth->width * depth->height : 0u;
    uint32_t first;
    uint8_t stencil_first;
    uint8_t possible_stencil_bits;
    size_t pixel;
    int uniform = 1;
    int uniform_stencil = 1;
    HRESULT hr;

    if (!depth || !pixels || !pixel_count || !device || !context ||
        depth->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8)
        return 0;

    first = pixels[0];
    for (pixel = 1u; pixel < pixel_count; ++pixel) {
        if (pixels[pixel] != first) {
            uniform = 0;
            break;
        }
    }
    stencil_first = (uint8_t)(first >> 24);
    possible_stencil_bits = stencil_first;
    for (pixel = 1u; pixel < pixel_count; ++pixel) {
        const uint8_t stencil = (uint8_t)(pixels[pixel] >> 24);
        possible_stencil_bits |= stencil;
        if (stencil != stencil_first)
            uniform_stencil = 0;
    }
    if (uniform) {
        ID3D11DeviceContext_ClearDepthStencilView(
            context, depth->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            (float)(first & 0x00FFFFFFu) / 16777215.0f, stencil_first);
        depth->uniform_stencil_known = 1;
        depth->uniform_stencil_value = stencil_first;
        depth->possible_stencil_bits = possible_stencil_bits;
        return 1;
    }

    memset(&initial_data, 0, sizeof(initial_data));
    initial_data.pSysMem = pixels;
    initial_data.SysMemPitch = row_bytes;
    initial_data.SysMemSlicePitch = row_bytes * depth->height;
    if (!create_guest_depth_resource(
            device, depth->width, depth->height,
            NV097_SET_SURFACE_FORMAT_ZETA_Z24S8, &initial_data,
            &replacement, &replacement_dsv,
            &replacement_depth_srv, &replacement_stencil_srv)) {
        hr = E_FAIL;
        fprintf(stderr,
                "[PGRAPH-SURFACE-ALIAS] depth upload failed hr=%08lX\n",
                hr);
        return 0;
    }

    if (depth->stencil_srv)
        ID3D11ShaderResourceView_Release(depth->stencil_srv);
    if (depth->depth_srv)
        ID3D11ShaderResourceView_Release(depth->depth_srv);
    ID3D11DepthStencilView_Release(depth->dsv);
    ID3D11Texture2D_Release(depth->texture);
    depth->texture = replacement;
    depth->dsv = replacement_dsv;
    depth->depth_srv = replacement_depth_srv;
    depth->stencil_srv = replacement_stencil_srv;
    depth->uniform_stencil_known = uniform_stencil;
    depth->uniform_stencil_value = stencil_first;
    depth->possible_stencil_bits = possible_stencil_bits;
    return 1;
}

static int sync_guest_depth_alias_from_depth(GuestDepthSurface *source,
                                             GuestDepthSurface *destination)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t *pixels = NULL;
    uint32_t *packed_color_pixels = NULL;
    const UINT row_bytes = destination ? destination->width * 4u : 0u;
    uint32_t y;
    int synchronized = 0;
    HRESULT hr;

    if (!source || !destination || source == destination || !device ||
        !context || source->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        destination->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        !source->width || !source->height || !destination->width ||
        !destination->height)
        return 0;

    {
        GuestColorSurface *alias_color =
            find_guest_color_surface(destination->offset);
        if (g_pg.bound_depth == source || g_pg.bound_depth == destination ||
            g_pg.bound_color == alias_color) {
            d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
            g_pg.bound_color = NULL;
            g_pg.bound_depth = NULL;
        }
        if (alias_color && alias_color->texture && alias_color->rtv &&
            alias_color->width == destination->width &&
            alias_color->height == destination->height &&
            d3d8_CopyScaledDepthStencilAlias(
                source->texture, source->depth_srv, source->stencil_srv,
                source->width, source->height,
                destination->dsv, alias_color->rtv,
                destination->width, destination->height,
                source->uniform_stencil_known,
                source->uniform_stencil_value,
                source->possible_stencil_bits,
                destination->uniform_stencil_known &&
                    destination->uniform_stencil_value ==
                        source->uniform_stencil_value)) {
            static uint32_t trace_count;
            destination->uniform_stencil_known =
                source->uniform_stencil_known;
            destination->uniform_stencil_value =
                source->uniform_stencil_value;
            destination->possible_stencil_bits =
                source->possible_stencil_bits;
            /* Record what actually produced the color alias.  Merely sharing
             * an address with a depth surface is not sufficient provenance:
             * games routinely recycle the same VRAM range for an unrelated
             * color render target. */
            alias_color->depth_alias_source_offset = source->offset;
            alias_color->depth_alias_source_serial = source->write_serial;
            alias_color->resolve_source_width = 0u;
            alias_color->resolve_source_height = 0u;
            alias_color->resolve_source_pitch = 0u;
            if (trace_count++ < 8u)
                fprintf(stderr,
                        "[PGRAPH-SURFACE-ALIAS] GPU zeta %08X %ux%u -> "
                        "packed color/zeta %08X %ux%u stencil=%s/%u\n",
                        source->offset, source->width, source->height,
                        destination->offset, destination->width,
                        destination->height,
                        source->uniform_stencil_known ? "uniform" : "variable",
                        source->uniform_stencil_value);
            return 1;
        }
    }

    ID3D11Texture2D_GetDesc(source->texture, &desc);
    desc.MipLevels = 1u;
    desc.ArraySize = 1u;
    desc.SampleDesc.Count = 1u;
    desc.SampleDesc.Quality = 0u;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr) || !staging) {
        fprintf(stderr,
                "[PGRAPH-SURFACE-ALIAS] depth staging create failed "
                "hr=%08lX\n", hr);
        goto done;
    }
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)source->texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0u,
                                 D3D11_MAP_READ, 0u, &mapped);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[PGRAPH-SURFACE-ALIAS] depth staging map failed hr=%08lX\n",
                hr);
        goto done;
    }

    pixels = (uint32_t *)malloc((size_t)row_bytes * destination->height);
    if (!pixels) {
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
        goto done;
    }
    for (y = 0u; y < destination->height; ++y) {
        const uint32_t sy = (uint32_t)(((uint64_t)y * source->height +
            destination->height / 2u) / destination->height);
        const uint32_t *source_row = (const uint32_t *)(
            (const uint8_t *)mapped.pData +
            (size_t)(sy < source->height ? sy : source->height - 1u) *
                mapped.RowPitch);
        uint32_t *destination_row = pixels +
            (size_t)y * destination->width;
        uint32_t x;
        for (x = 0u; x < destination->width; ++x) {
            const uint32_t sx = (uint32_t)(((uint64_t)x * source->width +
                destination->width / 2u) / destination->width);
            destination_row[x] =
                source_row[sx < source->width ? sx : source->width - 1u];
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
    if (pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_ALIAS_STENCIL") != NULL) {
        static uint32_t trace_count;
        if (trace_count++ < 4u) {
            uint32_t histogram[256] = { 0u };
            const size_t pixel_count = (size_t)destination->width *
                                       destination->height;
            size_t pixel;
            uint32_t unique = 0u, nonzero = 0u, value;
            for (pixel = 0u; pixel < pixel_count; ++pixel) {
                value = pixels[pixel] >> 24;
                ++histogram[value];
                nonzero += value != 0u;
            }
            fprintf(stderr,
                    "[PGRAPH-DEPTH-ALIAS-STENCIL] source=%08X %ux%u "
                    "destination=%08X %ux%u pixels=%zu nonzero=%u values=",
                    source->offset, source->width, source->height,
                    destination->offset, destination->width,
                    destination->height, pixel_count, nonzero);
            for (value = 0u; value < 256u; ++value) {
                if (histogram[value]) {
                    fprintf(stderr, "%s%u:%u", unique ? "," : "",
                            value, histogram[value]);
                    ++unique;
                }
            }
            fprintf(stderr, " unique=%u\n", unique);
        }
    }

    /* The Xbox exposes Z24S8 surfaces as ordinary color textures without a
     * copy. Mercenaries relies on that alias for its sky shader: stage 1 is
     * LIN_A8B8G8R8 and texreg2gb consumes the low/middle depth bytes. Keep a
     * raw-byte-order color-SRV representation alongside the host D24S8
     * surface. D3D11's packed word is depth[23:0] | stencil[31:24], while the
     * Xbox bytes are [stencil, depth-low, depth-middle, depth-high]. Sampling
     * an alias as A8R8G8B8 applies a draw-time R/B swap; A8B8G8R8 does not. */
    {
        GuestColorSurface *alias_color =
            find_guest_color_surface(destination->offset);
        const size_t pixel_count = (size_t)destination->width *
                                   destination->height;
        size_t pixel;

        if (alias_color && alias_color->texture &&
            alias_color->width == destination->width &&
            alias_color->height == destination->height) {
            packed_color_pixels = (uint32_t *)malloc(pixel_count * 4u);
            if (packed_color_pixels) {
                for (pixel = 0u; pixel < pixel_count; ++pixel) {
                    const uint32_t host_d24s8 = pixels[pixel];
                    packed_color_pixels[pixel] =
                        (host_d24s8 << 8) | (host_d24s8 >> 24);
                }
                if (g_pg.bound_color == alias_color) {
                    d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
                    g_pg.bound_color = NULL;
                    g_pg.bound_depth = NULL;
                }
                ID3D11DeviceContext_UpdateSubresource(
                    context, (ID3D11Resource *)alias_color->texture, 0u,
                    NULL, packed_color_pixels, row_bytes,
                    row_bytes * destination->height);
                alias_color->depth_alias_source_offset = source->offset;
                alias_color->depth_alias_source_serial = source->write_serial;
                alias_color->resolve_source_width = 0u;
                alias_color->resolve_source_height = 0u;
                alias_color->resolve_source_pitch = 0u;
            }
        }
    }
    synchronized = upload_guest_depth_pixels(destination, pixels, row_bytes);

done:
    free(pixels);
    free(packed_color_pixels);
    if (staging)
        ID3D11Texture2D_Release(staging);
    return synchronized;
}
static void sync_guest_color_alias_to_depth(GuestDepthSurface *depth)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    GuestColorSurface *color;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA initial_data;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *replacement = NULL;
    ID3D11DepthStencilView *replacement_dsv = NULL;
    ID3D11ShaderResourceView *replacement_depth_srv = NULL;
    ID3D11ShaderResourceView *replacement_stencil_srv = NULL;
    uint8_t *packed = NULL;
    const UINT row_bytes = depth ? depth->width * 4u : 0u;
    uint32_t y;
    int synchronized = 0;
    HRESULT hr;

    if (!depth || depth->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        !device || !context)
        return;
    color = find_guest_color_surface(depth->offset);
    if (!color || !color->gpu_drawn || !color->write_serial ||
        color->write_serial == depth->color_alias_serial ||
        color->width != depth->width || color->height != depth->height)
        return;

    /* A direct packed-depth copy may reuse the source zeta path while that
     * source is still the exact generation sampled by the draw.  Mercenaries'
     * title sky relies on this refresh.  Once the source has been written
     * again, however, the destination is an independent snapshot: recopying
     * the live source would overwrite the satellite filter with newer depth
     * (or an older title frame). */
    if (color->depth_alias_source_offset &&
        color->depth_alias_source_serial) {
        GuestDepthSurface *source = find_guest_depth_surface(
            color->depth_alias_source_offset);
        if (source && source->write_serial ==
                          color->depth_alias_source_serial &&
            sync_guest_depth_alias_from_depth(source, depth)) {
            depth->color_alias_serial = color->write_serial;
            return;
        }
    }

    /* The source generation no longer matches. Convert the destination's
     * actual packed-color pixels below; the GPU path is asynchronous and
     * preserves the snapshot. */
    {
        static uint32_t captured_aliases;
        if (captured_aliases < 3u &&
            pgraph_cached_getenv("MERCENARIES_CAPTURE_DEPTH_ALIASES") != NULL &&
            d3d8_CopyTextureToBackbuffer(color->texture, color->srv,
                                         color->width, color->height)) {
            fprintf(stderr,
                    "[PGRAPH-SURFACE-ALIAS-CAPTURE] ordinal=%u color=%08X "
                    "serial=%llu\n",
                    captured_aliases, color->offset,
                    (unsigned long long)color->write_serial);
            d3d8_DebugCaptureFrameNow();
            ++captured_aliases;
        }
    }

    /* NV2A color and zeta bindings alias the same VRAM. Mercenaries renders a
     * packed Z24S8 refresh through the color path, then immediately rebinds
     * that allocation as zeta. Preserve the raw 32-bit texels just as Xemu's
     * surface download/invalidate/upload lifecycle does. */
    if (g_pg.bound_color == color || g_pg.bound_depth == depth) {
        d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
        g_pg.bound_color = NULL;
        g_pg.bound_depth = NULL;
    }

    /* Preserve the same raw A8R8G8B8 -> D24S8 channel repack as the CPU
     * fallback below without synchronously reading a scaled render target
     * back to the CPU. Shader-stencil export is optional in D3D11, so the
     * helper is capability-gated and failure retains the known-correct path. */
    if (d3d8_CopyPackedColorToDepthStencil(color->srv, depth->dsv,
                                            depth->width, depth->height)) {
        depth->uniform_stencil_known = 0;
        depth->possible_stencil_bits = 0xFFu;
        depth->color_alias_serial = color->write_serial;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_ALIASES") != NULL &&
            pgraph_trace_surface_textures_enabled()) {
            static uint32_t trace_count;
            if (trace_count++ < 128u) {
                fprintf(stderr,
                        "[PGRAPH-SURFACE-ALIAS] GPU color %08X serial=%llu "
                        "-> zeta %08X %ux%u\n",
                        color->offset,
                        (unsigned long long)color->write_serial,
                        depth->offset, depth->width, depth->height);
            }
        }
        return;
    }
    ID3D11Texture2D_GetDesc(color->texture, &desc);

    desc.MipLevels = 1u;
    desc.ArraySize = 1u;
    desc.SampleDesc.Count = 1u;
    desc.SampleDesc.Quality = 0u;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr) || !staging)
        goto done;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)color->texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0u,
                                 D3D11_MAP_READ, 0u, &mapped);
    if (FAILED(hr))
        goto done;
    packed = (uint8_t *)malloc((size_t)row_bytes * depth->height);
    if (!packed) {
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
        goto done;
    }
    for (y = 0u; y < depth->height; ++y) {
        const uint8_t *source_row =
            (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
        uint32_t *destination_row =
            (uint32_t *)(packed + (size_t)y * row_bytes);
        uint32_t x;
        for (x = 0u; x < depth->width; ++x) {
            const uint8_t *rgba = source_row + (size_t)x * 4u;
            /* The color RTV exposes canonical RGBA channels. Reconstruct the
             * Xbox A8R8G8B8 word, where the aliased Z24 occupies bits 8..31
             * and stencil bits 0..7, then repack for DXGI D24S8 (depth in
             * bits 0..23, stencil in bits 24..31). */
            destination_row[x] = (uint32_t)rgba[1] |
                ((uint32_t)rgba[0] << 8) |
                ((uint32_t)rgba[3] << 16) |
                ((uint32_t)rgba[2] << 24);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
    {
        const uint32_t *pixels = (const uint32_t *)packed;
        const size_t pixel_count = (size_t)depth->width * depth->height;
        const uint32_t first = pixels[0];
        const uint8_t first_stencil = (uint8_t)(first >> 24);
        uint8_t possible_stencil_bits = first_stencil;
        size_t pixel;
        int uniform = 1;
        int uniform_stencil = 1;
        for (pixel = 1u; pixel < pixel_count; ++pixel) {
            if (pixels[pixel] != first) {
                uniform = 0;
                break;
            }
        }
        for (pixel = 1u; pixel < pixel_count; ++pixel) {
            const uint8_t stencil = (uint8_t)(pixels[pixel] >> 24);
            possible_stencil_bits |= stencil;
            if (stencil != first_stencil)
                uniform_stencil = 0;
        }
        if (uniform) {
            ID3D11DeviceContext_ClearDepthStencilView(
                context, depth->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                (float)(first & 0x00FFFFFFu) / 16777215.0f,
                (UINT8)(first >> 24));
            synchronized = 1;
        } else {
            memset(&initial_data, 0, sizeof(initial_data));
            initial_data.pSysMem = packed;
            initial_data.SysMemPitch = row_bytes;
            initial_data.SysMemSlicePitch = row_bytes * depth->height;
            if (create_guest_depth_resource(
                    device, depth->width, depth->height,
                    NV097_SET_SURFACE_FORMAT_ZETA_Z24S8, &initial_data,
                    &replacement, &replacement_dsv,
                    &replacement_depth_srv, &replacement_stencil_srv)) {
                if (depth->stencil_srv)
                    ID3D11ShaderResourceView_Release(depth->stencil_srv);
                if (depth->depth_srv)
                    ID3D11ShaderResourceView_Release(depth->depth_srv);
                ID3D11DepthStencilView_Release(depth->dsv);
                ID3D11Texture2D_Release(depth->texture);
                depth->texture = replacement;
                depth->dsv = replacement_dsv;
                depth->depth_srv = replacement_depth_srv;
                depth->stencil_srv = replacement_stencil_srv;
                replacement = NULL;
                replacement_dsv = NULL;
                replacement_depth_srv = NULL;
                replacement_stencil_srv = NULL;
                synchronized = 1;
            }
        }
        if (synchronized) {
            depth->uniform_stencil_known = uniform_stencil;
            depth->uniform_stencil_value = first_stencil;
            depth->possible_stencil_bits = possible_stencil_bits;
        }
    }
    if (synchronized)
        depth->color_alias_serial = color->write_serial;
    if (synchronized &&
        pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_ALIASES") != NULL &&
        pgraph_trace_surface_textures_enabled()) {
        static uint32_t trace_count;
        if (trace_count++ >= 128u)
            goto done;
        fprintf(stderr,
                "[PGRAPH-SURFACE-ALIAS] color %08X serial=%llu -> zeta "
                "%08X %ux%u\n",
                color->offset, (unsigned long long)color->write_serial,
                depth->offset, depth->width, depth->height);
    }

done:
    free(packed);
    if (replacement_stencil_srv)
        ID3D11ShaderResourceView_Release(replacement_stencil_srv);
    if (replacement_depth_srv)
        ID3D11ShaderResourceView_Release(replacement_depth_srv);
    if (replacement_dsv)
        ID3D11DepthStencilView_Release(replacement_dsv);
    if (replacement)
        ID3D11Texture2D_Release(replacement);
    if (staging)
        ID3D11Texture2D_Release(staging);
}

static int stencil_op_preserves_uniform_value(uint32_t operation,
                                              uint8_t value,
                                              uint8_t reference,
                                              uint8_t write_mask)
{
    uint8_t result;

    switch (operation) {
    case 1u: result = value; break;
    case 2u: result = 0u; break;
    case 3u: result = reference; break;
    case 4u: result = value == 0xFFu ? 0xFFu : (uint8_t)(value + 1u); break;
    case 5u: result = value == 0u ? 0u : (uint8_t)(value - 1u); break;
    case 6u: result = (uint8_t)~value; break;
    case 7u: result = (uint8_t)(value + 1u); break;
    case 8u: result = (uint8_t)(value - 1u); break;
    default: result = value; break;
    }

    result = (uint8_t)((value & (uint8_t)~write_mask) |
                       (result & write_mask));
    return result == value;
}

static void note_bound_depth_stencil_draw(void)
{
    GuestDepthSurface *depth = g_pg.bound_depth;

    if (!depth ||
        depth->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        !g_pg.stencil_enable || !g_pg.stencil_write_enable ||
        (g_pg.stencil_write_mask & 0xFFu) == 0u)
        return;
    if (depth->uniform_stencil_known &&
        stencil_op_preserves_uniform_value(g_pg.stencil_fail,
                                           depth->uniform_stencil_value,
                                           (uint8_t)g_pg.stencil_ref,
                                           (uint8_t)g_pg.stencil_write_mask) &&
        stencil_op_preserves_uniform_value(g_pg.stencil_zfail,
                                           depth->uniform_stencil_value,
                                           (uint8_t)g_pg.stencil_ref,
                                           (uint8_t)g_pg.stencil_write_mask) &&
        stencil_op_preserves_uniform_value(g_pg.stencil_zpass,
                                           depth->uniform_stencil_value,
                                           (uint8_t)g_pg.stencil_ref,
                                           (uint8_t)g_pg.stencil_write_mask))
        return;
    /* Track a conservative OR of every stencil bit that can occur.  A draw
     * may cover only a subset of pixels, so existing bits remain possible.
     * ZERO/KEEP cannot introduce bits, REPLACE can introduce reference bits,
     * and arithmetic/invert operations can introduce any writable bit.  This
     * lets scaled alias reconstruction skip provably-zero bit planes without
     * weakening byte-exact stencil semantics. */
    {
        const uint8_t write_mask = (uint8_t)g_pg.stencil_write_mask;
        const uint8_t reference = (uint8_t)g_pg.stencil_ref;
        const uint32_t operations[3] = {
            g_pg.stencil_fail, g_pg.stencil_zfail, g_pg.stencil_zpass
        };
        uint32_t operation_index;

        for (operation_index = 0u; operation_index < 3u; ++operation_index) {
            const uint32_t operation = operations[operation_index];
            if (operation == 3u)
                depth->possible_stencil_bits |= reference & write_mask;
            else if (operation >= 4u && operation <= 8u)
                depth->possible_stencil_bits |= write_mask;
        }
    }

    if (depth->uniform_stencil_known &&
        pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_ALIAS_STENCIL") != NULL) {
        static uint32_t trace_invalidation_count;

        if (trace_invalidation_count++ < 16u) {
            fprintf(stderr,
                    "[PGRAPH-DEPTH-ALIAS-INVALIDATE] value=%u ref=%u mask=%02X ops=%u/%u/%u\n",
                    (unsigned)depth->uniform_stencil_value,
                    (unsigned)((uint8_t)g_pg.stencil_ref),
                    (unsigned)((uint8_t)g_pg.stencil_write_mask),
                    (unsigned)g_pg.stencil_fail,
                    (unsigned)g_pg.stencil_zfail,
                    (unsigned)g_pg.stencil_zpass);
        }
    }

    /* A draw can cover an arbitrary subset and each outcome can select a
     * different operation. Once any writable operation is possible, a
     * uniform stencil plane is no longer provable without reading it back. */
    depth->uniform_stencil_known = 0;
}

static void prepare_guest_render_targets(int want_depth)
{
    GuestColorSurface *color = get_guest_color_surface();
    GuestDepthSurface *depth = NULL;

    if (!color) {
        d3d8_BindRenderTargets(NULL, NULL, 0, 0);
        g_pg.bound_color = NULL;
        g_pg.bound_depth = NULL;
        return;
    }
    if (want_depth)
        depth = get_guest_depth_surface(color->width, color->height,
                                        color->logical_width,
                                        color->logical_height);
    if (depth)
        sync_guest_color_alias_to_depth(depth);
    if (g_pg.bound_color != color || g_pg.bound_depth != depth) {
        d3d8_BindRenderTargets(color->rtv, depth ? depth->dsv : NULL,
                               color->width, color->height);
        g_pg.bound_color = color;
        g_pg.bound_depth = depth;
    }
    /* Xbox AA expands backing-store samples while vertex programs continue
     * to emit logical screen coordinates. Xemu applies the same split. */
    d3d8_SetLogicalRenderTargetSize(color->logical_width,
                                    color->logical_height);
}

static int texture_color_format_is_linear(uint32_t color_format);
static void get_guest_texture_dimensions(uint32_t format, uint32_t image_rect,
                                         uint32_t color_format,
                                         uint32_t *width, uint32_t *height);
static uint32_t get_guest_texture_levels(uint32_t format,
                                         uint32_t color_format,
                                         uint32_t control0);

static int guest_color_surface_texture_format_compatible(
    uint32_t surface_format, uint32_t texture_format)
{
    switch (surface_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        return texture_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        return texture_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5 ||
               texture_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        return texture_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 ||
               texture_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        return texture_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 ||
               texture_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8 ||
               texture_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
               texture_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    default:
        return 0;
    }
}

static int guest_color_surface_texture_shape_compatible(
    const GuestColorSurface *surface, uint32_t texture_format,
    uint32_t image_rect, uint32_t control0, uint32_t control1)
{
    const uint32_t color_format =
        (texture_format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    uint32_t width, height;
    uint32_t backing_width, backing_height;

    if (!surface ||
        (texture_format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) != 0u ||
        get_guest_texture_levels(texture_format, color_format, control0) > 1u)
        return 0;

    get_guest_texture_dimensions(texture_format, image_rect, color_format,
                                 &width, &height);
    backing_width = surface->logical_width;
    backing_height = surface->logical_height;
    /* SET_SURFACE_FORMAT antialiasing expands the Xbox backing allocation,
     * independently of host internal-resolution scaling. Texture image
     * dimensions and pitch describe that AA-expanded guest allocation. */
    switch (surface->anti_aliasing) {
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2:
        backing_width *= 2u;
        break;
    case NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4:
        backing_width *= 2u;
        backing_height *= 2u;
        break;
    default:
        break;
    }
    if (backing_width != width || backing_height != height) {
        /* A multisampled Xbox render target is commonly resolved into a
         * single-sample surface and then rebound using the source allocation's
         * AA-expanded texture description.  The SRV contains the resolved
         * image, so normalized sampling is still correct even though the
         * declaration names (for example) 1280x480 over a 640x480 target.
         * Accept this only when an actual preceding larger-surface blit
         * recorded matching guest dimensions and pitch. */
        if (surface->resolve_source_width != width ||
            surface->resolve_source_height != height)
            return 0;
    }

    /* Xemu also requires equal guest pitch for linear surface-to-texture
     * reuse. The render target remains physically enlarged, but its Xbox
     * allocation metadata stays in guest pixels/bytes. */
    if (texture_color_format_is_linear(color_format)) {
        uint32_t row_bytes = width * 4u;
        uint32_t pitch = control1 >> 16;
        if (color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
            color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8)
            row_bytes = width * 2u;
        if (pitch < row_bytes)
            pitch = row_bytes;
        if (surface->pitch != pitch && surface->resolve_source_pitch != pitch)
            return 0;
    }

    return guest_color_surface_texture_format_compatible(surface->format,
                                                         color_format);
}

static void guest_surface_backing_dimensions(uint32_t logical_width,
                                              uint32_t logical_height,
                                              uint32_t anti_aliasing,
                                              uint32_t *width,
                                              uint32_t *height)
{
    *width = logical_width;
    *height = logical_height;
    if (anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2 ||
        anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
        *width *= 2u;
    if (anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4)
        *height *= 2u;
}

static int guest_ranges_overlap(uint32_t first_offset, uint64_t first_size,
                                uint32_t second_offset, uint64_t second_size)
{
    const uint64_t first_end = (uint64_t)first_offset + first_size;
    const uint64_t second_end = (uint64_t)second_offset + second_size;
    return first_size && second_size &&
           (uint64_t)first_offset < second_end &&
           (uint64_t)second_offset < first_end;
}

static int download_guest_depth_surface_to_vram(GuestDepthSurface *surface)
{
    NV2AState *d = nv2a_get_state();
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t guest_width, guest_height;
    uint32_t bytes_per_pixel;
    uint32_t y;
    HRESULT hr;

    if (!surface || !surface->valid || !surface->texture ||
        !surface->write_serial ||
        surface->downloaded_serial == surface->write_serial)
        return 1;
    if (!d || !d->vram_ptr || !device || !context)
        return 0;

    guest_surface_backing_dimensions(surface->logical_width,
        surface->logical_height, surface->anti_aliasing,
        &guest_width, &guest_height);
    bytes_per_pixel = surface->format ==
        NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ? 4u : 2u;
    if (!guest_width || !guest_height ||
        surface->pitch < guest_width * bytes_per_pixel ||
        (uint64_t)surface->offset +
            (uint64_t)surface->pitch * guest_height >
                memory_region_size(d->vram))
        return 0;

    ID3D11Texture2D_GetDesc(surface->texture, &desc);
    desc.MipLevels = 1u;
    desc.ArraySize = 1u;
    desc.SampleDesc.Count = 1u;
    desc.SampleDesc.Quality = 0u;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr) || !staging)
        goto done;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)surface->texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0u,
                                 D3D11_MAP_READ, 0u, &mapped);
    if (FAILED(hr))
        goto done;

    for (y = 0u; y < guest_height; ++y) {
        const uint32_t source_y = (uint32_t)((uint64_t)y * desc.Height /
                                             guest_height);
        const uint8_t *source_row = (const uint8_t *)mapped.pData +
            (size_t)(source_y < desc.Height ? source_y : desc.Height - 1u) *
                mapped.RowPitch;
        uint8_t *guest_row = d->vram_ptr + surface->offset +
            (size_t)y * surface->pitch;
        uint32_t x;
        if (bytes_per_pixel == 4u) {
            for (x = 0u; x < guest_width; ++x) {
                const uint32_t source_x = (uint32_t)((uint64_t)x * desc.Width /
                                                     guest_width);
                uint32_t host_value;
                uint32_t guest_value;
                memcpy(&host_value, source_row +
                    (size_t)(source_x < desc.Width ? source_x : desc.Width - 1u) *
                        4u, 4u);
                /* D3D11 stores D24S8 as depth[23:0] | stencil[31:24]. Xbox
                 * Z24S8 exposes depth[31:8] | stencil[7:0] in guest memory. */
                guest_value = (host_value << 8) | (host_value >> 24);
                memcpy(guest_row + (size_t)x * 4u, &guest_value, 4u);
            }
        } else {
            for (x = 0u; x < guest_width; ++x) {
                const uint32_t source_x = (uint32_t)((uint64_t)x * desc.Width /
                                                     guest_width);
                memcpy(guest_row + (size_t)x * 2u,
                    source_row +
                        (size_t)(source_x < desc.Width ? source_x :
                                 desc.Width - 1u) * 2u,
                    2u);
            }
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
    surface->downloaded_serial = surface->write_serial;
    if (pgraph_trace_surface_textures_enabled())
        fprintf(stderr,
                "[PGRAPH-SURFACE-DOWNLOAD] zeta off=%08X guest=%ux%u "
                "host=%ux%u serial=%llu\n",
                surface->offset, guest_width, guest_height,
                desc.Width, desc.Height,
                (unsigned long long)surface->write_serial);

done:
    if (staging)
        ID3D11Texture2D_Release(staging);
    return surface->downloaded_serial == surface->write_serial;
}

static void download_guest_surfaces_in_range_if_dirty(uint32_t offset,
                                                       uint64_t size)
{
    uint32_t i;
    for (i = 0u; i < MAX_GUEST_DEPTH_SURFACES; ++i) {
        GuestDepthSurface *surface = &g_pg.depth_surfaces[i];
        uint32_t guest_width, guest_height;
        uint64_t surface_size;
        if (!surface->valid || !surface->write_serial ||
            surface->downloaded_serial == surface->write_serial)
            continue;
        guest_surface_backing_dimensions(surface->logical_width,
            surface->logical_height, surface->anti_aliasing,
            &guest_width, &guest_height);
        (void)guest_width;
        surface_size = (uint64_t)surface->pitch * guest_height;
        if (guest_ranges_overlap(offset, size, surface->offset, surface_size))
            download_guest_depth_surface_to_vram(surface);
    }
}
static ID3D11ShaderResourceView *get_guest_surface_texture(
    uint32_t offset, uint32_t texture_format, uint32_t image_rect,
    uint32_t control0, uint32_t control1)
{
    GuestColorSurface *surface = find_guest_color_surface(offset);
    GuestDepthSurface *newer_depth = find_guest_depth_surface(offset);
    static uint32_t trace_count;
    /* Color and zeta share Xbox VRAM, but a live GPU color surface is not
     * interchangeable with an independently bound zeta surface solely
     * because the latter has the newer host write serial.  In particular,
     * Mercenaries keeps its resolved camera color at the same base address as
     * an AA zeta allocation and later samples that color for the satellite
     * filter.  Replacing it with packed depth produces the neon green/cyan
     * full-screen corruption.  A color view which has never been rendered is
     * only an alias placeholder, however, and may be populated from the newer
     * depth writer.  Explicit depth-copy draws remain handled by
     * sync_guest_depth_alias_from_depth and carry their own provenance. */
    if (surface && newer_depth && newer_depth->valid &&
        !surface->gpu_drawn &&
        newer_depth->write_serial > surface->write_serial &&
        newer_depth->format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 &&
        surface->texture && surface->rtv &&
        !guest_color_surface_aliases_bound(surface) &&
        guest_color_surface_texture_shape_compatible(
            surface, texture_format, image_rect, control0, control1)) {
        const uint32_t color_format =
            (texture_format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
        uint32_t texture_width, texture_height;
        uint32_t depth_backing_width, depth_backing_height;
        uint32_t texture_pitch = control1 >> 16;
        get_guest_texture_dimensions(texture_format, image_rect, color_format,
                                     &texture_width, &texture_height);
        guest_surface_backing_dimensions(newer_depth->logical_width,
            newer_depth->logical_height, newer_depth->anti_aliasing,
            &depth_backing_width, &depth_backing_height);
        if (texture_pitch < texture_width * 4u)
            texture_pitch = texture_width * 4u;
        if (color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 &&
            texture_width == depth_backing_width &&
            texture_height == depth_backing_height &&
            texture_pitch == newer_depth->pitch &&
            d3d8_CopyScaledDepthStencilAlias(
                newer_depth->texture, newer_depth->depth_srv,
                newer_depth->stencil_srv,
                newer_depth->width, newer_depth->height,
                NULL, surface->rtv, surface->width, surface->height,
                newer_depth->uniform_stencil_known,
                newer_depth->uniform_stencil_value,
                newer_depth->possible_stencil_bits, FALSE)) {
            static uint32_t refresh_trace_count;
            surface->drawn = 1;
            surface->gpu_drawn = 1;
            surface->write_serial = newer_depth->write_serial;
            surface->depth_alias_source_offset = newer_depth->offset;
            surface->depth_alias_source_serial = newer_depth->write_serial;
            newer_depth->color_alias_serial = surface->write_serial;
            if (pgraph_trace_surface_textures_enabled() &&
                refresh_trace_count++ < 32u)
                fprintf(stderr,
                        "[PGRAPH-SURFACE-DEPTH-SAMPLE] zeta=%08X %ux%u "
                        "-> color=%08X %ux%u serial=%llu\n",
                        newer_depth->offset, newer_depth->width,
                        newer_depth->height, surface->offset,
                        surface->width, surface->height,
                        (unsigned long long)surface->write_serial);
        }
    }
    if (pgraph_trace_surface_textures_enabled() && trace_count < 512u) {
        uint32_t i;
        GuestColorSurface *containing = NULL;
        for (i = 0; i < MAX_GUEST_COLOR_SURFACES; ++i) {
            GuestColorSurface *candidate = &g_pg.color_surfaces[i];
            uint32_t guest_width, guest_height;
            uint64_t end;
            guest_surface_backing_dimensions(candidate->logical_width,
                candidate->logical_height, candidate->anti_aliasing,
                &guest_width, &guest_height);
            (void)guest_width;
            end = (uint64_t)candidate->offset +
                  (uint64_t)candidate->pitch * guest_height;
            if (candidate->valid && offset >= candidate->offset &&
                (uint64_t)offset < end) {
                containing = candidate;
                break;
            }
        }
        if (surface || containing) {
            fprintf(stderr,
                    "[PGRAPH-SURFACE-TEX] off=%08X exact=%08X within=%08X "
                    "drawn=%d gpu=%d bound=%08X gen=%u/%u\n",
                    offset, surface ? surface->offset : 0u,
                    containing ? containing->offset : 0u,
                    surface ? surface->drawn : 0,
                    surface ? surface->gpu_drawn : 0,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                    surface ? surface->draw_generation : 0u,
                    g_pg.surface_generation);
            ++trace_count;
        }
    }
    if (!surface || !surface->gpu_drawn ||
        guest_color_surface_aliases_bound(surface) ||
        !guest_color_surface_texture_shape_compatible(
            surface, texture_format, image_rect, control0, control1)) {
        static uint32_t reject_trace_count;
        if (surface && surface->gpu_drawn &&
            !guest_color_surface_aliases_bound(surface) &&
            pgraph_cached_getenv(
                "MERCENARIES_TRACE_SURFACE_TEXTURE_REJECTS") != NULL &&
            reject_trace_count++ < 512u) {
            const uint32_t color_format =
                (texture_format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
            uint32_t width = 0u, height = 0u;
            get_guest_texture_dimensions(texture_format, image_rect,
                                         color_format, &width, &height);
            fprintf(stderr,
                    "[PGRAPH-SURFACE-TEX-REJECT] off=%08X surf=%ux%u "
                    "pitch=%u fmt=%u resolve=%ux%u/%u created=%u "
                    "depth_alias=%08X tex=%ux%u pitch=%u fmt=%u levels=%u "
                    "cube=%u\n",
                    offset, surface->logical_width, surface->logical_height,
                    surface->pitch, surface->format,
                    surface->resolve_source_width,
                    surface->resolve_source_height,
                    surface->resolve_source_pitch, surface->created_draw,
                    surface->depth_alias_source_offset, width, height,
                    control1 >> 16, color_format,
                    get_guest_texture_levels(texture_format, color_format,
                                             control0),
                    (texture_format &
                     NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) != 0u);
        }
        return NULL;
    }
    return surface->srv;
}

static GuestDepthSurface *sampled_packed_depth_copy_source(
    const GuestColorSurface *target)
{
    static uint32_t trace_count;
    uint32_t stage;

    /* Mercenaries' depth reduction is an unblended, depth-disabled draw from
     * an Xbox Z24S8 allocation exposed as linear A8R8G8B8. Record provenance
     * only for that concrete shape instead of treating address overlap alone
     * as proof of a depth copy. */
    if (!target)
        return NULL;
    for (stage = 0u; stage < 4u; ++stage) {
        GuestDepthSurface *source;
        GuestColorSurface *color_source;
        uint32_t width, height;
        uint32_t backing_width, backing_height;
        uint32_t pitch;
        const uint32_t color_format =
            (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;

        if (!g_pg.tex[stage].enabled ||
            (g_pg.tex[stage].format &
                NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) != 0u ||
            get_guest_texture_levels(g_pg.tex[stage].format, color_format,
                                     g_pg.tex[stage].control0) > 1u)
            continue;
        source = find_guest_depth_surface(resolved_texture_offset(stage));
        color_source = find_guest_color_surface(
            resolved_texture_offset(stage));
        if (!source || !source->valid || !source->write_serial ||
            source->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
            source->offset == target->offset)
            continue;
        get_guest_texture_dimensions(g_pg.tex[stage].format,
            g_pg.tex[stage].image_rect, color_format, &width, &height);
        guest_surface_backing_dimensions(source->logical_width,
            source->logical_height, source->anti_aliasing,
            &backing_width, &backing_height);
        pitch = g_pg.tex[stage].control1 >> 16;
        if (pitch < width * 4u)
            pitch = width * 4u;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_ALIASES") != NULL &&
            trace_count++ < 64u) {
            fprintf(stderr,
                    "[PGRAPH-DEPTH-COPY-CANDIDATE] target=%08X stage=%u "
                    "source=%08X fmt=%u tex=%ux%u/%u backing=%ux%u/%u "
                    "depth=%d blend=%d\n",
                    target->offset, stage, source->offset,
                    color_format, width, height, pitch, backing_width,
                    backing_height, source->pitch, g_pg.depth_test,
                    g_pg.blend_enable);
        }
            if (color_format !=
                    NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
            g_pg.blend_enable || g_pg.depth_test)
            continue;
        if (width == backing_width && height == backing_height &&
            pitch == source->pitch) {
            /* Xemu resolves a sampled VRAM address through its one current
             * SurfaceBinding.  Our color and zeta caches are separate, so an
             * address can appear in both.  A compatible GPU-rendered color
             * surface is the current binding and must win over the parallel
             * zeta entry (Mercenaries' satellite camera at 0x01E34000).
             * With no such color surface, the texture fallback reads the
             * packed zeta payload (the title sky's 0x02538000 copy). */
            if (color_source && color_source->valid &&
                color_source->gpu_drawn &&
                guest_color_surface_texture_shape_compatible(
                    color_source, g_pg.tex[stage].format,
                    g_pg.tex[stage].image_rect,
                    g_pg.tex[stage].control0,
                    g_pg.tex[stage].control1)) {
                if (pgraph_cached_getenv(
                        "MERCENARIES_TRACE_SURFACE_ALIASES") != NULL) {
                    fprintf(stderr,
                            "[PGRAPH-DEPTH-COPY-COLOR-WINS] target=%08X "
                            "stage=%u source=%08X color_serial=%llu "
                            "depth_serial=%llu\n",
                            target->offset, stage, source->offset,
                            (unsigned long long)color_source->write_serial,
                            (unsigned long long)source->write_serial);
                }
                continue;
            }
            return source;
        }
    }
    return NULL;
}

static GuestDepthSurface *prepare_draw_render_targets(int want_depth)
{
    GuestColorSurface *pending =
        find_guest_color_surface(g_pg.surface_color_offset);
    GuestDepthSurface *source =
        sampled_packed_depth_copy_source(pending);

    /* Color and zeta bindings can name the same Xbox allocation.  Once that
     * allocation is reused by an ordinary color pass, its earlier packed-
     * depth identity has ended.  Clear the marker before render-target setup:
     * prepare_guest_render_targets may immediately bind the same allocation
     * as zeta and synchronize aliases, which is too early for post-draw
     * invalidation.  The concrete depth-copy producer keeps its marker. */
    if (pending && !source)
        pending->depth_alias_source_offset = 0u;
    if (pending && !source)
        pending->depth_alias_source_serial = 0u;

    /* Depth and stencil tests are independent. The shadow composite disables
     * Z but still needs its stencil mask; unbinding zeta makes the entire
     * fullscreen darkening quad pass instead of only shadowed pixels. */
    prepare_guest_render_targets(want_depth || g_pg.stencil_enable);
    if (g_pg.bound_color != pending)
        source = sampled_packed_depth_copy_source(g_pg.bound_color);
    return source;
}

static ID3D11ShaderResourceView *snapshot_bound_color_surface(
    GuestColorSurface *surface, ID3D11Texture2D **snapshot_texture)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;

    if (snapshot_texture)
        *snapshot_texture = NULL;
    if (!surface || surface != g_pg.bound_color || !device || !context ||
        !snapshot_texture)
        return NULL;

    ID3D11Texture2D_GetDesc(surface->texture, &desc);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0u;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    if (FAILED(hr) || !texture)
        goto fail;

    /* Xbox titles may sample the active color allocation during a fullscreen
     * post-process pass. D3D11 forbids the same resource being an RTV and SRV,
     * so snapshot it while temporarily unbound, then restore the retail render
     * target and logical AA viewport. */
    d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)texture,
                                    (ID3D11Resource *)surface->texture);
    d3d8_BindRenderTargets(surface->rtv,
                           g_pg.bound_depth ? g_pg.bound_depth->dsv : NULL,
                           surface->width, surface->height);
    d3d8_SetLogicalRenderTargetSize(surface->logical_width,
                                    surface->logical_height);
    hr = ID3D11Device_CreateShaderResourceView(
        device, (ID3D11Resource *)texture, NULL, &srv);
    if (FAILED(hr) || !srv)
        goto fail;

    *snapshot_texture = texture;    return srv;

fail:
    if (srv)
        ID3D11ShaderResourceView_Release(srv);
    if (texture)
        ID3D11Texture2D_Release(texture);
    return NULL;
}

static GuestDepthSurface *satellite_depth_filter_source(uint32_t stage,
                                                        uint32_t color_format,
                                                        const char **reason)
{
    GuestDepthSurface *source;
    uint32_t width, height;
    uint32_t backing_width, backing_height;
    uint32_t pitch;

    /* This is the single confirmed Mercenaries satellite reduction pass.
     * Do not turn this into a general depth-as-texture binding rule: title
     * and gameplay sky draws use superficially similar Z24S8 declarations
     * but require their established CPU/explicit-copy lifetime. Runs 1710,
     * 1711, and 1716 proved that intercepting those ordinary binds blacks the
     * sky and can resurrect the menu framebuffer. */
    /* Keep the run 1717 predicate unchanged; expose its first rejection. */
    *reason = "stage";
    if (stage != 0u) return NULL;
    *reason = "color-format";
    if (color_format != NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8)
        return NULL;
    *reason = "target-missing";
    if (!g_pg.bound_color) return NULL;
    *reason = "target-offset";
    if (g_pg.bound_color->offset != 0x02980000u) return NULL;
    *reason = "source-offset";
    if (resolved_texture_offset(stage) != 0x01E34000u) return NULL;
    *reason = "vertex-shader";
    if (g_pg.host_vsh_hash != 0x1730DD1Au) return NULL;
    *reason = "primitive-mode";
    if (g_pg.draw_mode != 8u) return NULL;
    *reason = "stage-program";
    if (g_pg.shader_stage_program != 0x00000001u) return NULL;
    *reason = "other-stage-input";
    if (g_pg.shader_other_stage_input != 0x00210000u) return NULL;
    *reason = "dot-mapping";
    if (g_pg.shader_dot_mapping != 0u) return NULL;
    *reason = "blend-enabled";
    if (g_pg.blend_enable) return NULL;
    *reason = "depth-enabled";
    if (g_pg.depth_test) return NULL;

    source = find_guest_depth_surface(resolved_texture_offset(stage));
    *reason = "zeta-missing";
    if (!source) return NULL;
    *reason = "zeta-invalid";
    if (!source->valid) return NULL;
    *reason = "zeta-unwritten";
    if (!source->write_serial) return NULL;
    *reason = "zeta-format";
    if (source->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) return NULL;
    get_guest_texture_dimensions(g_pg.tex[stage].format,
        g_pg.tex[stage].image_rect, color_format, &width, &height);
    guest_surface_backing_dimensions(source->logical_width,
        source->logical_height, source->anti_aliasing,
        &backing_width, &backing_height);
    pitch = g_pg.tex[stage].control1 >> 16;
    if (pitch < width * 4u)
        pitch = width * 4u;
    *reason = "texture-width";
    if (width != backing_width) return NULL;
    *reason = "texture-height";
    if (height != backing_height) return NULL;
    *reason = "texture-pitch";
    if (pitch != source->pitch) return NULL;
    *reason = "matched";
    return source;
}

static ID3D11ShaderResourceView *prepare_satellite_depth_sample(
    GuestDepthSurface *source)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;

    if (!source || !device || !g_pg.bound_color ||
        !g_pg.bound_color->width || !g_pg.bound_color->height)
        return NULL;
    if (g_pg.satellite_depth_sample_texture &&
        (g_pg.satellite_depth_sample_width != g_pg.bound_color->width ||
         g_pg.satellite_depth_sample_height != g_pg.bound_color->height))
        release_satellite_depth_sample();
    if (!g_pg.satellite_depth_sample_texture) {
        memset(&desc, 0, sizeof(desc));
        desc.Width = g_pg.bound_color->width;
        desc.Height = g_pg.bound_color->height;
        desc.MipLevels = 1u;
        desc.ArraySize = 1u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1u;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                         D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL,
            &g_pg.satellite_depth_sample_texture);
        if (FAILED(hr))
            goto fail;
        hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)g_pg.satellite_depth_sample_texture, NULL,
            &g_pg.satellite_depth_sample_rtv);
        if (FAILED(hr))
            goto fail;
        hr = ID3D11Device_CreateShaderResourceView(device,
            (ID3D11Resource *)g_pg.satellite_depth_sample_texture, NULL,
            &g_pg.satellite_depth_sample_srv);
        if (FAILED(hr))
            goto fail;
        g_pg.satellite_depth_sample_width = g_pg.bound_color->width;
        g_pg.satellite_depth_sample_height = g_pg.bound_color->height;
    }
    if (!d3d8_CopyScaledDepthStencilAlias(
            source->texture, source->depth_srv, source->stencil_srv,
            source->width, source->height, NULL,
            g_pg.satellite_depth_sample_rtv,
            g_pg.satellite_depth_sample_width,
            g_pg.satellite_depth_sample_height,
            source->uniform_stencil_known,
            source->uniform_stencil_value,
            source->possible_stencil_bits, FALSE))
        return NULL;
    ID3D11ShaderResourceView_AddRef(g_pg.satellite_depth_sample_srv);
    return g_pg.satellite_depth_sample_srv;

fail:
    fprintf(stderr,
            "[PGRAPH-SATELLITE-DEPTH] scratch create failed hr=%08lX\n",
            hr);
    release_satellite_depth_sample();
    return NULL;
}

/* The non-AA renderer samples its active Z24S8 buffer directly as ABGR.
 * Unlike AA reduction, this is not a persistent color render target: using
 * the color cache (or untouched CPU clear bytes) loses the current depth.
 * Keep this view transient so later color writes cannot be fed back into zeta. */
static GuestDepthSurface *direct_bound_depth_sample_source(uint32_t stage)
{
    GuestDepthSurface *source = g_pg.bound_depth;
    const uint32_t color_format =
        (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    uint32_t width, height, pitch;
    if (!g_pg.tex[stage].enabled || !source || !source->valid ||
        !source->write_serial || !g_pg.bound_color ||
        source->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        source->anti_aliasing != 0u || !g_pg.depth_test || g_pg.depth_write ||
        resolved_texture_offset(stage) != source->offset ||
        g_pg.bound_color->offset == source->offset ||
        color_format != NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 ||
        (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) ||
        get_guest_texture_levels(g_pg.tex[stage].format, color_format,
                                 g_pg.tex[stage].control0) > 1u)
        return NULL;
    get_guest_texture_dimensions(g_pg.tex[stage].format,
        g_pg.tex[stage].image_rect, color_format, &width, &height);
    pitch = g_pg.tex[stage].control1 >> 16;
    if (pitch < width * 4u) pitch = width * 4u;
    if (width != source->logical_width || height != source->logical_height ||
        pitch != source->pitch)
        return NULL;
    return source;
}

/* Title compatibility: xboxSky.vsh uses oPos.xy as the depth-map address.
 * Its visible vertices normally share the fog-plane W, but triangles crossing
 * the camera plane do not. Preserve screen registration for that one shader;
 * ordinary projective textures and every lighting pass retain guest UVs. */
static int pgraph_sky_screen_depth_stage(const NV2ACombinerState *state)
{
    GuestDepthSurface *depth = g_pg.bound_depth;
    GuestColorSurface *color = g_pg.bound_color;
    uint32_t width, height;
    const uint32_t format = (g_pg.tex[1].format &
        NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    if (g_pg.host_vsh_hash != 0x47F668F0u || !depth || !color ||
        !depth->valid || !depth->write_serial || !g_pg.depth_test ||
        g_pg.depth_write || depth->width != color->width ||
        depth->height != color->height ||
        resolved_texture_offset(1u) != depth->offset ||
        !g_pg.tex[1].enabled ||
        format != NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 ||
        state->tex_mode[1] != NV2A_TEXMODE_2D ||
        state->tex_mode[2] != NV2A_TEXMODE_DPNDNT_GB ||
        state->input_tex[2] != 1)
        return 0;
    get_guest_texture_dimensions(g_pg.tex[1].format,
        g_pg.tex[1].image_rect, format, &width, &height);
    return width == color->logical_width &&
           height == color->logical_height ? 2 : 0;
}

static void note_guest_surface_resolve(const char *draw_kind)
{
    GuestColorSurface *source = find_guest_color_surface(
        resolved_texture_offset(0));
    GuestColorSurface *target = g_pg.bound_color;
    static uint32_t resolve_log_count;
    if (!source || !target || source == target || !source->drawn)
        return;
    if (source->width <= target->width && source->height <= target->height)
        return;
    guest_surface_backing_dimensions(source->logical_width,
        source->logical_height, source->anti_aliasing,
        &target->resolve_source_width, &target->resolve_source_height);
    target->resolve_source_pitch = source->pitch;
    if (target->logical_width >= 640u && target->logical_height >= 480u)
        g_pg.last_resolved_color = target;

    if (g_debug_post_pda_resolve_trace_active &&
        g_debug_post_pda_resolve_trace_index < 16u) {
        const uint32_t index = g_debug_post_pda_resolve_trace_index++;
        fprintf(stderr,
                "[PGRAPH-POST-PDA-RESOLVE] index=%u kind=%s draw=%u gen=%u "
                "source=%08X/%ux%u/logical=%ux%u/aa=%u/drawn=%d/gpu=%d/"
                "drawgen=%u/write=%llu target=%08X/%ux%u/logical=%ux%u/"
                "aa=%u/drawgen=%u/write=%llu tex0=%08X/%08X fmt=%08X "
                "ctl0=%08X ctl1=%08X rect=%08X filter=%08X address=%08X "
                "mode=%u prim=%d vsh=%08X blend=%d/%04X/%04X/%04X "
                "depth=%d/%d/%u alpha=%d/%u mask=%08X deferred=%d\n",
                index, draw_kind ? draw_kind : "unknown",
                g_pg.stats.draw_calls + 1u, g_pg.surface_generation,
                source->offset, source->width, source->height,
                source->logical_width, source->logical_height,
                source->anti_aliasing, source->drawn, source->gpu_drawn,
                source->draw_generation,
                (unsigned long long)source->write_serial,
                target->offset, target->width, target->height,
                target->logical_width, target->logical_height,
                target->anti_aliasing, target->draw_generation,
                (unsigned long long)target->write_serial,
                g_pg.tex[0].offset, resolved_texture_offset(0),
                g_pg.tex[0].format, g_pg.tex[0].control0,
                g_pg.tex[0].control1, g_pg.tex[0].image_rect,
                g_pg.tex[0].filter, g_pg.tex[0].address,
                g_pg.draw_mode, g_pg.d3d_prim_type, g_pg.host_vsh_hash,
                g_pg.blend_enable, g_pg.blend_sfactor,
                g_pg.blend_dfactor, g_pg.blend_equation,
                g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                g_pg.alpha_test, g_pg.alpha_func, g_pg.color_mask,
                g_pg.deferred_flip);
        {
            const char *prefix = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_POST_PDA_RESOLVE_PREFIX");
            char path[MAX_PATH];
            if (prefix && *prefix) {
                snprintf(path, sizeof(path), "%s-%02u-target.bmp",
                         prefix, index);
                d3d8_DebugCaptureTextureToPath(target->texture, path);
                snprintf(path, sizeof(path), "%s-%02u-source.bmp",
                         prefix, index);
                d3d8_DebugCaptureTextureToPath(source->texture, path);            }
        }
    }
    if (resolve_log_count < 20u) {
        fprintf(stderr,
                "[PGRAPH-SURFACE] resolve %08X %ux%u -> %08X %ux%u\n",
                source->offset, source->width, source->height,
                target->offset, target->width, target->height);
    }
    ++resolve_log_count;
    if (g_pg.deferred_flip) {
        g_pg.deferred_flip = 0;
        g_pg.frame_had_draw = 0;
        g_pg.last_resolved_color = NULL;
        ++g_pg.surface_generation;
        g_pg.pending_scanout = target;
        g_pg.pending_scanout_write_us = pgraph_scanout_clock_us();
    }
}
static BOOL composite_pvideo_overlay(GuestColorSurface *background);
extern void d3d8_DebugSetSubmissionSource(uint32_t source);
static int service_pending_scanout(int force);

static BOOL present_guest_frontbuffer(void)
{
    NV2AState *d = nv2a_get_state();
    const uint32_t scanout = d ? (uint32_t)d->pcrtc.start & 0x07FFFFFFu : 0u;
    GuestColorSurface *surface = find_guest_color_surface(scanout);
    GuestColorSurface *configured = find_guest_color_surface(
        g_pg.surface_color_offset & 0x07FFFFFFu);
    static uint32_t present_log_count;
    static uint32_t present_trace_attempts;
    const char *debug_surface = pgraph_cached_getenv("MERCENARIES_DEBUG_PRESENT_SURFACE");
    int debug_surface_forced = 0;

    if (debug_surface && *debug_surface) {
        GuestColorSurface *requested = find_guest_color_surface(
            (uint32_t)strtoul(debug_surface, NULL, 16) & 0x07FFFFFFu);
        if (requested && requested->drawn) {
            surface = requested;
            debug_surface_forced = 1;
        }
    }

    if (pgraph_trace_present_enabled() && present_trace_attempts < 5000u) {
        fprintf(stderr,
                "[PGRAPH-PRESENT] gen=%u scanout=%08X scan=%08X/%u/%u "
                "resolved=%08X/%u drawn=%08X/%u bound=%08X/%u\n",
                g_pg.surface_generation, scanout,
                surface ? surface->offset : 0u,
                surface ? surface->drawn : 0,
                surface ? surface->draw_generation : 0u,
                g_pg.last_resolved_color ? g_pg.last_resolved_color->offset : 0u,
                g_pg.last_resolved_color ?
                    g_pg.last_resolved_color->draw_generation : 0u,
                g_pg.last_drawn_color ? g_pg.last_drawn_color->offset : 0u,
                g_pg.last_drawn_color ?
                    g_pg.last_drawn_color->draw_generation : 0u,
                g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                g_pg.bound_color ? g_pg.bound_color->draw_generation : 0u);
    }
    ++present_trace_attempts;
    if (!debug_surface_forced &&
        (!surface || !surface->drawn ||
         surface->draw_generation != g_pg.surface_generation))
        surface = NULL;
    /* Xemu synchronizes the active render surface at FLIP_STALL. The title's
     * explicit AA resolve completed the scanout image during this generation,
     * so prefer it over the newly configured back buffer. */
    if (!surface && g_pg.last_resolved_color &&
        g_pg.last_resolved_color->drawn &&
        g_pg.last_resolved_color->draw_generation == g_pg.surface_generation)
        surface = g_pg.last_resolved_color;
    if (!surface && configured && configured->drawn)
        surface = configured;
    if (!surface && g_pg.last_drawn_color &&
        g_pg.last_drawn_color->drawn &&
        g_pg.last_drawn_color->draw_generation == g_pg.surface_generation)
        surface = g_pg.last_drawn_color;
    if (g_debug_post_pda_resolve_trace_active &&
        g_debug_post_pda_present_trace_index < 96u) {
        fprintf(stderr,
                "[PGRAPH-POST-PDA-PRESENT] index=%u gen=%u scanout=%08X "
                "selected=%08X/%d/%u configured=%08X/%d/%u "
                "resolved=%08X/%d/%u last=%08X/%d/%u bound=%08X/%d/%u "
                "pending=%08X/%d/%u deferred=%d had=%d\n",
                g_debug_post_pda_present_trace_index++,
                g_pg.surface_generation, scanout,
                surface ? surface->offset : 0u,
                surface ? surface->drawn : 0,
                surface ? surface->draw_generation : 0u,
                configured ? configured->offset : 0u,
                configured ? configured->drawn : 0,
                configured ? configured->draw_generation : 0u,
                g_pg.last_resolved_color ? g_pg.last_resolved_color->offset : 0u,
                g_pg.last_resolved_color ? g_pg.last_resolved_color->drawn : 0,
                g_pg.last_resolved_color ? g_pg.last_resolved_color->draw_generation : 0u,
                g_pg.last_drawn_color ? g_pg.last_drawn_color->offset : 0u,
                g_pg.last_drawn_color ? g_pg.last_drawn_color->drawn : 0,
                g_pg.last_drawn_color ? g_pg.last_drawn_color->draw_generation : 0u,
                g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                g_pg.bound_color ? g_pg.bound_color->drawn : 0,
                g_pg.bound_color ? g_pg.bound_color->draw_generation : 0u,
                g_pg.pending_scanout ? g_pg.pending_scanout->offset : 0u,
                g_pg.pending_scanout ? g_pg.pending_scanout->drawn : 0,
                g_pg.pending_scanout ? g_pg.pending_scanout->draw_generation : 0u,
                g_pg.deferred_flip, g_pg.frame_had_draw);
    }
    if (!surface || !surface->drawn)
        return FALSE;
    if (!d3d8_CopyTextureToBackbuffer(surface->texture, surface->srv,
                                      surface->width, surface->height))
        return FALSE;
    composite_pvideo_overlay(surface);
    if (pgraph_trace_draws_enabled() &&
        (present_log_count < 20u || (present_log_count % 300u) == 0u)) {
        fprintf(stderr,
                "[PGRAPH-SURFACE] flip scanout=%08X source=%08X %ux%u\n",
                scanout, surface->offset, surface->width, surface->height);
    }
    ++present_log_count;
    return TRUE;
}

static void trace_sky_vertex_candidates(NV2AState *d)
{
    static int scanned;
    const size_t vertex_stride = 24u;
    const size_t sample_vertices = 8u;
    const size_t vram_size = d ? memory_region_size(d->vram) : 0u;
    uint32_t matches = 0;
    size_t offset;

    if (scanned || pgraph_cached_getenv("MERCENARIES_SCAN_SKY_VERTICES") == NULL ||
        !d || !d->vram_ptr)
        return;
    scanned = 1;

    for (offset = 0; offset + sample_vertices * vertex_stride <= vram_size;
         offset += 4u) {
        const uint8_t *p = d->vram_ptr + offset;
        const uint32_t zero_sign_mask = 0x7FFFFFFFu;
        float radius0, radius1;
        int valid = 1;
        uint32_t vertex;

        if ((read_le32(p + 0u) & zero_sign_mask) != 0u ||
            (read_le32(p + 8u) & zero_sign_mask) != 0u ||
            (read_le32(p + 12u) & 0xFFFFu) != 0u ||
            (read_le32(p + 16u) & zero_sign_mask) != 0u ||
            (read_le32(p + 20u) & zero_sign_mask) != 0u ||
            memcmp(p, p + 2u * vertex_stride, 12u) != 0 ||
            memcmp(p, p + 4u * vertex_stride, 12u) != 0 ||
            memcmp(p, p + 6u * vertex_stride, 12u) != 0)
            continue;

        radius0 = fabsf(u2f(read_le32(p + 4u)));
        {
            const float x = u2f(read_le32(p + vertex_stride + 0u));
            const float y = u2f(read_le32(p + vertex_stride + 4u));
            const float z = u2f(read_le32(p + vertex_stride + 8u));
            radius1 = sqrtf(x * x + y * y + z * z);
        }
        if (!isfinite(radius0) || !isfinite(radius1) || radius0 < 10.0f ||
            fabsf(radius0 - radius1) > radius0 * 0.02f)
            continue;

        for (vertex = 0; vertex < sample_vertices; ++vertex) {
            const uint8_t *v = p + (size_t)vertex * vertex_stride;
            const float x = u2f(read_le32(v + 0u));
            const float y = u2f(read_le32(v + 4u));
            const float z = u2f(read_le32(v + 8u));
            const float radius = sqrtf(x * x + y * y + z * z);
            const uint32_t expected_color = vertex & 1u;
            if (!isfinite(radius) ||
                fabsf(radius - radius0) > radius0 * 0.02f ||
                (read_le32(v + 12u) & 0xFFFFu) != expected_color) {
                valid = 0;
                break;
            }
        }
        if (!valid)
            continue;

        fprintf(stderr,
                "[PGRAPH-D3D11] sky vertex candidate off=%08zX radius=%.3f "
                "v1=(%.3f,%.3f,%.3f)\n",
                offset, radius0,
                u2f(read_le32(p + vertex_stride + 0u)),
                u2f(read_le32(p + vertex_stride + 4u)),
                u2f(read_le32(p + vertex_stride + 8u)));
        if (++matches >= 16u)
            break;
    }
    fprintf(stderr, "[PGRAPH-D3D11] sky vertex scan matches=%u\n", matches);
}

static DXGI_FORMAT vertex_array_dxgi_format(uint32_t format)
{
    const uint32_t type = format & 0xFu;
    const uint32_t size = (format >> 4) & 0xFu;
    static const DXGI_FORMAT float_formats[5] = {
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT
    };
    static const DXGI_FORMAT short_formats[5] = {
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16_SINT,
        DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16B16A16_SINT,
        DXGI_FORMAT_R16G16B16A16_SINT
    };
    static const DXGI_FORMAT snorm_formats[5] = {
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16_SNORM,
        DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
        DXGI_FORMAT_R16G16B16A16_SNORM
    };

    if (size > 4u)
        return DXGI_FORMAT_UNKNOWN;
    switch (type) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        return float_formats[size];
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        return short_formats[size];
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        return snorm_formats[size];
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        return size == 4u ? DXGI_FORMAT_R8G8B8A8_UNORM :
                            DXGI_FORMAT_UNKNOWN;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        return DXGI_FORMAT_R32_SINT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

static uint32_t prepare_inline_array_layout(void)
{
    uint32_t offset = 0u;
    uint32_t i;

    memset(g_pg.inline_array_offsets, 0, sizeof(g_pg.inline_array_offsets));
    for (i = 0u; i < 16u; ++i) {
        const uint32_t format = g_pg.vertex_array[i].format;
        const uint32_t type = format & 0xFu;
        const uint32_t count = (format >> 4) & 0xFu;
        uint32_t component_size;
        if (!count)
            continue;
        switch (type) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            component_size = 1u;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            component_size = 2u;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            component_size = 4u;
            break;
        default:
            return 0u;
        }
        offset = (offset + component_size - 1u) & ~(component_size - 1u);
        g_pg.inline_array_offsets[i] = offset;
        offset += component_size * count;
        offset = (offset + component_size - 1u) & ~(component_size - 1u);
    }
    g_pg.inline_array_vertex_size = offset;
    return offset;
}
/* Program RAM changes only through SET_TRANSFORM_PROGRAM. Keep its key until
 * a word or the entry point changes; constants and input declarations do not
 * change the program bytecode. Shader compilation still includes the layout. */
static uint32_t transform_program_key(uint32_t *length_out)
{
    if (!g_pg.transform_program_key_valid) {
        const uint32_t start = g_pg.transform_program_start;
        uint32_t length = 0u;
        uint32_t hash = 2166136261u;
        const uint8_t *bytes = (const uint8_t *)&g_pg.transform_program[start][0];
        for (uint32_t i = start; i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; ++i) {
            ++length;
            if (g_pg.transform_program[i][3] & 1u)
                break;
        }
        for (size_t i = 0; i < (size_t)length * 4u * sizeof(uint32_t); ++i) {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
        g_pg.transform_program_hash = hash;
        g_pg.transform_program_length = length;
        g_pg.transform_program_key_valid = 1;
    }
    *length_out = g_pg.transform_program_length;
    return g_pg.transform_program_hash;
}

static DWORD prepare_transform_program(uint32_t vertex_base,
                                       uint32_t vertex_stride)
{
    static uint32_t humvee_orientation[6][2];
    static int humvee_orientation_valid;
    static int humvee_repair_logged;
    uint32_t hash;
    uint32_t start = g_pg.transform_program_start;
    uint32_t length;
    uint32_t i;
    DXGI_FORMAT input_formats[NV2A_VS_MAX_INPUTS];
    UINT input_offsets[NV2A_VS_MAX_INPUTS];
    UINT input_components[NV2A_VS_MAX_INPUTS];
    uint32_t input_bgra_mask = 0u;
    uint32_t constant_snapshot[NV2A_VERTEXSHADER_CONSTANTS][4];
    const float *constant_data = (const float *)g_pg.vsh_constants;

    if ((g_pg.transform_execution_mode & 3u) != 2u ||
        start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH)
        return 0;
    hash = transform_program_key(&length);
    if (!g_pg.host_vsh_handle || g_pg.host_vsh_hash != hash) {
        if (g_pg.host_vsh_handle)
            d3d8_vsh_delete_shader(g_pg.host_vsh_handle);
        g_pg.host_vsh_handle = 0;
        if (FAILED(d3d8_vsh_create_shader(
                (const DWORD *)&g_pg.transform_program[start][0],
                (int)length, &g_pg.host_vsh_handle)))
            return 0;
        g_pg.host_vsh_hash = hash;
    }
    /* Diagnostic only: the retail Humvee's pristine rear segment arrives with
     * its X/Y orientation collapsed while adjacent body segments retain a valid
     * orientation. Borrow only those orientation components from the first
     * healthy body draw and preserve the rear segment's Z/translation. */
    if (pgraph_cached_getenv(
            "MERCENARIES_TEST_REPAIR_HUMVEE_PRISTINE_BACK") != NULL &&
        hash == 0xE80E3EFAu) {
        static const uint32_t rows[6] = { 4u, 5u, 6u, 23u, 24u, 25u };
        if (vertex_base == 0x02482000u) {
            for (i = 0u; i < 6u; ++i) {
                humvee_orientation[i][0] = g_pg.vsh_constants[rows[i]][0];
                humvee_orientation[i][1] = g_pg.vsh_constants[rows[i]][1];
            }
            humvee_orientation_valid = 1;
        } else if (vertex_base == 0x02474000u &&
                   humvee_orientation_valid &&
                   g_pg.vsh_constants[4][0] == 0u &&
                   g_pg.vsh_constants[4][1] == 0u) {
            memcpy(constant_snapshot, g_pg.vsh_constants,
                   sizeof(constant_snapshot));
            for (i = 0u; i < 6u; ++i) {
                constant_snapshot[rows[i]][0] = humvee_orientation[i][0];
                constant_snapshot[rows[i]][1] = humvee_orientation[i][1];
            }
            constant_data = (const float *)constant_snapshot;
            if (!humvee_repair_logged) {
                const char *repair_capture_path = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_HUMVEE_REPAIR_PATH");
                const uint32_t recent_end = g_recomp_recent_game_func_idx;
                const uint32_t recent_count = recent_end < 96u ?
                    recent_end : 96u;
                const uint32_t all_end = g_recomp_recent_func_idx;
                const uint32_t all_count = all_end < 64u ? all_end : 64u;
                fprintf(stderr,
                        "[PGRAPH-HUMVEE-REPAIR] restored pristine-back X/Y "
                        "orientation at base=%08X func=%08X\n", vertex_base,
                        g_recomp_current_func);
                fprintf(stderr, "[PGRAPH-HUMVEE-REPAIR-RECENT]");
                for (i = recent_count; i != 0u; --i)
                    fprintf(stderr, " %08X",
                            g_recomp_recent_game_funcs[
                                (recent_end - i) & 255u]);
                fputc('\n', stderr);
                fprintf(stderr, "[PGRAPH-HUMVEE-REPAIR-ALL]");
                for (i = all_count; i != 0u; --i)
                    fprintf(stderr, " %08X",
                            g_recomp_recent_funcs[(all_end - i) & 63u]);
                fputc('\n', stderr);
                if (repair_capture_path && repair_capture_path[0])
                    g_debug_humvee_repair_capture_pending = 1;
                humvee_repair_logged = 1;
            }
        }
    }
    /* XDK's private pass-through shader for D3DFVF_XYZRHW uses c0/c1 to
     * preserve already-transformed screen coordinates.  Around an AA
     * Present the retail driver can leave the resolve viewport's half-height
     * Y bias in c1 until after the next push-buffer submission.  Native D3D
     * executes submissions synchronously, so applying that stale private
     * bias moves an otherwise absolute 0..height quad down by height/2.
     * Honor the public XYZRHW contract at this boundary: for the canonical
     * unit-scale pass-through shader, X and Y use the same half-pixel bias.
     * Keep the guest constant file untouched because the following push may
     * still depend on its exact hardware-visible contents. */
    if ((g_pg.guest_fvf & 0x000Eu) == D3DFVF_XYZRHW &&
        g_pg.vsh_constants[0][0] == 0x3F800000u &&
        g_pg.vsh_constants[0][1] == 0x3F800000u &&
        g_pg.vsh_constants[1][0] == 0xBF080000u &&
        (u2f(g_pg.vsh_constants[1][1]) > 1.0f ||
         u2f(g_pg.vsh_constants[1][1]) < -1.0f)) {
        memcpy(constant_snapshot, g_pg.vsh_constants,
               sizeof(constant_snapshot));
        constant_snapshot[1][1] = constant_snapshot[1][0];
        constant_data = (const float *)constant_snapshot;
    }
    /* RedCanvas::_EndMask samples a same-coordinate screen copy. The retail
     * 0.53125 bias rounds to half a guest pixel: native raster samples still
     * cover [left,right), but upscaling magnifies the bias past the alpha-clear
     * rectangle. Align this identity copy to host pixel edges so its source,
     * mask clear and destination address the same pixels. Keep other canvas
     * geometry and the guest constant file unchanged. */
    if (hash == 0x1730DD1Au && g_pg.blend_enable &&
        g_pg.blend_sfactor == 0x0304u && g_pg.blend_dfactor == 0x0305u &&
        g_pg.bound_color &&
        (g_pg.bound_color->width > g_pg.bound_color->logical_width ||
         g_pg.bound_color->height > g_pg.bound_color->logical_height) &&
        g_pg.vsh_constants[28][0] == 0x3F080000u &&
        g_pg.vsh_constants[28][1] == 0x3F080000u) {
        GuestColorSurface *copy = find_guest_color_surface(resolved_texture_offset(0u));
        if (copy && copy->gpu_drawn && !copy->depth_alias_source_offset &&
            copy->logical_width == g_pg.bound_color->logical_width &&
            copy->logical_height == g_pg.bound_color->logical_height) {
            memcpy(constant_snapshot, g_pg.vsh_constants, sizeof(constant_snapshot));
            constant_snapshot[28][0] = constant_snapshot[28][1] = 0x3D000000u;
            constant_data = (const float *)constant_snapshot;
        }
    }
    d3d8_vsh_set_constant(0, constant_data, NV2A_VERTEXSHADER_CONSTANTS);
    {
        const char *trace_hash_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_VSH_STATE_HASH");
        const char *trace_base_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_VSH_STATE_BASE");
        const char *trace_after_standup_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_VSH_STATE_AFTER_STANDUP");
        static uint32_t traced_program_states;
        const uint32_t trace_hash = trace_hash_env ?
            (uint32_t)strtoul(trace_hash_env, NULL, 16) : 0u;
        const uint32_t trace_base = trace_base_env ?
            (uint32_t)strtoul(trace_base_env, NULL, 16) : 0u;
        if (trace_hash_env && traced_program_states < 16u && hash == trace_hash &&
            (!trace_after_standup_env ||
             g_mercenaries_standup_complete != 0u) &&
            (!trace_base_env || vertex_base == trace_base)) {
            fprintf(stderr,
                    "[PGRAPH-VSH-PROGRAM-STATE] sample=%u draw=%u hash=%08X "
                    "target=%08X comb=%08X stage=%08X final=%08X/%08X\n",
                    ++traced_program_states, g_pg.stats.draw_calls + 1u, hash,
                    g_pg.surface_color_offset, g_pg.combiner_control,
                    g_pg.shader_stage_program, g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1);
            for (i = 0u; i < 43u; ++i) {
                const uint32_t *c = g_pg.vsh_constants[i];
                fprintf(stderr,
                        "  c%u=%08X %08X %08X %08X "
                        "(%.9g,%.9g,%.9g,%.9g)\n",
                        i, c[0], c[1], c[2], c[3],
                        u2f(c[0]), u2f(c[1]), u2f(c[2]), u2f(c[3]));
            }
            for (i = 17u; i <= 18u; ++i) {
                const uint32_t *c = g_pg.vsh_constants[i];
                fprintf(stderr,
                        "  c%u=%08X %08X %08X %08X "
                        "(%.9g,%.9g,%.9g,%.9g)\n",
                        i, c[0], c[1], c[2], c[3],
                        u2f(c[0]), u2f(c[1]), u2f(c[2]), u2f(c[3]));
            }
            for (i = 0u; i < 7u; ++i) {
                const uint32_t format = g_pg.vertex_array[i].format;
                fprintf(stderr,
                        "  array%u off=%08X fmt=%08X type=%u count=%u "
                        "stride=%u resolved=%08X\n",
                        i, g_pg.vertex_array[i].offset, format,
                        format & 0xFu, (format >> 4) & 0xFu,
                        (format >> 8) & 0xFFu,
                        resolved_vertex_array_offset(i));
            }
            for (i = 0u; i < (g_pg.combiner_control & 0xffu) && i < 8u; ++i) {
                fprintf(stderr,
                        "  rc%u rgb=%08X/%08X alpha=%08X/%08X "
                        "factor=%08X/%08X\n",
                        i, g_pg.combiner_color_icw[i],
                        g_pg.combiner_color_ocw[i],
                        g_pg.combiner_alpha_icw[i],
                        g_pg.combiner_alpha_ocw[i],
                        g_pg.combiner_factor0[i], g_pg.combiner_factor1[i]);
            }
            for (i = 0u; i < 4u; ++i) {
                fprintf(stderr,
                        "  tex%u enabled=%d off=%08X fmt=%08X rect=%08X "
                        "ctl0=%08X ctl1=%08X filter=%08X\n",
                        i, g_pg.tex[i].enabled, g_pg.tex[i].offset,
                        g_pg.tex[i].format, g_pg.tex[i].image_rect,
                        g_pg.tex[i].control0, g_pg.tex[i].control1,
                        g_pg.tex[i].filter);
            }
            fprintf(stderr, "  final_factor=%08X/%08X other=%08X dot=%08X\n",
                    g_pg.combiner_final_factor[0],
                    g_pg.combiner_final_factor[1],
                    g_pg.shader_other_stage_input, g_pg.shader_dot_mapping);
            fflush(stderr);
        }
    }
    d3d8_vsh_set_inline_values(g_pg.immediate_vertex_attr);
    memset(input_formats, 0, sizeof(input_formats));
    memset(input_offsets, 0, sizeof(input_offsets));
    memset(input_components, 0, sizeof(input_components));
    for (i = 0; i < NV2A_VS_MAX_INPUTS; ++i) {
        if (g_pg.immediate_buffer_mode) {
            input_formats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            input_offsets[i] = i * 4u * sizeof(float);
            input_components[i] = 4u;
            continue;
        }
        if (g_pg.inline_array_mode) {
            input_formats[i] =
                vertex_array_dxgi_format(g_pg.vertex_array[i].format);
            input_offsets[i] = g_pg.inline_array_offsets[i];
            input_components[i] =
                (g_pg.vertex_array[i].format >> 4) & 0xFu;
            if ((g_pg.vertex_array[i].format & 0xFu) ==
                NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D)
                input_bgra_mask |= 1u << i;
            continue;
        }
        const uint32_t attribute_offset = resolved_vertex_array_offset(i);
        const uint32_t attribute_stride =
            (g_pg.vertex_array[i].format &
             NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE) >> 8;
        if (attribute_stride == vertex_stride &&
            attribute_offset >= vertex_base &&
            attribute_offset - vertex_base < vertex_stride) {
            input_formats[i] =
                vertex_array_dxgi_format(g_pg.vertex_array[i].format);
            input_offsets[i] = attribute_offset - vertex_base;
            input_components[i] =
                (g_pg.vertex_array[i].format >> 4) & 0xFu;
            if ((g_pg.vertex_array[i].format & 0xFu) ==
                NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D)
                input_bgra_mask |= 1u << i;
        }
    }
    d3d8_vsh_set_input_layout(input_formats, input_offsets,
                              input_components, input_bgra_mask);
    return g_pg.host_vsh_handle;
}

static uint8_t clamp_yuv_channel(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void convert_packed_yuv_pixel(const uint8_t *line, uint32_t x,
                                     int uyvy, uint8_t *bgra)
{
    const uint32_t pair = (x & ~1u) * 2u;
    const int y = (int)line[x * 2u + (uyvy ? 1u : 0u)] - 16;
    const int u = (int)line[pair + (uyvy ? 0u : 1u)] - 128;
    const int v = (int)line[pair + (uyvy ? 2u : 3u)] - 128;

    /* Match NV2A/Xemu's BT.601 limited-range YUV conversion. The host
     * texture is B8G8R8A8, so store the converted channels in BGRA order. */
    bgra[2] = clamp_yuv_channel((298 * y + 409 * v + 128) >> 8);
    bgra[1] = clamp_yuv_channel((298 * y - 100 * u - 208 * v + 128) >> 8);
    bgra[0] = clamp_yuv_channel((298 * y + 516 * u + 128) >> 8);
    bgra[3] = 255;
}

static float pvideo_calculate_scale(uint32_t din_dout, uint32_t output_size)
{
    double calculated_in;
    if (!output_size) return 1.0f;
    calculated_in = (double)din_dout * (double)(output_size - 1u);
    calculated_in = floor(calculated_in / 1048576.0 + 0.5);
    return (float)((calculated_in + 1.0) / (double)output_size);
}

static BOOL composite_pvideo_overlay(GuestColorSurface *background)
{
    static uint8_t *converted;
    static size_t converted_capacity;
    static uint32_t log_count;
    NV2AState *d = nv2a_get_state();
    const uint32_t buffer = d ? d->pvideo.regs[NV_PVIDEO_BUFFER] : 0u;
    const uint32_t size_in = d ? d->pvideo.regs[NV_PVIDEO_SIZE_IN] : 0u;
    uint32_t in_width, in_height, in_pitch, in_color;
    uint32_t out_x, out_y, out_width, out_height;
    uint32_t point_in, in_s, in_t, ds_dx, dt_dy;
    uint32_t format, base, limit, offset, color_key;
    uint32_t logical_width, logical_height;
    uint64_t relative_end, absolute_start, absolute_end;
    float scale_x = 1.0f, scale_y = 1.0f;
    size_t required;
    uint32_t y;

    if (!d || !d->vram_ptr || !(buffer & NV_PVIDEO_BUFFER_0_USE) ||
        size_in == 0xFFFFFFFFu)
        return FALSE;
    in_width = GET_MASK(size_in, NV_PVIDEO_SIZE_IN_WIDTH);
    in_height = GET_MASK(size_in, NV_PVIDEO_SIZE_IN_HEIGHT);
    out_width = GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
                         NV_PVIDEO_SIZE_OUT_WIDTH);
    out_height = GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT],
                          NV_PVIDEO_SIZE_OUT_HEIGHT);
    out_x = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT],
                     NV_PVIDEO_POINT_OUT_X);
    out_y = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT],
                     NV_PVIDEO_POINT_OUT_Y);
    point_in = d->pvideo.regs[NV_PVIDEO_POINT_IN];
    in_s = GET_MASK(point_in, NV_PVIDEO_POINT_IN_S);
    in_t = GET_MASK(point_in, NV_PVIDEO_POINT_IN_T);
    format = d->pvideo.regs[NV_PVIDEO_FORMAT];
    in_pitch = GET_MASK(format, NV_PVIDEO_FORMAT_PITCH);
    in_color = GET_MASK(format, NV_PVIDEO_FORMAT_COLOR);
    ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    if (ds_dx != NV_PVIDEO_DIN_DOUT_UNITY)
        scale_x = pvideo_calculate_scale(ds_dx, out_width);
    if (dt_dy != NV_PVIDEO_DIN_DOUT_UNITY)
        scale_y = pvideo_calculate_scale(dt_dy, out_height);
    if (in_width > out_width)
        in_width = (uint32_t)floorf((float)out_width * scale_x + 0.5f);
    if (in_height > out_height)
        in_height = (uint32_t)floorf((float)out_height * scale_y + 0.5f);
    if (!in_width || !in_height || !out_width || !out_height ||
        (in_width & 1u) || in_pitch < in_width * 2u ||
        in_color != NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8)
        return FALSE;

    base = d->pvideo.regs[NV_PVIDEO_BASE];
    limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    offset = d->pvideo.regs[NV_PVIDEO_OFFSET];
    relative_end = (uint64_t)offset + (uint64_t)in_pitch * in_height;
    absolute_start = (uint64_t)base + offset;
    absolute_end = (uint64_t)base + relative_end;
    if (relative_end > limit || absolute_end > memory_region_size(d->vram) ||
        absolute_start >= absolute_end)
        return FALSE;

    required = (size_t)in_width * in_height * 4u;
    if (required > converted_capacity) {
        uint8_t *new_buffer = (uint8_t *)realloc(converted, required);
        if (!new_buffer) return FALSE;
        converted = new_buffer;
        converted_capacity = required;
    }
    for (y = 0u; y < in_height; ++y) {
        const uint8_t *source = d->vram_ptr + absolute_start +
                                (size_t)y * in_pitch;
        uint8_t *dest = converted + (size_t)y * in_width * 4u;
        uint32_t x;
        for (x = 0u; x < in_width; ++x)
            convert_packed_yuv_pixel(source, x, 0, dest + x * 4u);
    }

    logical_width = background && background->logical_width ?
        background->logical_width : 640u;
    logical_height = background && background->logical_height ?
        background->logical_height : 480u;
    color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0x00FFFFFFu;
    if (log_count < 12u) {
        fprintf(stderr,
                "[PVIDEO] base=%08X offset=%08X limit=%08X in=%ux%u "
                "pitch=%u point=%u/%u out=%u/%u/%ux%u scale=%.6f/%.6f "
                "key=%d/%06X\n",
                base, offset, limit, in_width, in_height, in_pitch,
                in_s, in_t, out_x, out_y, out_width, out_height,
                scale_x, scale_y,
                GET_MASK(format, NV_PVIDEO_FORMAT_DISPLAY) != 0u,
                color_key);
        ++log_count;
    }
    return d3d8_CompositeVideoOverlay(
        converted, in_width, in_height, in_width * 4u,
        (float)in_s / 16.0f, (float)in_t / 8.0f,
        scale_x, scale_y, out_x, out_y, out_width, out_height,
        logical_width, logical_height,
        background ? background->srv : NULL,
        background ? background->width : 0u,
        background ? background->height : 0u,
        GET_MASK(format, NV_PVIDEO_FORMAT_DISPLAY) != 0u, color_key);
}

static uint32_t hash_guest_rows(const uint8_t *base, uint32_t offset,
                                uint32_t pitch, uint32_t row_bytes,
                                uint32_t row_count)
{
    static uint32_t humvee_orientation[6][2];
    static int humvee_orientation_valid;
    static int humvee_repair_logged;
    uint32_t hash = 2166136261u;
    uint32_t y;
    for (y = 0; y < row_count; ++y) {
        const uint8_t *row = base + offset + (size_t)y * pitch;
        uint32_t x;
        for (x = 0; x < row_bytes; ++x) {
            hash ^= row[x];
            hash *= 16777619u;
        }
    }
    return hash;
}

static uint64_t hash_guest_rows64(const uint8_t *base, uint32_t offset,
                                  uint32_t pitch, uint32_t row_bytes,
                                  uint32_t row_count)
{
    /* This hash only detects whether an already-uploaded host texture changed;
     * it is not persisted or exposed to the title. Byte-at-a-time FNV-1a made
     * large mip chains monopolize the render thread for seconds. Mix complete
     * 64-bit words instead, retaining an explicit byte tail for arbitrary row
     * widths. memcpy keeps unaligned guest VRAM reads well-defined. */
    uint64_t hash = UINT64_C(0x9E3779B185EBCA87);
    uint32_t y;
    for (y = 0; y < row_count; ++y) {
        const uint8_t *row = base + offset + (size_t)y * pitch;
        uint32_t x = 0u;
        /* Independent accumulators break the dependency chain for mip chains.
         * Read every byte; short rows keep the existing scalar path. */
        if (row_bytes >= 32u) {
            uint64_t lanes[4] = { hash, hash ^ UINT64_C(0xC2B2AE3D27D4EB4F),
                hash ^ UINT64_C(0x165667B19E3779F9), hash ^ UINT64_C(0x85EBCA77C2B2AE63) };
            for (; x + 32u <= row_bytes; x += 32u) {
                for (unsigned lane = 0; lane < 4; ++lane) {
                    uint64_t word;
                    memcpy(&word, row + x + lane * 8u, sizeof(word));
                    lanes[lane] ^= word * UINT64_C(0x9E3779B185EBCA87);
                    lanes[lane] = (lanes[lane] << 27) | (lanes[lane] >> 37);
                    lanes[lane] *= UINT64_C(0xC2B2AE3D27D4EB4F);
                }
            }
            for (unsigned lane = 0; lane < 4; ++lane) {
                hash ^= lanes[lane];
                hash = (hash << 23) | (hash >> 41);
                hash = hash * UINT64_C(0x9E3779B185EBCA87) + UINT64_C(0x165667B19E3779F9);
            }
        }
        for (; x + sizeof(uint64_t) <= row_bytes; x += sizeof(uint64_t)) {
            uint64_t word;
            memcpy(&word, row + x, sizeof(word));
            word ^= word >> 33;
            word *= UINT64_C(0xFF51AFD7ED558CCD);
            word ^= word >> 33;
            hash ^= word;
            hash = (hash << 27) | (hash >> 37);
            hash = hash * UINT64_C(5) + UINT64_C(0x52DCE729);
        }
        for (; x < row_bytes; ++x) {
            hash ^= row[x];
            hash *= UINT64_C(1099511628211);
        }
        hash ^= (uint64_t)row_bytes + ((uint64_t)y << 32);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xC4CEB9FE1A85EC53);
    hash ^= hash >> 33;
    return hash;
}
static void dump_locked_bgra_texture(const char *prefix,
                                     const D3DLOCKED_RECT *locked,
                                     uint32_t width, uint32_t height,
                                     uint32_t frame_index)
{
    char path[1024];
    uint8_t header[18] = { 0 };
    FILE *fp;
    uint32_t row;

    if (!prefix || !*prefix || !locked || !locked->pBits)
        return;
    header[2] = 2u;
    header[12] = (uint8_t)(width & 0xFFu);
    header[13] = (uint8_t)(width >> 8);
    header[14] = (uint8_t)(height & 0xFFu);
    header[15] = (uint8_t)(height >> 8);
    header[16] = 32u;
    header[17] = 0x28u;
    snprintf(path, sizeof(path), "%s-%04u.tga", prefix, frame_index);
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fwrite(header, sizeof(header), 1, fp);
    for (row = 0; row < height; ++row) {
        const uint8_t *source = (const uint8_t *)locked->pBits +
                                (size_t)row * locked->Pitch;
        fwrite(source, 1, width * 4u, fp);
    }
    fclose(fp);
    fprintf(stderr, "[PGRAPH-YUV] dumped converted frame %s\n", path);
}

static uint32_t texture_color_key_mask(uint32_t color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return 0x00FFFFFFu;
    default:
        return 0xFFFFFFFFu;
    }
}

static int texture_color_format_is_linear(uint32_t color_format)
{
    return color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
           color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8;
}

static void get_guest_texture_dimensions(uint32_t format, uint32_t image_rect,
                                         uint32_t color_format,
                                         uint32_t *width, uint32_t *height)
{
    if (texture_color_format_is_linear(color_format)) {
        *width = image_rect >> 16;
        *height = image_rect & 0xFFFFu;
        return;
    }

    /* NV2A non-linear/swizzled and DXT textures use the logarithmic base
     * sizes in SET_TEXTURE_FORMAT. SET_TEXTURE_IMAGE_RECT is only the source
     * of dimensions for linear formats. This matches Xemu's
     * pgraph_get_texture_shape() behavior. */
    *width = 1u << ((format & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U) >> 20);
    *height = 1u << ((format & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V) >> 24);
}

static uint32_t get_guest_texture_levels(uint32_t format,
                                         uint32_t color_format,
                                         uint32_t control0)
{
    uint32_t levels;
    uint32_t max_levels;
    uint32_t max_lod;
    uint32_t log_width;
    uint32_t log_height;

    if (texture_color_format_is_linear(color_format))
        return 1u;
    levels = (format & NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS) >> 16;
    if (levels == 0u)
        levels = 1u;
    log_width = (format & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U) >> 20;
    log_height = (format & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V) >> 24;
    max_levels = (log_width > log_height ? log_width : log_height) + 1u;
    if (levels > max_levels)
        levels = max_levels;
    max_lod = (control0 & NV097_SET_TEXTURE_CONTROL0_MAX_LOD_CLAMP) >> 6;
    if (levels > max_lod + 1u)
        levels = max_lod + 1u;
    return levels;
}

static int texture_validation_perf_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled =
            pgraph_cached_getenv("MERCENARIES_TRACE_TEXTURE_PERF") != NULL;
    return enabled;
}

static void trace_texture_validation_perf(uint64_t hashed_bytes,
                                          LONGLONG hash_ticks,
                                          int fast_cache_hit)
{
    static uint64_t interval_hashes;
    static uint64_t interval_hash_bytes;
    static uint64_t interval_hash_ticks;
    static uint64_t interval_fast_hits;
    static ULONGLONG interval_start_ms;
    static LONGLONG qpc_frequency;
    LARGE_INTEGER frequency;
    const ULONGLONG now = GetTickCount64();

    if (interval_start_ms == 0u)
        interval_start_ms = now;
    if (fast_cache_hit) {
        ++interval_fast_hits;
    } else {
        ++interval_hashes;
        interval_hash_bytes += hashed_bytes;
        interval_hash_ticks += (uint64_t)hash_ticks;
    }
    if (now - interval_start_ms < 1000u)
        return;
    if (qpc_frequency == 0) {
        QueryPerformanceFrequency(&frequency);
        qpc_frequency = frequency.QuadPart;
    }
    fprintf(stderr,
            "[PGRAPH-TEXTURE-PERF] hashes=%llu bytes=%llu hash_ms=%.3f "
            "fast_hits=%llu interval_ms=%llu generation=%u\n",
            (unsigned long long)interval_hashes,
            (unsigned long long)interval_hash_bytes,
            qpc_frequency > 0 ?
                (double)interval_hash_ticks * 1000.0 / (double)qpc_frequency :
                0.0,
            (unsigned long long)interval_fast_hits,
            (unsigned long long)(now - interval_start_ms),
            g_pg.surface_generation);
    fflush(stderr);
    interval_hashes = 0u;
    interval_hash_bytes = 0u;
    interval_hash_ticks = 0u;
    interval_fast_hits = 0u;
    interval_start_ms = now;
}

static void dump_guest_compressed_texture(const uint8_t *base,
                                          uint32_t offset,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t pitch,
                                          uint32_t row_bytes,
                                          uint32_t row_count,
                                          uint32_t color_format)
{
    const char *prefix = pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_PREFIX");
    const char *offset_filter =
        pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_OFFSET");
    static uint32_t dumped_offsets[16];
    static uint32_t dumped_count;
    uint32_t header[31];
    uint32_t magic = 0x20534444u;
    uint32_t fourcc;
    uint32_t i, row;
    char path[MAX_PATH];
    FILE *fp;

    if (!prefix || !*prefix || dumped_count >= 16u ||
        (offset_filter && offset !=
            ((uint32_t)strtoul(offset_filter, NULL, 16) & 0x07FFFFFFu)))
        return;
    for (i = 0; i < dumped_count; ++i)
        if (dumped_offsets[i] == offset) return;
    fourcc = color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
        0x31545844u :
        color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ?
        0x33545844u : 0x35545844u;
    memset(header, 0, sizeof(header));
    header[0] = 124u;
    header[1] = 0x00081007u;
    header[2] = height;
    header[3] = width;
    header[4] = row_bytes * row_count;
    header[6] = 1u;
    header[18] = 32u;
    header[19] = 4u;
    header[20] = fourcc;
    header[26] = 0x1000u;
    snprintf(path, sizeof(path), "%s-%08X.dds", prefix, offset);
    fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(&magic, sizeof(magic), 1, fp);
    fwrite(header, sizeof(header), 1, fp);
    for (row = 0; row < row_count; ++row)
        fwrite(base + offset + (size_t)row * pitch, 1, row_bytes, fp);
    fclose(fp);
    dumped_offsets[dumped_count++] = offset;
    fprintf(stderr, "[PGRAPH-TEXTURE] dumped %s %ux%u\n", path, width, height);
}
static void dump_guest_linear_bgra_texture(const uint8_t *base,
                                           uint32_t offset,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t pitch)
{
    const char *prefix = pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_PREFIX");
    const char *offset_filter =
        pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_OFFSET");
    static uint32_t dumped_offsets[16];
    static uint32_t dumped_count;
    uint8_t header[18] = { 0 };
    uint32_t i, row;
    char path[MAX_PATH];
    FILE *fp;

    if (!prefix || !*prefix || dumped_count >= 16u ||
        (offset_filter && offset !=
            ((uint32_t)strtoul(offset_filter, NULL, 16) & 0x07FFFFFFu)))
        return;
    for (i = 0; i < dumped_count; ++i)
        if (dumped_offsets[i] == offset) return;
    header[2] = 2u;
    header[12] = (uint8_t)width;
    header[13] = (uint8_t)(width >> 8);
    header[14] = (uint8_t)height;
    header[15] = (uint8_t)(height >> 8);
    header[16] = 32u;
    header[17] = 0x28u;
    snprintf(path, sizeof(path), "%s-%08X.tga", prefix, offset);
    fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(header, sizeof(header), 1, fp);
    for (row = 0; row < height; ++row)
        fwrite(base + offset + (size_t)row * pitch, 1, width * 4u, fp);
    fclose(fp);
    dumped_offsets[dumped_count++] = offset;
    fprintf(stderr, "[PGRAPH-TEXTURE] dumped %s %ux%u\n", path, width, height);
}

static IDirect3DTexture8 *prepare_guest_texture(IDirect3DDevice8 *dev,
                                                uint32_t stage)
{
    NV2AState *d = nv2a_get_state();
    const uint32_t offset = resolved_texture_offset(stage);
    const uint32_t format = g_pg.tex[stage].format;
    const uint32_t color_format = (format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    const uint32_t rect = g_pg.tex[stage].image_rect;
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    D3DFORMAT host_format;
    int compressed = 0;
    int packed_yuv = 0;
    int swizzled = 0;
    int g8b8 = 0;
    int a8b8g8r8 = 0;
    const int cubemap =
        (format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) != 0u;
    uint8_t *unswizzled = NULL;
    uint32_t pitch;
    uint32_t row_bytes;
    uint32_t row_count;
    uint64_t end;
    uint64_t source_length;
    uint64_t cube_face_length = 0u;
    uint64_t cube_face_stride = 0u;
    uint64_t source_hash;
    GuestTextureCacheEntry *cache_entry;
    int same_stage_binding;
    D3DLOCKED_RECT locked;
    HRESULT hr;
    uint32_t y;

    if (stage >= 4u || !g_pg.tex[stage].enabled)
        return NULL;

    {
        int external=0;
        IDirect3DTexture8 *texture=external_ui_texture(dev,offset,&external);
        if(external)return texture;
    }
    get_guest_texture_dimensions(format, rect, color_format, &width, &height);
    levels = get_guest_texture_levels(format, color_format,
                                      g_pg.tex[stage].control0);

    if (!dev || !d || !d->vram_ptr || width == 0 || height == 0 ||
        width > 4096u || height > 4096u)
        return NULL;

    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
        host_format = D3DFMT_LIN_A8R8G8B8;
        swizzled = 1;
        row_bytes = width * 4u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
        host_format = D3DFMT_LIN_X8R8G8B8;
        swizzled = 1;
        row_bytes = width * 4u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
        host_format = D3DFMT_LIN_A8R8G8B8;
        row_bytes = width * 4u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        host_format = D3DFMT_LIN_X8R8G8B8;
        row_bytes = width * 4u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
        /* Xemu maps this linear format as native RGBA bytes. The D3D8
         * host texture is BGRA in memory, so swap R/B while uploading. */
        host_format = D3DFMT_LIN_A8R8G8B8;
        a8b8g8r8 = 1;
        row_bytes = width * 4u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
        host_format = D3DFMT_LIN_A8R8G8B8;
        swizzled = 1;
        g8b8 = 1;
        row_bytes = width * 2u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        host_format = D3DFMT_LIN_A8R8G8B8;
        g8b8 = 1;
        row_bytes = width * 2u;
        row_count = height;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        host_format = D3DFMT_DXT1;
        compressed = 1;
        row_bytes = ((width + 3u) / 4u) * 8u;
        row_count = (height + 3u) / 4u;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        host_format = D3DFMT_DXT3;
        compressed = 1;
        row_bytes = ((width + 3u) / 4u) * 16u;
        row_count = (height + 3u) / 4u;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        host_format = D3DFMT_DXT5;
        compressed = 1;
        row_bytes = ((width + 3u) / 4u) * 16u;
        row_count = (height + 3u) / 4u;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
        if (width & 1u)
            return NULL;
        host_format = D3DFMT_LIN_A8R8G8B8;
        packed_yuv = color_format;
        row_bytes = width * 2u;
        row_count = height;
        break;
    default:
        {
            static uint64_t unsupported_formats;
            const uint64_t bit = color_format < 64u ? 1ull << color_format : 0;
            if (!bit || !(unsupported_formats & bit)) {
                fprintf(stderr,
                        "[PGRAPH-D3D11] Unsupported stage%u texture format %02X "
                        "off=%08X raw=%08X ctl0=%08X ctl1=%08X "
                        "rect=%08X dims=%ux%u first=%08X/%08X\n",
                        stage, color_format, offset, format, g_pg.tex[stage].control0,
                        g_pg.tex[stage].control1, rect, width, height,
                        read_le32(d->vram_ptr + offset),
                        read_le32(d->vram_ptr + offset + 4u));
                unsupported_formats |= bit;
            }
        }
        return NULL;
    }

    pitch = (compressed || swizzled) ? row_bytes :
                                          (g_pg.tex[stage].control1 >> 16);
    if (pitch < row_bytes)
        pitch = row_bytes;
    source_length = (uint64_t)pitch * row_count;
    if (levels > 1u) {
        uint32_t level;
        uint32_t level_width = width;
        uint32_t level_height = height;
        const uint32_t bytes_per_pixel = g8b8 ? 2u : 4u;
        const uint32_t block_size = color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8u : 16u;
        source_length = 0u;
        for (level = 0u; level < levels; ++level) {
            if (compressed) {
                const uint32_t blocks_w = (level_width + 3u) / 4u;
                const uint32_t blocks_h = (level_height + 3u) / 4u;
                source_length += (uint64_t)blocks_w * blocks_h * block_size;
            } else {
                source_length += (uint64_t)level_width * level_height *
                                 bytes_per_pixel;
            }
            if (level_width > 1u) level_width >>= 1;
            if (level_height > 1u) level_height >>= 1;
        }
    }
    if (cubemap) {
        if (width != height)
            return NULL;
        cube_face_length = source_length;
        cube_face_stride = (cube_face_length + 127u) & ~127ull;
        source_length = cube_face_stride * 6u;
    }
    end = (uint64_t)offset + source_length;
    if (end > (uint64_t)memory_region_size(d->vram))
        return NULL;

    /* CPU depth readback is a diagnostic fallback only. A gameplay frame can
     * reuse the depth allocation thousands of times, so synchronously mapping
     * it here destroys renderer throughput. Keep the evidence path available
     * without imposing it on normal rendering. */
    if (pgraph_cached_getenv(
            "MERCENARIES_ENABLE_SURFACE_DEPTH_READBACK") != NULL)
        download_guest_surfaces_in_range_if_dirty(offset, source_length);

    cache_entry = find_guest_texture_cache_entry(offset, format, rect, pitch);
    same_stage_binding = cache_entry && cache_entry->texture &&
        g_pg.guest_texture[stage] == cache_entry->texture &&
        g_pg.guest_texture_offset[stage] == offset &&
        g_pg.guest_texture_format[stage] == format &&
        g_pg.guest_texture_rect[stage] == rect &&
        g_pg.guest_texture_pitch[stage] == pitch;
    /* Xbox swizzled and block-compressed resources are title-managed GPU
     * assets, but their VRAM allocations can be recycled between loading
     * screens and gameplay. Without QEMU's page-dirty tracking, validate a
     * cached allocation when it is first used in a rendered generation.
     * Re-hashing the same authored texture every time stages alternate made
     * texture-heavy gameplay spend seconds walking unchanged VRAM. A texture
     * already checked in this generation is as safe as a consecutive binding;
     * the next FLIP_STALL generation checks it again, so recycled allocations
     * are still detected. Linear textures (including XMV's changing YUV
     * buffers) remain content-checked every draw.
     * MERCENARIES_VALIDATE_STATIC_TEXTURES keeps the exhaustive diagnostic
     * path available. */
    if (cache_entry && cache_entry->texture &&
        cache_entry->source_hash_valid && (compressed || swizzled) &&
        (same_stage_binding ||
         cache_entry->last_validation_generation == g_pg.surface_generation) &&
        pgraph_cached_getenv("MERCENARIES_VALIDATE_STATIC_TEXTURES") == NULL) {
        cache_entry->last_use = ++g_pg.texture_cache_use_serial;
        g_pg.guest_texture[stage] = cache_entry->texture;
        g_pg.guest_texture_offset[stage] = offset;
        g_pg.guest_texture_format[stage] = format;
        g_pg.guest_texture_rect[stage] = rect;
        g_pg.guest_texture_pitch[stage] = pitch;
        if (texture_validation_perf_enabled())
            trace_texture_validation_perf(0u, 0, 1);
        return cache_entry->texture;
    }
    {
        LARGE_INTEGER hash_begin, hash_end;
        const int trace_perf = texture_validation_perf_enabled();
        if (trace_perf)
            QueryPerformanceCounter(&hash_begin);
        source_hash = levels > 1u ?
            hash_guest_rows64(d->vram_ptr, offset, (uint32_t)source_length,
                              (uint32_t)source_length, 1u) :
            hash_guest_rows64(d->vram_ptr, offset, pitch, row_bytes, row_count);
        if (trace_perf) {
            QueryPerformanceCounter(&hash_end);
            trace_texture_validation_perf(
                source_length, hash_end.QuadPart - hash_begin.QuadPart, 0);
        }
    }
    if (cache_entry)
        cache_entry->last_validation_generation = g_pg.surface_generation;
    if (packed_yuv && pgraph_cached_getenv("MERCENARIES_TRACE_PACKED_YUV") != NULL) {
        static uint32_t previous_hash[4];
        static uint32_t change_count[4];
        const uint32_t source_hash = hash_guest_rows(
            d->vram_ptr, offset, pitch, row_bytes, row_count);
        if (source_hash != previous_hash[stage] && change_count[stage] < 128u) {
            uint32_t non_neutral = 0u;
            uint32_t sample;
            for (sample = 0; sample < width * height; sample += 257u) {
                const uint32_t sx = sample % width;
                const uint32_t sy = sample / width;
                const uint8_t *px = d->vram_ptr + offset +
                                    (size_t)sy * pitch + (size_t)sx * 2u;
                if (px[0] != 0u || px[1] != 0x80u)
                    ++non_neutral;
            }
            ++change_count[stage];
            fprintf(stderr,
                    "[PGRAPH-YUV] draw=%llu stage=%u change=%u off=%08X "
                    "fmt=%02X dims=%ux%u pitch=%u hash=%08X first=%08X/%08X "
                    "sampled_non_neutral=%u\n",
                    (unsigned long long)g_pg.stats.draw_calls, stage,
                    change_count[stage], offset, color_format, width, height,
                    pitch, source_hash, read_le32(d->vram_ptr + offset),
                    read_le32(d->vram_ptr + offset + 4u), non_neutral);
            previous_hash[stage] = source_hash;
        }
    }
    if (stage == 2u && g8b8 &&
        pgraph_cached_getenv("MERCENARIES_TRACE_SKY_TEXTURES") != NULL) {
        uint8_t max_lo = 0u, max_hi = 0u;
        uint32_t sample;
        for (sample = 0; sample < width * height; ++sample) {
            const uint8_t lo = d->vram_ptr[offset + sample * 2u + 0u];
            const uint8_t hi = d->vram_ptr[offset + sample * 2u + 1u];
            if (lo > max_lo) max_lo = lo;
            if (hi > max_hi) max_hi = hi;
        }
        fprintf(stderr,
                "[PGRAPH-SKY-TEX] stage2 off=%08X dims=%ux%u first=%08X/%08X max=%u/%u\n",
                offset, width, height, read_le32(d->vram_ptr + offset),
                read_le32(d->vram_ptr + offset + 4u), max_lo, max_hi);
    }
    if (compressed)
        dump_guest_compressed_texture(d->vram_ptr, offset, width, height,
                                      pitch, row_bytes, row_count,
                                      color_format);
    else if (color_format ==
             NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8)
        dump_guest_linear_bgra_texture(d->vram_ptr, offset, width, height,
                                       pitch);

    if (pgraph_trace_textures_enabled() && g_pg.stats.draw_calls < 8u) {
        uint32_t source_hash = 2166136261u;
        uint32_t source_y;
        for (source_y = 0; source_y < row_count; ++source_y) {
            const uint8_t *source_row = d->vram_ptr + offset +
                                        (size_t)source_y * pitch;
            uint32_t source_x;
            for (source_x = 0; source_x < row_bytes; ++source_x) {
                source_hash ^= source_row[source_x];
                source_hash *= 16777619u;
            }
        }
        fprintf(stderr,
                "[PGRAPH-D3D11] Texture stage%u bytes: off=%08X pitch=%u rows=%u "
                "row_bytes=%u hash=%08X first=%08X/%08X\n",
                stage, offset, pitch, row_count, row_bytes, source_hash,
                read_le32(d->vram_ptr + offset),
                read_le32(d->vram_ptr + offset + 4u));
    }

    {
        static int logged_texture_source;
        if (pgraph_trace_textures_enabled() && !logged_texture_source &&
            !compressed) {
            uint32_t sampled_nonblack = 0, sampled_nonwhite = 0;
            const uint32_t source_bpp = (g8b8 || packed_yuv) ? 2u : 4u;
            uint32_t sample;
            for (sample = 0; sample < width * height; sample += 257u) {
                const uint32_t sx = sample % width;
                const uint32_t sy = sample / width;
                const uint8_t *px = d->vram_ptr + offset +
                                    (size_t)sy * pitch + (size_t)sx * source_bpp;
                if ((px[0] | px[1] | (source_bpp > 2u ? px[2] : 0u)) != 0u) ++sampled_nonblack;
                if (px[0] != 0xFFu || px[1] != 0xFFu ||
                    (source_bpp > 2u && px[2] != 0xFFu))
                    ++sampled_nonwhite;
            }
            fprintf(stderr,
                    "[PGRAPH-D3D11] Guest texture stage%u source: first=%08X %08X "
                    "%08X %08X sampled_nonblack=%u sampled_nonwhite=%u\n",
                    stage, read_le32(d->vram_ptr + offset + 0u),
                    read_le32(d->vram_ptr + offset + 4u),
                    read_le32(d->vram_ptr + offset + 8u),
                    read_le32(d->vram_ptr + offset + 12u),
                    sampled_nonblack, sampled_nonwhite);
            logged_texture_source = 1;
        }
    }

    if (!cache_entry) {
        cache_entry = allocate_guest_texture_cache_entry(offset, format,
                                                         rect, pitch);
        if (!cache_entry)
            return NULL;
        hr = cubemap ?
            d3d8_CreateCubeTextureImpl(width, levels, 0, host_format,
                                       &cache_entry->texture) :
            dev->lpVtbl->CreateTexture(dev, width, height, levels, 0,
                                       host_format, D3DPOOL_DEFAULT,
                                       &cache_entry->texture);
        if (hr != S_OK || !cache_entry->texture) {
            fprintf(stderr,
                    "[PGRAPH-D3D11] Guest texture stage%u create failed: hr=0x%08X "
                    "off=%08X fmt=%02X %ux%u levels=%u cube=%u\n",
                    stage, (unsigned)hr, offset, color_format, width, height,
                    levels, cubemap);
            memset(cache_entry, 0, sizeof(*cache_entry));
            return NULL;
        }
        if (pgraph_trace_textures_enabled()) {
            fprintf(stderr,
                    "[PGRAPH-D3D11] Guest texture stage%u: off=%08X fmt=%02X "
                    "%ux%u levels=%u cube=%u\n",
                    stage, offset, color_format, width, height, levels,
                    cubemap);
        }
    }
    g_pg.guest_texture[stage] = cache_entry->texture;
    g_pg.guest_texture_offset[stage] = offset;
    g_pg.guest_texture_format[stage] = format;
    g_pg.guest_texture_rect[stage] = rect;
    g_pg.guest_texture_pitch[stage] = pitch;

    if (cache_entry->source_hash_valid &&
        cache_entry->source_hash == source_hash)
        return cache_entry->texture;
    if (cubemap) {
        uint32_t face;

        for (face = 0u; face < 6u; ++face) {
            uint32_t level;
            uint32_t level_width = width;
            uint32_t level_height = height;
            uint64_t guest_level_offset =
                offset + (uint64_t)face * cube_face_stride;

            for (level = 0u; level < levels; ++level) {
                uint32_t level_row_bytes;
                uint32_t level_row_count;
                const uint8_t *upload_data;
                uint8_t *cube_upload = NULL;

                if (compressed) {
                    level_row_bytes = ((level_width + 3u) / 4u) *
                        (color_format ==
                         NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                         8u : 16u);
                    level_row_count = (level_height + 3u) / 4u;
                    upload_data = d->vram_ptr +
                        (size_t)guest_level_offset;
                } else if (swizzled && !g8b8) {
                    level_row_bytes = level_width * 4u;
                    level_row_count = level_height;
                    cube_upload = (uint8_t *)malloc(
                        (size_t)level_row_bytes * level_row_count);
                    if (!cube_upload)
                        return NULL;
                    xbox_unswizzle_rect(cube_upload,
                        d->vram_ptr + (size_t)guest_level_offset,
                        level_width, level_height, 4u);
                    upload_data = cube_upload;
                } else {
                    /* No retail title cubemap currently uses a converted
                     * linear or G8B8 format. Keep unsupported combinations
                     * explicit rather than binding a resource with corrupt
                     * face data. */
                    return NULL;
                }

                hr = d3d8_UpdateCubeTextureFace(cache_entry->texture, face,
                    level, upload_data, level_row_bytes,
                    level_row_bytes * level_row_count);
                free(cube_upload);
                if (hr != S_OK)
                    return NULL;
                guest_level_offset +=
                    (uint64_t)level_row_bytes * level_row_count;
                if (level_width > 1u) level_width >>= 1;
                if (level_height > 1u) level_height >>= 1;
            }
        }
        if (pgraph_trace_textures_enabled())
            fprintf(stderr,
                    "[PGRAPH-D3D11] uploaded cubemap stage%u off=%08X "
                    "%ux%u levels=%u face-stride=%llu\n",
                    stage, offset, width, height, levels,
                    (unsigned long long)cube_face_stride);
    } else
    {
        uint32_t level;
        uint32_t level_width = width;
        uint32_t level_height = height;
        uint64_t guest_level_offset = offset;
        const uint32_t source_bpp = g8b8 ? 2u : 4u;
        const uint32_t block_size = color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8u : 16u;

        for (level = 0u; level < levels; ++level) {
            uint32_t level_row_bytes;
            uint32_t level_row_count;
            uint32_t level_pitch;

            if (compressed) {
                level_row_bytes = ((level_width + 3u) / 4u) * block_size;
                level_row_count = (level_height + 3u) / 4u;
            } else {
                level_row_bytes = level_width * source_bpp;
                level_row_count = level_height;
            }
            level_pitch = levels > 1u ? level_row_bytes : pitch;
            unswizzled = NULL;
            memset(&locked, 0, sizeof(locked));
            hr = g_pg.guest_texture[stage]->lpVtbl->LockRect(
                g_pg.guest_texture[stage], level, &locked, NULL, 0);
            if (hr != S_OK || !locked.pBits)
                return NULL;

            if (swizzled && !g8b8) {
                xbox_unswizzle_rect(locked.pBits,
                    d->vram_ptr + (size_t)guest_level_offset,
                    level_width, level_height, 4u);
            } else {
                if (swizzled) {
                    unswizzled = (uint8_t *)malloc(
                        (size_t)level_row_bytes * level_height);
                    if (!unswizzled) {
                        g_pg.guest_texture[stage]->lpVtbl->UnlockRect(
                            g_pg.guest_texture[stage], level);
                        return NULL;
                    }
                    xbox_unswizzle_rect(unswizzled,
                        d->vram_ptr + (size_t)guest_level_offset,
                        level_width, level_height, 2u);
                }
                for (y = 0; y < level_row_count; ++y) {
                    uint8_t *destination = (uint8_t *)locked.pBits +
                                           (size_t)y * locked.Pitch;
                    const uint8_t *source = unswizzled ?
                        unswizzled + (size_t)y * level_row_bytes :
                        d->vram_ptr + (size_t)guest_level_offset +
                            (size_t)y * level_pitch;
                    if (packed_yuv) {
                        uint32_t x;
                        const int uyvy = packed_yuv ==
                            NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8;
                        for (x = 0; x < level_width; ++x)
                            convert_packed_yuv_pixel(source, x, uyvy,
                                destination + (size_t)x * 4u);
                    } else if (g8b8) {
                        uint32_t x;
                        for (x = 0; x < level_width; ++x) {
                            const uint8_t lo = source[x * 2u + 0u];
                            const uint8_t hi = source[x * 2u + 1u];
                            destination[x * 4u + 0u] = lo;
                            destination[x * 4u + 1u] = hi;
                            destination[x * 4u + 2u] = lo;
                            destination[x * 4u + 3u] = hi;
                        }
                    } else if (a8b8g8r8) {
                        uint32_t x;
                        for (x = 0; x < level_width; ++x) {
                            destination[x * 4u + 0u] = source[x * 4u + 2u];
                            destination[x * 4u + 1u] = source[x * 4u + 1u];
                            destination[x * 4u + 2u] = source[x * 4u + 0u];
                            destination[x * 4u + 3u] = source[x * 4u + 3u];
                        }
                    } else {
                        memcpy(destination, source, level_row_bytes);
                    }
                }
                free(unswizzled);
                unswizzled = NULL;
            }

            if (level == 0u && packed_yuv) {
                const char *dump_prefix =
                    pgraph_cached_getenv("MERCENARIES_DUMP_PACKED_YUV_PREFIX");
                static uint32_t dumped_hash[4];
                static uint32_t dump_count[4];
                if (dump_prefix && *dump_prefix && dump_count[stage] < 8u) {
                    const uint32_t dump_hash = hash_guest_rows(
                        d->vram_ptr, offset, pitch, row_bytes, row_count);
                    if (dump_hash != dumped_hash[stage]) {
                        ++dump_count[stage];
                        dump_locked_bgra_texture(dump_prefix, &locked,
                            width, height, dump_count[stage]);
                        dumped_hash[stage] = dump_hash;
                    }
                }
            }
            if (level == 0u && swizzled && !g8b8) {
                const char *dump_prefix =
                    pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_PREFIX");
                const char *dump_offset =
                    pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_OFFSET");
                static uint32_t dumped_swizzled_offsets[16];
                static uint32_t dumped_swizzled_count;
                uint32_t dump_index;
                int already_dumped = 0;

                for (dump_index = 0u;
                     dump_index < dumped_swizzled_count; ++dump_index) {
                    if (dumped_swizzled_offsets[dump_index] == offset) {
                        already_dumped = 1;
                        break;
                    }
                }
                if (!already_dumped && dump_prefix && *dump_prefix &&
                    dumped_swizzled_count < 16u &&
                    (!dump_offset || offset ==
                        ((uint32_t)strtoul(dump_offset, NULL, 16) &
                         0x07FFFFFFu))) {
                    dump_locked_bgra_texture(dump_prefix, &locked,
                        width, height, dumped_swizzled_count + 1u);
                    dumped_swizzled_offsets[dumped_swizzled_count++] = offset;
                }
            }

            g_pg.guest_texture[stage]->lpVtbl->UnlockRect(
                g_pg.guest_texture[stage], level);
            guest_level_offset += (uint64_t)level_pitch * level_row_count;
            if (level_width > 1u) level_width >>= 1;
            if (level_height > 1u) level_height >>= 1;
        }
    }
    cache_entry->source_hash = source_hash;
    cache_entry->source_hash_valid = 1;
    g_pg.guest_texture_source_hash[stage] = source_hash;
    g_pg.guest_texture_source_hash_valid[stage] = 1;
    return cache_entry->texture;
}

static void mirror_guest_zeta_clear(uint32_t clear_parameter)
{
    NV2AState *d = nv2a_get_state();
    const uint32_t zeta_format =
        (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ZETA) >> 4;
    const uint32_t bytes_per_pixel =
        zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ? 4u : 2u;
    const uint32_t pitch = g_pg.surface_pitch >> 16;
    uint32_t xmin = g_pg.clear_rect_h & 0x0FFFu;
    uint32_t xmax = (g_pg.clear_rect_h >> 16) & 0x0FFFu;
    uint32_t ymin = g_pg.clear_rect_v & 0x0FFFu;
    uint32_t ymax = (g_pg.clear_rect_v >> 16) & 0x0FFFu;
    const uint32_t anti_aliasing =
        (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ANTI_ALIASING) >> 12;
    const size_t vram_size = d ? memory_region_size(d->vram) : 0u;
    uint32_t y;

    if (!(clear_parameter & NV097_CLEAR_SURFACE_Z) || !d || !d->vram_ptr ||
        pitch < bytes_per_pixel || xmin > xmax || ymin > ymax ||
        g_pg.surface_zeta_offset >= vram_size)
        return;
    /* Surface clip origin plus extent defines the backing surface size.
     * A clear rectangle may exceed it, but the GPU clips to the allocation;
     * the CPU mirror must not clear adjacent guest objects or row padding.
     * Clamp in logical coordinates before expanding antialiasing samples. */
    const uint32_t clip_width = g_pg.surface_clip_h >> 16;
    const uint32_t clip_height = g_pg.surface_clip_v >> 16;
    const uint32_t surface_width = (g_pg.surface_clip_h & 0xFFFFu) + clip_width;
    const uint32_t surface_height = (g_pg.surface_clip_v & 0xFFFFu) + clip_height;
    if (!clip_width || !clip_height ||
        xmin >= surface_width || ymin >= surface_height)
        return;
    if (xmax >= surface_width) xmax = surface_width - 1u;
    if (ymax >= surface_height) ymax = surface_height - 1u;
    /* NV2A clear rectangles are logical surface coordinates. Match Xemu's
     * physical sample layout before touching guest VRAM. */
    if (anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2 ||
        anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4) {
        xmin *= 2u;
        xmax = xmax * 2u + 1u;
    }
    if (anti_aliasing ==
            NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_SQUARE_OFFSET_4) {
        ymin *= 2u;
        ymax = ymax * 2u + 1u;
    }
    if (xmax >= pitch / bytes_per_pixel)
        xmax = pitch / bytes_per_pixel - 1u;
    if ((uint64_t)g_pg.surface_zeta_offset +
            (uint64_t)ymax * pitch +
            (uint64_t)(xmax + 1u) * bytes_per_pixel > vram_size)
        return;

    for (y = ymin; y <= ymax; ++y) {
        uint8_t *row = d->vram_ptr + g_pg.surface_zeta_offset +
                       (size_t)y * pitch;
        uint32_t x;
        /* Fixed-size copies permit vector stores without assuming guest
         * alignment or writing past the clipped row into padding. */
        if (bytes_per_pixel == 4u) {
            const uint32_t clear = g_pg.zstencil_clear;
            const uint32_t block[4] = { clear, clear, clear, clear };
            for (x = xmin; x + 3u <= xmax; x += 4u)
                memcpy(row + (size_t)x * 4u, block, sizeof(block));
            for (; x <= xmax; ++x)
                memcpy(row + (size_t)x * 4u, &clear, 4u);
        } else {
            const uint16_t clear16 = (uint16_t)g_pg.zstencil_clear;
            const uint16_t block[8] = { clear16, clear16, clear16, clear16,
                                       clear16, clear16, clear16, clear16 };
            for (x = xmin; x + 7u <= xmax; x += 8u)
                memcpy(row + (size_t)x * 2u, block, sizeof(block));
            for (; x <= xmax; ++x)
                memcpy(row + (size_t)x * 2u, &clear16, 2u);
        }
    }
    if (pgraph_cached_getenv("MERCENARIES_TRACE_COMBINER") != NULL) {
        fprintf(stderr,
                "[PGRAPH-ZETA] clear off=%08X pitch=%u rect=%u,%u-%u,%u "
                "fmt=%u value=%08X\n",
                g_pg.surface_zeta_offset, pitch, xmin, ymin, xmax, ymax,
                zeta_format, g_pg.zstencil_clear);
    }
}
/* ======================================================================
 * Initialization
 * ====================================================================== */

void pgraph_d3d11_init(void)
{
    memset(&g_pg, 0, sizeof(g_pg));
    g_pg.internal_width = 640u;
    g_pg.internal_height = 480u;
    g_pg.vert_stride = INLINE_VERT_DWORDS;
    g_pg.immediate_texcoord_base = NV2A_VERTEX_ATTR_TEXTURE0;
    g_pg.clear_color = 0xFF000000;
    g_pg.color_mask = 0x01010101;
    g_pg.depth_func = D3DCMP_LESSEQUAL;
    g_pg.depth_write = 1;
    g_pg.blend_equation = NV097_SET_BLEND_EQUATION_V_FUNC_ADD;
    g_pg.cull_face = 0x0405u;
    g_pg.front_face = 0x0900u;
    g_pg.alpha_func = D3DCMP_ALWAYS;
    g_pg.stencil_func = D3DCMP_ALWAYS;
    g_pg.stencil_mask = 0xFFu;
    g_pg.stencil_write_mask = 0xFFu;
    g_pg.stencil_fail = 1u;
    g_pg.stencil_zfail = 1u;
    g_pg.stencil_zpass = 1u;
    for (uint32_t attr = 0; attr < 16u; ++attr)
        g_pg.immediate_vertex_attr[attr][3] = 1.0f;
    g_pg.immediate_vertex_4ub[NV2A_VERTEX_ATTR_DIFFUSE] = 0xFFFFFFFFu;
    for (uint32_t component = 0; component < 4u; ++component)
        g_pg.immediate_vertex_attr[NV2A_VERTEX_ATTR_DIFFUSE][component] =
            1.0f;
    g_pg.initialized = 1;

    fprintf(stderr, "[PGRAPH-D3D11] Translator initialized\n");
}


/* The Xbox glow filter uses three bilinear samples per pass at 320x240.
 * Scaling its render targets makes each bilinear footprint too narrow and
 * exposes separate ghost images. Authentic mode keeps just this filter at
 * native resolution; scene geometry, HUD and other surface effects stay HD. */
typedef struct NativeHazeSurface {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
} NativeHazeSurface;
static NativeHazeSurface g_native_haze[3]; /* full input, two half ping-pong */
static int g_haze_mode = 1;
static int g_haze_last_slot;
static uint32_t g_haze_last_offset;
static uint64_t g_haze_last_serial;
void pgraph_d3d11_set_haze_mode(int mode)
{
    g_haze_mode = mode;
    g_haze_last_slot = 0;
}
static void release_native_haze(void)
{
    unsigned i;
    for (i = 0; i < 3; ++i) {
        if (g_native_haze[i].srv) ID3D11ShaderResourceView_Release(g_native_haze[i].srv);
        if (g_native_haze[i].rtv) ID3D11RenderTargetView_Release(g_native_haze[i].rtv);
        if (g_native_haze[i].texture) ID3D11Texture2D_Release(g_native_haze[i].texture);
    }
    memset(g_native_haze, 0, sizeof(g_native_haze));
    g_haze_last_slot = 0;
}
static int ensure_native_haze(void)
{
    unsigned i;
    ID3D11Device *device = d3d8_GetD3D11Device();
    if (g_native_haze[2].srv) return 1;
    if (!device) return 0;
    for (i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.Width = i ? 320u : 640u;
        desc.Height = i ? 240u : 480u;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &g_native_haze[i].texture)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)g_native_haze[i].texture, NULL, &g_native_haze[i].rtv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)g_native_haze[i].texture, NULL, &g_native_haze[i].srv))) {
            release_native_haze();
            return 0;
        }
    }
    return 1;
}
/* Returns the filter destination slot, or -1 for its additive composite.
 * Zero means an unrelated draw, native rendering, or allocation failure. */
static int begin_native_haze(uint32_t vertices, const GuestColorSurface *source,
                             ID3D11ShaderResourceView **sample)
{
    GuestColorSurface *target = g_pg.bound_color;
    int input, output, blur, composite;
    float dx, dy;
    if (g_haze_mode != 1 || vertices != 4u ||
        g_pg.immediate_vertex_count != 4u || !source || !target ||
        g_pg.depth_test || !g_pg.tex[0].enabled || g_pg.tex[3].enabled ||
        (target->width == target->logical_width && target->height == target->logical_height)) return 0;
    dx = fabsf(g_pg.immediate_vertices[2][0][0] - g_pg.immediate_vertices[0][0][0]);
    dy = fabsf(g_pg.immediate_vertices[2][0][1] - g_pg.immediate_vertices[0][0][1]);
    if (fabsf(dx - (float)target->logical_width) > 1.0f ||
        fabsf(dy - (float)target->logical_height) > 1.0f) return 0;
    blur = target->logical_width == 320u && target->logical_height == 240u &&
        !g_pg.blend_enable && g_pg.tex[1].enabled && g_pg.tex[2].enabled &&
        resolved_texture_offset(1) == resolved_texture_offset(0) &&
        resolved_texture_offset(2) == resolved_texture_offset(0);
    composite = target->logical_width == 640u && target->logical_height == 480u &&
        !g_pg.tex[1].enabled && !g_pg.tex[2].enabled &&
        g_pg.blend_enable && g_pg.blend_sfactor == 1u && g_pg.blend_dfactor == 1u;
    if (!blur && !composite) return 0;
    input = g_haze_last_slot && source->offset == g_haze_last_offset &&
        source->write_serial == g_haze_last_serial ? g_haze_last_slot : 0;
    /* A complete chain must start with the scene-sized first filter. This
     * excludes flare probes and unrelated half-surface composites. */
    if (!input && (!blur || source->logical_width != 640u || source->logical_height != 480u)) return 0;
    if (!ensure_native_haze()) return 0;
    if (!input && !d3d8_CopyTextureToRenderTarget(source->srv, g_native_haze[0].rtv, 640u, 480u)) return 0;
    *sample = g_native_haze[input].srv;
    if (composite) return -1;
    output = input == 1 ? 2 : 1;
    d3d8_BindRenderTargets(g_native_haze[output].rtv, NULL, 320u, 240u);
    d3d8_SetLogicalRenderTargetSize(320u, 240u);
    return output;
}
static void finish_native_haze(int slot)
{
    GuestColorSurface *target = g_pg.bound_color;
    if (slot <= 0) return;
    d3d8_CopyTextureToRenderTarget(g_native_haze[slot].srv, target->rtv, target->width, target->height);
    d3d8_BindRenderTargets(target->rtv, g_pg.bound_depth ? g_pg.bound_depth->dsv : NULL,
        target->width, target->height);
    d3d8_SetLogicalRenderTargetSize(target->logical_width, target->logical_height);
    g_haze_last_slot = slot;
    g_haze_last_offset = target->offset;
    g_haze_last_serial = g_pg.surface_write_serial + 1u;
}

void pgraph_d3d11_shutdown(void)
{
    release_native_haze();
    uint32_t stage;
    for(stage=0;stage<g_external_ui_count;++stage){
        if(g_external_ui[stage].texture){
            g_external_ui[stage].texture->lpVtbl->Release(g_external_ui[stage].texture);
            g_external_ui[stage].texture=NULL;
        }
    }


    if (g_pg.host_vsh_handle) {
        d3d8_vsh_delete_shader(g_pg.host_vsh_handle);
        g_pg.host_vsh_handle = 0;
    }
    for (stage = 0; stage < 4u; ++stage)
        g_pg.guest_texture[stage] = NULL;
    for (stage = 0; stage < MAX_GUEST_TEXTURE_CACHE; ++stage) {
        GuestTextureCacheEntry *entry = &g_pg.texture_cache[stage];
        if (entry->texture) {
            entry->texture->lpVtbl->Release(entry->texture);
            entry->texture = NULL;
        }
    }
    d3d8_BindRenderTargets(NULL, NULL, 0, 0);
    g_pg.bound_color = NULL;
    g_pg.bound_depth = NULL;
    g_pg.last_drawn_color = NULL;
    g_pg.last_resolved_color = NULL;
    for (stage = 0; stage < MAX_GUEST_COLOR_SURFACES; ++stage)
        release_guest_color_surface(&g_pg.color_surfaces[stage]);
    for (stage = 0; stage < MAX_GUEST_DEPTH_SURFACES; ++stage)
        release_guest_depth_surface(&g_pg.depth_surfaces[stage]);
    release_satellite_depth_sample();
    g_pg.initialized = 0;
    fprintf(stderr, "[PGRAPH-D3D11] Translator shut down (draws=%u, verts=%u)\n",
            g_pg.stats.draw_calls, g_pg.stats.vertices_submitted);
}

/* ======================================================================
 * Draw Submission
 * ====================================================================== */

/* Read-only, bounded GPU evidence for the flare visibility
 * probe. Its Z write is disabled, so a readback after the draw still contains
 * the scene depth against which the alpha quad should have been tested. */
static void debug_trace_flare_sample_source(const char *prefix, uint32_t capture,
                                            const NV2ACombinerState *actual_state)
{
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Resource *resource = NULL;
    D3D11_RESOURCE_DIMENSION dimension;
    GuestColorSurface *source;
    char path[1024];
    char hlsl[65536];
    FILE *file;

    /* Called only inside the bounded, opt-in flare-pass capture. A reduction
     * uses a separate source/target with full color writes and no depth. */
    if (!context || !g_pg.tex[0].enabled || g_pg.depth_test ||
        g_pg.color_mask != 0x01010101u || !g_pg.bound_color)
        return;
    source = find_guest_color_surface(resolved_texture_offset(0));
    if (!source)
        return;
    /* Self-sampling filters bind a resolved snapshot; capture the actual SRV. */
    ID3D11DeviceContext_PSGetShaderResources(context, 0, 1, &srv);
    if (srv) {
        ID3D11ShaderResourceView_GetResource(srv, &resource);
        if (resource) {
            ID3D11Resource_GetType(resource, &dimension);
            fprintf(stderr, "[FLARE-SAMPLE-SOURCE] n=%u expected=%08X "
                    "same_resource=%d dimension=%u gpu=%d\n", capture,
                    source->offset, resource == (ID3D11Resource *)source->texture,
                    (unsigned)dimension, source->gpu_drawn);
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                snprintf(path, sizeof(path), "%s-%02u-sampled.bmp", prefix, capture);
                d3d8_DebugCaptureTextureToPath((ID3D11Texture2D *)resource, path);
            }
            ID3D11Resource_Release(resource);
        }
        ID3D11ShaderResourceView_Release(srv);
    } else {
        fprintf(stderr, "[FLARE-SAMPLE-SOURCE] n=%u unbound\n", capture);
    }
    /* The final color filter indexes a 256-entry palette at stage 2.
     * Keep this readback inside the bounded diagnostic, never normal drawing. */
    if (actual_state && actual_state->tex_mode[2] == NV2A_TEXMODE_DOT_ST) {
        srv = NULL; resource = NULL;
        ID3D11DeviceContext_PSGetShaderResources(context, 2, 1, &srv);
        if (srv) {
            ID3D11ShaderResourceView_GetResource(srv, &resource);
            if (resource) {
                ID3D11Resource_GetType(resource, &dimension);
                if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    snprintf(path, sizeof(path), "%s-%02u-palette.bmp", prefix, capture);
                    d3d8_DebugCaptureTextureToPath((ID3D11Texture2D *)resource, path);
                }
                ID3D11Resource_Release(resource);
            }
            ID3D11ShaderResourceView_Release(srv);
        }
    }
    /* Capture the actual submitted state, including guest-grid/depth flags.
     * Reconstructing only NV2A registers omits host translation semantics. */
    if (actual_state && d3d8_combiners_generate_hlsl(
            actual_state, hlsl, sizeof(hlsl)) > 0) {
        snprintf(path, sizeof(path), "%s-%02u-combiner.hlsl", prefix, capture);
        file = fopen(path, "w");
        if (file) { fputs(hlsl, file); fclose(file); }
    }
}

static void debug_trace_flare_probe_depth(uint32_t capture, uint32_t vertices)
{
    GuestDepthSurface *depth = g_pg.bound_depth;
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    float xmin, xmax, ymin, ymax, z, scale_x, scale_y;
    uint32_t vertex, x, y, count = 0, lequal = 0;
    uint32_t minimum = 0xFFFFFFu, maximum = 0u;
    int left, right, top, bottom;
    HRESULT hr;

    if (!depth || !depth->texture || !device || !context || vertices < 4u ||
        !g_pg.depth_test || g_pg.depth_write ||
        depth->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8)
        return;
    xmin = xmax = g_pg.immediate_vertices[0][0][0];
    ymin = ymax = g_pg.immediate_vertices[0][0][1];
    z = g_pg.immediate_vertices[0][0][2];
    if (!isfinite(z)) return;
    for (vertex = 0; vertex < 4u; ++vertex) {
        const float *p = g_pg.immediate_vertices[vertex][0];
        if (!isfinite(p[0]) || !isfinite(p[1]) || p[2] != z) return;
        xmin = fminf(xmin, p[0]); xmax = fmaxf(xmax, p[0]);
        ymin = fminf(ymin, p[1]); ymax = fmaxf(ymax, p[1]);
    }
    ID3D11Texture2D_GetDesc(depth->texture, &desc);
    if (desc.Width > 2048u || desc.Height > 2048u ||
        (desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT &&
         desc.Format != DXGI_FORMAT_R24G8_TYPELESS) || desc.SampleDesc.Count != 1u)
        return;
    if (!g_pg.bound_color || !g_pg.bound_color->logical_width ||
        !g_pg.bound_color->logical_height) return;
    scale_x = (float)desc.Width / g_pg.bound_color->logical_width;
    scale_y = (float)desc.Height / g_pg.bound_color->logical_height;
    xmin *= scale_x; xmax *= scale_x;
    ymin *= scale_y; ymax *= scale_y;
    left = (int)fmaxf(0.0f, fminf((float)desc.Width, floorf(xmin)));
    right = (int)fmaxf(0.0f, fminf((float)desc.Width, ceilf(xmax)));
    top = (int)fmaxf(0.0f, fminf((float)desc.Height, floorf(ymin)));
    bottom = (int)fmaxf(0.0f, fminf((float)desc.Height, ceilf(ymax)));
    if (left >= right || top >= bottom) return;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr) || !staging) return;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)depth->texture);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0u,
                                D3D11_MAP_READ, 0u, &mapped);
    if (SUCCEEDED(hr)) {
        /* Reuse this opt-in readback for a per-pixel visibility oracle. Keep
         * only the probe rectangle, not another full-frame depth capture. */
        const char *prefix = pgraph_cached_getenv("MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX");
        FILE *samples = NULL;
        if (prefix && *prefix) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s-%02u-depth.csv", prefix, capture);
            samples = fopen(path, "w");
            if (samples) fprintf(samples, "x,y,scene_depth,probe_depth\n");
        }
        for (y = (uint32_t)top; y < (uint32_t)bottom; ++y) {
            const uint32_t *row = (const uint32_t *)(
                (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
            for (x = (uint32_t)left; x < (uint32_t)right; ++x) {
                const uint32_t sample = row[x] & 0xFFFFFFu;
                if (samples) fprintf(samples, "%u,%u,%u,%.9g\n", x, y, sample, z);
                if (sample < minimum) minimum = sample;
                if (sample > maximum) maximum = sample;
                if (z <= (float)sample) ++lequal;
                ++count;
            }
        }
        if (samples) fclose(samples);
        fprintf(stderr, "[FLARE-PROBE-DEPTH] n=%u zeta=%08X rect=%d,%d,%d,%d "
                "probe_z=%.9g scene_z=%u..%u lequal=%u/%u func=%u\n",
                capture, depth->offset, left, top, right, bottom,
                z, minimum, maximum, lequal, count, g_pg.depth_func);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
    } else {
        fprintf(stderr, "[FLARE-PROBE-DEPTH] n=%u readback-failed=%08lX\n",
                capture, hr);
    }
    ID3D11Texture2D_Release(staging);
}

/* The retail flare composite uses xboxBr3D: v0 is transformed by the three
 * object/view rows, then c26/c27 project the camera-space result directly to
 * Xbox screen coordinates. ImmediateOutputVertex is only the fixed-function
 * fallback and therefore cannot reveal a programmable flare that becomes
 * screen-filling after its vertex shader. Reproduce the authored shader's
 * inexpensive position path for diagnostics so anomalous near-camera W or
 * projected coverage can be caught without a GPU readback on every flare. */
static int debug_project_br3d_flare_bounds(uint32_t vertices,
                                            float *minimum_x,
                                            float *minimum_y,
                                            float *maximum_x,
                                            float *maximum_y,
                                            float *minimum_w,
                                            float *maximum_w)
{
    const uint32_t object_view_row = 23u; /* C_OBJ_VIEW_M0: -73 + 96 */
    const uint32_t projection_0 = 26u;    /* C_PROJ_SCREEN_0: -70 + 96 */
    const uint32_t projection_1 = 27u;    /* C_PROJ_SCREEN_1: -69 + 96 */
    uint32_t vertex;

    if (!vertices || vertices > 64u || !minimum_x || !minimum_y ||
        !maximum_x || !maximum_y || !minimum_w || !maximum_w)
        return 0;
    *minimum_x = *minimum_y = *minimum_w = INFINITY;
    *maximum_x = *maximum_y = *maximum_w = -INFINITY;
    for (vertex = 0u; vertex < vertices; ++vertex) {
        const float *position = g_pg.immediate_vertices[vertex][0];
        float camera[3];
        float screen_x, screen_y;
        uint32_t row, component;

        for (row = 0u; row < 3u; ++row) {
            camera[row] = 0.0f;
            for (component = 0u; component < 4u; ++component)
                camera[row] += position[component] *
                    u2f(g_pg.vsh_constants[object_view_row + row][component]);
        }
        if (!isfinite(camera[0]) || !isfinite(camera[1]) ||
            !isfinite(camera[2]) || fabsf(camera[2]) < 1.0e-20f)
            return 0;
        screen_x = (camera[0] * u2f(g_pg.vsh_constants[projection_0][0]) +
                    camera[2] * u2f(g_pg.vsh_constants[projection_1][0])) /
                   camera[2];
        screen_y = (camera[1] * u2f(g_pg.vsh_constants[projection_0][1]) +
                    camera[2] * u2f(g_pg.vsh_constants[projection_1][1])) /
                   camera[2];
        if (!isfinite(screen_x) || !isfinite(screen_y))
            return 0;
        *minimum_x = fminf(*minimum_x, screen_x);
        *minimum_y = fminf(*minimum_y, screen_y);
        *maximum_x = fmaxf(*maximum_x, screen_x);
        *maximum_y = fmaxf(*maximum_y, screen_y);
        *minimum_w = fminf(*minimum_w, camera[2]);
        *maximum_w = fmaxf(*maximum_w, camera[2]);
    }
    return 1;
}

static void prepare_host_depth_surface(IDirect3DDevice8 *dev)
{
    if (!dev || !g_pg.depth_test)
        return;
    if (g_pg.host_zeta_valid &&
        g_pg.host_zeta_offset == g_pg.surface_zeta_offset)
        return;

    /* Distinct guest zeta offsets now own distinct host DSVs. */
    g_pg.host_zeta_offset = g_pg.surface_zeta_offset;
    g_pg.host_zeta_valid = 1;

    if (pgraph_cached_getenv("MERCENARIES_TRACE_IMMEDIATE") != NULL) {
        fprintf(stderr, "[PGRAPH-ZETA] host depth now represents %08X\n",
                g_pg.host_zeta_offset);
    }
}

static void trace_guest_draw_state(void)
{
    if (g_pg.stats.draw_calls < 16u) {
        fprintf(stderr,
                "[PGRAPH-DRAW-STATE] next=%u target=%08X zeta=%08X "
                "depth=%d/%d/%u alpha=%d blend=%d %04X/%04X color=%X\n",
                g_pg.stats.draw_calls + 1u, g_pg.surface_color_offset,
                g_pg.surface_zeta_offset, g_pg.depth_test, g_pg.depth_write,
                g_pg.depth_func, g_pg.alpha_test, g_pg.blend_enable,
                g_pg.blend_sfactor, g_pg.blend_dfactor, g_pg.color_mask);
    }
}
static void trace_texture_edge_candidate(const char *kind,
                                         uint32_t vertex_count)
{
    typedef struct {
        uint32_t offset, format, address, filter, vsh;
        uint32_t alpha_blend;
    } candidate_t;
    static candidate_t candidates[256];
    static uint32_t candidate_count;
    static uint32_t flare_capture_count;
    static ULONGLONG trace_start_ms;
    candidate_t key;
    const char *delay_env;
    ULONGLONG delay_ms;
    uint32_t i;

    if (trace_start_ms == 0u)
        trace_start_ms = GetTickCount64();
    delay_env = pgraph_cached_getenv(
        "MERCENARIES_TRACE_TEXTURE_EDGES_DELAY_MS");
    delay_ms = delay_env && delay_env[0] != '\0' ?
        strtoull(delay_env, NULL, 0) : 0u;
    if (pgraph_cached_getenv("MERCENARIES_TRACE_TEXTURE_EDGES") == NULL ||
        GetTickCount64() - trace_start_ms < delay_ms ||
        !g_pg.tex[0].enabled || (!g_pg.alpha_test && !g_pg.blend_enable) ||
        candidate_count >= 256u)
        return;
    key.offset = resolved_texture_offset(0u);
    key.format = g_pg.tex[0].format;
    key.address = g_pg.tex[0].address;
    key.filter = g_pg.tex[0].filter;
    key.vsh = g_pg.host_vsh_hash;
    key.alpha_blend = (uint32_t)g_pg.alpha_test |
        ((uint32_t)g_pg.blend_enable << 1u) |
        (g_pg.alpha_func << 8u) | (g_pg.alpha_ref << 16u);
    for (i = 0u; i < candidate_count; ++i)
        if (memcmp(&candidates[i], &key, sizeof(key)) == 0)
            return;
    candidates[candidate_count++] = key;
    fprintf(stderr,
            "[PGRAPH-TEXTURE-EDGE] draw=%u kind=%s vertices=%u "
            "off=%08X fmt=%08X color=%02X border=%s rect=%08X "
            "address=%08X filter=%08X alpha=%d/%u/%u "
            "blend=%d/%04X/%04X vsh=%08X target=%08X mask=%08X "
            "depth=%d/%d/%u\n",
            g_pg.stats.draw_calls + 1u, kind, vertex_count, key.offset,
            key.format,
            (key.format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8,
            (key.format & NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE) ?
                "color" : "texture",
            g_pg.tex[0].image_rect, key.address, key.filter,
            g_pg.alpha_test, g_pg.alpha_func, g_pg.alpha_ref,
            g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
            key.vsh, g_pg.surface_color_offset, g_pg.color_mask,
            g_pg.depth_test, g_pg.depth_write, g_pg.depth_func);
    /* Foliage, terrain, and weather materials frequently source their cutout
     * or detail texture from stages 1-3.  Logging stage zero alone made a
     * texture-border diagnosis ambiguous, so retain the compact draw key above
     * while recording every enabled stage only for this opt-in trace. */
    for (i = 0u; i < 4u; ++i) {
        uint32_t width = 1u, height = 1u;
        const uint32_t stage_format = g_pg.tex[i].format;
        const uint32_t stage_color =
            (stage_format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
        if (!g_pg.tex[i].enabled)
            continue;
        get_guest_texture_dimensions(stage_format, g_pg.tex[i].image_rect,
                                     stage_color, &width, &height);
        fprintf(stderr,
                "  [PGRAPH-TEXTURE-EDGE-STAGE] stage=%u off=%08X "
                "fmt=%08X color=%02X border=%s rect=%08X size=%ux%u "
                "address=%08X filter=%08X\n",
                i, resolved_texture_offset(i), stage_format, stage_color,
                (stage_format & NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE) ?
                    "color" : "texture",
                g_pg.tex[i].image_rect, width, height,
                g_pg.tex[i].address, g_pg.tex[i].filter);
    }
    if (flare_capture_count < 8u &&
        g_pg.blend_enable &&
        g_pg.blend_sfactor == NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA &&
        g_pg.blend_dfactor == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE) {
        const char *prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_FLARE_TEXTURE_PREFIX");
        GuestColorSurface *source = find_guest_color_surface(key.offset);
        char path[MAX_PATH];
        if (prefix && *prefix && source && source->gpu_drawn) {
            snprintf(path, sizeof(path), "%s-%02u-source-%08X.bmp", prefix,
                     flare_capture_count, source->offset);
            d3d8_DebugCaptureTextureToPath(source->texture, path);
            if (g_pg.bound_color && g_pg.bound_color->texture) {
                snprintf(path, sizeof(path), "%s-%02u-target-%08X.bmp", prefix,
                         flare_capture_count, g_pg.bound_color->offset);
                d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
            }
            ++flare_capture_count;
        }
    }
    fflush(stderr);
}
static void trace_point_sprite_candidate(const char *kind,
                                         uint32_t vertex_count)
{
    static uint32_t reports;

    if (pgraph_cached_getenv("MERCENARIES_TRACE_POINT_SPRITES") == NULL ||
        g_pg.d3d_prim_type != D3DPT_POINTLIST || reports++ >= 256u)
        return;
    fprintf(stderr,
            "[PGRAPH-POINT] draw=%u kind=%s vertices=%u params=%u "
            "smooth=%u size=%u(%.3gpx) target=%08X tex0=%08X/%08X "
            "blend=%d/%04X/%04X alpha=%d/%u/%u vsh=%08X "
            "coeff=%g,%g,%g,%g,%g,%g,%g,%g\n",
            g_pg.stats.draw_calls + 1u, kind, vertex_count,
            g_pg.point_params_enable, g_pg.point_smooth_enable,
            g_pg.point_size, (double)g_pg.point_size / 8.0,
            g_pg.surface_color_offset, resolved_texture_offset(0),
            g_pg.tex[0].format, g_pg.blend_enable, g_pg.blend_sfactor,
            g_pg.blend_dfactor, g_pg.alpha_test, g_pg.alpha_func,
            g_pg.alpha_ref, g_pg.host_vsh_hash,
            (double)u2f(g_pg.point_params[0]),
            (double)u2f(g_pg.point_params[1]),
            (double)u2f(g_pg.point_params[2]),
            (double)u2f(g_pg.point_params[3]),
            (double)u2f(g_pg.point_params[4]),
            (double)u2f(g_pg.point_params[5]),
            (double)u2f(g_pg.point_params[6]),
            (double)u2f(g_pg.point_params[7]));
    fflush(stderr);
}
static void trace_line_candidate(const char *kind, uint32_t vertex_count)
{
    static uint32_t reports;

    if (pgraph_cached_getenv("MERCENARIES_TRACE_LINE_EFFECTS") == NULL ||
        (pgraph_cached_getenv("MERCENARIES_TRACE_LINE_EFFECTS_GAMEPLAY_ONLY") != NULL &&
         g_mercenaries_gameplay_capture_active == 0u) ||
        (g_pg.d3d_prim_type != D3DPT_LINELIST &&
         g_pg.d3d_prim_type != D3DPT_LINESTRIP) || reports++ >= 256u)
        return;
    fprintf(stderr,
            "[PGRAPH-LINE] draw=%u kind=%s prim=%d vertices=%u "
            "target=%08X tex0=%08X/%08X blend=%d/%04X/%04X "
            "alpha=%d/%u/%u depth=%d/%d/%u smooth=%u vsh=%08X\n",
            g_pg.stats.draw_calls + 1u, kind, g_pg.d3d_prim_type,
            vertex_count, g_pg.surface_color_offset,
            resolved_texture_offset(0), g_pg.tex[0].format,
            g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
            g_pg.alpha_test, g_pg.alpha_func, g_pg.alpha_ref,
            g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
            g_pg.line_smooth_enable,
            g_pg.host_vsh_hash);
    fflush(stderr);
}
static void debug_trace_bound_depth(uint32_t index)
{
    const char *enabled = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_DEPTH_TRACE");
    if (!enabled) enabled = pgraph_cached_getenv("MERCENARIES_CAPTURE_SHADOW_QUAD");
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    uint32_t min_z = 0xFFFFFFu;
    uint32_t max_z = 0u;
    uint64_t sum_z = 0u;
    uint32_t touched = 0u;
    uint32_t stencil_nonzero = 0u;
    uint32_t y;
    static const uint32_t sample_xy[][2] = {
        { 300u, 180u }, { 320u, 200u }, { 340u, 220u },
        { 360u, 240u }, { 380u, 260u }
    };

    if (!enabled || !g_pg.bound_depth || !g_pg.bound_depth->texture ||
        g_pg.bound_depth->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 ||
        !device || !context)
        return;
    ID3D11Texture2D_GetDesc(g_pg.bound_depth->texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[PGRAPH-DEPTH-TRACE] index=%u create_failed=%08lX\n",
                index, hr);
        return;
    }
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)g_pg.bound_depth->texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0u,
                                 D3D11_MAP_READ, 0u, &mapped);
    if (SUCCEEDED(hr)) {
        for (y = 0u; y < desc.Height; ++y) {
            const uint32_t *row = (const uint32_t *)
                ((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
            uint32_t x;
            for (x = 0u; x < desc.Width; ++x) {
                const uint32_t z = row[x] & 0xFFFFFFu;
                if (row[x] >> 24) ++stencil_nonzero;
                if (z < min_z) min_z = z;
                if (z > max_z) max_z = z;
                sum_z += z;
                if (z != 0xFFFFFFu) ++touched;
            }
        }
        fprintf(stderr,
                "[PGRAPH-DEPTH-TRACE] index=%u control0=%08X size=%ux%u "
                "min=%06X max=%06X avg=%.3f touched=%u stencil_nonzero=%u samples=",
                index, g_pg.control0, desc.Width, desc.Height, min_z, max_z,
                desc.Width && desc.Height ?
                    (double)sum_z / ((double)desc.Width * desc.Height) : 0.0,
                touched, stencil_nonzero);
        for (y = 0u; y < sizeof(sample_xy) / sizeof(sample_xy[0]); ++y) {
            const uint32_t sx = sample_xy[y][0] < desc.Width ?
                sample_xy[y][0] : desc.Width - 1u;
            const uint32_t sy = sample_xy[y][1] < desc.Height ?
                sample_xy[y][1] : desc.Height - 1u;
            const uint32_t *row = (const uint32_t *)
                ((const uint8_t *)mapped.pData + (size_t)sy * mapped.RowPitch);
            fprintf(stderr, "%s%u,%u:%06X", y ? "," : "", sx, sy,
                    row[sx] & 0xFFFFFFu);
        }
        fputc('\n', stderr);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
    } else {
        fprintf(stderr,
                "[PGRAPH-DEPTH-TRACE] index=%u map_failed=%08lX\n",
                index, hr);
    }
    ID3D11Texture2D_Release(staging);
}
static int debug_gameplay_capture_target_matches(void);
static void debug_write_array_capture_state(FILE *output);
static uint32_t g_debug_satellite_capture_epoch;
static int g_debug_satellite_filter_window;
static uint32_t g_debug_satellite_array_ordinal;
static uint32_t g_debug_satellite_array_capture_count;
static int g_debug_satellite_post_window_started;
static uint32_t g_debug_satellite_filter_texture;
static int g_debug_satellite_final_candidate;
/* Satellite diagnostics share the explicit capture gate. They observe the
 * run 1717 decision without changing surface ownership or texture selection. */
static uint32_t g_debug_satellite_depth_capture_draw;
static int g_debug_satellite_depth_capture_prepared;

static int debug_satellite_capture_armed(void)
{
    static FILETIME last_write;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    const char *prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    const char *gate = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_GATE_FILE");
    if (!prefix || !*prefix) return 0;
    if (!gate || !*gate) {
        if (!g_debug_satellite_capture_epoch) g_debug_satellite_capture_epoch = 1u;
        return 1;
    }
    if (!GetFileAttributesExA(gate, GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    if (!g_debug_satellite_capture_epoch ||
        CompareFileTime(&last_write, &attributes.ftLastWriteTime) != 0) {
        last_write = attributes.ftLastWriteTime;
        ++g_debug_satellite_capture_epoch;
        fprintf(stderr, "[SATELLITE-GATE] epoch=%u draw=%u\n",
                g_debug_satellite_capture_epoch, g_pg.stats.draw_calls + 1u);
        fflush(stderr);
    }
    return g_debug_satellite_capture_epoch <= 3u;
}

/* Tight raw rows preserve alpha/stencil and channel order; BMPs alone do not.
 * The sidecar describes the DXGI format instead of labelling depth as RGBA. */
static void debug_capture_satellite_raw(ID3D11Texture2D *texture,
                                         const char *label, uint32_t index)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const char *prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    char path[1024];
    HRESULT hr;
    FILE *raw, *meta;
    uint32_t y;
    int written = 1;
    if (!prefix && (pgraph_cached_getenv("MERCENARIES_CAPTURE_CANVAS_MASK") ||
                    pgraph_cached_getenv("MERCENARIES_CAPTURE_SHADER_HASH") ||
                    pgraph_cached_getenv("MERCENARIES_CAPTURE_SHADOW_QUAD") ||
                    pgraph_cached_getenv("MERCENARIES_CAPTURE_GAMEPLAY_GEOMETRY_ONLY")))
        prefix = pgraph_cached_getenv("MERCENARIES_CAPTURE_GAMEPLAY_DRAW_PREFIX");
    if (!texture || !device || !context || !prefix || !*prefix) return;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R24G8_TYPELESS &&
        desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT) {
        fprintf(stderr, "[SATELLITE-RAW] index=%u label=%s unsupported=%u\n",
                index, label, (unsigned)desc.Format);
        return;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr)) goto done;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)texture);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0u,
                                D3D11_MAP_READ, 0u, &mapped);
    if (FAILED(hr)) goto done;
    snprintf(path, sizeof(path), "%s-%02u-%s.raw", prefix, index, label);
    raw = fopen(path, "wb");
    if (raw) {
        for (y = 0u; y < desc.Height; ++y) {
            const uint8_t *row = (const uint8_t *)mapped.pData +
                (size_t)y * mapped.RowPitch;
            if (fwrite(row, 4u, desc.Width, raw) != desc.Width) written = 0;
        }
        if (fclose(raw) != 0) written = 0;
    } else written = 0;
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0u);
    snprintf(path, sizeof(path), "%s-%02u-%s.raw.txt", prefix, index, label);
    meta = fopen(path, "w");
    if (meta) {
        fprintf(meta, "width=%u height=%u dxgi_format=%u row_bytes=%u "
                "draw=%u vsh=%08X complete=%d\n",
                desc.Width, desc.Height, (unsigned)desc.Format,
                desc.Width * 4u, g_pg.stats.draw_calls + 1u,
                g_pg.host_vsh_hash, written);
        fclose(meta);
    }
    fprintf(stderr, "[SATELLITE-RAW] index=%u label=%s size=%ux%u "
            "format=%u complete=%d\n", index, label,
            desc.Width, desc.Height, (unsigned)desc.Format, written);
 done:
    if (FAILED(hr)) fprintf(stderr,
        "[SATELLITE-RAW] index=%u label=%s failed=%08lX\n", index, label, hr);
    if (staging) ID3D11Texture2D_Release(staging);
}

static void debug_trace_satellite_depth_decision(uint32_t stage,
    uint32_t color_format, const char *reason,
    GuestDepthSurface *matched_source, int prepared)
{
    static uint32_t reports, report_epoch;
    uint32_t report_index;
    GuestDepthSurface *source;
    uint32_t width, height, backing_width = 0u, backing_height = 0u;
    uint32_t pitch;
    if (stage != 0u) return;
    g_debug_satellite_depth_capture_draw = 0u;
    g_debug_satellite_depth_capture_prepared = 0;
    if ((g_pg.host_vsh_hash != 0x1730DD1Au &&
         (g_pg.surface_color_offset != 0x02980000u ||
          resolved_texture_offset(stage) != 0x01E34000u)) ||
        !debug_satellite_capture_armed()) return;
    if (report_epoch != g_debug_satellite_capture_epoch) {
        report_epoch = g_debug_satellite_capture_epoch;
        reports = 0u;
    }
    if (reports >= 16u) return;
    report_index = (report_epoch - 1u) * 16u + reports;
    source = find_guest_depth_surface(resolved_texture_offset(stage));
    get_guest_texture_dimensions(g_pg.tex[stage].format,
        g_pg.tex[stage].image_rect, color_format, &width, &height);
    pitch = g_pg.tex[stage].control1 >> 16;
    if (pitch < width * 4u) pitch = width * 4u;
    if (source) guest_surface_backing_dimensions(source->logical_width,
        source->logical_height, source->anti_aliasing,
        &backing_width, &backing_height);
    fprintf(stderr, "[SATELLITE-DEPTH-DECISION] index=%u draw=%u "
        "vsh=%08X reached=%d stage=%u enabled=%d reason=%s prepare=%s "
        "target=%08X bound=%08X source=%08X mode=%u "
        "shader=%08X/%08X dot=%08X blend=%d depth=%d/%d "
        "texfmt=%08X color=%02X rect=%08X size=%ux%u "
        "ctl0=%08X ctl1=%08X pitch=%u filter=%08X address=%08X "
        "zeta=%d valid=%d format=%u serial=%llu host=%ux%u logical=%ux%u "
        "backing=%ux%u zeta_pitch=%u aa=%u stencil=%d/%02X/%02X\n",
        report_index, g_pg.stats.draw_calls + 1u, g_pg.host_vsh_hash,
        g_pg.host_vsh_hash == 0x1730DD1Au, stage, g_pg.tex[stage].enabled,
        reason, matched_source ? (prepared ? "ok" : "failed") : "not-run",
        g_pg.surface_color_offset, g_pg.bound_color ? g_pg.bound_color->offset : 0u,
        resolved_texture_offset(stage), g_pg.draw_mode,
        g_pg.shader_stage_program, g_pg.shader_other_stage_input,
        g_pg.shader_dot_mapping, g_pg.blend_enable, g_pg.depth_test, g_pg.depth_write,
        g_pg.tex[stage].format, color_format, g_pg.tex[stage].image_rect,
        width, height, g_pg.tex[stage].control0, g_pg.tex[stage].control1,
        pitch, g_pg.tex[stage].filter, g_pg.tex[stage].address,
        source != NULL, source ? source->valid : 0, source ? source->format : 0u,
        (unsigned long long)(source ? source->write_serial : 0u),
        source ? source->width : 0u, source ? source->height : 0u,
        source ? source->logical_width : 0u, source ? source->logical_height : 0u,
        backing_width, backing_height, source ? source->pitch : 0u,
        source ? source->anti_aliasing : 0u,
        source ? source->uniform_stencil_known : 0,
        source ? source->uniform_stencil_value : 0u,
        source ? source->possible_stencil_bits : 0u);
    /* At most two large readbacks, even if the gate stays armed for minutes. */
    if (reports < 2u) {
        if (source) debug_capture_satellite_raw(source->texture, "live-zeta", report_index);
        if (prepared) debug_capture_satellite_raw(
            g_pg.satellite_depth_sample_texture, "transient-pre", report_index);
        g_debug_satellite_depth_capture_draw = g_pg.stats.draw_calls + 1u;
        g_debug_satellite_depth_capture_prepared = prepared;
    }
    ++reports;
    fflush(stderr);
}

static void debug_capture_satellite_sampled_texture(uint32_t index, uint32_t stage)
{
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Resource *resource = NULL;
    D3D11_RESOURCE_DIMENSION dimension;
    char path[1024], label[32];
    const char *prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    if (!context || !prefix || !*prefix) return;
    snprintf(label, sizeof(label), "sampled-t%u", stage);
    /* After DrawPrimitiveUP, this is the SRV actually consumed by the GPU. */
    ID3D11DeviceContext_PSGetShaderResources(context, stage, 1u, &srv);
    if (srv) ID3D11ShaderResourceView_GetResource(srv, &resource);
    fprintf(stderr, "[SATELLITE-SAMPLED] index=%u stage=%u draw=%u bound=%d transient=%d\n",
        index, stage, g_pg.stats.draw_calls + 1u, srv != NULL,
        resource && resource == (ID3D11Resource *)g_pg.satellite_depth_sample_texture);
    if (resource) {
        ID3D11Resource_GetType(resource, &dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            debug_capture_satellite_raw((ID3D11Texture2D *)resource, label, index);
            snprintf(path, sizeof(path), "%s-%02u-%s.bmp", prefix, index, label);
            d3d8_DebugCaptureTextureToPath((ID3D11Texture2D *)resource, path);
        }
        ID3D11Resource_Release(resource);
    }
    if (srv) ID3D11ShaderResourceView_Release(srv);
}

/* RedCanvas::_CopyToOffscreen issues NV09F SRCCOPY after the scene and
 * monochrome filter. Its destination may be the allocation previously used
 * for depth. This explicit writer replaces the requested rectangle with
 * color; looking up that address alone must never perform this operation. */
int pgraph_d3d11_image_blit(void)
{
    NV2AState *d = nv2a_get_state();
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *context = d3d8_GetD3D11Context();
    GuestColorSurface *source = NULL, *destination = NULL;
    ID3D11Texture2D *snapshot = NULL;
    ID3D11RenderTargetView *saved_rtv = NULL;
    ID3D11DepthStencilView *saved_dsv = NULL;
    ID3D11ShaderResourceView *saved_srvs[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *null_srvs[4] = { NULL, NULL, NULL, NULL };
    uint8_t *source_dma, *dest_dma;
    hwaddr source_len = 0, dest_len = 0;
    uint64_t source_addr = 0u, dest_addr = 0u, source_end, dest_end;
    uint32_t width, height, in_x, in_y, out_x, out_y, format;
    uint32_t source_guest_width, source_guest_height;
    uint32_t dest_guest_width, dest_guest_height;
    PgraphImageBlitRect source_rect, dest_rect;
    D3D11_BOX box;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr = S_OK;
    const char *reason = "no-device";
    static uint32_t reports, capture_epoch, captures;
    uint32_t capture_index = 0u;
    int capture = 0, copied = 0;
    if (!d || !device || !context) return 0;
    width = d->pgraph.image_blit_size & 0xFFFFu;
    height = d->pgraph.image_blit_size >> 16;
    in_x = d->pgraph.image_blit_point_in & 0xFFFFu;
    in_y = d->pgraph.image_blit_point_in >> 16;
    out_x = d->pgraph.image_blit_point_out & 0xFFFFu;
    out_y = d->pgraph.image_blit_point_out >> 16;
    format = d->pgraph.context_surfaces_2d_color_format;
    reason = "empty";
    if (!width || !height) goto done;
    reason = "context-mismatch";
    if (d->pgraph.context_surfaces_2d_object !=
        d->pgraph.image_blit_context_surfaces) goto done;
    reason = "operation";
    if (d->pgraph.image_blit_operation != NV09F_SET_OPERATION_SRCCOPY) goto done;
    /* A8R8G8B8/Y32 preserve every byte. X8 formats require alpha patching;
     * leave them explicitly unsupported until their semantics are implemented. */
    reason = "format";
    if (format != NV062_SET_COLOR_FORMAT_LE_A8R8G8B8 &&
        format != NV062_SET_COLOR_FORMAT_LE_Y32) goto done;
    source_dma = nv_dma_map(d, d->pgraph.context_surfaces_2d_dma_source, &source_len);
    dest_dma = nv_dma_map(d, d->pgraph.context_surfaces_2d_dma_dest, &dest_len);
    reason = "dma";
    if (!source_dma || !dest_dma || (uintptr_t)source_dma < (uintptr_t)d->vram_ptr ||
        (uintptr_t)dest_dma < (uintptr_t)d->vram_ptr) goto done;
    source_addr = (uintptr_t)source_dma - (uintptr_t)d->vram_ptr +
        (uint64_t)d->pgraph.context_surfaces_2d_source_offset;
    dest_addr = (uintptr_t)dest_dma - (uintptr_t)d->vram_ptr +
        (uint64_t)d->pgraph.context_surfaces_2d_dest_offset;
    reason = "pitch";
    if ((uint64_t)(in_x + width) * 4u > d->pgraph.context_surfaces_2d_source_pitch ||
        (uint64_t)(out_x + width) * 4u > d->pgraph.context_surfaces_2d_dest_pitch)
        goto done;
    source_end = (uint64_t)(in_y + height - 1u) *
        d->pgraph.context_surfaces_2d_source_pitch + (uint64_t)(in_x + width) * 4u;
    dest_end = (uint64_t)(out_y + height - 1u) *
        d->pgraph.context_surfaces_2d_dest_pitch + (uint64_t)(out_x + width) * 4u;
    reason = "dma-bounds";
    if ((uint64_t)d->pgraph.context_surfaces_2d_source_offset + source_end > source_len ||
        (uint64_t)d->pgraph.context_surfaces_2d_dest_offset + dest_end > dest_len ||
        source_addr + source_end > memory_region_size(d->vram) ||
        dest_addr + dest_end > memory_region_size(d->vram)) goto done;
    source = find_guest_color_surface((uint32_t)source_addr);
    destination = find_guest_color_surface((uint32_t)dest_addr);
    /* NV09F writes may precede any 3D binding of their destination. */
    if (!destination && source && source->gpu_drawn &&
        !source->depth_alias_source_offset &&
        source->format == NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 &&
        source->pitch == d->pgraph.context_surfaces_2d_source_pitch)
        destination = get_image_blit_depth_destination((uint32_t)dest_addr,
            d->pgraph.context_surfaces_2d_dest_pitch);
    reason = "gpu-surface-missing";
    if (!source || !destination || !source->gpu_drawn ||
        !source->texture || !destination->texture) goto done;
    reason = "source-is-packed-depth";
    if (source->depth_alias_source_offset) goto done;
    reason = "surface-format-or-pitch";
    if (source->format != destination->format ||
        source->pitch != d->pgraph.context_surfaces_2d_source_pitch ||
        destination->pitch != d->pgraph.context_surfaces_2d_dest_pitch) goto done;
    guest_surface_backing_dimensions(source->logical_width, source->logical_height,
        source->anti_aliasing, &source_guest_width, &source_guest_height);
    guest_surface_backing_dimensions(destination->logical_width, destination->logical_height,
        destination->anti_aliasing, &dest_guest_width, &dest_guest_height);
    reason = "surface-bounds-or-scale";
    if (!pgraph_image_blit_scale_rect(in_x, in_y, width, height,
            source_guest_width, source_guest_height, source->width, source->height,
            &source_rect) ||
        !pgraph_image_blit_scale_rect(out_x, out_y, width, height,
            dest_guest_width, dest_guest_height, destination->width, destination->height,
            &dest_rect) ||
        source_rect.right - source_rect.left != dest_rect.right - dest_rect.left ||
        source_rect.bottom - source_rect.top != dest_rect.bottom - dest_rect.top)
        goto done;
    if (debug_satellite_capture_armed()) {
        if (capture_epoch != g_debug_satellite_capture_epoch) {
            capture_epoch = g_debug_satellite_capture_epoch;
            captures = 0u;
        }
        if (captures < 2u) {
            capture = 1;
            capture_index = (capture_epoch - 1u) * 2u + captures++;
            debug_capture_satellite_raw(source->texture, "blit-source", capture_index);
            debug_capture_satellite_raw(destination->texture, "blit-dest-pre", capture_index);
        }
    }
    if (source->texture == destination->texture) {
        ID3D11Texture2D_GetDesc(source->texture, &desc);
        desc.BindFlags = 0u;
        desc.MiscFlags = 0u;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &snapshot);
        reason = "snapshot-create";
        if (FAILED(hr)) goto done;
    }
    ID3D11DeviceContext_OMGetRenderTargets(context, 1u, &saved_rtv, &saved_dsv);
    ID3D11DeviceContext_PSGetShaderResources(context, 0u, 4u, saved_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(context, 0u, NULL, NULL);
    ID3D11DeviceContext_PSSetShaderResources(context, 0u, 4u, null_srvs);
    if (snapshot) ID3D11DeviceContext_CopyResource(context,
        (ID3D11Resource *)snapshot, (ID3D11Resource *)source->texture);
    box.left = source_rect.left; box.top = source_rect.top; box.front = 0u;
    box.right = source_rect.right; box.bottom = source_rect.bottom; box.back = 1u;
    ID3D11DeviceContext_CopySubresourceRegion(context,
        (ID3D11Resource *)destination->texture, 0u, dest_rect.left, dest_rect.top, 0u,
        (ID3D11Resource *)(snapshot ? snapshot : source->texture), 0u, &box);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1u, &saved_rtv, saved_dsv);
    ID3D11DeviceContext_PSSetShaderResources(context, 0u, 4u, saved_srvs);
    if (saved_rtv) ID3D11RenderTargetView_Release(saved_rtv);
    if (saved_dsv) ID3D11DepthStencilView_Release(saved_dsv);
    for (uint32_t stage = 0u; stage < 4u; ++stage)
        if (saved_srvs[stage]) ID3D11ShaderResourceView_Release(saved_srvs[stage]);
    if (snapshot) ID3D11Texture2D_Release(snapshot);
    destination->drawn = 1;
    destination->gpu_drawn = 1;
    destination->draw_generation = g_pg.surface_generation;
    destination->write_serial = ++g_pg.surface_write_serial;
    /* Only this actual color copy consumes the destination's depth identity. */
    destination->depth_alias_source_offset = 0u;
    destination->depth_alias_source_serial = 0u;
    if (g_pg.pending_scanout == destination)
        g_pg.pending_scanout_write_us = pgraph_scanout_clock_us();
    if (capture) debug_capture_satellite_raw(destination->texture,
                                            "blit-dest-post", capture_index);
    copied = 1;
    reason = "gpu-copy";
 done:
    if (capture || reports++ < 32u) {
        fprintf(stderr, "[PGRAPH-IMAGE-BLIT] draw=%u result=%s op=%u format=%u "
            "context=%08X/%08X source=%08X/%08X dest=%08X/%08X "
            "pitch=%u/%u in=%u,%u out=%u,%u size=%ux%u serial=%llu hr=%08lX\n",
            g_pg.stats.draw_calls + 1u, reason, d->pgraph.image_blit_operation, format,
            d->pgraph.context_surfaces_2d_object, d->pgraph.image_blit_context_surfaces,
            d->pgraph.context_surfaces_2d_source_offset, (uint32_t)source_addr,
            d->pgraph.context_surfaces_2d_dest_offset, (uint32_t)dest_addr,
            d->pgraph.context_surfaces_2d_source_pitch,
            d->pgraph.context_surfaces_2d_dest_pitch, in_x, in_y, out_x, out_y,
            width, height, (unsigned long long)(destination ? destination->write_serial : 0u), hr);
        fflush(stderr);
    }
    return copied;
}

static void debug_capture_satellite_pass_state(uint32_t index)
{
    const char *prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    char path[1024], hlsl[65536];
    NV2ACombinerState combiner_state;
    FILE *state;
    uint32_t stage;
    snprintf(path, sizeof(path), "%s-%02u-pass.state.txt", prefix, index);
    state = fopen(path, "w");
    if (!state) return;
    debug_write_array_capture_state(state);
    fprintf(state, "final_factor=%08X/%08X transient=%d\n",
        g_pg.combiner_final_factor[0], g_pg.combiner_final_factor[1],
        g_debug_satellite_depth_capture_draw == g_pg.stats.draw_calls + 1u &&
        g_debug_satellite_depth_capture_prepared);
    for (stage = 0u; stage < 8u; ++stage)
        fprintf(state, "factor%u=%08X/%08X\n", stage,
                g_pg.combiner_factor0[stage], g_pg.combiner_factor1[stage]);
    d3d8_combiners_from_nv2a_registers(g_pg.combiner_control,
        g_pg.combiner_color_icw, g_pg.combiner_alpha_icw,
        g_pg.combiner_color_ocw, g_pg.combiner_alpha_ocw,
        g_pg.combiner_final_inputs_0, g_pg.combiner_final_inputs_1,
        g_pg.combiner_factor0, g_pg.combiner_factor1,
        g_pg.combiner_final_factor[0], g_pg.combiner_final_factor[1],
        g_pg.shader_stage_program, g_pg.shader_dot_mapping,
        g_pg.shader_other_stage_input, &combiner_state);
    if (d3d8_combiners_generate_hlsl(&combiner_state, hlsl, sizeof(hlsl)) > 0)
        fputs(hlsl, state);
    fclose(state);
}

static int debug_capture_satellite_pass_before(void)
{
    static uint32_t capture_count, capture_epoch, epoch_passes;
    const char *prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    const char *gate = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_GATE_FILE");
    GuestColorSurface *source;
    char path[MAX_PATH];
    uint32_t index;

    if (!prefix || !*prefix || !g_pg.bound_color || capture_count >= 9u ||
        (g_pg.host_vsh_hash != 0xCE41C616u &&
         g_pg.host_vsh_hash != 0x1730DD1Au &&
         g_pg.host_vsh_hash != 0xB0828BA6u) || !debug_satellite_capture_armed())
        return -1;
    if (capture_epoch != g_debug_satellite_capture_epoch) {
        capture_epoch = g_debug_satellite_capture_epoch;
        epoch_passes = 0u;
        g_debug_satellite_post_window_started = 0;
        g_debug_satellite_filter_window = 0;
    }
    if (epoch_passes >= 3u) return -1;
    /* Re-arm only at the beginning of a full resolve/filter sequence. */
    if (!epoch_passes && g_pg.host_vsh_hash != 0xCE41C616u) return -1;
    ++epoch_passes;
    index = capture_count++;
    debug_capture_satellite_pass_state(index);
    snprintf(path, sizeof(path), "%s-%02u-pre-%08X-target-%08X.bmp",
             prefix, index, g_pg.host_vsh_hash,
             g_pg.bound_color->offset);
    d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);

    source = g_pg.tex[0].enabled ?
        find_guest_color_surface(resolved_texture_offset(0u)) : NULL;
    if (source) {
        snprintf(path, sizeof(path), "%s-%02u-pre-%08X-source-%08X.bmp",
                 prefix, index, g_pg.host_vsh_hash, source->offset);
        d3d8_DebugCaptureTextureToPath(source->texture, path);
    }
    fprintf(stderr,
            "[SATELLITE-PASS] index=%u phase=pre draw=%u vsh=%08X "
            "target=%08X/%ux%u logical=%ux%u source=%08X "
            "shader=%08X/%08X dot=%08X combiner=%08X\n",
            index, g_pg.stats.draw_calls + 1u, g_pg.host_vsh_hash,
            g_pg.bound_color->offset, g_pg.bound_color->width,
            g_pg.bound_color->height, g_pg.bound_color->logical_width,
            g_pg.bound_color->logical_height, resolved_texture_offset(0u),
            g_pg.shader_stage_program, g_pg.shader_other_stage_input,
            g_pg.shader_dot_mapping, d3d8_combiners_debug_state_hash());
    fflush(stderr);
    return (int)index;
}

static void debug_capture_satellite_pass_after(int index)
{
    const char *prefix;
    char path[MAX_PATH];

    if (index < 0 || !g_pg.bound_color)
        return;
    prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    if (!prefix || !*prefix)
        return;
    for (uint32_t stage = 0u; stage < 4u; ++stage)
        if (g_pg.tex[stage].enabled)
            debug_capture_satellite_sampled_texture((uint32_t)index, stage);
    debug_capture_satellite_raw(g_pg.bound_color->texture, "pass-post", (uint32_t)index);
    if (g_debug_satellite_depth_capture_draw == g_pg.stats.draw_calls + 1u &&
        g_debug_satellite_depth_capture_prepared)
        debug_capture_satellite_raw(g_pg.satellite_depth_sample_texture,
                                   "transient-post", (uint32_t)index);
    snprintf(path, sizeof(path), "%s-%02d-post-%08X-target-%08X.bmp",
             prefix, index, g_pg.host_vsh_hash,
             g_pg.bound_color->offset);
    d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
    fprintf(stderr,
            "[SATELLITE-PASS] index=%d phase=post draw=%u vsh=%08X "
            "target=%08X\n",
            index, g_pg.stats.draw_calls + 1u, g_pg.host_vsh_hash,
            g_pg.bound_color->offset);
    fflush(stderr);
    if (g_pg.host_vsh_hash == 0x1730DD1Au &&
        !g_debug_satellite_post_window_started) {
        g_debug_satellite_filter_window = 1;
        g_debug_satellite_filter_texture = g_pg.bound_color->offset;
        g_debug_satellite_array_ordinal = (g_debug_satellite_capture_epoch - 1u) * 512u;
        g_debug_satellite_array_capture_count = 0u;
        g_debug_satellite_post_window_started = 1;
    } else if (g_pg.host_vsh_hash == 0xB0828BA6u &&
               g_debug_satellite_filter_window == 1) {
        g_debug_satellite_filter_window = 2;
        g_debug_satellite_array_ordinal = (g_debug_satellite_capture_epoch - 1u) * 512u;
        g_debug_satellite_array_capture_count = 0u;
    }
}

static void debug_capture_satellite_draw_before(const char *kind)
{
    const char *prefix;
    GuestColorSurface *source;
    char path[MAX_PATH];

    g_debug_satellite_final_candidate = 0;
    if (g_debug_satellite_filter_window != 2 || !g_pg.bound_color ||
        !g_pg.tex[0].enabled ||
        resolved_texture_offset(0u) != g_debug_satellite_filter_texture ||
        g_pg.bound_color->offset == g_debug_satellite_filter_texture)
        return;
    prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    if (!prefix || !*prefix)
        return;

    snprintf(path, sizeof(path),
             "%s-final-%03u-%s-pre-target-%08X.bmp", prefix,
             g_debug_satellite_array_ordinal, kind,
             g_pg.bound_color->offset);
    d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
    source = find_guest_color_surface(g_debug_satellite_filter_texture);
    if (source) {
        snprintf(path, sizeof(path),
                 "%s-final-%03u-%s-pre-source-%08X.bmp", prefix,
                 g_debug_satellite_array_ordinal, kind,
                 g_debug_satellite_filter_texture);
        d3d8_DebugCaptureTextureToPath(source->texture, path);
    }
    g_debug_satellite_final_candidate = 1;
}

static void debug_capture_satellite_draw_after(const char *kind,
                                               uint32_t start,
                                               uint32_t count,
                                               uint32_t offset,
                                               uint32_t stride,
                                               const void *vertices,
                                               uint32_t vertex_stride,
                                               uint32_t vertex_count)
{
    const char *prefix;
    uint32_t ordinal;
    int capture;

    if (!g_debug_satellite_filter_window || !g_pg.bound_color)
        return;
    prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_SATELLITE_PASS_PREFIX");
    if (!prefix || !*prefix)
        return;

    if (g_debug_satellite_filter_window == 1)
        return;
    ordinal = g_debug_satellite_array_ordinal++;

    capture = g_debug_satellite_final_candidate &&
              g_debug_satellite_array_capture_count < 8u;
    fprintf(stderr,
            "[SATELLITE-DRAW] ordinal=%u kind=%s draw=%u start=%u count=%u "
            "base=%08X stride=%u mode=%u prim=%u vsh=%08X "
            "target=%08X tex0=%08X/%08X blend=%d/%04X/%04X/%04X "
            "mask=%08X depth=%d/%d/%u alpha=%d/%u/%u "
            "shader=%08X/%08X dot=%08X combiner=%08X "
            "attrs=%08X/%08X/%08X/%08X/%08X capture=%d\n",
            ordinal, kind, g_pg.stats.draw_calls + 1u, start, count,
            offset, stride,
            g_pg.draw_mode, g_pg.d3d_prim_type, g_pg.host_vsh_hash,
            g_pg.bound_color->offset, resolved_texture_offset(0u),
            g_pg.tex[0].format, g_pg.blend_enable, g_pg.blend_sfactor,
            g_pg.blend_dfactor, g_pg.blend_equation, g_pg.color_mask,
            g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
            g_pg.alpha_test, g_pg.alpha_func, g_pg.alpha_ref,
            g_pg.shader_stage_program,
            g_pg.shader_other_stage_input, g_pg.shader_dot_mapping,
            d3d8_combiners_debug_state_hash(), g_pg.vertex_array[0].format,
            g_pg.vertex_array[1].format, g_pg.vertex_array[2].format,
            g_pg.vertex_array[3].format, g_pg.vertex_array[4].format,
            capture);
    if (capture) {
        char path[MAX_PATH];
        FILE *state;
        snprintf(path, sizeof(path),
                 "%s-final-%03u-%s-post-%08X-target-%08X.bmp",
                 prefix, ordinal, kind, g_pg.host_vsh_hash,
                 g_pg.bound_color->offset);
        d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
        snprintf(path, sizeof(path), "%s-final-%03u-%s.state.txt",
                 prefix, ordinal, kind);
        state = fopen(path, "w");
        if (state) {
            NV2ACombinerState combiner_state;
            char hlsl[16384];
            uint32_t vertex;
            fprintf(state, "source=%08X target=%08X blend=%d/%04X/%04X/%04X "
                    "mask=%08X depth=%d/%d/%u alpha=%d/%u/%u\n",
                    resolved_texture_offset(0u), g_pg.bound_color->offset,
                    g_pg.blend_enable, g_pg.blend_sfactor,
                    g_pg.blend_dfactor, g_pg.blend_equation,
                    g_pg.color_mask, g_pg.depth_test, g_pg.depth_write,
                    g_pg.depth_func, g_pg.alpha_test, g_pg.alpha_func,
                    g_pg.alpha_ref);
            debug_write_array_capture_state(state);
            if (vertices && vertex_stride) {
                const uint8_t *bytes = (const uint8_t *)vertices;
                const uint32_t sample_count = vertex_count < 8u ?
                    vertex_count : 8u;
                for (vertex = 0u; vertex < sample_count; ++vertex) {
                    const uint8_t *raw = bytes +
                        (size_t)vertex * vertex_stride;
                    fprintf(state,
                            "vertex%u stride=%u raw=%08X/%08X/%08X/%08X/%08X\n",
                            vertex, vertex_stride,
                            vertex_stride >= 4u ? read_le32(raw + 0u) : 0u,
                            vertex_stride >= 8u ? read_le32(raw + 4u) : 0u,
                            vertex_stride >= 12u ? read_le32(raw + 8u) : 0u,
                            vertex_stride >= 16u ? read_le32(raw + 12u) : 0u,
                            vertex_stride >= 20u ? read_le32(raw + 16u) : 0u);
                }
            }
            d3d8_combiners_from_nv2a_registers(
                g_pg.combiner_control, g_pg.combiner_color_icw,
                g_pg.combiner_alpha_icw, g_pg.combiner_color_ocw,
                g_pg.combiner_alpha_ocw, g_pg.combiner_final_inputs_0,
                g_pg.combiner_final_inputs_1, g_pg.combiner_factor0,
                g_pg.combiner_factor1, g_pg.combiner_final_factor[0],
                g_pg.combiner_final_factor[1], g_pg.shader_stage_program,
                g_pg.shader_dot_mapping, g_pg.shader_other_stage_input,
                &combiner_state);
            if (d3d8_combiners_generate_hlsl(
                    &combiner_state, hlsl, sizeof(hlsl)) > 0) {
                fputs("\n--- generated combiner HLSL ---\n", state);
                fputs(hlsl, state);
            }
            fclose(state);
        }
        ++g_debug_satellite_array_capture_count;
        if (g_debug_satellite_array_capture_count >= 8u)
            g_debug_satellite_filter_window = 0;
    } else if (ordinal % 512u >= 511u) {
        fprintf(stderr,
                "[SATELLITE-DRAW] filter use not found within 512 draws\n");
        g_debug_satellite_filter_window = 0;
    }
    g_debug_satellite_final_candidate = 0;
    fflush(stderr);
}

static void debug_capture_gameplay_draw_before(const char *kind,
                                               uint32_t vertex_count,
                                               uint32_t vertex_base,
                                               const void *guest_vertices,
                                               uint32_t guest_stride,
                                               int combiner_active)
{
    typedef struct VehiclePartDrawTrace {
        uint32_t draw;
        uint32_t clear;
        uint32_t base;
        uint32_t count;
        uint32_t texture;
        uint32_t constants[6][4];
    } VehiclePartDrawTrace;
    static VehiclePartDrawTrace vehicle_part_ring[16];
    static uint32_t vehicle_part_ring_write;
    static uint32_t vehicle_part_ring_count;
    static uint32_t vehicle_body_reports;
    const char *prefix = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_PREFIX");
    const char *trace_env = pgraph_cached_getenv(
        "MERCENARIES_TRACE_GAMEPLAY_DRAWS");
    const char *humvee_trace_env = pgraph_cached_getenv(
        "MERCENARIES_TRACE_HUMVEE_XFORMS");
    const char *humvee_wheel_trace_env = pgraph_cached_getenv(
        "MERCENARIES_TRACE_HUMVEE_WHEEL_XFORMS");
    const char *exact_env;
    const char *exact_base_env;
    const char *stride_env;
    const char *minimum_env;
    const char *limit_env;
    uint32_t exact;
    uint32_t stride;
    uint32_t minimum;
    uint32_t limit;
    uint32_t index;
    int selected;
    int dump_only;
    char path[MAX_PATH];

    g_debug_gameplay_draw_after_pending = 0;
    /* The HQ briefing deliberately disables sunlight and relies on the
     * RedEngine ambient constants. Keep this opt-in observer bounded to the
     * first rendered briefing frame so we can distinguish a bad constant
     * upload from a shader/texture issue without flooding normal runs. In the
     * Xbox constant namespace C_LIGHT_DIR=-95 maps to slot 1 here, followed by
     * ambient in slot 2 and diffuse in slot 3. */
    if (pgraph_cached_getenv("MERCENARIES_TRACE_HQ_LIGHTING") &&
        g_mercenaries_hq_briefing_active != 0u) {
        static uint32_t hq_lighting_samples;
        static uint32_t hq_skinned_samples;
        if (hq_lighting_samples < 768u) {
            fprintf(stderr,
                    "[PGRAPH-HQ-LIGHTING] sample=%u clear=%u draw=%u "
                    "kind=%s prim=%u base=%08X count=%u stride=%u "
                    "vsh=%08X tex=%08X/%08X depth=%d/%d/%u "
                    "blend=%d alpha=%d cull=%d "
                    "material=(%.7g,%.7g,%.7g,%.7g) "
                    "light_dir=(%.7g,%.7g,%.7g,%.7g) "
                    "ambient=(%.7g,%.7g,%.7g,%.7g) "
                    "diffuse=(%.7g,%.7g,%.7g,%.7g)\n",
                    hq_lighting_samples++, g_debug_gameplay_clear_index,
                    g_pg.stats.draw_calls + 1u, kind, g_pg.d3d_prim_type,
                    vertex_base, vertex_count, guest_stride,
                    g_pg.host_vsh_hash, resolved_texture_offset(0u),
                    g_pg.tex[0].format, g_pg.depth_test, g_pg.depth_write,
                    g_pg.depth_func, g_pg.blend_enable, g_pg.alpha_test,
                    g_pg.cull_enable,
                    u2f(g_pg.vsh_constants[0][0]),
                    u2f(g_pg.vsh_constants[0][1]),
                    u2f(g_pg.vsh_constants[0][2]),
                    u2f(g_pg.vsh_constants[0][3]),
                    u2f(g_pg.vsh_constants[1][0]),
                    u2f(g_pg.vsh_constants[1][1]),
                    u2f(g_pg.vsh_constants[1][2]),
                    u2f(g_pg.vsh_constants[1][3]),
                    u2f(g_pg.vsh_constants[2][0]),
                    u2f(g_pg.vsh_constants[2][1]),
                    u2f(g_pg.vsh_constants[2][2]),
                    u2f(g_pg.vsh_constants[2][3]),
                    u2f(g_pg.vsh_constants[3][0]),
                    u2f(g_pg.vsh_constants[3][1]),
                    u2f(g_pg.vsh_constants[3][2]),
                    u2f(g_pg.vsh_constants[3][3]));
        }
        /* xboxSknL2O.xvu (FNV-1a 64CCCAF9) is the exact two-omni-light
         * skinned program used by the briefing actors. Preserve a small,
         * separate sample after the broad first-frame survey: C_LIGHT_POINT0
         * starts at local slot 7 and each authored light occupies color/pos
         * pairs. Logging the packed input declarations with the lights also
         * catches a UB_D3D color decode mismatch without changing rendering. */
        if (g_pg.host_vsh_hash == 0x64CCCAF9u &&
            hq_skinned_samples < 96u) {
            const uint32_t hq_skinned_sample = hq_skinned_samples++;
            fprintf(stderr,
                    "[PGRAPH-HQ-SKINNED] sample=%u draw=%u base=%08X "
                    "count=%u tex=%08X "
                    "light0_color=(%.7g,%.7g,%.7g,%.7g) "
                    "light0_pos=(%.7g,%.7g,%.7g,%.7g) "
                    "light1_color=(%.7g,%.7g,%.7g,%.7g) "
                    "light1_pos=(%.7g,%.7g,%.7g,%.7g) "
                    "a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X\n",
                    hq_skinned_sample, g_pg.stats.draw_calls + 1u,
                    vertex_base, vertex_count, resolved_texture_offset(0u),
                    u2f(g_pg.vsh_constants[7][0]),
                    u2f(g_pg.vsh_constants[7][1]),
                    u2f(g_pg.vsh_constants[7][2]),
                    u2f(g_pg.vsh_constants[7][3]),
                    u2f(g_pg.vsh_constants[8][0]),
                    u2f(g_pg.vsh_constants[8][1]),
                    u2f(g_pg.vsh_constants[8][2]),
                    u2f(g_pg.vsh_constants[8][3]),
                    u2f(g_pg.vsh_constants[9][0]),
                    u2f(g_pg.vsh_constants[9][1]),
                    u2f(g_pg.vsh_constants[9][2]),
                    u2f(g_pg.vsh_constants[9][3]),
                    u2f(g_pg.vsh_constants[10][0]),
                    u2f(g_pg.vsh_constants[10][1]),
                    u2f(g_pg.vsh_constants[10][2]),
                    u2f(g_pg.vsh_constants[10][3]),
                    g_pg.vertex_array[0].format,
                    g_pg.vertex_array[1].format,
                    g_pg.vertex_array[2].format,
                    g_pg.vertex_array[3].format,
                    g_pg.vertex_array[4].format);
            if (hq_skinned_sample == 0u && guest_vertices &&
                guest_stride >= 20u) {
                const uint8_t *vertices = (const uint8_t *)guest_vertices;
                const uint32_t sampled_vertices =
                    vertex_count < 8u ? vertex_count : 8u;
                uint32_t sample_vertex;
                uint32_t constant;
                NV2ACombinerState state;
                const char *combiner_path = pgraph_cached_getenv(
                    "MERCENARIES_DUMP_HQ_COMBINER_PATH");

                for (sample_vertex = 0u;
                     sample_vertex < sampled_vertices;
                     ++sample_vertex) {
                    const uint8_t *raw =
                        vertices + (size_t)sample_vertex * guest_stride;
                    fprintf(stderr,
                            "  [PGRAPH-HQ-VERTEX] v%u=%08X %08X %08X "
                            "%08X %08X\n",
                            sample_vertex, read_le32(raw + 0u),
                            read_le32(raw + 4u), read_le32(raw + 8u),
                            read_le32(raw + 12u), read_le32(raw + 16u));
                }
                for (constant = 0u; constant < 48u; ++constant) {
                    const uint32_t *value = g_pg.vsh_constants[constant];
                    fprintf(stderr,
                            "  [PGRAPH-HQ-CONSTANT] c%u=%08X %08X %08X "
                            "%08X (%.9g,%.9g,%.9g,%.9g)\n",
                            constant, value[0], value[1], value[2], value[3],
                            u2f(value[0]), u2f(value[1]), u2f(value[2]),
                            u2f(value[3]));
                }
                fprintf(stderr,
                        "  [PGRAPH-HQ-COMBINER] control=%08X stages=%08X "
                        "other=%08X dot=%08X final=%08X/%08X "
                        "factor=%08X/%08X\n",
                        g_pg.combiner_control, g_pg.shader_stage_program,
                        g_pg.shader_other_stage_input, g_pg.shader_dot_mapping,
                        g_pg.combiner_final_inputs_0,
                        g_pg.combiner_final_inputs_1,
                        g_pg.combiner_final_factor[0],
                        g_pg.combiner_final_factor[1]);
                for (constant = 0u; constant < 4u; ++constant) {
                    fprintf(stderr,
                            "  [PGRAPH-HQ-COMBINER] s%u rgb=%08X/%08X "
                            "alpha=%08X/%08X c=%08X/%08X\n",
                            constant,
                            g_pg.combiner_color_icw[constant],
                            g_pg.combiner_color_ocw[constant],
                            g_pg.combiner_alpha_icw[constant],
                            g_pg.combiner_alpha_ocw[constant],
                            g_pg.combiner_factor0[constant],
                            g_pg.combiner_factor1[constant]);
                }
                if (combiner_path && combiner_path[0]) {
                    char hlsl[16384];
                    FILE *combiner_file;
                    d3d8_combiners_from_nv2a_registers(
                        g_pg.combiner_control, g_pg.combiner_color_icw,
                        g_pg.combiner_alpha_icw, g_pg.combiner_color_ocw,
                        g_pg.combiner_alpha_ocw,
                        g_pg.combiner_final_inputs_0,
                        g_pg.combiner_final_inputs_1,
                        g_pg.combiner_factor0, g_pg.combiner_factor1,
                        g_pg.combiner_final_factor[0],
                        g_pg.combiner_final_factor[1],
                        g_pg.shader_stage_program,
                        g_pg.shader_dot_mapping,
                        g_pg.shader_other_stage_input, &state);
                    if (d3d8_combiners_generate_hlsl(
                            &state, hlsl, sizeof(hlsl)) > 0) {
                        combiner_file = fopen(combiner_path, "w");
                        if (combiner_file) {
                            fputs(hlsl, combiner_file);
                            fclose(combiner_file);
                            fprintf(stderr,
                                    "  [PGRAPH-HQ-COMBINER] wrote=%s\n",
                                    combiner_path);
                        }
                    }
                }
                fflush(stderr);
            }
        }
    }

    if (humvee_wheel_trace_env &&
        g_mercenaries_gameplay_capture_active != 0u && g_pg.bound_color &&
        g_pg.surface_color_offset == 0x0339C000u &&
        g_pg.host_vsh_hash == 0x0D055DC8u &&
        resolved_texture_offset(0u) == 0x0286F580u) {
        static const uint32_t rows[6] = {4u, 5u, 6u, 23u, 24u, 25u};
        VehiclePartDrawTrace *entry =
            &vehicle_part_ring[vehicle_part_ring_write++ & 15u];
        entry->draw = g_pg.stats.draw_calls + 1u;
        entry->clear = g_debug_gameplay_clear_index;
        entry->base = vertex_base;
        entry->count = vertex_count;
        entry->texture = resolved_texture_offset(0u);
        for (uint32_t row = 0u; row < 6u; ++row) {
            for (uint32_t component = 0u; component < 4u; ++component) {
                entry->constants[row][component] =
                    g_pg.vsh_constants[rows[row]][component];
            }
        }
        if (vehicle_part_ring_count < 16u)
            ++vehicle_part_ring_count;
    }

    if (humvee_wheel_trace_env && vehicle_body_reports < 16u &&
        g_mercenaries_gameplay_capture_active != 0u && g_pg.bound_color &&
        g_pg.surface_color_offset == 0x0339C000u &&
        g_pg.host_vsh_hash == 0xE80E3EFAu &&
        vertex_base == 0x0248A380u && vertex_count == 341u) {
        const uint32_t body_draw = g_pg.stats.draw_calls + 1u;
        fprintf(stderr,
                "[PGRAPH-VEHICLE-BODY] report=%u clear=%u draw=%u "
                "base=%08X count=%u pos=(%.7g,%.7g,%.7g) "
                "preceding_parts=%u\n",
                vehicle_body_reports++, g_debug_gameplay_clear_index,
                body_draw, vertex_base, vertex_count,
                u2f(g_pg.vsh_constants[4][3]),
                u2f(g_pg.vsh_constants[5][3]),
                u2f(g_pg.vsh_constants[6][3]), vehicle_part_ring_count);
        for (uint32_t age = vehicle_part_ring_count; age != 0u; --age) {
            const uint32_t ring_index =
                (vehicle_part_ring_write - age) & 15u;
            const VehiclePartDrawTrace *entry =
                &vehicle_part_ring[ring_index];
            if (entry->draw >= body_draw || body_draw - entry->draw > 32u)
                continue;
            fprintf(stderr,
                    "[PGRAPH-VEHICLE-PART] age=%u clear=%u draw=%u "
                    "delta=%u base=%08X count=%u tex=%08X "
                    "c4=(%.7g,%.7g,%.7g,%.7g) "
                    "c5=(%.7g,%.7g,%.7g,%.7g) "
                    "c6=(%.7g,%.7g,%.7g,%.7g) "
                    "c23=(%.7g,%.7g,%.7g,%.7g) "
                    "c24=(%.7g,%.7g,%.7g,%.7g) "
                    "c25=(%.7g,%.7g,%.7g,%.7g)\n",
                    age, entry->clear, entry->draw,
                    body_draw - entry->draw, entry->base, entry->count,
                    entry->texture,
                    u2f(entry->constants[0][0]),
                    u2f(entry->constants[0][1]),
                    u2f(entry->constants[0][2]),
                    u2f(entry->constants[0][3]),
                    u2f(entry->constants[1][0]),
                    u2f(entry->constants[1][1]),
                    u2f(entry->constants[1][2]),
                    u2f(entry->constants[1][3]),
                    u2f(entry->constants[2][0]),
                    u2f(entry->constants[2][1]),
                    u2f(entry->constants[2][2]),
                    u2f(entry->constants[2][3]),
                    u2f(entry->constants[3][0]),
                    u2f(entry->constants[3][1]),
                    u2f(entry->constants[3][2]),
                    u2f(entry->constants[3][3]),
                    u2f(entry->constants[4][0]),
                    u2f(entry->constants[4][1]),
                    u2f(entry->constants[4][2]),
                    u2f(entry->constants[4][3]),
                    u2f(entry->constants[5][0]),
                    u2f(entry->constants[5][1]),
                    u2f(entry->constants[5][2]),
                    u2f(entry->constants[5][3]));
        }
        fflush(stderr);
    }

    if (humvee_trace_env && g_mercenaries_gameplay_capture_active != 0u &&
        g_debug_humvee_transform_samples < 64u && g_pg.bound_color &&
        g_pg.surface_color_offset == 0x0339C000u &&
        g_pg.host_vsh_hash == 0xE80E3EFAu &&
        (vertex_count == 191u || vertex_count == 263u ||
         vertex_count == 514u || vertex_count == 478u ||
         vertex_count == 273u)) {
        fprintf(stderr,
                "[PGRAPH-HUMVEE-XFORM] sample=%u base=%08X count=%u "
                "c4=(%.7g,%.7g,%.7g,%.7g) "
                "c5=(%.7g,%.7g,%.7g,%.7g) "
                "c6=(%.7g,%.7g,%.7g,%.7g) "
                "c23=(%.7g,%.7g,%.7g,%.7g) "
                "c24=(%.7g,%.7g,%.7g,%.7g) "
                "c25=(%.7g,%.7g,%.7g,%.7g)\n",
                g_debug_humvee_transform_samples++, vertex_base, vertex_count,
                u2f(g_pg.vsh_constants[4][0]), u2f(g_pg.vsh_constants[4][1]),
                u2f(g_pg.vsh_constants[4][2]), u2f(g_pg.vsh_constants[4][3]),
                u2f(g_pg.vsh_constants[5][0]), u2f(g_pg.vsh_constants[5][1]),
                u2f(g_pg.vsh_constants[5][2]), u2f(g_pg.vsh_constants[5][3]),
                u2f(g_pg.vsh_constants[6][0]), u2f(g_pg.vsh_constants[6][1]),
                u2f(g_pg.vsh_constants[6][2]), u2f(g_pg.vsh_constants[6][3]),
                u2f(g_pg.vsh_constants[23][0]), u2f(g_pg.vsh_constants[23][1]),
                u2f(g_pg.vsh_constants[23][2]), u2f(g_pg.vsh_constants[23][3]),
                u2f(g_pg.vsh_constants[24][0]), u2f(g_pg.vsh_constants[24][1]),
                u2f(g_pg.vsh_constants[24][2]), u2f(g_pg.vsh_constants[24][3]),
                u2f(g_pg.vsh_constants[25][0]), u2f(g_pg.vsh_constants[25][1]),
                u2f(g_pg.vsh_constants[25][2]), u2f(g_pg.vsh_constants[25][3]));
    }

    if (humvee_wheel_trace_env &&
        g_mercenaries_gameplay_capture_active != 0u && g_pg.bound_color &&
        g_pg.surface_color_offset == 0x0339C000u &&
        g_pg.host_vsh_hash == 0x0D055DC8u &&
        ((vertex_base == 0x02468F00u && vertex_count == 297u) ||
         (vertex_base == 0x026D2E80u && vertex_count == 303u))) {
        fprintf(stderr,
                "[PGRAPH-HUMVEE-WHEEL-XFORM] base=%08X count=%u "
                "c4=(%.7g,%.7g,%.7g,%.7g) "
                "c5=(%.7g,%.7g,%.7g,%.7g) "
                "c6=(%.7g,%.7g,%.7g,%.7g) "
                "c23=(%.7g,%.7g,%.7g,%.7g) "
                "c24=(%.7g,%.7g,%.7g,%.7g) "
                "c25=(%.7g,%.7g,%.7g,%.7g)\n",
                vertex_base, vertex_count,
                u2f(g_pg.vsh_constants[4][0]), u2f(g_pg.vsh_constants[4][1]),
                u2f(g_pg.vsh_constants[4][2]), u2f(g_pg.vsh_constants[4][3]),
                u2f(g_pg.vsh_constants[5][0]), u2f(g_pg.vsh_constants[5][1]),
                u2f(g_pg.vsh_constants[5][2]), u2f(g_pg.vsh_constants[5][3]),
                u2f(g_pg.vsh_constants[6][0]), u2f(g_pg.vsh_constants[6][1]),
                u2f(g_pg.vsh_constants[6][2]), u2f(g_pg.vsh_constants[6][3]),
                u2f(g_pg.vsh_constants[23][0]), u2f(g_pg.vsh_constants[23][1]),
                u2f(g_pg.vsh_constants[23][2]), u2f(g_pg.vsh_constants[23][3]),
                u2f(g_pg.vsh_constants[24][0]), u2f(g_pg.vsh_constants[24][1]),
                u2f(g_pg.vsh_constants[24][2]), u2f(g_pg.vsh_constants[24][3]),
                u2f(g_pg.vsh_constants[25][0]), u2f(g_pg.vsh_constants[25][1]),
                u2f(g_pg.vsh_constants[25][2]), u2f(g_pg.vsh_constants[25][3]));
    }
    static int shadow_capture_done;
    const int shadow_capture = pgraph_cached_getenv("MERCENARIES_CAPTURE_SHADOW_QUAD") != NULL;
    const int canvas_capture = pgraph_cached_getenv("MERCENARIES_CAPTURE_CANVAS_MASK") != NULL;
    const char *capture_program = pgraph_cached_getenv("MERCENARIES_CAPTURE_SHADER_HASH");
    static unsigned capture_program_seen;
    const int exact_capture = canvas_capture || capture_program != NULL;
    if ((!prefix && !trace_env) ||
        ((shadow_capture || exact_capture) ? (shadow_capture_done || !debug_draw_series_phase_active()) :
                          !g_debug_gameplay_draw_series_active) ||
        !debug_gameplay_capture_target_matches())
        return;
    index = g_debug_gameplay_draw_index++;
    if (trace_env && !shadow_capture) {
        fprintf(stderr,
                "[PGRAPH-GAMEPLAY-DRAW-TRACE] clear=%u index=%u kind=%s "
                "draw=%u prim=%u vertices=%u base=%08X stride=%u vsh=%08X "
                "target=%08X/%ux%u tex=%08X/%08X ctl0=%08X key=%08X "
                "ak=%d comb=%d depth=%d/%d/%u "
                "blend=%d/%04X/%04X/%04X alpha=%d/%u/%u "
                "cull=%d mask=%08X\n",
                g_debug_gameplay_clear_index, index, kind,
                g_pg.stats.draw_calls + 1u, g_pg.d3d_prim_type, vertex_count,
                vertex_base, guest_stride, g_pg.host_vsh_hash,
                g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                g_pg.bound_color ? g_pg.bound_color->logical_width : 0u,
                g_pg.bound_color ? g_pg.bound_color->logical_height : 0u,
                resolved_texture_offset(0u), g_pg.tex[0].format,
                g_pg.tex[0].control0, g_pg.color_key[0],
                (g_pg.tex[0].control0 &
                 NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN) != 0u,
                combiner_active,
                g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
                g_pg.blend_equation, g_pg.alpha_test, g_pg.alpha_func,
                g_pg.alpha_ref, g_pg.cull_enable, g_pg.color_mask);
        if (g_pg.host_vsh_hash == 0xE80E3EFAu &&
            vertex_base >= 0x0246F000u && vertex_base < 0x0248C000u) {
            fprintf(stderr,
                    "[PGRAPH-HUMVEE-XFORM] index=%u base=%08X count=%u "
                    "c4=(%.7g,%.7g,%.7g,%.7g) "
                    "c5=(%.7g,%.7g,%.7g,%.7g) "
                    "c6=(%.7g,%.7g,%.7g,%.7g)\n",
                    index, vertex_base, vertex_count,
                    u2f(g_pg.vsh_constants[4][0]),
                    u2f(g_pg.vsh_constants[4][1]),
                    u2f(g_pg.vsh_constants[4][2]),
                    u2f(g_pg.vsh_constants[4][3]),
                    u2f(g_pg.vsh_constants[5][0]),
                    u2f(g_pg.vsh_constants[5][1]),
                    u2f(g_pg.vsh_constants[5][2]),
                    u2f(g_pg.vsh_constants[5][3]),
                    u2f(g_pg.vsh_constants[6][0]),
                    u2f(g_pg.vsh_constants[6][1]),
                    u2f(g_pg.vsh_constants[6][2]),
                    u2f(g_pg.vsh_constants[6][3]));
        }
    }
    if (!prefix)
        return;
    exact_env = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_EXACT");
    exact_base_env = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_EXACT_BASE");
    stride_env = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_STRIDE");
    minimum_env = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_MIN");
    limit_env = pgraph_cached_getenv(
        "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_LIMIT");
    exact = exact_env ? (uint32_t)strtoul(exact_env, NULL, 10) : 0u;
    stride = stride_env ? (uint32_t)strtoul(stride_env, NULL, 10) : 50u;
    minimum = minimum_env ? (uint32_t)strtoul(minimum_env, NULL, 10) : 0u;
    limit = limit_env ? (uint32_t)strtoul(limit_env, NULL, 10) : 500u;
    if (stride == 0u)
        stride = 1u;
    selected = exact_base_env ?
        vertex_base == (uint32_t)strtoul(exact_base_env, NULL, 0) :
        exact_env ? index == exact :
        (index >= minimum && index <= limit && index % stride == 0u);
    if (shadow_capture) {
        selected = vertex_count == 4u && !g_pg.depth_test &&
            g_pg.stencil_enable && g_pg.blend_enable &&
            g_pg.blend_sfactor == NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO &&
            g_pg.blend_dfactor == NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_ALPHA;
        if (selected) {
            static unsigned shadow_capture_seen;
            const char *ordinal = pgraph_cached_getenv("MERCENARIES_CAPTURE_SHADOW_ORDINAL");
            selected = ++shadow_capture_seen == (ordinal ? strtoul(ordinal, NULL, 10) : 1u);
        }
    }
    if (canvas_capture) {
        selected = g_pg.blend_enable && g_pg.blend_sfactor == 0x0304u &&
            g_pg.blend_dfactor == 0x0305u;
    }
    if (capture_program) {
        static unsigned char capture_counts_seen[1024];
        if (g_pg.host_vsh_hash == (uint32_t)strtoul(capture_program, NULL, 16) &&
            vertex_count < 1024u && !capture_counts_seen[vertex_count]) {
            capture_counts_seen[vertex_count] = 1;
            fprintf(stderr, "[PGRAPH-SHADER-SHAPE] hash=%08X vertices=%u stride=%u prim=%u base=%08X\n",
                g_pg.host_vsh_hash, vertex_count, guest_stride, g_pg.d3d_prim_type, vertex_base);
        }
        selected = g_pg.host_vsh_hash == (uint32_t)strtoul(capture_program, NULL, 16) &&
            (!getenv("MERCENARIES_CAPTURE_SHADER_VERTICES") || vertex_count ==
                (unsigned)strtoul(getenv("MERCENARIES_CAPTURE_SHADER_VERTICES"), NULL, 10)) &&
            ++capture_program_seen == (getenv("MERCENARIES_CAPTURE_SHADER_ORDINAL") ?
                (unsigned)strtoul(getenv("MERCENARIES_CAPTURE_SHADER_ORDINAL"), NULL, 10) : 8u);
    }
    if (!selected)
        return;
    if (shadow_capture || exact_capture) shadow_capture_done = 1;
    fprintf(stderr, "[PGRAPH-CAPTURE-STENCIL] index=%u enable=%d func=%u ref=%u mask=%08X write=%08X ops=%u/%u/%u zcontrol=%08X clip=%g/%g\n",
            index, g_pg.stencil_enable, g_pg.stencil_func, g_pg.stencil_ref,
            g_pg.stencil_mask, g_pg.stencil_write_mask,
            g_pg.stencil_fail, g_pg.stencil_zfail, g_pg.stencil_zpass,
            g_pg.zmin_max_control, u2f(g_pg.clip_min), u2f(g_pg.clip_max));
    dump_only = pgraph_cached_getenv("MERCENARIES_CAPTURE_GAMEPLAY_GEOMETRY_ONLY") != NULL;
    if (dump_only && vertex_count == 4u && !g_pg.depth_test &&
        g_pg.stencil_enable && g_pg.blend_enable &&
        g_pg.blend_sfactor == NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO &&
        g_pg.blend_dfactor == NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_ALPHA) {
        /* Pair this bounded geometry frame with its actual depth/stencil mask.
         * No readback occurs unless the existing opt-in frame is selected. */
        if (g_pg.bound_depth)
            debug_capture_satellite_raw(g_pg.bound_depth->texture, "geometry-shadow-stencil", index);
        debug_capture_satellite_raw(g_pg.bound_color->texture, "geometry-shadow-color", index);
    }
    if (!dump_only) {
    if (shadow_capture) {
        /* One gated capture of the actual stencil mask, before its composite. */
        if (g_pg.bound_depth)
            debug_capture_satellite_raw(g_pg.bound_depth->texture, "shadow-stencil", index);
        debug_capture_satellite_raw(g_pg.bound_color->texture, "shadow-color", index);
    }
    if (exact_capture) {
        snprintf(path, sizeof(path), "%s-%04u-before-target.bmp", prefix, index);
        d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
        debug_capture_satellite_raw(g_pg.bound_color->texture, "canvas-mask", index);
        GuestColorSurface *sample = find_guest_color_surface(resolved_texture_offset(0u));
        if (sample) debug_capture_satellite_raw(sample->texture, "canvas-source", index);
    }
    debug_trace_bound_depth(index);
    if (!d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                      g_pg.bound_color->srv,
                                      g_pg.bound_color->width,
                                      g_pg.bound_color->height))
        return;
    snprintf(path, sizeof(path), "%s-%04u-before.bmp", prefix, index);
    snprintf(g_debug_gameplay_draw_after_path,
             sizeof(g_debug_gameplay_draw_after_path),
             "%s-%04u-after-target.bmp", prefix, index);
    g_debug_gameplay_draw_after_index = index;
    g_debug_gameplay_draw_after_pending = 1;
    fprintf(stderr,
            "[PGRAPH-GAMEPLAY-DRAW-CAPTURE] clear=%u index=%u kind=%s "
            "draw=%u prim=%u vertices=%u base=%08X stride=%u vsh=%08X "
            "tex=%08X/%08X depth=%d/%d/%u blend=%d/%04X/%04X/%04X "
            "alpha=%d/%u/%u cull=%d/%08X/%08X mask=%08X path=%s\n",
            g_debug_gameplay_clear_index, index, kind,
            g_pg.stats.draw_calls + 1u, g_pg.d3d_prim_type, vertex_count,
            vertex_base, guest_stride, g_pg.host_vsh_hash,
            resolved_texture_offset(0u), g_pg.tex[0].format,
            g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
            g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
            g_pg.blend_equation, g_pg.alpha_test, g_pg.alpha_func,
            g_pg.alpha_ref, g_pg.cull_enable, g_pg.cull_face,
            g_pg.front_face, g_pg.color_mask, path);
    }
    {
        char geometry_path[MAX_PATH];
        const char *dump_path = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_DUMP_PATH");
        const char *dump_index_env = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_DUMP_INDEX");
        const char *dump_base_env = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_DUMP_BASE");
        const uint32_t dump_index = exact_capture ? index : dump_index_env ?
            (uint32_t)strtoul(dump_index_env, NULL, 10) : UINT32_MAX;
        if (exact_capture) {
            snprintf(geometry_path, sizeof(geometry_path), "%s-%04u.bin", prefix, index);
            dump_path = geometry_path;
        }
        const uint32_t dump_base = dump_base_env ?
            (uint32_t)strtoul(dump_base_env, NULL, 0) : UINT32_MAX;
        if (dump_only) {
            snprintf(geometry_path, sizeof(geometry_path), "%s-%04u.bin", prefix, index);
            dump_path = geometry_path;
        }
        if (dump_path && guest_vertices && guest_stride != 0u &&
            (dump_only || (index == dump_index || vertex_base == dump_base))) {
            char state_path[MAX_PATH];
            FILE *dump = fopen(dump_path, "wb");
            uint32_t stage;
            if (dump) {
                fwrite(guest_vertices, guest_stride, vertex_count, dump);
                fclose(dump);
            }
            /* The legacy immediate dump contains only XY/UV/color. Keep the
             * complete input registers as well so 3D flare replay retains Z/W.
             * This runs only inside the existing bounded, opt-in draw capture. */
            if (strcmp(kind, "immediate") == 0 &&
                vertex_count == g_pg.immediate_vertex_count &&
                vertex_count <= MAX_INLINE_VERTS) {
                snprintf(state_path, sizeof(state_path), "%s.attributes.bin", dump_path);
                dump = fopen(state_path, "wb");
                if (dump) {
                    fwrite(g_pg.immediate_vertices,
                           sizeof(g_pg.immediate_vertices[0]), vertex_count, dump);
                    fclose(dump);
                }
            }
            snprintf(state_path, sizeof(state_path), "%s.constants.bin", dump_path);
            dump = fopen(state_path, "wb");
            if (dump) {
                fwrite(g_pg.vsh_constants, 1, sizeof(g_pg.vsh_constants), dump);
                fclose(dump);
            }
            snprintf(state_path, sizeof(state_path), "%s.program.bin", dump_path);
            dump = fopen(state_path, "wb");
            if (dump) {
                fwrite(g_pg.transform_program, 1, sizeof(g_pg.transform_program), dump);
                fclose(dump);
            }
            if (g_pg.tex[0].enabled && !dump_only) {
                NV2AState *d = nv2a_get_state();
                uint32_t texture_width = 1u;
                uint32_t texture_height = 1u;
                const uint32_t texture_color =
                    (g_pg.tex[0].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
                const uint32_t texture_offset = resolved_texture_offset(0u);
                uint32_t texture_pitch = g_pg.tex[0].control1 >> 16;
                get_guest_texture_dimensions(g_pg.tex[0].format,
                                             g_pg.tex[0].image_rect,
                                             texture_color,
                                             &texture_width,
                                             &texture_height);
                if (texture_pitch == 0u)
                    texture_pitch = texture_width * 4u;
                snprintf(state_path, sizeof(state_path), "%s.texture0.bin",
                         dump_path);
                dump = fopen(state_path, "wb");
                if (dump && d && d->vram_ptr && d->vram &&
                    texture_offset < memory_region_size(d->vram) &&
                    (size_t)texture_pitch * texture_height <=
                        memory_region_size(d->vram) - texture_offset) {
                    fwrite(d->vram_ptr + texture_offset, texture_pitch,
                           texture_height, dump);
                }
                if (dump)
                    fclose(dump);
            }            snprintf(state_path, sizeof(state_path), "%s.state.txt", dump_path);
            dump = fopen(state_path, "w");
            if (dump) {
                fprintf(dump,
                        "clear=%u index=%u draw=%u kind=%s prim=%u count=%u "
                        "base=%08X stride=%u vsh=%08X mode=%08X program=%u/%u\n",
                        g_debug_gameplay_clear_index, index,
                        g_pg.stats.draw_calls + 1u, kind, g_pg.d3d_prim_type,
                        vertex_count, vertex_base, guest_stride,
                        g_pg.host_vsh_hash, g_pg.transform_execution_mode,
                        g_pg.transform_program_start, g_pg.transform_program_load);
                fprintf(dump,
                        "surface=%08X zeta=%08X depth=%d/%d/%u blend=%d/%04X/%04X "
                        "alpha=%d/%u cull=%08X front=%08X color_mask=%08X\n",
                        g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                        g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                        g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
                        g_pg.alpha_test, g_pg.alpha_func, g_pg.cull_face,
                        g_pg.front_face, g_pg.color_mask);
                fprintf(dump,"polygon_offset point=%u line=%u fill=%u bias=%08X factor=%08X surface_format=%08X\n",
                    g_pg.polygon_offset_point_enable,g_pg.polygon_offset_line_enable,
                    g_pg.polygon_offset_fill_enable,g_pg.polygon_offset_bias,
                    g_pg.polygon_offset_factor,g_pg.surface_format);
                if (exact_capture) {
                    NV2ACombinerState captured_combiner;
                    char captured_hlsl[65536];
                    debug_write_array_capture_state(dump);
                    fprintf(dump, "final_factor=%08X/%08X\n", g_pg.combiner_final_factor[0], g_pg.combiner_final_factor[1]);
                    for (stage = 0u; stage < 8u; ++stage)
                        fprintf(dump, "factor%u=%08X/%08X\n", stage, g_pg.combiner_factor0[stage], g_pg.combiner_factor1[stage]);
                    d3d8_combiners_from_nv2a_registers(g_pg.combiner_control,
                        g_pg.combiner_color_icw, g_pg.combiner_alpha_icw,
                        g_pg.combiner_color_ocw, g_pg.combiner_alpha_ocw,
                        g_pg.combiner_final_inputs_0, g_pg.combiner_final_inputs_1,
                        g_pg.combiner_factor0, g_pg.combiner_factor1,
                        g_pg.combiner_final_factor[0], g_pg.combiner_final_factor[1],
                        g_pg.shader_stage_program, g_pg.shader_dot_mapping,
                        g_pg.shader_other_stage_input, &captured_combiner);
                    if (d3d8_combiners_generate_hlsl(&captured_combiner, captured_hlsl, sizeof(captured_hlsl)) > 0)
                        fputs(captured_hlsl, dump);
                }
                for (stage = 0u; stage < 8u; ++stage) {
                    const uint32_t history =
                        (g_debug_depth_method_serial + stage) & 7u;
                    fprintf(dump, "depth_history%u draw=%u value=%08X\n",
                            stage, g_debug_depth_method_draw[history],
                            g_debug_depth_method_value[history]);
                }
                for (stage = 0u; stage < 4u; ++stage) {
                    fprintf(dump,
                            "tex%u enabled=%d offset=%08X resolved=%08X format=%08X "
                            "control0=%08X control1=%08X rect=%08X address=%08X "
                            "filter=%08X\n",
                            stage, g_pg.tex[stage].enabled, g_pg.tex[stage].offset,
                            resolved_texture_offset(stage), g_pg.tex[stage].format,
                            g_pg.tex[stage].control0, g_pg.tex[stage].control1,
                            g_pg.tex[stage].image_rect, g_pg.tex[stage].address,
                            g_pg.tex[stage].filter);
                }
                for (stage = 0u; stage < NV2A_VS_MAX_INPUTS; ++stage) {
                    fprintf(dump, "array%u format=%08X offset=%08X resolved=%08X\n",
                            stage, g_pg.vertex_array[stage].format,
                            g_pg.vertex_array[stage].offset,
                            resolved_vertex_array_offset(stage));
                }
                for (stage = 0u; stage < 48u; ++stage) {
                    fprintf(dump,
                            "constant%u=%08X,%08X,%08X,%08X\n", stage,
                            g_pg.vsh_constants[stage][0],
                            g_pg.vsh_constants[stage][1],
                            g_pg.vsh_constants[stage][2],
                            g_pg.vsh_constants[stage][3]);
                }
                {
                    const uint32_t serial =
                        g_pg.transform_constant_history_serial;
                    const uint32_t first = serial > 63u ? serial - 63u : 1u;
                    uint32_t history_serial;
                    for (history_serial = first;
                         history_serial <= serial; ++history_serial) {
                        const uint32_t history_index = history_serial & 63u;
                        const uint32_t recorded =
                            g_pg.transform_constant_history[history_index].serial;
                        if (recorded != history_serial)
                            continue;
                        fprintf(dump,
                                "constant_history serial=%u draw=%u kind=%s "
                                "method=%04X param=%08X load_before=%u "
                                "constant=%u component=%u\n",
                                recorded,
                                g_pg.transform_constant_history[history_index].draw,
                                g_pg.transform_constant_history[history_index].is_load ?
                                    "load" : "write",
                                g_pg.transform_constant_history[history_index].method,
                                g_pg.transform_constant_history[history_index].parameter,
                                g_pg.transform_constant_history[history_index].load_before,
                                g_pg.transform_constant_history[history_index].constant,
                                g_pg.transform_constant_history[history_index].component);
                    }
                }                fclose(dump);
            }
            fprintf(stderr,
                    "[PGRAPH-GAMEPLAY-DRAW-DUMP] index=%u path=%s count=%u "
                    "stride=%u\n",
                    index, dump_path, vertex_count, guest_stride);
        }
    }
    if (!dump_only) d3d8_DebugCaptureFrameToPath(path);
}

static void debug_capture_gameplay_draw_after(const char *kind)
{
    if (!g_debug_gameplay_draw_after_pending)
        return;
    g_debug_gameplay_draw_after_pending = 0;
    if (!g_pg.bound_color || !g_pg.bound_color->texture)
        return;
    d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture,
                                   g_debug_gameplay_draw_after_path);
    fprintf(stderr,
            "[PGRAPH-GAMEPLAY-DRAW-AFTER] index=%u kind=%s draw=%u "
            "target=%08X path=%s\n",
            g_debug_gameplay_draw_after_index, kind ? kind : "unknown",
            g_pg.stats.draw_calls + 1u, g_pg.bound_color->offset,
            g_debug_gameplay_draw_after_path);
}

static OutputVertex decode_array_fallback_vertex(const uint8_t *src,
    uint32_t stride, int linear_uv, uint32_t width, uint32_t height)
{
    OutputVertex value;
    value.x = stride >= 4u ? u2f(read_le32(src)) : 0.0f;
    value.y = stride >= 8u ? u2f(read_le32(src + 4u)) : 0.0f;
    value.z = stride >= 12u ? u2f(read_le32(src + 8u)) : 0.0f;
    value.rhw = stride >= 16u ? u2f(read_le32(src + 12u)) : 1.0f;
    value.color = 0xFFFFFFFFu;
    value.u = stride >= 20u ? u2f(read_le32(src + 16u)) : 0.0f;
    value.v = stride >= 24u ? u2f(read_le32(src + 20u)) : 0.0f;
    if (linear_uv && width && height) {
        value.u /= (float)width;
        value.v /= (float)height;
    }
    return value;
}

static OutputVertex *prepare_array_fallback_vertices(DWORD shader,
    const uint8_t *vertices, uint32_t stride, uint32_t count,
    int linear_uv, uint32_t width, uint32_t height)
{
    OutputVertex *output;
    uint32_t i;
    /* Programmable draws consume the original packed attributes directly.
     * Do not allocate/decode a second, unused XYZRHW array for every draw. */
    if (shader || !count || count > SIZE_MAX / sizeof(*output))
        return NULL;
    output = (OutputVertex *)malloc((size_t)count * sizeof(*output));
    if (!output)
        return NULL;
    for (i = 0u; i < count; ++i)
        output[i] = decode_array_fallback_vertex(
            vertices + (size_t)i * stride, stride, linear_uv, width, height);
    return output;
}

static void debug_write_array_capture_state(FILE *output)
{
    uint32_t i;
    if (!output)
        return;
    fprintf(output, "draw=%u vsh=%08X mode=%u combiner=%08X stage_program=%08X "
        "other_stage=%08X final=%08X/%08X\n", g_pg.stats.draw_calls + 1u,
        g_pg.host_vsh_hash, g_pg.draw_mode, g_pg.combiner_control,
        g_pg.shader_stage_program, g_pg.shader_other_stage_input,
        g_pg.combiner_final_inputs_0, g_pg.combiner_final_inputs_1);
    for (i = 0u; i < 16u; ++i)
        fprintf(output, "[PGRAPH-CAPTURE-ARRAY-ATTR] slot=%u "
            "format=%08X offset=%08X inline=%u\n", i,
            g_pg.vertex_array[i].format, resolved_vertex_array_offset(i),
            g_pg.inline_array_offsets[i]);
    for (i = 0u; i < 4u; ++i)
        fprintf(output, "[PGRAPH-CAPTURE-ARRAY-TEX] stage=%u "
            "enabled=%d offset=%08X format=%08X rect=%08X "
            "control0=%08X control1=%08X address=%08X filter=%08X\n", i,
            g_pg.tex[i].enabled, resolved_texture_offset(i),
            g_pg.tex[i].format, g_pg.tex[i].image_rect, g_pg.tex[i].control0,
            g_pg.tex[i].control1, g_pg.tex[i].address, g_pg.tex[i].filter);
    for (i = 0u; i < 8u; ++i)
        fprintf(output, "combiner%u color=%08X/%08X alpha=%08X/%08X\n", i,
            g_pg.combiner_color_icw[i], g_pg.combiner_color_ocw[i],
            g_pg.combiner_alpha_icw[i], g_pg.combiner_alpha_ocw[i]);
}

static int debug_array_capture_gate_open(void)
{
    static int initialized;
    static const char *path;
    DWORD attributes;
    if (!initialized) {
        initialized = 1;
        path = pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_GATE_FILE");
    }
    if (!path || !path[0])
        return 1;
    attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static int debug_gameplay_capture_target_matches(void)
{
    if (!g_pg.bound_color)
        return 0;
    if (pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_ALL_TARGETS") != NULL)
        return 1;
    if (pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_ANY_TARGET") != NULL)
        return g_pg.bound_color->logical_width == 640u &&
               g_pg.bound_color->logical_height == 480u;
    return g_pg.surface_color_offset == 0x0339C000u;
}

/* A line segment is complete with two vertices. RedCanvas emits the GPS
 * crosshair as separate two-vertex LINESTRIPs, so a triangle-only minimum
 * silently removes the reticle in both array and immediate submission. */
static int draw_has_enough_vertices(uint32_t count, int primitive_type)
{
    const uint32_t minimum =
        (primitive_type == D3DPT_LINELIST || primitive_type == D3DPT_LINESTRIP)
            ? 2u : 3u;
    return count >= minimum;
}

static void submit_array_draw(void)
{
    static uint32_t captured_rain_batches;
    static uint32_t traced_rain_batches;
    static uint32_t colored_overlay_capture_count;
    static int post_shadow_depth_restore_window;
    static uint32_t post_shadow_depth_surface;
    NV2AState *d = nv2a_get_state();
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    const uint32_t count = g_pg.inline_element_count ?
        g_pg.inline_element_count : g_pg.draw_array_count;
    const uint32_t start = g_pg.inline_element_count ? 0u :
        g_pg.draw_array_start;
    uint32_t stride = g_pg.stream[0].stride;
    uint32_t offset = g_pg.stream[0].physical_offset;
    uint64_t first, end;
    uint32_t prim_count;
    uint32_t i;
    OutputVertex *out = NULL;
    IDirect3DTexture8 *tex;
    IDirect3DTexture8 *stage_tex[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *stage_surface_srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11Texture2D *feedback_textures[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *feedback_srvs[4] = { NULL, NULL, NULL, NULL };
    NV2ACombinerState combiner_state;
    uint32_t tex_width;
    uint32_t tex_height;
    DWORD draw_shader;
    const void *draw_data;
    uint32_t draw_stride;
    uint8_t *expanded_quads = NULL;
    uint8_t *expanded_line_loop = NULL;
    uint32_t submitted_vertex_count = count;
    const uint32_t color_format =
        (g_pg.tex[0].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    const int linear_uv = texture_color_format_is_linear(color_format);
    const uint8_t *vertex_bytes;
    uint8_t *expanded_elements = NULL;
    int host_depth_test;
    GuestDepthSurface *depth_alias_source;
    int colored_overlay_capture_pending = 0;
    /* Snapshot once: the before/after pair must describe the same draw even
     * if the diagnostic gate file appears while the draw is executing. */
    const int array_capture_gate_open = debug_array_capture_gate_open();

    /* NV097 vertex arrays are authoritative for programmable draws. The
     * retail D3D driver may update its CPU-side stream cache for other vertex
     * buffers after emitting SET_VERTEX_DATA_ARRAY_*, so stream[0] is not
     * guaranteed to describe the array that DRAW_ARRAYS consumes. Match the
     * NV2A/Xemu model by taking the base and stride from position array 0.
     * Other active interleaved attributes are addressed relative to it in
     * prepare_transform_program(). */
    if ((g_pg.transform_execution_mode & 3u) == 2u &&
        vertex_array_dxgi_format(g_pg.vertex_array[0].format) !=
            DXGI_FORMAT_UNKNOWN) {
        const uint32_t array_stride =
            (g_pg.vertex_array[0].format &
             NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE) >> 8;
        if (array_stride != 0u) {
            offset = resolved_vertex_array_offset(0u);
            stride = array_stride;
        }
    }
    if (g_pg.inline_array_mode) {
        stride = g_pg.inline_array_vertex_size;
        offset = 0u;
    }

    get_guest_texture_dimensions(g_pg.tex[0].format,
                                 g_pg.tex[0].image_rect, color_format,
                                 &tex_width, &tex_height);

    if (!dev || !d || !d->vram_ptr || stride == 0u ||
        !draw_has_enough_vertices(count, g_pg.d3d_prim_type))
        return;
    if (g_pg.inline_array_mode) {
        first = 0u;
        end = (uint64_t)count * stride;
        if (end > (uint64_t)g_pg.inline_count * sizeof(uint32_t))
            return;
        vertex_bytes = (const uint8_t *)g_pg.inline_data;
    } else if (g_pg.inline_element_count) {
        uint32_t max_index = 0u;
        uint32_t element;
        for (element = 0u; element < count; ++element) {
            if (g_pg.inline_elements[element] > max_index)
                max_index = g_pg.inline_elements[element];
        }
        first = offset;
        end = (uint64_t)offset + ((uint64_t)max_index + 1u) * stride;
        if (end > (uint64_t)memory_region_size(d->vram))
            return;
        expanded_elements = (uint8_t *)malloc((size_t)count * stride);
        if (!expanded_elements)
            return;
        for (element = 0u; element < count; ++element) {
            memcpy(expanded_elements + (size_t)element * stride,
                   d->vram_ptr + offset +
                       (size_t)g_pg.inline_elements[element] * stride,
                   stride);
        }
        vertex_bytes = expanded_elements;
    } else {
        first = (uint64_t)offset + (uint64_t)start * stride;
        end = first + (uint64_t)count * stride;
        if (end > (uint64_t)memory_region_size(d->vram))
            return;
        vertex_bytes = d->vram_ptr + (size_t)first;
    }

    /* The authored C-17 CloudFlight/RushingAir effects are RedCanvas Brush3D
     * primitives: 24-byte vertices and ten independent four-vertex diamonds
     * joined with two degenerate
     * vertices by ResetTriStrip.  That makes one 58-vertex strip, not a set of
     * four-vertex draws. Texture enable is deliberately not part of the
     * predicate because SetTexture(NULL) can leave the NV2A texture-enable
     * register cached while the combiner selects TFACTOR. */
    if (pgraph_cached_getenv("MERCENARIES_TRACE_CLOUD_FLIGHT") != NULL &&
        array_capture_gate_open && g_pg.d3d_prim_type == D3DPT_TRIANGLESTRIP &&
        count == 58u && stride == 24u &&
        read_le32(vertex_bytes + 12u) == 0x00808080u &&
        read_le32(vertex_bytes + stride + 12u) == 0x80808080u &&
        read_le32(vertex_bytes + stride * 2u + 12u) == 0x80808080u &&
        read_le32(vertex_bytes + stride * 3u + 12u) == 0x00808080u &&
        memcmp(vertex_bytes + stride * 3u,
               vertex_bytes + stride * 4u, 16u) == 0 &&
        memcmp(vertex_bytes + stride * 5u,
               vertex_bytes + stride * 6u, 16u) == 0) {
        static uint32_t traced_cloud_arrays;
        if (traced_cloud_arrays < 96u) {
            const uint32_t sample = ++traced_cloud_arrays;
            fprintf(stderr,
                    "[PGRAPH-CLOUD-ARRAY] sample=%u draw=%u start=%u "
                    "base=%08X stride=%u inline=%u/%u target=%08X zeta=%08X "
                    "depth=%d/%d/%u blend=%d/%04X/%04X/%04X "
                    "vsh=%08X/%08X/%u/%u comb=%08X final=%08X/%08X "
                    "factor=%08X/%08X tex0=%d/%08X/%08X/%08X "
                    "filter=%08X address=%08X texprog=%08X\n",
                    sample, g_pg.stats.draw_calls + 1u, start, offset, stride,
                    g_pg.inline_array_mode, g_pg.inline_element_count,
                    g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                    g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                    g_pg.blend_enable, g_pg.blend_sfactor,
                    g_pg.blend_dfactor, g_pg.blend_equation,
                    g_pg.transform_execution_mode, g_pg.host_vsh_hash,
                    g_pg.transform_program_start, g_pg.transform_program_load,
                    g_pg.combiner_control, g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1, g_pg.combiner_factor0[0],
                    g_pg.combiner_factor1[0], g_pg.tex[0].enabled,
                    resolved_texture_offset(0u), g_pg.tex[0].format,
                    g_pg.tex[0].control0, g_pg.tex[0].filter,
                    g_pg.tex[0].address, g_pg.shader_stage_program);
            fprintf(stderr,
                    "  cloud-comb0 rgb=%08X/%08X alpha=%08X/%08X "
                    "other=%08X\n",
                    g_pg.combiner_color_icw[0],
                    g_pg.combiner_color_ocw[0],
                    g_pg.combiner_alpha_icw[0],
                    g_pg.combiner_alpha_ocw[0],
                    g_pg.shader_other_stage_input);
            for (i = 0u; i < count; ++i) {
                const uint8_t *raw = vertex_bytes + (size_t)i * stride;
                fprintf(stderr,
                        "  cloud-v%u=%08X %08X %08X %08X %08X %08X\n",
                        i, stride >= 4u ? read_le32(raw + 0u) : 0u,
                        stride >= 8u ? read_le32(raw + 4u) : 0u,
                        stride >= 12u ? read_le32(raw + 8u) : 0u,
                        stride >= 16u ? read_le32(raw + 12u) : 0u,
                        stride >= 20u ? read_le32(raw + 16u) : 0u,
                        stride >= 24u ? read_le32(raw + 20u) : 0u);
            }
            fflush(stderr);
        }
    }

    {
        const char *dump_path =
            pgraph_cached_getenv("MERCENARIES_DUMP_ARRAY_PATH");
        const char *dump_base =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_BASE");
        static int dumped_array;
        if (!dumped_array && dump_path && dump_base &&
            offset == ((uint32_t)strtoul(dump_base, NULL, 16) & 0x07FFFFFFu)) {
            FILE *dump = fopen(dump_path, "wb");
            if (dump) {
                fwrite(vertex_bytes, stride, count, dump);
                fclose(dump);
                fprintf(stderr,
                        "[PGRAPH-DUMP-ARRAY] path=%s base=%08X stride=%u count=%u\n",
                        dump_path, offset, stride, count);
                fflush(stderr);
            }
            dumped_array = 1;
        }
    }

    if (g_pg.draw_mode == 8u) {
        prim_count = count / 4u * 2u;
    } else if (g_pg.draw_mode == 3u) {
        /* NV2A LINE_LOOP closes the final vertex back to the first. D3D11
         * has no line-loop topology, so submit one duplicate vertex through
         * LINESTRIP. */
        prim_count = count;
    } else switch (g_pg.d3d_prim_type) {
    case D3DPT_TRIANGLELIST:  prim_count = count / 3u; break;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   prim_count = count - 2u; break;
    case D3DPT_LINELIST:      prim_count = count / 2u; break;
    case D3DPT_LINESTRIP:     prim_count = count - 1u; break;
    default:                  return;
    }
    if (prim_count == 0u) {
        free(expanded_elements);
        return;
    }

    /* RsRotorWashBrush records one 100-vertex triangle strip in the shared
     * Brush3D buffer.  Keep this observer exact and opt-in: broad array traces
     * materially perturb the aircraft sequence and can disguise the rendering
     * fault we are trying to measure. */
    if (pgraph_cached_getenv("MERCENARIES_TRACE_ROTOR_WASH") != NULL &&
        g_pg.d3d_prim_type == D3DPT_TRIANGLESTRIP &&
        count == 100u && stride == 24u) {
        static uint32_t traced_rotor_wash;
        if (traced_rotor_wash < 32u) {
            fprintf(stderr,
                    "[PGRAPH-ROTOR-WASH] sample=%u draw=%u start=%u "
                    "base=%08X target=%08X zeta=%08X "
                    "depth=%d/%d/%u blend=%d/%04X/%04X/%04X "
                    "cull=%d/%08X/%08X tex=%08X/%08X/%08X "
                    "vsh=%08X/%u/%u comb=%08X final=%08X/%08X\n",
                    ++traced_rotor_wash, g_pg.stats.draw_calls + 1u, start,
                    offset, g_pg.surface_color_offset,
                    g_pg.surface_zeta_offset, g_pg.depth_test,
                    g_pg.depth_write, g_pg.depth_func, g_pg.blend_enable,
                    g_pg.blend_sfactor, g_pg.blend_dfactor,
                    g_pg.blend_equation, g_pg.cull_enable,
                    g_pg.cull_face, g_pg.front_face,
                    resolved_texture_offset(0u), g_pg.tex[0].format,
                    g_pg.tex[0].address, g_pg.transform_execution_mode,
                    g_pg.transform_program_start,
                    g_pg.transform_program_load, g_pg.combiner_control,
                    g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1);
            fprintf(stderr,
                    "  rotor-comb0 rgb=%08X/%08X alpha=%08X/%08X "
                    "texctl=%08X filter=%08X border=%08X\n",
                    g_pg.combiner_color_icw[0],
                    g_pg.combiner_color_ocw[0],
                    g_pg.combiner_alpha_icw[0],
                    g_pg.combiner_alpha_ocw[0],
                    g_pg.tex[0].control0, g_pg.tex[0].filter,
                    g_pg.tex[0].border_color);
            for (i = 0u; i < NV2A_VS_MAX_INPUTS; ++i) {
                if (g_pg.vertex_array[i].format ||
                    g_pg.vertex_array[i].offset) {
                    fprintf(stderr,
                            "  rotor-array%u off=%08X fmt=%08X rel=%d\n",
                            i, g_pg.vertex_array[i].offset,
                            g_pg.vertex_array[i].format,
                            g_pg.vertex_array[i].offset >= offset &&
                            g_pg.vertex_array[i].offset - offset < stride ?
                                (int)(g_pg.vertex_array[i].offset - offset) :
                                -1);
                }
            }
            for (i = 0u; i < 6u; ++i) {
                const uint8_t *raw = vertex_bytes + (size_t)i * stride;
                fprintf(stderr,
                        "  rotor-v%u raw=%08X %08X %08X %08X %08X %08X "
                        "pos=(%.6g,%.6g,%.6g) uv=(%.6g,%.6g)\n",
                        i, read_le32(raw + 0u), read_le32(raw + 4u),
                        read_le32(raw + 8u), read_le32(raw + 12u),
                        read_le32(raw + 16u), read_le32(raw + 20u),
                        u2f(read_le32(raw + 0u)),
                        u2f(read_le32(raw + 4u)),
                        u2f(read_le32(raw + 8u)),
                        u2f(read_le32(raw + 16u)),
                        u2f(read_le32(raw + 20u)));
            }
            fflush(stderr);
        }
    }

    if (pgraph_trace_draws_enabled()) {
        fprintf(stderr,
                "[PGRAPH-D3D11] DRAW_ARRAYS start=%u count=%u stream=%08X "
                "stride=%u fvf=%08X\n",
                start, count, offset, stride, g_pg.guest_fvf);
    }
    {
        const char *window_env = pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_WINDOW");
        const char *surface_window_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_SURFACE_WINDOW");
        const char *minimum_env = pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_DRAW_MIN");
        const char *target_env = pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_TARGET");
        const uint32_t minimum_draw = minimum_env ?
            (uint32_t)strtoul(minimum_env, NULL, 10) : 0u;
        const uint32_t window = window_env ?
            (uint32_t)strtoul(window_env, NULL, 10) : 0u;
        const uint32_t surface_window = surface_window_env ?
            (uint32_t)strtoul(surface_window_env, NULL, 10) : 0u;
        const uint32_t surface_draw = g_pg.bound_color &&
            g_pg.stats.draw_calls >= g_pg.bound_color->created_draw ?
                g_pg.stats.draw_calls - g_pg.bound_color->created_draw :
                UINT32_MAX;
        const int absolute_match = window &&
            g_pg.stats.draw_calls >= minimum_draw &&
            g_pg.stats.draw_calls < minimum_draw + window;
        const int surface_match = surface_window && g_pg.bound_color &&
            g_pg.bound_color->created_draw >= minimum_draw &&
            g_pg.bound_color->anti_aliasing !=
                NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_1 &&
            surface_draw < surface_window;
        if ((absolute_match || surface_match) &&
            (!target_env || g_pg.surface_color_offset ==
                ((uint32_t)strtoul(target_env, NULL, 16) & 0x07FFFFFFu))) {
            fprintf(stderr,
                    "[PGRAPH-ARRAY-WINDOW] draw=%u relative=%u mode=%u prim=%d "
                    "start=%u count=%u target=%08X zeta=%08X "
                    "depth=%d/%d/%u stencil=%d/%u/%02X/%02X/%u/%u/%u "
                    "blend=%d/%04X/%04X/%04X cull=%d/%08X/%08X "
                    "mask=%08X base=%08X stride=%u aa=%u vsh=%08X/%u/%u "
                    "a0=%08X/%08X a3=%08X/%08X a4=%08X/%08X "
                    "comb=%08X tex=%08X/%08X addr=%08X final=%08X/%08X\n",
                    g_pg.stats.draw_calls, surface_draw, g_pg.draw_mode,
                    g_pg.d3d_prim_type, start, count,
                    g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                    g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                    g_pg.stencil_enable, g_pg.stencil_func,
                    g_pg.stencil_ref, g_pg.stencil_mask,
                    g_pg.stencil_fail, g_pg.stencil_zfail,
                    g_pg.stencil_zpass,
                    g_pg.blend_enable, g_pg.blend_sfactor,
                    g_pg.blend_dfactor, g_pg.blend_equation,
                    g_pg.cull_enable, g_pg.cull_face,
                    g_pg.front_face, g_pg.color_mask, offset, stride,
                    g_pg.bound_color ? g_pg.bound_color->anti_aliasing : 0u,
                    g_pg.transform_execution_mode,
                    g_pg.transform_program_start,
                    g_pg.transform_program_load,
                    g_pg.vertex_array[0].offset,
                    g_pg.vertex_array[0].format,
                    g_pg.vertex_array[3].offset,
                    g_pg.vertex_array[3].format,
                    g_pg.vertex_array[4].offset,
                    g_pg.vertex_array[4].format,
                    g_pg.combiner_control,
                    g_pg.shader_stage_program, g_pg.tex[0].offset,
                    g_pg.tex[0].address,
                    g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1);
        }
    }
    const char *trace_array_vertices_env =
        pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_VERTICES");
    const uint32_t trace_array_vertices_min = trace_array_vertices_env ?
        (uint32_t)strtoul(trace_array_vertices_env, NULL, 10) : 0u;
    if (trace_array_vertices_env &&
        count >= (trace_array_vertices_min > 1u ?
                     trace_array_vertices_min : 100u)) {
        static uint32_t logged_large_arrays;
        const char *minimum_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_DRAW_MIN");
        const char *surface_window_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_SURFACE_WINDOW");
        const char *target_env = pgraph_cached_getenv("MERCENARIES_TRACE_ARRAY_TARGET");
        const uint32_t minimum_draw = minimum_env ?
            (uint32_t)strtoul(minimum_env, NULL, 10) : 0u;
        const uint32_t surface_window = surface_window_env ?
            (uint32_t)strtoul(surface_window_env, NULL, 10) : 0u;
        const uint32_t relative_draw = g_pg.bound_color &&
            g_pg.stats.draw_calls >= g_pg.bound_color->created_draw ?
                g_pg.stats.draw_calls - g_pg.bound_color->created_draw :
                UINT32_MAX;
        const int absolute_match = !surface_window_env &&
            g_pg.stats.draw_calls >= minimum_draw;
        const int surface_match = surface_window && g_pg.bound_color &&
            g_pg.bound_color->created_draw >= minimum_draw &&
            g_pg.bound_color->anti_aliasing !=
                NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_1 &&
            relative_draw < surface_window;
        if ((absolute_match || surface_match) &&
            (!target_env || g_pg.surface_color_offset ==
                ((uint32_t)strtoul(target_env, NULL, 16) & 0x07FFFFFFu)) &&
            logged_large_arrays < 16u) {
            fprintf(stderr,
                    "  [large] draw=%u mode=%u prim=%d start=%u count=%u "
                    "target=%08X base=%08X depth=%d/%d/%u\n",
                    g_pg.stats.draw_calls, g_pg.draw_mode,
                    g_pg.d3d_prim_type,
                    start, count, g_pg.surface_color_offset, offset,
                    g_pg.depth_test, g_pg.depth_write, g_pg.depth_func);
            for (i = 0; i < count && i < 8u; ++i) {
                const uint8_t *src = vertex_bytes + (size_t)i * stride;
                const OutputVertex diagnostic = decode_array_fallback_vertex(
                    src, stride, linear_uv, tex_width, tex_height);
                fprintf(stderr,
                        "  [large %u] raw=%08X %08X %08X %08X %08X %08X "
                        "pos=(%.3f,%.3f,%.3f,%.3f) uv=(%.3f,%.3f)\n",
                        i, stride >= 4u ? read_le32(src) : 0u,
                        stride >= 8u ? read_le32(src + 4u) : 0u,
                        stride >= 12u ? read_le32(src + 8u) : 0u,
                        stride >= 16u ? read_le32(src + 12u) : 0u,
                        stride >= 20u ? read_le32(src + 16u) : 0u,
                        stride >= 24u ? read_le32(src + 20u) : 0u,
                        diagnostic.x, diagnostic.y, diagnostic.z, diagnostic.rhw,
                        diagnostic.u, diagnostic.v);
            }
            fprintf(stderr,
                    "  [large] vsh mode=%08X start=%u load=%u\n",
                    g_pg.transform_execution_mode,
                    g_pg.transform_program_start,
                    g_pg.transform_program_load);
            for (i = 0; i < 16u; ++i) {
                if (g_pg.vertex_array[i].format ||
                    g_pg.vertex_array[i].offset) {
                    fprintf(stderr,
                            "  [large] array%u off=%08X fmt=%08X\n", i,
                            g_pg.vertex_array[i].offset,
                            g_pg.vertex_array[i].format);
                }
            }
            for (i = g_pg.transform_program_start;
                 i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH &&
                 i < g_pg.transform_program_start + 32u; ++i) {
                fprintf(stderr,
                        "  [large] vsh%u=%08X %08X %08X %08X\n", i,
                        g_pg.transform_program[i][0],
                        g_pg.transform_program[i][1],
                        g_pg.transform_program[i][2],
                        g_pg.transform_program[i][3]);
                if (g_pg.transform_program[i][3] & 1u)
                    break;
            }
            for (i = 0; i < 48u; ++i) {
                const uint32_t *c = g_pg.vsh_constants[i];
                if (c[0] || c[1] || c[2] || c[3]) {
                    fprintf(stderr,
                            "  [large] c%u=%08X %08X %08X %08X "
                            "(%.9g,%.9g,%.9g,%.9g)\n",
                            i, c[0], c[1], c[2], c[3],
                            u2f(c[0]), u2f(c[1]), u2f(c[2]), u2f(c[3]));
                }
            }
            ++logged_large_arrays;
        }
    }
    if (count == 528u)
        trace_sky_vertex_candidates(d);
    if (count == 528u &&
        pgraph_cached_getenv("MERCENARIES_TRACE_COMBINER") != NULL) {
        static int logged_combiner;
        if (!logged_combiner) {
            uint32_t combiner_stage;
            fprintf(stderr,
                    "[PGRAPH-COMB] control=%08X texmodes=%08X other=%08X "
                    "dot=%08X final=%08X/%08X fc=%08X/%08X alpha=%d\n",
                    g_pg.combiner_control, g_pg.shader_stage_program,
                    g_pg.shader_other_stage_input, g_pg.shader_dot_mapping,
                    g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1,
                    g_pg.combiner_final_factor[0],
                    g_pg.combiner_final_factor[1], g_pg.alpha_test);
            for (combiner_stage = 0; combiner_stage < 4u;
                 ++combiner_stage) {
                fprintf(stderr,
                        "[PGRAPH-COMB] s%u rgb=%08X/%08X "
                        "alpha=%08X/%08X c=%08X/%08X "
                        "tex=%d,%08X,%08X,%08X,%08X\n",
                        combiner_stage,
                        g_pg.combiner_color_icw[combiner_stage],
                        g_pg.combiner_color_ocw[combiner_stage],
                        g_pg.combiner_alpha_icw[combiner_stage],
                        g_pg.combiner_alpha_ocw[combiner_stage],
                        g_pg.combiner_factor0[combiner_stage],
                        g_pg.combiner_factor1[combiner_stage],
                        g_pg.tex[combiner_stage].enabled,
                        g_pg.tex[combiner_stage].offset,
                        g_pg.tex[combiner_stage].format,
                        g_pg.tex[combiner_stage].control1,
                        g_pg.tex[combiner_stage].image_rect);
            }
            logged_combiner = 1;
        }
    }    if (g_pg.stats.draw_calls < 8u) {
        fprintf(stderr,
                "[PGRAPH-D3D11] TEX0 enabled=%d off=%08X fmt=%08X "
                "color=%02X ctl0=%08X ctl1=%08X rect=%08X (%ux%u)\n",
                g_pg.tex[0].enabled, g_pg.tex[0].offset,
                g_pg.tex[0].format, color_format,
                g_pg.tex[0].control0, g_pg.tex[0].control1,
                g_pg.tex[0].image_rect, tex_width, tex_height);
    }
    if (g_pg.stats.draw_calls < 3u ||
        (pgraph_cached_getenv("MERCENARIES_TRACE_SPLASH_VSH") != NULL &&
         g_pg.stats.draw_calls < 8u)) {
        for (i = 0; i < count && i < 8u; ++i) {
            const OutputVertex diagnostic = decode_array_fallback_vertex(
                vertex_bytes + (size_t)i * stride, stride, linear_uv,
                tex_width, tex_height);
            fprintf(stderr, "  [%u] pos=(%.1f, %.1f, %.1f, %.1f) "
                    "uv=(%.3f, %.3f)\n", i, diagnostic.x, diagnostic.y,
                    diagnostic.z, diagnostic.rhw, diagnostic.u, diagnostic.v);
        }
    }

    host_depth_test = g_pg.depth_test;
    {
        /* RedEngine's StencilShadowShader::End draws a full-screen darkening
         * quad with Z disabled, then explicitly restores Z testing before the
         * following opaque phase. The retail push stream occasionally omits
         * that cached restore after the quad. Xemu nevertheless observes the
         * intended enabled state. Keep the guest snapshot untouched, but use
         * depth for the narrowly identifiable opaque, depth-writing phase
         * until the stream changes target or supplies its next Z state. */
        const int is_shadow_quad = g_pg.draw_mode == 8u && count == 4u &&
            !g_pg.depth_test && g_pg.stencil_enable && g_pg.blend_enable &&
            g_pg.blend_sfactor ==
                NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO &&
            g_pg.blend_dfactor ==
                NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_ALPHA;
        if (post_shadow_depth_restore_window &&
            (g_pg.surface_color_offset != post_shadow_depth_surface ||
             g_pg.depth_test)) {
            post_shadow_depth_restore_window = 0;
        }
        if (post_shadow_depth_restore_window && g_pg.draw_mode == 6u &&
            !g_pg.stencil_enable && g_pg.depth_write &&
            g_pg.depth_func == D3DCMP_LESSEQUAL) {
            host_depth_test = 1;
        }
        if (is_shadow_quad) {
            post_shadow_depth_restore_window = 1;
            post_shadow_depth_surface = g_pg.surface_color_offset;
        }
        {
            const char *trace_depth_base = pgraph_cached_getenv(
                "MERCENARIES_TRACE_HOST_DEPTH_BASE");
            if (trace_depth_base &&
                    g_mercenaries_gameplay_capture_active != 0u && offset ==
                    (uint32_t)strtoul(trace_depth_base, NULL, 16)) {
                fprintf(stderr,
                        "[PGRAPH-HOST-DEPTH-BASE] draw=%u base=%08X "
                        "guest=%d host=%d write=%d func=%u shadow=%d "
                        "restore-window=%d surface=%08X/%08X\n",
                        g_pg.stats.draw_calls + 1u, offset,
                        g_pg.depth_test, host_depth_test, g_pg.depth_write,
                        g_pg.depth_func, is_shadow_quad,
                        post_shadow_depth_restore_window,
                        g_pg.surface_color_offset,
                        post_shadow_depth_surface);
                fflush(stderr);
            }
        }
    }
    {
        const char *force_depth_base =
            pgraph_cached_getenv("MERCENARIES_DEBUG_FORCE_DEPTH_BASE");
        if (force_depth_base && offset ==
                (uint32_t)strtoul(force_depth_base, NULL, 16))
            host_depth_test = 1;
    }
    depth_alias_source = prepare_draw_render_targets(host_depth_test);
    if (host_depth_test && !g_pg.depth_test) {
        g_pg.depth_test = 1;
        prepare_host_depth_surface(dev);
        g_pg.depth_test = 0;
    } else {
        prepare_host_depth_surface(dev);
    }
    /* Apply the guest snapshot before scoped host overrides.  Applying it
     * afterward silently undid FORCE_DEPTH, NO_DEPTH, NO_CULL, and the sky
     * diagnostics, making their A/B results invalid. */
    apply_guest_render_states(dev);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, host_depth_test);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, g_pg.depth_func);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE,
                                g_pg.depth_write);
    if (pgraph_cached_getenv("MERCENARIES_DEBUG_NO_DEPTH") != NULL) {
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    }
    if (count == 528u && pgraph_cached_getenv("MERCENARIES_DEBUG_SKY_NO_DEPTH") != NULL)
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE,
        pgraph_cached_getenv("MERCENARIES_DEBUG_NO_CULL") != NULL ? D3DCULL_NONE :
        nv2a_cull_to_d3d(g_pg.cull_enable, g_pg.cull_face,
                         g_pg.front_face));
    if (count == 528u && pgraph_cached_getenv("MERCENARIES_DEBUG_SKY_NO_SCISSOR") != NULL)
        d3d8_states_set_scissor(FALSE, 0, 0, 0, 0);
    if (count == 528u && pgraph_cached_getenv("MERCENARIES_TRACE_SKY_STATE") != NULL) {
        static int logged_sky_state;
        if (!logged_sky_state) {
            fprintf(stderr,
                "[PGRAPH-SKY-STATE] target=%08X zeta=%08X mode=%u prim=%d "
                "depth=%d/%d/%u blend=%d/%04X/%04X mask=%08X "
                "surface=%08X pitch=%08X clip=%08X/%08X "
                "window=%08X/%08X type=%u "
                "tex0-off=%08X tex0-format=%08X tex0-control=%08X "
                "tex0-control1=%08X tex0-rect=%08X tex0-aniso=%u "
                "tex0-filter=%08X tex0-address=%08X\n",
                g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                g_pg.draw_mode, g_pg.d3d_prim_type, g_pg.depth_test,
                g_pg.depth_write, g_pg.depth_func, g_pg.blend_enable,
                g_pg.blend_sfactor, g_pg.blend_dfactor, g_pg.color_mask,
                g_pg.surface_format, g_pg.surface_pitch,
                g_pg.surface_clip_h, g_pg.surface_clip_v,
                g_pg.window_clip_h[0], g_pg.window_clip_v[0],
                g_pg.window_clip_type, resolved_texture_offset(0),
                g_pg.tex[0].format, g_pg.tex[0].control0,
                g_pg.tex[0].control1, g_pg.tex[0].image_rect,
                1u << ((g_pg.tex[0].control0 &
                         NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY) >> 4),
                g_pg.tex[0].filter, g_pg.tex[0].address);
            logged_sky_state = 1;
        }
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                  g_pg.blend_enable);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND,
        nv2a_blend_to_d3d(g_pg.blend_sfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND,
        nv2a_blend_to_d3d(g_pg.blend_dfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP,
        nv2a_blend_equation_to_d3d(g_pg.blend_equation));
    if (pgraph_cached_getenv("MERCENARIES_TRACE_SPLASH_VSH") != NULL &&
        g_pg.stats.draw_calls < 8u) {
        fprintf(stderr,
                "[PGRAPH-SPLASH-VSH] next=%u mode=%08X start=%u "
                "base=%08X stride=%u fvf=%08X\n",
                g_pg.stats.draw_calls + 1u, g_pg.transform_execution_mode,
                g_pg.transform_program_start, offset, stride,
                g_pg.guest_fvf);
        for (i = 0; i < NV2A_VS_MAX_INPUTS; ++i) {
            const uint32_t fmt = g_pg.vertex_array[i].format;
            const uint32_t array_stride =
                (fmt & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE) >> 8;
            if (fmt || g_pg.vertex_array[i].offset) {
                fprintf(stderr,
                        "  array%u off=%08X fmt=%08X stride=%u dxgi=%u "
                        "relative=%d\n",
                        i, g_pg.vertex_array[i].offset, fmt, array_stride,
                        (unsigned)vertex_array_dxgi_format(fmt),
                        array_stride == stride &&
                        g_pg.vertex_array[i].offset >= offset &&
                        g_pg.vertex_array[i].offset - offset < stride ?
                            (int)(g_pg.vertex_array[i].offset - offset) : -1);
            }
        }
        for (i = 0; i < 4u; ++i) {
            const uint32_t *c = g_pg.vsh_constants[i];
            fprintf(stderr,
                    "  c%u=%08X %08X %08X %08X "
                    "(%.9g,%.9g,%.9g,%.9g)\n",
                    i, c[0], c[1], c[2], c[3],
                    u2f(c[0]), u2f(c[1]), u2f(c[2]), u2f(c[3]));
        }
    }
    draw_shader = prepare_transform_program(offset, stride);
    if (g_mercenaries_roadblock_model_draw_active != 0u &&
        g_pg.inline_element_count != 0u && count <= 64u &&
        pgraph_cached_getenv("MERCENARIES_TRACE_ROADBLOCK_PGRAPH") != NULL) {
        static uint32_t roadblock_draw_samples;
        if (roadblock_draw_samples < 256u) {
            fprintf(stderr,
                    "[ROADBLOCK-PGRAPH] sample=%u draw=%u count=%u mode=%u "
                    "base=%08X stride=%u vsh=%08X tex=%08X/%08X "
                    "depth=%d/%d/%u blend=%d alpha=%d cull=%d/%08X/%08X "
                    "a0=%08X/%08X a1=%08X/%08X a2=%08X/%08X a3=%08X/%08X\n",
                    roadblock_draw_samples++, g_pg.stats.draw_calls + 1u,
                    count, g_pg.draw_mode, offset, stride,
                    g_pg.host_vsh_hash, resolved_texture_offset(0u),
                    g_pg.tex[0].format, g_pg.depth_test, g_pg.depth_write,
                    g_pg.depth_func, g_pg.blend_enable, g_pg.alpha_test,
                    g_pg.cull_enable, g_pg.cull_face, g_pg.front_face,
                    g_pg.vertex_array[0].offset, g_pg.vertex_array[0].format,
                    g_pg.vertex_array[1].offset, g_pg.vertex_array[1].format,
                    g_pg.vertex_array[2].offset, g_pg.vertex_array[2].format,
                    g_pg.vertex_array[3].offset, g_pg.vertex_array[3].format);
            fflush(stderr);
        }
    }
    {
        const char *trace_hash_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_VSH_STATE_HASH");
        const char *trace_base_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_VSH_STATE_BASE");
        const char *trace_after_standup_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_VSH_STATE_AFTER_STANDUP");
        static uint32_t traced_vsh_state_draws;
        const uint32_t trace_hash = trace_hash_env ?
            (uint32_t)strtoul(trace_hash_env, NULL, 16) : 0u;
        const uint32_t trace_base = trace_base_env ?
            (uint32_t)strtoul(trace_base_env, NULL, 16) : 0u;
        if (trace_hash_env && traced_vsh_state_draws < 16u &&
            g_pg.host_vsh_hash == trace_hash &&
            (!trace_after_standup_env ||
             g_mercenaries_standup_complete != 0u) &&
            (!trace_base_env || offset == trace_base)) {
            fprintf(stderr,
                    "[PGRAPH-VSH-STATE] sample=%u draw=%u hash=%08X "
                    "target=%08X tex=%08X/%08X comb=%08X final=%08X/%08X\n",
                    ++traced_vsh_state_draws, g_pg.stats.draw_calls + 1u,
                    g_pg.host_vsh_hash, g_pg.surface_color_offset,
                    g_pg.tex[0].offset, g_pg.tex[0].format,
                    g_pg.combiner_control, g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1);
            for (i = 0u; i < 32u; ++i) {
                const uint32_t *c = g_pg.vsh_constants[i];
                fprintf(stderr,
                        "  c%u=%08X %08X %08X %08X "
                        "(%.9g,%.9g,%.9g,%.9g)\n",
                        i, c[0], c[1], c[2], c[3],
                        u2f(c[0]), u2f(c[1]), u2f(c[2]), u2f(c[3]));
            }
            fflush(stderr);
        }
    }
    {
        const char *target_env = pgraph_cached_getenv("MERCENARIES_TRACE_TEXTURE_OFFSET");
        static uint32_t target_samples;
        if (target_env != NULL && target_samples < 256u &&
            (g_pg.tex[0].offset & 0x07FFFFFFu) ==
                ((uint32_t)strtoul(target_env, NULL, 16) & 0x07FFFFFFu)) {
            fprintf(stderr,
                    "[PGRAPH-TEXTURE-TARGET] sample=%u draw=%u target=%08X "
                    "mode=%u prim=%d start=%u count=%u base=%08X stride=%u "
                    "tex=%08X/%08X/%ux%u blend=%d/%04X/%04X/%04X "
                    "vsh=%08X/%08X/%u/%u fvf=%08X\n",
                    ++target_samples, g_pg.stats.draw_calls + 1u,
                    g_pg.surface_color_offset, g_pg.draw_mode,
                    g_pg.d3d_prim_type, start, count, offset, stride,
                    g_pg.tex[0].offset, g_pg.tex[0].format,
                    tex_width, tex_height, g_pg.blend_enable,
                    g_pg.blend_sfactor, g_pg.blend_dfactor,
                    g_pg.blend_equation, g_pg.transform_execution_mode,
                    g_pg.host_vsh_hash, g_pg.transform_program_start,
                    g_pg.transform_program_load, g_pg.guest_fvf);
            if (target_samples <= 8u || (start == 404u && count == 46u))
            for (i = 0u; i < NV2A_VS_MAX_INPUTS; ++i) {
                if (g_pg.vertex_array[i].format ||
                    g_pg.vertex_array[i].offset) {
                    fprintf(stderr,
                            "  array%u off=%08X fmt=%08X rel=%d\n", i,
                            g_pg.vertex_array[i].offset,
                            g_pg.vertex_array[i].format,
                            g_pg.vertex_array[i].offset >= offset &&
                            g_pg.vertex_array[i].offset - offset < stride ?
                                (int)(g_pg.vertex_array[i].offset - offset) : -1);
                }
            }
            if (target_samples <= 8u || (start == 404u && count == 46u))
            for (i = 0u; i < count && i < 6u; ++i) {
                const uint8_t *raw = vertex_bytes + (size_t)i * stride;
                fprintf(stderr,
                        "  v%u raw=%08X %08X %08X %08X %08X "
                        "xy=(%.4g,%.4g)\n", i,
                        stride >= 4u ? read_le32(raw + 0u) : 0u,
                        stride >= 8u ? read_le32(raw + 4u) : 0u,
                        stride >= 12u ? read_le32(raw + 8u) : 0u,
                        stride >= 16u ? read_le32(raw + 12u) : 0u,
                        stride >= 20u ? read_le32(raw + 16u) : 0u,
                        stride >= 4u ? u2f(read_le32(raw + 0u)) : 0.0f,
                        stride >= 8u ? u2f(read_le32(raw + 4u)) : 0.0f);
            }
            if (start == 404u && count == 46u) {
                uint32_t c;
                fprintf(stderr, "  font shader microcode:\n");
                for (c = g_pg.transform_program_start;
                     c < g_pg.transform_program_load; ++c) {
                    fprintf(stderr, "    p%u=%08X %08X %08X %08X\n", c,
                            g_pg.transform_program[c][0],
                            g_pg.transform_program[c][1],
                            g_pg.transform_program[c][2],
                            g_pg.transform_program[c][3]);
                }
                fprintf(stderr, "  nonzero font shader constants:\n");
                for (c = 0u; c < NV2A_VERTEXSHADER_CONSTANTS; ++c) {
                    const uint32_t *v = g_pg.vsh_constants[c];
                    if (v[0] || v[1] || v[2] || v[3]) {
                        fprintf(stderr,
                                "    c%u=%08X %08X %08X %08X "
                                "(%.9g,%.9g,%.9g,%.9g)\n", c,
                                v[0], v[1], v[2], v[3],
                                u2f(v[0]), u2f(v[1]), u2f(v[2]), u2f(v[3]));
                    }
                }
            }
            fflush(stderr);
        }
    }
    out = prepare_array_fallback_vertices(draw_shader, vertex_bytes, stride,
        count, linear_uv, tex_width, tex_height);
    if (!draw_shader && !out) {
        free(expanded_elements);
        return;
    }
    if (draw_shader) {
        draw_data = vertex_bytes;
        draw_stride = stride;
        dev->lpVtbl->SetVertexShader(dev, draw_shader);
    } else {
        draw_data = out;
        draw_stride = sizeof(OutputVertex);
        dev->lpVtbl->SetVertexShader(dev,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    }

    /* NV2A mode 8 is a quad list regardless of whether vertices arrive via
     * DRAW_ARRAYS, ARRAY_ELEMENT, or INLINE_ARRAY. D3D11 has no quad
     * primitive, so preserve each vertex verbatim and expand v0,v1,v2,v3
     * into (v0,v1,v2), (v0,v2,v3) before the host consumes it. */
    if (g_pg.draw_mode == 8u) {
        const uint32_t quad_count = count / 4u;
        const size_t expanded_size = (size_t)quad_count * 6u * draw_stride;
        const uint8_t *packed_vertices = (const uint8_t *)draw_data;
        uint32_t q;
        if (!quad_count) {
            free(out);
            free(expanded_elements);
            return;
        }
        expanded_quads = (uint8_t *)_alloca(expanded_size);
        for (q = 0u; q < quad_count; ++q) {
            const uint8_t *src = packed_vertices +
                                 (size_t)q * 4u * draw_stride;
            uint8_t *dst = expanded_quads +
                           (size_t)q * 6u * draw_stride;
            memcpy(dst + (size_t)0u * draw_stride,
                   src + (size_t)0u * draw_stride, draw_stride);
            memcpy(dst + (size_t)1u * draw_stride,
                   src + (size_t)1u * draw_stride, draw_stride);
            memcpy(dst + (size_t)2u * draw_stride,
                   src + (size_t)2u * draw_stride, draw_stride);
            memcpy(dst + (size_t)3u * draw_stride,
                   src + (size_t)0u * draw_stride, draw_stride);
            memcpy(dst + (size_t)4u * draw_stride,
                   src + (size_t)2u * draw_stride, draw_stride);
            memcpy(dst + (size_t)5u * draw_stride,
                   src + (size_t)3u * draw_stride, draw_stride);
        }
        draw_data = expanded_quads;
        submitted_vertex_count = quad_count * 6u;
    } else if (g_pg.draw_mode == 3u) {
        const size_t expanded_size = (size_t)(count + 1u) * draw_stride;
        expanded_line_loop = (uint8_t *)_alloca(expanded_size);
        memcpy(expanded_line_loop, draw_data, (size_t)count * draw_stride);
        memcpy(expanded_line_loop + (size_t)count * draw_stride,
               draw_data, draw_stride);
        draw_data = expanded_line_loop;
        submitted_vertex_count = count + 1u;
    }

    if (g_pg.inline_array_mode &&
        pgraph_cached_getenv("MERCENARIES_TRACE_INLINE_ARRAY") != NULL) {
        static uint32_t traced_inline_arrays;
        if (traced_inline_arrays < 128u) {
            fprintf(stderr,
                    "[PGRAPH-INLINE-ARRAY] draw=%u mode=%u words=%u "
                    "stride=%u count=%u submitted=%u prims=%u vsh=%08X\n",
                    g_pg.stats.draw_calls + 1u, g_pg.draw_mode,
                    g_pg.inline_count, stride, count,
                    submitted_vertex_count, prim_count,
                    (unsigned)draw_shader);
            for (i = 0u; i < NV2A_VS_MAX_INPUTS; ++i) {
                const uint32_t format = g_pg.vertex_array[i].format;
                const uint32_t components = (format >> 4) & 0xFu;
                if (components) {
                    fprintf(stderr,
                            "  attr%u fmt=%08X type=%u count=%u "
                            "off=%u dxgi=%u\n",
                            i, format, format & 0xFu, components,
                            g_pg.inline_array_offsets[i],
                            (unsigned)vertex_array_dxgi_format(format));
                }
            }
            ++traced_inline_arrays;
        }
    }

    for (i = 0; i < 4u; ++i) {
        GuestColorSurface *stage_surface = NULL;
        uint32_t stage_width = 1u, stage_height = 1u;
        const uint32_t stage_color_format =
            (g_pg.tex[i].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
        const int stage_is_linear =
            texture_color_format_is_linear(stage_color_format);
        get_guest_texture_dimensions(g_pg.tex[i].format,
                                     g_pg.tex[i].image_rect,
                                     stage_color_format,
                                     &stage_width, &stage_height);
        d3d8_combiners_set_texture_scale(
            i,
            stage_is_linear && stage_width ?
                1.0f / (float)stage_width : 1.0f,
            stage_is_linear && stage_height ?
                1.0f / (float)stage_height : 1.0f);
        d3d8_combiners_set_texture_red_blue_swap(i, FALSE);
        /* Xemu derives color-key mode and key comparison from the current
         * texture registers for every active texture fetch. Keep ordinary
         * array draws synchronized just like the immediate path; otherwise
         * keyed black texels become opaque rectangles around HUD sprites. */
        d3d8_combiners_set_color_key(
            i,
            g_pg.tex[i].enabled ?
                (g_pg.tex[i].control0 &
                 NV_PGRAPH_TEXCTL0_0_COLORKEYMODE) : 0u,
            g_pg.color_key[i],
            texture_color_key_mask(stage_color_format));
        /* A stale offset may still name a render target after the guest has
         * disabled this texture stage. Xemu gates sampling on TEXCTL0.ENABLE;
         * do the same before resolving an offset to a host SRV. */
        stage_surface_srv[i] = NULL;
        if (g_pg.tex[i].enabled) {
            GuestDepthSurface *direct_depth = direct_bound_depth_sample_source(i);
            if (direct_depth) {
                feedback_srvs[i] = prepare_satellite_depth_sample(direct_depth);
                stage_surface_srv[i] = feedback_srvs[i];
            }
            stage_surface = find_guest_color_surface(resolved_texture_offset(i));
            if (!stage_surface_srv[i] && stage_surface &&
                guest_color_surface_texture_shape_compatible(
                    stage_surface, g_pg.tex[i].format,
                    g_pg.tex[i].image_rect, g_pg.tex[i].control0,
                    g_pg.tex[i].control1) &&
                guest_color_surface_aliases_bound(stage_surface)) {
                /* D3D11 cannot sample a resource while it is bound as an RTV.
                 * Array draws can perform the same retail framebuffer-feedback
                 * composites as immediate draws (HUD, PDA, and shop effects),
                 * so give both submission paths the same pre-draw snapshot. */
                feedback_srvs[i] = snapshot_bound_color_surface(
                    g_pg.bound_color, &feedback_textures[i]);
                stage_surface_srv[i] = feedback_srvs[i];
            } else if (!stage_surface_srv[i]) {
                stage_surface_srv[i] = get_guest_surface_texture(
                    resolved_texture_offset(i), g_pg.tex[i].format,
                    g_pg.tex[i].image_rect, g_pg.tex[i].control0,
                    g_pg.tex[i].control1);
                stage_surface = find_guest_color_surface(
                    resolved_texture_offset(i));
            }
        }
        d3d8_combiners_set_texture_red_blue_swap(
            i, stage_surface_srv[i] && stage_surface &&
               stage_surface->depth_alias_source_offset != 0u &&
               stage_color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8);
        if (count == 528u && i == 1u &&
            pgraph_cached_getenv("MERCENARIES_DEBUG_SKY_STAGE1_VRAM") != NULL)
            stage_surface_srv[i] = NULL;

        dev->lpVtbl->SetTextureStageState(
            dev, i, D3DTSS_ALPHAKILL,
            (g_pg.tex[i].control0 &
             NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN) != 0u);

        if (stage_surface_srv[i]) {
            dev->lpVtbl->SetTexture(dev, i, NULL);
            d3d8_BindExternalTexture(i, stage_surface_srv[i]);
        } else {
            stage_tex[i] = prepare_guest_texture(dev, i);
            dev->lpVtbl->SetTexture(dev, i,
                (IDirect3DBaseTexture8 *)stage_tex[i]);
        }
        if (stage_tex[i] || stage_surface_srv[i]) {
            dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_ADDRESSU,
                nv2a_texture_address_to_d3d(g_pg.tex[i].address & 7u));
            dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_ADDRESSV,
                nv2a_texture_address_to_d3d(
                    (g_pg.tex[i].address >> 8) & 7u));
            dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_BORDERCOLOR,
                                               g_pg.tex[i].border_color);
            apply_nv2a_texture_filter(dev, i, g_pg.tex[i].filter,
                                      g_pg.tex[i].control0,
                                      stage_is_linear);
        }
    }
    tex = stage_tex[0];
    if (tex || stage_surface_srv[0]) {
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP,
            pgraph_cached_getenv("MERCENARIES_DEBUG_TEXTURE_ONLY") != NULL ?
                D3DTOP_SELECTARG1 :
            pgraph_cached_getenv("MERCENARIES_DEBUG_DIFFUSE_ONLY") != NULL ?
                D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1,
                                          D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2,
                                          D3DTA_DIFFUSE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,
                                          D3DTOP_SELECTARG1);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1,
                                          D3DTA_TEXTURE);
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ADDRESSU,
            nv2a_texture_address_to_d3d(g_pg.tex[0].address & 7u));
        dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ADDRESSV,
            nv2a_texture_address_to_d3d(
                (g_pg.tex[0].address >> 8) & 7u));
        apply_nv2a_texture_filter(dev, 0, g_pg.tex[0].filter,
                                  g_pg.tex[0].control0,
                                  texture_color_format_is_linear(
                                      (g_pg.tex[0].format &
                                       NV097_SET_TEXTURE_FORMAT_COLOR) >> 8));
    }

    if (g_pg.combiner_valid &&
        pgraph_cached_getenv("MERCENARIES_DEBUG_TEXTURE_ONLY") == NULL &&
        pgraph_cached_getenv("MERCENARIES_DEBUG_DIFFUSE_ONLY") == NULL) {
        d3d8_combiners_from_nv2a_registers(
            g_pg.combiner_control,
            g_pg.combiner_color_icw, g_pg.combiner_alpha_icw,
            g_pg.combiner_color_ocw, g_pg.combiner_alpha_ocw,
            g_pg.combiner_final_inputs_0, g_pg.combiner_final_inputs_1,
            g_pg.combiner_factor0, g_pg.combiner_factor1,
            g_pg.combiner_final_factor[0],
            g_pg.combiner_final_factor[1],
            g_pg.shader_stage_program, g_pg.shader_dot_mapping,
            g_pg.shader_other_stage_input,
            &combiner_state);
        combiner_state.guest_depth = draw_shader != 0 &&
            g_pg.depth_test && pgraph_guest_depth_enabled() &&
            g_pg.d3d_prim_type >= D3DPT_TRIANGLELIST;
        if (draw_shader != 0 && g_pg.d3d_prim_type >= D3DPT_TRIANGLELIST &&
            g_pg.d3d_prim_type <= D3DPT_TRIANGLEFAN &&
            pgraph_test_precise_depth_enabled())
            combiner_state.guest_depth = 2;
        combiner_state.screen_depth_stage = pgraph_sky_screen_depth_stage(&combiner_state);
        d3d8_combiners_set_nv2a_state(&combiner_state);
    } else {
        d3d8_combiners_set_nv2a_state(NULL);
    }

    /* Colored RedCanvas overlays (damage/survivor flashes) use an untextured,
     * alpha-blended, four-vertex screen-space quad.  Keep this diagnostic
     * independent from the generic full-screen budget: framebuffer resolves
     * and other neutral-color passes otherwise exhaust that budget before a
     * gameplay flash occurs.  Qualifying geometrically and by render state is
     * title-agnostic, while rejecting the ordinary black/grey dimmers keeps the
     * trace focused on the packed diffuse color that reaches the NV2A path. */
    {
        const char *trace_fullscreen = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE");
        const char *trace_gate = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE");
        static uint32_t colored_overlay_trace_count;
        if (trace_fullscreen && colored_overlay_trace_count < 64u &&
            count == 4u && stride == 20u && g_pg.blend_enable &&
            !g_pg.tex[0].enabled &&
            (!trace_gate || !*trace_gate ||
             GetFileAttributesA(trace_gate) != INVALID_FILE_ATTRIBUTES)) {
            const uint32_t color = read_le32(vertex_bytes + 8u);
            const uint8_t blue = (uint8_t)(color & 0xffu);
            const uint8_t green = (uint8_t)((color >> 8) & 0xffu);
            const uint8_t red = (uint8_t)((color >> 16) & 0xffu);
            const float logical_width =
                g_pg.bound_color && g_pg.bound_color->logical_width ?
                (float)g_pg.bound_color->logical_width : 640.0f;
            const float logical_height =
                g_pg.bound_color && g_pg.bound_color->logical_height ?
                (float)g_pg.bound_color->logical_height : 480.0f;
            float min_x = u2f(read_le32(vertex_bytes));
            float max_x = min_x;
            float min_y = u2f(read_le32(vertex_bytes + 4u));
            float max_y = min_y;
            int valid = isfinite(min_x) && isfinite(min_y) &&
                read_le32(vertex_bytes + stride + 8u) == color &&
                read_le32(vertex_bytes + stride * 2u + 8u) == color &&
                read_le32(vertex_bytes + stride * 3u + 8u) == color &&
                (red != green || green != blue);
            uint32_t vertex;
            for (vertex = 1u; vertex < count && valid; ++vertex) {
                const uint8_t *raw = vertex_bytes + (size_t)vertex * stride;
                const float x = u2f(read_le32(raw));
                const float y = u2f(read_le32(raw + 4u));
                if (!isfinite(x) || !isfinite(y)) {
                    valid = 0;
                    break;
                }
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
            if (valid && min_x >= -8.0f && min_y >= -8.0f &&
                min_x <= 8.0f && min_y <= 8.0f &&
                max_x >= logical_width * 0.95f &&
                max_x <= logical_width * 1.05f &&
                max_y >= logical_height * 0.95f &&
                max_y <= logical_height * 1.05f) {
                fprintf(stderr,
                        "[PGRAPH-COLORED-OVERLAY] sample=%u draw=%u "
                        "target=%08X/%ux%u logical=%ux%u bounds="
                        "(%.3f,%.3f)..(%.3f,%.3f) color=%08X "
                        "blend=%d/%04X/%04X/%04X mask=%08X "
                        "depth=%d/%d/%u alpha=%d/%u/%u "
                        "comb=%08X final=%08X/%08X "
                        "rc0=%08X/%08X/%08X/%08X factor=%08X/%08X "
                        "vsh=%08X psh=%08X "
                        "attrs=%08X/%08X/%08X/%08X/%08X\n",
                        ++colored_overlay_trace_count,
                        g_pg.stats.draw_calls + 1u, g_pg.surface_color_offset,
                        g_pg.bound_color ? g_pg.bound_color->width : 0u,
                        g_pg.bound_color ? g_pg.bound_color->height : 0u,
                        g_pg.bound_color ? g_pg.bound_color->logical_width : 0u,
                        g_pg.bound_color ? g_pg.bound_color->logical_height : 0u,
                        min_x, min_y, max_x, max_y, color,
                        g_pg.blend_enable, g_pg.blend_sfactor,
                        g_pg.blend_dfactor, g_pg.blend_equation,
                        g_pg.color_mask, g_pg.depth_test, g_pg.depth_write,
                        g_pg.depth_func, g_pg.alpha_test, g_pg.alpha_func,
                        g_pg.alpha_ref, g_pg.combiner_control,
                        g_pg.combiner_final_inputs_0,
                        g_pg.combiner_final_inputs_1,
                        g_pg.combiner_color_icw[0],
                        g_pg.combiner_color_ocw[0],
                        g_pg.combiner_alpha_icw[0],
                        g_pg.combiner_alpha_ocw[0],
                        g_pg.combiner_factor0[0], g_pg.combiner_factor1[0],
                        g_pg.host_vsh_hash,
                        d3d8_combiners_debug_state_hash(),
                        g_pg.vertex_array[0].format,
                        g_pg.vertex_array[1].format,
                        g_pg.vertex_array[2].format,
                        g_pg.vertex_array[3].format,
                        g_pg.vertex_array[4].format);
                for (vertex = 0u; vertex < count; ++vertex) {
                    const uint8_t *raw = vertex_bytes +
                        (size_t)vertex * stride;
                    fprintf(stderr,
                            "  colored-v%u raw=%08X %08X %08X %08X %08X\n",
                            vertex, read_le32(raw), read_le32(raw + 4u),
                            read_le32(raw + 8u), read_le32(raw + 12u),
                            read_le32(raw + 16u));
                }
                if (pgraph_cached_getenv(
                        "MERCENARIES_CAPTURE_COLORED_OVERLAY_PREFIX") != NULL &&
                    colored_overlay_capture_count < 8u)
                    colored_overlay_capture_pending = 1;
                fflush(stderr);
            }
        }
    }

    /* A RedCanvas screen dimmer is a four-vertex, 20-byte screen-space quad
     * whose diffuse color is 0x20000000. Trace this exact control case
     * independently so ordinary full-screen resolves cannot consume the
     * broader diagnostic budget before the menu opens. */
    {
        const char *trace_fullscreen = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE");
        const char *trace_gate = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE");
        static uint32_t dimmer_array_trace_count;
        if (trace_fullscreen && dimmer_array_trace_count < 16u &&
            count == 4u && stride == 20u &&
            read_le32(vertex_bytes + 8u) == 0x20000000u &&
            read_le32(vertex_bytes + stride + 8u) == 0x20000000u &&
            read_le32(vertex_bytes + stride * 2u + 8u) == 0x20000000u &&
            read_le32(vertex_bytes + stride * 3u + 8u) == 0x20000000u &&
            (!trace_gate || !*trace_gate ||
             GetFileAttributesA(trace_gate) != INVALID_FILE_ATTRIBUTES)) {
            uint32_t vertex;
            fprintf(stderr,
                    "[PGRAPH-DIMMER-ARRAY] sample=%u draw=%u "
                    "target=%08X/%ux%u logical=%ux%u verts=%u stride=%u "
                    "start=%u base=%08X vsh=%08X/%08X "
                    "blend=%d/%04X/%04X/%04X mask=%08X "
                    "depth=%d/%d/%u alpha=%d/%u/%u "
                    "tex0=%d/%08X/%08X comb=%08X final=%08X/%08X "
                    "rc0=%08X/%08X/%08X/%08X factor=%08X/%08X\n",
                    ++dimmer_array_trace_count,
                    g_pg.stats.draw_calls + 1u, g_pg.surface_color_offset,
                    g_pg.bound_color ? g_pg.bound_color->width : 0u,
                    g_pg.bound_color ? g_pg.bound_color->height : 0u,
                    g_pg.bound_color ? g_pg.bound_color->logical_width : 0u,
                    g_pg.bound_color ? g_pg.bound_color->logical_height : 0u,
                    count, stride, start, offset,
                    g_pg.transform_execution_mode, draw_shader,
                    g_pg.blend_enable, g_pg.blend_sfactor,
                    g_pg.blend_dfactor, g_pg.blend_equation,
                    g_pg.color_mask, g_pg.depth_test, g_pg.depth_write,
                    g_pg.depth_func, g_pg.alpha_test, g_pg.alpha_func,
                    g_pg.alpha_ref, g_pg.tex[0].enabled,
                    g_pg.tex[0].offset, g_pg.tex[0].format,
                    g_pg.combiner_control, g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1,
                    g_pg.combiner_color_icw[0],
                    g_pg.combiner_color_ocw[0],
                    g_pg.combiner_alpha_icw[0],
                    g_pg.combiner_alpha_ocw[0],
                    g_pg.combiner_factor0[0], g_pg.combiner_factor1[0]);
            if (g_xbox_mem_offset != 0) {
                const volatile uint32_t *dimmer =
                    (const volatile uint32_t *)((uintptr_t)g_xbox_mem_offset +
                                                0x0035CFF0u);
                fprintf(stderr,
                        "  dimmer-object color=%08X flag=%d flash=%08X "
                        "timer=%g/%08X time=%g/%08X flash_flag=%d\n",
                        dimmer[0x2Cu / 4u],
                        (int32_t)dimmer[0x30u / 4u],
                        dimmer[0x34u / 4u],
                        u2f(dimmer[0x38u / 4u]), dimmer[0x38u / 4u],
                        u2f(dimmer[0x3Cu / 4u]), dimmer[0x3Cu / 4u],
                        (int32_t)dimmer[0x40u / 4u]);
            }
            for (vertex = 0u; vertex < count; ++vertex) {
                const uint8_t *raw = vertex_bytes + (size_t)vertex * stride;
                fprintf(stderr,
                        "  dimmer-v%u raw=%08X %08X %08X %08X %08X\n",
                        vertex, read_le32(raw), read_le32(raw + 4u),
                        read_le32(raw + 8u), read_le32(raw + 12u),
                        read_le32(raw + 16u));
            }
            fflush(stderr);
        }
    }
    /* RedCanvas batches screen-space quads in vertex buffers. Diagnose those
     * through the same opt-in gate as immediate overlays. */
    {
        const char *trace_fullscreen = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE");
        const char *trace_gate = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE");
        static uint32_t fullscreen_array_trace_count;
        if (trace_fullscreen && fullscreen_array_trace_count < 64u &&
            count >= 3u && count <= 8u && stride >= 8u &&
            (!trace_gate || !*trace_gate ||
             GetFileAttributesA(trace_gate) != INVALID_FILE_ATTRIBUTES)) {
            float min_x = u2f(read_le32(vertex_bytes));
            float max_x = min_x;
            float min_y = u2f(read_le32(vertex_bytes + 4u));
            float max_y = min_y;
            float need_x = g_pg.bound_color && g_pg.bound_color->logical_width ?
                0.8f * (float)g_pg.bound_color->logical_width : 512.0f;
            float need_y = g_pg.bound_color && g_pg.bound_color->logical_height ?
                0.8f * (float)g_pg.bound_color->logical_height : 384.0f;
            int valid_bounds = isfinite(min_x) && isfinite(min_y);
            uint32_t vertex;
            for (vertex = 1u; vertex < count && valid_bounds; ++vertex) {
                const uint8_t *raw = vertex_bytes + (size_t)vertex * stride;
                const float x = u2f(read_le32(raw));
                const float y = u2f(read_le32(raw + 4u));
                if (!isfinite(x) || !isfinite(y)) valid_bounds = 0;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
            if (valid_bounds && max_x - min_x >= need_x &&
                max_y - min_y >= need_y) {
                fprintf(stderr,
                        "[PGRAPH-FULLSCREEN-ARRAY] sample=%u draw=%u "
                        "target=%08X/%ux%u logical=%ux%u bounds="
                        "(%.3f,%.3f)..(%.3f,%.3f) verts=%u stride=%u "
                        "start=%u base=%08X vsh=%08X/%08X "
                        "blend=%d/%04X/%04X/%04X mask=%08X "
                        "depth=%d/%d/%u alpha=%d/%u/%u "
                        "tex0=%d/%08X/%08X comb=%08X final=%08X/%08X "
                        "rc0=%08X/%08X/%08X/%08X factor=%08X/%08X\n",
                        ++fullscreen_array_trace_count,
                        g_pg.stats.draw_calls + 1u, g_pg.surface_color_offset,
                        g_pg.bound_color ? g_pg.bound_color->width : 0u,
                        g_pg.bound_color ? g_pg.bound_color->height : 0u,
                        g_pg.bound_color ? g_pg.bound_color->logical_width : 0u,
                        g_pg.bound_color ? g_pg.bound_color->logical_height : 0u,
                        min_x, min_y, max_x, max_y, count, stride, start,
                        offset, g_pg.transform_execution_mode, draw_shader,
                        g_pg.blend_enable, g_pg.blend_sfactor,
                        g_pg.blend_dfactor, g_pg.blend_equation,
                        g_pg.color_mask, g_pg.depth_test, g_pg.depth_write,
                        g_pg.depth_func, g_pg.alpha_test, g_pg.alpha_func,
                        g_pg.alpha_ref, g_pg.tex[0].enabled,
                        g_pg.tex[0].offset, g_pg.tex[0].format,
                        g_pg.combiner_control, g_pg.combiner_final_inputs_0,
                        g_pg.combiner_final_inputs_1,
                        g_pg.combiner_color_icw[0],
                        g_pg.combiner_color_ocw[0],
                        g_pg.combiner_alpha_icw[0],
                        g_pg.combiner_alpha_ocw[0],
                        g_pg.combiner_factor0[0], g_pg.combiner_factor1[0]);
                for (vertex = 0u; vertex < count; ++vertex) {
                    const uint8_t *raw = vertex_bytes +
                        (size_t)vertex * stride;
                    fprintf(stderr,
                            "  v%u raw=%08X %08X %08X %08X %08X\n",
                            vertex, read_le32(raw), read_le32(raw + 4u),
                            stride >= 12u ? read_le32(raw + 8u) : 0u,
                            stride >= 16u ? read_le32(raw + 12u) : 0u,
                            stride >= 20u ? read_le32(raw + 16u) : 0u);
                }
                fflush(stderr);
            }
        }
    }
    {
        const char *series_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_PREFIX");
        if (series_prefix && g_debug_gameplay_array_series_active &&
            g_pg.surface_color_offset == 0x0339C000u && g_pg.bound_color) {
            const char *stride_env = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_STRIDE");
            const char *limit_env = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_LIMIT");
            const char *exact_env = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_EXACT");
            const uint32_t series_stride = stride_env ?
                (uint32_t)strtoul(stride_env, NULL, 10) : 50u;
            const uint32_t series_limit = limit_env ?
                (uint32_t)strtoul(limit_env, NULL, 10) : 700u;
            const uint32_t series_exact = exact_env ?
                (uint32_t)strtoul(exact_env, NULL, 10) : UINT32_MAX;
            const uint32_t series_index = g_debug_gameplay_array_index++;
            if (series_index <= series_limit &&
                (exact_env ? series_index == series_exact :
                 series_index % (series_stride ? series_stride : 1u) == 0u) &&
                d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                             g_pg.bound_color->srv,
                                             g_pg.bound_color->width,
                                             g_pg.bound_color->height)) {
                char series_path[MAX_PATH];
                snprintf(series_path, sizeof(series_path), "%s-%04u.bmp",
                         series_prefix, series_index);
                fprintf(stderr,
                        "[PGRAPH-GAMEPLAY-ARRAY-SERIES] clear=%u array=%u "
                        "draw=%u count=%u base=%08X vsh=%08X tex=%08X/%08X "
                        "path=%s\n",
                        g_debug_gameplay_clear_index, series_index,
                        g_pg.stats.draw_calls + 1u, count, offset,
                        g_pg.host_vsh_hash, resolved_texture_offset(0),
                        g_pg.tex[0].format, series_path);
                {
                    const char *dump_path = pgraph_cached_getenv(
                        "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_DUMP_PATH");
                    if (dump_path) {
                        FILE *dump = fopen(dump_path, "wb");
                        if (dump) {
                            char state_path[MAX_PATH];
                            FILE *state_dump;
                            fwrite(vertex_bytes, stride, count, dump);
                            fclose(dump);
                            snprintf(state_path, sizeof(state_path),
                                     "%s.constants.bin", dump_path);
                            state_dump = fopen(state_path, "wb");
                            if (state_dump) {
                                fwrite(g_pg.vsh_constants, 1,
                                       sizeof(g_pg.vsh_constants), state_dump);
                                fclose(state_dump);
                            }
                            snprintf(state_path, sizeof(state_path),
                                     "%s.program.bin", dump_path);
                            state_dump = fopen(state_path, "wb");
                            if (state_dump) {
                                fwrite(g_pg.transform_program, 1,
                                       sizeof(g_pg.transform_program), state_dump);
                                fclose(state_dump);
                            }
                            fprintf(stderr,
                                    "[PGRAPH-GAMEPLAY-ARRAY-SERIES-DUMP] "
                                    "path=%s stride=%u count=%u program=%u/%u\n",
                                    dump_path, stride, count,
                                    g_pg.transform_program_start,
                                    g_pg.transform_program_load);
                        }
                    }
                }
                d3d8_DebugCaptureFrameToPath(series_path);
            }
        }
    }

    if (g_pg.d3d_prim_type == D3DPT_LINELIST &&
        pgraph_cached_getenv("MERCENARIES_TRACE_RAIN") != NULL &&
        (pgraph_cached_getenv("MERCENARIES_TRACE_RAIN_AFTER_MOVIE") == NULL ||
         GetEnvironmentVariableA("MERCENARIES_RUNTIME_MOVIE_CLOSED", NULL, 0) != 0) &&
        (pgraph_cached_getenv("MERCENARIES_TRACE_RAIN_DRAW_MIN") == NULL ||
         g_pg.stats.draw_calls >= (uint32_t)strtoul(pgraph_cached_getenv(
             "MERCENARIES_TRACE_RAIN_DRAW_MIN"), NULL, 10)) &&
        traced_rain_batches < 8u) {
        fprintf(stderr,
                "[PGRAPH-RAIN] draw=%u gen=%u start=%u count=%u target=%08X "
                "stride=%u blend=%d/%04X/%04X depth=%d/%d/%u "
                "vsh=%08X/%u/%u comb=%08X tex=%08X\n",
                g_pg.stats.draw_calls, g_pg.surface_generation, start, count,
                g_pg.surface_color_offset, stride, g_pg.blend_enable,
                g_pg.blend_sfactor, g_pg.blend_dfactor, g_pg.depth_test,
                g_pg.depth_write, g_pg.depth_func, g_pg.host_vsh_hash,
                g_pg.transform_program_start, g_pg.transform_program_load,
                g_pg.combiner_control, g_pg.shader_stage_program);

        for (i = 0u; i < 7u; ++i) {
            const uint32_t format = g_pg.vertex_array[i].format;
            if (((format >> 4) & 0xFu) == 0u)
                continue;
            fprintf(stderr,
                    "  rain-array%u off=%08X resolved=%08X fmt=%08X "
                    "type=%u count=%u stride=%u\n",
                    i, g_pg.vertex_array[i].offset,
                    resolved_vertex_array_offset(i), format,
                    format & 0xFu, (format >> 4) & 0xFu,
                    (format >> 8) & 0xFFu);
        }

        for (i = 0u; i < count && i < 8u; ++i) {
            const uint8_t *raw = vertex_bytes + (size_t)i * stride;
            fprintf(stderr,
                    "  rain%u raw=%08X %08X %08X %08X %08X\n", i,
                    stride >= 4u ? read_le32(raw + 0u) : 0u,
                    stride >= 8u ? read_le32(raw + 4u) : 0u,
                    stride >= 12u ? read_le32(raw + 8u) : 0u,
                    stride >= 16u ? read_le32(raw + 12u) : 0u,
                    stride >= 20u ? read_le32(raw + 16u) : 0u);
        }
        ++traced_rain_batches;
        fflush(stderr);
    }
    if (captured_rain_batches < 2u &&
        g_pg.d3d_prim_type == D3DPT_LINELIST &&
        (pgraph_cached_getenv("MERCENARIES_TRACE_RAIN_AFTER_MOVIE") == NULL ||
         GetEnvironmentVariableA("MERCENARIES_RUNTIME_MOVIE_CLOSED", NULL, 0) != 0) &&
        (pgraph_cached_getenv("MERCENARIES_TRACE_RAIN_DRAW_MIN") == NULL ||
         g_pg.stats.draw_calls >= (uint32_t)strtoul(pgraph_cached_getenv(
             "MERCENARIES_TRACE_RAIN_DRAW_MIN"), NULL, 10)) &&
        pgraph_cached_getenv("MERCENARIES_CAPTURE_RAIN") != NULL && g_pg.bound_color &&
        d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                     g_pg.bound_color->srv,
                                     g_pg.bound_color->width,
                                     g_pg.bound_color->height)) {
        fprintf(stderr, "[PGRAPH-RAIN-CAPTURE] before draw=%u\n",
                g_pg.stats.draw_calls);
        d3d8_DebugCaptureFrameNow();
    }
    {
        const char *capture_vsh = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_ARRAY_VSH_BEFORE_HASH");
        const char *capture_stride_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_STRIDE");
        const char *capture_draw_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_DRAW");
        const char *capture_start_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_START");
        const char *capture_count_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_COUNT");
        const char *capture_base_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_BASE");
        const char *capture_path = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_ARRAY_VSH_BEFORE_PATH");
        const char *capture_dump_path = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_ARRAY_DUMP_PATH");
        const uint32_t capture_draw = capture_draw_env ?
            (uint32_t)strtoul(capture_draw_env, NULL, 10) : 0u;
        const uint32_t capture_start = capture_start_env ?
            (uint32_t)strtoul(capture_start_env, NULL, 10) : 0u;
        const uint32_t capture_count = capture_count_env ?
            (uint32_t)strtoul(capture_count_env, NULL, 10) : 0u;
        static int captured_array_vsh_before;
        if (!captured_array_vsh_before && array_capture_gate_open &&
            (!capture_stride_env || stride ==
                (uint32_t)strtoul(capture_stride_env, NULL, 0)) &&
            (capture_vsh || capture_count_env) &&
            g_pg.stats.draw_calls >= capture_draw && g_pg.bound_color &&
            (!capture_start_env || start == capture_start) &&
            (!capture_count_env || count == capture_count) &&
            (!capture_base_env || offset ==
                ((uint32_t)strtoul(capture_base_env, NULL, 16) & 0x07FFFFFFu)) &&
            (!capture_vsh || g_pg.host_vsh_hash ==
                (uint32_t)strtoul(capture_vsh, NULL, 16)) &&
            d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                         g_pg.bound_color->srv,
                                         g_pg.bound_color->width,
                                         g_pg.bound_color->height)) {
            fprintf(stderr,
                    "[PGRAPH-CAPTURE-VSH-BEFORE] draw=%u target=%08X "
                    "vsh=%08X start=%u count=%u %ux%u\n",
                    g_pg.stats.draw_calls + 1u, g_pg.bound_color->offset,
                    g_pg.host_vsh_hash, start, count, g_pg.bound_color->width,
                    g_pg.bound_color->height);
            if (capture_path)
                d3d8_DebugCaptureFrameToPath(capture_path);
            else
                d3d8_DebugCaptureFrameNow();
            if (capture_dump_path && vertex_bytes && stride != 0u) {
                char state_path[MAX_PATH];
                FILE *dump = fopen(capture_dump_path, "wb");
                if (dump) {
                    fwrite(vertex_bytes, stride, count, dump);
                    fclose(dump);
                }
                snprintf(state_path, sizeof(state_path), "%s.constants.bin",
                         capture_dump_path);
                dump = fopen(state_path, "wb");
                if (dump) {
                    fwrite(g_pg.vsh_constants, 1,
                           sizeof(g_pg.vsh_constants), dump);
                    fclose(dump);
                }
                snprintf(state_path, sizeof(state_path), "%s.program.bin",
                         capture_dump_path);
                dump = fopen(state_path, "wb");
                if (dump) {
                    fwrite(g_pg.transform_program, 1,
                           sizeof(g_pg.transform_program), dump);
                    fclose(dump);
                }
                fprintf(stderr,
                        "[PGRAPH-CAPTURE-ARRAY-DUMP] path=%s stride=%u "
                        "count=%u program=%u/%u\n",
                        capture_dump_path, stride, count,
                        g_pg.transform_program_start,
                        g_pg.transform_program_load);
                debug_write_array_capture_state(stderr);
                snprintf(state_path, sizeof(state_path), "%s.state.txt",
                         capture_dump_path);
                dump = fopen(state_path, "w");
                if (dump) {
                    fprintf(dump, "stride=%u count=%u start=%u program=%u/%u\n",
                            stride, count, start, g_pg.transform_program_start,
                            g_pg.transform_program_load);
                    debug_write_array_capture_state(dump);
                    fclose(dump);
                }
            }
            captured_array_vsh_before = 1;
        }
    }
    trace_texture_edge_candidate("array", count);
    trace_point_sprite_candidate("array", count);
    trace_line_candidate("array", count);
    trace_guest_draw_state();
    debug_capture_gameplay_draw_before("array", count, offset,
                                       vertex_bytes, stride, 1);
    if (colored_overlay_capture_pending && g_pg.bound_color &&
        g_pg.bound_color->texture) {
        const char *prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_COLORED_OVERLAY_PREFIX");
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s-%02u-before.bmp", prefix,
                 colored_overlay_capture_count);
        d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
    }
    debug_capture_satellite_draw_before("array");
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->DrawPrimitiveUP(dev,
        (D3DPRIMITIVETYPE)g_pg.d3d_prim_type, prim_count, draw_data,
        draw_stride);
    debug_capture_satellite_draw_after("array", start, count, offset, stride,
                                       draw_data, draw_stride,
                                       submitted_vertex_count);
    if (colored_overlay_capture_pending && g_pg.bound_color &&
        g_pg.bound_color->texture) {
        const char *prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_COLORED_OVERLAY_PREFIX");
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s-%02u-target.bmp", prefix,
                 colored_overlay_capture_count++);
        d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
        d3d8_states_debug_trace_blend("colored-overlay-after");
        fprintf(stderr,
                "[PGRAPH-COLORED-OVERLAY-CAPTURE] draw=%u target=%08X "
                "path=%s\n",
                g_pg.stats.draw_calls + 1u, g_pg.bound_color->offset, path);
        fflush(stderr);
    }
    debug_capture_gameplay_draw_after("array");
    for (i = 0u; i < 4u; ++i) {
        if (feedback_srvs[i]) {
            d3d8_BindExternalTexture(i, NULL);
            ID3D11ShaderResourceView_Release(feedback_srvs[i]);
        }
        if (feedback_textures[i])
            ID3D11Texture2D_Release(feedback_textures[i]);
    }
    if (pgraph_cached_getenv("MERCENARIES_TRACE_HOST_BLEND_BASE") != NULL &&
        offset == ((uint32_t)strtoul(pgraph_cached_getenv(
            "MERCENARIES_TRACE_HOST_BLEND_BASE"), NULL, 16) & 0x07FFFFFFu))
        d3d8_states_debug_trace_blend("array");
    if (pgraph_cached_getenv("MERCENARIES_CAPTURE_SPLASH") != NULL) {
        d3d8_PresentFrame();
    }
    g_pg.frame_had_draw = 1;
    if (g_pg.bound_color) {
        g_pg.bound_color->drawn = 1;
        g_pg.bound_color->gpu_drawn = 1;
        g_pg.bound_color->draw_generation = g_pg.surface_generation;
        g_pg.bound_color->write_serial = ++g_pg.surface_write_serial;
        debug_capture_post_pda_pending_write("array");
        if (g_pg.pending_scanout == g_pg.bound_color)
            g_pg.pending_scanout_write_us = pgraph_scanout_clock_us();
        if (depth_alias_source) {
            g_pg.bound_color->depth_alias_source_offset =
                depth_alias_source->offset;
            g_pg.bound_color->depth_alias_source_serial =
                depth_alias_source->write_serial;
        } else {
            /* A subsequent ordinary color write replaces the packed-depth
             * payload.  Keep AA-resolve provenance for partial composites,
             * but do not let stale depth provenance leak into later effects
             * such as Mercenaries' satellite camera. */
            g_pg.bound_color->depth_alias_source_offset = 0u;
            g_pg.bound_color->depth_alias_source_serial = 0u;
        }
        /* Array draws retain AA-resolve provenance across partial composites;
         * packed-depth provenance survives only its concrete producer. */
        g_pg.last_drawn_color = g_pg.bound_color;
    }
    if (g_pg.bound_depth && host_depth_test && g_pg.depth_write &&
        pgraph_cached_getenv("MERCENARIES_DEBUG_NO_DEPTH") == NULL)
        g_pg.bound_depth->write_serial = ++g_pg.surface_write_serial;
    note_bound_depth_stencil_draw();
    if (captured_rain_batches < 2u &&
        g_pg.d3d_prim_type == D3DPT_LINELIST &&
        (pgraph_cached_getenv("MERCENARIES_TRACE_RAIN_AFTER_MOVIE") == NULL ||
         GetEnvironmentVariableA("MERCENARIES_RUNTIME_MOVIE_CLOSED", NULL, 0) != 0) &&
        (pgraph_cached_getenv("MERCENARIES_TRACE_RAIN_DRAW_MIN") == NULL ||
         g_pg.stats.draw_calls >= (uint32_t)strtoul(pgraph_cached_getenv(
             "MERCENARIES_TRACE_RAIN_DRAW_MIN"), NULL, 10)) &&
        pgraph_cached_getenv("MERCENARIES_CAPTURE_RAIN") != NULL && g_pg.bound_color &&
        d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                     g_pg.bound_color->srv,
                                     g_pg.bound_color->width,
                                     g_pg.bound_color->height)) {
        fprintf(stderr, "[PGRAPH-RAIN-CAPTURE] after draw=%u\n",
                g_pg.stats.draw_calls);
        d3d8_DebugCaptureFrameNow();
        ++captured_rain_batches;
    }
    if (g_debug_humvee_repair_capture_pending && count == 273u &&
        g_pg.bound_color) {
        const char *repair_capture_path = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_HUMVEE_REPAIR_PATH");
        if (repair_capture_path && repair_capture_path[0] &&
            d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                         g_pg.bound_color->srv,
                                         g_pg.bound_color->width,
                                         g_pg.bound_color->height)) {
            d3d8_DebugCaptureFrameToPath(repair_capture_path);
            fprintf(stderr,
                    "[PGRAPH-HUMVEE-REPAIR-CAPTURE] draw=%u path=%s\n",
                    g_pg.stats.draw_calls + 1u, repair_capture_path);
            g_debug_humvee_repair_capture_pending = 0;
        }
    }
    {
        const char *capture_target =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_TARGET");
        const char *capture_draw_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_DRAW");
        const char *capture_surface_draw_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_SURFACE_DRAW");
        const uint32_t capture_draw = capture_draw_env ?
            (uint32_t)strtoul(capture_draw_env, NULL, 10) : 0u;
        const uint32_t capture_surface_draw = capture_surface_draw_env ?
            (uint32_t)strtoul(capture_surface_draw_env, NULL, 10) : 0u;
        static int captured_array_target;
        if (!captured_array_target && capture_target &&
            (pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_GAMEPLAY") == NULL ||
             g_mercenaries_gameplay_capture_active != 0u) &&
            (!capture_draw_env || g_pg.stats.draw_calls >= capture_draw) &&
            (!capture_surface_draw_env || (g_pg.bound_color &&
                g_pg.stats.draw_calls >= g_pg.bound_color->created_draw +
                    capture_surface_draw)) &&
            g_pg.bound_color &&
            g_pg.bound_color->anti_aliasing !=
                NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_1 &&
            g_pg.bound_color->offset ==
                ((uint32_t)strtoul(capture_target, NULL, 16) & 0x07FFFFFFu) &&
            d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                         g_pg.bound_color->srv,
                                         g_pg.bound_color->width,
                                         g_pg.bound_color->height)) {
            fprintf(stderr,
                    "[PGRAPH-CAPTURE] array draw=%u target=%08X %ux%u\n",
                    g_pg.stats.draw_calls, g_pg.bound_color->offset,
                    g_pg.bound_color->width, g_pg.bound_color->height);
            d3d8_DebugCaptureFrameNow();
            captured_array_target = 1;
        }
    }
    {
        const char *capture_vsh =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_VSH_HASH");
        const char *capture_stride_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_STRIDE");
        const char *capture_draw_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_DRAW");
        const char *capture_start_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_START");
        const char *capture_count_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_COUNT");
        const char *capture_base_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_BASE");
        const char *capture_path = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_ARRAY_VSH_AFTER_PATH");
        const uint32_t capture_draw = capture_draw_env ?
            (uint32_t)strtoul(capture_draw_env, NULL, 10) : 0u;
        const uint32_t capture_start = capture_start_env ?
            (uint32_t)strtoul(capture_start_env, NULL, 10) : 0u;
        const uint32_t capture_count = capture_count_env ?
            (uint32_t)strtoul(capture_count_env, NULL, 10) : 0u;
        static int captured_array_vsh;
        if (!captured_array_vsh && array_capture_gate_open &&
            (!capture_stride_env || stride ==
                (uint32_t)strtoul(capture_stride_env, NULL, 0)) &&
            (capture_vsh || capture_count_env) &&
            g_pg.stats.draw_calls >= capture_draw && g_pg.bound_color &&
            (!capture_start_env || start == capture_start) &&
            (!capture_count_env || count == capture_count) &&
            (!capture_base_env || offset ==
                ((uint32_t)strtoul(capture_base_env, NULL, 16) & 0x07FFFFFFu)) &&
            (!capture_vsh || g_pg.host_vsh_hash ==
                (uint32_t)strtoul(capture_vsh, NULL, 16)) &&
            d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                         g_pg.bound_color->srv,
                                         g_pg.bound_color->width,
                                         g_pg.bound_color->height)) {
            fprintf(stderr,
                    "[PGRAPH-CAPTURE-VSH] draw=%u target=%08X vsh=%08X "
                    "start=%u count=%u %ux%u\n",
                    g_pg.stats.draw_calls, g_pg.bound_color->offset,
                    g_pg.host_vsh_hash, start, count, g_pg.bound_color->width,
                    g_pg.bound_color->height);
            if (capture_path)
                d3d8_DebugCaptureFrameToPath(capture_path);
            else
                d3d8_DebugCaptureFrameNow();
            captured_array_vsh = 1;
        }
    }
    if (count == 528u && pgraph_cached_getenv("MERCENARIES_CAPTURE_SKY_NOW") != NULL &&
        g_pg.bound_color &&
        d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                     g_pg.bound_color->srv,
                                     g_pg.bound_color->width,
                                     g_pg.bound_color->height)) {
        d3d8_DebugCaptureFrameNow();
    }
    note_guest_surface_resolve("array");
    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += count;
    if (pgraph_trace_draws_enabled()) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw #%u: %u array verts, prim=%d, "
                "prims=%u indexed=%s texture=%s\n", g_pg.stats.draw_calls,
                count, g_pg.d3d_prim_type, prim_count,
                g_pg.inline_element_count ? "yes" : "no",
                tex ? "yes" : "no");
    }
    free(out);
    free(expanded_elements);
}

/* Mirror simple fixed-function texture blits into guest VRAM. Xbox titles use
 * these full-screen quads to build render-to-texture surfaces that later
 * PGRAPH draws sample. Until the D3D11 backend owns guest render targets,
 * keeping this copy path here preserves that producer/consumer boundary. */
static void mirror_guest_fixed_function_blit(const ImmediateOutputVertex *vertices,
                                              uint32_t vertex_count)
{
    NV2AState *d = nv2a_get_state();
    const uint32_t color_format =
        (g_pg.tex[0].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    const uint32_t source_offset = resolved_texture_offset(0);
    const uint32_t target_offset = g_pg.surface_color_offset;
    uint32_t source_width, source_height, source_pitch, target_pitch;
    float min_x, min_y, max_x, max_y;
    uint32_t target_x, target_y, target_width, target_height;
    uint64_t source_end, target_end;
    uint32_t x, y;

    if (g_pg.draw_mode != 8u || vertex_count < 4u ||
        g_pg.blend_enable || !g_pg.tex[0].enabled ||
        !d || !d->vram_ptr || !target_offset ||
        source_offset == target_offset)
        return;
    if (color_format != NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 &&
        color_format != NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 &&
        color_format != NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8)
        return;

    get_guest_texture_dimensions(g_pg.tex[0].format,
        g_pg.tex[0].image_rect, color_format, &source_width, &source_height);
    if (!source_width || !source_height)
        return;
    source_pitch = g_pg.tex[0].control1 >> 16;
    if (source_pitch < source_width * 4u)
        source_pitch = source_width * 4u;
    target_pitch = g_pg.surface_pitch & 0xFFFFu;

    /* This legacy CPU mirror only represents fully contained rectangular
     * copies. The GPU handles clipped geometry; never infer a larger backing
     * allocation or row stride from a quad that extends outside its surface. */
    const uint32_t clip_x = g_pg.surface_clip_h & 0xFFFFu;
    const uint32_t clip_y = g_pg.surface_clip_v & 0xFFFFu;
    const uint32_t clip_width = g_pg.surface_clip_h >> 16;
    const uint32_t clip_height = g_pg.surface_clip_v >> 16;
    if (!clip_width || !clip_height)
        return;
    for (x = 0u; x < vertex_count; ++x) {
        if (!isfinite(vertices[x].x) || !isfinite(vertices[x].y))
            return;
    }
    min_x = max_x = vertices[0].x;
    min_y = max_y = vertices[0].y;
    for (x = 1u; x < vertex_count; ++x) {
        if (vertices[x].x < min_x) min_x = vertices[x].x;
        if (vertices[x].x > max_x) max_x = vertices[x].x;
        if (vertices[x].y < min_y) min_y = vertices[x].y;
        if (vertices[x].y > max_y) max_y = vertices[x].y;
    }
    if (min_x < (float)clip_x || min_y < (float)clip_y ||
        max_x > (float)(clip_x + clip_width) ||
        max_y > (float)(clip_y + clip_height) ||
        max_x <= min_x || max_y <= min_y)
        return;
    target_x = (uint32_t)(min_x + 0.5f);
    target_y = (uint32_t)(min_y + 0.5f);
    target_width = (uint32_t)(max_x - min_x + 0.5f);
    target_height = (uint32_t)(max_y - min_y + 0.5f);
    if (!target_width || !target_height || target_width > 4096u ||
        target_height > 4096u)
        return;
    if (target_pitch < (target_x + target_width) * 4u ||
        target_x + target_width > clip_x + clip_width ||
        target_y + target_height > clip_y + clip_height)
        return;

    source_end = (uint64_t)source_offset +
                 (uint64_t)source_pitch * source_height;
    target_end = (uint64_t)target_offset +
                 (uint64_t)target_pitch * (target_y + target_height);
    if (source_end > (uint64_t)memory_region_size(d->vram) ||
        target_end > (uint64_t)memory_region_size(d->vram))
        return;

    if (source_width == target_width && source_height == target_height &&
        (source_end <= (uint64_t)target_offset ||
         target_end <= (uint64_t)source_offset)) {
        /* A large share of fixed-function render-to-texture blits are exact
         * 1:1 copies between distinct guest allocations. Copying each
         * four-byte pixel individually is needlessly expensive on the
         * emulation thread. Whole-row copies are byte-identical in this
         * strictly disjoint case; retain the pixel path below for every
         * scaled or potentially overlapping blit. */
        for (y = 0u; y < target_height; ++y) {
            uint8_t *target_row = d->vram_ptr + target_offset +
                (size_t)(target_y + y) * target_pitch +
                (size_t)target_x * 4u;
            const uint8_t *source_row = d->vram_ptr + source_offset +
                (size_t)y * source_pitch;
            memcpy(target_row, source_row, (size_t)target_width * 4u);
        }
    } else {
        /* The horizontal nearest-neighbour lookup is identical for every row.
         * Compute it once rather than repeating the quotient/remainder walk
         * for every pixel. Keep row/pixel order for overlapping guest buffers. */
        uint32_t source_columns[4096];
        for (x = 0u; x < target_width; ++x)
            source_columns[x] = (uint32_t)(((uint64_t)x * source_width) /
                                         target_width) * 4u;
        uint32_t source_y = 0u;
        const uint32_t source_y_step = source_height / target_height;
        const uint32_t source_y_remainder_step = source_height % target_height;
        uint32_t source_y_remainder = 0u;
        uint32_t previous_source_y = UINT32_MAX;
        const int disjoint = source_end <= (uint64_t)target_offset ||
                             target_end <= (uint64_t)source_offset;

        for (y = 0u; y < target_height; ++y) {
            uint8_t *target_row = d->vram_ptr + target_offset +
                (size_t)(target_y + y) * target_pitch +
                (size_t)target_x * 4u;
            const uint8_t *source_row = d->vram_ptr + source_offset +
                (size_t)source_y * source_pitch;
            /* Disjoint copies may reuse identical scaled rows. Overlapping
             * guest buffers must retain the original read/write pixel order. */
            if (disjoint && source_width == target_width) {
                memcpy(target_row, source_row, (size_t)target_width * 4u);
            } else if (disjoint && source_y == previous_source_y) {
                memcpy(target_row, target_row - target_pitch,
                       (size_t)target_width * 4u);
            } else {
                for (x = 0u; x < target_width; ++x) {
                    memcpy(target_row + (size_t)x * 4u,
                           source_row + source_columns[x], 4u);
                }
            }
            previous_source_y = source_y;
            source_y += source_y_step;
            source_y_remainder += source_y_remainder_step;
            if (source_y_remainder >= target_height) {
                source_y_remainder -= target_height;
                ++source_y;
            }
        }
    }

    if (pgraph_cached_getenv("MERCENARIES_TRACE_IMMEDIATE") != NULL) {
        fprintf(stderr,
                "[PGRAPH-BLIT] %08X %ux%u pitch=%u -> %08X %ux%u pitch=%u "
                "first=%08X\n",
                source_offset, source_width, source_height, source_pitch,
                target_offset, target_width, target_height, target_pitch,
                read_le32(d->vram_ptr + target_offset));
    }
}

static void submit_draw(void)
{
    if (g_pg.inline_array_mode) {
        const uint32_t stride = prepare_inline_array_layout();
        if (stride != 0u) {
            g_pg.draw_array_start = 0u;
            g_pg.draw_array_count =
                (uint32_t)(((uint64_t)g_pg.inline_count * 4u) / stride);
            submit_array_draw();
        }
        g_pg.inline_count = 0u;
        return;
    }
    if (g_pg.inline_count == 0) {
        if (g_pg.draw_array_count != 0 || g_pg.inline_element_count != 0)
            submit_array_draw();
        return;
    }
    if (g_pg.vert_stride == 0)
        return;

    uint32_t num_verts = g_pg.inline_count / g_pg.vert_stride;
    if (!draw_has_enough_vertices(num_verts, g_pg.d3d_prim_type))
        return;

    const uint32_t *src = g_pg.inline_data;
    int actual_prim_type = g_pg.d3d_prim_type;
    uint32_t out_vert_count = num_verts;
    float (*program_vertices)[NV2A_VS_MAX_INPUTS][4] = NULL;
    DWORD immediate_draw_shader = 0;
    NV2ACombinerState combiner_state;
    const void *immediate_draw_data;
    UINT immediate_draw_stride;
    ID3D11Texture2D *feedback_textures[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *feedback_srvs[4] = { NULL, NULL, NULL, NULL };
    GuestDepthSurface *depth_alias_source;
    int native_haze_slot = 0;
    ID3D11ShaderResourceView *native_haze_sample = NULL;
    const GuestColorSurface *immediate_source_surface =
        g_pg.tex[0].enabled ? find_guest_color_surface(
            resolved_texture_offset(0)) : NULL;
    const int use_dot_st_combiners =
        ((g_pg.shader_stage_program >> 5) & 0x1fu) ==
            NV2A_TEXMODE_DOTPRODUCT &&
        ((g_pg.shader_stage_program >> 10) & 0x1fu) ==
            NV2A_TEXMODE_DOT_ST;
    /* RsLightLensFlare renders an alpha-only visibility probe into a small
     * GPU surface, then composites the flare with SRCALPHA + ONE.  The
     * fixed-function fallback multiplies the probe's intentionally black RGB
     * by diffuse and therefore adds black.  Keep the broader immediate
     * combiner path conservative, but use the retail register combiner for
     * this unambiguous surface-backed additive composite. */
    const int use_flare_combiners = immediate_source_surface &&
        g_pg.blend_enable &&
        g_pg.blend_sfactor == NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA &&
        g_pg.blend_dfactor == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE;
    /* RetroStrike's lens-flare visibility pass selects xboxFlare.xvu at
     * transform-program slot 64.  That screen-space shader copies the full
     * submitted float4 position to oPos, including the projected Z/W used by
     * the depth-tested alpha probe.  Sending this pass through the legacy
     * XYZRHW fallback replaces Z/W with 0/1 and makes every probe visible,
     * so aircraft beacons and vehicle headlights shine through geometry. */
    const int use_flare_probe_vsh =
        (g_pg.transform_execution_mode & 3u) == 2u &&
        g_pg.transform_program_start == 64u &&
        g_pg.immediate_vertex_count == num_verts;
    /* NV097 register combiners apply to every draw submission method.  The
     * earlier immediate fallback only honored them for a handful of inferred
     * effects and otherwise emitted texture * diffuse.  That is not NV2A
     * behavior and corrupts any immediate-mode material whose final combiner
     * selects or complements other inputs (seen as black world geometry).
     * Use the same decoded combiner state as indexed/array draws whenever the
     * guest has supplied valid registers; fixed-function remains solely the
     * pre-combiner startup fallback. */
    const int use_immediate_combiners = g_pg.combiner_valid;

    /* Handle QUADS (mode 8): convert to triangle list (6 verts per quad) */
    int is_quads = (g_pg.draw_mode == 8);
    int is_line_loop = (g_pg.draw_mode == 3);
    uint32_t num_quads = is_quads ? (num_verts / 4) : 0;
    if (is_quads) {
        out_vert_count = num_quads * 6;  /* 2 triangles per quad */
        actual_prim_type = D3DPT_TRIANGLELIST;
    } else if (is_line_loop) {
        out_vert_count = num_verts + 1u;
        actual_prim_type = D3DPT_LINESTRIP;
    }

    /* Calculate primitive count */
    uint32_t prim_count = 0;
    switch (actual_prim_type) {
        case D3DPT_TRIANGLELIST:  prim_count = out_vert_count / 3; break;
        case D3DPT_TRIANGLESTRIP: prim_count = out_vert_count - 2; break;
        case D3DPT_TRIANGLEFAN:   prim_count = out_vert_count - 2; break;
        case D3DPT_LINELIST:      prim_count = out_vert_count / 2; break;
        case D3DPT_LINESTRIP:     prim_count = out_vert_count - 1; break;
        default: prim_count = out_vert_count / 3; break;
    }
    if (prim_count == 0)
        return;

    if (pgraph_cached_getenv("MERCENARIES_TRACE_CLOUD_FLIGHT") != NULL &&
        debug_array_capture_gate_open() &&
        g_pg.d3d_prim_type == D3DPT_TRIANGLESTRIP && num_verts == 4u &&
        !g_pg.tex[0].enabled) {
        static uint32_t traced_cloud_immediate;
        if (traced_cloud_immediate < 96u) {
            uint32_t vertex;
            fprintf(stderr,
                    "[PGRAPH-CLOUD-IMM] sample=%u draw=%u target=%08X "
                    "zeta=%08X depth=%d/%d/%u blend=%d/%04X/%04X/%04X "
                    "vsh=%08X/%08X/%u/%u comb=%08X final=%08X/%08X "
                    "factor=%08X/%08X\n",
                    ++traced_cloud_immediate, g_pg.stats.draw_calls + 1u,
                    g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                    g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                    g_pg.blend_enable, g_pg.blend_sfactor,
                    g_pg.blend_dfactor, g_pg.blend_equation,
                    g_pg.transform_execution_mode, g_pg.host_vsh_hash,
                    g_pg.transform_program_start, g_pg.transform_program_load,
                    g_pg.combiner_control, g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1, g_pg.combiner_factor0[0],
                    g_pg.combiner_factor1[0]);
            for (vertex = 0u; vertex < num_verts; ++vertex) {
                const float *position =
                    g_pg.immediate_vertices[vertex][NV2A_VERTEX_ATTR_POSITION];
                fprintf(stderr,
                        "  cloud-v%u=(%.9g,%.9g,%.9g,%.9g) diffuse=%08X\n",
                        vertex, position[0], position[1], position[2],
                        position[3],
                        g_pg.inline_data[vertex * g_pg.vert_stride + 10u]);
            }
            fflush(stderr);
        }
    }

    /* Convert persistent Xbox immediate registers to a pre-transformed host vertex. */
    ImmediateOutputVertex *out = (ImmediateOutputVertex *)_alloca(
        out_vert_count * sizeof(ImmediateOutputVertex));

    /* Helper to convert one inline vertex */
    #define CONVERT_VERT(dst_idx, src_idx) do { \
        uint32_t _b = (src_idx) * g_pg.vert_stride; \
        out[dst_idx].x     = u2f(src[_b + 0]); \
        out[dst_idx].y     = u2f(src[_b + 1]); \
        out[dst_idx].z     = 0.0f; \
        out[dst_idx].rhw   = 1.0f; \
        out[dst_idx].u0    = u2f(src[_b + 2]); \
        out[dst_idx].v0    = u2f(src[_b + 3]); \
        out[dst_idx].u1    = u2f(src[_b + 4]); \
        out[dst_idx].v1    = u2f(src[_b + 5]); \
        out[dst_idx].u2    = u2f(src[_b + 6]); \
        out[dst_idx].v2    = u2f(src[_b + 7]); \
        out[dst_idx].u3    = u2f(src[_b + 8]); \
        out[dst_idx].v3    = u2f(src[_b + 9]); \
        out[dst_idx].color = src[_b + 10]; \
    } while(0)

    if (is_quads) {
        /* Convert quads (v0,v1,v2,v3) -> two triangles (v0,v1,v2), (v0,v2,v3) */
        uint32_t out_idx = 0;
        for (uint32_t q = 0; q < num_quads; q++) {
            uint32_t qi = q * 4;
            CONVERT_VERT(out_idx + 0, qi + 0);  /* tri 1: v0 */
            CONVERT_VERT(out_idx + 1, qi + 1);  /* tri 1: v1 */
            CONVERT_VERT(out_idx + 2, qi + 2);  /* tri 1: v2 */
            CONVERT_VERT(out_idx + 3, qi + 0);  /* tri 2: v0 */
            CONVERT_VERT(out_idx + 4, qi + 2);  /* tri 2: v2 */
            CONVERT_VERT(out_idx + 5, qi + 3);  /* tri 2: v3 */
            out_idx += 6;
        }
    } else {
        for (uint32_t i = 0; i < num_verts; i++) {
            CONVERT_VERT(i, i);
        }
        if (is_line_loop)
            out[num_verts] = out[0];
    }
    #undef CONVERT_VERT

    /* Persistent NV097 immediate attributes are object-space vertex inputs,
     * not an implicit XYZRHW stream. Xemu retains all 16 float4 attributes
     * for each completed position and runs the active transform program.
     * Build the same interleaved input here; retain the pre-transformed
     * OutputVertex path only as a fallback for fixed-function submissions. */
    if ((g_pg.transform_execution_mode & 3u) == 2u &&
        g_pg.immediate_vertex_count == num_verts) {
        program_vertices = (float (*)[NV2A_VS_MAX_INPUTS][4])_alloca(
            (size_t)out_vert_count * sizeof(*program_vertices));
        if (is_quads) {
            uint32_t q, out_index = 0u;
            for (q = 0u; q < num_quads; ++q) {
                const uint32_t base = q * 4u;
                memcpy(program_vertices[out_index + 0u],
                       g_pg.immediate_vertices[base + 0u],
                       sizeof(*program_vertices));
                memcpy(program_vertices[out_index + 1u],
                       g_pg.immediate_vertices[base + 1u],
                       sizeof(*program_vertices));
                memcpy(program_vertices[out_index + 2u],
                       g_pg.immediate_vertices[base + 2u],
                       sizeof(*program_vertices));
                memcpy(program_vertices[out_index + 3u],
                       g_pg.immediate_vertices[base + 0u],
                       sizeof(*program_vertices));
                memcpy(program_vertices[out_index + 4u],
                       g_pg.immediate_vertices[base + 2u],
                       sizeof(*program_vertices));
                memcpy(program_vertices[out_index + 5u],
                       g_pg.immediate_vertices[base + 3u],
                       sizeof(*program_vertices));
                out_index += 6u;
            }
        } else {
            memcpy(program_vertices, g_pg.immediate_vertices,
                   (size_t)num_verts * sizeof(*program_vertices));
            if (is_line_loop)
                memcpy(program_vertices[num_verts], program_vertices[0],
                       sizeof(*program_vertices));
        }
    }

    /* NV2A PROJECT2D texture modes consume the Xbox linear coordinates
     * directly; the generated combiner shader applies the per-stage
     * 1/width,1/height scale. The legacy fixed-function fallback expects
     * normalized UVs here, so only pre-normalize for that fallback. */
    if (!use_immediate_combiners) {
        uint32_t stage;
        for (stage = 0; stage < 4u; ++stage) {
            const uint32_t color_format =
                (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
            if (texture_color_format_is_linear(color_format)) {
                uint32_t width = 1u, height = 1u, vertex;
                float *uv;
                get_guest_texture_dimensions(g_pg.tex[stage].format,
                    g_pg.tex[stage].image_rect, color_format, &width, &height);
                for (vertex = 0; vertex < out_vert_count; ++vertex) {
                    uv = &out[vertex].u0 + stage * 2u;
                    if (width) uv[0] /= (float)width;
                    if (height) uv[1] /= (float)height;
                }
            }
        }
    }

    mirror_guest_fixed_function_blit(out, out_vert_count);

    /* Chyron scroll: shift X for vertices in the chyron Y band (366-382).
     * Simple continuous scroll - no per-vertex wrapping to avoid artifacts
     * from split triangle-strip quads spanning the screen. */
    if (g_pg.chyron_scroll_offset != 0.0f && out_vert_count >= 6) {
        /* Check if this draw is in the chyron band */
        int is_chyron = 1;
        for (uint32_t i = 0; i < (out_vert_count < 8 ? out_vert_count : 8); i++) {
            if (out[i].y < 360.0f || out[i].y > 390.0f) {
                is_chyron = 0;
                break;
            }
        }
        if (is_chyron) {
            /* Find the total text width */
            float min_x = 9999.0f, max_x = -9999.0f;
            for (uint32_t i = 0; i < out_vert_count; i++) {
                if (out[i].x < min_x) min_x = out[i].x;
                if (out[i].x > max_x) max_x = out[i].x;
            }
            float text_width = max_x - min_x;

            /* Scroll loops: text slides left, then resets to start position.
             * Total cycle = text scrolls fully off-left + re-enters from right. */
            float cycle = text_width + 640.0f;
            float scroll = fmodf(g_pg.chyron_scroll_offset, cycle);

            /* Apply uniform shift to ALL vertices (no per-vertex wrap) */
            for (uint32_t i = 0; i < out_vert_count; i++) {
                out[i].x -= scroll;
            }
        }
    }

    /* Log first few draws' vertex positions (once) */
    if (g_pg.stats.draw_calls < 3 && num_verts >= 3) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw verts (mode=%u, %u in -> %u out):\n",
                g_pg.draw_mode, num_verts, out_vert_count);
        uint32_t show = num_verts < 8 ? num_verts : 8;
        for (uint32_t i = 0; i < show; i++) {
            uint32_t b = i * g_pg.vert_stride;
            fprintf(stderr, "  [%u] pos=(%.1f, %.1f) uv=(%.3f, %.3f) color=0x%08X\n",
                    i, u2f(src[b+0]), u2f(src[b+1]), u2f(src[b+2]), u2f(src[b+3]), src[b+10]);
        }
    }

    /* Get D3D8 device */
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    if (!dev) return;

    /* Restore the NV2A state captured for this immediate-mode draw. */
    depth_alias_source = prepare_draw_render_targets(g_pg.depth_test);
    prepare_host_depth_surface(dev);
    native_haze_slot = begin_native_haze(num_verts, immediate_source_surface, &native_haze_sample);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, g_pg.depth_test);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, g_pg.depth_func);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE,
                                g_pg.depth_write);
    if (pgraph_cached_getenv("MERCENARIES_DEBUG_NO_DEPTH") != NULL) {
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE,
        pgraph_cached_getenv("MERCENARIES_DEBUG_NO_CULL") != NULL ? D3DCULL_NONE :
        nv2a_cull_to_d3d(g_pg.cull_enable, g_pg.cull_face,
                         g_pg.front_face));
    apply_guest_render_states(dev);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                g_pg.blend_enable);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND,
        nv2a_blend_to_d3d(g_pg.blend_sfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND,
        nv2a_blend_to_d3d(g_pg.blend_dfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP,
        nv2a_blend_equation_to_d3d(g_pg.blend_equation));

    immediate_draw_data = out;
    immediate_draw_stride = sizeof(ImmediateOutputVertex);
    if (program_vertices) {
        g_pg.immediate_buffer_mode = 1;
        immediate_draw_shader = prepare_transform_program(
            0u, (uint32_t)sizeof(*program_vertices));
        g_pg.immediate_buffer_mode = 0;
    }
    if (immediate_draw_shader) {
        immediate_draw_data = program_vertices;
        immediate_draw_stride = (UINT)sizeof(*program_vertices);
        dev->lpVtbl->SetVertexShader(dev, immediate_draw_shader);
    } else {
        dev->lpVtbl->SetVertexShader(
            dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX3);
    }
    {
        uint32_t stage_width = 1u, stage_height = 1u;
        const uint32_t stage_color_format =
            (g_pg.tex[0].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
        const int stage_is_linear =
            texture_color_format_is_linear(stage_color_format);
        get_guest_texture_dimensions(g_pg.tex[0].format,
            g_pg.tex[0].image_rect, stage_color_format,
            &stage_width, &stage_height);
        d3d8_combiners_set_texture_scale(0,
            stage_is_linear && stage_width ?
                1.0f / (float)stage_width : 1.0f,
            stage_is_linear && stage_height ?
                1.0f / (float)stage_height : 1.0f);
    }

    /* Bind texture based on NV2A VRAM offset.
     * Game-specific texture mapping is handled via GAME_HAS_FONT_ATLAS
     * compile flag. Generic path uses vertex color only. */
#ifdef GAME_HAS_FONT_ATLAS
    if (g_textures_loaded) {
        if (!g_pg.texture_lookup_done) {
            g_pg.texture_lookup_done = 1;
            fprintf(stderr, "[PGRAPH-D3D11] Texture lookup init (global_txd has %d textures)\n",
                    g_global_txd.count);
            for (int ti = 0; ti < g_global_txd.count; ti++) {
                fprintf(stderr, "    [%3d] %-24s %3ux%-3u fmt=0x%X\n",
                        ti, g_global_txd.entries[ti].name,
                        g_global_txd.entries[ti].width,
                        g_global_txd.entries[ti].height,
                        g_global_txd.entries[ti].format);
            }
        }

        IDirect3DTexture8 *tex = NULL;
        uint32_t vram_off = resolved_texture_offset(0);
        switch (vram_off) {
            case 0x03C1ED00: tex = txd_find(&g_global_txd, "B3Logo"); break;
            case 0x03C24700: tex = txd_find(&g_global_txd, "bg"); break;
            case 0x03C24B80: tex = txd_find(&g_global_txd, "big_curve"); break;
            case 0x03C7BE00: tex = txd_find(&g_global_txd, "Buttons"); break;
            case 0x03C95700: tex = txd_find(&g_global_txd, "dpad"); break;
            case 0x03C95980: tex = txd_find(&g_global_txd, "FE"); break;
            case 0x03CA1A80: tex = txd_find(&g_global_txd, "small_curve"); break;
            case 0x03D57000: tex = txd_find(&g_global_txd, "box_curve"); break;
            case 0x03CB9200: tex = txd_find(&g_global_txd, "grid"); break;
            case 0x02EC0400:
                dev->lpVtbl->EndScene(dev);
                g_pg.inline_count = 0;
                return;
            case 0x021C4100:
                if (!g_pg.font_atlas) {
                    g_pg.font_atlas = create_dxt5_texture(dev,
                        FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT,
                        font_atlas_dxt5, FONT_ATLAS_SIZE);
                }
                tex = g_pg.font_atlas;
                break;
            case 0: tex = NULL; break;
            default: tex = NULL; break;
        }

        if (tex) {
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 13 /*ADDRESSU*/, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 14 /*ADDRESSV*/, 3 /*CLAMP*/);
        } else {
            /* No texture - use vertex color only */
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 0 /*DIFFUSE*/);
        }
    } else {
        dev->lpVtbl->SetTexture(dev, 0, NULL);
    }
#else
    /* Generic path: bind every NV2A texture stage. Mercenaries' glow and
     * video filters sample the current render target through stages 0..2. */
    {
        IDirect3DTexture8 *stage_tex[4] = { NULL, NULL, NULL, NULL };
        ID3D11ShaderResourceView *stage_surface_srv[4] = { NULL, NULL, NULL, NULL };
        uint32_t stage;

        for (stage = 0; stage < 4u; ++stage) {
            GuestColorSurface *stage_surface = NULL;
            GuestDepthSurface *satellite_depth_source = NULL;
            int stage_uses_satellite_depth = 0;
            const char *satellite_depth_reason = "stage-disabled";
            uint32_t width = 1u, height = 1u;
            const uint32_t color_format =
                (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
            const int linear = texture_color_format_is_linear(color_format);
            get_guest_texture_dimensions(g_pg.tex[stage].format,
                g_pg.tex[stage].image_rect, color_format, &width, &height);
            d3d8_combiners_set_texture_scale(stage,
                linear && width ? 1.0f / (float)width : 1.0f,
                linear && height ? 1.0f / (float)height : 1.0f);
            d3d8_combiners_set_texture_red_blue_swap(stage, FALSE);
            d3d8_combiners_set_color_key(stage,
                g_pg.tex[stage].enabled ?
                    (g_pg.tex[stage].control0 &
                     NV_PGRAPH_TEXCTL0_0_COLORKEYMODE) : 0u,
                g_pg.color_key[stage],
                texture_color_key_mask(color_format));

            /* Immediate draws share the host sampler state with array draws.
             * Synchronize ALPHAKILL as well: a previous cutout material may
             * enable it, but a later render-target reduction must write zero
             * alpha rather than discard those texels and retain stale pixels.
             * Xemu derives alpha_kill from the current TEXCTL0 for each draw. */
            dev->lpVtbl->SetTextureStageState(
                dev, stage, D3DTSS_ALPHAKILL,
                (g_pg.tex[stage].control0 &
                 NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN) != 0u);

            /* SET_TEXTURE_CONTROL0 can disable a stage without clearing its
             * previous offset. Never resurrect that stale surface binding. */
            stage_surface_srv[stage] = NULL;
            if (g_pg.tex[stage].enabled) {
                satellite_depth_source = satellite_depth_filter_source(
                    stage, color_format, &satellite_depth_reason);
                if (satellite_depth_source) {
                    feedback_srvs[stage] = prepare_satellite_depth_sample(
                        satellite_depth_source);
                    stage_surface_srv[stage] = feedback_srvs[stage];
                    stage_uses_satellite_depth =
                        stage_surface_srv[stage] != NULL;
                }
                stage_surface = find_guest_color_surface(
                    resolved_texture_offset(stage));
                if (!stage_surface_srv[stage] && stage_surface &&
                    guest_color_surface_texture_shape_compatible(
                        stage_surface, g_pg.tex[stage].format,
                        g_pg.tex[stage].image_rect,
                        g_pg.tex[stage].control0,
                        g_pg.tex[stage].control1) &&
                    guest_color_surface_aliases_bound(stage_surface)) {
                    feedback_srvs[stage] = snapshot_bound_color_surface(
                        g_pg.bound_color, &feedback_textures[stage]);
                    stage_surface_srv[stage] = feedback_srvs[stage];
                } else if (!stage_surface_srv[stage]) {
                    stage_surface_srv[stage] = get_guest_surface_texture(
                        resolved_texture_offset(stage),
                        g_pg.tex[stage].format,
                        g_pg.tex[stage].image_rect,
                        g_pg.tex[stage].control0,
                        g_pg.tex[stage].control1);
                    stage_surface = find_guest_color_surface(
                        resolved_texture_offset(stage));
                }
            }
            d3d8_combiners_set_texture_red_blue_swap(
                stage, stage_surface_srv[stage] &&
                       (stage_uses_satellite_depth ||
                        (stage_surface &&
                         stage_surface->depth_alias_source_offset != 0u)) &&
                       color_format ==
                           NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8);

            debug_trace_satellite_depth_decision(stage, color_format,
                satellite_depth_reason, satellite_depth_source,
                stage_uses_satellite_depth);

            if (native_haze_sample && stage < (native_haze_slot > 0 ? 3u : 1u))
                stage_surface_srv[stage] = native_haze_sample;
            if (stage_surface_srv[stage]) {
                dev->lpVtbl->SetTexture(dev, stage, NULL);
                d3d8_BindExternalTexture(stage, stage_surface_srv[stage]);
            } else {
                stage_tex[stage] = prepare_guest_texture(dev, stage);
                dev->lpVtbl->SetTexture(dev, stage,
                    (IDirect3DBaseTexture8 *)stage_tex[stage]);
            }
            if (stage_tex[stage] || stage_surface_srv[stage]) {
                dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_ADDRESSU,
                    nv2a_texture_address_to_d3d(
                        g_pg.tex[stage].address & 7u));
                dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_ADDRESSV,
                    nv2a_texture_address_to_d3d(
                        (g_pg.tex[stage].address >> 8) & 7u));
                dev->lpVtbl->SetTextureStageState(
                    dev, stage, D3DTSS_BORDERCOLOR,
                    g_pg.tex[stage].border_color);
                apply_nv2a_texture_filter(dev, stage,
                                          g_pg.tex[stage].filter,
                                          g_pg.tex[stage].control0, linear);
            }
        }

        /* Immediate and array submissions share the guest's register-combiner
         * state; submission style does not alter NV2A pixel semantics. */
        if (use_immediate_combiners) {
            d3d8_combiners_from_nv2a_registers(
                g_pg.combiner_control,
                g_pg.combiner_color_icw, g_pg.combiner_alpha_icw,
                g_pg.combiner_color_ocw, g_pg.combiner_alpha_ocw,
                g_pg.combiner_final_inputs_0, g_pg.combiner_final_inputs_1,
                g_pg.combiner_factor0, g_pg.combiner_factor1,
                g_pg.combiner_final_factor[0],
                g_pg.combiner_final_factor[1],
                g_pg.shader_stage_program, g_pg.shader_dot_mapping,
                g_pg.shader_other_stage_input,
                &combiner_state);
            /* xboxFlare reduces visibility in authored 8/4/2 guest-pixel
             * cells. Host-resolution bilinear footprints must not replace that
             * filter: at 4K a partly occluded lamp can otherwise become zero.
             * Restrict guest-grid filtering to its surface-backed reduction
             * and additive composite passes; scene textures remain ordinary. */
            if (pgraph_cached_getenv("MERCENARIES_DISABLE_FLARE_GRID") == NULL &&
                immediate_source_surface && g_pg.bound_color &&
                texture_color_format_is_linear((g_pg.tex[0].format &
                    NV097_SET_TEXTURE_FORMAT_COLOR) >> 8)) {
                const int reduction = use_flare_probe_vsh && !g_pg.depth_test &&
                    !g_pg.blend_enable && g_pg.color_mask == 0x01010101u &&
                    immediate_source_surface != g_pg.bound_color;
                if (reduction || use_flare_combiners) {
                    combiner_state.flare_grid = reduction ? 3 : 1;
                    d3d8_combiners_set_flare_grid_ratio(
                        (float)g_pg.bound_color->logical_width / g_pg.bound_color->width,
                        (float)g_pg.bound_color->logical_height / g_pg.bound_color->height);
                }
            }
            combiner_state.guest_depth = immediate_draw_shader != 0 &&
                g_pg.depth_test && pgraph_guest_depth_enabled() &&
            g_pg.d3d_prim_type >= D3DPT_TRIANGLELIST;
            if (immediate_draw_shader != 0 && g_pg.d3d_prim_type >= D3DPT_TRIANGLELIST &&
                g_pg.d3d_prim_type <= D3DPT_TRIANGLEFAN &&
                pgraph_test_precise_depth_enabled())
                combiner_state.guest_depth = 2;
            combiner_state.screen_depth_stage = pgraph_sky_screen_depth_stage(&combiner_state);
            d3d8_combiners_set_nv2a_state(&combiner_state);
        } else {
            d3d8_combiners_set_nv2a_state(NULL);
            /* DrawPrimitiveUP asks the D3D8 layer to prepare shaders again.
             * Clear any token left by an earlier HLE draw so the fixed-
             * function state below is not silently replaced by a stale
             * combiner. The retail loading wheel deliberately sets PS=0. */
            d3d8_combiners_set_pixel_shader(0);
            if (stage_tex[0] || stage_surface_srv[0]) {
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP,
                                                  D3DTOP_MODULATE);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1,
                                                  D3DTA_TEXTURE);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG2,
                                                  D3DTA_DIFFUSE);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,
                                                  D3DTOP_SELECTARG1);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1,
                                                  D3DTA_TEXTURE);
            } else {
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP,
                                                  D3DTOP_SELECTARG1);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1,
                                                  D3DTA_DIFFUSE);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,
                                                  D3DTOP_SELECTARG1);
                dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1,
                                                  D3DTA_DIFFUSE);
            }
        }
    }
#endif
    if (pgraph_cached_getenv("MERCENARIES_TRACE_IMMEDIATE") != NULL) {
        fprintf(stderr,
                "[PGRAPH-IMM] mode=%u verts=%u color=%08X zeta=%08X "
                "surface_fmt=%08X pitch=%08X tex0=%08X fmt=%08X "
                "ctl1=%08X rect=%08X\n",
                g_pg.draw_mode, num_verts, g_pg.surface_color_offset,
                g_pg.surface_zeta_offset, g_pg.surface_format,
                g_pg.surface_pitch, g_pg.tex[0].offset,
                g_pg.tex[0].format, g_pg.tex[0].control1,
                g_pg.tex[0].image_rect);
    }

    {
        const char *window_env = pgraph_cached_getenv("MERCENARIES_TRACE_IMMEDIATE_WINDOW");
        const char *minimum_env = pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_MIN");
        const uint32_t minimum_draw = minimum_env ?
            (uint32_t)strtoul(minimum_env, NULL, 10) : 0u;
        const uint32_t window = window_env ?
            (uint32_t)strtoul(window_env, NULL, 10) : 0u;
        if (window && g_pg.stats.draw_calls >= minimum_draw &&
            g_pg.stats.draw_calls < minimum_draw + window) {
            fprintf(stderr,
                    "[PGRAPH-IMM-WINDOW] draw=%u mode=%u prim=%d "
                    "verts=%u target=%08X zeta=%08X depth=%d/%d/%u "
                    "stencil=%d/%u/%02X/%02X/%u/%u/%u "
                    "blend=%d/%04X/%04X/%04X cull=%d/%08X/%08X "
                    "vsh=%08X/%u/%u a0=%08X/%08X a3=%08X/%08X "
                    "a4=%08X/%08X comb=%08X tex=%08X final=%08X/%08X\n",
                    g_pg.stats.draw_calls, g_pg.draw_mode,
                    actual_prim_type, out_vert_count,
                    g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                    g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                    g_pg.stencil_enable, g_pg.stencil_func,
                    g_pg.stencil_ref, g_pg.stencil_mask,
                    g_pg.stencil_fail, g_pg.stencil_zfail,
                    g_pg.stencil_zpass,
                    g_pg.blend_enable, g_pg.blend_sfactor,
                    g_pg.blend_dfactor, g_pg.blend_equation,
                    g_pg.cull_enable, g_pg.cull_face,
                    g_pg.front_face, g_pg.transform_execution_mode,
                    g_pg.transform_program_start,
                    g_pg.transform_program_load,
                    g_pg.vertex_array[0].offset,
                    g_pg.vertex_array[0].format,
                    g_pg.vertex_array[3].offset,
                    g_pg.vertex_array[3].format,
                    g_pg.vertex_array[4].offset,
                    g_pg.vertex_array[4].format,
                    g_pg.combiner_control,
                    g_pg.shader_stage_program,
                    g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1);
        }
    }
    {
        const char *trace_target = pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_TARGET");
        const char *trace_source = pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_SOURCE");
        const char *trace_min_env = pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_MIN");
        const uint32_t trace_min = trace_min_env ?
            (uint32_t)strtoul(trace_min_env, NULL, 10) : 0u;
        static uint32_t targeted_trace_count;
        if ((trace_target || trace_source) &&
            g_pg.stats.draw_calls >= trace_min &&
            targeted_trace_count < 16u &&
            (!trace_target || g_pg.surface_color_offset ==
                ((uint32_t)strtoul(trace_target, NULL, 16) & 0x07FFFFFFu)) &&
            (!trace_source || resolved_texture_offset(0u) ==
                ((uint32_t)strtoul(trace_source, NULL, 16) & 0x07FFFFFFu))) {
            uint32_t vertex;
            uint32_t combiner_stage;
            fprintf(stderr,
                    "[PGRAPH-IMM-TARGET] draw=%u target=%08X host=%ux%u "
                    "clip=%08X/%08X verts=%u stride=%u\n",
                    g_pg.stats.draw_calls, g_pg.surface_color_offset,
                    d3d8_GetRenderTargetWidth(), d3d8_GetRenderTargetHeight(),
                    g_pg.surface_clip_h, g_pg.surface_clip_v,
                    out_vert_count, g_pg.vert_stride);
            fprintf(stderr,
                    "  comb=%08X modes=%08X other=%08X final=%08X/%08X\n",
                    g_pg.combiner_control, g_pg.shader_stage_program,
                    g_pg.shader_other_stage_input,
                    g_pg.combiner_final_inputs_0,
                    g_pg.combiner_final_inputs_1);
            fprintf(stderr, "  final_factor=%08X/%08X\n",
                    g_pg.combiner_final_factor[0],
                    g_pg.combiner_final_factor[1]);
            for (combiner_stage = 0u;
                 combiner_stage < (g_pg.combiner_control & 0xFFu) &&
                 combiner_stage < 8u;
                 ++combiner_stage) {
                fprintf(stderr,
                        "  rc%u rgb=%08X/%08X alpha=%08X/%08X "
                        "factor=%08X/%08X\n",
                        combiner_stage,
                        g_pg.combiner_color_icw[combiner_stage],
                        g_pg.combiner_color_ocw[combiner_stage],
                        g_pg.combiner_alpha_icw[combiner_stage],
                        g_pg.combiner_alpha_ocw[combiner_stage],
                        g_pg.combiner_factor0[combiner_stage],
                        g_pg.combiner_factor1[combiner_stage]);
            }
            for (vertex = 0u; vertex < 3u; ++vertex) {
                uint32_t tex_width = 1u, tex_height = 1u;
                const uint32_t tex_color =
                    (g_pg.tex[vertex].format &
                     NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
                get_guest_texture_dimensions(g_pg.tex[vertex].format,
                    g_pg.tex[vertex].image_rect, tex_color,
                    &tex_width, &tex_height);
                fprintf(stderr,
                        "  t%u off=%08X fmt=%08X rect=%08X size=%ux%u\n",
                        vertex, g_pg.tex[vertex].offset,
                        g_pg.tex[vertex].format,
                        g_pg.tex[vertex].image_rect,
                        tex_width, tex_height);
            }
            for (vertex = 0u; vertex < out_vert_count && vertex < 6u;
                 ++vertex) {
                fprintf(stderr,
                        "  v%u xy=(%.3f,%.3f) uv0=(%.3f,%.3f) "
                        "uv1=(%.3f,%.3f) uv2=(%.3f,%.3f) color=%08X\n",
                        vertex, out[vertex].x, out[vertex].y,
                        out[vertex].u0, out[vertex].v0,
                        out[vertex].u1, out[vertex].v1,
                        out[vertex].u2, out[vertex].v2,
                        out[vertex].color);
            }
            {
                const char *capture_source_prefix = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_DRAW_SOURCE_PREFIX");
                if (capture_source_prefix && *capture_source_prefix &&
                    immediate_source_surface &&
                    immediate_source_surface->texture) {
                    char source_path[MAX_PATH];
                    snprintf(source_path, sizeof(source_path),
                             "%s-%u-source-%08X.bmp", capture_source_prefix,
                             g_pg.stats.draw_calls,
                             immediate_source_surface->offset);
                    d3d8_DebugCaptureTextureToPath(
                        immediate_source_surface->texture, source_path);
                }
            }
            ++targeted_trace_count;
        }
    }

    /* Opt-in, bounded diagnosis of screen-covering immediate draws. This is
     * deliberately geometric and title-agnostic: callers may gate it with a
     * file created at the interesting transition, while the shared renderer
     * reports the actual NV2A state rather than guessing the effect's owner. */
    {
        const char *trace_fullscreen = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE");
        const char *trace_gate = pgraph_cached_getenv(
            "MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE");
        static uint32_t fullscreen_trace_count;
        if (trace_fullscreen && fullscreen_trace_count < 64u &&
            (!trace_gate || !*trace_gate ||
             GetFileAttributesA(trace_gate) != INVALID_FILE_ATTRIBUTES)) {
            float minimum_x = out[0].x, maximum_x = out[0].x;
            float minimum_y = out[0].y, maximum_y = out[0].y;
            float required_width = 512.0f;
            float required_height = 384.0f;
            uint32_t vertex;
            for (vertex = 1u; vertex < out_vert_count; ++vertex) {
                if (out[vertex].x < minimum_x) minimum_x = out[vertex].x;
                if (out[vertex].x > maximum_x) maximum_x = out[vertex].x;
                if (out[vertex].y < minimum_y) minimum_y = out[vertex].y;
                if (out[vertex].y > maximum_y) maximum_y = out[vertex].y;
            }
            if (g_pg.bound_color && g_pg.bound_color->logical_width &&
                g_pg.bound_color->logical_height) {
                required_width = 0.8f *
                    (float)g_pg.bound_color->logical_width;
                required_height = 0.8f *
                    (float)g_pg.bound_color->logical_height;
            }
            if (maximum_x - minimum_x >= required_width &&
                maximum_y - minimum_y >= required_height) {
                uint32_t stage;
                uint32_t combiner_stage;
                fprintf(stderr,
                        "[PGRAPH-FULLSCREEN-IMM] sample=%u draw=%u "
                        "target=%08X/%ux%u logical=%ux%u bounds="
                        "(%.3f,%.3f)..(%.3f,%.3f) verts=%u mode=%u "
                        "vsh=%08X/%08X/%08X start=%u combiner=%d color=%08X "
                        "blend=%d/%04X/%04X/%04X mask=%08X "
                        "depth=%d/%d/%u alpha=%d/%u/%u "
                        "stencil=%d/%u/%02X/%02X\n",
                        ++fullscreen_trace_count,
                        g_pg.stats.draw_calls + 1u,
                        g_pg.surface_color_offset,
                        g_pg.bound_color ? g_pg.bound_color->width : 0u,
                        g_pg.bound_color ? g_pg.bound_color->height : 0u,
                        g_pg.bound_color ?
                            g_pg.bound_color->logical_width : 0u,
                        g_pg.bound_color ?
                            g_pg.bound_color->logical_height : 0u,
                        minimum_x, minimum_y, maximum_x, maximum_y,
                        out_vert_count, g_pg.draw_mode,
                        g_pg.transform_execution_mode,
                        immediate_draw_shader, g_pg.host_vsh_hash,
                        g_pg.transform_program_start,
                        use_immediate_combiners,
                        out[0].color, g_pg.blend_enable,
                        g_pg.blend_sfactor, g_pg.blend_dfactor,
                        g_pg.blend_equation, g_pg.color_mask,
                        g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                        g_pg.alpha_test, g_pg.alpha_func, g_pg.alpha_ref,
                        g_pg.stencil_enable, g_pg.stencil_func,
                        g_pg.stencil_ref, g_pg.stencil_mask);
                fprintf(stderr,
                        "  rc control=%08X final=%08X/%08X "
                        "factor=%08X/%08X shader=%08X/%08X dot=%08X\n",
                        g_pg.combiner_control,
                        g_pg.combiner_final_inputs_0,
                        g_pg.combiner_final_inputs_1,
                        g_pg.combiner_final_factor[0],
                        g_pg.combiner_final_factor[1],
                        g_pg.shader_stage_program,
                        g_pg.shader_other_stage_input,
                        g_pg.shader_dot_mapping);
                for (stage = 0u; stage < 4u; ++stage) {
                    fprintf(stderr,
                            "  tex%u enabled=%d off=%08X fmt=%08X "
                            "ctl0=%08X ctl1=%08X rect=%08X\n",
                            stage, g_pg.tex[stage].enabled,
                            g_pg.tex[stage].offset, g_pg.tex[stage].format,
                            g_pg.tex[stage].control0,
                            g_pg.tex[stage].control1,
                            g_pg.tex[stage].image_rect);
                }
                for (combiner_stage = 0u;
                     combiner_stage < (g_pg.combiner_control & 0xFFu) &&
                     combiner_stage < 8u; ++combiner_stage) {
                    fprintf(stderr,
                            "  rc%u rgb=%08X/%08X alpha=%08X/%08X "
                            "factor=%08X/%08X\n",
                            combiner_stage,
                            g_pg.combiner_color_icw[combiner_stage],
                            g_pg.combiner_color_ocw[combiner_stage],
                            g_pg.combiner_alpha_icw[combiner_stage],
                            g_pg.combiner_alpha_ocw[combiner_stage],
                            g_pg.combiner_factor0[combiner_stage],
                            g_pg.combiner_factor1[combiner_stage]);
                }
                for (vertex = 0u; vertex < out_vert_count && vertex < 6u;
                     ++vertex) {
                    fprintf(stderr,
                            "  v%u xy=(%.3f,%.3f) color=%08X "
                            "uv0=(%.3f,%.3f)\n",
                            vertex, out[vertex].x, out[vertex].y,
                            out[vertex].color,
                            out[vertex].u0, out[vertex].v0);
                    if (program_vertices) {
                        const float *a4 = program_vertices[vertex][4];
                        const float *a5 = program_vertices[vertex][5];
                        const float *a6 = program_vertices[vertex][6];
                        const float *a7 = program_vertices[vertex][7];
                        const float *a8 = program_vertices[vertex][8];
                        const float *tc0 = program_vertices[vertex]
                            [NV2A_VERTEX_ATTR_TEXTURE0];
                        const float *tc1 = program_vertices[vertex]
                            [NV2A_VERTEX_ATTR_TEXTURE1];
                        const float *tc2 = program_vertices[vertex]
                            [NV2A_VERTEX_ATTR_TEXTURE2];
                        fprintf(stderr,
                                "    attr4=(%.9g,%.9g,%.9g,%.9g) "
                                "a5=(%.9g,%.9g,%.9g,%.9g) "
                                "a6=(%.9g,%.9g,%.9g,%.9g) "
                                "a7=(%.9g,%.9g,%.9g,%.9g) "
                                "a8=(%.9g,%.9g,%.9g,%.9g)\n"
                                "    attr-t0=(%.9g,%.9g,%.9g,%.9g) "
                                "t1=(%.9g,%.9g,%.9g,%.9g) "
                                "t2=(%.9g,%.9g,%.9g,%.9g)\n",
                                a4[0], a4[1], a4[2], a4[3],
                                a5[0], a5[1], a5[2], a5[3],
                                a6[0], a6[1], a6[2], a6[3],
                                a7[0], a7[1], a7[2], a7[3],
                                a8[0], a8[1], a8[2], a8[3],
                                tc0[0], tc0[1], tc0[2], tc0[3],
                                tc1[0], tc1[1], tc1[2], tc1[3],
                                tc2[0], tc2[1], tc2[2], tc2[3]);
                    }
                }
                fflush(stderr);
            }
        }
    }
    /* Begin scene if needed */
    {
        const int satellite_pass_capture =
            debug_capture_satellite_pass_before();
    trace_texture_edge_candidate("immediate", num_verts);
    trace_point_sprite_candidate("immediate", num_verts);
    trace_line_candidate("immediate", num_verts);
    trace_guest_draw_state();
    debug_capture_gameplay_draw_before("immediate", num_verts, 0u,
                                       src, g_pg.vert_stride * 4u,
                                       use_immediate_combiners);
    debug_capture_satellite_draw_before("immediate");
    dev->lpVtbl->BeginScene(dev);

    /* Draw */
    dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)actual_prim_type,
                                  prim_count, immediate_draw_data,
                                  immediate_draw_stride);
    finish_native_haze(native_haze_slot);
    debug_capture_satellite_pass_after(satellite_pass_capture);
    debug_capture_satellite_draw_after("immediate", 0u, num_verts, 0u,
                                       immediate_draw_stride,
                                       immediate_draw_data,
                                       immediate_draw_stride, num_verts);
    }
    debug_capture_gameplay_draw_after("immediate");
    {
        const char *capture_after_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_DRAW_TARGET_AFTER_PREFIX");
        const char *capture_after_target = pgraph_cached_getenv(
            "MERCENARIES_TRACE_DRAW_TARGET");
        const char *capture_after_source = pgraph_cached_getenv(
            "MERCENARIES_TRACE_DRAW_SOURCE");
        const char *capture_after_min_env = pgraph_cached_getenv(
            "MERCENARIES_TRACE_DRAW_MIN");
        const uint32_t capture_after_min = capture_after_min_env ?
            (uint32_t)strtoul(capture_after_min_env, NULL, 10) : 0u;
        static uint32_t captured_after_draws;
        if (capture_after_prefix && *capture_after_prefix &&
            g_pg.stats.draw_calls >= capture_after_min &&
            captured_after_draws < 16u && g_pg.bound_color &&
            (!capture_after_target || g_pg.surface_color_offset ==
                ((uint32_t)strtoul(capture_after_target, NULL, 16) &
                 0x07FFFFFFu)) &&
            (!capture_after_source || resolved_texture_offset(0u) ==
                ((uint32_t)strtoul(capture_after_source, NULL, 16) &
                 0x07FFFFFFu))) {
            char capture_after_path[MAX_PATH];
            snprintf(capture_after_path, sizeof(capture_after_path),
                     "%s-%u-target-%08X.bmp", capture_after_prefix,
                     g_pg.stats.draw_calls, g_pg.bound_color->offset);
            d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture,
                                           capture_after_path);
            ++captured_after_draws;
        }
    }
    if (pgraph_cached_getenv(
            "MERCENARIES_TRACE_HOST_BLEND_IMMEDIATE") != NULL &&
        g_debug_gameplay_draw_series_active)
        d3d8_states_debug_trace_blend("immediate");
    /* Opt-in watch for the reported screen-filling flare flashes.  Keep this
     * trace independent of the ordinary 32-draw capture budget so a long route
     * can wait for the first geometrically abnormal composite. */
    if (use_flare_combiners) {
        const char *suspicious_text = pgraph_cached_getenv(
            "MERCENARIES_TRACE_SUSPICIOUS_FLARE_BOUNDS" );
        if (suspicious_text && *suspicious_text) {
            const float suspicious_limit = (float)strtod(suspicious_text, NULL);
            const char *suspicious_gate = pgraph_cached_getenv(
                "MERCENARIES_TRACE_SUSPICIOUS_FLARE_GATE_FILE" );
            uint32_t suspicious_vertex;
            float suspicious_min_x = INFINITY;
            float suspicious_min_y = INFINITY;
            float suspicious_max_x = -INFINITY;
            float suspicious_max_y = -INFINITY;
            float projected_min_x = INFINITY;
            float projected_min_y = INFINITY;
            float projected_max_x = -INFINITY;
            float projected_max_y = -INFINITY;
            float projected_min_w = INFINITY;
            float projected_max_w = -INFINITY;
            const float logical_width = g_pg.bound_color &&
                g_pg.bound_color->logical_width ?
                    (float)g_pg.bound_color->logical_width : 640.0f;
            const float logical_height = g_pg.bound_color &&
                g_pg.bound_color->logical_height ?
                    (float)g_pg.bound_color->logical_height : 480.0f;
            int projected_valid;
            int projected_suspicious;
            static uint32_t suspicious_count;
            for (suspicious_vertex = 0u;
                 suspicious_vertex < num_verts && suspicious_vertex < 64u;
                 ++suspicious_vertex) {
                const float *position =
                    g_pg.immediate_vertices[suspicious_vertex][0];
                suspicious_min_x = fminf(suspicious_min_x, position[0]);
                suspicious_min_y = fminf(suspicious_min_y, position[1]);
                suspicious_max_x = fmaxf(suspicious_max_x, position[0]);
                suspicious_max_y = fmaxf(suspicious_max_y, position[1]);
            }
            projected_valid = debug_project_br3d_flare_bounds(
                num_verts < 64u ? num_verts : 64u,
                &projected_min_x, &projected_min_y,
                &projected_max_x, &projected_max_y,
                &projected_min_w, &projected_max_w);
            projected_suspicious = projected_valid &&
                (projected_max_x - projected_min_x > logical_width * 0.8f ||
                 projected_max_y - projected_min_y > logical_height * 0.8f ||
                 projected_min_w <= 0.0f ||
                 projected_min_x < -logical_width * 2.0f ||
                 projected_max_x > logical_width * 3.0f ||
                 projected_min_y < -logical_height * 2.0f ||
                 projected_max_y > logical_height * 3.0f);
            if (suspicious_count < 32u && suspicious_limit > 0.0f &&
                (!suspicious_gate || !*suspicious_gate ||
                 GetFileAttributesA(suspicious_gate) != INVALID_FILE_ATTRIBUTES) &&
                (!isfinite(suspicious_min_x) || !isfinite(suspicious_min_y) ||
                 !isfinite(suspicious_max_x) || !isfinite(suspicious_max_y) ||
                 suspicious_max_x - suspicious_min_x > suspicious_limit ||
                 suspicious_max_y - suspicious_min_y > suspicious_limit ||
                 projected_suspicious)) {
                const char *suspicious_prefix = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_SUSPICIOUS_FLARE_PREFIX" );
                fprintf(stderr,
                        "[FLARE-SUSPICIOUS] n=%u draw=%u vertices=%u "
                        "raw=(%.9g,%.9g)..(%.9g,%.9g) span=(%.9g,%.9g) "
                        "projected=%d/(%.9g,%.9g)..(%.9g,%.9g) "
                        "pspan=(%.9g,%.9g) w=(%.9g,%.9g) logical=%.0fx%.0f "
                        "target=%08X source=%08X vsh=%08X\\n",
                        suspicious_count, g_pg.stats.draw_calls, out_vert_count,
                        suspicious_min_x, suspicious_min_y,
                        suspicious_max_x, suspicious_max_y,
                        suspicious_max_x - suspicious_min_x,
                        suspicious_max_y - suspicious_min_y,
                        projected_valid,
                        projected_min_x, projected_min_y,
                        projected_max_x, projected_max_y,
                        projected_max_x - projected_min_x,
                        projected_max_y - projected_min_y,
                        projected_min_w, projected_max_w,
                        logical_width, logical_height,
                        g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                        immediate_source_surface ?
                            immediate_source_surface->offset : 0u,
                        g_pg.host_vsh_hash);
                if (suspicious_prefix && *suspicious_prefix &&
                    g_pg.bound_color && g_pg.bound_color->texture) {
                    char suspicious_path[MAX_PATH];
                    snprintf(suspicious_path, sizeof(suspicious_path),
                             "%s-%02u-target-%08X.bmp", suspicious_prefix,
                             suspicious_count, g_pg.bound_color->offset);
                    d3d8_DebugCaptureTextureToPath(
                        g_pg.bound_color->texture, suspicious_path);
                }
                ++suspicious_count;
                fflush(stderr);
            }
        }
    }
    /* Bounded, opt-in evidence for the final additive flare composite.  The
     * probe capture below proves the visibility mask; this companion capture
     * proves which mask resource and register combiner the final draw used. */
    if (use_flare_combiners) {
        static uint32_t captured_flare_composites;
        const char *composite_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_FLARE_COMPOSITE_PREFIX");
        const char *composite_gate = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_FLARE_COMPOSITE_GATE_FILE");
        if (composite_prefix && *composite_prefix &&
            captured_flare_composites < 32u && g_pg.bound_color &&
            (!composite_gate || !*composite_gate ||
             GetFileAttributesA(composite_gate) != INVALID_FILE_ATTRIBUTES)) {
            char composite_path[MAX_PATH];
            uint32_t composite_vertex;
            float composite_min_x = INFINITY;
            float composite_min_y = INFINITY;
            float composite_max_x = -INFINITY;
            float composite_max_y = -INFINITY;
            for (composite_vertex = 0u;
                 composite_vertex < num_verts && composite_vertex < 64u;
                 ++composite_vertex) {
                const float *position =
                    g_pg.immediate_vertices[composite_vertex][0];
                composite_min_x = fminf(composite_min_x, position[0]);
                composite_min_y = fminf(composite_min_y, position[1]);
                composite_max_x = fmaxf(composite_max_x, position[0]);
                composite_max_y = fmaxf(composite_max_y, position[1]);
            }
            fprintf(stderr,
                    "[FLARE-COMPOSITE] n=%u draw=%u target=%08X "
                    "source=%08X combiner=%d vsh=%08X vertices=%u "
                    "uv=(%.6g,%.6g) pos=(%.6g,%.6g,%.6g,%.6g) "
                    "bounds=(%.6g,%.6g)..(%.6g,%.6g)\n",
                    captured_flare_composites, g_pg.stats.draw_calls,
                    g_pg.bound_color->offset,
                    immediate_source_surface ? immediate_source_surface->offset : 0u,
                    use_immediate_combiners, g_pg.host_vsh_hash,
                    out_vert_count,
                    g_pg.immediate_vertices[0][4][0],
                    g_pg.immediate_vertices[0][4][1],
                    g_pg.immediate_vertices[0][0][0],
                    g_pg.immediate_vertices[0][0][1],
                    g_pg.immediate_vertices[0][0][2],
                    g_pg.immediate_vertices[0][0][3],
                    composite_min_x, composite_min_y,
                    composite_max_x, composite_max_y);
            debug_trace_flare_sample_source(composite_prefix,
                                            captured_flare_composites,
                                            use_immediate_combiners ? &combiner_state : NULL);
            snprintf(composite_path, sizeof(composite_path),
                     "%s-%02u-target-%08X.bmp", composite_prefix,
                     captured_flare_composites, g_pg.bound_color->offset);
            d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture,
                                           composite_path);
            ++captured_flare_composites;
        }
    }
    /* Read-only, bounded captures of the retail flare clear/probe/downsample
     * passes. Final-composite screenshots cannot tell whether visibility was
     * lost during depth testing or while packing the 2x2 alpha samples. */
    if (use_flare_probe_vsh) {
        static uint32_t captured_flare_probe_passes;
        const char *probe_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX");
        const char *probe_min_text = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_DRAW_MIN");
        const uint32_t probe_min = probe_min_text ?
            (uint32_t)strtoul(probe_min_text, NULL, 10) : 0u;
        const char *probe_gate = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_FLARE_PROBE_GATE_FILE");
        if (probe_prefix && *probe_prefix && captured_flare_probe_passes < 128u &&
            g_pg.stats.draw_calls >= probe_min && g_pg.bound_color &&
            (!probe_gate || GetFileAttributesA(probe_gate) != INVALID_FILE_ATTRIBUTES) &&
            g_pg.bound_color->texture) {
            char probe_path[MAX_PATH];
            uint32_t probe_vertex;
            snprintf(probe_path, sizeof(probe_path), "%s-%02u-%08X.bmp",
                     probe_prefix, captured_flare_probe_passes,
                     g_pg.bound_color->offset);
            fprintf(stderr,
                    "[FLARE-PROBE-PASS] n=%u draw=%u target=%08X size=%ux%u "
                    "zeta=%08X depth=%d/%d/%u mask=%08X cull=%d/%08X/%08X "
                    "vsh=%08X combiner=%d tex0=%08X/%08X rect=%08X pitch=%u "
                    "factors=%08X/%08X\n",
                    captured_flare_probe_passes, g_pg.stats.draw_calls,
                    g_pg.bound_color->offset, g_pg.bound_color->width,
                    g_pg.bound_color->height, g_pg.surface_zeta_offset,
                    g_pg.depth_test, g_pg.depth_write, g_pg.depth_func,
                    g_pg.color_mask, g_pg.cull_enable, g_pg.cull_face,
                    g_pg.front_face, immediate_draw_shader,
                    use_immediate_combiners, resolved_texture_offset(0),
                    g_pg.tex[0].format, g_pg.tex[0].image_rect,
                    g_pg.tex[0].control1 >> 16, g_pg.combiner_factor0[0],
                    g_pg.combiner_factor1[0]);
            for (probe_vertex = 0u; probe_vertex < num_verts && probe_vertex < 64u;
                 ++probe_vertex) {
                const float *position = g_pg.immediate_vertices[probe_vertex][0];
                const float *uv = g_pg.immediate_vertices[probe_vertex][4];
                fprintf(stderr, "  probe-v%u xyzw=%.9g,%.9g,%.9g,%.9g uv=%.9g,%.9g,%.9g,%.9g\n",
                        probe_vertex, position[0], position[1], position[2],
                        position[3], uv[0], uv[1], uv[2], uv[3]);
            }
            d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, probe_path);
            debug_trace_flare_probe_depth(captured_flare_probe_passes, num_verts);
            debug_trace_flare_sample_source(probe_prefix, captured_flare_probe_passes,
                                            use_immediate_combiners ? &combiner_state : NULL);
            ++captured_flare_probe_passes;
            fflush(stderr);
        }
    }
    {
        const char *capture_glow = pgraph_cached_getenv("MERCENARIES_CAPTURE_GLOW_STAGES");
        const char *capture_prefix = pgraph_cached_getenv("MERCENARIES_CAPTURE_GLOW_PREFIX");
        static uint32_t captured_glow_stages;
        /* Full-screen surface filters only. Flare visibility reductions reuse
         * these surfaces but occupy tiny rectangles and are a separate pass. */
        const int surface_filter = capture_glow && capture_prefix &&
            immediate_source_surface &&
            !g_pg.depth_test && g_pg.bound_color &&
            num_verts == 4u &&
            fabsf(g_pg.immediate_vertices[2][0][0] - g_pg.immediate_vertices[0][0][0]) >=
                (float)(g_pg.surface_clip_h >> 16) * 0.9f &&
            fabsf(g_pg.immediate_vertices[2][0][1] - g_pg.immediate_vertices[0][0][1]) >=
                (float)(g_pg.surface_clip_v >> 16) * 0.9f &&
            (g_pg.tex[1].enabled || g_pg.tex[2].enabled ||
             (g_pg.blend_enable && g_pg.blend_sfactor == 1u && g_pg.blend_dfactor == 1u));
        /* Default remains the five glow draws. An explicit diagnostic can
         * include the following palette pass; cap repeated-frame captures. */
        const char *capture_count_text = surface_filter ?
            pgraph_cached_getenv("MERCENARIES_CAPTURE_GLOW_COUNT") : NULL;
        unsigned capture_count = capture_count_text ?
            (unsigned)strtoul(capture_count_text, NULL, 10) : 5u;
        if (capture_count > 12u) capture_count = 12u;
        if (capture_glow && capture_prefix && surface_filter && captured_glow_stages < capture_count) {
            char path[1024];
            uint32_t v;
            fprintf(stderr,
                    "[PGRAPH-GLOW-CAPTURE] stage=%u draw=%u target=%08X source=%08X "
                    "size=%ux%u blend=%d/%04X/%04X factors=%08X/%08X\n",
                    captured_glow_stages, g_pg.stats.draw_calls, g_pg.bound_color->offset,
                    resolved_texture_offset(0), g_pg.bound_color->width, g_pg.bound_color->height,
                    g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
                    g_pg.combiner_factor0[0], g_pg.combiner_factor1[0]);
            for (v = 0; v < num_verts; ++v) {
                const float (*attr)[4] = g_pg.immediate_vertices[v];
                fprintf(stderr, "  glow-v%u xy=%g,%g uv0=%g,%g uv1=%g,%g uv2=%g,%g\n",
                        v, attr[0][0], attr[0][1], attr[4][0], attr[4][1],
                        attr[5][0], attr[5][1], attr[6][0], attr[6][1]);
            }
            snprintf(path, sizeof(path), "%s-%02u-target.bmp", capture_prefix, captured_glow_stages);
            d3d8_DebugCaptureTextureToPath(g_pg.bound_color->texture, path);
            debug_trace_flare_sample_source(capture_prefix, captured_glow_stages,
                                           use_immediate_combiners ? &combiner_state : NULL);
            ++captured_glow_stages;
        }
    }
    {
        uint32_t feedback_stage;
        for (feedback_stage = 0u; feedback_stage < 4u; ++feedback_stage) {
            if (feedback_srvs[feedback_stage]) {
                d3d8_BindExternalTexture(feedback_stage, NULL);
                ID3D11ShaderResourceView_Release(
                    feedback_srvs[feedback_stage]);
            }
            if (feedback_textures[feedback_stage])
                ID3D11Texture2D_Release(feedback_textures[feedback_stage]);
        }
    }
    g_pg.frame_had_draw = 1;
    if (g_pg.bound_color) {
        g_pg.bound_color->drawn = 1;
        g_pg.bound_color->gpu_drawn = 1;
        g_pg.bound_color->draw_generation = g_pg.surface_generation;
        g_pg.bound_color->write_serial = ++g_pg.surface_write_serial;
        debug_capture_post_pda_pending_write("immediate");
        if (g_pg.pending_scanout == g_pg.bound_color)
            g_pg.pending_scanout_write_us = pgraph_scanout_clock_us();
        if (depth_alias_source) {
            g_pg.bound_color->depth_alias_source_offset =
                depth_alias_source->offset;
            g_pg.bound_color->depth_alias_source_serial =
                depth_alias_source->write_serial;
        } else {
            /* See the array path: ordinary color replacement consumes the
             * packed-depth identity, while AA-resolve history remains valid
             * across this partial overlay. */
            g_pg.bound_color->depth_alias_source_offset = 0u;
            g_pg.bound_color->depth_alias_source_serial = 0u;
        }
        /* Immediate overlays use the same provenance lifetime as arrays. */
        g_pg.last_drawn_color = g_pg.bound_color;
    }
    if (g_pg.bound_depth && g_pg.depth_test && g_pg.depth_write &&
        pgraph_cached_getenv("MERCENARIES_DEBUG_NO_DEPTH") == NULL)
        g_pg.bound_depth->write_serial = ++g_pg.surface_write_serial;
    note_bound_depth_stencil_draw();
    {
        const char *capture_target = pgraph_cached_getenv("MERCENARIES_CAPTURE_DRAW_TARGET");
        const char *capture_min_env = pgraph_cached_getenv("MERCENARIES_CAPTURE_DRAW_MIN");
        const uint32_t capture_min = capture_min_env ?
            (uint32_t)strtoul(capture_min_env, NULL, 10) : 0u;
        const char *capture_count_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_DRAW_COUNT");
        const uint32_t capture_count = capture_count_env ?
            (uint32_t)strtoul(capture_count_env, NULL, 10) : 1u;
        static uint32_t captured_target;
        float capture_min_x = out[0].x, capture_max_x = out[0].x;
        float capture_min_y = out[0].y, capture_max_y = out[0].y;
        uint32_t capture_vertex;
        for (capture_vertex = 1u; capture_vertex < out_vert_count;
             ++capture_vertex) {
            if (out[capture_vertex].x < capture_min_x)
                capture_min_x = out[capture_vertex].x;
            if (out[capture_vertex].x > capture_max_x)
                capture_max_x = out[capture_vertex].x;
            if (out[capture_vertex].y < capture_min_y)
                capture_min_y = out[capture_vertex].y;
            if (out[capture_vertex].y > capture_max_y)
                capture_max_y = out[capture_vertex].y;
        }
        if (captured_target < capture_count && capture_target &&
            g_pg.stats.draw_calls >= capture_min && g_pg.bound_color &&
            g_pg.bound_color->offset ==
                ((uint32_t)strtoul(capture_target, NULL, 16) & 0x07FFFFFFu) &&
            capture_max_x - capture_min_x >=
                (float)g_pg.bound_color->width * 0.9f &&
            capture_max_y - capture_min_y >=
                (float)g_pg.bound_color->height * 0.9f &&
            d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                         g_pg.bound_color->srv,
                                         g_pg.bound_color->width,
                                         g_pg.bound_color->height)) {
            fprintf(stderr,
                    "[PGRAPH-DRAW-TARGET-CAPTURE] ordinal=%u draw=%u "
                    "target=%08X source=%08X\n",
                    captured_target, g_pg.stats.draw_calls,
                    g_pg.bound_color->offset,
                    resolved_texture_offset(0));
            d3d8_DebugCaptureFrameNow();
            ++captured_target;
        }
    }
    note_guest_surface_resolve("immediate");

    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += num_verts;

    if (pgraph_trace_draws_enabled() &&
        (g_pg.stats.draw_calls <= 5 ||
         (g_pg.stats.draw_calls % 1000) == 0)) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw #%u: %u verts, prim=%d, prims=%u\n",
                g_pg.stats.draw_calls, num_verts, g_pg.d3d_prim_type, prim_count);
    }
}

static void commit_immediate_vertex(void)
{
    uint32_t *dst;

    if (!g_pg.in_draw ||
        g_pg.inline_count + INLINE_VERT_DWORDS >
            MAX_INLINE_VERTS * INLINE_VERT_DWORDS)
        return;

    g_pg.inline_array_mode = 0;
    dst = &g_pg.inline_data[g_pg.inline_count];
    dst[0] = f2u(g_pg.immediate_vertex_attr[NV2A_VERTEX_ATTR_POSITION][0]);
    dst[1] = f2u(g_pg.immediate_vertex_attr[NV2A_VERTEX_ATTR_POSITION][1]);
    for (uint32_t stage = 0; stage < 4u; ++stage) {
        const uint32_t attr = g_pg.immediate_texcoord_base + stage;
        dst[2u + stage * 2u] = f2u(g_pg.immediate_vertex_attr[attr][0]);
        dst[3u + stage * 2u] = f2u(g_pg.immediate_vertex_attr[attr][1]);
    }
    dst[10] = g_pg.immediate_vertex_4ub[NV2A_VERTEX_ATTR_DIFFUSE];
    memcpy(g_pg.immediate_vertices[g_pg.immediate_vertex_count],
           g_pg.immediate_vertex_attr,
           sizeof(g_pg.immediate_vertex_attr));
    ++g_pg.immediate_vertex_count;
    g_pg.inline_count += INLINE_VERT_DWORDS;
}

/* ======================================================================
 * Method Handler
 * ====================================================================== */

/* After the first scalar method has selected the context and handled any
 * pending DRAW_ARRAYS, consecutive non-incrementing data words only append to
 * these buffers. They cannot submit a draw or alter other GPU state. */
uint32_t pgraph_d3d11_inline_batch(uint32_t method, const uint32_t *words,
                                  uint32_t count)
{
    uint32_t copied;
    if (!g_pg.initialized || !words || !count ||
        (method != NV097_ARRAY_ELEMENT16 && method != NV097_ARRAY_ELEMENT32 &&
         method != NV097_INLINE_ARRAY))
        return 0u;
    if (g_pg.in_draw && g_pg.draw_array_count != 0u &&
        method != NV097_INLINE_ARRAY)
        return 0u;
    g_pg.stats.methods_handled += count;
    if (!g_pg.in_draw)
        return count;
    if (method == NV097_INLINE_ARRAY) {
        copied = MAX_INLINE_VERTS * INLINE_VERT_DWORDS - g_pg.inline_count;
        if (copied > count) copied = count;
        if (copied) {
            memcpy(g_pg.inline_data + g_pg.inline_count, words,
                   (size_t)copied * sizeof(uint32_t));
            g_pg.inline_count += copied;
            g_pg.inline_array_mode = 1;
        }
    } else if (method == NV097_ARRAY_ELEMENT32) {
        copied = MAX_INLINE_ELEMENTS - g_pg.inline_element_count;
        if (copied > count) copied = count;
        memcpy(g_pg.inline_elements + g_pg.inline_element_count, words,
               (size_t)copied * sizeof(uint32_t));
        g_pg.inline_element_count += copied;
    } else {
        copied = (MAX_INLINE_ELEMENTS - g_pg.inline_element_count) / 2u;
        if (copied > count) copied = count;
        for (uint32_t i = 0u; i < copied; ++i) {
            g_pg.inline_elements[g_pg.inline_element_count++] = words[i] & 0xFFFFu;
            g_pg.inline_elements[g_pg.inline_element_count++] = words[i] >> 16;
        }
    }
    /* Scalar handlers also consume words that exceed the buffer capacity. */
    return count;
}

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    if (!g_pg.initialized)
        return 0;

    g_pg.stats.methods_handled++;

    /* NV097_SET_POINT_PARAMS is an eight-float incrementing range.  Keep
     * the complete Xbox attenuation state, matching Xemu, even before the
     * host point-sprite geometry stage consumes it. */
    if (method >= NV097_SET_POINT_PARAMS &&
        method < NV097_SET_POINT_PARAMS + 8u * 4u) {
        g_pg.point_params[(method - NV097_SET_POINT_PARAMS) / 4u] = param;
        return 1;
    }

    /* xemu defines both combiner-factor methods as eight-word incrementing
     * ranges, and SET_VERTEX_DATA4UB as a sixteen-slot immediate range. */
    /* Match Xemu's incrementing NV097 combiner register ranges. */
    if (method >= NV097_SET_COMBINER_ALPHA_ICW &&
        method < NV097_SET_COMBINER_ALPHA_ICW + 8u * 4u) {
        g_pg.combiner_alpha_icw[
            (method - NV097_SET_COMBINER_ALPHA_ICW) / 4u] = param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_COMBINER_ALPHA_OCW &&
        method < NV097_SET_COMBINER_ALPHA_OCW + 8u * 4u) {
        g_pg.combiner_alpha_ocw[
            (method - NV097_SET_COMBINER_ALPHA_OCW) / 4u] = param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_COMBINER_COLOR_ICW &&
        method < NV097_SET_COMBINER_COLOR_ICW + 8u * 4u) {
        g_pg.combiner_color_icw[
            (method - NV097_SET_COMBINER_COLOR_ICW) / 4u] = param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_COMBINER_COLOR_OCW &&
        method < NV097_SET_COMBINER_COLOR_OCW + 8u * 4u) {
        g_pg.combiner_color_ocw[
            (method - NV097_SET_COMBINER_COLOR_OCW) / 4u] = param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_SPECULAR_FOG_FACTOR &&
        method < NV097_SET_SPECULAR_FOG_FACTOR + 2u * 4u) {
        g_pg.combiner_final_factor[
            (method - NV097_SET_SPECULAR_FOG_FACTOR) / 4u] = param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_COMBINER_FACTOR0 &&
        method < NV097_SET_COMBINER_FACTOR0 + 8u * 4u) {
        g_pg.combiner_factor0[(method - NV097_SET_COMBINER_FACTOR0) / 4u] =
            param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_COMBINER_FACTOR1 &&
        method < NV097_SET_COMBINER_FACTOR1 + 8u * 4u) {
        g_pg.combiner_factor1[(method - NV097_SET_COMBINER_FACTOR1) / 4u] =
            param;
        g_pg.combiner_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_VERTEX4F &&
        method < NV097_SET_VERTEX4F + 4u * 4u) {
        const uint32_t component = (method - NV097_SET_VERTEX4F) / 4u;
        g_pg.immediate_vertex_attr[NV2A_VERTEX_ATTR_POSITION][component] =
            u2f(param);
        if (component == 3u)
            commit_immediate_vertex();
        return 1;
    }
    if (method >= NV097_SET_TEXCOORD0_2F &&
        method < NV097_SET_TEXCOORD0_2F + 2u * 4u) {
        const uint32_t component = (method - NV097_SET_TEXCOORD0_2F) / 4u;
        float *texcoord =
            g_pg.immediate_vertex_attr[NV2A_VERTEX_ATTR_TEXTURE0];
        g_pg.immediate_texcoord_base = NV2A_VERTEX_ATTR_TEXTURE0;
        texcoord[component] = u2f(param);
        texcoord[2] = 0.0f;
        texcoord[3] = 1.0f;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M &&
        method < NV097_SET_VERTEX_DATA2F_M + 16u * 2u * 4u) {
        const uint32_t item = (method - NV097_SET_VERTEX_DATA2F_M) / 4u;
        const uint32_t attr = item / 2u;
        const uint32_t component = item & 1u;
        float *value = g_pg.immediate_vertex_attr[attr];
        if (attr >= 4u && attr <= 7u)
            g_pg.immediate_texcoord_base = 4u;
        else if (attr >= NV2A_VERTEX_ATTR_TEXTURE0 &&
                 attr <= NV2A_VERTEX_ATTR_TEXTURE3)
            g_pg.immediate_texcoord_base = NV2A_VERTEX_ATTR_TEXTURE0;
        value[component] = u2f(param);
        value[2] = 0.0f;
        value[3] = 1.0f;
        if (attr == NV2A_VERTEX_ATTR_POSITION && component == 1u)
            commit_immediate_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S &&
        method < NV097_SET_VERTEX_DATA2S + 16u * 4u) {
        const uint32_t attr = (method - NV097_SET_VERTEX_DATA2S) / 4u;
        float *value = g_pg.immediate_vertex_attr[attr];
        if (attr >= 4u && attr <= 7u)
            g_pg.immediate_texcoord_base = 4u;
        else if (attr >= NV2A_VERTEX_ATTR_TEXTURE0 &&
                 attr <= NV2A_VERTEX_ATTR_TEXTURE3)
            g_pg.immediate_texcoord_base = NV2A_VERTEX_ATTR_TEXTURE0;
        value[0] = (float)(int16_t)(param & 0xFFFFu);
        value[1] = (float)(int16_t)(param >> 16);
        value[2] = 0.0f;
        value[3] = 1.0f;
        if (attr == NV2A_VERTEX_ATTR_POSITION)
            commit_immediate_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M &&
        method < NV097_SET_VERTEX_DATA4S_M + 16u * 2u * 4u) {
        const uint32_t item = (method - NV097_SET_VERTEX_DATA4S_M) / 4u;
        const uint32_t attr = item / 2u;
        const uint32_t part = item & 1u;
        float *value = g_pg.immediate_vertex_attr[attr];
        if (attr >= 4u && attr <= 7u)
            g_pg.immediate_texcoord_base = 4u;
        else if (attr >= NV2A_VERTEX_ATTR_TEXTURE0 &&
                 attr <= NV2A_VERTEX_ATTR_TEXTURE3)
            g_pg.immediate_texcoord_base = NV2A_VERTEX_ATTR_TEXTURE0;
        value[part * 2u] = (float)(int16_t)(param & 0xFFFFu);
        value[part * 2u + 1u] = (float)(int16_t)(param >> 16);
        if (attr == NV2A_VERTEX_ATTR_POSITION && part == 1u)
            commit_immediate_vertex();
        return 1;
    }    if (method >= NV097_SET_VERTEX_DATA4F_M &&
        method < NV097_SET_VERTEX_DATA4F_M + 16u * 4u * 4u) {
        const uint32_t item = (method - NV097_SET_VERTEX_DATA4F_M) / 4u;
        const uint32_t attr = item / 4u;
        const uint32_t component = item & 3u;
        if (attr >= 4u && attr <= 7u)
            g_pg.immediate_texcoord_base = 4u;
        else if (attr >= NV2A_VERTEX_ATTR_TEXTURE0 &&
                 attr <= NV2A_VERTEX_ATTR_TEXTURE3)
            g_pg.immediate_texcoord_base = NV2A_VERTEX_ATTR_TEXTURE0;
        g_pg.immediate_vertex_attr[attr][component] = u2f(param);
        if (attr == NV2A_VERTEX_ATTR_POSITION && component == 3u)
            commit_immediate_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB &&
        method < NV097_SET_VERTEX_DATA4UB + 16u * 4u) {
        const uint32_t attr = (method - NV097_SET_VERTEX_DATA4UB) / 4u;
        g_pg.immediate_vertex_4ub[attr] = param;
        g_pg.immediate_vertex_attr[attr][0] =
            (float)(param & 0xFFu) / 255.0f;
        g_pg.immediate_vertex_attr[attr][1] =
            (float)((param >> 8) & 0xFFu) / 255.0f;
        g_pg.immediate_vertex_attr[attr][2] =
            (float)((param >> 16) & 0xFFu) / 255.0f;
        g_pg.immediate_vertex_attr[attr][3] =
            (float)((param >> 24) & 0xFFu) / 255.0f;
        return 1;
    }
    if (method >= NV097_SET_WINDOW_CLIP_HORIZONTAL &&
        method < NV097_SET_WINDOW_CLIP_HORIZONTAL + 8u * 4u) {
        uint32_t slot = (method - NV097_SET_WINDOW_CLIP_HORIZONTAL) / 4u;
        for (; slot < 8u; ++slot)
            g_pg.window_clip_h[slot] = param;
        g_pg.window_clip_h_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_WINDOW_CLIP_VERTICAL &&
        method < NV097_SET_WINDOW_CLIP_VERTICAL + 8u * 4u) {
        uint32_t slot = (method - NV097_SET_WINDOW_CLIP_VERTICAL) / 4u;
        for (; slot < 8u; ++slot)
            g_pg.window_clip_v[slot] = param;
        g_pg.window_clip_v_valid = 1;
        return 1;
    }
    if (method >= NV097_SET_EYE_VECTOR &&
        method < NV097_SET_EYE_VECTOR + 3u * 4u) {
        g_pg.eye_vector[(method - NV097_SET_EYE_VECTOR) / 4u] = param;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET &&
        method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 16u * 4u) {
        const uint32_t slot =
            (method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4u;
        g_pg.vertex_array[slot].dma_select = param >> 31;
        g_pg.vertex_array[slot].offset = param & 0x7FFFFFFFu;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_VERTEX_ARRAY_METHODS") != NULL &&
            g_pg.stats.draw_calls >= 100000u) {
            fprintf(stderr,
                    "[PGRAPH-ARRAY-METHOD] draw=%u sub=%d OFFSET[%u]="
                    "%08X raw=%08X\n",
                    g_pg.stats.draw_calls, subchannel, slot,
                    g_pg.vertex_array[slot].offset, param);
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT &&
        method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 16u * 4u) {
        const uint32_t slot =
            (method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4u;
        g_pg.vertex_array[slot].format = param;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_VERTEX_ARRAY_METHODS") != NULL &&
            g_pg.stats.draw_calls >= 100000u) {
            fprintf(stderr,
                    "[PGRAPH-ARRAY-METHOD] draw=%u sub=%d FORMAT[%u]="
                    "%08X type=%u size=%u stride=%u\n",
                    g_pg.stats.draw_calls, subchannel, slot, param,
                    param & 0xFu, (param >> 4) & 0xFu, param >> 8);
        }
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_PROGRAM &&
        method < NV097_SET_TRANSFORM_PROGRAM + 32u * 4u) {
        const uint32_t slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4u;
        const uint32_t instruction = g_pg.transform_program_load;
        const uint32_t component = slot & 3u;

        if (instruction >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH)
            return 0;
        if (g_pg.transform_program[instruction][component] != param)
            g_pg.transform_program_key_valid = 0;
        g_pg.transform_program[instruction][component] = param;
        if (component == 3u)
            g_pg.transform_program_load = instruction + 1u;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT &&
        method < NV097_SET_TRANSFORM_CONSTANT + 32u * 4u) {
        const uint32_t slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4u;
        const uint32_t constant = g_pg.transform_constant_load;
        const uint32_t component = slot & 3u;
        const uint32_t history_serial =
            ++g_pg.transform_constant_history_serial;
        const uint32_t history_index = history_serial & 63u;

        g_pg.transform_constant_history[history_index].serial = history_serial;
        g_pg.transform_constant_history[history_index].draw =
            g_pg.stats.draw_calls;
        g_pg.transform_constant_history[history_index].method = method;
        g_pg.transform_constant_history[history_index].parameter = param;
        g_pg.transform_constant_history[history_index].load_before = constant;
        g_pg.transform_constant_history[history_index].constant = constant;
        g_pg.transform_constant_history[history_index].component = component;
        g_pg.transform_constant_history[history_index].is_load = 0u;

        if (constant >= NV2A_VERTEXSHADER_CONSTANTS)
            return 0;
        g_pg.vsh_constants_dirty[constant] |=
            g_pg.vsh_constants[constant][component] != param;
        g_pg.vsh_constants[constant][component] = param;
        if ((pgraph_cached_getenv("MERCENARIES_TRACE_VSH_CONSTANTS") != NULL &&
             constant >= 16u && constant <= 42u) ||
            (pgraph_cached_getenv("MERCENARIES_TRACE_SPLASH_VSH") != NULL &&
             constant <= 3u && g_pg.stats.draw_calls < 8u)) {
            fprintf(stderr,
                    "[PGRAPH-VSH-C] draw=%u c%u.%c=%08X (%.9g) method=%04X\n",
                    g_pg.stats.draw_calls, constant, "xyzw"[component], param,
                    u2f(param), method);
        }
        if (component == 3u)
            g_pg.transform_constant_load = constant + 1u;
        return 1;
    }

    switch (method) {

    /* -- Register combiner / texture shader state -- */
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0:
        g_pg.combiner_final_inputs_0 = param;
        g_pg.combiner_valid = 1;
        return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1:
        g_pg.combiner_final_inputs_1 = param;
        g_pg.combiner_valid = 1;
        return 1;
    case NV097_SET_COMBINER_CONTROL:
        g_pg.combiner_control = param;
        g_pg.combiner_valid = 1;
        return 1;
    case NV097_SET_SHADER_STAGE_PROGRAM:
        g_pg.shader_stage_program = param;
        g_pg.combiner_valid = 1;
        return 1;
    case NV097_SET_DOT_RGBMAPPING:
        g_pg.shader_dot_mapping = param;
        g_pg.combiner_valid = 1;
        return 1;
    case NV097_SET_SHADER_OTHER_STAGE_INPUT:
        g_pg.shader_other_stage_input = param;
        g_pg.combiner_valid = 1;
        return 1;

    /* -- Draw Begin/End -- */
    case NV097_SET_BEGIN_END:
        if (pgraph_trace_movie_texture_enabled() &&
            pgraph_texture_format_is_packed_yuv(g_pg.tex[0].format)) {
            static uint32_t movie_draw_trace_count;
            if (movie_draw_trace_count++ < 128u) {
                fprintf(stderr,
                        "[PGRAPH-MOVIE-DRAW] sample=%u draw=%llu op=%u "
                        "off=%08X fmt=%08X ctl0=%08X ctl1=%08X "
                        "rect=%08X enabled=%d immediate=%u arrays=%u\n",
                        movie_draw_trace_count,
                        (unsigned long long)g_pg.stats.draw_calls, param,
                        g_pg.tex[0].offset, g_pg.tex[0].format,
                        g_pg.tex[0].control0, g_pg.tex[0].control1,
                        g_pg.tex[0].image_rect, g_pg.tex[0].enabled,
                        g_pg.immediate_vertex_count, g_pg.draw_array_count);
            }
        }
        if (param == 0) {
            /* END: submit accumulated vertices */
            if (g_pg.in_draw) {
                const int trace_end =
                    pgraph_cached_getenv("MERCENARIES_TRACE_END_STALL") != NULL;
                if (trace_end) {
                    fprintf(stderr,
                            "[PGRAPH-END-ENTER] draw=%u mode=%u arrays=%u "
                            "elements=%u inline=%u/%u stride=%u vsh=%08X\n",
                            g_pg.stats.draw_calls, g_pg.draw_mode,
                            g_pg.draw_array_count, g_pg.inline_element_count,
                            g_pg.inline_count, g_pg.inline_array_mode,
                            g_pg.vert_stride, g_pg.host_vsh_hash);
                    fflush(stderr);
                }
                submit_draw();
                if (trace_end) {
                    fprintf(stderr, "[PGRAPH-END-EXIT] draw=%u\n",
                            g_pg.stats.draw_calls);
                    fflush(stderr);
                }
                g_pg.in_draw = 0;
            }
        } else {
            /* BEGIN: start new draw */
            g_pg.in_draw = 1;
            g_pg.draw_mode = param;
            g_pg.d3d_prim_type = nv2a_draw_mode_to_d3d(param);
            g_pg.inline_count = 0;
            g_pg.immediate_vertex_count = 0;
            g_pg.inline_array_mode = 0;
            g_pg.draw_array_start = 0;
            g_pg.draw_array_count = 0;
            g_pg.inline_element_count = 0;
        }
        return 1;

    case NV097_ARRAY_ELEMENT16:
        if (g_pg.in_draw) {
            uint32_t i;
            if (g_pg.draw_array_count != 0u) {
                for (i = 0u; i < g_pg.draw_array_count &&
                     g_pg.inline_element_count < MAX_INLINE_ELEMENTS; ++i) {
                    g_pg.inline_elements[g_pg.inline_element_count++] =
                        g_pg.draw_array_start + i;
                }
                g_pg.draw_array_start = 0u;
                g_pg.draw_array_count = 0u;
            }
            if (g_pg.inline_element_count + 2u <= MAX_INLINE_ELEMENTS) {
                g_pg.inline_elements[g_pg.inline_element_count++] =
                    param & 0xFFFFu;
                g_pg.inline_elements[g_pg.inline_element_count++] =
                    param >> 16;
            }
        }
        return 1;

    case NV097_ARRAY_ELEMENT32:
        if (g_pg.in_draw) {
            uint32_t i;
            if (g_pg.draw_array_count != 0u) {
                for (i = 0u; i < g_pg.draw_array_count &&
                     g_pg.inline_element_count < MAX_INLINE_ELEMENTS; ++i) {
                    g_pg.inline_elements[g_pg.inline_element_count++] =
                        g_pg.draw_array_start + i;
                }
                g_pg.draw_array_start = 0u;
                g_pg.draw_array_count = 0u;
            }
            if (g_pg.inline_element_count < MAX_INLINE_ELEMENTS)
                g_pg.inline_elements[g_pg.inline_element_count++] = param;
        }
        return 1;

    case NV097_DRAW_ARRAYS:
    {
        const uint32_t start = param & 0x00FFFFFFu;
        const uint32_t count = (param >> 24) + 1u;
        const char *trace_base_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_ARRAYS_BASE");
        const char *trace_vsh_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_ARRAYS_VSH");
        const char *trace_min_env =
            pgraph_cached_getenv("MERCENARIES_TRACE_DRAW_ARRAYS_MIN");
        const uint32_t trace_min = trace_min_env ?
            (uint32_t)strtoul(trace_min_env, NULL, 10) : 0u;
        if (g_pg.stats.draw_calls >= trace_min &&
            ((trace_base_env && resolved_vertex_array_offset(0) ==
                ((uint32_t)strtoul(trace_base_env, NULL, 16) & 0x07FFFFFFu)) ||
             (trace_vsh_env && g_pg.host_vsh_hash ==
                (uint32_t)strtoul(trace_vsh_env, NULL, 16)))) {
            fprintf(stderr,
                    "[PGRAPH-DRAW-ARRAYS] draw=%u mode=%u param=%08X "
                    "start=%u count=%u pending=%u/%u elements=%u in=%d\n",
                    g_pg.stats.draw_calls + 1u, g_pg.draw_mode, param,
                    start, count, g_pg.draw_array_start,
                    g_pg.draw_array_count, g_pg.inline_element_count,
                    g_pg.in_draw);
        }
        if (g_pg.in_draw) {
            if (g_pg.inline_element_count != 0u) {
                uint32_t i;
                for (i = 0u; i < count &&
                     g_pg.inline_element_count < MAX_INLINE_ELEMENTS; ++i)
                    g_pg.inline_elements[g_pg.inline_element_count++] =
                        start + i;
            } else if (g_pg.draw_array_count == 0u) {
                g_pg.draw_array_start = start;
                g_pg.draw_array_count = count;
            } else if (start == g_pg.draw_array_start +
                                g_pg.draw_array_count) {
                g_pg.draw_array_count += count;
            } else {
                submit_draw();
                g_pg.draw_array_start = start;
                g_pg.draw_array_count = count;
            }
        }
        return 1;
    }

    /* -- Inline Vertex Data -- */
    case NV097_INLINE_ARRAY:
        if (g_pg.in_draw && g_pg.inline_count < MAX_INLINE_VERTS * INLINE_VERT_DWORDS) {
            g_pg.inline_array_mode = 1;
            g_pg.inline_data[g_pg.inline_count++] = param;
        }
        return 1;

    /* -- Clear -- */
    case NV097_SET_ZSTENCIL_CLEAR_VALUE:
        g_pg.zstencil_clear = param;
        return 1;

    case NV097_SET_COLOR_CLEAR_VALUE:
        g_pg.clear_color = param;
        return 1;

    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        g_pg.clear_rect_h = param;
        return 1;

    case NV097_SET_CLEAR_RECT_VERTICAL:
        g_pg.clear_rect_v = param;
        return 1;

    case NV097_CLEAR_SURFACE:
    {
        const char *series_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_PREFIX");
        const char *clear_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_CLEAR_PREFIX");
        const char *draw_prefix = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_PREFIX");
        const char *draw_trace = pgraph_cached_getenv(
            "MERCENARIES_TRACE_GAMEPLAY_DRAWS");
        if ((series_prefix || clear_prefix || draw_prefix || draw_trace) &&
            debug_draw_series_phase_active() &&
            debug_gameplay_capture_target_matches()) {
            const char *frame_env = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_GAMEPLAY_ARRAY_SERIES_FRAME");
            const char *draw_frame_env = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_GAMEPLAY_DRAW_FRAME");
            const uint32_t frame = frame_env ?
                (uint32_t)strtoul(frame_env, NULL, 10) : 1u;
            const uint32_t draw_frame = draw_frame_env ?
                (uint32_t)strtoul(draw_frame_env, NULL, 10) : 5u;
            ++g_debug_gameplay_clear_index;
            g_debug_gameplay_array_index = 0u;
            g_debug_gameplay_draw_index = 0u;
            g_debug_gameplay_array_series_active =
                series_prefix &&
                g_debug_gameplay_clear_index == (frame ? frame : 1u);
            g_debug_gameplay_draw_series_active =
                (draw_prefix || draw_trace) &&
                g_debug_gameplay_clear_index == (draw_frame ? draw_frame : 5u);
            /* Diagnostic-only selection: HUD/minimap clears vary with scene.
             * Capture one world depth pass instead of guessing a clear ordinal. */
            if (pgraph_cached_getenv("MERCENARIES_CAPTURE_GAMEPLAY_DEPTH_CLEAR")) {
                g_debug_gameplay_draw_series_active =
                    (draw_prefix || draw_trace) && !g_debug_captured_depth_pass && (param & 1u);
                if (g_debug_gameplay_draw_series_active) g_debug_captured_depth_pass = 1;
            }
            fprintf(stderr,
                    "[PGRAPH-GAMEPLAY-SERIES-CLEAR] index=%u draw=%u "
                    "param=%08X array_active=%d draw_active=%d\n",
                    g_debug_gameplay_clear_index, g_pg.stats.draw_calls,
                    param, g_debug_gameplay_array_series_active,
                    g_debug_gameplay_draw_series_active);
            if (clear_prefix && g_pg.bound_color) {
                const char *exact_env = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_GAMEPLAY_CLEAR_EXACT");
                const char *phase_env = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_GAMEPLAY_CLEAR_PHASE");
                const uint32_t exact = exact_env ?
                    (uint32_t)strtoul(exact_env, NULL, 10) : 1u;
                if (g_debug_gameplay_clear_index == (exact ? exact : 1u) &&
                    (!phase_env || strcmp(phase_env, "after") != 0) &&
                    d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                                 g_pg.bound_color->srv,
                                                 g_pg.bound_color->width,
                                                 g_pg.bound_color->height)) {
                    char clear_path[MAX_PATH];
                    snprintf(clear_path, sizeof(clear_path), "%s-%02u-before.bmp",
                             clear_prefix, g_debug_gameplay_clear_index);
                    fprintf(stderr,
                            "[PGRAPH-GAMEPLAY-CLEAR-CAPTURE] index=%u phase=before "
                            "draw=%u param=%08X path=%s\n",
                            g_debug_gameplay_clear_index, g_pg.stats.draw_calls,
                            param, clear_path);
                    d3d8_DebugCaptureFrameToPath(clear_path);
                }
            }
        }
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();
        if (dev) {
            uint32_t flags = 0;
            float clear_depth = 1.0f;
            DWORD clear_stencil = 0u;
            const uint32_t color_write_mask = (param >> 4u) & 0x0Fu;
            const uint32_t zeta_format =
                (g_pg.surface_format & NV097_SET_SURFACE_FORMAT_ZETA) >> 4;
            const int floating_z =
                (g_pg.control0 & NV097_SET_CONTROL0_Z_FORMAT) != 0u;
            if (color_write_mask == 0x0Fu) flags |= 1;  /* D3DCLEAR_TARGET */
            if (param & 0x01) flags |= 2;  /* D3DCLEAR_ZBUFFER */
            if (param & 0x02) flags |= 4;  /* D3DCLEAR_STENCIL */
            if (zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16) {
                const uint32_t z = g_pg.zstencil_clear & 0xFFFFu;
                clear_depth = floating_z ?
                    (z ? u2f((z << 11) + 0x3C000000u) / 511.9375f : 0.0f) :
                    (float)z / 65535.0f;
            } else if (zeta_format ==
                       NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) {
                const uint32_t z = g_pg.zstencil_clear >> 8;
                clear_stencil = g_pg.zstencil_clear & 0xFFu;
                clear_depth = floating_z ?
                    (z ? u2f(z << 7) / 1.0e30f : 0.0f) :
                    (float)z / 16777215.0f;
            }
            /* A stencil-only clear also requires the zeta attachment. */
            prepare_guest_render_targets((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0u);
            {
                const char *force_color_frame_env = pgraph_cached_getenv(
                    "MERCENARIES_DEBUG_GAMEPLAY_FORCE_COLOR_CLEAR_FRAME");
                const uint32_t force_color_frame = force_color_frame_env ?
                    (uint32_t)strtoul(force_color_frame_env, NULL, 10) : 0u;
                if (force_color_frame != 0u &&
                    g_debug_gameplay_clear_index == force_color_frame &&
                    g_pg.surface_color_offset == 0x0339C000u) {
                    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                                       0xFF000000u, 1.0f, 0u);
                    fprintf(stderr,
                            "[PGRAPH-GAMEPLAY-FORCE-COLOR-CLEAR] index=%u "
                            "draw=%u\n",
                            g_debug_gameplay_clear_index, g_pg.stats.draw_calls);
                }
            }
            if (color_write_mask != 0u && color_write_mask != 0x0Fu &&
                g_pg.bound_color) {
                uint32_t xmin = g_pg.clear_rect_h & 0x0FFFu;
                uint32_t xmax = (g_pg.clear_rect_h >> 16) & 0x0FFFu;
                uint32_t ymin = g_pg.clear_rect_v & 0x0FFFu;
                uint32_t ymax = (g_pg.clear_rect_v >> 16) & 0x0FFFu;
                const uint32_t logical_width =
                    g_pg.bound_color->logical_width ?
                        g_pg.bound_color->logical_width :
                        g_pg.bound_color->width;
                const uint32_t logical_height =
                    g_pg.bound_color->logical_height ?
                        g_pg.bound_color->logical_height :
                        g_pg.bound_color->height;
                uint32_t left, top, right, bottom;

                if (xmin > xmax || ymin > ymax) {
                    xmin = ymin = 0u;
                    xmax = logical_width - 1u;
                    ymax = logical_height - 1u;
                }
                if (xmax >= logical_width) xmax = logical_width - 1u;
                if (ymax >= logical_height) ymax = logical_height - 1u;
                left = (uint32_t)((uint64_t)xmin *
                    g_pg.bound_color->width / logical_width);
                top = (uint32_t)((uint64_t)ymin *
                    g_pg.bound_color->height / logical_height);
                right = (uint32_t)(((uint64_t)(xmax + 1u) *
                    g_pg.bound_color->width + logical_width - 1u) /
                    logical_width);
                bottom = (uint32_t)(((uint64_t)(ymax + 1u) *
                    g_pg.bound_color->height + logical_height - 1u) /
                    logical_height);
                if (right > g_pg.bound_color->width)
                    right = g_pg.bound_color->width;
                if (bottom > g_pg.bound_color->height)
                    bottom = g_pg.bound_color->height;
                if (g_debug_post_pda_resolve_trace_active &&
                    g_pg.pending_scanout == g_pg.bound_color) {
                    fprintf(stderr,
                            "[PGRAPH-POST-PDA-MASKED-CLEAR] param=%08X "
                            "mask=%X color=%08X rect=%u,%u-%u,%u "
                            "surface=%08X\n",
                            param, color_write_mask, g_pg.clear_color,
                            left, top, right, bottom,
                            g_pg.bound_color->offset);
                }
                if (!d3d8_ClearRenderTargetMasked(g_pg.clear_color,
                        color_write_mask, left, top, right, bottom)) {
                    fprintf(stderr,
                            "[PGRAPH] masked color clear failed: "
                            "param=%08X mask=%X surface=%08X\n",
                            param, color_write_mask,
                            g_pg.bound_color->offset);
                }
            }
            dev->lpVtbl->Clear(dev, 0, NULL, flags, g_pg.clear_color,
                               clear_depth, clear_stencil);
            if ((flags & D3DCLEAR_STENCIL) && g_pg.bound_depth &&
                g_pg.bound_depth->format ==
                    NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) {
                g_pg.bound_depth->uniform_stencil_known = 1;
                g_pg.bound_depth->uniform_stencil_value =
                    (uint8_t)(clear_stencil & 0xFFu);
                g_pg.bound_depth->possible_stencil_bits =
                    (uint8_t)(clear_stencil & 0xFFu);
            }
            if (color_write_mask != 0u && g_pg.bound_color) {
                g_pg.bound_color->drawn = 1;
                g_pg.bound_color->gpu_drawn = 1;
                g_pg.bound_color->draw_generation = g_pg.surface_generation;
                g_pg.bound_color->write_serial = ++g_pg.surface_write_serial;
                debug_capture_post_pda_pending_write("clear");
                if (g_pg.pending_scanout == g_pg.bound_color)
                    g_pg.pending_scanout_write_us = pgraph_scanout_clock_us();
                g_pg.bound_color->depth_alias_source_offset = 0u;
                g_pg.bound_color->depth_alias_source_serial = 0u;
                g_pg.bound_color->resolve_source_width = 0u;
                g_pg.bound_color->resolve_source_height = 0u;
                g_pg.bound_color->resolve_source_pitch = 0u;
                g_pg.last_drawn_color = g_pg.bound_color;
            }
            if (flags & D3DCLEAR_ZBUFFER) {
                g_pg.host_zeta_offset = g_pg.surface_zeta_offset;
                g_pg.host_zeta_valid = 1;
            }
            if (clear_prefix && g_pg.bound_color &&
                g_mercenaries_gameplay_capture_active != 0u &&
                g_pg.surface_color_offset == 0x0339C000u) {
                const char *exact_env = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_GAMEPLAY_CLEAR_EXACT");
                const char *phase_env = pgraph_cached_getenv(
                    "MERCENARIES_CAPTURE_GAMEPLAY_CLEAR_PHASE");
                const uint32_t exact = exact_env ?
                    (uint32_t)strtoul(exact_env, NULL, 10) : 1u;
                if (g_debug_gameplay_clear_index == (exact ? exact : 1u) &&
                    phase_env && strcmp(phase_env, "after") == 0 &&
                    d3d8_CopyTextureToBackbuffer(g_pg.bound_color->texture,
                                                 g_pg.bound_color->srv,
                                                 g_pg.bound_color->width,
                                                 g_pg.bound_color->height)) {
                    char clear_path[MAX_PATH];
                    snprintf(clear_path, sizeof(clear_path), "%s-%02u-after.bmp",
                             clear_prefix, g_debug_gameplay_clear_index);
                    fprintf(stderr,
                            "[PGRAPH-GAMEPLAY-CLEAR-CAPTURE] index=%u phase=after "
                            "draw=%u param=%08X path=%s\n",
                            g_debug_gameplay_clear_index, g_pg.stats.draw_calls,
                            param, clear_path);
                    d3d8_DebugCaptureFrameToPath(clear_path);
                }
            }
        }
        mirror_guest_zeta_clear(param);
        g_pg.stats.clears++;
        return 1;
    }

    /* -- Render State -- */
    case NV097_SET_DEPTH_TEST_ENABLE:
        g_debug_depth_method_draw[g_debug_depth_method_serial & 7u] =
            g_pg.stats.draw_calls;
        g_debug_depth_method_value[g_debug_depth_method_serial & 7u] = param;
        ++g_debug_depth_method_serial;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_METHODS") != NULL) {
            const char *minimum_env =
                pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_MIN");
            const char *maximum_env =
                pgraph_cached_getenv("MERCENARIES_TRACE_DEPTH_MAX");
            const uint32_t minimum = minimum_env ?
                (uint32_t)strtoul(minimum_env, NULL, 10) : 0u;
            const uint32_t maximum = maximum_env ?
                (uint32_t)strtoul(maximum_env, NULL, 10) : UINT32_MAX;
            if (g_pg.stats.draw_calls >= minimum &&
                g_pg.stats.draw_calls < maximum) {
                fprintf(stderr,
                        "[PGRAPH-DEPTH-METHOD] draw=%u enable=%08X\n",
                        g_pg.stats.draw_calls, param);
                fflush(stderr);
            }
        }
        g_pg.depth_test = param ? 1 : 0;
        return 1;

    case NV097_SET_DEPTH_FUNC:
        if (param >= 0x200u && param <= 0x207u)
            g_pg.depth_func = (param & 7u) + 1u;
        return 1;

    case NV097_SET_DEPTH_MASK:
        g_pg.depth_write = param ? 1 : 0;
        return 1;

    case NV097_SET_CLIP_MIN:
        g_pg.clip_min = param;
        if (pgraph_trace_depth_window_enabled())
            fprintf(stderr, "[PGRAPH-ZCLIP-METHOD] draw=%u min=%08X (%g)\n",
                    g_pg.stats.draw_calls, param, u2f(param));
        return 1;

    case NV097_SET_CLIP_MAX:
        g_pg.clip_max = param;
        if (pgraph_trace_depth_window_enabled())
            fprintf(stderr, "[PGRAPH-ZCLIP-METHOD] draw=%u max=%08X (%g)\n",
                    g_pg.stats.draw_calls, param, u2f(param));
        return 1;

    case NV097_SET_ZMIN_MAX_CONTROL:
        g_pg.zmin_max_control = param;
        if (pgraph_trace_depth_window_enabled())
            fprintf(stderr, "[PGRAPH-ZCLIP-METHOD] draw=%u control=%08X mode=%u\n",
                    g_pg.stats.draw_calls, param,
                    (param & NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_EN) >> 4);
        return 1;

    case NV097_SET_BLEND_ENABLE:
        g_pg.blend_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_FUNC_SFACTOR:
        g_pg.blend_sfactor = param;
        return 1;

    case NV097_SET_BLEND_FUNC_DFACTOR:
        g_pg.blend_dfactor = param;
        return 1;

    case NV097_SET_BLEND_EQUATION:
        g_pg.blend_equation = param;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_BLEND_EQUATION") != NULL) {
            fprintf(stderr,
                    "[PGRAPH-BLEND-EQUATION] draw=%u value=%08X\n",
                    g_pg.stats.draw_calls, param);
        }
        if (g_pg.stats.draw_calls > 10000u &&
            param == NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT) {
            const char *capture_path =
                pgraph_cached_getenv("MERCENARIES_CAPTURE_BLEND_EQUATION_PATH");
            const char *capture_gate = pgraph_cached_getenv(
                "MERCENARIES_CAPTURE_BLEND_EQUATION_GATE_FILE");
            if (capture_path && (!capture_gate || !*capture_gate ||
                GetFileAttributesA(capture_gate) != INVALID_FILE_ATTRIBUTES)) {
                static int gated_capture_queued;
                if (capture_gate && *capture_gate) {
                    /* Deferred scanout does not service legacy flip captures.
                     * Queue this one-shot at the completed frame instead. */
                    if (!gated_capture_queued) {
                        d3d8_DebugQueueFrameCapture(capture_path);
                        gated_capture_queued = 1;
                    }
                } else {
                    d3d8_DebugArmFlipCapture(capture_path);
                }
            }
        }
        return 1;

    case NV097_SET_CULL_FACE_ENABLE:
        g_pg.cull_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_CULL_FACE:
        if (param == 0x0404u || param == 0x0405u || param == 0x0408u)
            g_pg.cull_face = param;
        return 1;

    case NV097_SET_FRONT_FACE:
        if (param == 0x0900u || param == 0x0901u)
            g_pg.front_face = param;
        return 1;

    case NV097_SET_ALPHA_TEST_ENABLE:
        g_pg.alpha_test = param ? 1 : 0;
        return 1;

    case NV097_SET_ALPHA_FUNC:
        if (param >= 0x200u && param <= 0x207u)
            g_pg.alpha_func = (param & 7u) + 1u;
        return 1;

    case NV097_SET_ALPHA_REF:
        g_pg.alpha_ref = param & 0xFFu;
        return 1;

    case NV097_SET_FOG_ENABLE:
        g_pg.fog_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_FOG_COLOR:
        g_pg.fog_color = (param & 0xFF000000u) |
                         ((param & 0x00FF0000u) >> 16) |
                         (param & 0x0000FF00u) |
                         ((param & 0x000000FFu) << 16);
        return 1;

    case NV097_SET_STENCIL_TEST_ENABLE:
        g_pg.stencil_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_STENCIL_FUNC:
        if (param >= 0x200u && param <= 0x207u)
            g_pg.stencil_func = (param & 7u) + 1u;
        return 1;

    case NV097_SET_STENCIL_FUNC_REF:
        g_pg.stencil_ref = param & 0xFFu;
        return 1;

    case NV097_SET_STENCIL_FUNC_MASK:
        g_pg.stencil_mask = param & 0xFFu;
        return 1;

    case NV097_SET_STENCIL_MASK:
        g_pg.stencil_write_mask = param & 0xFFu;
        return 1;

    case NV097_SET_STENCIL_OP_FAIL:
        g_pg.stencil_fail = nv2a_stencil_op_to_d3d(param);
        return 1;

    case NV097_SET_STENCIL_OP_ZFAIL:
        g_pg.stencil_zfail = nv2a_stencil_op_to_d3d(param);
        return 1;

    case NV097_SET_STENCIL_OP_ZPASS:
        g_pg.stencil_zpass = nv2a_stencil_op_to_d3d(param);
        return 1;

    case NV097_SET_CONTROL0:
        g_pg.control0 = param;
        g_pg.stencil_write_enable =
            (param & NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE) != 0u;
        return 1;

    case NV097_SET_WINDOW_CLIP_TYPE:
        g_pg.window_clip_type = param;
        return 1;

    case NV097_SET_POINT_PARAMS_ENABLE:
        g_pg.point_params_enable = param;
        return 1;

    case NV097_SET_POINT_SMOOTH_ENABLE:
        g_pg.point_smooth_enable = param;
        return 1;

    case NV097_SET_LINE_SMOOTH_ENABLE:
        g_pg.line_smooth_enable = param;
        return 1;

    case NV097_SET_POLY_OFFSET_POINT_ENABLE:
        g_pg.polygon_offset_point_enable = param;
        return 1;

    case NV097_SET_POLY_OFFSET_LINE_ENABLE:
        g_pg.polygon_offset_line_enable = param;
        return 1;

    case NV097_SET_POLY_OFFSET_FILL_ENABLE:
        g_pg.polygon_offset_fill_enable = param;
        return 1;

    case NV097_SET_POLYGON_OFFSET_SCALE_FACTOR:
        g_pg.polygon_offset_factor = param;
        return 1;

    case NV097_SET_POLYGON_OFFSET_BIAS:
        g_pg.polygon_offset_bias = param;
        return 1;

    case NV097_SET_FRONT_POLYGON_MODE:
        g_pg.front_polygon_mode = param;
        return 1;

    case NV097_SET_DITHER_ENABLE:
        g_pg.dither_enable = param;
        return 1;

    case NV097_SET_LIGHTING_ENABLE:
        g_pg.lighting_enable = param;
        return 1;

    case NV097_SET_LIGHT_CONTROL:
        g_pg.light_control = param;
        return 1;

    case NV097_SET_FOG_MODE:
        g_pg.fog_mode = param;
        return 1;

    case NV097_SET_FOG_GEN_MODE:
        g_pg.fog_gen_mode = param;
        return 1;

    case NV097_SET_POINT_SIZE:
        g_pg.point_size = param;
        return 1;

    case NV097_SET_PROVOKING_VERTEX:
        g_pg.provoking_vertex = param;
        return 1;

    case NV097_WAIT_FOR_IDLE:
        return 1;

    case NV097_SET_CONTEXT_DMA_VERTEX_A:
    case NV097_SET_CONTEXT_DMA_VERTEX_B:
    {
        NV2AState *d = nv2a_get_state();
        DMAObject dma = nv_dma_load(d, param);
        uint32_t base = (uint32_t)dma.address & 0x07FFFFFFu;

        if (method == NV097_SET_CONTEXT_DMA_VERTEX_A) {
            g_pg.vertex_dma_a = param;
            g_pg.vertex_dma_a_base = base;
        } else {
            g_pg.vertex_dma_b = param;
            g_pg.vertex_dma_b_base = base;
        }
        if (pgraph_cached_getenv("MERCENARIES_TRACE_VERTEX_DMA") != NULL) {
            fprintf(stderr,
                    "[PGRAPH-VERTEX-DMA] method=%04X instance=%08X "
                    "class=%u target=%u base=%08X limit=%08X\n",
                    method, param, dma.dma_class, dma.dma_target, base,
                    (uint32_t)dma.limit);
        }
        return 1;
    }

    case NV097_SET_CONTEXT_DMA_A:
    case NV097_SET_CONTEXT_DMA_B:
    {
        NV2AState *d = nv2a_get_state();
        DMAObject dma = nv_dma_load(d, param);
        uint32_t base = (uint32_t)dma.address & 0x07FFFFFFu;

        if (method == NV097_SET_CONTEXT_DMA_A) {
            g_pg.texture_dma_a = param;
            g_pg.texture_dma_a_base = base;
        } else {
            g_pg.texture_dma_b = param;
            g_pg.texture_dma_b_base = base;
        }
        if (pgraph_cached_getenv("MERCENARIES_TRACE_TEXTURE_DMA") != NULL) {
            fprintf(stderr,
                    "[PGRAPH-TEXTURE-DMA] method=%04X instance=%08X "
                    "class=%u target=%u base=%08X limit=%08X\n",
                    method, param, dma.dma_class, dma.dma_target, base,
                    (uint32_t)dma.limit);
        }
        return 1;
    }

    case NV097_SET_CONTEXT_DMA_COLOR:
    case NV097_SET_CONTEXT_DMA_ZETA:
    {
        NV2AState *d = nv2a_get_state();
        DMAObject dma = nv_dma_load(d, param);
        uint32_t base = (uint32_t)dma.address & 0x07FFFFFFu;

        if (method == NV097_SET_CONTEXT_DMA_COLOR) {
            g_pg.surface_dma_color = param;
            g_pg.surface_dma_color_base = base;
        } else {
            g_pg.surface_dma_zeta = param;
            g_pg.surface_dma_zeta_base = base;
        }
        if (pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_DMA") != NULL &&
            pgraph_trace_surface_textures_enabled() &&
            g_debug_surface_method_trace_count++ < 1024u) {
            fprintf(stderr,
                    "[PGRAPH-SURFACE-DMA] method=%04X instance=%08X "
                    "class=%u target=%u base=%08X limit=%08X\n",
                    method, param, dma.dma_class, dma.dma_target, base,
                    (uint32_t)dma.limit);
        }
        return 1;
    }

    case NV097_SET_SURFACE_FORMAT:
        if (pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_METHODS") != NULL &&
            pgraph_trace_surface_textures_enabled() &&
            g_debug_surface_method_trace_count++ < 1024u) {
            fprintf(stderr,
                    "[PGRAPH-SURFACE-METHOD] draw=%u FORMAT %08X -> %08X "
                    "color=%08X bound=%08X\n",
                    g_pg.stats.draw_calls, g_pg.surface_format, param,
                    g_pg.surface_color_offset,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u);
        }
        g_pg.surface_format = param;
        return 1;

    case NV097_SET_SURFACE_PITCH:
        if (pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_METHODS") != NULL &&
            pgraph_trace_surface_textures_enabled() &&
            g_debug_surface_method_trace_count++ < 1024u) {
            fprintf(stderr,
                    "[PGRAPH-SURFACE-METHOD] draw=%u PITCH %08X -> %08X "
                    "color=%08X bound=%08X\n",
                    g_pg.stats.draw_calls, g_pg.surface_pitch, param,
                    g_pg.surface_color_offset,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u);
        }
        g_pg.surface_pitch = param;
        return 1;

    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_METHODS") != NULL &&
            pgraph_trace_surface_textures_enabled() &&
            g_debug_surface_method_trace_count++ < 1024u) {
            fprintf(stderr,
                    "[PGRAPH-SURFACE-METHOD] draw=%u COLOR %08X -> %08X "
                    "fmt=%08X pitch=%08X bound=%08X/%u/%d\n",
                    g_pg.stats.draw_calls, g_pg.surface_color_offset,
                    param & 0x07FFFFFFu, g_pg.surface_format,
                    g_pg.surface_pitch,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                    g_pg.bound_color ? g_pg.bound_color->anti_aliasing : 0u,
                    g_pg.bound_color ? g_pg.bound_color->drawn : 0);
        }
        g_pg.surface_color_offset =
            (g_pg.surface_dma_color_base + param) & 0x07FFFFFFu;
        return 1;

    case NV097_SET_SURFACE_ZETA_OFFSET:
        g_pg.surface_zeta_offset =
            (g_pg.surface_dma_zeta_base + param) & 0x07FFFFFFu;
        return 1;

    case NV097_SET_COLOR_MASK:
        g_pg.color_mask = param;
        return 1;

    case NV097_SET_SHADE_MODE:
        /* 1=flat, 2=gouraud - we always use gouraud */
        return 1;

    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        if (param >= NV2A_VERTEXSHADER_CONSTANTS)
            return 0;
        {
            const uint32_t history_serial =
                ++g_pg.transform_constant_history_serial;
            const uint32_t history_index = history_serial & 63u;
            g_pg.transform_constant_history[history_index].serial =
                history_serial;
            g_pg.transform_constant_history[history_index].draw =
                g_pg.stats.draw_calls;
            g_pg.transform_constant_history[history_index].method = method;
            g_pg.transform_constant_history[history_index].parameter = param;
            g_pg.transform_constant_history[history_index].load_before =
                g_pg.transform_constant_load;
            g_pg.transform_constant_history[history_index].constant = param;
            g_pg.transform_constant_history[history_index].component = 0u;
            g_pg.transform_constant_history[history_index].is_load = 1u;
        }
        g_pg.transform_constant_load = param;
        if ((pgraph_cached_getenv("MERCENARIES_TRACE_VSH_CONSTANTS") != NULL &&
             param >= 16u && param <= 42u) ||
            (pgraph_cached_getenv("MERCENARIES_TRACE_SPLASH_VSH") != NULL &&
             param <= 3u && g_pg.stats.draw_calls < 8u)) {
            fprintf(stderr, "[PGRAPH-VSH-C] draw=%u load=%u\n",
                    g_pg.stats.draw_calls, param);
        }
        return 1;

    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        g_pg.transform_execution_mode = param;
        return 1;

    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        if (param >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH)
            return 0;
        g_pg.transform_program_load = param;
        return 1;

    case NV097_SET_TRANSFORM_PROGRAM_START:
        if (param >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH)
            return 0;
        if (g_pg.transform_program_start != param)
            g_pg.transform_program_key_valid = 0;
        g_pg.transform_program_start = param;
        return 1;

    case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN:
        return 1;

    /* -- Viewport -- */
    case NV097_SET_VIEWPORT_OFFSET:
    case NV097_SET_VIEWPORT_OFFSET + 4:
    case NV097_SET_VIEWPORT_OFFSET + 8:
    case NV097_SET_VIEWPORT_OFFSET + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
        g_pg.vp_offset[idx] = u2f(param);
        return 1;
    }

    case NV097_SET_VIEWPORT_SCALE:
    case NV097_SET_VIEWPORT_SCALE + 4:
    case NV097_SET_VIEWPORT_SCALE + 8:
    case NV097_SET_VIEWPORT_SCALE + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_SCALE) / 4;
        g_pg.vp_scale[idx] = u2f(param);
        return 1;
    }

    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        g_pg.surface_clip_h = param;
        return 1;

    case NV097_SET_SURFACE_CLIP_VERTICAL:
        g_pg.surface_clip_v = param;
        return 1;

    case NV097_SET_COLOR_KEY_COLOR:
    case NV097_SET_COLOR_KEY_COLOR + 4:
    case NV097_SET_COLOR_KEY_COLOR + 8:
    case NV097_SET_COLOR_KEY_COLOR + 12:
    {
        const uint32_t stage = (method - NV097_SET_COLOR_KEY_COLOR) / 4u;
        g_pg.color_key[stage] = param;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_COLOR_KEY") != NULL) {
            static uint32_t trace_count;
            if (trace_count++ < 64u)
                fprintf(stderr,
                        "[PGRAPH-COLORKEY] draw=%u stage=%u color=%08X\n",
                        g_pg.stats.draw_calls, stage, param);
        }
        return 1;
    }

    /* -- Texture state tracking (4 stages, 0x40 stride) -- */
    case NV097_SET_TEXTURE_OFFSET:
    case NV097_SET_TEXTURE_OFFSET + 0x40:
    case NV097_SET_TEXTURE_OFFSET + 0x80:
    case NV097_SET_TEXTURE_OFFSET + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_OFFSET) / 0x40;
        pgraph_trace_movie_texture_method("offset", stage, param);
        g_pg.tex[stage].offset = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FORMAT:
    case NV097_SET_TEXTURE_FORMAT + 0x40:
    case NV097_SET_TEXTURE_FORMAT + 0x80:
    case NV097_SET_TEXTURE_FORMAT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FORMAT) / 0x40;
        pgraph_trace_movie_texture_method("format", stage, param);
        g_pg.tex[stage].format = param;
        return 1;
    }
    case NV097_SET_TEXTURE_ADDRESS:
    case NV097_SET_TEXTURE_ADDRESS + 0x40:
    case NV097_SET_TEXTURE_ADDRESS + 0x80:
    case NV097_SET_TEXTURE_ADDRESS + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_ADDRESS) / 0x40;
        g_pg.tex[stage].address = param;
        return 1;
    }
    case NV097_SET_TEXTURE_CONTROL0:
    case NV097_SET_TEXTURE_CONTROL0 + 0x40:
    case NV097_SET_TEXTURE_CONTROL0 + 0x80:
    case NV097_SET_TEXTURE_CONTROL0 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL0) / 0x40;
        pgraph_trace_movie_texture_method("control0", stage, param);
        g_pg.tex[stage].control0 = param;
        g_pg.tex[stage].enabled = (param >> 30) & 1;
        if ((param & NV_PGRAPH_TEXCTL0_0_COLORKEYMODE) != 0u &&
            pgraph_cached_getenv("MERCENARIES_TRACE_COLOR_KEY") != NULL) {
            static uint32_t trace_count;
            if (trace_count++ < 128u)
                fprintf(stderr,
                        "[PGRAPH-COLORKEY] draw=%u stage=%d mode=%u "
                        "control0=%08X color=%08X\n",
                        g_pg.stats.draw_calls, stage,
                        param & NV_PGRAPH_TEXCTL0_0_COLORKEYMODE, param,
                        g_pg.color_key[stage]);
        }
        return 1;
    }

    case NV097_SET_TEXTURE_CONTROL1:
    case NV097_SET_TEXTURE_CONTROL1 + 0x40:
    case NV097_SET_TEXTURE_CONTROL1 + 0x80:
    case NV097_SET_TEXTURE_CONTROL1 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL1) / 0x40;
        pgraph_trace_movie_texture_method("control1", stage, param);
        g_pg.tex[stage].control1 = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FILTER:
    case NV097_SET_TEXTURE_FILTER + 0x40:
    case NV097_SET_TEXTURE_FILTER + 0x80:
    case NV097_SET_TEXTURE_FILTER + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FILTER) / 0x40;
        g_pg.tex[stage].filter = param;
        if (pgraph_cached_getenv("MERCENARIES_TRACE_TEXTURE_FILTERS") != NULL) {
            static uint32_t trace_count;
            static uint64_t trace_after_draw = UINT64_MAX;
            if (trace_after_draw == UINT64_MAX) {
                const char *after = pgraph_cached_getenv(
                    "MERCENARIES_TRACE_TEXTURE_FILTERS_AFTER_DRAW");
                trace_after_draw = after && after[0] ?
                    _strtoui64(after, NULL, 0) : 0u;
            }
            if (g_pg.stats.draw_calls >= trace_after_draw &&
                trace_count++ < 256u)
                fprintf(stderr,
                        "[PGRAPH-TEXFILTER] draw=%llu stage=%d raw=%08X "
                        "min=%u mag=%u bias=%d off=%08X fmt=%08X "
                        "ctl0=%08X addr=%08X rect=%08X\n",
                        (unsigned long long)g_pg.stats.draw_calls, stage, param,
                        (param >> 16) & 0x3Fu, (param >> 24) & 0xFu,
                        ((int32_t)(param << 19)) >> 19,
                        g_pg.tex[stage].offset, g_pg.tex[stage].format,
                        g_pg.tex[stage].control0, g_pg.tex[stage].address,
                        g_pg.tex[stage].image_rect);
        }
        return 1;
    }
    case NV097_SET_TEXTURE_IMAGE_RECT:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x40:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x80:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_IMAGE_RECT) / 0x40;
        pgraph_trace_movie_texture_method("image_rect", stage, param);
        g_pg.tex[stage].image_rect = param;
        return 1;
    }
    case NV097_SET_TEXTURE_BORDER_COLOR:
    case NV097_SET_TEXTURE_BORDER_COLOR + 0x40:
    case NV097_SET_TEXTURE_BORDER_COLOR + 0x80:
    case NV097_SET_TEXTURE_BORDER_COLOR + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_BORDER_COLOR) / 0x40;
        g_pg.tex[stage].border_color = param;
        return 1;
    }

    case NV097_SET_FLIP_READ:
        g_pg.flip_read = param;
        return 1;
    case NV097_SET_FLIP_WRITE:
        g_pg.flip_write = param;
        return 1;
    case NV097_SET_FLIP_MODULO:
        g_pg.flip_modulo = param;
        return 1;
    case NV097_FLIP_INCREMENT_WRITE:
        if (pgraph_cached_getenv("MERCENARIES_TRACE_FLIPS") != NULL) {
            fprintf(stderr,
                    "[PGRAPH-FLIP-INC] gen=%u bound=%08X/%u/%u "
                    "last=%08X/%u/%u color=%08X flip=%u/%u/%u\n",
                    g_pg.surface_generation,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                    g_pg.bound_color ? g_pg.bound_color->anti_aliasing : 0u,
                    g_pg.bound_color ? g_pg.bound_color->draw_generation : 0u,
                    g_pg.last_drawn_color ? g_pg.last_drawn_color->offset : 0u,
                    g_pg.last_drawn_color ?
                        g_pg.last_drawn_color->anti_aliasing : 0u,
                    g_pg.last_drawn_color ?
                        g_pg.last_drawn_color->draw_generation : 0u,
                    g_pg.surface_color_offset, g_pg.flip_read,
                    g_pg.flip_write, g_pg.flip_modulo);
        }
        if (g_pg.flip_modulo)
            g_pg.flip_write = (g_pg.flip_write + 1u) % g_pg.flip_modulo;
        return 1;

    case NV097_FLIP_STALL:
    {
        int uploaded_cpu_frame = 0;
        NV2AState *d = nv2a_get_state();
        /* An unresolved presentation may still be waiting for a quiet period
         * when the guest finishes its next frame. Consume that older resolved
         * frame before another resolve replaces it. FLIP_STALL is an explicit
         * frame boundary; no wall-clock quiescence heuristic is needed here. */
        if (service_pending_scanout(1)) {
            d3d8_DebugSetSubmissionSource(1u);
            d3d8_PresentFrame();
        }
        if (g_debug_post_pda_resolve_trace_active &&
            g_debug_post_pda_flip_trace_index < 96u) {
            fprintf(stderr,
                    "[PGRAPH-POST-PDA-FLIP] index=%u gen=%u had=%d "
                    "deferred=%d bound=%08X/%u/%d/%u last=%08X/%u/%d/%u "
                    "pending=%08X/%d/%u color=%08X scanout=%08X\n",
                    g_debug_post_pda_flip_trace_index++,
                    g_pg.surface_generation, g_pg.frame_had_draw,
                    g_pg.deferred_flip,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                    g_pg.bound_color ? g_pg.bound_color->anti_aliasing : 0u,
                    g_pg.bound_color ? g_pg.bound_color->drawn : 0,
                    g_pg.bound_color ? g_pg.bound_color->draw_generation : 0u,
                    g_pg.last_drawn_color ? g_pg.last_drawn_color->offset : 0u,
                    g_pg.last_drawn_color ? g_pg.last_drawn_color->anti_aliasing : 0u,
                    g_pg.last_drawn_color ? g_pg.last_drawn_color->drawn : 0,
                    g_pg.last_drawn_color ? g_pg.last_drawn_color->draw_generation : 0u,
                    g_pg.pending_scanout ? g_pg.pending_scanout->offset : 0u,
                    g_pg.pending_scanout ? g_pg.pending_scanout->drawn : 0,
                    g_pg.pending_scanout ? g_pg.pending_scanout->draw_generation : 0u,
                    g_pg.surface_color_offset,
                    d ? (uint32_t)(d->pcrtc.start & 0x07FFFFFFu) : 0u);
        }
        if (pgraph_cached_getenv("MERCENARIES_TRACE_FLIPS") != NULL) {
            fprintf(stderr,
                    "[PGRAPH-FLIP] gen=%u had=%d bound=%08X/%u/%d/%u "
                    "last=%08X/%u/%u scanout=%08X flip=%u/%u/%u color=%08X\n",
                    g_pg.surface_generation, g_pg.frame_had_draw,
                    g_pg.bound_color ? g_pg.bound_color->offset : 0u,
                    g_pg.bound_color ? g_pg.bound_color->anti_aliasing : 0u,
                    g_pg.bound_color ? g_pg.bound_color->drawn : 0,
                    g_pg.bound_color ? g_pg.bound_color->draw_generation : 0u,
                    g_pg.last_drawn_color ? g_pg.last_drawn_color->offset : 0u,
                    g_pg.last_drawn_color ?
                        g_pg.last_drawn_color->anti_aliasing : 0u,
                    g_pg.last_drawn_color ?
                        g_pg.last_drawn_color->draw_generation : 0u,
                    d ? (uint32_t)(d->pcrtc.start & 0x07FFFFFFu) : 0u,
                    g_pg.flip_read, g_pg.flip_write, g_pg.flip_modulo,
                    g_pg.surface_color_offset);
        }
        if (g_pg.deferred_flip) {
            pgraph_d3d11_flush();
            present_guest_frontbuffer();
            g_pg.deferred_flip = 0;
            g_pg.frame_had_draw = 0;
            g_pg.last_resolved_color = NULL;
            ++g_pg.surface_generation;
            d3d8_DebugSetSubmissionSource(2u);
            d3d8_PresentFrame();
        }
        if (g_pg.frame_had_draw && g_pg.bound_color &&
            g_pg.bound_color->anti_aliasing !=
                NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_1 &&
            g_pg.surface_color_offset != g_pg.bound_color->offset) {
            /* Xbox Present queues its AA resolve after FLIP_STALL and does
             * not expose the new scanout until that resolve completes. */
            g_pg.deferred_flip = 1;
            pgraph_d3d11_flush();
            return 1;
        }
        if (!g_pg.frame_had_draw) {
            const uint32_t width = 640u;
            const uint32_t height = 480u;
            /* NV097_SET_SURFACE_PITCH packs the color pitch in the low
             * halfword and the zeta pitch in the high halfword.  Using the
             * larger value here made a 640x480 color surface with a 2560-byte
             * pitch get uploaded at the 5120-byte multisampled zeta pitch,
             * exposing the memory after the image as the lower half-frame. */
            uint32_t pitch = g_pg.surface_pitch & 0xFFFFu;
            uint64_t end;

            if (pitch < width * 4u) pitch = width * 4u;
            end = (uint64_t)g_pg.surface_color_offset +
                  (uint64_t)pitch * height;
            if (d && d->vram_ptr &&
                end <= (uint64_t)memory_region_size(d->vram)) {
                d3d8_UploadFrameX8R8G8B8(
                    d->vram_ptr + g_pg.surface_color_offset,
                    pitch, width, height);
                composite_pvideo_overlay(NULL);
                uploaded_cpu_frame = 1;
            }
        }
        g_pg.frame_had_draw = 0;
        pgraph_d3d11_flush();
        if (!uploaded_cpu_frame)
            present_guest_frontbuffer();
        g_pg.last_resolved_color = NULL;
        d3d8_DebugSetSubmissionSource(3u);
        d3d8_PresentFrame();
        ++g_pg.surface_generation;
        return 1;
    }

    default:
        /* Check if it's in a known range we can safely ignore */
        if ((method >= 0x0B80 && method < 0x0C00) ||  /* Transform program */
            (method >= 0x0E00 && method < 0x1000) ||  /* Transform constants */
            (method >= 0x1680 && method < 0x1780) ||  /* Vertex array format/offset */
            (method >= 0x1B00 && method < 0x1C00) ||  /* Texture registers */
            (method >= 0x1D60 && method < 0x1EA0) ||  /* Combiners */
            method == 0x0100 ||                        /* NOP */
            method == 0x0180 ||                        /* SET_OBJECT */
            method == 0x0394 ||                        /* TRANSFORM_EXECUTION_MODE */
            method == 0x0398 ||                        /* TRANSFORM_PROGRAM_CXT_WRITE_EN */
            method == 0x039C ||                        /* TRANSFORM_PROGRAM_LOAD */
            method == 0x01E0)                          /* SHADER_STAGE_PROGRAM */
        {
            return 1;  /* Silently handled (ignored but acknowledged) */
        }

        g_pg.stats.methods_ignored++;
        return 0;  /* Truly unhandled */
    }
}

void pgraph_d3d11_flush(void)
{
    if (g_pg.in_draw) {
        submit_draw();
        g_pg.in_draw = 0;
    }
    g_pg.stats.frames++;
}

static int service_pending_scanout(int force)
{
    static int captured_gameplay_scanout;
    static uint32_t gameplay_scanout_count;
    GuestColorSurface *surface = g_pg.pending_scanout;
    /* A resolve is followed by overlay draws, so neither the resolve nor a
     * short gap between host polls is a frame boundary. In continuous play,
     * the next FLIP_STALL consumes the completed image exactly once. Let that
     * boundary own presentation; the old 2 ms idle test sometimes stole the
     * next image from it and slept an extra frame inside a guest kernel wait.
     * Keep an idle fallback for loading/paused guests that stop issuing flips.
     * 50 ms exceeds both supported continuous frame periods (30 and 60 Hz). */
    if (!surface || (!force &&
        pgraph_scanout_clock_us() - g_pg.pending_scanout_write_us < 50000u))
        return 0;
    if (g_debug_post_pda_resolve_trace_active &&
        g_debug_post_pda_scanout_trace_index < 96u) {
        fprintf(stderr,
                "[PGRAPH-POST-PDA-SCANOUT] index=%u gen=%u surface=%08X/"
                "%ux%u/drawn=%d/drawgen=%u/write=%llu age=%llu\n",
                g_debug_post_pda_scanout_trace_index++,
                g_pg.surface_generation, surface->offset,
                surface->width, surface->height, surface->drawn,
                surface->draw_generation,
                (unsigned long long)surface->write_serial,
                (unsigned long long)((pgraph_scanout_clock_us() -
                                     g_pg.pending_scanout_write_us) / 1000u));
    }
    if (pgraph_cached_getenv("MERCENARIES_TRACE_SCANOUT_SURFACE") != NULL) {
        fprintf(stderr,
                "[PGRAPH-SCANOUT] tick=%llu gen=%u surface=%08X "
                "size=%ux%u logical=%ux%u aa=%u drawn=%d drawgen=%u "
                "write=%llu age=%llu\n",
                (unsigned long long)GetTickCount64(),
                g_pg.surface_generation, surface->offset,
                surface->width, surface->height,
                surface->logical_width, surface->logical_height,
                surface->anti_aliasing, surface->drawn,
                surface->draw_generation,
                (unsigned long long)surface->write_serial,
                (unsigned long long)((pgraph_scanout_clock_us() -
                                     g_pg.pending_scanout_write_us) / 1000u));
        {
            const char *guest_env = pgraph_cached_getenv(
                "MERCENARIES_TRACE_SCANOUT_GUEST_DWORD");
            if (guest_env != NULL && g_xbox_mem_offset != 0) {
                const unsigned long parsed = strtoul(guest_env, NULL, 16);
                if (parsed >= 0x00010000ul && parsed <= 0x03FFFFFCul) {
                    const uint32_t guest_value = *(volatile uint32_t *)(
                        (uintptr_t)g_xbox_mem_offset + (uint32_t)parsed);
                    fprintf(stderr,
                            "[PGRAPH-SCANOUT-GUEST] tick=%llu va=%08lX "
                            "value=%08X\n",
                            (unsigned long long)GetTickCount64(), parsed,
                            guest_value);
                }
            }
        }
        fflush(stderr);
    }
    if (!d3d8_CopyTextureToBackbuffer(surface->texture, surface->srv,
                                      surface->width, surface->height))
        return 0;
    composite_pvideo_overlay(surface);
    if (g_debug_post_pda_resolve_trace_active &&
        g_debug_post_pda_scanout_trace_index == 1u) {
        const char *path = pgraph_cached_getenv(
            "MERCENARIES_CAPTURE_POST_PDA_SCANOUT_PATH");
        if (path && *path)
            d3d8_DebugCaptureFrameToPath(path);
    }
    g_pg.pending_scanout = NULL;
    if (!captured_gameplay_scanout &&
        pgraph_cached_getenv("MERCENARIES_CAPTURE_GAMEPLAY_SERVICE_SCANOUT") != NULL &&
        g_mercenaries_gameplay_capture_active != 0u) {
        const char *index_env =
            pgraph_cached_getenv("MERCENARIES_CAPTURE_GAMEPLAY_SCANOUT_INDEX");
        const uint32_t capture_index = index_env ?
            (uint32_t)strtoul(index_env, NULL, 10) : 1u;
        if (++gameplay_scanout_count >= (capture_index ? capture_index : 1u)) {
            d3d8_DebugCaptureFrameNow();
            captured_gameplay_scanout = 1;
        }
    } else if (pgraph_cached_getenv("MERCENARIES_CAPTURE_SERVICE_SCANOUT") != NULL) {
        d3d8_DebugCaptureFrameNow();
    }
    return 1;
}

int pgraph_d3d11_service_scanout(void)
{
    return service_pending_scanout(0);
}

void pgraph_d3d11_set_guest_stream(uint32_t stream, uint32_t physical_offset,
                                   uint32_t stride)
{
    if (stream >= 16u)
        return;
    g_pg.stream[stream].physical_offset = physical_offset & 0x07FFFFFFu;
    g_pg.stream[stream].stride = stride;
    if (stream == 0u && pgraph_trace_draws_enabled()) {
        fprintf(stderr,
                "[PGRAPH-D3D11] Stream 0: offset=%08X stride=%u\n",
                g_pg.stream[stream].physical_offset, stride);
    }
}

void pgraph_d3d11_set_guest_fvf(uint32_t fvf)
{
    g_pg.guest_fvf = fvf;
}

void pgraph_d3d11_set_chyron_scroll(uint32_t pixels)
{
    g_pg.chyron_scroll_offset = (float)pixels;
}

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out)
{
    if (out) *out = g_pg.stats;
}

void pgraph_d3d11_get_diagnostic_state(PgraphD3D11DiagnosticState *out)
{
    if(!out) return;
    out->color=g_pg.surface_color_offset;out->depth=g_pg.surface_zeta_offset;
    out->vertex_shader=g_pg.host_vsh_hash;out->combiner=g_pg.combiner_control;
    out->primitive=g_pg.draw_mode;out->depth_test=g_pg.depth_test;
    out->depth_write=g_pg.depth_write;out->blend=g_pg.blend_enable;out->alpha_test=g_pg.alpha_test;
    for(unsigned i=0;i<4;i++){out->texture[i]=resolved_texture_offset(i);out->format[i]=g_pg.tex[i].format;}
}

