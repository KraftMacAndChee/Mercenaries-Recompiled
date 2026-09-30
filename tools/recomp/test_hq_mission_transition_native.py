"""Execute the diagnostic HQ transition predicate; no guest state writes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HQTransitionTests(unittest.TestCase):
    def test_return_requires_interior_and_actual_starter_input(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        start = source.index('static int recomp_test_hq_returned_to_world(')
        body = source[start:source.index('\n}\n', start) + 3]
        self.assertNotIn('guest_', body)
        self.assertNotIn('starter removed after use', source)
        harness = r'''
#include <assert.h>
#include <math.h>
''' + body + r'''
int main(void) {
    assert(!recomp_test_hq_returned_to_world(0, 0, 1489.342f, 845.322f));
    assert(!recomp_test_hq_returned_to_world(0, 1, 1489.342f, 845.322f));
    assert(!recomp_test_hq_returned_to_world(1, 0, 1489.342f, 845.322f));
    assert(!recomp_test_hq_returned_to_world(1, 1, 2346.517f, -2.540522f));
    assert(recomp_test_hq_returned_to_world(1, 1, 1489.342f, 845.322f));
    assert(!recomp_test_hq_returned_to_world(1, 1, NAN, 845.322f));
    assert(!recomp_test_hq_returned_to_world(1, 1, 1489.342f, INFINITY));
    assert(!recomp_test_hq_returned_to_world(1, 1, 1489.342f, 1800.0f));
    return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-hq-transition-') as tmp:
            path = Path(tmp)
            (path / 'fixture.c').write_text(harness, encoding='utf-8')
            subprocess.run([compiler, str(path / 'fixture.c'), '-o', str(path / 'fixture.exe')], check=True)
            subprocess.run([str(path / 'fixture.exe')], check=True)


if __name__ == '__main__':
    unittest.main()
