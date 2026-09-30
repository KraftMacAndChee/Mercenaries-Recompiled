/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8→D3D11 rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D11 render target + viewport
 *   - Render state (blend, depth, cull) → D3D11 state objects
 *   - Begin/End draw + Inline vertex data → D3D11 DrawPrimitiveUP
 *   - Texture binding → D3D11 shader resource views
 *   - Clear commands → D3D11 ClearRenderTargetView
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D11_H
#define NV2A_PGRAPH_D3D11_H

#include <stdint.h>

/* Initialize the PGRAPH→D3D11 translator. Call after D3D11 device is created. */
void pgraph_d3d11_init(void);

/* Shut down and release resources. */
void pgraph_d3d11_shutdown(void);
void pgraph_d3d11_set_haze_mode(int mode);

/* Select the physical size used for the Xbox 640x480 render surface while
 * preserving the guest's logical coordinates. This is true internal
 * rendering resolution, not a resize of the completed low-resolution frame. */
void pgraph_d3d11_set_internal_resolution(uint32_t width, uint32_t height);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);

/* Append bounded non-incrementing data after one scalar context-selection
 * method. Returns consumed words, or zero for unsupported state/methods. */
uint32_t pgraph_d3d11_inline_batch(uint32_t method, const uint32_t *words,
                                  uint32_t count);

/* Execute an explicit NV09F SRCCOPY between compatible GPU color surfaces. */
int pgraph_d3d11_image_blit(void);

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Copy a completed, quiet guest resolve target to the host scanout.
 * Returns nonzero only when a distinct pending guest frame was consumed. */
int pgraph_d3d11_service_scanout(void);

/* Xbox D3D's retail driver caches stream bindings in CPU-side state and may
 * emit only DRAW_ARRAYS in the push buffer. Static recomp ports can mirror
 * those bindings here when the title calls SetStreamSource. */
void pgraph_d3d11_set_guest_stream(uint32_t stream, uint32_t physical_offset,
                                   uint32_t stride);
void pgraph_d3d11_set_guest_fvf(uint32_t fvf);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
} PgraphD3D11Stats;

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out);
/* Read-only, render-thread snapshot for sparse preview diagnostics. */
typedef struct {
    uint32_t color,depth,vertex_shader,combiner,primitive;
    uint32_t texture[4],format[4];
    uint32_t depth_test,depth_write,blend,alpha_test;
} PgraphD3D11DiagnosticState;
void pgraph_d3d11_get_diagnostic_state(PgraphD3D11DiagnosticState *out);

void pgraph_d3d11_reset_gameplay_capture_series(void);

/* Call on the guest render thread. Reserve the guest offset for this texture;
 * pixels are borrowed without copying and must remain valid for process life,
 * including shutdown/reinit (shutdown releases only the host texture).
 * Returns 1 on registration or an existing offset with the same pixel pointer;
 * repeated registration does not replace stored dimensions/pixels. Returns 0
 * for invalid input, a conflicting pointer, or capacity exhaustion. Neither
 * result transfers ownership of pixels or the caller's guest allocation. */
int pgraph_d3d11_register_ui_texture(uint32_t offset,uint32_t width,uint32_t height,const uint32_t *pixels);

/* Same ownership contract; also selects a generated mip chain for the entry. */
int pgraph_d3d11_register_world_texture(uint32_t offset,uint32_t width,uint32_t height,const uint32_t *pixels);

#endif /* NV2A_PGRAPH_D3D11_H */
