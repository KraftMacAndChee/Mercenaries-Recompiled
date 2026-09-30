"""Replay observed guest allocations through the production allocator in isolation.

Compiles a temporary native harness; never attaches to or changes a game process.
Allocation classes can be inferred from known guest owners in an older trace.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def trace_commands(text, page_owners):
    commands = []
    for line in text.splitlines():
        if "xbox_HeapAlloc: out of memory" in line:
            break  # A failed allocation is not present in the success log.
        match = re.search(
            r"\[HEAP\] (?:#\d+:|reuse:) size=(\d+) align=(\d+).*?"
            r"0x([0-9A-Fa-f]+).*?guest=([0-9A-Fa-f]+)",
            line,
        )
        if match:
            size, alignment, address, owner = match.groups()
            logged_class = re.search(r"class=(\d+)", line)
            allocation_class = int(logged_class.group(1)) if logged_class else int(int(owner, 16) in page_owners)
            commands.append(
                f"a {size} {alignment} {address} {owner} {allocation_class}"
            )
        else:
            match = re.search(r"\[HEAP\] free: 0x([0-9A-Fa-f]+)", line)
            if match:
                commands.append(f"f 0 0 {match[1]} 0 0")
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--allocator-source", type=Path, default=ROOT / "src/kernel/xbox_memory_layout.c")
    parser.add_argument("--probe-size", type=int, default=0)
    parser.add_argument(
        "--page-owner", action="append", type=lambda value: int(value, 0),
        default=[], help="Treat allocations by this guest owner as page-backed",
    )
    parser.add_argument("--probe-alignment", type=int, default=4096)
    parser.add_argument("--probe-page", action="store_true")
    args = parser.parse_args()
    if args.probe_size < 0:
        parser.error("probe size must be nonnegative")
    source = args.allocator_source.read_text(encoding="utf-8")
    structures = source[source.index("#define XBOX_HEAP_MAX_ALLOCS"):source.index("static void xbox_HeapCaptureOwner(")]
    body = source[source.index("static xbox_heap_allocation *xbox_HeapAppendAllocation("):source.index("static const xbox_heap_allocation *xbox_HeapFindAllocation(")]
    free = source[source.index("void xbox_HeapFree("):source.index("HANDLE xbox_GetMappingHandle(")]
    prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XBOX_HEAP_BASE 0x008C0000u
#define XBOX_HEAP_SIZE (0x04000000u-XBOX_HEAP_BASE)
static uintptr_t g_memory_offset;
static uint32_t g_recomp_current_func;
static uint32_t g_recomp_recent_game_func_idx;
static uint32_t g_recomp_recent_game_funcs[256];
#define XBOX_CPU_ALIAS_BASE 0x80000000u
#define XBOX_CPU_ALIAS_END  0xC0000000u
'''
    stubs = r'''
static void xbox_HeapCaptureOwner(xbox_heap_allocation *p) {memset(p->owner_ring,0,sizeof(p->owner_ring));}
static int xbox_HeapTraceEnabled(void) {return 0;}
static void xbox_HeapDumpCensus(void) {}
'''
    harness = r'''
typedef struct {unsigned old,now,size;} mapping;
static mapping map[XBOX_HEAP_MAX_ALLOCS];
int main(int argc,char **argv) {
 unsigned count=0,events=0,mismatches=0,size,alignment,address,owner,allocation_class;char op;
 g_memory_offset=(uintptr_t)calloc(1,0x04000000u);if(!g_memory_offset)return 2;
 while(scanf(" %c %u %u %x %x %u",&op,&size,&alignment,&address,&owner,&allocation_class)==6) {
  ++events;
  if(op=='a') {
   g_recomp_current_func=owner;
   unsigned result=allocation_class ? xbox_HeapAllocPageRounded(size,alignment) : xbox_HeapAlloc(size,alignment);
   if(!result) {
    printf("EARLY_OOM event=%u size=%u alignment=%u class=%u\n",events,size,alignment,allocation_class);
    for(int j=0;j<g_heap_alloc_count;++j) {
     xbox_heap_allocation *p=&g_heap_allocations[j];
     if(!p->in_use && p->allocation_size>=524288u)
      printf("free va=%08X size=%u class=%u\n",p->xbox_va,p->allocation_size,p->allocation_class);
    }
    return 3;
   }
   if(result!=address)++mismatches;
   unsigned slot=0;while(slot<count&&map[slot].old)++slot;
   if(slot>=XBOX_HEAP_MAX_ALLOCS)return 4;if(slot==count)++count;
   map[slot]=(mapping){address,result,size};
  } else if(op=='f') {
   unsigned slot=0;while(slot<count&&map[slot].old!=address)++slot;
   if(slot==count){printf("UNKNOWN_FREE event=%u old=%08X\n",events,address);return 5;}
   xbox_HeapFree(map[slot].now);map[slot].old=0;
  } else return 6;
 }
 unsigned largest=0;uint64_t total=0,live=0;
 for(int i=0;i<g_heap_alloc_count;++i) {
  xbox_heap_allocation *p=&g_heap_allocations[i];
  if(p->in_use)live+=p->allocation_size;
  else {total+=p->allocation_size;if(p->allocation_size>largest)largest=p->allocation_size;}
 }
 printf("events=%u moved_addresses=%u live=%llu free=%llu largest=%u frontier_remaining=%u\n",
 events,mismatches,(unsigned long long)live,(unsigned long long)total,largest,g_heap_page_next-g_heap_next);
 if(argc==4) {
  unsigned request=(unsigned)strtoul(argv[1],NULL,10),align=(unsigned)strtoul(argv[2],NULL,10);
  if(request)printf("probe=%u alignment=%u result=%08X\n",request,align,atoi(argv[3]) ? xbox_HeapAllocPageRounded(request,align) : xbox_HeapAlloc(request,align));
 }
 free((void *)g_memory_offset);return 0;
}
'''
    commands = trace_commands(
        args.trace.read_text(encoding="utf-8", errors="replace"),
        set(args.page_owner),
    )
    if not commands:
        parser.error("no allocation trace events")
    compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
    with tempfile.TemporaryDirectory(prefix="mercs-heap-replay-") as directory:
        c = Path(directory) / "replay.c"
        exe = Path(directory) / "replay.exe"
        c.write_text(prelude + structures + stubs + body + free + harness, encoding="utf-8")
        subprocess.run([compiler, "-std=c11", "-O2", str(c), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe), str(args.probe_size), str(args.probe_alignment), str(int(args.probe_page))],
                                input="\n".join(commands) + "\n", text=True, capture_output=True)
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
