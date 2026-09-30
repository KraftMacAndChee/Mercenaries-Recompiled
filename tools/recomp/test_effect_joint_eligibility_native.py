"""Execute effect-joint eligibility against retail CMP branches and HQ ordering."""
from pathlib import Path
import re,runpy,subprocess,shutil,tempfile,sys,struct
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT))
from tools.recomp import config
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0003.c').read_text()
code=re.search(r'void sub_000B2840\(void\)\n\{.*?\n\}',s,re.S)[0]
patches=[p for p in runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES'] if p.name.startswith('Effect-joint eligibility preserves incoming CMP ')]
assert len(patches)==7
old=code
config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'));xbe=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
for p in patches:
 assert p.after in old;old=old.replace(p.after,p.before)
 addr=int(p.name.rsplit(' ',1)[1],16);imm=int(re.search(r'cmp eax, 0x([0-9A-F]+)',p.before)[1],16);off=config.va_to_file_offset(addr)
 assert xbe[off:off+5]==b'='+struct.pack('<I',imm),(hex(addr),xbe[off:off+5].hex())
 # All seven edges jump to the shared JE, which must consume their own CMP.
 branch=xbe[off+5:off+10]
 if branch[0]==0xeb:dest=addr+7+struct.unpack('<b',branch[1:2])[0]
 else:assert branch[0]==0xe9;dest=addr+10+struct.unpack('<i',branch[1:5])[0]
 assert dest==0xb2937,hex(dest)
known=[0xdf80a870,0x28d57b77,0x9c98f29f,0x565bdc6e,0xa4896014,0xdb286afa,0x125f6d96,0x1fffdbcd,0x8ab18034,0x7294e511,0x7c94f4cf,0xc01b8ae2,0x115cd802,0x105cd66f,0x0f5cd4dc,0x0e5cd349,0x0d5cd1b6,0x0c5cd023,0x052e12d9,0xc0812ed3,0xeeb1a84d,0x28cb2231,0x7fcf80a3,0x80cf8236,0x81cf83c9,0xa95cae9a,0xe82e7ca6]
pre=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static uint32_t mem[1024],eax,esp;
#define MEM32(a) mem[(a)/4]
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define CMP_A(a,b) ((uint32_t)(a)>(uint32_t)(b))
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
'''
main='static const uint32_t names[]={'+','.join(hex(x)+'u' for x in known)+r'''};
static int accepted(uint32_t name) {
 esp=256;MEM32(esp+4)=name;MEM32(esp+8)=99;sub_000B2840();
 assert(esp==268);assert((eax&0xffffff00u)==(name&0xffffff00u));return eax&255;
}
int main(int argc,char **argv) {
 int failures=0;
 for(unsigned i=0;i<sizeof(names)/4;i++) {
  failures+=!accepted(names[i]);
  assert(!accepted(names[i]-1));assert(!accepted(names[i]+1));
 }
 assert(!accepted(0));assert(!accepted(0xffffffffu));
 uint32_t jointOrder[]={0x105cd66f,0xf5cd4dc,0x115cd802,0xe5cd349,0xd5cd1b6,0xd5cd1b6,0xd5cd1b6,0xd5cd1b6,0xd5cd1b6,0xd5cd1b6};
 unsigned activeCount=0;for(unsigned i=0;i<10;i++)activeCount+=accepted(jointOrder[i]);
 if(argc>1){assert(failures==7);assert(activeCount==3);puts("PASS old translation reproduces seven rejected effects and 3/10 HQ light entries");}
 else {assert(!failures);assert(activeCount==10);puts("PASS 27 retail effect names, 56 exclusions, preserved return/stack, 10/10 Chinese HQ attachment order");}
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='effect-joints-') as d:
 p=Path(d);cc=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
 for tag,body in [('old',old),('fixed',code)]:
  src=p/(tag+'.c');exe=p/(tag+'.exe');src.write_text(pre+body+main)
  subprocess.run([cc,'-O2',str(src),'-o',str(exe)],check=True)
  subprocess.run([str(exe)]+(['old'] if tag=='old' else []),check=True)
