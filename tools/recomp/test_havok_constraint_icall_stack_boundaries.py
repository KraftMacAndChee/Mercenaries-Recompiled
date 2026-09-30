from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)


def test_breakable_constraint_keeps_saved_ebx_on_guest_stack() -> None:
    start = PATCHER.index(
        '"Havok breakable constraint preserves mid-function EBX save"'
    )
    body = PATCHER[start : start + 1000]
    push = body.rindex("PUSH32(esp, ebx);")
    snapshot = body.rindex("uint32_t _icall_esp = g_esp;")
    assert push < snapshot


def test_wheel_constraint_keeps_seh_frame_outside_icall_cleanup() -> None:
    start = PATCHER.index(
        '"Havok wheel constraint keeps SEH prologue outside ICALL cleanup"'
    )
    body = PATCHER[start : start + 2600]
    seh_head = body.rindex("PUSH32(esp, 0xFFFFFFFFu);")
    saved_esi = body.rindex("PUSH32(esp, esi);")
    snapshot = body.rindex("uint32_t _icall_esp = g_esp;")
    assert seh_head < saved_esi < snapshot


def test_motion_setter_keeps_aligned_seh_frame_outside_icall_cleanup() -> None:
    start = PATCHER.index(
        '"Havok rigid-body motion setter keeps aligned SEH prologue outside ICALL cleanup"'
    )
    body = PATCHER[start : start + 3200]
    aligned_frame = body.rindex("esp = esp & 0xFFFFFFF0u;")
    saved_esi = body.rindex("PUSH32(esp, esi);")
    snapshot = body.rindex("uint32_t _icall_esp = g_esp;")
    assert aligned_frame < saved_esi < snapshot


if __name__ == "__main__":
    test_breakable_constraint_keeps_saved_ebx_on_guest_stack()
    test_wheel_constraint_keeps_seh_frame_outside_icall_cleanup()
    test_motion_setter_keeps_aligned_seh_frame_outside_icall_cleanup()
    print("havok constraint ICALL stack-boundary tests passed")