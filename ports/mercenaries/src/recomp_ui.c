/* Aspect correction for positioned retail HUD brushes. All generated code is
 * disposable; Patch-Generated installs the entry/exit hooks from the ISO. */
#include "recomp/recomp_types.h"
#include "recomp_options.h"
#include <math.h>
#include <string.h>

typedef struct HudAnchor {
    uint32_t brush, vtable;
    float anchor, offset;
} HudAnchor;
typedef struct HudPaintScope {
    uint32_t first, base, prim_first, prim_base;
    float anchor, scale;
    int active, pda_background, sniper;
} HudPaintScope;
static HudAnchor anchors[128];
static HudPaintScope scopes[16];
static unsigned scope_depth;

static int hud_canvas(uint32_t canvas)
{
    /* RsHud instrument, primary instrument, message and VO subtitle canvases.
     * The mask, flags, screen transitions, 3D markers and PDA are separate. */
    return canvas == 0x363FD0u || canvas == 0x364028u ||
           canvas == 0x364218u || canvas == 0x364268u;
}
static HudAnchor *hud_anchor(uint32_t brush, int create)
{
    if (brush < 0x35C000u || brush >= 0x363F00u) return NULL;
    for (unsigned i = 0; i < 128; ++i) {
        HudAnchor *a = &anchors[i];
        if (a->brush == brush) return a;
        if (!a->brush) {
            if (!create) return NULL;
            a->brush = brush; return a;
        }
    }
    return NULL;
}
void recomp_ui_record_position(uint32_t brush, float x, uint32_t screen_ref)
{
    HudAnchor *a;
    uint32_t canvas;
    static const uint32_t screen_x[3] = {0x3015B4u,0x3015BCu,0x3015C0u};
    if (screen_ref > 8 || !(a = hud_anchor(brush, 1))) return;
    canvas = MEM32(brush + 4u);
    if (!hud_canvas(canvas)) return;
    a->vtable = MEM32(brush);
    a->anchor = x + MEMF(screen_x[screen_ref % 3u]);
    a->offset = MEMF(brush + 0x1Cu);
}
void recomp_ui_begin_brush(uint32_t brush)
{
    HudPaintScope *s;
    HudAnchor *a;
    uint32_t canvas,width,height;
    int pda, sniper, menu;
    if (scope_depth++ >= 16) return;
    s = &scopes[scope_depth - 1]; memset(s, 0, sizeof(*s));
    /* RsDataPodDisplay's seven 2D brushes form one physical PDA. Preserve
     * its whole layout around the screen center, including maps and clipping. */
    pda = brush == 0x342F80u || brush == 0x342FACu || brush == 0x34300Cu ||
          brush == 0x34303Cu || brush == 0x343068u || brush == 0x3430ACu ||
          brush == 0x3433CCu;
    /* These two retail brushes do not register HUD screen anchors. Scope
     * type zero is the sniper; GPS, binoculars and designators have distinct
     * layouts and must not inherit this correction. */
    sniper = brush == 0x35CDC8u && MEM32(brush) == 0x2EC264u &&
             MEM32(brush + 0x2Cu) == 0u;
    menu = brush >= 0x10000u && brush < 0x4000000u - 0x30u &&
           MEM32(brush) == 0x2F55A0u;
    a = hud_anchor(brush, 0);
    if (!pda && !sniper && !menu && (!a || a->vtable != MEM32(brush))) return;
    canvas = MEM32(brush + 4u);
    if (canvas < 0x10000u || canvas > 0x4000000u - 0x50u) return;
    if ((pda ? canvas != 0x342F30u :
         sniper ? canvas != 0x364170u : !menu && !hud_canvas(canvas)) ||
        MEM32(MEM32(brush) + 0x18u) != 0x20AB50u) return;
    recomp_options_presentation_aspect(&width, &height);
    s->scale = (4.0f * (float)height) / (3.0f * (float)width);
    if (s->scale == 1.0f && !pda) return;
    s->sniper = sniper;
    s->anchor = (pda || sniper) ? 320.0f : menu ?
                 (MEMF(brush + 0x1Cu) + MEMF(canvas + 0x48u)) *
                  MEMF(canvas + 0x3Cu) + MEMF(0x7AB5C8u) : (a->anchor + MEMF(brush + 0x1Cu) - a->offset +
                 MEMF(canvas + 0x48u)) * MEMF(canvas + 0x3Cu) + MEMF(0x7AB5C8u);
    s->pda_background = brush == 0x342F80u;
    s->base = MEM32(0x7AC800u); s->first = MEM32(0x7AC804u);
    s->prim_base = MEM32(0x7AC810u); s->prim_first = MEM32(0x7AC814u);
    s->active = isfinite(s->anchor) && s->base && s->first >= s->base;
}
void recomp_ui_end_brush(void)
{
    HudPaintScope *s;
    uint32_t end,prim_end;
    if (!scope_depth) return;
    if (--scope_depth >= 16) return;
    s = &scopes[scope_depth];
    if (!s->active) return;
    end = MEM32(0x7AC804u); prim_end = MEM32(0x7AC814u);
    /* Retail locked 20-byte vertex buffer and 28-byte primitive records.
     * Reject resets, wraparound, or unexpected capacity before touching data. */
    if (MEM32(0x7AC800u) != s->base || end < s->first ||
        end - s->base > 11264u * 20u || (end - s->first) % 20u ||
        MEM32(0x7AC810u) != s->prim_base || prim_end < s->prim_first ||
        prim_end > 2500u) return;
    uint32_t geometry_first = s->first;
    /* Retail PDA backing stays within the device canvas; the world remains
     * visible at its safe-area edges. */
    for (uint32_t p = geometry_first; p < end; p += 20u) {
        const float x = MEMF(p);
        /* Sniper shading consists of outer rectangles joined to the circular
         * aperture. Keep their screen-edge vertices in place while correcting
         * their inner edges with the reticle, so no unshaded side strips open.
         * Retail's half-pixel offset may put the left edge at -0.5. */
        if (s->sniper && (x <= 0.0f || x >= 639.5f)) continue;
        MEMF(p) = s->anchor + (x - s->anchor) * s->scale;
    }
    for (uint32_t i = s->prim_first; i < prim_end; ++i) {
        uint32_t p = s->prim_base + i * 28u;
        float left = (float)(int16_t)MEM16(p + 16u);
        float right = (float)(int16_t)MEM16(p + 20u);
        /* Leave unrestricted clips alone; transform actual local masks/clips. */
        if (left > 0.0f || right < 640.0f) {
            MEM16(p + 16u) = (uint16_t)(int16_t)floorf(s->anchor + (left - s->anchor) * s->scale);
            MEM16(p + 20u) = (uint16_t)(int16_t)ceilf(s->anchor + (right - s->anchor) * s->scale);
        }
    }
}

/* Prompt glyphs keep square pixels even in retail canvases that do not use
 * an anchored HUD brush (for example the action hint and front-end menus). */
float recomp_ui_prompt_aspect(void)
{
    uint32_t width,height;
    for(unsigned i=0;i<scope_depth && i<16;++i)if(scopes[i].active)return 1.0f;
    recomp_options_presentation_aspect(&width,&height);
    return width && height ? (3.0f*width)/(4.0f*height) : 1.0f;
}

/* Outline vertices undergo the anchored aspect correction at brush exit.
 * Compensate only their thickness when that would fall below one render pixel;
 * positions, text, textures and outlines already covered at higher resolutions
 * retain the authored geometry. */
float recomp_ui_outline_scale(float width, float axis_x, float axis_y)
{
    uint32_t height;
    if (!scope_depth || scope_depth > 16 || !scopes[scope_depth - 1].active)
        return 1.0f;
    axis_x *= scopes[scope_depth - 1].scale;
    recomp_options_internal_resolution_size(NULL, &height);
    const float pixels = fabsf(width) * sqrtf(axis_x * axis_x + axis_y * axis_y)
                       * ((float)height / 480.0f);
    return pixels > 0.0f && pixels < 1.0f ? 1.0f / pixels : 1.0f;
}
