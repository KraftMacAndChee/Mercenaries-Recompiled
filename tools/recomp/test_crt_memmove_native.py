"""Run the actual CRT entry against memmove with overlapping ranges and ABI checks."""
import argparse,re,shutil,subprocess,tempfile,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 p=argparse.ArgumentParser();p.add_argument('--retail-oracle',type=Path);p.add_argument('--buffered-reader',action='store_true');p.add_argument('--source',type=Path,default=ROOT/'ports/mercenaries/src/recomp/gen/recomp_0011.c');a=p.parse_args()
 source=a.source.read_text(encoding='utf-8')
 body=re.search(r'void sub_00238C00\(void\)\n\{.*?\n\}',source,re.S)[0]
 if 'loc_00238C66:' in body:
  # Everything after this point is unreachable misdecoded jump-table data;
  # real destinations are dispatched to the maintained tail helper.
  body=body[:body.index('    /* nop */\n    if (_flags /* jo: overflow */)')]+ '\n}\n'
 manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
 helper=re.search(r'static void mercenaries_memmove_finish\(void\)\n\{.*?\n\}',manual,re.S)[0]
 fixed=re.search(r'void recomp_crt_memmove\(void\)\n\{.*?\n\}',manual,re.S)
 fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x400000],expected[4096];
static void *guest_ptr(uint32_t a){assert(a<sizeof(memory));return memory+a;}
static uint32_t guest_u32(uint32_t a){assert(a<=sizeof(memory)-4);uint32_t v;memcpy(&v,memory+a,4);return v;}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(a) mercenaries_memmove_finish()
"""+helper+'\n'+(fixed[0] if fixed else '')+'\n'+body+r"""
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;unsigned total=0,bad=0;
 for(unsigned i=0;i<8;i++)MEM32(0x238CE0+i*4)=0x238D00+i*8;
 for(unsigned i=0;i<4;i++){MEM32(0x238D4C+i*4)=0x238D5C+i*8;MEM32(0x238C60+i*4)=0x238C70;}
 for(unsigned n=0;n<=512;n++)for(unsigned align=0;align<4;align++)for(int delta=-65;delta<=65;delta++){
  unsigned src=1024+align,dst=src+delta;
  for(unsigned j=0;j<sizeof(expected);j++)memory[j]=(unsigned char)((j*37+(j>>4)*13)^(j>>8));
  memcpy(expected,memory,sizeof(expected));memmove(expected+dst,expected+src,n);
  esp=0x3f0000;MEM32(esp)=0x12345678;MEM32(esp+4)=dst;MEM32(esp+8)=src;MEM32(esp+12)=n;
  ebx=0x11223344;esi=0x55667788;edi=0x99aabbcc;g_seh_ebp=0x3ef000;
  sub_00238C00();
  assert(esp==0x3f0004&&eax==dst&&ebx==0x11223344&&esi==0x55667788&&edi==0x99aabbcc&&g_seh_ebp==0x3ef000);
  total++;if(memcmp(memory,expected,sizeof(expected))){if(bad<6)printf("mismatch length=%u src=%u dst=%u delta=%d\n",n,src,dst,delta);bad++;}
 }
 printf("CRT memmove oracle: %u cases, %u mismatches; ABI preserved\n",total,bad);return bad?1:0;
}
"""
 if a.buffered_reader:
  functions={}
  for file in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
   for name in ('0018F4D0','0018FAA0'):
    match=re.search(r'void sub_'+name+r'\(void\)\n\{.*?\n\}',file.read_text(encoding='utf-8'),re.S)
    if match:functions[name]=match[0]
  assert len(functions)==2
  fixture=fixture.replace('int main(void){', r'''
/* Execute the real buffered-reader refill, with only the child I/O stubbed. */
static unsigned stream_reads;
static void child_call(uint32_t target) {
 if(target==1) {
  uint32_t result=MEM32(esp+4);MEM8(result)=1;eax=result;esp+=8;
 } else {
  assert(target==2);uint32_t dst=MEM32(esp+4),n=MEM32(esp+8);
  assert(dst+n<=8192);for(uint32_t j=0;j<n;j++)MEM8(dst+j)=(unsigned char)(0x80u^(j*19u));
  stream_reads++;eax=n;esp+=12;
 }
}
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(target,saved) child_call(target)
''' + functions['0018F4D0']+'\n'+functions['0018FAA0'] + r'''
static unsigned test_buffered_reader(void) {
 unsigned cases=0,bad=0;unsigned char want[8192];
 const unsigned capacities[]={512,1024,4096};
 for(unsigned c=0;c<3;c++)for(unsigned mark=1;mark<capacities[c];mark++) {
  unsigned capacity=capacities[c],kept=capacity-mark,dest=(-kept)&511u,used=kept+dest,n=capacity-used;
  for(unsigned j=0;j<8192;j++)memory[j]=(unsigned char)((j*37+(j>>4)*13)^(j>>8));
  memcpy(want,memory,8192);memmove(want+1024+dest,want+1024+mark,kept);
  for(unsigned j=0;j<n;j++)want[1024+used+j]=(unsigned char)(0x80u^(j*19u));
  uint32_t obj=0x20000,stream=0x20100,vt=0x20200;
  MEM32(obj+8)=stream;MEM32(stream)=vt;MEM32(vt+4)=1;MEM32(vt+8)=2;
  MEM32(obj+12)=1024;MEM32(obj+16)=capacity;MEM32(obj+20)=capacity;MEM32(obj+24)=capacity;MEM32(obj+28)=mark;MEM32(obj+32)=capacity;
  esp=0x3f0000;MEM32(esp)=0x12345678;ecx=obj;stream_reads=0;
  ebx=0x11223344;esi=0x55667788;edi=0x99aabbcc;g_seh_ebp=0x3ef000;
  sub_0018FAA0();
  assert(esp==0x3f0004&&eax==0&&ebx==0x11223344&&esi==0x55667788&&edi==0x99aabbcc&&g_seh_ebp==0x3ef000);
  assert(MEM32(obj+16)==used&&MEM32(obj+20)==capacity&&MEM32(obj+28)==dest&&stream_reads==(n!=0));
  cases++;if(memcmp(memory,want,8192)){if(bad<6)printf("refill mismatch capacity=%u mark=%u kept=%u dest=%u\n",capacity,mark,kept,dest);bad++;}
 }
 printf("Havok buffered-reader refill: %u cases, %u mismatches; metadata and ABI preserved\n",cases,bad);return bad;
}
int main(void){
''')
  fixture=fixture.replace('return bad?1:0;', 'bad+=test_buffered_reader();return bad?1:0;')
 if a.retail_oracle:
  sys.path.insert(0,str(ROOT))
  from tools.xbe_parser.xbe_parser import XBEParser
  parser=XBEParser(str(a.retail_oracle));original=parser.parse()
  offset=parser._va_to_file(0x230000,original.header.base_address)
  code=original.raw_data[offset:offset+0x10000]
  assert len(code)==0x10000
  fixture=fixture.replace('#include <stdio.h>', '#include <windows.h>\n#include <stdio.h>')
  fixture=fixture.replace('int main(void){', 'static void *retail_base;\nstatic LONG WINAPI fault(EXCEPTION_POINTERS *e){__asm__ volatile("cld");fprintf(stderr,"oracle fault ip=%08lx original=%08lx address=%08lx base=%p\\n",e->ContextRecord->Eip,e->ContextRecord->Eip-(uint32_t)retail_base+0x230000,e->ExceptionRecord->ExceptionInformation[1],retail_base);ExitProcess(9);return EXCEPTION_CONTINUE_SEARCH;}\nint main(void){')
  fixture=fixture.replace('int main(void){', 'int main(int argc,char **argv){\n assert(sizeof(void*)==4 && argc==2);\n void *code=VirtualAlloc(NULL,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(code);retail_base=code;AddVectoredExceptionHandler(1,fault);\n FILE *f=fopen(argv[1],"rb");assert(f && fread(code,1,0x10000,f)==0x10000);fclose(f);\n uint32_t relocation=(uint32_t)code-0x230000u;\n unsigned tables[]={0x8c60,0x8ce0,0x8d4c,0x8dec,0x8e7c,0x8ee8}, counts[]={4,8,4,4,8,4};\n for(unsigned j=0;j<6;j++)for(unsigned k=0;k<counts[j];k++){uint32_t *v=(uint32_t*)((char*)code+tables[j]+k*4);if(*v>=0x238c00&&*v<0x239000)*v+=relocation;}\n for(unsigned j=0x8c00;j<0x8fe0;j++){unsigned char *b=(unsigned char*)code+j;if(b[0]==0xff&&b[1]==0x24){uint32_t v;memcpy(&v,b+3,4);if(v>=0x238c00&&v<0x239000){v+=relocation;memcpy(b+3,&v,4);}}}\n FlushInstructionCache(GetCurrentProcess(),code,0x10000);\n void *(__cdecl *retail_copy)(void*,const void*,size_t)=(void*)((char*)code+0x8c00);')
  fixture=fixture.replace('memmove(expected+dst,expected+src,n);','assert(retail_copy(expected+dst,expected+src,n)==expected+dst);')
  fixture=fixture.replace('CRT memmove oracle:', 'Retail x86 CRT oracle:')
 with tempfile.TemporaryDirectory(prefix='crt-copy-native-') as t:
  path=Path(t);(path/'test.c').write_text(fixture,encoding='utf-8')
  subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing',str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
  args=[str(path/'test.exe')]
  if a.retail_oracle:
   (path/'retail.bin').write_bytes(code);args.append(str(path/'retail.bin'))
  subprocess.run(args,check=True,timeout=60)
if __name__=='__main__':main()
