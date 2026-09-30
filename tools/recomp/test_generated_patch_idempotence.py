"""Reapply the complete patch pipeline only to an isolated generated copy."""
from pathlib import Path
import runpy
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GeneratedPatchIdempotenceTests(unittest.TestCase):
    def test_current_tree_preserved_and_second_pass_byte_stable(self):
        source = ROOT / 'ports/mercenaries/src/recomp/gen'
        patch = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['patch_generated']
        with tempfile.TemporaryDirectory(prefix='merc-patch-idempotence-') as directory:
            target = Path(directory) / 'gen'
            shutil.copytree(source, target)
            shutil.copy2(source.parent / 'recomp_types.h', target.parent / 'recomp_types.h')
            results = patch(target, allow_missing=True)
            self.assertGreater(len(results), 500)
            # Existing mixed CRLF/LF files may be normalized, but not their C text.
            for original in source.iterdir():
                if original.is_file():
                    self.assertEqual(original.read_text(), (target / original.name).read_text(), original.name)
            self.assertEqual((source.parent / 'recomp_types.h').read_text(),
                             (target.parent / 'recomp_types.h').read_text())
            first = {p.relative_to(target.parent): p.read_bytes()
                     for p in target.parent.rglob('*') if p.is_file()}
            patch(target, allow_missing=True)
            second = {p.relative_to(target.parent): p.read_bytes()
                      for p in target.parent.rglob('*') if p.is_file()}
            self.assertEqual(first, second)


if __name__ == '__main__':
    unittest.main()
