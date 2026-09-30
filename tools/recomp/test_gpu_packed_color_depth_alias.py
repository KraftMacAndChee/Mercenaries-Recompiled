#!/usr/bin/env python3
"""Regression guards for GPU-preserved NV2A packed color/depth aliases."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEVICE = (ROOT / "src" / "d3d" / "d3d8_device.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "d3d" / "d3d8_internal.h").read_text(encoding="utf-8")
COMBINERS = (ROOT / "src" / "d3d" / "d3d8_combiners.c").read_text(
    encoding="utf-8"
)
COMBINER_HEADER = (ROOT / "src" / "d3d" / "d3d8_combiners.h").read_text(
    encoding="utf-8"
)
PGRAPH = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)

# D3D11 only permits a pixel shader to export the stencil reference on devices
# that report the D3D11.3 capability. Unsupported hardware reconstructs the
# exact stencil byte with eight masked GPU bit passes; CPU readback remains the
# final fallback only if that portable pipeline cannot be created.
assert "D3D11_FEATURE_D3D11_OPTIONS2" in DEVICE
assert "PSSpecifiedStencilRefSupported" in DEVICE
assert "unsupported; retaining CPU fallback" in DEVICE
assert "SV_Depth" in DEVICE
assert "SV_StencilRef" in DEVICE
assert "d3d8_CopyPackedColorToDepthStencil" in HEADER
assert "d3d8_ensure_packed_color_depth_fallback_pipeline" in DEVICE
assert "packed_color_depth_fallback_ps" in DEVICE
assert "packed_color_stencil_bit_ps[8]" in DEVICE
assert "packed_color_stencil_bit_state[8]" in DEVICE
assert "rgba.b & STENCIL_BIT" in DEVICE

# The color RTV exposes RGBA. The aliased Xbox A8R8G8B8 word maps to host
# D24S8 as G | R<<8 | A<<16 | B<<24. Guard both implementations and prove the
# arithmetic agrees on representative channel values.
assert "rgba.g|(rgba.r<<8)|(rgba.a<<16)" in DEVICE
assert "o.stencil=rgba.b" in DEVICE
assert "(uint32_t)rgba[1] |" in PGRAPH
assert "((uint32_t)rgba[0] << 8) |" in PGRAPH
assert "((uint32_t)rgba[3] << 16) |" in PGRAPH
assert "((uint32_t)rgba[2] << 24)" in PGRAPH
for rgba in ((0, 0, 0, 0), (255, 255, 255, 255), (1, 2, 3, 4),
             (17, 129, 250, 63), (254, 127, 128, 255)):
    r, g, b, a = rgba
    cpu = g | (r << 8) | (a << 16) | (b << 24)
    gpu_depth = g | (r << 8) | (a << 16)
    gpu = gpu_depth | (b << 24)
    assert gpu == cpu

alias = PGRAPH[
    PGRAPH.index("static void sync_guest_color_alias_to_depth") :
    PGRAPH.index("static void prepare_guest_render_targets")
]
gpu_call = "d3d8_CopyPackedColorToDepthStencil(color->srv, depth->dsv"
cpu_staging = "ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)"
assert gpu_call in alias
assert cpu_staging in alias
assert alias.index(gpu_call) < alias.index(cpu_staging)
assert "depth->color_alias_serial = color->write_serial;" in alias
assert "return;" in alias[alias.index(gpu_call) : alias.index(cpu_staging)]

# The helper must preserve the renderer state it temporarily replaces.
helper = DEVICE[
    DEVICE.index("BOOL d3d8_CopyPackedColorToDepthStencil") :
    DEVICE.index("const DWORD         *d3d8_GetRenderStates")
]
for required in (
    "OMGetRenderTargets", "OMSetRenderTargets", "RSGetViewports",
    "RSSetViewports", "PSGetShaderResources", "PSSetShaderResources",
    "OMGetDepthStencilState", "OMSetDepthStencilState",
):
    assert required in helper
assert "use_stencil_bit_passes" in helper
assert "D3D11_CLEAR_STENCIL" in helper
assert "packed_color_depth_fallback_state" in helper
assert "packed_color_depth_fallback_ps" in helper
assert "packed_color_stencil_bit_state[bit]" in helper
assert "packed_color_stencil_bit_ps[bit]" in helper
assert "1u << bit" in helper

# Mercenaries' AA depth-copy pass is 2x wider than its destination. It must use
# the original integer nearest-neighbor rule on GPU, preserve all 24 depth bits
# plus stencil, and write the raw Xbox alias bytes in the same pass.
assert "DXGI_FORMAT_R24G8_TYPELESS" in DEVICE
assert "DXGI_FORMAT_R24_UNORM_X8_TYPELESS" in DEVICE
assert "DXGI_FORMAT_X24_TYPELESS_G8_UINT" in DEVICE
assert "src.x=min((dst.x*alias_dims.x+alias_dims.z/2)/alias_dims.z" in DEVICE
assert "src.y=min((dst.y*alias_dims.y+alias_dims.w/2)/alias_dims.w" in DEVICE
assert (
    "o.color=float4(s,zb&255,(zb>>8)&255,(zb>>16)&255)/255.0"
    in DEVICE
)
assert "Xbox bytes are [stencil, depth-low, depth-middle, depth-high]" in PGRAPH
assert "(host_d24s8 << 8) | (host_d24s8 >> 24)" in PGRAPH
assert "d3d8_CopyScaledDepthStencilAlias" in HEADER

# A raw Z24S8 alias is sampled by two distinct Xbox formats in Mercenaries.
# The sky's A8B8G8R8 path consumes raw guest channel order, while satellite
# filtering declares A8R8G8B8 and needs R/B exchanged.  Guard the per-stage
# draw-time conversion so a satellite fix cannot globally black out the sky.
assert "d3d8_combiners_set_texture_red_blue_swap" in COMBINER_HEADER
assert "g_texture_scale[stage][2] = swap ? 1.0f : 0.0f;" in COMBINERS
assert "if (tex_scale[%d].z > 0.5) r_t%d = r_t%d.bgra;" in COMBINERS
assert PGRAPH.count("d3d8_combiners_set_texture_red_blue_swap(") >= 4
assert PGRAPH.count(
    "NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8"
) >= 4

depth_alias = PGRAPH[
    PGRAPH.index("static int sync_guest_depth_alias_from_depth") :
    PGRAPH.index("static void sync_guest_color_alias_to_depth")
]
gpu_scaled = "d3d8_CopyScaledDepthStencilAlias("
cpu_readback = "ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)"
assert gpu_scaled in depth_alias
assert cpu_readback in depth_alias
assert depth_alias.index(gpu_scaled) < depth_alias.index(cpu_readback)
assert "return 1;" in depth_alias[depth_alias.index(gpu_scaled) : depth_alias.index(cpu_readback)]

scaled_helper = DEVICE[
    DEVICE.index("BOOL d3d8_CopyScaledDepthStencilAlias") :
    DEVICE.index("BOOL d3d8_CopyPackedColorToDepthStencil")
]
for required in (
    "CopyResource", "OMGetRenderTargets", "OMSetRenderTargets",
    "PSGetConstantBuffers", "PSSetConstantBuffers", "RSGetViewports",
    "RSSetViewports", "PSGetShaderResources", "PSSetShaderResources",
    "OMGetDepthStencilState", "OMSetDepthStencilState",
):
    assert required in scaled_helper
# Hardware without pixel-shader stencil export may still take the exact GPU
# path when runtime metadata proves the entire source stencil plane is uniform.
# Creation/full clears establish that proof; any potentially writing draw
# invalidates it, and CPU uploads recompute it from all stencil bytes.
assert "depth_alias_uniform_ps" in DEVICE
uniform_pipeline = DEVICE[
    DEVICE.index("static BOOL d3d8_ensure_depth_alias_uniform_pipeline") :
    DEVICE.index("static void d3d8_release_depth_alias_source_views")
]
assert "SV_Depth" in uniform_pipeline
assert "SV_StencilRef" not in uniform_pipeline
assert "StencilEnable = FALSE" in uniform_pipeline
assert "D3D11_CLEAR_STENCIL" in scaled_helper
assert "uniform_stencil_known" in scaled_helper
assert "use_uniform_stencil" in scaled_helper
assert "use_uniform_stencil && !destination_stencil_matches" in scaled_helper
assert "uniform_stencil_known" in PGRAPH
assert "static void note_bound_depth_stencil_draw" in PGRAPH
assert "depth->uniform_stencil_known = 0" in PGRAPH
assert "g_pg.bound_depth->uniform_stencil_known = 1" in PGRAPH
assert "source->uniform_stencil_known" in depth_alias
assert "source->uniform_stencil_value" in depth_alias
assert "source->possible_stencil_bits" in depth_alias
assert "destination->uniform_stencil_known &&" in depth_alias
assert "destination->uniform_stencil_value ==" in depth_alias
assert "source->texture, source->depth_srv, source->stencil_srv" in depth_alias
assert "alias_color->depth_alias_source_offset = source->offset;" in depth_alias
assert "DXGI_FORMAT_R24G8_TYPELESS" in PGRAPH
assert "DXGI_FORMAT_R24_UNORM_X8_TYPELESS" in PGRAPH
assert "DXGI_FORMAT_X24_TYPELESS_G8_UINT" in PGRAPH
assert "D3D11_BIND_SHADER_RESOURCE" in PGRAPH
assert PGRAPH.count("create_guest_depth_resource(") >= 4
assert "if (!source_depth_srv || !source_stencil_srv)" in scaled_helper
assert "new_srvs[0] = source_depth_srv" in scaled_helper
assert "new_srvs[1] = source_stencil_srv" in scaled_helper
assert "static int stencil_op_preserves_uniform_value" in PGRAPH
assert "case 2u: result = 0u" in PGRAPH
assert "case 3u: result = reference" in PGRAPH
assert "value == 0xFFu ? 0xFFu" in PGRAPH
assert "value == 0u ? 0u" in PGRAPH
assert "return result == value" in PGRAPH
assert "[PGRAPH-DEPTH-ALIAS-INVALIDATE]" in PGRAPH
assert "depth_alias_stencil_bit_ps[8]" in DEVICE
assert "depth_alias_stencil_bit_state[8]" in DEVICE
assert "static BOOL d3d8_ensure_depth_alias_stencil_bit_pipeline" in DEVICE
assert '" if ((s & STENCIL_BIT)==0) discard; }"' in DEVICE
assert "depth_desc.StencilWriteMask = (UINT8)(1u << bit)" in DEVICE
assert "use_stencil_bit_passes" in scaled_helper
assert "D3D11_CLEAR_STENCIL, 1.0f, 0u" in scaled_helper
assert "g_device_state.depth_alias_stencil_bit_state[bit]" in scaled_helper
assert "g_device_state.depth_alias_stencil_bit_ps[bit]" in scaled_helper
assert "1u << bit" in scaled_helper
assert "possible_stencil_bits & (1u << bit)" in scaled_helper
assert '"[D3D-DEPTH-ALIAS-BITS] possible=%02X passes=%u "' in scaled_helper
assert "++stencil_bit_pass_count" in scaled_helper
assert "uint8_t possible_stencil_bits" in PGRAPH
assert "depth->possible_stencil_bits |= reference & write_mask" in PGRAPH
assert "depth->possible_stencil_bits |= write_mask" in PGRAPH
assert "g_pg.bound_depth->possible_stencil_bits" in PGRAPH

# Array and immediate draws retain explicit packed-depth provenance only when
# they are the concrete producer. A later ordinary color draw must clear that
# marker so the satellite filter is not reinterpreted as depth; AA-resolve
# provenance remains intact across partial composites. The producer detector
# is restricted to an actual, shape-compatible Z24S8 source exposed as linear
# A8R8G8B8 by an unblended/depth-disabled draw.
assert "static GuestDepthSurface *sampled_packed_depth_copy_source(" in PGRAPH
packed_source = PGRAPH[
    PGRAPH.index("static GuestDepthSurface *sampled_packed_depth_copy_source") :
    PGRAPH.index("static ID3D11ShaderResourceView *snapshot_bound_color_surface")
]
assert "g_pg.blend_enable || g_pg.depth_test" in packed_source
assert "NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8" in packed_source
assert "source->format != NV097_SET_SURFACE_FORMAT_ZETA_Z24S8" in packed_source
assert "width == backing_width && height == backing_height" in packed_source
assert "pitch == source->pitch" in packed_source
array_write = PGRAPH[
    PGRAPH.index("static void submit_array_draw(void)") :
    PGRAPH.index("static void mirror_guest_fixed_function_blit")
]
array_tail = array_write[
    array_write.index('debug_capture_post_pda_pending_write("array")') :
]
assert "g_pg.bound_color->depth_alias_source_offset = 0u;" in array_tail
immediate_write = PGRAPH[
    PGRAPH.index("static void submit_draw(void)") :
    PGRAPH.index("static void commit_immediate_vertex(void)")
]
assert "depth_alias_source = prepare_draw_render_targets(host_depth_test);" in array_write
assert "depth_alias_source = prepare_draw_render_targets(g_pg.depth_test);" in immediate_write
prepare_draw = PGRAPH[
    PGRAPH.index("static GuestDepthSurface *prepare_draw_render_targets") :
    PGRAPH.index("static ID3D11ShaderResourceView *snapshot_bound_color_surface")
]
assert "pending->depth_alias_source_offset = 0u;" in prepare_draw
assert prepare_draw.index("pending->depth_alias_source_offset = 0u;") < \
    prepare_draw.index("prepare_guest_render_targets(want_depth || g_pg.stencil_enable);")
assert PGRAPH.count(
    "g_pg.bound_color->depth_alias_source_offset =\n"
    "                depth_alias_source->offset;"
) == 2
immediate_tail = immediate_write[
    immediate_write.index('debug_capture_post_pda_pending_write("immediate")') :
]
assert "g_pg.bound_color->depth_alias_source_offset = 0u;" in immediate_tail
assert "resolve_source_width = 0u;" not in array_tail
assert "resolve_source_width = 0u;" not in immediate_tail
assert PGRAPH.count("depth_alias_source_offset = 0u;") >= 1

# A newer zeta writer may populate a color alias placeholder, but must not
# replace an independently rendered color surface merely because both host
# resources share a guest base address. Mercenaries deliberately retains the
# latter for its satellite camera while an AA zeta allocation is live there.
# Explicit packed-depth copies carry separate provenance.
surface_texture = PGRAPH[
    PGRAPH.index("static ID3D11ShaderResourceView *get_guest_surface_texture") :
    PGRAPH.index("static GuestDepthSurface *sampled_packed_depth_copy_source")
]
assert "newer_depth->write_serial > surface->write_serial" in surface_texture
assert "!surface->gpu_drawn" in surface_texture
assert surface_texture.index("!surface->gpu_drawn") < surface_texture.index(
    "newer_depth->write_serial > surface->write_serial"
)
assert "texture_width == depth_backing_width" in surface_texture
assert "texture_height == depth_backing_height" in surface_texture
assert "texture_pitch == newer_depth->pitch" in surface_texture
assert "NULL, surface->rtv, surface->width, surface->height" in surface_texture
assert "newer_depth->color_alias_serial = surface->write_serial;" in surface_texture
assert "surface->depth_alias_source_offset = newer_depth->offset;" in surface_texture
assert "[PGRAPH-SURFACE-DEPTH-SAMPLE]" in surface_texture

# The satellite feed needs the live gameplay zeta payload, but materializing a
# general color alias at texture-bind time regressed both title and gameplay
# skies in runs 1710/1711/1716. Keep the live conversion transient and gated to
# the fully observed satellite reduction signature.
assert "static GuestDepthSurface *satellite_depth_filter_source" in PGRAPH
assert "g_pg.bound_color->offset != 0x02980000u" in PGRAPH
assert "resolved_texture_offset(stage) != 0x01E34000u" in PGRAPH
assert "g_pg.host_vsh_hash != 0x1730DD1Au" in PGRAPH
assert "g_pg.shader_other_stage_input != 0x00210000u" in PGRAPH
assert "prepare_satellite_depth_sample(" in PGRAPH
assert "stage_uses_satellite_depth" in PGRAPH
assert "create_sampled_depth_color_alias" not in PGRAPH
assert "sampled-zeta color create" not in PGRAPH

# A direct packed-depth copy may preserve the title-sky alias while the source
# generation is unchanged. Once that zeta is written again, rebinding the
# destination must convert its captured pixels rather than recopy the live
# source and overwrite the satellite filter.
color_to_depth = PGRAPH[
    PGRAPH.index("static void sync_guest_color_alias_to_depth") :
    PGRAPH.index("static ID3D11ShaderResourceView *get_guest_surface_texture")
]
assert "color->depth_alias_source_serial" in color_to_depth
assert "source->write_serial ==" in color_to_depth
assert "color->depth_alias_source_serial" in color_to_depth
assert "sync_guest_depth_alias_from_depth(source, depth)" in color_to_depth
assert "d3d8_CopyPackedColorToDepthStencil(" in color_to_depth
assert "source generation no longer matches" in color_to_depth

assert "uint64_t depth_alias_source_serial;" in PGRAPH
assert PGRAPH.count("depth_alias_source->write_serial;") == 2
assert "depth_alias_source_serial = newer_depth->write_serial;" in PGRAPH

# Xemu has one current SurfaceBinding per VRAM address. Our renderer keeps
# separate color and zeta caches, so a newly bound zeta surface must retire a
# stale color surface at that address. Mercenaries reuses its front-end color
# allocation as gameplay zeta; retaining both made the satellite camera sample
# the main menu and broke the gameplay sky. Same-address color/zeta targets are
# preserved for the title's explicit packed-depth post-process path.
depth_surface_create = PGRAPH[
    PGRAPH.index("static GuestDepthSurface *get_guest_depth_surface") :
    PGRAPH.index("static int upload_guest_depth_pixels")
]
assert "g_pg.surface_color_offset != g_pg.surface_zeta_offset" in depth_surface_create
assert "find_guest_color_surface(g_pg.surface_zeta_offset)" in depth_surface_create
assert "release_guest_color_surface(stale_color);" in depth_surface_create
assert "[PGRAPH-SURFACE-EVICT]" in depth_surface_create
assert depth_surface_create.index(
    "surface->anti_aliasing == anti_aliasing)\n        return surface;"
) < depth_surface_create.index("[PGRAPH-SURFACE-EVICT]")

# A compatible color surface that is intentionally retained for a
# same-address color/zeta target still wins inside producer classification;
# ordinary zeta-only sampling continues through the packed-depth path.
assert "GuestColorSurface *color_source;" in packed_source
assert "color_source = find_guest_color_surface(" in packed_source
assert "color_source->gpu_drawn" in packed_source
assert "guest_color_surface_texture_shape_compatible(" in packed_source
assert "[PGRAPH-DEPTH-COPY-COLOR-WINS]" in packed_source
assert "static GuestDepthSurface *confirmed_sampled_packed_depth_copy_source(" not in PGRAPH
assert "stage_sampled_packed_depth" not in PGRAPH

scaled_alias = DEVICE[
    DEVICE.index("BOOL d3d8_CopyScaledDepthStencilAlias") :
    DEVICE.index("BOOL d3d8_CopyPackedColorToDepthStencil")
]
assert "const BOOL color_only = destination_dsv == NULL;" in scaled_alias
assert "color_only ? NULL" in scaled_alias
assert "!color_only && use_stencil_bit_passes" in scaled_alias

def apply_stencil_op(value: int, operation: int, reference: int,
                     write_mask: int) -> int:
    if operation == 1:
        result = value
    elif operation == 2:
        result = 0
    elif operation == 3:
        result = reference
    elif operation == 4:
        result = min(value + 1, 0xFF)
    elif operation == 5:
        result = max(value - 1, 0)
    elif operation == 6:
        result = (~value) & 0xFF
    elif operation == 7:
        result = (value + 1) & 0xFF
    elif operation == 8:
        result = (value - 1) & 0xFF
    else:
        result = value
    return ((value & (~write_mask & 0xFF)) | (result & write_mask)) & 0xFF


# The possible-bit abstraction may overestimate but must never omit a bit an
# Xbox stencil operation can produce. Exercise every value/reference pair and
# representative masks across every supported operation.
for old_value in range(256):
    for reference in range(256):
        for write_mask in (0, 1, 2, 3, 0x0F, 0x33, 0x55, 0x80, 0xF0, 0xFF):
            for operation in range(1, 9):
                possible = old_value
                if operation == 3:
                    possible |= reference & write_mask
                elif 4 <= operation <= 8:
                    possible |= write_mask
                actual = apply_stencil_op(
                    old_value, operation, reference, write_mask
                )
                assert actual & ~possible == 0

print("GPU packed color/depth alias regression passed")

