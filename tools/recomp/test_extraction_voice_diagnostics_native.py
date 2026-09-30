"""Audio failure evidence must survive long sessions without unbounded bursts."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text()
function = re.search(r"void recomp_xact_prepare_failure\(.*?\n}\n", source, re.S)[0]
prelude = r'''
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static uint32_t memory[1024], logs, last_suppressed, enabled=1;
static uint64_t now=1000;
static uint32_t guest_u32(uint32_t a){return memory[a/4];}
static int xbox_preview_log_enabled(void){return enabled;}
static uint64_t GetTickCount64(void){return now;}
static void xbox_preview_log_event(const char *tag,const char *format,...){
    assert(!strcmp(tag,"audio-prepare-failure"));
    va_list args;va_start(args,format);
    for(int i=0;i<7;i++)(void)va_arg(args,unsigned);
    last_suppressed=va_arg(args,unsigned);va_end(args);++logs;
}
'''
harness = r'''
int main(void){
    const uint32_t wrapper=256;
    memory[(wrapper+0x1c)/4]=123;
    for(int i=0;i<1000;i++)recomp_xact_prepare_failure(0,wrapper,0x8007000e);
    assert(logs==16);
    now+=10000;recomp_xact_prepare_failure(0,wrapper,0x8007000e);
    assert(logs==17 && last_suppressed==984);
    for(int i=0;i<30;i++)recomp_xact_prepare_failure(0,wrapper,0x8007000e);
    assert(logs==32);
    memory[(wrapper+0x1c)/4]=0x23f08077;
    recomp_xact_prepare_failure(0,wrapper,0x8007000e);
    assert(logs==33 && last_suppressed==15);
    memory[(wrapper+0x1c)/4]=0xd24e4223;
    recomp_xact_prepare_failure(0,wrapper,0x8007000e);assert(logs==34);
    enabled=0;now+=100000;recomp_xact_prepare_failure(0,wrapper,0x8007000e);assert(logs==34);
    enabled=1;now+=36000000;memory[(wrapper+0x1c)/4]=123;
    recomp_xact_prepare_failure(0,wrapper,0x8007000e);assert(logs==35);
    puts("PASS: bounded bursts, suppression counts, late failures, extraction cues, logging disabled");
}
'''
# The manager's first_free field is outside the small fixture; redirect that
# read alone, preserving the production throttling and logged fields.
function = function.replace("guest_u32(manager + 0x7564u)", "0u")
with tempfile.TemporaryDirectory(prefix="merc-extraction-log-") as directory:
    p=Path(directory);(p/"test.c").write_text(prelude+function+harness)
    subprocess.run(["C:/MinGW/bin/gcc.exe","-std=c11","-O2",str(p/"test.c"),"-o",str(p/"test.exe")],check=True)
    subprocess.run([str(p/"test.exe")],check=True)
