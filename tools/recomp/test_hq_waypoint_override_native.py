"""Native safety checks for the optional, game-local diagnostic waypoint file."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long long ULONGLONG;
static const char *test_path;
static const char *test_getenv(const char *name) {
    assert(strcmp(name, "MERCENARIES_TEST_HQ_WAYPOINT_FILE") == 0);
    return test_path;
}
#define getenv test_getenv
"""

HARNESS = r"""
static void write_command(const char *text) {
    FILE *file = fopen(test_path, "w");
    assert(file != NULL);
    fputs(text, file);
    fclose(file);
}
int main(int argc, char **argv) {
    float x = 10.0f, z = 20.0f;
    assert(argc == 2);
    /* Absent opt-in never touches outputs or opens a file. */
    assert(!recomp_test_hq_waypoint_override(0u, 0u, &x, &z));
    assert(x == 10.0f && z == 20.0f);
    test_path = argv[1];
    assert(!recomp_test_hq_waypoint_override(0u, 100u, &x, &z));
    assert(x == 10.0f && z == 20.0f);
    write_command("# numeric test data\n1 0 1442 1152\n");
    assert(recomp_test_hq_waypoint_override(0u, 1000u, &x, &z));
    assert(x == 1442.0f && z == 1152.0f);
    /* No repeated restart for a previously consumed sequence. */
    assert(!recomp_test_hq_waypoint_override(0u, 1500u, &x, &z));
    write_command("2 0 1425 1150\n");
    assert(!recomp_test_hq_waypoint_override(0u, 1501u, &x, &z));
    assert(x == 1442.0f && z == 1152.0f); /* polling is rate limited */
    assert(recomp_test_hq_waypoint_override(0u, 2000u, &x, &z));
    assert(x == 1425.0f && z == 1150.0f);
    write_command("3 0 nan 1150\n4 0 0 inf\n5 0 10001 1150\n0 0 0 0\n");
    assert(!recomp_test_hq_waypoint_override(0u, 2500u, &x, &z));
    assert(x == 1425.0f && z == 1150.0f); /* reject malformed replacements */
    /* Overrides are specific to the current test stage; they cannot jump it. */
    x = 30.0f; z = 40.0f;
    assert(!recomp_test_hq_waypoint_override(1u, 2501u, &x, &z));
    assert(x == 30.0f && z == 40.0f);
    write_command("7 2 100 200\n8 1 1421 1131\n");
    assert(recomp_test_hq_waypoint_override(1u, 3001u, &x, &z));
    assert(x == 1421.0f && z == 1131.0f);
    /* Editing coordinates without a new sequence cannot rearm an abort. */
    write_command("8 1 1 2\n");
    assert(!recomp_test_hq_waypoint_override(1u, 3501u, &x, &z));
    assert(x == 1421.0f && z == 1131.0f);
    /* Card-route stages use a separate namespace, still current-stage only. */
    write_command("9 101 1458 827\n");
    assert(!recomp_test_hq_waypoint_override(100u, 4001u, &x, &z));
    assert(x == 1421.0f && z == 1131.0f);
    assert(recomp_test_hq_waypoint_override(101u, 4501u, &x, &z));
    assert(x == 1458.0f && z == 827.0f);
    write_command("10 101 1452 830\n");
    assert(recomp_test_hq_waypoint_override(101u, 5001u, &x, &z));
    assert(x == 1452.0f && z == 830.0f);
    puts("waypoint opt-in, numeric validation, stage binding, polling and rearm passed");
    return 0;
}
"""


class WaypointOverrideNativeTests(unittest.TestCase):
    def test_production_parser(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None and Path("C:/MinGW/bin/gcc.exe").exists():
            compiler = "C:/MinGW/bin/gcc.exe"
        if compiler is None:
            self.skipTest("native waypoint regression requires GCC or Clang")
        source = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
        start = source.index("static int recomp_test_hq_waypoint_override(")
        end = source.index("\n}\n", start) + 3
        with tempfile.TemporaryDirectory(prefix="waypoint-native-") as temp:
            directory = Path(temp)
            test_source = directory / "waypoint-test.c"
            executable = directory / "waypoint-test.exe"
            test_source.write_text(PRELUDE + source[start:end] + HARNESS, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(test_source), "-o", str(executable)],
                           check=True, capture_output=True, text=True, timeout=60)
            result = subprocess.run([str(executable), str(directory / "waypoints.txt")],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
