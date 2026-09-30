from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADERS = (
    ROOT / "ports/mercenaries/src/recomp/recomp_types.h",
    ROOT / "templates/runtime/recomp_types.h",
)
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c"
TRANSIENT_GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0003.c"
FRONTEND_GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0004.c"


def test_global_indirect_calls_do_not_override_split_function_register_flow() -> None:
    for header in HEADERS:
        text = header.read_text(encoding="utf-8")
        start = text.index("#define RECOMP_ICALL_SAFE")
        end = text.index("\n\n/**\n * RECOMP_ITAIL", start)
        macro = text[start:end]

        for register in ("ebx", "esi", "edi"):
            assert f"const uint32_t _saved_{register} = g_{register};" not in macro
def test_indirect_calls_do_not_rewind_valid_caller_cleaned_arguments() -> None:
    """Resolved cdecl targets leave their argument area for the caller."""
    retail = (ROOT / "game_files/mercenaries-retail/default.xbe").read_bytes()
    call_bytes = retail[0x000460DC:0x0004612F]
    assert bytes.fromhex(
        "6a016a00"              # push 1; push 0
        "89864c0800008b16"    # state update; load vtable
        "6a018bce"            # push 1; this = esi
        "ff9224010000"        # call [edx + 0x124]
    ) in call_bytes
    assert bytes.fromhex("83c41c") in call_bytes  # add esp, 0x1c
    assert retail[0x00044900:0x00044907] == bytes.fromhex(
        "8b816c070000c3"        # mov eax,[ecx+0x76c]; ret
    )

    for header in HEADERS:
        text = header.read_text(encoding="utf-8")
        start = text.index("#define RECOMP_ICALL_SAFE")
        end = text.index("\n\n/**\n * RECOMP_ITAIL", start)
        macro = text[start:end]
        assert 'strcmp(__FUNCTION__, "sub_00056000")' not in macro
        assert "_human_update_stack_mismatch" not in macro

def test_human_general_nested_callback_preserves_actor_esi() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    assert '"HumanGeneral virtual update ESI guard"' in patcher
    assert '"HumanGeneral update nested callback ESI guard"' in patcher
    assert "const uint32_t _human_general_virtual_esi = esi;" in generated
    assert "recomp_update_esi_checkpoint(0x000539ABu" in generated
    assert "esi = _human_general_virtual_esi;" in generated
    assert "const uint32_t _human_general_esi = esi;" in generated
    assert "recomp_update_esi_checkpoint(0x000538F0u" in generated
    assert "esi = _human_general_esi;" in generated


def test_transient_update_callback_preserves_its_live_nonvolatiles() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = TRANSIENT_GENERATED.read_text(encoding="utf-8")
    assert '"retail transient updater virtual callback ABI guard"' in patcher
    assert "const uint32_t _transient_esi = esi;" in generated
    assert "const uint32_t _transient_edi = edi;" in generated
    assert "const uint32_t _transient_esp = esp;" in generated
    assert "recomp_transient_update_abi_checkpoint(" in generated
    assert "esi = _transient_esi;" in generated
    assert "edi = _transient_edi;" in generated
    assert "esp = _transient_esp;" in generated


def test_human_animation_pointer_is_traced_around_update_tail() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    assert patcher.count("HumanGeneral animation pointer") >= 6
    assert generated.count("recomp_human_animation_pointer_checkpoint") >= 6
    assert '"HumanGeneral vtable 3EC callback nonvolatile guard"' in patcher
    assert "const uint32_t _human_3ec_saved_esi = esi;" in generated
    assert "recomp_update_esi_checkpoint(0x00053A6Bu" in generated
    assert '"HumanGeneral pose-state virtual callback nonvolatile guard"' in patcher
    assert "const uint32_t _human_pose_saved_ebx = ebx;" in generated
    assert "const uint32_t _human_pose_saved_esi = esi;" in generated
    assert "const uint32_t _human_pose_saved_edi = edi;" in generated
    assert "recomp_update_esi_checkpoint(0x00053A80u" in generated


def test_retail_head_tweak_has_opt_in_state_trace() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
        encoding="utf-8"
    )
    assert '"retail human head tweak state diagnostics"' in patcher
    assert '"retail human head tweak result diagnostics"' in patcher
    assert generated.count("recomp_human_head_checkpoint") == 2
    assert 'getenv("MERCENARIES_TRACE_HUMAN_HEAD")' in manual


def test_retail_head_step_clamps_after_both_approach_directions() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    assert '"retail StepToValue merged-comiss head clamp"' in patcher
    function = generated[
        generated.index("void sub_000624E0(void)") : generated.index(
            "void sub_000626B0(void)"
        )
    ]
    assert "loc_0006262F" not in function
    increasing = function[
        function.index("loc_0006261A: ;") : function.index("loc_00062623: ;")
    ]
    decreasing = function[
        function.index("loc_00062628: ;") : function.index("loc_00062631: ;")
    ]
    assert "xmm0 <= xmm1" in increasing
    assert "goto loc_00062631" in increasing
    assert "xmm1 <= xmm0" in decreasing
    vertical = generated[
        generated.index("loc_00062857: ;") : generated.index("loc_00062860: ;")
    ]
    assert "MEMF(esp + 4) = xmm0" in vertical
    assert "xmm0 <= xmm1" in vertical
    assert "goto loc_00062874" in vertical

def test_frontend_shell_callbacks_preserve_live_nonvolatiles() -> None:
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = FRONTEND_GENERATED.read_text(encoding="utf-8")
    assert '"frontend shell callback ABI guards"' in patcher
    assert "uint32_t frontend_shell_this = 0u;" in generated
    assert "frontend_shell_this = ebx;" in generated
    assert "loc_000D53E4: ;\n    ebx = frontend_shell_this;" in generated

    for call_site, return_site in (
        ("000D53FE", "000D540C"),
        ("000D541D", "000D542B"),
        ("000D5460", "000D5473"),
        ("000D547E", "000D548C"),
    ):
        call = generated.index(f"loc_{call_site}: ;")
        returned = generated.index(f"loc_{return_site}: ;", call)
        call_body = generated[call:returned]
        return_body = generated[returned : returned + 220]
        for register in ("ebx", "esi", "edi"):
            restore = f"{register} = frontend_loop_saved_{register};"
            assert f"frontend_loop_saved_{register} = {register};" in call_body
            assert restore in call_body
            assert restore not in return_body

if __name__ == "__main__":
    test_global_indirect_calls_do_not_override_split_function_register_flow()
    test_human_general_nested_callback_preserves_actor_esi()
    test_transient_update_callback_preserves_its_live_nonvolatiles()
    test_human_animation_pointer_is_traced_around_update_tail()
    test_retail_head_tweak_has_opt_in_state_trace()
    test_retail_head_step_clamps_after_both_approach_directions()
    test_frontend_shell_callbacks_preserve_live_nonvolatiles()
    print("targeted HumanGeneral nonvolatile preservation: ok")
