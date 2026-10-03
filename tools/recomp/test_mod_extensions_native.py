"""Exercise opt-in config, retail grenade accessors and stun collision dispatch."""
from pathlib import Path
import os
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp.generated_test_utils import generated_text_containing
from tools.diagnostics.inspect_retail_templates import hash_string

class ModExtensionTests(unittest.TestCase):
    def test_native_extensions(self):
        names = ['0004CA70','0004CAA0','0004CD90','0004CF80','0005B1F0','000579C0',
                 '000212E0','0001CAD0','00033550','00034040','000CD700']
        bodies = []
        for name in names:
            text = generated_text_containing('void sub_'+name+'(void)\n{')
            bodies.append(re.search(r'void sub_'+name+r'\(void\)\n\{.*?\n\}', text, re.S)[0])
        prefix = r"""
#define RECOMP_GENERATED_CODE
#include "recomp/recomp_types.h"
#include "mod_compatibility.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x800000];
enum { PLAYER=0x10000, NPC=0x11000, ITEM=0x20000, SHOT=0x30000,
       PROP=0x40000, SHOP=0x50000, STACK=0x600000, POOL=0x700000, VT=0x710000 };
static unsigned allocations, frees, deleted, bounces, explosions, order_attempts;
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t align) {
    assert(size==12 && align==16 && allocations==frees); ++allocations; return POOL;
}
void xbox_HeapFree(uint32_t addr) { assert(addr==POOL); ++frees; }
void xbox_SetExtendedGraphicsMemory(BOOL enable) { (void)enable; }
static void dispatch(uint32_t target) {
    if(target==0x3DC) { MEM32(ecx+0x7BC)=MEM32(esp+4); esp+=8; return; }
    if(target==0x98) { eax=MEM32(esp+4); MEMF(eax)=1; MEMF(eax+4)=2; MEMF(eax+8)=3; esp+=8; return; }
    assert(target==0x1DC || target==0x1CC || target==0x10 || target==0x1D8 || target==0x1D0);
    if(target==0x10) ++deleted;
    esp+=4;
}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(t,s) dispatch(t)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(t) dispatch(t)
static void sub_000C9CA0(void) { esp+=8; }
static void sub_0008C7E0(void) { ++order_attempts; eax=0; esp+=4; }
static void sub_000C8710(void) { esp+=4; }
static void sub_00017370(void) { ++bounces; esp+=4; }
static void sub_001EF7B0(void) { eax=ecx; esp+=4; }
static void sub_000994F0(void) { esp+=16; }
static void sub_000B6460(void) {
    assert(MEM32(esp+28)==0 && MEM32(esp+32)==7 && MEM32(esp+36)==10);
    ++explosions; esp+=4;
}
static void sub_00030B60(void) { esp+=8; }
static void sub_00061FF0(void) { assert(0); }
static void sub_00011D80(void) { assert(0); }
"""
        defined=set(names)|{'00017370','001EF7B0','000994F0','000B6460','00030B60','00061FF0','00011D80','000C9CA0','0008C7E0','000C8710'}
        calls=set(re.findall(r'sub_([0-9A-F]{8})\(\)', '\n'.join(bodies)))
        prefix+='\n'.join('static void sub_'+n+'(void) { assert(!"unexpected retail branch '+n+'"); }' for n in sorted(calls-defined))+'\n'
        prefix+='\n'.join('void sub_'+n+'(void);' for n in names)+'\n'
        tail = r"""
static void call1(void (*fn)(void), uint32_t self, uint32_t arg) {
    ecx=self; esp=STACK; esi=0x1234; edi=0x5678; ebx=0x9ABC; g_seh_ebp=0x1111;
    MEM32(esp)=0xABCDEF; MEM32(esp+4)=arg;
    fn(); assert(esp==STACK+8 && esi==0x1234 && edi==0x5678 && ebx==0x9ABC);
}
static void call0(void (*fn)(void), uint32_t self) {
    ecx=self; esp=STACK; esi=0x1234; edi=0x5678; ebx=0x9ABC; g_seh_ebp=0x1111;
    fn(); assert(esp==STACK+4 && esi==0x1234 && edi==0x5678 && ebx==0x9ABC);
}
int main(int argc,char **argv) {
    int enabled=argc>1 && atoi(argv[1]);
    g_xbox_mem_offset=(ptrdiff_t)memory;
    recomp_mod_compatibility_init();
    assert(recomp_mod_extra_grenade_slot()==enabled);
    MEM32(PLAYER)=0x2E32B8; MEM32(NPC)=VT;
    MEMF(0x2E6CE8)=-0.6f;
    MEM32(ITEM+0x1A4)=AMMO_HASH;
    MEMF(0x323338+0x1C+(6*8+1)*4)=-1;
    assert(recomp_mod_weapon_allowed(ITEM,PLAYER)==!enabled);
    if(enabled) { call1(sub_00033550,ITEM,PLAYER); assert(!LO8(eax)); call1(sub_00034040,ITEM,PLAYER); assert(!LO8(eax)); }
    assert(recomp_mod_weapon_allowed(ITEM,NPC));
    assert(recomp_mod_weapon_fixed_price(ITEM)==enabled);
    MEMF(0x323338+0x1C+(6*8+1)*4)=-0.6f;
    assert(recomp_mod_weapon_allowed(ITEM,PLAYER)==!enabled);
    MEMF(0x323338+0x1C+(6*8+1)*4)=-0.599f;
    assert(recomp_mod_weapon_allowed(ITEM,PLAYER));
    MEMF(0x323338+0x1C+(4*8+1)*4)=-1;
    MEM32(SHOP+0x1794)=1; MEM32(SHOP+0x1190)=1;
    MEM32(SHOP+0x1194)=0; MEM32(SHOP+0x198)=SHOP_HASH;
    assert(recomp_mod_pda_allowed(SHOP,0));
    assert(recomp_mod_pda_fixed_price(SHOP,0)==enabled);
    assert(recomp_mod_pda_has_available_supplier(SHOP)==enabled);
    MEMF(0x323338+0x1C+(6*8+1)*4)=-1;
    assert(recomp_mod_pda_allowed(SHOP,0)==!enabled);
    assert(!recomp_mod_pda_has_available_supplier(SHOP));
    MEM32(ITEM+0x1A4)=123;
    assert(recomp_mod_weapon_allowed(ITEM,PLAYER) && !recomp_mod_weapon_fixed_price(ITEM));
    MEM32(ITEM+0x1A4)=MAFIA_HASH;
    assert(!recomp_mod_weapon_fixed_price(ITEM));
    assert(recomp_mod_weapon_allowed(ITEM,PLAYER)==!enabled);

    recomp_mod_player_inventory_begin(PLAYER);
    assert(recomp_mod_secondary_capacity(PLAYER)==(enabled?3:2));
    assert(recomp_mod_secondary_base(NPC)==NPC+0x7C0);
    assert(recomp_mod_secondary_capacity(NPC)==2);
    uint32_t base=recomp_mod_secondary_base(PLAYER);
    MEM32(PLAYER+0x7CC)=0xDEADBEEF;
    for(unsigned i=0;i<3;++i) {
        MEM32(ITEM+i*0x1000)=VT;
        MEM32(VT+0x1DC)=0x1DC; MEM32(VT+0x1CC)=0x1CC; MEM32(VT+0x10)=0x10;
    }
    MEM32(0x2E32B8+0x3DC)=0x3DC;
    for(unsigned i=0;i<(enabled?3u:2u);++i)
        call1(sub_0005B1F0,PLAYER,ITEM+i*0x1000);
    assert(MEM32(PLAYER+0x7C8)==(enabled?3:2));
    assert(MEM32(PLAYER+0x7CC)==0xDEADBEEF);
    call1(sub_0004CF80,PLAYER,2); assert(eax==(enabled?ITEM+0x2000:0));
    call1(sub_0004CA70,PLAYER,ITEM+0x2000); assert(LO8(eax)==enabled);
    call1(sub_0004CD90,PLAYER,0); assert(MEM32(PLAYER+0x7BC)==ITEM);
    call0(sub_0004CAA0,PLAYER); assert(MEM32(PLAYER+0x7BC)==ITEM+0x1000);
    call0(sub_0004CAA0,PLAYER); assert(MEM32(PLAYER+0x7BC)==(enabled?ITEM+0x2000:ITEM));
    if(enabled) { call0(sub_0004CAA0,PLAYER); assert(MEM32(PLAYER+0x7BC)==ITEM); }
    call0(sub_000579C0,PLAYER);
    assert(deleted==(enabled?3:2) && MEM32(PLAYER+0x7C8)==0 && MEM32(PLAYER+0x7BC)==0);
    recomp_mod_player_inventory_end(PLAYER);
    assert(allocations==frees && recomp_mod_secondary_base(PLAYER)==PLAYER+0x7C0);

    // Exercise the order action, not only its display/affordability helper.
    MEM32(SHOP+0x198)=SHOP_HASH; MEM32(SHOP+0x19AC)=2; MEM32(SHOP+0x17A4)=0;
    MEM8(0xCDA4C)=0; MEM32(0xCDA28)=0xCD945;
    MEMF(0x323338+0x1C+(6*8+1)*4)=-1;
    call1(sub_000CD700,SHOP,17); assert(order_attempts==!enabled);
    MEMF(0x323338+0x1C+(6*8+1)*4)=0;
    call1(sub_000CD700,SHOP,17); assert(order_attempts==(enabled?1:2));

    MEM32(SHOT)=VT; MEM32(VT+0x98)=0x98;
    MEM32(SHOT+8)=PROP; MEM32(PROP+0x28)=123456; MEM32(SHOT+4)=0;
    MEM32(PROP+4)=PROP+0x100; MEM16(PROP+0x104)=1; MEM32(PROP+0x108)=PROP+0x200;
    MEM32(PROP+0x200)=0x8D39BDE6; MEM32(PROP+0x204)=STUN_HASH;
    MEMF(SHOT+0x174)=100; MEMF(SHOT+0x18C)=200;
    MEM32(SHOT+0x1A0)=0xECA96359;
    assert(recomp_mod_projectile_impact_stun(SHOT)==enabled);
    // A derived property table inherits Name from its parent.
    MEM32(PROP+4)=PROP+0x300; MEM32(PROP+0x300)=PROP+0x100;
    assert(recomp_mod_projectile_impact_stun(SHOT)==enabled);
    recomp_mod_projectile_init(SHOT);
    assert(MEMF(SHOT+0x18C)==(enabled?0:200));
    call0(sub_000212E0,SHOT);
    assert(LO8(eax)==enabled && explosions==enabled && bounces==!enabled);
    MEM32(PROP+0x204)=999; // An ordinary stun grenade still bounces.
    call0(sub_000212E0,SHOT); assert(!LO8(eax) && bounces==(enabled?1:2));
    puts("PASS: config, factions, inventory add/cycle/clear, collision stun, vanilla isolation");
    return 0;
}
"""
        hashes={'AMMO_HASH':'template_sam_deliverAlliesHumvee','SHOP_HASH':'allies_humvee','MAFIA_HASH':'template_sam_mafia_test','STUN_HASH':'template_amm_STUNGRENADELAUNCHER'}
        for macro,name in hashes.items(): prefix+=f'\n#define {macro} 0x{hash_string(name):08X}u\n'
        cc=next((p for p in [shutil.which('gcc'), 'C:/MinGW/bin/gcc.exe','C:/msys64/mingw64/bin/gcc.exe'] if p and Path(p).exists()),None)
        self.assertIsNotNone(cc,'GCC is required')
        with tempfile.TemporaryDirectory(prefix='merc-mod-extensions-') as tmp:
            folder=Path(tmp); source=folder/'test.c';exe=folder/'test.exe'
            source.write_text(prefix+'\n'.join(bodies)+tail)
            port=ROOT/'ports/mercenaries/src'
            result=subprocess.run([cc,'-std=c11','-O2','-include','windows.h','-I',str(ROOT/'src'),'-I',str(port),str(source),str(port/'mod_compatibility.c'),str(port/'mod_extensions.c'),'-o',str(exe)],capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            mappings='[SupportFactions]\ntemplate_sam_deliverAlliesHumvee=allies\nallies_humvee=allies\ntemplate_sam_mafia_test=mafia\n[ImpactStunAmmo]\ntemplate_amm_STUNGRENADELAUNCHER=1\n'
            for enabled in [0,1]:
                (folder/'modcompatibility.ini').write_text(f'[Mods]\nfaction_support={enabled}\nimpact_stun_ammo={enabled}\nextra_grenade_slot={enabled}\n'+mappings)
                result=subprocess.run([str(exe),str(enabled)],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            for invalid in ['[SupportFactions]\nbad=alliess\n','[ImpactStunAmmo]\nbad=2\n']:
                (folder/'modcompatibility.ini').write_text('[Mods]\nfaction_support=1\nimpact_stun_ammo=1\n'+invalid)
                result=subprocess.run([str(exe),'0'],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,86,result.stderr)

    def test_patch_round_trip(self):
        patches=runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Mod-Extensions-Patches.py'))['PATCHES']
        sources={p:p.read_text() for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c')}
        for name,before,after in reversed(patches):
            matches=[p for p,s in sources.items() if after in s]
            self.assertEqual(len(matches),1,name)
            p=matches[0];self.assertEqual(sources[p].count(after),1,name)
            sources[p]=sources[p].replace(after,before)
        for name,before,after in patches:
            matches=[p for p,s in sources.items() if before in s]
            self.assertEqual(len(matches),1,name)
            p=matches[0];self.assertEqual(sources[p].count(before),1,name)
            sources[p]=sources[p].replace(before,after)
        for p,s in sources.items():self.assertEqual(s,p.read_text(),p.name)

if __name__=='__main__':unittest.main()
