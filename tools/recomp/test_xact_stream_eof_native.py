"""Execute the production EOF branch against observed short-voice slot states."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]


class StreamEOFTests(unittest.TestCase):
    def test_fix_is_durable_and_scoped_to_eof(self):
        generated = generated_text_containing(
            'recomp_xact_stream_has_unsubmitted_read(MEM32(esi + 0x40))'
        )
        patcher = (ROOT / 'ports/mercenaries/scripts/Patch-Generated.py').read_text(encoding='utf-8')
        predicate = 'recomp_xact_stream_has_unsubmitted_read(MEM32(esi + 0x40))'
        self.assertEqual(generated.count(predicate), 1)
        self.assertIn(predicate, patcher)
        self.assertIn(predicate, generated[generated.index('loc_00285CDF: ;'):generated.index('loc_00285CE6: ;')])

    def test_completed_read_survives_eof_until_submission(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        start = source.index('int recomp_xact_stream_has_unsubmitted_read(')
        helper = source[start:source.index('\n}\n', start) + 3]
        generated = generated_text_containing(
            'recomp_xact_stream_has_unsubmitted_read(MEM32(esi + 0x40))'
        )
        branch = generated[generated.index('loc_00285CDF: ;'):generated.index('loc_00285CE6: ;')]
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t memory[0x4000000];
#define MEM32(p) (*(uint32_t *)(memory + (p)))
#define MEM8(p) memory[p]
#define TEST_NZ(a,b) (((a)&(b)) != 0)
#define recomp_xact_sound_update_checkpoint(a,b,c,d) ((void)0)
static uint32_t guest_u32(uint32_t p) {
    assert(p >= 0x10000 && p <= sizeof(memory) - 4); return MEM32(p);
}
'''
        harness = r'''
int main(void) {
    const uint32_t track=0x10000, stream=0x12000;
    MEM32(track+0x40)=stream;
    /* Run490 FIOPIA01: one successful read, other slot EOF/DEAD. */
    MEM32(stream+0x10)=0; MEM32(stream+0x14)=0x3BC4;
    MEM32(stream+0x18)=0xAAAA; MEM32(stream+0x1C)=0;
    MEM32(stream+0x30)=0x8000000A; MEM32(stream+0x38)=0xDEAD;
    uint8_t before[0x50]; memcpy(before,memory+stream,sizeof(before));
    MEM8(track)=0xD; eof(track,0); assert(MEM8(track)==0xD);
    assert(!memcmp(before,memory+stream,sizeof(before)));
    MEM32(stream+0x1C)=0x103; eof(track,1); assert(MEM8(track)==0xD);
    /* Submitted/completed, failed, empty, and dead reads may retire. */
    MEM32(stream+0x18)=0xFFFF; MEM32(stream+0x10)=0;
    MEM8(track)=0xD; eof(track,0); assert(MEM8(track)==5);
    MEM32(stream+0x18)=0xAAAA; MEM32(stream+0x1C)=0xC0000001;
    MEM8(track)=0xD; eof(track,0); assert(MEM8(track)==5);
    MEM32(stream+0x1C)=0; MEM32(stream+0x14)=0;
    MEM8(track)=0xD; eof(track,0); assert(MEM8(track)==5);
    MEM32(stream+0x18)=0xDEAD;
    MEM32(stream+0x38)=0xAAAA; MEM32(stream+0x34)=99; MEM32(stream+0x3C)=0;
    MEM8(track)=0xD; eof(track,0); assert(MEM8(track)==0xD);
    assert(!recomp_xact_stream_has_unsubmitted_read(0));
    assert(!recomp_xact_stream_has_unsubmitted_read(0x3FFFFFF));
    return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-stream-eof-') as tmp:
            path = Path(tmp)
            for mode in ('fixed', 'old'):
                actual = branch
                if mode == 'old':
                    actual = actual.replace(' ||\n        recomp_xact_stream_has_unsubmitted_read(MEM32(esi + 0x40))', '')
                wrapper = 'static void eof(uint32_t esi,uint32_t eax) {\n' + actual + '\nloc_00285CE6: return;\n}\n'
                (path / 'fixture.c').write_text(prelude + helper + wrapper + harness, encoding='utf-8')
                subprocess.run([compiler, str(path / 'fixture.c'), '-o', str(path / 'fixture.exe')], check=True)
                result = subprocess.run([str(path / 'fixture.exe')], capture_output=True)
                if mode == 'fixed': self.assertEqual(result.returncode, 0, result.stderr)
                else: self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
