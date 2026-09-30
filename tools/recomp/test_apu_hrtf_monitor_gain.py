from pathlib import Path
import shutil
import subprocess
import tempfile
import re


ROOT = Path(__file__).resolve().parents[2]
STATE = (ROOT / "src" / "apu" / "apu_state.h").read_text(encoding="utf-8")
VP = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")


def test_vp_monitor_does_not_invent_per_voice_gain() -> None:
    monitor = VP.split("/* VP monitor mix */", 1)[1].split(
        "trace_voice_energy_record", 1
    )[0]
    assert "hrtf_filter_vp_monitor_gain" not in STATE
    assert "hrtf_filter_vp_monitor_gain" not in monitor
    assert "g *= ea_value;" in monitor
    assert "g *= hrtf" not in monitor


def test_headroom_trace_is_opt_in_and_observational() -> None:
    assert 'getenv("MERCENARIES_TRACE_APU_HEADROOM")' in VP
    assert '[APU-HEADROOM] type=hrtf amount=%u' in VP
    assert '[APU-HEADROOM] type=submix bin=%u amount=%u' in VP
    assert '[APU-HEADROOM] type=hrtf-submix bins=%u/%u/%u/%u' in VP


def test_primary_routes_and_monitor_recombination_use_distinct_headroom() -> None:
    helper = re.search(
        r"static inline float voice_route_headroom\(.*?\n\}", VP, re.S
    )
    assert helper is not None
    assert VP.count("voice_route_headroom(d, v, b, bin[b])") == 1
    monitor = VP.split("/* VP monitor mix */", 1)[1].split(
        "trace_voice_energy_record", 1
    )[0]
    assert "submix_headroom[bin[b]]" in monitor
    assert "applying it twice can attenuate fallback voices by 128x" in monitor
    harness = r'''
#include <assert.h>
#include <stdint.h>
#define MCPX_HW_MAX_3D_VOICES 64
#define NUM_MIXBINS 32
typedef struct {
    struct {
        uint8_t hrtf_headroom;
        uint8_t submix_headroom[NUM_MIXBINS];
    } vp;
} MCPXAPUState;
''' + helper.group(0) + r'''
int main(void) {
    MCPXAPUState d = {0};
    d.vp.hrtf_headroom = 0;
    d.vp.submix_headroom[6] = 1;
    assert(voice_route_headroom(&d, 0, 0, 6) == 1.0f);
    assert(voice_route_headroom(&d, 0, 3, 6) == 1.0f);
    assert(voice_route_headroom(&d, 0, 4, 6) == 2.0f);
    assert(voice_route_headroom(&d, MCPX_HW_MAX_3D_VOICES, 0, 6) == 2.0f);
    d.vp.hrtf_headroom = 3;
    assert(voice_route_headroom(&d, 7, 2, 6) == 8.0f);
    return 0;
}
'''
    compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
    with tempfile.TemporaryDirectory(prefix="merc-hrtf-headroom-") as directory:
        directory = Path(directory)
        for mutated in (False, True):
            source = directory / ("mutant.c" if mutated else "actual.c")
            executable = directory / ("mutant.exe" if mutated else "actual.exe")
            fixture = harness.replace("route < 4", "route < 0") if mutated else harness
            source.write_text(fixture, encoding="utf-8")
            subprocess.run(
                [compiler, "-std=c11", "-O2", str(source), "-o", str(executable)],
                check=True,
            )
            result = subprocess.run([str(executable)])
            assert (result.returncode != 0) if mutated else (result.returncode == 0)


if __name__ == "__main__":
    test_vp_monitor_does_not_invent_per_voice_gain()
    test_headroom_trace_is_opt_in_and_observational()
    test_primary_routes_and_monitor_recombination_use_distinct_headroom()
