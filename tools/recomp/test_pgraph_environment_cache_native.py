"""Ensure disabled window-clip diagnostics never rescan CRT env per draw."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class PgraphEnvironmentCacheTests(unittest.TestCase):
    def test_cached_values_and_full_cache(self):
        source=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        body=re.search(r'static const char \*pgraph_cached_getenv\(const char \*name\)\n\{.*?\n\}',source,re.S)[0]
        rest=source.replace(body,'')
        self.assertNotRegex(rest,r'\bgetenv\(')
        self.assertIn('pgraph_cached_getenv("MERCENARIES_DEBUG_DISABLE_WINDOW_CLIP")',rest)
        capacity=int(re.search(r'cache\[(\d+)\]',body)[1])
        self.assertGreaterEqual(capacity,source.count('pgraph_cached_getenv('))
        prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned calls;
static const char *configured;
static const char *stub_getenv(const char *name){++calls;return configured;}
#define getenv stub_getenv
'''
        tail=r'''
int main(int argc,char **argv){
 const char *key="MERCENARIES_DEBUG_DISABLE_WINDOW_CLIP";
 int mode=atoi(argv[1]);configured=mode==0?NULL:mode==1?"":"1";
 const char *expected=configured;
 for(unsigned i=0;i<100000;i++)assert(pgraph_cached_getenv(key)==expected);
 assert(calls==1);
 /* The configuration is captured at launch; absent and empty are distinct. */
 configured="changed";assert(pgraph_cached_getenv(key)==expected);assert(calls==1);
 char names[512][32];
 for(unsigned i=0;i<512;i++){
  snprintf(names[i],sizeof(names[i]),"UNIQUE_DIAGNOSTIC_%u",i);
  assert(pgraph_cached_getenv(names[i])==configured);
 }
 assert(calls==513); /* one overflow lookup remains uncached by design */
 for(unsigned i=0;i<511;i++)assert(pgraph_cached_getenv(names[i])==configured);
 assert(calls==513);
 assert(pgraph_cached_getenv(key)==expected);assert(calls==513);
 assert(pgraph_cached_getenv(names[511])==configured);assert(calls==514);
 printf("mode=%d: 100000 hot lookups use one getenv; cached entries survive overflow\n",mode);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-pgraph-env-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+body+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            for mode in range(3):
                run=subprocess.run([str(exe),str(mode)],capture_output=True)
                self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
                print(run.stdout.decode().strip())


if __name__=='__main__':unittest.main()
