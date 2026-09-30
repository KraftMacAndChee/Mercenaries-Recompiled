"""
Self-check for the recompiler's section layout.

Run: py -3 tools/recomp/test_config.py

Regression guard for the bug where config.py held a hand-edited section table
for whichever game was worked on last. Any binary larger than that stale table
had every function past its end classified as "not code", so those functions
failed to lift -- silently, and looking like an instruction-coverage problem
rather than a configuration one.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402


def _halo_like():
    """A layout resembling Halo's: .text well past the old fallback's end."""
    return [
        config.Section(".text", 0x00012000, 0x001D49CC, 0x00002000, 0x001D49CC, True),
        config.Section("DSOUND", 0x00203600, 0x0001F798, 0x001F1000, 0x0001F664, True),
        config.Section(".rdata", 0x00253080, 0x00075430, 0x0024C000, 0x00075430, False),
        # virtual_size > raw_size: the tail is zero-filled BSS with no file bytes.
        config.Section(".data", 0x002C84C0, 0x0036A918, 0x002C2000, 0x00069A3C, False),
    ]


def test_fallback_is_replaced():
    assert config.configured_from() is None
    config._install(_halo_like(), entry_point=0x001D43B4,
                    kernel_thunk_addr=0x00253090, origin="halo-test")
    assert config.configured_from() == "halo-test"
    assert config.TEXT_VA_START == 0x00012000
    assert config.TEXT_VA_END == 0x00012000 + 0x001D49CC
    assert config.ENTRY_POINT == 0x001D43B4
    assert config.KERNEL_THUNK_ADDR == 0x00253090
    print("ok  fallback_is_replaced")


def test_code_beyond_old_fallback_end():
    config._install(_halo_like(), 0x001D43B4, 0x00253090, "halo-test")
    # The stale Dashboard table ended at 0x00170000. These are the addresses
    # that used to fail: real code, well past that boundary.
    assert config.is_code_address(0x00170440)
    assert config.is_code_address(0x001E68BB)
    assert config.is_code_address(0x00203609)   # DSOUND
    assert not config.is_data_address(0x00203609)
    print("ok  code_beyond_old_fallback_end")


def test_data_sections_are_not_code():
    config._install(_halo_like(), 0x001D43B4, 0x00253090, "halo-test")
    assert config.is_data_address(0x00253080)   # .rdata
    assert not config.is_code_address(0x00253080)
    assert config.is_data_address(0x002C84C0)   # .data
    # Outside every section: neither.
    assert not config.is_code_address(0x7FFFFFFF)
    assert not config.is_data_address(0x7FFFFFFF)
    print("ok  data_sections_are_not_code")


def test_va_to_file_offset():
    config._install(_halo_like(), 0x001D43B4, 0x00253090, "halo-test")
    assert config.va_to_file_offset(0x00012000) == 0x00002000
    assert config.va_to_file_offset(0x00012010) == 0x00002010
    assert config.va_to_file_offset(0x00203609) == 0x001F1009
    # Unmapped address.
    assert config.va_to_file_offset(0x7FFFFFFF) is None
    # BSS tail: inside virtual_size but past raw_size, so there are no file
    # bytes to read. Returning a bogus offset here would read a neighbouring
    # section's data as if it were this one's.
    bss = 0x002C84C0 + 0x00069A3C + 0x10
    assert config.va_to_file_offset(bss) is None
    print("ok  va_to_file_offset")


def test_classify_uses_flags_and_overrides():
    assert config._classify(".text", config.SECTION_EXECUTABLE) is True
    assert config._classify("D3D", config.SECTION_EXECUTABLE) is True
    assert config._classify("BINK", 0) is False
    # .data is data even when the XBE marks it executable (many titles do).
    assert config._classify(".data", config.SECTION_EXECUTABLE) is False
    assert config._classify(".rdata", config.SECTION_EXECUTABLE) is False
    # XIPS holds the XAP parser despite not being flagged executable.
    assert config._classify("XIPS", 0) is True
    print("ok  classify_uses_flags_and_overrides")


if __name__ == "__main__":
    test_fallback_is_replaced()
    test_code_beyond_old_fallback_end()
    test_data_sections_are_not_code()
    test_va_to_file_offset()
    test_classify_uses_flags_and_overrides()
    print("\nall passed")
