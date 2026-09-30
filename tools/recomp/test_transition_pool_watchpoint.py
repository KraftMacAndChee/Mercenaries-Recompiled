from pathlib import Path
import importlib.util
import sys


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "ports/mercenaries/src/main.c"
TYPES = ROOT / "ports/mercenaries/src/recomp/recomp_types.h"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c"


def main() -> None:
    main_source = MAIN.read_text(encoding="utf-8")
    assert "MERCENARIES_TRACE_TRANSITION_POOL_COUNT" in main_source
    assert "[TRANSITION-POOL-WATCH]" in main_source
    manual = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(
        encoding='utf-8'
    )
    assert '[TRANSITION-RESET-ICALL]' in manual
    assert '_target_trace_va == 0x00043A60u' in TYPES.read_text(
        encoding='utf-8'
    )
    assert "if (value > 32u)" in main_source
    assert '[TRANSITION-POOL-RESET]' in main_source
    assert 'recomp_transition_pool_reset_checkpoint(ecx);' in GENERATED.read_text(
        encoding='utf-8'
    )

    generated = GENERATED.read_text(encoding="utf-8")
    constructor = generated[generated.index("void sub_00043900(void)") :]
    constructor = constructor[: constructor.index("void sub_00043A60(void)")]
    initializer = constructor.index("sub_00203D00();")
    watch = constructor.index(
        "recomp_arm_transition_pool_watchpoint(esi + 0x6E8);"
    )
    assert initializer < watch
    assert "recomp_arm_transition_pool_watchpoint" in TYPES.read_text(
        encoding="utf-8"
    )
    patch_path = ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'
    spec = importlib.util.spec_from_file_location('transition_patch_test', patch_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    original = ('loc_00043957: ;\n    work();\n\n'
                'loc_00043AB2: ;\n    ecx = esi + 0x6E8;\n    work();\n')
    patched = module.trace_transition_pool(original)
    assert patched.count('recomp_arm_transition_pool_watchpoint(') == 1
    assert patched.count('recomp_transition_pool_reset_checkpoint(') == 1
    assert module.trace_transition_pool(patched) == patched
    assert module.trace_transition_pool('unrelated text') == 'unrelated text'
    try:
        module.trace_transition_pool('loc_00043AB2: ;\n    changed();\n')
    except RuntimeError:
        pass
    else:
        raise AssertionError('Changed probe address setup must fail closed')
    print("Transition pool watchpoint checks passed")


if __name__ == "__main__":
    main()
