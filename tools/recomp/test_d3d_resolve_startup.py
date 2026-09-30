"""Keep the fixed resolve pipeline out of late gameplay compilation paths."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEVICE = (ROOT / "src/d3d/d3d8_device.c").read_text(encoding="utf-8")

create_start = DEVICE.index("static HRESULT __stdcall d3d8_CreateDevice")
create_end = DEVICE.index("static const IDirect3D8Vtbl", create_start)
create = DEVICE[create_start:create_end]

shader_init = create.index("hr = d3d8_shaders_init();")
resolve_init = create.index("if (!d3d8_ensure_resolve_pipeline())")
state_init = create.index("hr = d3d8_states_init();")

assert shader_init < resolve_init < state_init
assert 'fprintf(stderr, "D3D8: Resolve pipeline init failed\\n")' in create
assert 'fprintf(stderr, "D3D8: Compiling fixed resolve pipeline\\n")' in DEVICE

print("fixed resolve pipeline is compiled and validated during D3D startup")
