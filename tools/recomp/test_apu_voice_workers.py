"""Static regression checks for the Xemu-derived parallel APU mixer."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VP = ROOT / "src" / "apu" / "apu_vp.c"
SHIM = ROOT / "src" / "apu" / "apu_shim.h"


def test_parallel_voice_dispatch_is_default_and_bounded() -> None:
    vp = VP.read_text(encoding="utf-8")
    shim = SHIM.read_text(encoding="utf-8")
    assert ".vp = { .num_workers = 0 }" in shim
    assert "static void *voice_worker_thread(void *arg)" in vp
    assert "static void voice_work_schedule(MCPXAPUState *d)" in vp
    assert "static void voice_work_dispatch(MCPXAPUState *d," in vp
    assert "get_voice_bin_src_dst" in vp
    assert "vwd->num_workers = MAX(1, MIN(requested, 8));" in vp
    assert 'getenv("MERCENARIES_DISABLE_APU_WORKERS")' in vp
    assert "if (threaded)" in vp
    assert "voice_work_enqueue(d, v, list);" in vp
    assert "voice_work_dispatch(d, mixbins);" in vp


if __name__ == "__main__":
    test_parallel_voice_dispatch_is_default_and_bounded()
    print("APU voice-worker checks passed")
