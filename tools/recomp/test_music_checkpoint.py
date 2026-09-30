from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(encoding="utf-8")
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
LUA = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0005.c").read_text(encoding="utf-8")
DJ = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0006.c").read_text(encoding="utf-8")

assert 'getenv("MERCENARIES_TRACE_MUSIC")' in MANUAL
assert "recomp_music_checkpoint(20u, 0x0037B9B4u, 0u);" in MANUAL
assert "recomp_music_checkpoint(1u, 0x0037B9B4u, eax);" in PATCHER
assert "recomp_music_checkpoint(2u, 0x0037B9B4u, MEM32(0x0037B9B8u));" in PATCHER
assert "recomp_music_checkpoint(10u, ecx, MEM32(esp + 4));" in PATCHER
assert "recomp_music_checkpoint(11u, esi, ebx);" in PATCHER
assert "recomp_music_checkpoint(1u, 0x0037B9B4u, eax);" in LUA
assert "recomp_music_checkpoint(2u, 0x0037B9B4u, MEM32(0x0037B9B8u));" in LUA
assert "recomp_music_checkpoint(10u, ecx, MEM32(esp + 4));" in DJ
assert "recomp_music_checkpoint(11u, esi, ebx);" in DJ

print("ok music_checkpoint")