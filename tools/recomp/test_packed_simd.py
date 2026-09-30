"""Regression coverage for full four-lane packed SSE translation."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00014000
# movaps xmm5,[eax]; movaps xmm4,[ecx+0x10]; addps xmm5,xmm4;
# mulps xmm5,[ecx+0x30]; minps xmm5,xmm3; maxps xmm5,[0x15000];
# movaps [esp+0x30],xmm5; ret
CODE = bytes.fromhex(
    "0f2828"
    "0f286110"
    "0f58ec"
    "0f596930"
    "0f5dec"
    "0f5f2d00500100"
    "0f296c2430"
    "c3"
)


def test_packed_sse_preserves_all_four_lanes():
    config._install(
        [config.Section(".text", BASE, len(CODE), 0, len(CODE), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="packed-simd-test",
    )
    info = {
        "name": f"sub_{BASE:08X}",
        "start": f"0x{BASE:08X}",
        "end": BASE + len(CODE),
        "size": len(CODE),
    }
    translated = FunctionTranslator(CODE, {BASE: info}).translate_function(BASE, info)

    assert "recomp_xmm_load(xmm5v, eax);" in translated
    assert "recomp_xmm_load(xmm4v, ecx + 0x10);" in translated
    assert "RECOMP_XMM_BINARY_RR(xmm5v, xmm4v, +);" in translated
    assert "RECOMP_XMM_BINARY_RM(xmm5v, ecx + 0x30, *);" in translated
    assert "recomp_xmm_minps(xmm5v, xmm4v);" in translated
    assert "recomp_xmm_maxps_mem(xmm5v, 0x15000);" in translated
    assert "recomp_xmm_store(esp + 0x30, xmm5v);" in translated
    assert "packed 4xfloat" not in translated



def test_shuffle_mask_compare_and_partial_moves_are_not_noops():
    code = bytes.fromhex(
        "0f57c0" "0fc6c81b" "0f14ca" "0f15cb" "0fc2ca01"
        "0f50c1" "0f1210" "0f165008" "0f16d3" "0f12d4"
        "0f51da" "0f5218" "0f53da" "0f54ca" "0f56cb" "c3"
    )
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=0, origin="packed-simd-ops-test",
    )
    info = {"name": f"sub_{BASE:08X}", "start": f"0x{BASE:08X}",
            "end": BASE + len(code), "size": len(code)}
    translated = FunctionTranslator(code, {BASE: info}).translate_function(BASE, info)

    expected = (
        "recomp_xmm_zero(xmm0v)", "recomp_xmm_shufps(xmm1v, xmm0v",
        "recomp_xmm_unpcklps(xmm1v, xmm2v)",
        "recomp_xmm_unpckhps(xmm1v, xmm3v)",
        "recomp_xmm_cmp_ps(xmm1v, xmm2v, 2u)",
        "recomp_xmm_movmskps(xmm1v)", "recomp_xmm_movlps_load(xmm2v, eax)",
        "recomp_xmm_movhps_load(xmm2v, eax + 8)",
        "recomp_xmm_movlhps(xmm2v, xmm3v)",
        "recomp_xmm_movhlps(xmm2v, xmm4v)",
        "recomp_xmm_unary_ps(xmm3v, xmm2v, 0u)",
        "recomp_xmm_unary_ps_mem(xmm3v, eax, 1u)",
        "recomp_xmm_unary_ps(xmm3v, xmm2v, 2u)",
        "recomp_xmm_bitwise(xmm1v, xmm2v, 1u)",
        "recomp_xmm_bitwise(xmm1v, xmm3v, 2u)",
    )
    for snippet in expected:
        assert snippet in translated, snippet
    assert "packed compare" not in translated
    assert "\n    /* shufps" not in translated


def test_mmx_word_arithmetic_and_bitwise_ops_are_not_noops():
    # pcmpgtw mm1,mm4; paddw mm1,mm3; psubw mm2,[esp];
    # pand mm1,[esp]; pxor mm0,mm0; pcmpgtd mm5,mm3; ret
    code = bytes.fromhex(
        "0f65cc" "0ffdcb" "0ff91424" "0fdb0c24" "0fefc0" "0f66eb" "c3"
    )
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=0, origin="mmx-packed-ops-test",
    )
    info = {"name": f"sub_{BASE:08X}", "start": f"0x{BASE:08X}",
            "end": BASE + len(code), "size": len(code)}
    translated = FunctionTranslator(code, {BASE: info}).translate_function(BASE, info)

    expected = (
        "recomp_mmx_pcmpgtw(g_mm1, g_mm4)",
        "recomp_mmx_paddw(g_mm1, g_mm3)",
        "recomp_mmx_psubw(g_mm2, MEM64(esp))",
        "g_mm1 & MEM64(esp)",
        "g_mm0 ^ g_mm0",
        "recomp_mmx_pcmpgtd(g_mm5, g_mm3)",
    )
    for snippet in expected:
        assert snippet in translated, snippet
    assert "MMX/SIMD integer" not in translated

def test_xmv_mmx_unpack_pack_shift_and_conversion_ops_are_not_noops():
    code = bytes.fromhex(
        "0f60c4" "0f68da" "0f61d4" "0f69d9" "0f6bd4" "0f67d4"
        "0ffec6" "0ff5cd" "0f72e20f" "0f72f010" "0f73f208"
        "0fd3de" "0f70c8fa" "0f2aca" "0f2dd3" "c3"
    )
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=0, origin="xmv-mmx-ops-test",
    )
    info = {"name": f"sub_{BASE:08X}", "start": f"0x{BASE:08X}",
            "end": BASE + len(code), "size": len(code)}
    translated = FunctionTranslator(code, {BASE: info}).translate_function(
        BASE, info
    )

    expected = (
        "recomp_mmx_punpcklbw(g_mm0, g_mm4)",
        "recomp_mmx_punpckhbw(g_mm3, g_mm2)",
        "recomp_mmx_punpcklwd(g_mm2, g_mm4)",
        "recomp_mmx_punpckhwd(g_mm3, g_mm1)",
        "recomp_mmx_packssdw(g_mm2, g_mm4)",
        "recomp_mmx_packuswb(g_mm2, g_mm4)",
        "recomp_mmx_paddd(g_mm0, g_mm6)",
        "recomp_mmx_pmaddwd(g_mm1, g_mm5)",
        "recomp_mmx_psrad(g_mm2, (uint64_t)(0xF))",
        "recomp_mmx_pslld(g_mm0, (uint64_t)(0x10))",
        "recomp_mmx_psllq(g_mm2, (uint64_t)(8))",
        "recomp_mmx_psrlq(g_mm3, (uint64_t)(g_mm6))",
        "recomp_mmx_pshufw(g_mm0, (uint8_t)0xFA)",
        "recomp_xmm_cvtpi2ps(xmm1v, g_mm2)",
        "recomp_xmm_cvtps2pi(xmm3v, 0u)",
    )
    for snippet in expected:
        assert snippet in translated, snippet
    assert "TODO:" not in translated

if __name__ == "__main__":
    test_packed_sse_preserves_all_four_lanes()
    test_shuffle_mask_compare_and_partial_moves_are_not_noops()
    test_mmx_word_arithmetic_and_bitwise_ops_are_not_noops()
    test_xmv_mmx_unpack_pack_shift_and_conversion_ops_are_not_noops()
    print("ok  packed SIMD preserves four lanes")