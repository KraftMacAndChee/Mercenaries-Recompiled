"""Exercise launch-only environment caching, including collisions and saturation."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
SOURCE=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
helper=SOURCE[SOURCE.index('#define RECOMP_ENV_CACHE_CAPACITY'):SOURCE.index('static uint32_t guest_u32')]
program=r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static unsigned calls;
static char value[]="enabled";
static char *fake_getenv(const char *name){++calls;return name[0]=='P'?value:NULL;}
#define getenv fake_getenv
"""+helper+r"""
int main(void){
 static char names[1100][80];
 assert(!recomp_cached_getenv(NULL));assert(calls==0);
 for(unsigned i=0;i<1100;i++)sprintf(names[i],"%s_%u",i%2?"PRESENT":"ABSENT",i);
 for(unsigned i=0;i<1024;i++)assert(recomp_cached_getenv(names[i])==(i%2?value:NULL));
 assert(calls==1024);
 for(unsigned j=0;j<50;j++)for(unsigned i=0;i<1024;i++)assert(recomp_cached_getenv(names[i])==(i%2?value:NULL));
 assert(calls==1024);
 for(unsigned i=1024;i<1100;i++)assert(recomp_cached_getenv(names[i])==(i%2?value:NULL));
 assert(calls==1100);
 puts("launch environment cache: hits, misses, collisions and full table passed");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-env-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(program)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
