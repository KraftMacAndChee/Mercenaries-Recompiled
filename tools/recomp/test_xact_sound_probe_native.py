"""Execute the production read-only sound probe with bounded guest fixtures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]
MANUAL = ROOT / 'ports/mercenaries/src/recomp_manual.c'


class SoundProbeTests(unittest.TestCase):
    def test_probe_bounds_filter_and_no_guest_mutation(self):
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        if not Path(compiler).exists():
            self.skipTest('GCC required')
        source = MANUAL.read_text(encoding='utf-8')
        begin = source.index('void recomp_xact_sound_update_checkpoint(')
        function = source[begin:source.index('\n}\n', begin) + 3]
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t memory[0x4000000];
static void *guest_ptr(uint32_t p) { assert(p < sizeof(memory)); return memory + p; }
static uint32_t reads;
static int enabled;
static int g_trace_weapon_fire_seen;
static char *fake_getenv(const char *key) {
    if (!strcmp(key, "MERCENARIES_TRACE_XACT_SOUND_UPDATE")) return enabled ? "1" : NULL;
    if (!strcmp(key, "MERCENARIES_TRACE_XACT_SOUND_TARGET")) return enabled == 2 ? "0x11000" : NULL;
    if (!strcmp(key, "MERCENARIES_TRACE_XACT_WAVE_ID")) return enabled == 3 || enabled == 4 ? "0x400" : NULL;
    if (!strcmp(key, "MERCENARIES_TRACE_XACT_SOUND_AFTER_FIRE")) return enabled == 5 ? "1" : NULL;
    if (!strcmp(key, "MERCENARIES_TRACE_XACT_SOUND_LIMIT")) {
        static char *limits[] = {"16384", "999999", "0", "bad", "-1", "64oops", "1"};
        return enabled >= 6 && enabled <= 12 ? limits[enabled-6] : NULL;
    }
    return NULL;
}
#define getenv fake_getenv
static uint32_t guest_u32(uint32_t p) {
    uint32_t v; assert(p >= 0x10000 && p <= sizeof(memory) - 4);
    ++reads; memcpy(&v, memory + p, 4); return v;
}
static uint16_t guest_u16(uint32_t p) {
    uint16_t v; assert(p >= 0x10000 && p <= sizeof(memory) - 2);
    ++reads; memcpy(&v, memory + p, 2); return v;
}
static unsigned long long GetTickCount64(void) { return 1234; }
static void put(uint32_t p, uint32_t v) { memcpy(memory + p, &v, 4); }
'''
        harness = r'''
int main(int argc, char **argv) {
    assert(argc == 2); enabled = atoi(argv[1]);
    const uint32_t sound = 0x10000, track = 0x12000, packet_node = 0x13000;
    put(track + 4, 0x3FFFFFF); /* An invalid object must not be dereferenced. */
    put(track + 0x56, enabled == 3 ? 0x400 : 0x401);
    put(packet_node + 0x14, 0x14000);
    put(packet_node + 0x18, 0x11223344);
    put(packet_node + 0x1C, 0x55667788);
    put(0x14000 + 8, 0x9876);
    uint8_t before[0x5000]; memcpy(before, memory + 0x10000, sizeof(before));
    if (enabled == 5) {
        /* Waiting must consume neither guest reads nor the 8192-event cap. */
        for (int i = 0; i < 9000; ++i)
            recomp_xact_sound_update_checkpoint(100, sound, track, packet_node);
        assert(reads == 0);
        g_trace_weapon_fire_seen = 1;
    }
    recomp_xact_sound_update_checkpoint(100, sound, track, packet_node);
    recomp_xact_sound_update_checkpoint(101, sound, track, 0x80004005);
    recomp_xact_sound_update_checkpoint(100, 0x3FFFFFF, 0x3FFFFFF, 0x3FFFFFF);
    if (enabled >= 6)
        for (unsigned i = 0; i < 70000; ++i)
            recomp_xact_sound_update_checkpoint(100, 0x3FFFFFF, 0x3FFFFFF, 0);
    assert(!memcmp(before, memory + 0x10000, sizeof(before)));
    if (!enabled || enabled == 2) assert(reads == 0);
    else assert(reads > 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mercs-sound-probe-') as temp:
            path = Path(temp)
            c = path / 'probe.c'; exe = path / 'probe.exe'
            c.write_text(prelude + function + harness, encoding='utf-8')
            subprocess.run([compiler, '-std=c11', '-O0', str(c), '-o', str(exe)], check=True, capture_output=True)
            for mode in map(str, range(13)):
                result = subprocess.run([str(exe), mode], check=True, capture_output=True, text=True)
                if int(mode) >= 6:
                    limits = {6:16384, 7:65536, 8:8192, 9:8192, 10:8192, 11:8192, 12:1}
                    self.assertEqual(result.stderr.count('[XACT-SOUND-UPDATE]'), limits[int(mode)])
                elif mode in ('1', '3', '5'):
                    self.assertIn('due=55667788:11223344 packet=00014000', result.stderr)
                    self.assertIn('value=80004005', result.stderr)
                    self.assertIn('ms=1234', result.stderr)
                    if mode == '3':
                        self.assertEqual(result.stderr.count('[XACT-SOUND-UPDATE]'), 2)
                    if mode == '5':
                        self.assertEqual(result.stderr.count('[XACT-SOUND-UPDATE]'), 3)
                else:
                    self.assertEqual(result.stderr, '')

    def test_generated_hooks_are_durable(self):
        generated = generated_text_containing(
            'recomp_xact_sound_update_checkpoint(100u, edi, esi, MEM32(ebp + 0xC));'
        )
        patcher = (ROOT / 'ports/mercenaries/scripts/Patch-Generated.py').read_text(encoding='utf-8')
        for hook in ('recomp_xact_sound_update_checkpoint(100u, edi, esi, MEM32(ebp + 0xC));',
                     'recomp_xact_sound_update_checkpoint(101u, edi, esi, eax);',
                     'recomp_xact_sound_update_checkpoint(102u, MEM32(ebp + -12), esi, eax);',
                     'recomp_xact_sound_update_checkpoint(103u, MEM32(ebp + -4), esi, eax);',
                     'recomp_xact_sound_update_checkpoint(104u, MEM32(ebp + -12), esi, eax);'):
            self.assertIn(hook, generated)
            self.assertIn(hook, patcher)
        packets = generated_text_containing(
            'recomp_xact_alloc_checkpoint(50u, esi, MEM32(ebp + 8), 0u);'
        )
        for stage, result in ((50, '0u'), (51, 'eax')):
            hook = f'recomp_xact_alloc_checkpoint({stage}u, esi, MEM32(ebp + 8), {result});'
            self.assertIn(hook, packets)
            self.assertIn(hook, patcher)


if __name__ == '__main__':
    unittest.main()
