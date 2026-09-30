"""Native tests of optional graphics RAM mappings and the lifted pool ABI."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
CC = Path('C:/msys64/mingw64/bin/gcc.exe')

class ExtendedGraphicsMemoryTests(unittest.TestCase):
    def run_native(self, source):
        env=os.environ.copy();env['PATH']=str(CC.parent)+os.pathsep+env['PATH']
        with tempfile.TemporaryDirectory(prefix='mercs-graphics-memory-') as tmp:
            src,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe'
            src.write_text(source,encoding='utf-8')
            r=subprocess.run([str(CC),'-std=c11','-O2','-fno-strict-aliasing','-I',str(ROOT/'src'),'-I',str(ROOT/'ports/mercenaries/src'),str(src),'-o',str(exe)],env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(r.returncode,0,r.stderr)
            r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_mappings_aliases_heap_bounds_and_reinitialization(self):
        source='#include <windows.h>\n#include "'+(ROOT/'src/kernel/xbox_memory_layout.c').as_posix()+'"\n'
        self.run_native(source+r'''
#include <assert.h>
void xbox_kernel_set_thunk_address(uint32_t a,uint32_t n) {(void)a;(void)n;}
#define WORD(a) (*(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset()+(a)))
int main(void) {
 unsigned char xbe[4096]={0};
 for(unsigned pass=0;pass<4;pass++) {
  unsigned extended=pass&1u,size=extended?0x08000000u:0x04000000u;
  xbox_SetExtendedGraphicsMemory(extended);
  assert(xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
  assert(xbox_GetGraphicsMemorySize()==size);
  assert(xbox_GetExtendedGraphicsPool()==(extended?0x04000000u:0));
  xbox_SetExtendedGraphicsMemory(!extended);
  assert(xbox_GetGraphicsMemorySize()==size); /* immutable while mapped */
  WORD(0x01230000u)=0x11223344u;WORD(0x05230000u)=0x55667788u;
  assert(WORD(0x01230000u)==(extended?0x11223344u:0x55667788u));
  assert(WORD(size+0x01230000u)==WORD(0x01230000u));
  assert(xbox_MapPhysicalAlias(0x81230000u));
  assert(xbox_MapPhysicalAlias(0x85230000u));
  assert(WORD(0x81230000u)==WORD(0x01230000u));
  assert(WORD(0x85230000u)==WORD(0x05230000u));
  void *gpu=MapViewOfFileEx(xbox_GetMappingHandle(),FILE_MAP_ALL_ACCESS,0,0,size,
   (void *)((uintptr_t)xbox_GetMemoryOffset()+0xF0000000u));
  assert(gpu==(void *)((uintptr_t)xbox_GetMemoryOffset()+0xF0000000u));
  assert(WORD(0xF1230000u)==WORD(0x01230000u));
  if(extended)assert(WORD(0xF5230000u)==WORD(0x05230000u));
  assert(UnmapViewOfFile(gpu));
  uint32_t block=xbox_HeapAlloc(XBOX_HEAP_SIZE,4096);assert(block==XBOX_HEAP_BASE);
  assert(!xbox_HeapAlloc(4096,4096)); /* graphics bank is not general heap */
  if(extended){xbox_HeapFree(block|0x84000000u);assert(xbox_HeapGetAllocationSize(block)==XBOX_HEAP_SIZE);}
  xbox_HeapFree(block|0x80000000u);assert(!xbox_HeapGetAllocationSize(block));
  xbox_MemoryLayoutShutdown();assert(!xbox_GetExtendedGraphicsPool());
 }
 return 0;
}
''')

    def test_occupied_addresses_fallback_and_failure_diagnostics(self):
        source = r'''#include <windows.h>
#include <assert.h>
static int fail_section,fail_views,map_calls;
static HANDLE test_section(HANDLE file,LPSECURITY_ATTRIBUTES security,DWORD protect,DWORD high,DWORD low,LPCSTR name){
 if(fail_section){SetLastError(ERROR_COMMITMENT_LIMIT);return NULL;}
 return CreateFileMappingA(file,security,protect,high,low,name);
}
static LPVOID test_view(HANDLE section,DWORD access,DWORD high,DWORD low,SIZE_T size,LPVOID base){
 ++map_calls;
 if(fail_views){SetLastError(ERROR_ACCESS_DENIED);return NULL;}
 return MapViewOfFileEx(section,access,high,low,size,base);
}
#define CreateFileMappingA test_section
#define MapViewOfFileEx test_view
''' + '#include "' + (ROOT/'src/kernel/xbox_memory_layout.c').as_posix() + '"\n'
        self.run_native(source+r'''
#undef CreateFileMappingA
#undef MapViewOfFileEx
void xbox_kernel_set_thunk_address(uint32_t a,uint32_t n){(void)a;(void)n;}
int main(void){
 unsigned char xbe[4096]={0};
 const uintptr_t low[]={0x10000,0x800000,0x1000000,0x2000000,0x10000000};
 const uintptr_t high[]={0x1000000000ull,0x2000000000ull,0x3000000000ull};
 void *occupied[8]={0};
 for(unsigned i=0;i<8;i++){
  /* High hints have available RAM but a blocked future VRAM alias. */
  uintptr_t address=i<5?low[i]:high[i-5]+0xf0000000ull;
  occupied[i]=VirtualAlloc((void*)address,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  if(occupied[i])*(unsigned*)occupied[i]=0x1234abcd;
  else {MEMORY_BASIC_INFORMATION info;assert(VirtualQuery((void*)address,&info,sizeof(info)));assert(info.State!=MEM_FREE);}
 }
 for(unsigned extended=0;extended<2;extended++){
  xbox_SetExtendedGraphicsMemory(extended);
  assert(xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
  uintptr_t base=(uintptr_t)xbox_GetMemoryOffset();
  assert(base>=0x1000000000ull);
  for(unsigned i=0;i<3;i++)assert(base!=high[i]);
  unsigned size=xbox_GetGraphicsMemorySize();
  *(unsigned*)(base+0x1230000)=0x11223344;
  assert(*(unsigned*)(base+size+0x1230000)==0x11223344);
  assert(xbox_MapPhysicalAlias(0x81230000));
  assert(*(unsigned*)(base+0x81230000)==0x11223344);
  void *gpu=MapViewOfFileEx(xbox_GetMappingHandle(),FILE_MAP_ALL_ACCESS,0,0,size,(void*)(base+0xf0000000ull));
  assert(gpu==(void*)(base+0xf0000000ull));
  assert(*(unsigned*)(base+0xf1230000)==0x11223344);
  assert(UnmapViewOfFile(gpu));xbox_MemoryLayoutShutdown();
  for(unsigned i=0;i<8;i++)if(occupied[i])assert(*(unsigned*)occupied[i]==0x1234abcd);
 }
 fail_section=1;
 assert(!xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
 assert(strstr(xbox_GetMemoryLayoutError(),"backing memory") && strstr(xbox_GetMemoryLayoutError(),"1455"));
 assert(!xbox_GetMappingHandle() && !xbox_GetMemoryBase());fail_section=0;
 fail_views=1;map_calls=0;
 assert(!xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
 assert(map_calls>=5 && map_calls<100);
 assert(strstr(xbox_GetMemoryLayoutError(),"virtual address") && strstr(xbox_GetMemoryLayoutError(),"error 5"));
 assert(!xbox_GetMappingHandle() && !xbox_GetMemoryBase());fail_views=0;
 assert(xbox_MemoryLayoutInit(xbe,sizeof(xbe)));assert(!*xbox_GetMemoryLayoutError());xbox_MemoryLayoutShutdown();
 for(unsigned i=0;i<8;i++)if(occupied[i])assert(VirtualFree(occupied[i],0,MEM_RELEASE));
 puts("PASS: occupied native/high addresses, dynamic arena, CPU/GPU mirrors, host preservation, failure diagnostics and retry");
 return 0;
}
''')

    def test_lifted_pool_capacity_contents_and_reclamation(self):
        text='\n'.join(p.read_text(encoding='utf-8') for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'))
        functions={n:b for b,n in re.findall(r'(void (sub_[0-9A-F]{8})\(void\)\n\{.*?\n\})',text,re.S)}
        selected=set()
        def add(n):
            if n in selected or n in ('sub_0022AEE9','sub_001F6C20'):return
            selected.add(n)
            for child in re.findall(r'(sub_[0-9A-F]{8})\(\);',functions[n]):add(child)
        for n in ('sub_0020F9C0','sub_001F7140','sub_001F6970'):add(n)
        source='#include <windows.h>\n#undef __forceinline\n#define __forceinline inline\n#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'
        source+=r'''
#include <assert.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned extended,physical_calls;
uint32_t xbox_GetExtendedGraphicsPool(void){return extended?0x04000000u:0;}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
void sub_0022AEE9(void){assert(MEM32(esp+4)==0x1680000u);physical_calls++;eax=0x02980000u;esp+=20;}
void sub_001F6C20(void){abort();}
'''
        source+='\n'.join('void '+n+'(void);' for n in sorted(selected))
        source+='\n'+'\n'.join(functions[n] for n in sorted(selected))
        self.run_native(source+r'''
enum{STACK=0x008BFFF0u,POOL=0x645934u};
static uint32_t slots[4500];
static void begin(void){esp=STACK;ebx=0x12345678;esi=0x23456789;edi=0x3456789A;}
static uint32_t allocate(uint32_t size){
 begin();MEM32(esp+4)=size;MEM32(esp+8)=0;MEM32(esp+12)=0;sub_001F7140();
 assert(esp==STACK+4);assert(ebx==0x12345678&&esi==0x23456789&&edi==0x3456789A);return eax;
}
static void release(uint32_t p){begin();ecx=POOL;MEM32(esp+4)=p;MEM32(esp+8)=0;sub_001F6970();assert(esp==STACK+12);}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)calloc(1,0x08000000u);assert(g_xbox_mem_offset);
 for(extended=0;extended<2;extended++){
  begin();sub_0020F9C0();assert(esp==STACK+4);
  uint32_t start=extended?0x04000000u:0x02980000u,size=extended?0x03FF0000u:0x01680000u;
  assert(MEM32(POOL)==start&&MEM32(POOL+4)==size);
  assert(MEM32(POOL+0x14)==(extended?8192u:4000u));
  assert(MEM32(POOL+0x10)==(extended?0x07FF0000u:0x7AD608u));
  uint32_t p=allocate(0x02000000u);assert(extended?p==start:p==0);
  if(p){MEM32(p)=0xABCDEF12u;MEM32(p+0x01FFFFFCu)=0xFEDCBA21u;release(p);}
  unsigned count=extended?4500u:3000u;
  for(unsigned i=0;i<count;i++){
   slots[i]=allocate(4096);assert(slots[i]>=start&&slots[i]+4096<=start+size);
   assert((slots[i]&127u)==0);MEM32(slots[i])=i;
  }
  for(unsigned parity=0;parity<2;parity++)for(unsigned i=parity;i<count;i+=2){assert(MEM32(slots[i])==i);release(slots[i]);}
  assert(MEM32(POOL+8)==size);
  p=allocate(size);assert(p==start);assert(!allocate(128));release(p);assert(MEM32(POOL+8)==size);
 }
 assert(physical_calls==1);free((void *)g_xbox_mem_offset);return 0;
}
''')

if __name__=='__main__':unittest.main()
