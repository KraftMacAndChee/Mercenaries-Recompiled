"""Execute the retail save-clock path against the production EEPROM provider."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(text, name):
    start = text.index("void " + name + "(void)")
    return text[start:text.index("\n}\n", start) + 3]


class SaveLocalTimeTests(unittest.TestCase):
    def test_host_timezone_reaches_retail_save_fields(self):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required")
        kernel = (ROOT / "src/kernel/kernel_xbox.c").read_text(encoding="utf-8")
        start = kernel.index("static LONG xbox_current_timezone_bias(void)")
        end = kernel.index("NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(", start)
        provider = kernel[start:end]
        header = (ROOT / "src/kernel/kernel.h").read_text(encoding="utf-8")
        constants = "\n".join(re.findall(r"^#define XC_.*$", header, re.M))
        text = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0011.c").read_text(encoding="utf-8")
        names = ["sub_002296EE", "sub_0022970F", "sub_00229747",
                 "sub_00229AE3", "sub_00229C3F", "sub_00229C95"]
        generated = "\n".join(function(text, name) for name in names)
        text = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0010.c").read_text(encoding="utf-8")
        generated += function(text, "sub_001FFCD0")
        for label, implementation in (("fixed", provider), ("zero_timezone", provider.replace(
                "settings[1] = (ULONG)xbox_current_timezone_bias();", "settings[1] = 0;"))):
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix="mercs-save-clock-") as folder:
                cfile = Path(folder) / "test.c"
                exe = Path(folder) / "test.exe"
                cfile.write_text(PRELUDE + constants + "\n" + implementation + BRIDGE + generated + MAIN,
                                 encoding="utf-8")
                result = subprocess.run([compiler, "-std=c11", str(cfile), "-o", str(exe)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                result = subprocess.run([str(exe)], capture_output=True)
                if label == "fixed":
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                else:
                    self.assertNotEqual(result.returncode, 0, "UTC regression was not detected")


PRELUDE = r"""
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#define STATUS_SUCCESS ((LONG)0)
#define STATUS_INVALID_PARAMETER ((LONG)0xC000000D)
typedef LONG NTSTATUS;
#define AV_STANDARD_NTSC_M 0x100u
#define AV_FLAGS_60Hz 0x40u
#define xbox_log(...) ((void)0)
static ULONG xbox_get_user_video_flags(void) { return 0x10; }
static TIME_ZONE_INFORMATION test_zone;
static DWORD test_zone_state;
static int use_host_zone;
static DWORD test_timezone(TIME_ZONE_INFORMATION *zone) {
 if(use_host_zone) return GetTimeZoneInformation(zone);
 *zone=test_zone;return test_zone_state;
}
#define GetTimeZoneInformation test_timezone
"""

BRIDGE = r"""
static uint8_t memory[0x400000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
#define g_esp esp
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEM16(a) (*(uint16_t *)(void *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define LO16(a) ((uint16_t)(a))
#define ZX8(a) ((uint32_t)(uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xFFFFFF00u)|(uint8_t)(b))
#define SET_LO16(a,b) ((a)=((a)&0xFFFF0000u)|(uint16_t)(b))
#define PUSH32(s,v) do {uint32_t value_=(v);(s)-=4;MEM32(s)=value_;} while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;} while(0)
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define TEST_NZ(a,b) (!TEST_Z(a,b))
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_G(a,b) ((int32_t)(a)>(int32_t)(b))
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define CMP_AE(a,b) ((uint32_t)(a)>=(uint32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static FILETIME utc;
static void import_call(uint32_t ordinal) {
 if(ordinal==128) { memcpy(memory+MEM32(esp+4),&utc,8);esp+=8;eax=0; }
 else if(ordinal==305) {
  SYSTEMTIME st; CHECK(FileTimeToSystemTime((FILETIME*)(memory+MEM32(esp+4)),&st));
  uint16_t fields[]={st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,st.wDayOfWeek};
  memcpy(memory+MEM32(esp+8),fields,sizeof(fields));esp+=12;eax=0;
 } else CHECK(0);
}
#define RECOMP_ICALL_SAFE(target,stack) import_call(target)
static void sub_0022C5A4(void) {
 uint32_t result=MEM32(esp+20);
 eax=xbox_ExQueryNonVolatileSetting(MEM32(esp+4),(PULONG)(memory+MEM32(esp+8)),
     memory+MEM32(esp+12),MEM32(esp+16),result?(PULONG)(memory+result):NULL);esp+=24;
}
static void sub_0022B178(void) { CHECK(0); }
static void sub_002298D9(void) { CHECK(0); } /* no Xbox DST transitions in host snapshot */
static void sub_00237D10(void) {
 uint64_t a=(uint64_t)MEM32(esp+4)|((uint64_t)MEM32(esp+8)<<32);
 uint64_t b=(uint64_t)MEM32(esp+12)|((uint64_t)MEM32(esp+16)<<32);
 uint64_t product=a*b;eax=(uint32_t)product;edx=(uint32_t)(product>>32);esp+=20;
}
"""

MAIN = r"""
int main(void) {
 /* All use 2026-01-01 03:15:27 UTC. Explicit zone states also cover DST
  * without changing the host computer's timezone. */
 struct {LONG bias,standard,daylight;DWORD state;int y,m,d,h,min;} cases[]={
  {360,0,-60,TIME_ZONE_ID_STANDARD,2025,12,31,21,15},
  {360,0,-60,TIME_ZONE_ID_DAYLIGHT,2025,12,31,22,15},
  {0,0,0,TIME_ZONE_ID_UNKNOWN,2026,1,1,3,15},
  {-330,0,0,TIME_ZONE_ID_UNKNOWN,2026,1,1,8,45},
  {-345,0,0,TIME_ZONE_ID_UNKNOWN,2026,1,1,9,0},
  {-600,0,-30,TIME_ZONE_ID_DAYLIGHT,2026,1,1,13,45},
  {360,0,-60,TIME_ZONE_ID_INVALID,2026,1,1,3,15}
 };
 SYSTEMTIME now={2026,1,0,1,3,15,27,0};CHECK(SystemTimeToFileTime(&now,&utc));
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
  memset(&test_zone,0,sizeof(test_zone));test_zone.Bias=cases[i].bias;
  test_zone.StandardBias=cases[i].standard;test_zone.DaylightBias=cases[i].daylight;
  test_zone_state=cases[i].state;
  memset(memory,0,sizeof(memory));esp=0x300000;
  MEM32(0x2DBCF8)=128;MEM32(0x2DBD54)=305;MEM32(esp+4)=0x1000;
  sub_001FFCD0();
  CHECK(esp==0x300004);
  CHECK(MEM8(0x1000)==27 && MEM8(0x1001)==cases[i].min && MEM8(0x1002)==cases[i].h);
  CHECK(MEM8(0x1003)==cases[i].d && MEM8(0x1004)==cases[i].m && MEM16(0x1006)==cases[i].y);
  ULONG values[25],type=0,length=0;memset(values,0xAB,sizeof(values));
  CHECK(xbox_ExQueryNonVolatileSetting(XC_MAX_OS,&type,values,96,&length)==0);
  CHECK(type==3 && length==96 && values[24]==0xABABABAB);
  LONG bias;CHECK(xbox_ExQueryNonVolatileSetting(XC_TIMEZONE_BIAS,&type,&bias,4,&length)==0);
  CHECK((LONG)values[1]==bias && type==4 && length==4);
  CHECK(values[12]==1 && values[13]==xbox_get_user_video_flags() && values[14]==0x10001 && values[23]==1);
  memset(values,0xAB,sizeof(values));
  CHECK(xbox_ExQueryNonVolatileSetting(XC_MAX_OS,&type,values,95,&length)==(LONG)0xC0000023);
  CHECK(length==96 && values[0]==0xABABABAB && values[23]==0xABABABAB);
  CHECK(xbox_ExQueryNonVolatileSetting(XC_TIMEZONE_BIAS,&type,values,3,&length)==(LONG)0xC0000023);
  CHECK(length==4 && values[0]==0xABABABAB);
 }
 /* Check the actual Windows timezone against the same captured UTC instant. */
 use_host_zone=1;
 GetSystemTimeAsFileTime(&utc);
 FILETIME local;SYSTEMTIME expected;
 CHECK(FileTimeToLocalFileTime(&utc,&local) && FileTimeToSystemTime(&local,&expected));
 esp=0x300000;MEM32(esp+4)=0x1000;sub_001FFCD0();
 CHECK(MEM8(0x1000)==expected.wSecond && MEM8(0x1001)==expected.wMinute);
 CHECK(MEM8(0x1002)==expected.wHour && MEM8(0x1003)==expected.wDay);
 CHECK(MEM8(0x1004)==expected.wMonth && MEM16(0x1006)==expected.wYear);
 return 0;
}
"""

if __name__ == "__main__":
    unittest.main()
