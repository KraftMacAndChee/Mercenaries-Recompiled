"""Execute the real generated PblThread update with mutating guest callbacks.

The implementation is extracted, not copied: both cursor helpers and the retail
update body under test come from the production files. No game assets are needed.
The current behavioral basis is retail execution at VA 0x1FA0F0; the separate
test_notification_retail_oracle.py compares all nine callback-mutation cases.
Bounds guards and global-register preservation are project compatibility policy.
See docs/runtime/retail-notification-evidence.md for the replacement history.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0010.c"

PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t memory[0x04000000];
static uint32_t g_eax, g_ecx, g_edx, g_esi, g_edi, g_esp;
static uint32_t g_seh_ebp, g_recomp_current_func;
static uint32_t *word(uint32_t address) {
    assert(address >= 0x10000u && address <= sizeof(memory) - 4u);
    assert((address & 3u) == 0u);
    return (uint32_t *)(void *)(memory + address);
}
#define MEM32(a) (*word((uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esi g_esi
#define edi g_edi
#define esp g_esp
#define PUSH32(s, v) do { (s) -= 4u; MEM32(s) = (v); } while (0)
#define POP32(s, v) do { (v) = MEM32(s); (s) += 4u; } while (0)
#define LO8(v) ((v) & 255u)
#define SET_LO8(v, b) ((v) = ((v) & ~255u) | ((b) & 255u))
#define CMP_EQ(a, b) ((a) == (b))
#define CMP_NE(a, b) ((a) != (b))
#define TEST_Z(a, b) (((a) & (b)) == 0u)
#define RECOMP_TRACE_FUNC(a) (g_recomp_current_func = (a))
#define recomp_notification_list_checkpoint(...) ((void)0)
#define recomp_update_esi_checkpoint(...) ((void)0)
#define recomp_icall_stack_mismatch_trace(...) assert(!"callback stack mismatch")
static uint32_t guest_u32(uint32_t a) { return MEM32(a); }
static void callback(uint32_t target);
#define RECOMP_ICALL_SAFE(target, saved) do { \
    callback(target); g_esp += 8u; assert(g_esp == (saved)); \
} while (0)
"""

HARNESS = r"""
enum { HEAD = 0x30F074, COUNT = 0x30F084, CURRENT = 0x64DAC8,
       A = 0x11000, B = 0x11100, C = 0x11200, D = 0x11300,
       VTABLE = 0x20000, UPDATE = 0x20100, STACK = 0x100000 };
static unsigned scenario, visits;
static uint32_t visited[8];

static void insert_after(uint32_t previous, uint32_t object) {
    uint32_t node = object + 4u, next = MEM32(previous);
    MEM32(object) = VTABLE;
    MEM32(node) = next;
    MEM32(node + 4u) = previous;
    MEM32(node + 8u) = object;
    MEM32(previous) = node;
    MEM32(next + 4u) = node;
    ++MEM32(COUNT);
}
static void remove_object(uint32_t object) {
    uint32_t node = object + 4u;
    assert(MEM32(node + 8u) == object);
    MEM32(MEM32(node) + 4u) = MEM32(node + 4u);
    MEM32(MEM32(node + 4u)) = MEM32(node);
    MEM32(node + 8u) = 0u;
    --MEM32(COUNT);
}
static void callback(uint32_t target) {
    uint32_t current = ecx;
    assert(target == UPDATE);
    assert(MEM32(CURRENT) == current);
    assert(MEM32(esp + 4u) == 0x3C888889u); /* delta-time argument */
    assert(visits < sizeof(visited) / sizeof(visited[0]));
    visited[visits++] = current;
    if (current == A) {
        switch (scenario) {
        case 1: remove_object(B); break;
        case 2: remove_object(B); remove_object(C); break;
        case 3: insert_after(A + 4u, D); break;
        case 4: MEM8(A + 0x18u) |= 1u; break;
        case 5: MEM8(A + 0x18u) |= 1u; remove_object(B); break;
        case 6: MEM8(A + 0x18u) |= 1u; MEM8(A + 0x18u) &= ~1u; break;
        case 7: remove_object(A); break;
        case 8: remove_object(A); remove_object(B); remove_object(C); break;
        default: break;
        }
    }
    /* Guest dispatch may clobber global register emulation. The generated
     * caller must retain its iterator, time step, and saved stack registers. */
    eax = 0xDEAD0000u; ecx = 0xDEAD0004u; edx = 0xDEAD0008u;
    esi = 0xDEAD0010u; edi = 0xDEAD0014u;
}
static void reset_list(void) {
    memset(memory, 0, sizeof(memory));
    visits = 0;
    MEM32(HEAD) = HEAD; MEM32(HEAD + 4u) = HEAD;
    MEM32(VTABLE + 4u) = UPDATE;
    insert_after(HEAD, A); insert_after(A + 4u, B); insert_after(B + 4u, C);
}
static void check_guards(void) {
    reset_list();
    assert(recomp_notification_cursor_sanitize(HEAD) == HEAD);
    assert(recomp_notification_cursor_sanitize(A + 4u) == A + 4u);
    assert(recomp_notification_cursor_sanitize(0u) == 0u);
    assert(recomp_notification_cursor_sanitize(0xE8F28B5Eu) == 0u);
    assert(recomp_notification_cursor_sanitize(A + 5u) == 0u);
    assert(recomp_notification_cursor_sanitize(0x03FFFFFFu) == 0u);
    MEM32(A + 4u) = 0xE8F28B5Eu;
    assert(recomp_notification_cursor_sanitize(A + 4u) == 0u);
    reset_list();
    MEM32(A + 12u) = 0xE8F28B5Eu;
    assert(recomp_notification_cursor_sanitize(A + 4u) == 0u);
    reset_list();
    remove_object(B);
    assert(recomp_notification_cursor_sanitize(B + 4u) == 0u);
    assert(recomp_notification_cursor_sanitize(A + 4u) == A + 4u);
    remove_object(A); remove_object(C);
    assert(recomp_notification_cursor_sanitize(HEAD) == HEAD);
}
int main(int argc, char **argv) {
    static const uint32_t expected[][4] = {
        {A, B, C}, {A, C}, {A}, {A, D, B, C}, {A, B, C},
        {A, C}, {A, B, C}, {A, B, C}, {A}
    };
    static const unsigned lengths[] = {3, 2, 1, 4, 3, 2, 3, 3, 1};
    static const unsigned counts[] = {3, 2, 1, 4, 2, 1, 3, 2, 0};
    unsigned i;
    assert(argc == 2);
    scenario = (unsigned)strtoul(argv[1], NULL, 10);
    assert(scenario < sizeof(lengths) / sizeof(lengths[0]));
    check_guards();
    reset_list();
    ecx = HEAD; esp = STACK; esi = 0xABCDEF00u; edi = 0xABCDEF04u;
    g_seh_ebp = 0xABCDEF08u;
    MEM32(esp) = 0x12345678u; MEM32(esp + 4u) = 0x3C888889u;
    sub_001FA0F0();
    assert(visits == lengths[scenario]);
    for (i = 0; i < visits; ++i) assert(visited[i] == expected[scenario][i]);
    assert(MEM32(COUNT) == counts[scenario]);
    assert(MEM32(CURRENT) == 0u);
    assert((MEM8(A + 0x18u) & 1u) == 0u);
    assert(esp == STACK + 8u);
    assert(esi == 0xABCDEF00u && edi == 0xABCDEF04u);
    assert(recomp_notification_cursor_sanitize(HEAD) == HEAD);
    printf("scenario %u: visits=%u remaining=%u passed\n", scenario, visits, MEM32(COUNT));
    printf("trace: ");
    for (i = 0; i < visits; ++i) printf("%08X ", visited[i]);
    printf("remaining=%u\n", MEM32(COUNT));
    return 0;
}
"""


