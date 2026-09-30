#!/usr/bin/env python3
"""Regression checks for the exact 1:1 fixed-function blit fast path."""
from pathlib import Path
import random

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")


def reference_copy(vram: bytearray, source: int, source_pitch: int,
                   target: int, target_pitch: int, target_x: int,
                   target_y: int, width: int, height: int) -> bytearray:
    out = bytearray(vram)
    for y in range(height):
        for x in range(width):
            src = source + y * source_pitch + x * 4
            dst = target + (target_y + y) * target_pitch + (target_x + x) * 4
            out[dst:dst + 4] = out[src:src + 4]
    return out


def row_copy(vram: bytearray, source: int, source_pitch: int,
             target: int, target_pitch: int, target_x: int,
             target_y: int, width: int, height: int) -> bytearray:
    out = bytearray(vram)
    for y in range(height):
        src = source + y * source_pitch
        dst = target + (target_y + y) * target_pitch + target_x * 4
        out[dst:dst + width * 4] = out[src:src + width * 4]
    return out


def scaled_reference_copy(vram: bytearray, source: int, source_pitch: int,
                          target: int, target_pitch: int, source_width: int,
                          source_height: int, target_width: int,
                          target_height: int) -> bytearray:
    out = bytearray(vram)
    for y in range(target_height):
        source_y = y * source_height // target_height
        for x in range(target_width):
            source_x = x * source_width // target_width
            src = source + source_y * source_pitch + source_x * 4
            dst = target + y * target_pitch + x * 4
            out[dst:dst + 4] = out[src:src + 4]
    return out


def quotient_remainder_copy(vram: bytearray, source: int, source_pitch: int,
                            target: int, target_pitch: int, source_width: int,
                            source_height: int, target_width: int,
                            target_height: int) -> bytearray:
    out = bytearray(vram)
    source_y = 0
    source_y_step, source_y_remainder_step = divmod(source_height, target_height)
    source_y_remainder = 0
    for y in range(target_height):
        source_x = 0
        source_x_step, source_x_remainder_step = divmod(source_width, target_width)
        source_x_remainder = 0
        for x in range(target_width):
            src = source + source_y * source_pitch + source_x * 4
            dst = target + y * target_pitch + x * 4
            out[dst:dst + 4] = out[src:src + 4]
            source_x += source_x_step
            source_x_remainder += source_x_remainder_step
            if source_x_remainder >= target_width:
                source_x_remainder -= target_width
                source_x += 1
        source_y += source_y_step
        source_y_remainder += source_y_remainder_step
        if source_y_remainder >= target_height:
            source_y_remainder -= target_height
            source_y += 1
    return out


def main() -> None:
    start = SOURCE.index("static void mirror_guest_fixed_function_blit")
    end = SOURCE.index("static void submit_draw", start)
    block = SOURCE[start:end]
    assert "source_width == target_width && source_height == target_height" in block
    assert "source_end <= (uint64_t)target_offset" in block
    assert "target_end <= (uint64_t)source_offset" in block
    assert "memcpy(target_row, source_row, (size_t)target_width * 4u);" in block
    assert block.index("if (source_width == target_width") < block.index(
        "uint32_t source_y = 0u")
    assert "source_columns[x]" in block
    assert "source_y_step = source_height / target_height" in block
    assert "((uint64_t)x * source_width)" in block
    assert "((uint64_t)y * source_height)" not in block

    rng = random.Random(0x4D455243)
    for _ in range(1000):
        width = rng.randrange(1, 129)
        height = rng.randrange(1, 65)
        source_pitch = width * 4 + rng.randrange(0, 8) * 4
        target_x = rng.randrange(0, 8)
        target_y = rng.randrange(0, 8)
        target_pitch = (target_x + width + rng.randrange(0, 8)) * 4
        source = 0x100
        source_end = source + source_pitch * height
        target = (source_end + 0xFF) & ~0xFF
        size = target + target_pitch * (target_y + height) + 0x100
        vram = bytearray(rng.randbytes(size))
        assert row_copy(vram, source, source_pitch, target, target_pitch,
                        target_x, target_y, width, height) == reference_copy(
                            vram, source, source_pitch, target, target_pitch,
                            target_x, target_y, width, height)

    for _ in range(1000):
        source_width = rng.randrange(1, 97)
        source_height = rng.randrange(1, 97)
        target_width = rng.randrange(1, 97)
        target_height = rng.randrange(1, 97)
        source_pitch = (source_width + rng.randrange(0, 8)) * 4
        target_pitch = (target_width + rng.randrange(0, 8)) * 4
        source = 0x100
        source_end = source + source_pitch * source_height
        target = (source_end + 0xFF) & ~0xFF
        size = target + target_pitch * target_height + 0x100
        vram = bytearray(rng.randbytes(size))
        assert quotient_remainder_copy(
            vram, source, source_pitch, target, target_pitch,
            source_width, source_height, target_width, target_height
        ) == scaled_reference_copy(
            vram, source, source_pitch, target, target_pitch,
            source_width, source_height, target_width, target_height)

    print("PASS: fixed-function blit fast paths preserve reference pixels")


if __name__ == "__main__":
    main()