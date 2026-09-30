"""Exercise the real opt-in gamepad parser, expiry, and capture handoff."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t ULONGLONG;
typedef uint16_t WORD;
typedef int16_t SHORT;
typedef struct { uint32_t dwPacketNumber; struct {
    WORD wButtons; uint8_t bAnalogButtons[8];
    SHORT sThumbLX,sThumbLY,sThumbRX,sThumbRY;
} Gamepad; } XBOX_INPUT_STATE;
static const char *path;
static unsigned captures;
static const char *fake_getenv(const char *key) {
    if (!strcmp(key,"MERCENARIES_TEST_GAMEPAD_FILE")) return path;
    assert(!strcmp(key,"MERCENARIES_CAPTURE_TEST_GAMEPAD_PREFIX"));
    return "capture";
}
#define getenv fake_getenv
static void d3d8_DebugQueueFrameCapture(const char *value) {
    assert(!strcmp(value,"capture-000001.bmp")); ++captures;
}
static void command(const char *value) {
    FILE *f=fopen(path,"w"); assert(f); fputs(value,f); fclose(f);
}
static void neutral(const XBOX_INPUT_STATE *s) {
    unsigned i; assert(!s->Gamepad.wButtons);
    for(i=0;i<8;++i) assert(!s->Gamepad.bAnalogButtons[i]);
    assert(!s->Gamepad.sThumbLX && !s->Gamepad.sThumbLY);
    assert(!s->Gamepad.sThumbRX && !s->Gamepad.sThumbRY);
}
'''
HARNESS = r'''
int main(int argc,char **argv) {
    XBOX_INPUT_STATE s, before;
    assert(argc==3); memset(&s,0x12,sizeof(s)); before=s;
    path=atoi(argv[2]) ? argv[1] : NULL;
    assert(!recomp_apply_test_gamepad_command(&s,100));
    assert(!memcmp(&s,&before,sizeof(s)));
    if(!path) return 0;
    command("1 1 500 02 89 -32768 32767 123 -456 1\n");
    assert(recomp_apply_test_gamepad_command(&s,1000));
    assert(s.Gamepad.wButtons==2);
    assert(s.Gamepad.bAnalogButtons[0]==255 && s.Gamepad.bAnalogButtons[3]==255);
    assert(s.Gamepad.bAnalogButtons[7]==255 && !s.Gamepad.bAnalogButtons[1]);
    assert(s.Gamepad.sThumbLX==-32768 && s.Gamepad.sThumbLY==32767);
    assert(s.Gamepad.sThumbRX==123 && s.Gamepad.sThumbRY==-456);
    assert(!captures);
    assert(recomp_apply_test_gamepad_command(&s,1500)); neutral(&s);
    assert(!captures);
    assert(recomp_apply_test_gamepad_command(&s,1700)); neutral(&s); assert(captures==1);
    assert(recomp_apply_test_gamepad_command(&s,1800)); assert(captures==1);
    /* Same/older sequences cannot extend a lease or re-press a button. */
    command("1 1 2000 FF FF 32767 32767 0 0 0\n");
    assert(recomp_apply_test_gamepad_command(&s,1900)); neutral(&s);
    command("-1 1 100 00 FF 0 0 0 0 0\n2 1 2001 00 00 0 0 0 0 0\n"
            "2 1 100 100 00 0 0 0 0 0\n2 1 100 00 00 32768 0 0 0 0\n"
            "2 1 100 00 00 0 0 0 0 0 trailing\n");
    assert(recomp_apply_test_gamepad_command(&s,2000)); neutral(&s);
    assert(remove(path)==0);
    assert(recomp_apply_test_gamepad_command(&s,2100)); neutral(&s);
    /* Explicit hand-back leaves the real input untouched. */
    command("2 0 0 00 00 0 0 0 0 0\n"); s=before;
    assert(!recomp_apply_test_gamepad_command(&s,2200));
    assert(!memcmp(&s,&before,sizeof(s)));
    command("3 1 2000 00 01 0 0 0 0 0\n");
    assert(recomp_apply_test_gamepad_command(&s,2300));
    assert(s.Gamepad.bAnalogButtons[0]==255);
    assert(recomp_apply_test_gamepad_command(&s,4300)); neutral(&s);
    /* A zero-duration command is exactly one hardware sample. */
    command("4 1 0 00 01 0 0 0 0 0\n");
    assert(recomp_apply_test_gamepad_command(&s,4400));
    assert(s.Gamepad.bAnalogButtons[0]==255);
    assert(recomp_apply_test_gamepad_command(&s,4401)); neutral(&s);
    return 0;
}
'''


class GamepadCommandTests(unittest.TestCase):
    def test_production_parser_and_expiry(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        start = source.index('static int recomp_apply_test_gamepad_command(')
        helper = source[start:source.index('\n}\n', start)+3]
        bridge = source[source.index('result = xbox_InputGetState(port, &state);'):]
        self.assertLess(bridge.index('recomp_apply_test_gamepad_command'),
                        bridge.index('recomp_apply_test_aircraft_pickup_route'))
        self.assertIn('goto test_gamepad_ready;', bridge)
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-gamepad-') as temp:
            directory = Path(temp)
            for mutant in (False, True):
                actual = helper.replace(
                    'if (single_poll_pending || now < expires)', 'if (1)'
                ) if mutant else helper
                c = directory/'fixture.c'; exe = directory/'fixture.exe'
                c.write_text(PRELUDE+actual+HARNESS,encoding='utf-8')
                built = subprocess.run([compiler,'-std=c11',str(c),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                for enabled in (0,1):
                    data=directory/f'command-{mutant}-{enabled}.txt'
                    result=subprocess.run([str(exe),str(data),str(enabled)],capture_output=True,text=True)
                    if mutant and enabled: self.assertNotEqual(result.returncode,0)
                    else: self.assertEqual(result.returncode,0,result.stderr)


if __name__ == '__main__':
    unittest.main()
