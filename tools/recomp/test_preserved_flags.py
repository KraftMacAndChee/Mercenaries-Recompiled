"""Regression coverage for EFLAGS surviving register restores."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00013000


def _translate(code: bytes, base: int = BASE, section: str | None = None,
               reviewed_range: bool = False) -> str:
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="preserved-flags-test",
    )
    functions = {
        base: {
            "name": f"sub_{base:08X}",
            "start": f"0x{base:08X}",
            "end": base + len(code),
            "size": len(code),
            "section": section,
            "_reviewed_range": reviewed_range,
        }
    }
    return FunctionTranslator(code, functions).translate_function(
        base, functions[base]
    )


def test_non_xmv_branch_inherits_actual_predecessor_comparison():
    # cmp eax,2; jne target; test al,al; ret; target: mov edi,[esp+4];
    # jge done; inc ecx; done: ret. The TEST is on a different CFG path.
    source = _translate(bytes.fromhex("83f802750384c0c38b7c24047d0141c3"))
    assert "if (CMP_GE(eax, 2)) goto" in source, source
    assert "((LO8(eax) & LO8(eax))) >=" not in source


def test_non_xmv_cmp_carry_reaches_sbb():
    # cmp eax,25; mov edx,[esp+4]; sbb ecx,ecx; ret
    source = _translate(bytes.fromhex("83f8198b5424041bc9c3"))
    assert "_cf = ((uint32_t)(eax) < (uint32_t)(0x19));" in source, source
    assert source.index("_cf =") < source.index("ecx = _cf ?")


def test_cmp_pop_je_snapshots_condition_before_pop():
    # cmp eax, esi; pop esi; je +1; ret; ret
    source = _translate(bytes.fromhex("39f05e7401c3c3"))

    snapshot = "_flags = (CMP_EQ(eax, esi));"
    assert snapshot in source
    assert source.index(snapshot) < source.index("POP32(esp, esi);")
    assert "if (_flags != 0) goto" in source
    assert "if (CMP_EQ(eax, esi)) goto" not in source



def test_cross_block_cmp_pop_jl_snapshots_before_join_restore():
    # Retail Store next-item accessor shape: both incoming paths compare
    # EAX against the item count in ESI, then a shared block restores the
    # caller's EDI/ESI before JL consumes those comparison flags.
    source = _translate(bytes.fromhex(
        "39f07d034039f05f5e7c01c3c3"
    ))

    snapshot = "_flags = (CMP_L(eax, esi));"
    assert snapshot in source, source
    assert source.index(snapshot) < source.index("POP32(esp, edi);")
    assert "if (_flags != 0) goto" in source
    assert "if (CMP_L(eax, esi)) goto" not in source


def test_narrow_test_pop_full_register_jns_snapshots_condition():
    # test bl,bl; pop ebx; jns +1; ret; ret
    source = _translate(bytes.fromhex("84db5b7901c3c3"))

    snapshot = (
        "_flags = (((int8_t)(uint8_t)(LO8(ebx) & LO8(ebx)) >= 0));"
    )
    assert snapshot in source
    assert source.index(snapshot) < source.index("POP32(esp, ebx);")
    assert "if (_flags != 0) goto" in source
    assert "if (((int32_t)(LO8(ebx) & LO8(ebx)) >= 0)) goto" not in source

def test_dec_mov_jne_snapshots_condition_before_register_reload():
    # dec ecx; mov [esp+4], ecx; mov ecx, [esp+8]; jne +1; ret; ret
    source = _translate(bytes.fromhex("49894c24048b4c24087501c3c3"))

    snapshot = "_flags = ((ecx != 0));"
    assert snapshot in source
    assert source.index(snapshot) < source.index("ecx = MEM32(esp + 8);")
    assert "if (_flags != 0) goto" in source
    assert "if ((ecx != 0)) goto" not in source


def test_test_x87_work_register_reload_snapshots_condition():
    # Retail options-slider renderer idiom:
    # test bl,bl; fst [esp+3Ch]; fadd [esp+18h]; fstp [esp+18h];
    # mov ebx,[esp+18h]; jz. The x87 operations and MOV preserve EFLAGS,
    # so JZ must consume TEST's result rather than the coordinate reloaded
    # into EBX immediately before the branch.
    source = _translate(bytes.fromhex(
        "84dbd954243cd8442418d95c24188b5c24187401c3c3"
    ))

    snapshot = "_flags = (TEST_Z(LO8(ebx), LO8(ebx)));"
    reload = "ebx = MEM32(esp + 0x18);"
    assert snapshot in source, source
    assert source.index(snapshot) < source.index(reload)
    assert "if (_flags != 0) goto" in source
    assert "if (TEST_Z(LO8(ebx), LO8(ebx))) goto" not in source


def test_cmp_mov_cmove_snapshots_condition_before_register_reload():
    # cmp dl,2Eh; mov dl,[ecx+1]; cmove esi,ecx; ret
    source = _translate(bytes.fromhex("80fa2e8a51010f44f1c3"))

    snapshot = "_flags = (((uint8_t)(LO8(edx)) == (uint8_t)(0x2E)));"
    reload = "SET_LO8(edx, MEM8(ecx + 1));"
    assert snapshot in source
    assert source.index(snapshot) < source.index(reload)
    assert "if (_flags != 0) esi = ecx; /* cmove */" in source
    assert "if (((uint8_t)(LO8(edx)) == (uint8_t)(0x2E))) esi = ecx" not in source


def test_cmp_memory_lea_two_cmovs_snapshot_address_registers():
    # cmp word [eax+ecx*4],si; lea ecx,[eax+ecx*4];
    # cmovb eax,ecx; cmovae edx,ecx; ret
    source = _translate(bytes.fromhex(
        "663934888d0c880f42c10f43d1c3"
    ))

    snapshot = (
        "_flags = (((uint16_t)(MEM16(eax + ecx * 4)) "
        "< (uint16_t)(LO16(esi))));"
    )
    reload = "ecx = eax + ecx * 4;"
    assert snapshot in source
    assert source.index(snapshot) < source.index(reload)
    assert "if (_flags != 0) eax = ecx; /* cmovb */" in source
    assert "if (_flags == 0) edx = ecx; /* cmovae */" in source
    assert "MEM16(eax + ecx * 4)" not in source[source.index(reload):]


def test_cmp_carry_survives_flag_neutral_loads_into_sbb_self():
    # Retail XMV VLC decode idiom:
    # cmp eax,[esi+8]; mov ecx,[esi+0Ch]; movzx ecx,[ecx+eax];
    # mov edx,[esi+18h]; movsx edi,[edx+eax]; sbb ebx,ebx; inc ebx; ret
    source = _translate(bytes.fromhex(
        "3b46088b4e0c0fb60c018b56180fbe3c021bdb43c3"
    ), base=0x002573C8)

    carry = "_cf = ((uint32_t)(eax) < (uint32_t)(MEM32(esi + 8)));"
    assert carry in source
    assert source.index(carry) < source.index("ecx = MEM32(esi + 0xC);")
    assert "ebx = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */" in source

def test_different_flag_producers_snapshot_simple_cfg_join():
    # Two single-successor paths reach one JE with different flag producers.
    # Source order must not make the later CMP stand in for TEST on the jump
    # edge (the retail XMV coefficient decoder uses this exact CFG shape).
    source = _translate(bytes.fromhex(
        "83f900740783fa0089c7eb0485c0eb007401c3c3"
    ), base=0x00257E3D, section="XMV")

    cmp_snapshot = (
        "_flags = (CMP_EQ(edx, 0)); /* preserve flags for je at CFG join"
    )
    test_snapshot = (
        "_flags = (TEST_Z(eax, eax)); /* preserve flags for je at CFG join"
    )
    assert cmp_snapshot in source
    assert test_snapshot in source
    assert source.count("if (_flags != 0) goto") == 1


def test_reviewed_range_same_kind_producers_snapshot_cfg_join():
    # Two CMP-producing, single-successor paths reach the same JE.  The
    # comparator owner at 0x658B0 has this shape with different operands.
    source = _translate(bytes.fromhex(
        "83f900740583fa00eb0583f800eb007401c3c3"
    ), reviewed_range=True)

    assert "_flags = (CMP_EQ(edx, 0));" in source, source
    assert "_flags = (CMP_EQ(eax, 0));" in source, source
    assert source.count("if (_flags != 0) goto") == 1


def test_single_predecessor_branch_target_inherits_its_flags():    # test eax,eax; jne target; and [ecx],eax; jmp done;
    # target: jle done; ret; done: ret
    # JLE consumes TEST's flags on the taken JNE edge. The intervening AND is
    # earlier only in source order and must not become the target's producer.
    source = _translate(bytes.fromhex(
        "85c075042101eb037e01c3c3"
    ), base=0x0025769D, section="XMV")

    assert "if (CMP_LE((eax & eax), 0)) goto" in source
    assert "if (((int32_t)MEM32(ecx) <= 0)) goto" not in source

def test_memory_test_pop_snapshots_address_dependency():
    # Retail DSOUND 002A3601: test dword [esi+8],82000h; pop esi; je.
    source = _translate(bytes.fromhex("f74608002008005e7401c3c3"))
    snapshot = "_flags = (TEST_Z(MEM32(esi + 8), 0x82000));"
    assert snapshot in source
    assert source.index(snapshot) < source.index("POP32(esp, esi);")
    assert "if (_flags != 0) goto" in source
    assert "if (TEST_Z(MEM32(esi + 8), 0x82000)) goto" not in source


def test_memory_cmp_index_reload_setne_snapshots_address_dependency():
    # cmp dword [eax+ecx*4],1; mov ecx,[ecx+4]; setne al; ret.
    source = _translate(bytes.fromhex("833c88018b49040f95c0c3"))
    snapshot = "_flags = (CMP_NE(MEM32(eax + ecx * 4), 1));"
    assert snapshot in source
    assert source.index(snapshot) < source.index("ecx = MEM32(ecx + 4);")
    assert "SET_LO8(eax, (_flags != 0) ? 1 : 0);" in source


def test_stack_argument_test_push_snapshots_implicit_esp_change():
    # test byte [esp+4],1; push esi; mov esi,ecx; je +1; nop; pop esi; ret 4
    source = _translate(bytes.fromhex("f644240401568bf17401905ec20400"))
    snapshot = "_flags = (TEST_Z(MEM8(esp + 4), 1));"
    assert snapshot in source
    assert source.index(snapshot) < source.index("PUSH32(esp, esi);")
    assert "if (TEST_Z(MEM8(esp + 4), 1)) goto" not in source


def test_stack_argument_cmp_pop_snapshots_implicit_esp_change():
    # cmp dword [esp+8],1; pop edi; setne al; ret
    source = _translate(bytes.fromhex("837c2408015f0f95c0c3"))
    snapshot = "_flags = (CMP_NE(MEM32(esp + 8), 1));"
    assert snapshot in source
    assert source.index(snapshot) < source.index("POP32(esp, edi);")
    assert "SET_LO8(eax, (_flags != 0) ? 1 : 0);" in source


def test_helicopter_comiss_stack_argument_survives_push():
    # Retail 13ADD0 entry, with a short JAE and two balanced test exits.
    source = _translate(bytes.fromhex(
        "0f57c083ec240f2f442428568bf17301905e83c424c20400"
    ))
    snapshot = "_flags = ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x28)))) && (xmm0 >= MEMF(esp + 0x28))));"
    assert snapshot in source, source
    assert source.index(snapshot) < source.index("PUSH32(esp, esi);")
    assert "if (_flags != 0) goto" in source


def test_ucomisd_stack_argument_survives_pop_for_setcc():
    # ucomisd xmm0,qword [esp+8]; pop edi; setae al; ret
    source = _translate(bytes.fromhex("660f2e4424085f0f93c0c3"))
    assert "preserve ucomisd flags across 1 instruction(s)" in source, source
    assert source.index("_flags =") < source.index("POP32(esp, edi);")
    assert "SET_LO8(eax, (_flags != 0) ? 1 : 0);" in source


def test_comiss_index_address_survives_lea():
    # Retail boundary: comiss xmm1,[ebp+ecx*4]; lea ecx,[ebp+ecx*4]; jbe.
    source = _translate(bytes.fromhex("0f2f4c8d008d4c8d00760190c3"))
    assert "preserve comiss flags across 1 instruction(s)" in source, source
    assert source.index("_flags =") < source.index("ecx = ebp + ecx * 4;")
    assert "if (_flags != 0) goto" in source


def test_comiss_object_address_survives_register_restore():
    source = _translate(bytes.fromhex("0f2f83d40200005f5e5b760190c3"))
    assert "preserve comiss flags across 3 instruction(s)" in source, source
    assert source.index("_flags =") < source.index("POP32(esp, edi);")
    assert "if (_flags != 0) goto" in source


if __name__ == "__main__":
    test_comiss_index_address_survives_lea()
    test_comiss_object_address_survives_register_restore()
    test_helicopter_comiss_stack_argument_survives_push()
    test_ucomisd_stack_argument_survives_pop_for_setcc()
    test_non_xmv_cmp_carry_reaches_sbb()
    test_non_xmv_branch_inherits_actual_predecessor_comparison()
    test_stack_argument_test_push_snapshots_implicit_esp_change()
    test_stack_argument_cmp_pop_snapshots_implicit_esp_change()
    test_memory_test_pop_snapshots_address_dependency()
    test_memory_cmp_index_reload_setne_snapshots_address_dependency()
    test_cmp_pop_je_snapshots_condition_before_pop()
    test_cross_block_cmp_pop_jl_snapshots_before_join_restore()
    test_narrow_test_pop_full_register_jns_snapshots_condition()
    test_dec_mov_jne_snapshots_condition_before_register_reload()
    test_test_x87_work_register_reload_snapshots_condition()
    test_cmp_mov_cmove_snapshots_condition_before_register_reload()
    test_cmp_memory_lea_two_cmovs_snapshot_address_registers()
    test_cmp_carry_survives_flag_neutral_loads_into_sbb_self()
    test_different_flag_producers_snapshot_simple_cfg_join()
    test_reviewed_range_same_kind_producers_snapshot_cfg_join()
    test_single_predecessor_branch_target_inherits_its_flags()
    print("ok  preserved_flags")
