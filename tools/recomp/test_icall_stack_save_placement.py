"""Regression tests for safe indirect-call stack snapshot placement."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import _fixup_icall_esp_save


def test_icall_save_stays_after_aligned_seh_prologue_and_saved_registers():
    lines = [
        "loc_001C4100: ;",
        "    PUSH32(esp, ebp);",
        "    ebp = esp;",
        "    esp = esp & 0xFFFFFFF0u;",
        "    PUSH32(esp, 0xFFFFFFFFu);",
        "    PUSH32(esp, 0x245CBE);",
        "    eax = MEM32(0);",
        "    PUSH32(esp, eax);",
        "    MEM32(0) = esp;",
        "    esp = esp - 0x68;",
        "    PUSH32(esp, ebx);",
        "    PUSH32(esp, esi);",
        "    esi = ecx;",
        "    ecx = MEM32(esi + 0x3C);",
        "    eax = MEM32(ecx);",
        "    PUSH32(esp, edi);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x18), _icall_esp);",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
        "    POP32(esp, ebx);",
        "    esp = ebp;",
        "    POP32(esp, ebp);",
    ]

    output = _fixup_icall_esp_save(lines)
    save = output.index("    { uint32_t _icall_esp = g_esp;")
    saved_edi = output.index("    PUSH32(esp, edi);")
    call = next(i for i, line in enumerate(output) if "RECOMP_ICALL_SAFE" in line)

    assert saved_edi < save < call


def test_icall_save_still_precedes_interleaved_argument_pushes():
    lines = [
        "loc_00100000: ;",
        "    eax = MEM32(ecx);",
        "    PUSH32(esp, 0x22);",
        "    edx = MEM32(esi + 4);",
        "    PUSH32(esp, edx);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x14), _icall_esp);",
    ]

    output = _fixup_icall_esp_save(lines)
    save = output.index("    { uint32_t _icall_esp = g_esp;")
    first_arg = output.index("    PUSH32(esp, 0x22);")

    assert save < first_arg


def test_later_basic_block_argument_push_is_not_treated_as_prologue_save():
    lines = [
        "loc_00100000: ;",
        "    esp = esp - 8;",
        "    PUSH32(esp, esi);",
        "    esi = ecx;",
        "    if (TEST_Z(eax, eax)) goto loc_00100020;",
        "",
        "loc_00100020: ;",
        "    eax = MEM32(esi + 0xC);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax), _icall_esp);",
        "    POP32(esp, esi);",
    ]

    output = _fixup_icall_esp_save(lines)
    save = output.index("    { uint32_t _icall_esp = g_esp;")
    argument = max(i for i, line in enumerate(output)
                   if line == "    PUSH32(esp, esi);")

    assert save < argument


def test_second_nonvolatile_push_in_entry_block_is_an_argument():
    lines = [
        "loc_00100000: ;",
        "    esp = esp - 8;",
        "    PUSH32(esp, esi);",
        "    esi = ecx;",
        "    PUSH32(esp, edi);",
        "    edi = ecx;",
        "    eax = MEM32(edi);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax), _icall_esp);",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
    ]

    output = _fixup_icall_esp_save(lines)
    save = output.index("    { uint32_t _icall_esp = g_esp;")
    argument = max(i for i, line in enumerate(output)
                   if line == "    PUSH32(esp, esi);")

    assert save < argument


def test_icall_save_follows_full_interleaved_aligned_frame_prologue():
    """Mirror retail sub_00173A60, whose first ICALL exposed this bug."""
    lines = [
        "loc_00173A60: ;",
        "    PUSH32(esp, ebp);",
        "    ebp = esp;",
        "    esp = esp & 0xFFFFFFF0u;",
        "    esp = esp - 0x54;",
        "    PUSH32(esp, ebx);",
        "    PUSH32(esp, esi);",
        "    esi = MEM32(ebp + 8);",
        "    eax = MEM32(esi);",
        "    PUSH32(esp, edi);",
        "    edi = ecx;",
        "    PUSH32(esp, 0x8D39BDE6u);",
        "    ecx = esi;",
        "    MEM32(esp + 0x20) = edi;",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0xC), _icall_esp);",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
        "    POP32(esp, ebx);",
        "    esp = ebp;",
        "    POP32(esp, ebp);",
    ]

    output = _fixup_icall_esp_save(lines)
    save = output.index("    { uint32_t _icall_esp = g_esp;")
    frame_edi = output.index("    PUSH32(esp, edi);")
    first_argument = output.index("    PUSH32(esp, 0x8D39BDE6u);")
    entry_frame = output.index("    PUSH32(esp, ebp);")

    assert entry_frame < frame_edi < save < first_argument
    assert output.count("    { uint32_t _icall_esp = g_esp;") == 1


def test_icall_save_follows_delayed_callee_save_across_fallthrough_labels():
    """Mirror retail sub_000574B0's delayed EDI save and two virtual calls."""
    lines = [
        "loc_000574B0: ;",
        "    PUSH32(esp, esi);",
        "    ecx = 0x3217F8;",
        "    PUSH32(esp, 0); sub_000573A0();",
        "loc_000574BB: ;",
        "    esi = eax;",
        "    if (TEST_Z(esi, esi)) goto loc_000574DA;",
        "loc_000574C1: ;",
        "    eax = MEM32(esi);",
        "    PUSH32(esp, edi);",
        "    edi = MEM32(esp + 0xC);",
        "    PUSH32(esp, edi);",
        "    ecx = esi;",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C), _icall_esp);",
        "loc_000574CE: ;",
        "    edx = MEM32(esi);",
        "    PUSH32(esp, edi);",
        "    ecx = esi;",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x1BC), _icall_esp);",
        "loc_000574D9: ;",
        "    POP32(esp, edi);",
        "loc_000574DA: ;",
        "    eax = esi;",
        "    POP32(esp, esi);",
        "    esp += 4; return;",
    ]

    output = _fixup_icall_esp_save(lines)
    saves = [i for i, line in enumerate(output)
             if line == "    { uint32_t _icall_esp = g_esp;"]
    frame_edi = output.index("    PUSH32(esp, edi);")
    first_argument = next(i for i, line in enumerate(output[frame_edi + 1:],
                                                      frame_edi + 1)
                          if line == "    PUSH32(esp, edi);")
    first_call = next(i for i, line in enumerate(output)
                      if "MEM32(eax + 0x1C)" in line)

    assert frame_edi < saves[0] < first_argument < first_call
    assert len(saves) == 2

