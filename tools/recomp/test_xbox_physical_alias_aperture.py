"""Regression coverage for the Xbox KSEG0/KSEG1 64 MiB RAM aliases."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
LAYOUT = ROOT / "src/kernel/xbox_memory_layout.c"
MAIN = ROOT / "ports/mercenaries/src/main.c"


class XboxPhysicalAliasApertureTests(unittest.TestCase):
    def test_full_direct_mapped_cpu_aperture_is_demand_mapped(self):
        source = LAYOUT.read_text(encoding="utf-8")
        main = MAIN.read_text(encoding="utf-8")

        self.assertIn("#define XBOX_CPU_ALIAS_BASE 0x80000000u", source)
        self.assertIn("#define XBOX_CPU_ALIAS_END  0xC0000000u", source)
        self.assertIn(
            "block_index = (alias_base - XBOX_CPU_ALIAS_BASE) / "
            "XBOX_ALIAS_BLOCK_SIZE;",
            source,
        )
        self.assertIn(
            "xbox_va >= XBOX_CPU_ALIAS_BASE && xbox_va < XBOX_CPU_ALIAS_END",
            source,
        )
        self.assertIn("fault_xbox_va < 0xC0000000u", main)

    def test_crash_address_wraps_to_the_retail_64_mib_backing(self):
        crash_pointer = 0x852D8804
        physical = crash_pointer & (64 * 1024 * 1024 - 1)
        self.assertEqual(physical, 0x012D8804)

        # Repeated physical pages still need separate host virtual views.
        first_alias = 0x812D0000
        second_alias = 0x852D0000
        block_size = 0x10000
        first_slot = (first_alias - 0x80000000) // block_size
        second_slot = (second_alias - 0x80000000) // block_size
        self.assertNotEqual(first_slot, second_slot)
        self.assertEqual(first_alias & 0x03FF0000, second_alias & 0x03FF0000)


if __name__ == "__main__":
    unittest.main()
