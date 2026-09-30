"""Native regression cases for additional retail DirectSound flag lifetimes."""
from pathlib import Path
import re
import unittest
import test_xact_condition_code_native as native

SOURCE = Path(__file__).resolve().parents[2] / "ports/mercenaries/src/recomp/gen/recomp_0013.c"


class DirectSoundConditionTests(unittest.TestCase):
    execute = native.XactConditionCodeTests.execute

    def function(self, address):
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index(f"void sub_{address}(void)")
        return text[start:text.index("\n}\n", start)+3]

    def original(self, fixed, condition):
        old, count = re.subn(r"    _flags = \(" + re.escape(condition) + r"\); /\* preserve .*?\*/\n", "", fixed)
        self.assertEqual(count, 1)
        self.assertEqual(old.count("if (_flags != 0)"), 1)
        return old.replace("if (_flags != 0)", f"if ({condition})")

    def test_scalar_destructor_delete_flag(self):
        code = self.function("0029DDDD")
        prefix = "static unsigned frees; static void sub_0029FFBC(void) {assert(MEM32(esp+4)==0x1000); ++frees; esp+=8;}\n"
        self.execute(prefix+code, prefix+self.original(code, "TEST_Z(MEM8(esp + 4), 1)"), r'''
int main(void) {
 for(unsigned flag=0;flag<4;++flag) for(unsigned ret=0;ret<2;++ret) {
  memset(memory,0,sizeof(memory)); frees=0; ecx=0x1000; esi=0x4321; esp=0x90000;
  MEM32(esp)=0xDEAD0000|ret; MEM32(esp+4)=flag;
  sub_0029DDDD();
  assert(frees==(flag&1) && esp==0x90008 && esi==0x4321);
  assert(eax==0x1000 && MEM32(eax)==0x302ED4);
 }
 return 0;
}
''')

    def test_deferred_update_flag_across_pop(self):
        source = self.function("0029FABF")
        start = source.index("    (void)0; /* test MEM8(esp + 0x18), 1")
        block = source[start:source.index("loc_0029FBAB: ;", start)]
        code = "static unsigned deferred; static void check(void) { int _flags=0;\n" + block + "deferred=0;return; loc_0029FBB1: deferred=1; }\n"
        self.execute(code, self.original(code, "TEST_NZ(MEM8(esp + 0x18), 1)"), r'''
int main(void) {
 for(unsigned flag=0;flag<4;++flag) for(unsigned next=0;next<2;++next) {
  memset(memory,0,sizeof(memory)); esp=0x90000;
  MEM32(esp)=0xBEEF; MEM8(esp+0x18)=flag; MEM8(esp+0x1C)=next;
  check();
  assert(deferred==(flag&1) && esp==0x90004 && ebx==0xBEEF);
 }
 return 0;
}
''')

    def test_voice_mode_reads_original_resource_flags(self):
        source = self.function("002A3E8A")
        start = source.index("loc_002A3EEC: ;")
        block = source[start:source.index("loc_002A3F02: ;", start)]
        code = "static void check(void) {int _flags=0;\n" + block + "loc_002A3F02: ;}\n"
        self.execute(code, self.original(code, "TEST_Z(MEM32(edi + 8), 0x200010)"), r'''
int main(void) {
 const unsigned flags[]={0,0x10,0x200000,0x200010,0x80};
 for(unsigned i=0;i<5;++i) for(unsigned mode=0;mode<4;++mode) {
  memset(memory,0,sizeof(memory)); ebx=0x1000; ebp=0x80000; esp=0x90000;
  MEM32(ebx+0x70)=0x2000; MEM32(0x2008)=flags[i]; MEM32(ebp-32)=mode;
  MEM32(mode+8)=(flags[i]&0x200010)?0:0x200010;
  check();
  assert(edi==((flags[i]&0x200010)&&!mode?3:mode));
  assert(esp==0x90000);
 }
 return 0;
}
''')

    def test_operation_argument_before_push(self):
        code = self.function("002A58C0")
        prefix = r'''
#define CMP_NE(a,b) (!CMP_EQ(a,b))
static unsigned operation;
static void sub_002A578A(void) {operation=1;esp+=4;}
static void sub_002A4CEB(void) {operation=2;esp+=4;}
'''
        self.execute(prefix+code, prefix+self.original(code, "CMP_NE(MEM32(esp + 4), 0)"), r'''
int main(void) {
 for(unsigned arg=0;arg<2;++arg) for(unsigned flag=0;flag<2;++flag) {
  memset(memory,0,sizeof(memory)); ecx=0x1000; esi=0xBEEF; esp=0x90000; operation=0;
  MEM32(esp)=0xDEADBEEF; MEM32(esp+4)=arg;
  MEM32(0x1080)=0x2000; MEM8(0x200A)=flag?4:0; MEM32(0x1148)=123;
  sub_002A58C0();
  assert(operation==(!arg&&flag?1:2));
  assert(esp==0x90008 && esi==0xBEEF && eax==0 && MEM32(0x1148)==0);
 }
 return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
