"""Discovery must not parse fixture CLI arguments or launch native work."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeFixtureImportTests(unittest.TestCase):
    def test_import_is_independent_of_cli_and_native_prerequisites(self):
        code = r'''
import runpy
import sys
from unittest.mock import patch
path = sys.argv[1]
sys.argv = ["pytest", "-q", "--not-a-fixture-option"]
with patch("subprocess.run", side_effect=AssertionError("native work during import")), \
     patch("tempfile.TemporaryDirectory", side_effect=AssertionError("fixture allocation during import")):
    module = runpy.run_path(path)
assert callable(module["main"])
'''
        for name in ("test_cpu_frame_upload_native.py", "test_datapod_intel_native.py"):
            with self.subTest(module=name):
                result = subprocess.run(
                    [sys.executable, "-X", "utf8", "-B", "-c", code,
                     str(ROOT / "tools/recomp" / name)],
                    cwd=ROOT, capture_output=True, text=True, timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
