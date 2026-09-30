"""Ensure vertex-shader diagnostics never rescan the CRT environment per shader."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VshEnvironmentCacheTests(unittest.TestCase):
    def test_cached_values_and_shader_call_sites(self):
        source = (ROOT / "src/d3d/d3d8_vsh.c").read_text(encoding="utf-8")
        body = re.search(
            r"static const char \*vsh_cached_getenv\(const char \*name\)\n\{.*?\n\}",
            source,
            re.S,
        )[0]
        rest = source.replace(body, "")
        self.assertNotRegex(rest, r"\bgetenv\(")
        options = (
            "MERCENARIES_STRICT_SCREEN_ROUNDING",
            "MERCENARIES_DEBUG_VSH_COLOR",
            "MERCENARIES_TRACE_VSH_HLSL_HASH",
            "MERCENARIES_TRACE_VSH_HLSL_RAW_HASH",
            "MERCENARIES_TRACE_VSH_HLSL",
            "MERCENARIES_TRACE_VSH_HANDLES",
        )
        for option in options:
            self.assertIn(f'vsh_cached_getenv("{option}")', rest)

        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned calls;
static const char *configured;
static const char *stub_getenv(const char *name){(void)name;++calls;return configured;}
#define getenv stub_getenv
'''
        tail = r'''
int main(int argc,char **argv){
 int mode=atoi(argv[1]);configured=mode==0?NULL:mode==1?"":"1";
 const char *expected=configured;
 const char *keys[]={
  "MERCENARIES_DEBUG_VSH_COLOR",
  "MERCENARIES_TRACE_VSH_HLSL_HASH",
  "MERCENARIES_TRACE_VSH_HLSL_RAW_HASH",
  "MERCENARIES_TRACE_VSH_HLSL",
  "MERCENARIES_TRACE_VSH_HANDLES"
 };
 for(unsigned key=0;key<5;key++)
  for(unsigned i=0;i<100000;i++)assert(vsh_cached_getenv(keys[key])==expected);
 assert(calls==5);
 configured="changed";
 for(unsigned key=0;key<5;key++)assert(vsh_cached_getenv(keys[key])==expected);
 assert(calls==5);
 printf("mode=%d: 500000 shader diagnostic queries use five getenv calls\n",mode);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="merc-vsh-env-") as directory:
            path = Path(directory)
            c_file = path / "test.c"
            exe = path / "test.exe"
            c_file.write_text(prelude + body + tail, encoding="utf-8")
            build = subprocess.run(
                [shutil.which("gcc") or "C:/MinGW/bin/gcc.exe", "-O2", "-std=c11", str(c_file), "-o", str(exe)],
                capture_output=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr.decode(errors="replace"))
            for mode in range(3):
                run = subprocess.run([str(exe), str(mode)], capture_output=True)
                self.assertEqual(run.returncode, 0, run.stderr.decode(errors="replace"))
                print(run.stdout.decode().strip())


if __name__ == "__main__":
    unittest.main()