def test_first_icall_retains_mid_function_ebx_save_before_snapshot():
    """Mirror retail sub_001C2580: EBX is saved, passed, then restored."""
    lines = [
        "loc_001C2580: ;",
        "    esp = esp - 0xC;",
        "    PUSH32(esp, esi);",
        "    esi = ecx;",
        "    PUSH32(esp, edi);",
        "loc_001C2592: ;",
        "    ecx = MEM32(esi + 0x20);",
        "    eax = MEM32(ecx);",
        "    PUSH32(esp, ebx);",
        "    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x14), _icall_esp);",
        "loc_001C2610: ;",
        "    POP32(esp, ebx);",
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
    ]

    output = _fixup_icall_esp_save(lines)
    save = output.index("    { uint32_t _icall_esp = g_esp;")
    saved_ebx = output.index("    PUSH32(esp, ebx);")
    call = next(i for i, line in enumerate(output) if "RECOMP_ICALL_SAFE" in line)

    assert saved_ebx < save < call


if __name__ == "__main__":
    test_icall_save_stays_after_aligned_seh_prologue_and_saved_registers()
    test_icall_save_still_precedes_interleaved_argument_pushes()
    test_later_basic_block_argument_push_is_not_treated_as_prologue_save()
    test_second_nonvolatile_push_in_entry_block_is_an_argument()
    test_icall_save_follows_full_interleaved_aligned_frame_prologue()
    test_icall_save_follows_delayed_callee_save_across_fallthrough_labels()
    test_first_icall_retains_mid_function_ebx_save_before_snapshot()
    print("ok  icall_stack_save_placement")
