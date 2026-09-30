"""
Self-check for __SEH_prolog / __SEH_epilog detection.

Run: py -3 tools/recomp/test_seh_detect.py

Regression guard for the bug where these addresses were hardcoded to one
game's CRT. On any other title the lifter never emitted the "read ebp back
from the SEH helper" line, so ebp kept whatever stale value it had and the
first ebp-relative local access read through it. In Halo that was a fault at
0xFFFFFFFC -- ebp was 0, and the function's first act was [ebp-4].
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.lifter import detect_seh_helpers  # noqa: E402

# Real bytes from Halo build 2276 cachebeta.xbe, 0x001DD5C8.
SEH_PROLOG_BYTES = bytes.fromhex(
    "68ecbd1d00"        # push   0x1dbdec
    "64a100000000"      # mov    eax, fs:[0]
    "50"                # push   eax
    "64892500000000"    # mov    fs:[0], esp
    "8b442410"          # mov    eax, [esp+0x10]
    "896c2410"          # mov    [esp+0x10], ebp
    "8d6c2410"          # lea    ebp, [esp+0x10]
    "2be0"              # sub    esp, eax
    "535657"            # push   ebx / esi / edi
    "8b45f8"            # mov    eax, [ebp-8]
    "8965e8"            # mov    [ebp-0x18], esp
    "50"                # push   eax
    "8b45fc"            # mov    eax, [ebp-4]
    "c745fcffffffff"    # mov    [ebp-4], -1
    "8945f8"            # mov    [ebp-8], eax
    "c3"                # ret
)

# Real bytes from the same binary, 0x001DD601.
SEH_EPILOG_BYTES = bytes.fromhex(
    "8b4df0"            # mov    ecx, [ebp-0x10]
    "64890d00000000"    # mov    fs:[0], ecx
    "595f5e5b"          # pop    ecx / edi / esi / ebx
    "c9"                # leave
    "51"                # push   ecx
    "c3"                # ret
)

# An ordinary function that touches fs:[0] but is not a SEH helper.
DECOY_BYTES = bytes.fromhex(
    "64a100000000"      # mov    eax, fs:[0]
    "8b4004"            # mov    eax, [eax+4]
    "c3"                # ret
)

BASE = 0x00012000
RAW = 0x00002000


def _layout():
    config._install(
        [config.Section(".text", BASE, 0x100000, RAW, 0x100000, True)],
        entry_point=BASE, kernel_thunk_addr=0, origin="seh-test",
    )


def _image(placements):
    """Build a fake .text image with each blob at its file offset."""
    size = max(off + len(b) for off, b in placements) + 16
    buf = bytearray(size)
    for off, blob in placements:
        buf[off:off + len(blob)] = blob
    return bytes(buf)


def _fn(addr, size):
    return {"start": f"0x{addr:08X}", "end": f"0x{addr + size:08X}",
            "size": size, "section": ".text"}


def test_detects_both():
    _layout()
    p_va, e_va, d_va = BASE + 0x100, BASE + 0x200, BASE + 0x300
    data = _image([
        (RAW + 0x100, SEH_PROLOG_BYTES),
        (RAW + 0x200, SEH_EPILOG_BYTES),
        (RAW + 0x300, DECOY_BYTES),
    ])
    func_db = {
        p_va: _fn(p_va, len(SEH_PROLOG_BYTES)),
        e_va: _fn(e_va, len(SEH_EPILOG_BYTES)),
        d_va: _fn(d_va, len(DECOY_BYTES)),
    }
    prolog, epilog = detect_seh_helpers(func_db, data)
    assert prolog == p_va, hex(prolog or 0)
    assert epilog == e_va, hex(epilog or 0)
    print("ok  detects_both")


def test_decoy_alone_is_not_a_prolog():
    """fs:[0] access on its own is common; the lea ebp,[esp+0x10] is the tell."""
    _layout()
    d_va = BASE + 0x300
    data = _image([(RAW + 0x300, DECOY_BYTES)])
    prolog, epilog = detect_seh_helpers({d_va: _fn(d_va, len(DECOY_BYTES))}, data)
    assert prolog is None
    assert epilog is None
    print("ok  decoy_alone_is_not_a_prolog")


def test_absent_helpers_are_not_an_error():
    """A title whose CRT does not use these must detect cleanly as None."""
    _layout()
    prolog, epilog = detect_seh_helpers({}, b"")
    assert prolog is None and epilog is None
    print("ok  absent_helpers_are_not_an_error")


def test_accepts_int_end_from_batch_translator():
    """BatchTranslator rewrites func_info["end"] to an int in place."""
    _layout()
    p_va = BASE + 0x100
    data = _image([(RAW + 0x100, SEH_PROLOG_BYTES)])
    info = _fn(p_va, len(SEH_PROLOG_BYTES))
    info["end"] = p_va + len(SEH_PROLOG_BYTES)   # int, not hex string
    del info["size"]                             # force the end-based path
    prolog, _ = detect_seh_helpers({p_va: info}, data)
    assert prolog == p_va
    print("ok  accepts_int_end_from_batch_translator")


def test_oversized_match_is_rejected():
    """A big function that happens to contain the markers is not the helper."""
    _layout()
    va = BASE + 0x100
    padded = SEH_PROLOG_BYTES + b"\x90" * 400
    data = _image([(RAW + 0x100, padded)])
    prolog, _ = detect_seh_helpers({va: _fn(va, len(padded))}, data)
    assert prolog is None
    print("ok  oversized_match_is_rejected")


def test_unmapped_address_is_skipped():
    """va_to_file_offset returns None outside every section; not a crash."""
    _layout()
    data = _image([(RAW + 0x100, SEH_PROLOG_BYTES)])
    prolog, epilog = detect_seh_helpers(
        {0x7FFFFFFF: _fn(0x7FFFFFFF, len(SEH_PROLOG_BYTES))}, data)
    assert prolog is None and epilog is None
    print("ok  unmapped_address_is_skipped")


def test_missing_xbe_data_is_skipped():
    """Lifter can be constructed without the binary's bytes."""
    _layout()
    p_va = BASE + 0x100
    func_db = {p_va: _fn(p_va, len(SEH_PROLOG_BYTES))}
    assert detect_seh_helpers(func_db, None) == (None, None)
    print("ok  missing_xbe_data_is_skipped")


if __name__ == "__main__":
    test_detects_both()
    test_decoy_alone_is_not_a_prolog()
    test_absent_helpers_are_not_an_error()
    test_accepts_int_end_from_batch_translator()
    test_oversized_match_is_rejected()
    test_unmapped_address_is_skipped()
    test_missing_xbe_data_is_skipped()
    print("\nall passed")