def production_source(*, stale_successor: bool = False) -> str:
    manual = MANUAL.read_text(encoding="utf-8")
    start = manual.index("uint32_t recomp_notification_cursor_sanitize(")
    helpers = manual[start:manual.index("void recomp_vehicle_reward_checkpoint(", start)]
    generated = GENERATED.read_text(encoding="utf-8")
    start = generated.index("void sub_001FA0F0(void)\n{")
    routine = generated[start:generated.index("\n/**", start)]
    if stale_successor:
        old = "recomp_pbl_thread_next_after_update(esi, notification_next)"
        assert routine.count(old) == 1
        routine = routine.replace(old, "recomp_notification_cursor_sanitize(notification_next)")
    return PRELUDE + helpers + routine + HARNESS


def build_executables(directory):
    compiler = shutil.which("gcc") or shutil.which("clang")
    if compiler is None and Path("C:/MinGW/bin/gcc.exe").exists():
        compiler = "C:/MinGW/bin/gcc.exe"
    if compiler is None:
        raise unittest.SkipTest("native cursor regression requires GCC or Clang")
    results = []
    for mutant in (False, True):
        # Avoid Windows installer-detection names such as "update": this is
        # an ordinary unprivileged console test, never an elevated installer.
        stem = "stale-successor-mutant" if mutant else "retail-cursor-test"
        source = directory / f"{stem}.c"
        executable = directory / f"{stem}.exe"
        source.write_text(production_source(stale_successor=mutant), encoding="utf-8")
        subprocess.run(
            [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-label", "-Wno-unused-variable", str(source), "-o", str(executable)],
            check=True, capture_output=True, text=True, timeout=60,
        )
        results.append(executable)
    return results


class NotificationCursorNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="notification-native-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executables = build_executables(Path(cls.directory.name))

    def test_retail_update_callback_mutation(self):
        names = [
            "unchanged", "remove-successor", "remove-two-successors", "insert-successor",
            "deferred-self-removal", "self-and-successor-removal", "self-reactivation",
            "unexpected-current-unlink", "unexpected-clear-all",
        ]
        for scenario, name in enumerate(names):
            with self.subTest(scenario=name):
                result = subprocess.run([str(self.executables[0]), str(scenario)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_pre_callback_successor_regression(self):
        """Prove the checks detect the previous iteration-order bug."""
        for scenario in [1, 3, 5]:
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.executables[1]), str(scenario)],
                                        capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0,
                                    "stale-successor mutant unexpectedly passed")
                self.assertIn("Assertion", result.stderr)

    def test_notification_entry_trace_is_opt_in_and_bounded(self):
        source = MANUAL.read_text(encoding="utf-8")
        self.assertIn('getenv("MERCENARIES_TRACE_NOTIFICATION_ENTRIES")', source)
        self.assertIn("observed_count < 256u", source)
        self.assertIn("[NOTIFICATION-ENTRY]", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
