from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SHIM = (ROOT / "src" / "apu" / "apu_shim.h").read_text(encoding="utf-8")
XEMU_CONFIG = (ROOT / "references" / "xemu" / "config_spec.yml").read_text(
    encoding="utf-8"
)


def test_recomp_defaults_to_verified_3d_bus_fallback():
    assert ".hrtf = false" in SHIM
    assert "MERCENARIES_ENABLE_APU_HRTF" in SHIM
    assert "  hrtf:\n    type: bool\n    default: true" in XEMU_CONFIG


if __name__ == "__main__":
    test_recomp_defaults_to_verified_3d_bus_fallback()
