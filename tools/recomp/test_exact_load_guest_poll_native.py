"""Prove exact-load edge inputs are consumed only by the retail guest poll."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ExactLoadGuestPollTests(unittest.TestCase):
    def test_background_host_queries_cannot_consume_single_poll_edges(self):
        source = (ROOT / "src/input/xinput_device.c").read_text(encoding="utf-8")
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
            encoding="utf-8"
        )
        names = (
            "test_input_delay_ms",
            "apply_test_exact_load_game_sequence",
            "xbox_InputApplyTestExactLoadGame",
        )
        bodies = [
            re.search(
                rf"(?:static )?(?:BOOL|ULONGLONG) {name}\(.*?\n\}}", source, re.S
            )[0]
            for name in names
        ]
        bridge = manual[manual.index("result = xbox_InputGetState(port, &state);") :]
        self.assertLess(
            bridge.index("xbox_InputApplyTestExactLoadGame"),
            bridge.index("recomp_apply_test_gamepad_command"),
        )
        self.assertIn(
            "apply_test_exact_load_game_sequence(state, &buttons, elapsed, FALSE);",
            source,
        )
        program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "input/xinput_xbox.h"
static ULONGLONG tick;
static ULONGLONG g_input_start_ms;
static unsigned int g_test_exact_load_game_emitted;
static unsigned int g_test_exact_load_game_active_slot;
static ULONGLONG g_test_exact_load_game_pulse_start_ms;
static const char *fake_getenv(const char *name) {
    if (!strcmp(name, "MERCENARIES_TEST_EXACT_LOAD_GAME_SEQUENCE") ||
        !strcmp(name, "MERCENARIES_TEST_EXACT_LOAD_SINGLE_POLL")) return "1";
    return NULL;
}
#define input_cached_getenv fake_getenv
#define GetTickCount64() tick
''' + "\n".join(bodies) + r'''
int main(void) {
    XBOX_INPUT_STATE state;
    WORD buttons;
    g_test_exact_load_game_active_slot = 6u;
    tick = 90000u;
    memset(&state, 0, sizeof(state)); buttons = 0u;
    assert(!apply_test_exact_load_game_sequence(&state, &buttons, tick, FALSE));
    assert(buttons == 0u && g_test_exact_load_game_emitted == 0u);
    assert(xbox_InputApplyTestExactLoadGame(&state));
    assert(state.Gamepad.wButtons == XBOX_GAMEPAD_START);
    tick = 90301u; memset(&state, 0, sizeof(state));
    assert(xbox_InputApplyTestExactLoadGame(&state));
    assert(state.Gamepad.wButtons == 0u);
    assert((g_test_exact_load_game_emitted & 1u) != 0u);
    tick = 105000u; memset(&state, 0, sizeof(state));
    assert(xbox_InputApplyTestExactLoadGame(&state));
    assert(state.Gamepad.wButtons == XBOX_GAMEPAD_DPAD_DOWN);
    assert((g_test_exact_load_game_emitted & 2u) != 0u);
    memset(&state, 0, sizeof(state)); buttons = 0u;
    assert(!apply_test_exact_load_game_sequence(&state, &buttons, tick, FALSE));
    assert(buttons == 0u);
    puts("exact-load single-poll edges remain guest-polled");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="merc-exact-load-poll-") as directory:
            directory = Path(directory)
            source_path = directory / "test.c"
            executable = directory / "test.exe"
            source_path.write_text(program, encoding="utf-8")
            build = subprocess.run(
                [
                    shutil.which("gcc") or "C:/MinGW/bin/gcc.exe",
                    "-std=c11", "-O2", "-I", str(ROOT / "src"),
                    str(source_path), "-o", str(executable),
                ],
                capture_output=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr.decode(errors="replace"))
            run = subprocess.run([str(executable)], capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr.decode(errors="replace"))
            print(run.stdout.decode().strip())


if __name__ == "__main__":
    unittest.main()