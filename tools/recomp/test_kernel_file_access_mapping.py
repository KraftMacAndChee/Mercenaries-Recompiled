"""Regression coverage for portable Xbox file access-mask translation."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class KernelFileAccessMappingTests(unittest.TestCase):
    def test_xbox_generic_all_does_not_request_host_acl_administration(self):
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        match = re.search(
            r"static DWORD xbox_access_to_win32\(ACCESS_MASK Access\)\s*\{(.*?)\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        body = match.group(1)

        generic_all = re.search(
            r"if \(Access & XBOX_GENERIC_ALL\)\s*\n?\s*result \|= ([^;]+);",
            body,
        )
        self.assertIsNotNone(generic_all)
        mapped_rights = generic_all.group(1)
        self.assertNotIn("GENERIC_ALL", mapped_rights)
        self.assertIn("GENERIC_READ", mapped_rights)
        self.assertIn("GENERIC_WRITE", mapped_rights)
        self.assertIn("DELETE", mapped_rights)

    def test_specific_xbox_data_rights_remain_mapped(self):
        source = (ROOT / "src/kernel/kernel_file.c").read_text(encoding="utf-8")
        for right in (
            "XBOX_FILE_READ_DATA",
            "XBOX_FILE_WRITE_DATA",
            "XBOX_FILE_APPEND_DATA",
            "XBOX_FILE_READ_ATTRIBUTES",
            "XBOX_FILE_WRITE_ATTRIBUTES",
            "XBOX_SYNCHRONIZE",
            "XBOX_DELETE",
        ):
            self.assertIn(f"if (Access & {right})", source)


if __name__ == "__main__":
    unittest.main()
