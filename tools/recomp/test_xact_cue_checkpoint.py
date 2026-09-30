#!/usr/bin/env python3
"""Regression coverage for retail RedXactManager cue-play tracing."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports" / "mercenaries" / "scripts" / "Patch-Generated.py").read_text(encoding="utf-8")
MANUAL = (ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c").read_text(encoding="utf-8")
GENERATED = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0011.c").read_text(encoding="utf-8")
STUBS = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_stubs_unresolved.c").read_text(encoding="utf-8")
SOUND_GENERATED = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0010.c").read_text(encoding="utf-8")

assert '"XACT cue play diagnostics"' in PATCHER
assert "recomp_xact_cue_checkpoint(1u, edi, esi, 0u);" in PATCHER
assert "recomp_xact_cue_checkpoint(2u, edi, esi, eax);" in PATCHER
assert "void recomp_xact_cue_checkpoint" in MANUAL
assert "void recomp_xact_alloc_checkpoint" in MANUAL
assert "started=%u peak=%u" in MANUAL
assert "void recomp_xact_setup_checkpoint" in MANUAL
assert "void recomp_xact_play_checkpoint" in MANUAL
assert "void recomp_xact_managed_checkpoint" in MANUAL
assert "void recomp_xact_managed_update_checkpoint" in MANUAL
assert "int recomp_xact_low_level_handle_exists" in MANUAL
assert "int recomp_xact_low_level_handle_exists" in (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "recomp_types.h").read_text(encoding="utf-8")
assert "recomp_xact_managed_update_checkpoint(esi);" in PATCHER
assert '"RedXactCue stale managed-handle release"' in PATCHER
assert "MEM32(esi + 8) != 0u && MEMF(esi + 0x40) == 0.0f" in PATCHER
assert "MEMF(esi + 0x44) <= 0.0f &&" in PATCHER
assert "!recomp_xact_low_level_handle_exists(MEM32(esi + 8))" in PATCHER
assert '"[XACT-MANAGED-STALE]' in MANUAL
assert '"pool=%u/%u next=%u active=%u states=%u/%u/%u/%u/%u "' in MANUAL
assert 'cue_name == 0x6E3A9CDAu || cue_name == 0x7240BE8Au' in MANUAL
assert 'getenv("MERCENARIES_TRACE_WEAPON_FIRE_SOUND")' in MANUAL
assert '"map=%u/%08X/%08X default=%08X manager=%08X "' in MANUAL
for address in ("0x00690B3Cu", "0x00690B48u", "0x00690B44u", "0x00690B4Cu", "0x00690AC0u"):
    assert address in MANUAL
assert "recomp_xact_managed_checkpoint(1u" in PATCHER
assert "recomp_xact_managed_checkpoint(2u" in PATCHER
assert "recomp_xact_managed_checkpoint(3u" in PATCHER
for stage in range(10, 14):
    assert f"recomp_xact_managed_checkpoint({stage}u" in PATCHER
assert "recomp_xact_managed_checkpoint(1u" in (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0010.c").read_text(encoding="utf-8")
assert "recomp_xact_play_checkpoint(1u" in PATCHER
assert "recomp_xact_play_checkpoint(2u" in PATCHER
assert "recomp_xact_play_checkpoint(3u" in PATCHER
assert "stage == 3u ? recomp_xact_low_level_handle_exists(result)" in MANUAL
for stage in range(10, 15):
    assert f"recomp_xact_setup_checkpoint({stage}u" in PATCHER
    assert f"recomp_xact_setup_checkpoint({stage}u" in GENERATED
assert 'getenv("MERCENARIES_TRACE_XACT_CUES")' in MANUAL
assert 'getenv("MERCENARIES_TRACE_XACT_CUES_AFTER_MOVIE")' in MANUAL
assert 'getenv("MERCENARIES_TRACE_XACT_FAILURES")' in MANUAL
assert 'getenv("MERCENARIES_TRACE_XACT_INTERNAL_FAILURES")' in MANUAL
assert "stage == 1u && object == 0u" in MANUAL
assert "stage == 2u || stage == 20u || stage == 21u" in MANUAL
assert "g_trace_xact_cues_after_movie_active = 1;" in MANUAL
assert '"[XACT-CUE] trace enabled after movie close\\n"' in MANUAL
for field in ("cue + 0x08u", "cue + 0x14u", "cue + 0x0Cu", "cue + 0x18u", "cue + 0x1Cu", "cue + 0x28u", "cue + 0x34u"):
    assert field in MANUAL
assert '"state=%u handle=%08X pCue=%08X source=%u spatial=%08X "' in MANUAL
assert "guest_f32(cue + 0x28u)" in MANUAL
assert "guest_f32(cue + 0x2Cu)" in MANUAL
assert "recomp_xact_cue_checkpoint(1u, edi, esi, 0u);" in GENERATED
assert "recomp_xact_cue_checkpoint(2u, edi, esi, eax);" in GENERATED
for checkpoint in (
    "recomp_xact_cue_checkpoint(3u, esi, edi, MEM32(esp + 0x50));",
    "recomp_xact_cue_checkpoint(4u, edi, esi, MEM32(esp + 0x28));",
    "recomp_xact_cue_checkpoint(5u, edi, esi, eax);",
):
    assert checkpoint in GENERATED and checkpoint in PATCHER
assert 'getenv("MERCENARIES_TRACE_XACT_RETIRE")' in MANUAL
ALLOC_GENERATED = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0012.c").read_text(encoding="utf-8")
assert '"XACT variation selector CMP/SBB carry preservation"' in PATCHER
assert "_cf = ((uint32_t)(eax) < (uint32_t)(ecx)); /* preserve cmp carry across 1 instruction(s) */" in PATCHER
assert "_cf = ((uint32_t)(eax) < (uint32_t)(ecx)); /* preserve cmp carry across 1 instruction(s) */" in ALLOC_GENERATED
INIT_GENERATED = "\n".join(
    path.read_text(encoding="utf-8")
    for path in sorted((ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen").glob("recomp_*.c"))
)
assert '"XACT event heap capacity for recomp scheduling"' in PATCHER
assert "PUSH32(esp, 0x200);" in PATCHER
assert "PUSH32(esp, 0x200);" in ALLOC_GENERATED
for stage in range(1, 3):
    assert f"recomp_xact_alloc_checkpoint({stage}u" in PATCHER
    assert f"recomp_xact_alloc_checkpoint({stage}u" in ALLOC_GENERATED
for stage in range(20, 22):
    assert f"recomp_xact_alloc_checkpoint({stage}u" in PATCHER
    assert f"recomp_xact_alloc_checkpoint({stage}u" in ALLOC_GENERATED
for stage in range(30, 37):
    assert f"recomp_xact_alloc_checkpoint({stage}u" in PATCHER
    assert f"recomp_xact_alloc_checkpoint({stage}u" in INIT_GENERATED
for stage in range(40, 43):
    assert f"recomp_xact_alloc_checkpoint({stage}u" in PATCHER
    assert f"recomp_xact_alloc_checkpoint({stage}u" in INIT_GENERATED
for stage in range(3, 9):
    assert f"recomp_xact_alloc_checkpoint({stage}u" in PATCHER
    assert f"recomp_xact_alloc_checkpoint({stage}u" in INIT_GENERATED
assert '"RedSoundSystem omitted dataset wrappers"' in PATCHER
assert "void sub_001FFDA0(void)" in SOUND_GENERATED and "sub_001FE7D0();" in SOUND_GENERATED
assert "void sub_001FFDB0(void)" in SOUND_GENERATED and "sub_00227660();" in SOUND_GENERATED
assert '"RedSoundSystem omitted general bank loader wrapper"' in PATCHER
assert "void sub_001FFE20(void)" in SOUND_GENERATED
assert "ecx = 0x85C600;" in SOUND_GENERATED
assert "PUSH32(esp, 0); sub_00225B90();" in SOUND_GENERATED
assert "recomp_xact_bank_checkpoint(1u, ecx);" in SOUND_GENERATED
assert "recomp_xact_bank_checkpoint(2u, 0x85C600u);" in SOUND_GENERATED
assert 'getenv("MERCENARIES_TRACE_XACT_BANKS")' in MANUAL
MANAGED_GENERATED = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0010.c").read_text(encoding="utf-8")
assert "recomp_xact_play_checkpoint(3u, esi, MEM32(esi + 0x34), eax);" in MANAGED_GENERATED
assert "recomp_xact_managed_update_checkpoint(esi);" in MANAGED_GENERATED
assert "MEM32(esi + 8) != 0u && MEMF(esi + 0x40) == 0.0f" in MANAGED_GENERATED
assert "MEMF(esi + 0x44) <= 0.0f &&" in MANAGED_GENERATED
assert "!recomp_xact_low_level_handle_exists(MEM32(esi + 8))" in MANAGED_GENERATED
assert '"Empty dynamic hash lookup returns not found"' in PATCHER
assert 'MERCENARIES_TRACE_HASH_TABLE' in MANUAL
assert 'cue_name == guest_u32(0x00371DC8u)' in MANUAL
assert 'if (esi < 0x00010000u || esi >= 0x04000000u ||' in PATCHER
assert 'edx > (0x04000000u - esi - 4u) / 4u' in PATCHER
assert 'if (esi < 0x00010000u || esi >= 0x04000000u ||' in MANAGED_GENERATED
assert 'edx > (0x04000000u - esi - 4u) / 4u' in MANAGED_GENERATED
assert 'goto loc_001F2A51;' in MANAGED_GENERATED
assert "_flags = TEST_NZ(edi, edi); /* preserve shared jne source */" in MANAGED_GENERATED
assert MANAGED_GENERATED.count("_flags = TEST_NZ(edx, edx); /* generated handle feeds shared jne */") == 2
assert "/* std - direction flag */" in GENERATED
assert "edi -= 1u; --ecx;" in GENERATED
assert GENERATED.count("MEM32(edi - _i*4) = MEM32(esi - _i*4);") >= 2
assert GENERATED.count("esi -= ecx * 4; edi -= ecx * 4;") >= 2
assert "_flags = TEST_NZ(edi, edi); /* preserve shared jne source */" in GENERATED
assert GENERATED.count("_flags = TEST_NZ(edx, edx); /* generated handle feeds shared jne */") == 2

assert '"RedXactManager cue property diagnostics"' in PATCHER
assert "recomp_xact_properties_checkpoint(esi, esp + 4, eax);" in PATCHER
assert "recomp_xact_properties_checkpoint(esi, esp + 4, eax);" in GENERATED
assert "void recomp_xact_properties_checkpoint" in MANUAL

print("ok xact_cue_checkpoint")
