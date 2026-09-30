#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mmintrin.h>
#include <xmmintrin.h>

#define RECOMP_GENERATED_CODE 1
#include "../../ports/mercenaries/src/recomp/recomp_types.h"

static uint64_t bits(__m64 value) {
    uint64_t out;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

static __m64 mm(uint64_t value) {
    __m64 out;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

static void check(const char *name, uint64_t got, uint64_t expected,
                  uint64_t left, uint64_t right) {
    if (got != expected) {
        std::fprintf(stderr,
                     "%s mismatch: left=%016llX right=%016llX "
                     "got=%016llX expected=%016llX\n",
                     name, (unsigned long long)left,
                     (unsigned long long)right, (unsigned long long)got,
                     (unsigned long long)expected);
        std::exit(1);
    }
}

static uint64_t next_random(uint64_t &state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(0x2545F4914F6CDD1D);
}

int main() {
    uint64_t state = UINT64_C(0x7A6D4C9E21B583F1);
    for (unsigned sample = 0; sample < 200000; ++sample) {
        const uint64_t a = next_random(state);
        const uint64_t b = next_random(state);
        const __m64 ma = mm(a);
        const __m64 mb = mm(b);

        check("paddb", recomp_mmx_paddb(a, b), bits(_mm_add_pi8(ma, mb)), a, b);
        check("psubb", recomp_mmx_psubb(a, b), bits(_mm_sub_pi8(ma, mb)), a, b);
        check("paddw", recomp_mmx_paddw(a, b), bits(_mm_add_pi16(ma, mb)), a, b);
        check("psubw", recomp_mmx_psubw(a, b), bits(_mm_sub_pi16(ma, mb)), a, b);
        check("paddd", recomp_mmx_paddd(a, b), bits(_mm_add_pi32(ma, mb)), a, b);
        check("psubd", recomp_mmx_psubd(a, b), bits(_mm_sub_pi32(ma, mb)), a, b);
        check("pmullw", recomp_mmx_pmullw(a, b), bits(_mm_mullo_pi16(ma, mb)), a, b);
        check("pmaddwd", recomp_mmx_pmaddwd(a, b), bits(_mm_madd_pi16(ma, mb)), a, b);
        check("pavgb", recomp_mmx_pavgb(a, b), bits(_mm_avg_pu8(ma, mb)), a, b);
        check("pcmpeqb", recomp_mmx_pcmpeqb(a, b), bits(_mm_cmpeq_pi8(ma, mb)), a, b);
        check("pcmpeqw", recomp_mmx_pcmpeqw(a, b), bits(_mm_cmpeq_pi16(ma, mb)), a, b);
        check("pcmpgtb", recomp_mmx_pcmpgtb(a, b), bits(_mm_cmpgt_pi8(ma, mb)), a, b);
        check("pcmpgtw", recomp_mmx_pcmpgtw(a, b), bits(_mm_cmpgt_pi16(ma, mb)), a, b);
        check("pcmpgtd", recomp_mmx_pcmpgtd(a, b), bits(_mm_cmpgt_pi32(ma, mb)), a, b);
        check("punpcklbw", recomp_mmx_punpcklbw(a, b), bits(_mm_unpacklo_pi8(ma, mb)), a, b);
        check("punpckhbw", recomp_mmx_punpckhbw(a, b), bits(_mm_unpackhi_pi8(ma, mb)), a, b);
        check("punpcklwd", recomp_mmx_punpcklwd(a, b), bits(_mm_unpacklo_pi16(ma, mb)), a, b);
        check("punpckhwd", recomp_mmx_punpckhwd(a, b), bits(_mm_unpackhi_pi16(ma, mb)), a, b);
        check("punpckldq", recomp_mmx_punpckldq(a, b), bits(_mm_unpacklo_pi32(ma, mb)), a, b);
        check("punpckhdq", recomp_mmx_punpckhdq(a, b), bits(_mm_unpackhi_pi32(ma, mb)), a, b);
        check("packssdw", recomp_mmx_packssdw(a, b), bits(_mm_packs_pi32(ma, mb)), a, b);
        check("packsswb", recomp_mmx_packsswb(a, b), bits(_mm_packs_pi16(ma, mb)), a, b);
        check("packuswb", recomp_mmx_packuswb(a, b), bits(_mm_packs_pu16(ma, mb)), a, b);

        const uint64_t count = next_random(state) & 127u;
        const __m64 mc = mm(count);
        check("psllw", recomp_mmx_psllw(a, count), bits(_mm_sll_pi16(ma, mc)), a, count);
        check("psrlw", recomp_mmx_psrlw(a, count), bits(_mm_srl_pi16(ma, mc)), a, count);
        check("psraw", recomp_mmx_psraw(a, count), bits(_mm_sra_pi16(ma, mc)), a, count);
        check("pslld", recomp_mmx_pslld(a, count), bits(_mm_sll_pi32(ma, mc)), a, count);
        check("psrld", recomp_mmx_psrld(a, count), bits(_mm_srl_pi32(ma, mc)), a, count);
        check("psrad", recomp_mmx_psrad(a, count), bits(_mm_sra_pi32(ma, mc)), a, count);
        check("psllq", recomp_mmx_psllq(a, count), bits(_mm_sll_si64(ma, mc)), a, count);
        check("psrlq", recomp_mmx_psrlq(a, count), bits(_mm_srl_si64(ma, mc)), a, count);

        check("pshufw-50", recomp_mmx_pshufw(a, 0x50),
              bits(_mm_shuffle_pi16(ma, 0x50)), a, 0x50);
        check("pshufw-fa", recomp_mmx_pshufw(a, 0xFA),
              bits(_mm_shuffle_pi16(ma, 0xFA)), a, 0xFA);
    }

    const uint64_t edge = UINT64_C(0x8000800080008000);
    check("pmaddwd-overflow", recomp_mmx_pmaddwd(edge, edge),
          bits(_mm_madd_pi16(mm(edge), mm(edge))), edge, edge);

    _mm_empty();
    std::puts("ok: packed MMX helpers match native x86 semantics");
    return 0;
}