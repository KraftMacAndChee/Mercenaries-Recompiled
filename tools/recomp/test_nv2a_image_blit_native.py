"""Exercise the real NV062/NV09F decoder and scaled copy rectangle bounds."""
from pathlib import Path
import shutil, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[2]
core = (ROOT/'src/nv2a/nv2a_core.c').read_text(encoding='utf-8')
header = (ROOT/'src/nv2a/nv2a_state.h').read_text(encoding='utf-8')
renderer = (ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
start = core.index('        if (graphics_class == NV_CONTEXT_SURFACES_2D) {')
end = core.index('\n        if (graphics_class == NV_CONTEXT_PATTERN', start)
decoder = core[start:end]
fields = header[header.index('struct PGRAPHState {'):header.index('\n};',header.index('struct PGRAPHState {'))+3]
allocation = renderer[renderer.index('static GuestColorSurface *get_image_blit_depth_destination('):renderer.index('static GuestColorSurface *get_guest_color_surface(void)')]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "src/nv2a/nv2a_regs.h"
#include "src/nv2a/nv2a_image_blit.h"
''' + r'''
typedef struct { uint32_t valid,anti_aliasing,format,pitch,logical_width,logical_height,width,height; } GuestDepthSurface;
typedef struct { uint32_t width,height; } GuestColorSurface;
static GuestDepthSurface depth;
static GuestColorSurface color;
static unsigned allocations;
static GuestDepthSurface *find_guest_depth_surface(uint32_t offset) { return offset==0x026B4000 ? &depth : NULL; }
static GuestColorSurface *get_guest_color_surface_at(uint32_t offset,uint32_t pitch,uint32_t format,uint32_t aa,uint32_t lw,uint32_t lh,uint32_t w,uint32_t h) {
 assert(offset==0x026B4000 && pitch==2560 && format==8 && aa==0);
 assert(lw==640 && lh==480 && w==960 && h==720);
 ++allocations; color.width=w; color.height=h; return &color;
}
''' + allocation + fields + r'''
typedef struct { struct PGRAPHState pgraph; } NV2AState;
static NV2AState state, submitted;
static unsigned copies;
static int pgraph_d3d11_image_blit(void) { submitted=state; ++copies; return 1; }
static void method(uint32_t graphics_class, uint32_t method, uint32_t param) {
    NV2AState *d=&state;
    int method_handled=0;
''' + decoder + r'''
    assert(method_handled);
}
int main(void) {
    PgraphImageBlitRect r;
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_OBJECT,0x900);
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_CONTEXT_DMA_IMAGE_SOURCE,0xA00);
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_CONTEXT_DMA_IMAGE_DESTIN,0xB00);
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_COLOR_FORMAT,11);
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_PITCH,0x0A000A00);
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_OFFSET_SOURCE,0x82664000);
    method(NV_CONTEXT_SURFACES_2D,NV062_SET_OFFSET_DESTIN,0x82980000);
    method(NV_IMAGE_BLIT,NV09F_SET_CONTEXT_SURFACES,0x900);
    method(NV_IMAGE_BLIT,NV09F_SET_OPERATION,NV09F_SET_OPERATION_SRCCOPY);
    method(NV_IMAGE_BLIT,NV09F_CONTROL_POINT_IN,0x0014000A);
    method(NV_IMAGE_BLIT,NV09F_CONTROL_POINT_OUT,0x0028001E);
    assert(!copies);
    method(NV_IMAGE_BLIT,NV09F_SIZE,0x006400C8);
    assert(copies==1);
    assert(submitted.pgraph.context_surfaces_2d_object==0x900);
    assert(submitted.pgraph.image_blit_context_surfaces==0x900);
    assert(submitted.pgraph.context_surfaces_2d_dma_source==0xA00);
    assert(submitted.pgraph.context_surfaces_2d_dma_dest==0xB00);
    assert(submitted.pgraph.context_surfaces_2d_source_offset==0x02664000);
    assert(submitted.pgraph.context_surfaces_2d_dest_offset==0x02980000);
    assert(submitted.pgraph.context_surfaces_2d_source_pitch==2560);
    assert(submitted.pgraph.context_surfaces_2d_dest_pitch==2560);
    assert(submitted.pgraph.image_blit_point_in==0x0014000A);
    assert(submitted.pgraph.image_blit_point_out==0x0028001E);
    assert(submitted.pgraph.image_blit_size==0x006400C8);
    /* A second new game uses a depth-only allocation first clipped to the
     * radar. The copy must establish its full 640x480 identity before drawing. */
    depth=(GuestDepthSurface){1,0,2,2560,640,480,960,720};
    assert(get_image_blit_depth_destination(0x026B4000,2560)==&color);
    assert(allocations==1 && color.width==960 && color.height==720);
    assert(!get_image_blit_depth_destination(0x02980000,2560));
    assert(!get_image_blit_depth_destination(0x026B4000,1280));
    depth.valid=0; assert(!get_image_blit_depth_destination(0x026B4000,2560)); depth.valid=1;
    depth.anti_aliasing=1; assert(!get_image_blit_depth_destination(0x026B4000,2560)); depth.anti_aliasing=0;
    depth.format=1; assert(!get_image_blit_depth_destination(0x026B4000,2560));
    assert(allocations==1);
    assert(pgraph_image_blit_scale_rect(0,0,640,480,640,480,2880,2160,&r));
    assert(r.left==0 && r.top==0 && r.right==2880 && r.bottom==2160);
    assert(pgraph_image_blit_scale_rect(11,21,201,101,640,480,2880,2160,&r));
    assert(r.left==49 && r.top==94 && r.right==954 && r.bottom==549);
    assert(pgraph_image_blit_scale_rect(639,479,1,1,640,480,2880,2160,&r));
    assert(r.right==2880 && r.bottom==2160);
    assert(!pgraph_image_blit_scale_rect(639,479,2,1,640,480,2880,2160,&r));
    assert(!pgraph_image_blit_scale_rect(0,0,0,1,640,480,2880,2160,&r));
    assert(!pgraph_image_blit_scale_rect(UINT32_MAX,0,1,1,640,480,2880,2160,&r));
    assert(!pgraph_image_blit_scale_rect(1,0,UINT32_MAX,1,640,480,2880,2160,&r));
    assert(!pgraph_image_blit_scale_rect(0,0,1,1,0,480,2880,2160,&r));
    assert(!pgraph_image_blit_scale_rect(0,0,1,1,640,480,0,2160,&r));
    return 0;
}
'''
compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
with tempfile.TemporaryDirectory(prefix='nv2a-image-blit-') as temp:
    p=Path(temp); (p/'test.c').write_text(fixture,encoding='utf-8')
    subprocess.run([compiler,'-std=c11','-I',str(ROOT),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
    subprocess.run([str(p/'test.exe')],check=True)
blit=renderer[renderer.index('int pgraph_d3d11_image_blit(void)'):renderer.index('static void debug_capture_satellite_pass_state')]
for marker in ('CopySubresourceRegion','source_addr + source_end','dest_addr + dest_end',
               'OMGetRenderTargets','OMSetRenderTargets','PSGetShaderResources',
               'PSSetShaderResources','source->texture == destination->texture',
               'destination->depth_alias_source_offset = 0u;',
               'destination->depth_alias_source_serial = 0u;'):
    assert marker in blit
assert blit.index('CopySubresourceRegion') < blit.index('destination->depth_alias_source_offset = 0u;')
assert 'release_guest_color_surface' not in blit
assert 'release_guest_depth_surface' not in blit
print('NV2A image blit decoder and rectangle regression passed')
