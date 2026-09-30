from pathlib import Path


ROOT = Path(__file__).parents[2]


def _snap_surface_edge(value: float, size: float) -> float:
    if abs(value + 0.5) < 0.03125:
        return 0.0
    if abs(value - (size - 0.5)) < 0.03125:
        return size
    return value


def _covered_pixels(left: float, right: float, scale: int) -> set[int]:
    """Model scaled D3D11 samples after canonical surface-edge snapping."""
    physical_left = _snap_surface_edge(left, 640.0) * scale
    physical_right = _snap_surface_edge(right, 640.0) * scale
    return {
        pixel
        for pixel in range(640 * scale)
        if physical_left <= pixel + 0.5 < physical_right
    }


def main() -> None:
    fixed = (ROOT / "src" / "d3d" / "d3d8_shaders.c").read_text(encoding="utf-8")
    programmable = (ROOT / "src" / "d3d" / "d3d8_vsh.c").read_text(encoding="utf-8")

    assert '"        float2 rasterPos = input.pos.xy;\\n"' in fixed
    assert '"        if (abs(rasterPos.x + 0.5) < 0.03125) rasterPos.x = 0.0;\\n"' in fixed
    assert '"        if (abs(rasterPos.x - (ScreenSize.x - 0.5)) < 0.03125) rasterPos.x = ScreenSize.x;\\n"' in fixed
    assert '"    if (abs(oPos.x + 0.5) < 0.03125) oPos.x = 0.0;\\n"' in programmable
    assert '"    if (abs(oPos.x - (surfaceSize.x - 0.5)) < 0.03125) oPos.x = surfaceSize.x;\\n"' in programmable
    assert programmable.index("trunc(oPos.xy * 16.0) / 16.0") < programmable.index(
        "abs(oPos.x + 0.5)"
    )

    # Mercenaries emits both conventions for full-screen passes.  Either must
    # reach the first and final physical pixel at native and 9x internal scale.
    for scale in (1, 9):
        for left, right in ((-0.5, 639.5), (0.0, 640.0)):
            covered = _covered_pixels(left, right, scale)
            assert 0 in covered
            assert 640 * scale - 1 in covered

    # Ordinary screen-space geometry must not acquire a global half-pixel shift.
    assert _snap_surface_edge(12.0, 640.0) == 12.0
    assert _snap_surface_edge(639.0, 640.0) == 639.0

    print("ok d3d11_nv2a_pixel_centers")


if __name__ == "__main__":
    main()
