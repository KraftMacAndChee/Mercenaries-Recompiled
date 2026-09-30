/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * Runtime template for generated code. Each port must supply the memory
 * mapping and compatibility services required by its supported executable.
 *
 * Memory model:
 *   Guest-memory accesses translate Xbox VAs through g_xbox_mem_offset.
 *   The port supplies the mapping and initializes that offset before
 *   guest execution. Native C executes the translated instructions.
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   through global registers and the simulated Xbox stack.
 *   Return values are communicated through g_eax.
 *   The call instruction pushes a dummy return address; ret pops it.
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions. */
#if !defined(_MSC_VER) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Guest-to-host address translation: host_address = xbox_va + offset.
 * Set during memory initialization before guest execution, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;

/** x87 register stack, shared across translated function calls. */
extern double g_fp_stack[8];
extern uint32_t g_fp_top;
extern uint16_t g_x87_control_word;
extern uint16_t g_x87_status_word;

/** SSE and MMX architectural registers, shared across translated calls. */
extern float g_xmm0[4], g_xmm1[4], g_xmm2[4], g_xmm3[4];
extern float g_xmm4[4], g_xmm5[4], g_xmm6[4], g_xmm7[4];
extern uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
extern uint64_t g_mm4, g_mm5, g_mm6, g_mm7;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
extern uint32_t g_seh_ebp;

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);
/* Game-specific renderer bridges used by persistent generated-code patches. */
void recomp_d3d_set_stream_source(uint32_t stream, uint32_t vertex_buffer,
                                  uint32_t stride);
void recomp_d3d_set_vertex_shader(uint32_t shader);
void recomp_xact_stream_checkpoint(uint32_t object, uint32_t descriptor,
                                    uint32_t format);
void recomp_xact_list_checkpoint(uint32_t stage, uint32_t manager,
                                  uint32_t object);
void recomp_xact_cue_checkpoint(uint32_t stage, uint32_t manager,
                                uint32_t cue, uint32_t result);
void recomp_xact_setup_checkpoint(uint32_t stage, uint32_t manager,
                                  uint32_t cue_name, uint32_t value0,
                                  uint32_t value1);
void recomp_xact_play_checkpoint(uint32_t stage, uint32_t cue,
                                 uint32_t config, uint32_t result);
void recomp_xact_managed_checkpoint(uint32_t stage, uint32_t handle,
                                    uint32_t cue_name, uint32_t value);
void recomp_pbl_file_checkpoint(uint32_t stage, uint32_t object,
                                uint32_t value);
/* Xbox-rate timestamp counter used by lifted RDTSC instructions. */
uint64_t xbox_ReadTimeStampCounter(void);


/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
#define MEM64(addr)  (*(volatile uint64_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* Full 128-bit XMM helpers. Scalar SSE instructions use lane zero through
 * the xmmN aliases below; packed instructions use the xmmNv aliases. */
static __forceinline void recomp_xmm_load(float dst[4], uint32_t addr) {
    memcpy(dst, (const void *)XBOX_PTR(addr), 16);
}
static __forceinline void recomp_xmm_store(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), src, 16);
}
static __forceinline void recomp_xmm_copy(float dst[4], const float src[4]) {
    memcpy(dst, src, 16);
}
static __forceinline void recomp_xmm_zero(float dst[4]) {
    memset(dst, 0, 16);
}
static __forceinline void recomp_xmm_loadss(float dst[4], uint32_t addr) {
    memcpy(&dst[0], (const void *)XBOX_PTR(addr), 4);
    dst[1] = dst[2] = dst[3] = 0.0f;
}
static __forceinline void recomp_xmm_loadsd(float dst[4], uint32_t addr) {
    memcpy(&dst[0], (const void *)XBOX_PTR(addr), 8);
    dst[2] = dst[3] = 0.0f;
}
static __forceinline void recomp_xmm_copysd(float dst[4], const float src[4]) {
    memcpy(&dst[0], &src[0], 8);
}
static __forceinline void recomp_xmm_storesd(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), &src[0], 8);
}
static __forceinline void recomp_xmm_movlps_load(float dst[4], uint32_t addr) {
    memcpy(&dst[0], (const void *)XBOX_PTR(addr), 8);
}
static __forceinline void recomp_xmm_movlps_store(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), &src[0], 8);
}
static __forceinline void recomp_xmm_movhps_load(float dst[4], uint32_t addr) {
    memcpy(&dst[2], (const void *)XBOX_PTR(addr), 8);
}
static __forceinline void recomp_xmm_movhps_store(uint32_t addr, const float src[4]) {
    memcpy((void *)XBOX_PTR(addr), &src[2], 8);
}
static __forceinline void recomp_xmm_movlhps(float dst[4], const float src[4]) {
    float s0 = src[0], s1 = src[1]; dst[2] = s0; dst[3] = s1;
}
static __forceinline void recomp_xmm_movhlps(float dst[4], const float src[4]) {
    float s2 = src[2], s3 = src[3]; dst[0] = s2; dst[1] = s3;
}
static __forceinline void recomp_xmm_set_u32(float dst[4], uint32_t value) {
    memcpy(&dst[0], &value, 4); dst[1] = dst[2] = dst[3] = 0.0f;
}
static __forceinline uint32_t recomp_xmm_get_u32(const float src[4]) {
    uint32_t value; memcpy(&value, &src[0], 4); return value;
}
static __forceinline void recomp_xmm_movq_copy(float dst[4], const float src[4]) {
    memcpy(&dst[0], &src[0], 8); dst[2] = dst[3] = 0.0f;
}
static __forceinline void recomp_xmm_movq_load(float dst[4], uint32_t addr) {
    recomp_xmm_loadsd(dst, addr);
}
static __forceinline float recomp_xmm_min_sse(float a, float b) {
    return (isnan(a) || isnan(b) || a == b) ? b : (a < b ? a : b);
}
static __forceinline float recomp_xmm_max_sse(float a, float b) {
    return (isnan(a) || isnan(b) || a == b) ? b : (a > b ? a : b);
}
#define RECOMP_XMM_BINARY_RR(dst, src, op) do { \
    for (unsigned _xmm_i = 0; _xmm_i < 4; ++_xmm_i) \
        (dst)[_xmm_i] = (dst)[_xmm_i] op (src)[_xmm_i]; \
} while (0)
#define RECOMP_XMM_BINARY_RM(dst, addr, op) do { \
    float _xmm_src[4]; recomp_xmm_load(_xmm_src, (uint32_t)(addr)); \
    RECOMP_XMM_BINARY_RR((dst), _xmm_src, op); \
} while (0)
static __forceinline void recomp_xmm_minps(float dst[4], const float src[4]) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = recomp_xmm_min_sse(dst[i], src[i]);
}
static __forceinline void recomp_xmm_maxps(float dst[4], const float src[4]) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = recomp_xmm_max_sse(dst[i], src[i]);
}
static __forceinline void recomp_xmm_minps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_minps(dst, src);
}
static __forceinline void recomp_xmm_maxps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_maxps(dst, src);
}
static __forceinline uint32_t recomp_xmm_lane_bits(const float src[4], unsigned lane) {
    uint32_t value; memcpy(&value, &src[lane], 4); return value;
}
static __forceinline void recomp_xmm_set_lane_bits(float dst[4], unsigned lane,
                                                   uint32_t value) {
    memcpy(&dst[lane], &value, 4);
}
static __forceinline void recomp_xmm_bitwise(float dst[4], const float src[4],
                                             unsigned operation) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t a = recomp_xmm_lane_bits(dst, i);
        uint32_t b = recomp_xmm_lane_bits(src, i);
        uint32_t value = operation == 0u ? (a ^ b) :
                         operation == 1u ? (a & b) : (a | b);
        recomp_xmm_set_lane_bits(dst, i, value);
    }
}
static __forceinline void recomp_xmm_bitwise_mem(float dst[4], uint32_t addr,
                                                 unsigned operation) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_bitwise(dst, src, operation);
}
static __forceinline void recomp_xmm_shufps(float dst[4], const float src[4],
                                            uint8_t control) {
    float a[4], b[4], out[4];
    recomp_xmm_copy(a, dst); recomp_xmm_copy(b, src);
    out[0] = a[(control >> 0) & 3u]; out[1] = a[(control >> 2) & 3u];
    out[2] = b[(control >> 4) & 3u]; out[3] = b[(control >> 6) & 3u];
    recomp_xmm_copy(dst, out);
}
static __forceinline void recomp_xmm_shufps_mem(float dst[4], uint32_t addr,
                                                uint8_t control) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_shufps(dst, src, control);
}
static __forceinline void recomp_xmm_unpcklps(float dst[4], const float src[4]) {
    float a0 = dst[0], a1 = dst[1], b0 = src[0], b1 = src[1];
    dst[0] = a0; dst[1] = b0; dst[2] = a1; dst[3] = b1;
}
static __forceinline void recomp_xmm_unpckhps(float dst[4], const float src[4]) {
    float a2 = dst[2], a3 = dst[3], b2 = src[2], b3 = src[3];
    dst[0] = a2; dst[1] = b2; dst[2] = a3; dst[3] = b3;
}
static __forceinline void recomp_xmm_unpcklps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_unpcklps(dst, src);
}
static __forceinline void recomp_xmm_unpckhps_mem(float dst[4], uint32_t addr) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_unpckhps(dst, src);
}
static __forceinline void recomp_xmm_unary_ps(float dst[4], const float src[4],
                                              unsigned operation) {
    float input[4]; recomp_xmm_copy(input, src);
    for (unsigned i = 0; i < 4; ++i) {
        dst[i] = operation == 0u ? sqrtf(input[i]) :
                 operation == 1u ? 1.0f / sqrtf(input[i]) : 1.0f / input[i];
    }
}
static __forceinline void recomp_xmm_unary_ps_mem(float dst[4], uint32_t addr,
                                                  unsigned operation) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_unary_ps(dst, src, operation);
}
static __forceinline void recomp_xmm_cmp_ps(float dst[4], const float src[4],
                                            unsigned predicate) {
    float a[4], b[4]; recomp_xmm_copy(a, dst); recomp_xmm_copy(b, src);
    for (unsigned i = 0; i < 4; ++i) {
        int unordered = isnan(a[i]) || isnan(b[i]);
        int result = predicate == 0u ? (unordered || a[i] != b[i]) :
                     predicate == 1u ? (!unordered && a[i] == b[i]) :
                     predicate == 2u ? (!unordered && a[i] < b[i]) :
                                       (!unordered && a[i] <= b[i]);
        recomp_xmm_set_lane_bits(dst, i, result ? 0xFFFFFFFFu : 0u);
    }
}
static __forceinline void recomp_xmm_cmp_ps_mem(float dst[4], uint32_t addr,
                                                unsigned predicate) {
    float src[4]; recomp_xmm_load(src, addr); recomp_xmm_cmp_ps(dst, src, predicate);
}
static __forceinline uint32_t recomp_xmm_movmskps(const float src[4]) {
    uint32_t mask = 0;
    for (unsigned i = 0; i < 4; ++i)
        mask |= ((recomp_xmm_lane_bits(src, i) >> 31) & 1u) << i;
    return mask;
}
/* Keep ordinary RAM copies fast, but perform MMIO copies through volatile
 * scalar accesses so the fault bridge can emulate each hardware register. */
static __forceinline void XBOX_MEMCPY(uint32_t destination, uint32_t source,
                                      uint32_t length) {
    uint32_t offset = 0;
    if (destination < 0xFD000000u && source < 0xFD000000u) {
        memcpy((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source),
               length);
        return;
    }
    while (length - offset >= 4u &&
           (((destination + offset) | (source + offset)) & 3u) == 0u) {
        MEM32(destination + offset) = MEM32(source + offset);
        offset += 4u;
    }
    while (offset < length) {
        MEM8(destination + offset) = MEM8(source + offset);
        ++offset;
    }
}
/* REP MOVS copies one guest element at a time when forward overlap can
 * feed newly written data into later reads. memcpy is undefined for overlap;
 * Keep disjoint RAM copies on the bulk fast path. */
static __forceinline void XBOX_REP_MOVS(uint32_t destination, uint32_t source,
                                       uint32_t count, uint32_t width) {
    uint64_t length = (uint64_t)count * width;
    if (count == 0u) return;
    if ((uint64_t)destination + length <= 0xFD000000ull &&
        (uint64_t)source + length <= 0xFD000000ull) {
        if ((uint64_t)destination + length <= source ||
            (uint64_t)source + length <= destination) {
            memcpy((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source),
                   (size_t)length);
            return;
        }
        if (destination <= source) {
            memmove((void *)XBOX_PTR(destination), (const void *)XBOX_PTR(source),
                    (size_t)length);
            return;
        }
    }
    /* Volatile guest accesses preserve element width/order for overlap and
     * hardware registers, including unaligned word/dword instructions. */
    while (count-- != 0u) {
        if (width == 4u) MEM32(destination) = MEM32(source);
        else if (width == 2u) MEM16(destination) = MEM16(source);
        else MEM8(destination) = MEM8(source);
        destination += width;
        source += width;
    }
}
/** Apply the guest x87 rounding-control field for FIST/FISTP. */
static inline double x87_round_integral(double value) {
    double rounded;
    switch ((g_x87_control_word >> 10) & 3u) {
    case 1u: rounded = floor(value); break;
    case 2u: rounded = ceil(value); break;
    case 3u: rounded = trunc(value); break;
    default: rounded = nearbyint(value); break;
    }
    if (rounded != value)
        g_x87_status_word |= 0x20u;
    return rounded;
}
static inline int16_t X87_FIST16(double value) {
    double rounded = x87_round_integral(value);
    return (!isfinite(rounded) || rounded < -32768.0 || rounded >= 32768.0)
        ? INT16_MIN : (int16_t)rounded;
}
static inline int32_t X87_FIST32(double value) {
    double rounded = x87_round_integral(value);
    return (!isfinite(rounded) || rounded < -2147483648.0 || rounded >= 2147483648.0)
        ? INT32_MIN : (int32_t)rounded;
}
static inline int64_t X87_FIST64(double value) {
    double rounded = x87_round_integral(value);
    return (!isfinite(rounded) || rounded < -9223372036854775808.0 ||
            rounded >= 9223372036854775808.0)
        ? INT64_MIN : (int64_t)rounded;
}

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
#define CMP_L(a, b)   ((int32_t)(a) <  (int32_t)(b))     /* less (SF!=OF) */
#define CMP_GE(a, b)  ((int32_t)(a) >= (int32_t)(b))     /* greater or equal */
#define CMP_LE(a, b)  ((int32_t)(a) <= (int32_t)(b))     /* less or equal */
#define CMP_G(a, b)   ((int32_t)(a) >  (int32_t)(b))     /* greater */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
static inline int EVEN_PARITY8(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    return (int)((0x9669u >> (value & 0xFu)) & 1u);
}
#define TEST_S(a, b)  (sizeof(a) == 1 ? \
    ((((uint8_t)(a) & (uint8_t)(b)) & 0x80u) != 0) : \
    (sizeof(a) == 2 ? \
        ((((uint16_t)(a) & (uint16_t)(b)) & 0x8000u) != 0) : \
        ((((uint32_t)(a) & (uint32_t)(b)) & 0x80000000u) != 0))) /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return (val >> n) | (val << (32 - n));
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 a dummy return address before this macro.
 * If not found, pops the dummy return address to keep the stack balanced.
 *
 * The range check (0x00400000 to 0xFE000000) skips garbage VAs that
 * come from uninitialized vtable pointers. Adjust this range based
 * on your game's .text section boundaries. Kernel thunks at
 * 0xFE000000+ must NOT be blocked.
 *
 * CUSTOMIZE: Change the VA range check to match your game's code range.
 * Your .text section typically spans 0x00010000 to ~0x003XXXXX.
 * Any VA outside .text and below 0xFE000000 is likely garbage.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    const uint32_t _callee_saved_ebx = g_ebx; \
    const uint32_t _callee_saved_esi = g_esi; \
    const uint32_t _callee_saved_edi = g_edi; \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { \
        _fn(); \
        g_ebx = _callee_saved_ebx; \
        g_esi = _callee_saved_esi; \
        g_edi = _callee_saved_edi; \
    } \
    else { g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    const uint32_t _callee_saved_ebx = g_ebx; \
    const uint32_t _callee_saved_esi = g_esi; \
    const uint32_t _callee_saved_edi = g_edi; \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { \
        _fn(); \
        /* EBX, ESI, and EDI are callee-saved in the Xbox x86 ABI.  Keep the
         * caller contract even when a recovered/generated callee has an
         * incomplete epilogue or exits through a translated tail path. */ \
        g_ebx = _callee_saved_ebx; \
        g_esi = _callee_saved_esi; \
        g_edi = _callee_saved_edi; \
    } \
    else { g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    recomp_func_t _fn = recomp_lookup_manual((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup_kernel((uint32_t)(xbox_va)); \
    if (_fn) _fn(); \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define xmm0 g_xmm0[0]
#define xmm1 g_xmm1[0]
#define xmm2 g_xmm2[0]
#define xmm3 g_xmm3[0]
#define xmm4 g_xmm4[0]
#define xmm5 g_xmm5[0]
#define xmm6 g_xmm6[0]
#define xmm7 g_xmm7[0]
#define xmm0v g_xmm0
#define xmm1v g_xmm1
#define xmm2v g_xmm2
#define xmm3v g_xmm3
#define xmm4v g_xmm4
#define xmm5v g_xmm5
#define xmm6v g_xmm6
#define xmm7v g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
static __forceinline uint64_t recomp_mmx_pcmpgtw(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int16_t a = (int16_t)(uint16_t)(left >> (i * 16));
        int16_t b = (int16_t)(uint16_t)(right >> (i * 16));
        result |= (uint64_t)(a > b ? 0xFFFFu : 0u) << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpgtd(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t a = (int32_t)(uint32_t)(left >> (i * 32));
        int32_t b = (int32_t)(uint32_t)(right >> (i * 32));
        result |= (uint64_t)(a > b ? UINT32_MAX : 0u) << (i * 32);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpgtb(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        int8_t a = (int8_t)(uint8_t)(left >> (i * 8));
        int8_t b = (int8_t)(uint8_t)(right >> (i * 8));
        result |= (uint64_t)(a > b ? 0xFFu : 0u) << (i * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpeqb(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= (uint64_t)(((uint8_t)(left >> (i * 8)) ==
                              (uint8_t)(right >> (i * 8))) ? 0xFFu : 0u)
                  << (i * 8);
    return result;
}
static __forceinline uint64_t recomp_mmx_pcmpeqw(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= (uint64_t)(((uint16_t)(left >> (i * 16)) ==
                              (uint16_t)(right >> (i * 16))) ? 0xFFFFu : 0u)
                  << (i * 16);
    return result;
}
static __forceinline uint64_t recomp_mmx_paddb(uint64_t left,
                                                uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= (uint64_t)(uint8_t)((uint8_t)(left >> (i * 8)) +
                                      (uint8_t)(right >> (i * 8))) << (i * 8);
    return result;
}
static __forceinline uint64_t recomp_mmx_psubb(uint64_t left,
                                                uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= (uint64_t)(uint8_t)((uint8_t)(left >> (i * 8)) -
                                      (uint8_t)(right >> (i * 8))) << (i * 8);
    return result;
}
static __forceinline uint64_t recomp_mmx_psubd(uint64_t left,
                                                uint64_t right) {
    uint32_t lo = (uint32_t)left - (uint32_t)right;
    uint32_t hi = (uint32_t)(left >> 32) - (uint32_t)(right >> 32);
    return lo | ((uint64_t)hi << 32);
}
static __forceinline uint64_t recomp_mmx_pmullw(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = (uint16_t)((int16_t)(left >> (i * 16)) *
                                    (int16_t)(right >> (i * 16)));
        result |= (uint64_t)value << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pavgb(uint64_t left,
                                                uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned a = (uint8_t)(left >> (i * 8));
        unsigned b = (uint8_t)(right >> (i * 8));
        result |= (uint64_t)((a + b + 1u) >> 1) << (i * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_punpckldq(uint64_t left,
                                                    uint64_t right) {
    return (uint32_t)left | ((uint64_t)(uint32_t)right << 32);
}
static __forceinline uint64_t recomp_mmx_punpckhdq(uint64_t left,
                                                    uint64_t right) {
    return (uint32_t)(left >> 32) | ((uint64_t)(uint32_t)(right >> 32) << 32);
}
static __forceinline uint64_t recomp_mmx_paddw(uint64_t left,
                                               uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = (uint16_t)((uint16_t)(left >> (i * 16)) +
                                    (uint16_t)(right >> (i * 16)));
        result |= (uint64_t)value << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_psubw(uint64_t left,
                                               uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = (uint16_t)((uint16_t)(left >> (i * 16)) -
                                    (uint16_t)(right >> (i * 16)));
        result |= (uint64_t)value << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_paddd(uint64_t left,
                                               uint64_t right) {
    uint64_t lo = (uint32_t)left + (uint32_t)right;
    uint64_t hi = (uint32_t)(left >> 32) + (uint32_t)(right >> 32);
    return (uint32_t)lo | ((uint64_t)(uint32_t)hi << 32);
}
static __forceinline uint64_t recomp_mmx_pmaddwd(uint64_t left,
                                                 uint64_t right) {
    uint64_t result = 0;
    for (unsigned pair = 0; pair < 2; ++pair) {
        unsigned lane = pair * 2;
        int32_t a0 = (int16_t)(left >> (lane * 16));
        int32_t a1 = (int16_t)(left >> ((lane + 1) * 16));
        int32_t b0 = (int16_t)(right >> (lane * 16));
        int32_t b1 = (int16_t)(right >> ((lane + 1) * 16));
        uint32_t value = (uint32_t)(a0 * b0 + a1 * b1);
        result |= (uint64_t)value << (pair * 32);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_punpcklbw(uint64_t left,
                                                    uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result |= ((left >> (i * 8)) & 0xFFu) << (i * 16);
        result |= ((right >> (i * 8)) & 0xFFu) << (i * 16 + 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_punpckhbw(uint64_t left,
                                                    uint64_t right) {
    return recomp_mmx_punpcklbw(left >> 32, right >> 32);
}
static __forceinline uint64_t recomp_mmx_punpcklwd(uint64_t left,
                                                    uint64_t right) {
    return (left & 0xFFFFu) | ((right & 0xFFFFu) << 16) |
           (((left >> 16) & 0xFFFFu) << 32) |
           (((right >> 16) & 0xFFFFu) << 48);
}
static __forceinline uint64_t recomp_mmx_punpckhwd(uint64_t left,
                                                    uint64_t right) {
    return recomp_mmx_punpcklwd(left >> 32, right >> 32);
}
static __forceinline int32_t recomp_mmx_clamp_s16(int32_t value) {
    return value < -32768 ? -32768 : (value > 32767 ? 32767 : value);
}
static __forceinline int32_t recomp_mmx_clamp_s8(int32_t value) {
    return value < -128 ? -128 : (value > 127 ? 127 : value);
}
static __forceinline uint32_t recomp_mmx_clamp_u8(int32_t value) {
    return value < 0 ? 0u : (value > 255 ? 255u : (uint32_t)value);
}
static __forceinline uint64_t recomp_mmx_packssdw(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t value = (int32_t)(left >> (i * 32));
        result |= (uint64_t)(uint16_t)recomp_mmx_clamp_s16(value) << (i * 16);
        value = (int32_t)(right >> (i * 32));
        result |= (uint64_t)(uint16_t)recomp_mmx_clamp_s16(value) << ((i + 2) * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_packsswb(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int32_t value = (int16_t)(left >> (i * 16));
        result |= (uint64_t)(uint8_t)recomp_mmx_clamp_s8(value) << (i * 8);
        value = (int16_t)(right >> (i * 16));
        result |= (uint64_t)(uint8_t)recomp_mmx_clamp_s8(value) << ((i + 4) * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_packuswb(uint64_t left,
                                                  uint64_t right) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int32_t value = (int16_t)(left >> (i * 16));
        result |= (uint64_t)recomp_mmx_clamp_u8(value) << (i * 8);
        value = (int16_t)(right >> (i * 16));
        result |= (uint64_t)recomp_mmx_clamp_u8(value) << ((i + 4) * 8);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_psllw(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count >= 16) return 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= (uint64_t)(uint16_t)((uint16_t)(value >> (i * 16)) << count) << (i * 16);
    return result;
}
static __forceinline uint64_t recomp_mmx_psrlw(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count >= 16) return 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= (uint64_t)(uint16_t)((uint16_t)(value >> (i * 16)) >> count) << (i * 16);
    return result;
}
static __forceinline uint64_t recomp_mmx_psraw(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count > 15) count = 15;
    for (unsigned i = 0; i < 4; ++i) {
        int16_t lane = (int16_t)(value >> (i * 16));
        result |= (uint64_t)(uint16_t)(lane >> count) << (i * 16);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_pslld(uint64_t value, uint64_t count) {
    if (count >= 32) return 0;
    return (uint32_t)((uint32_t)value << count) |
           ((uint64_t)(uint32_t)((uint32_t)(value >> 32) << count) << 32);
}
static __forceinline uint64_t recomp_mmx_psrld(uint64_t value, uint64_t count) {
    if (count >= 32) return 0;
    return (uint32_t)((uint32_t)value >> count) |
           ((uint64_t)(uint32_t)((uint32_t)(value >> 32) >> count) << 32);
}
static __forceinline uint64_t recomp_mmx_psrad(uint64_t value, uint64_t count) {
    uint64_t result = 0;
    if (count > 31) count = 31;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t lane = (int32_t)(value >> (i * 32));
        result |= (uint64_t)(uint32_t)(lane >> count) << (i * 32);
    }
    return result;
}
static __forceinline uint64_t recomp_mmx_psllq(uint64_t value, uint64_t count) {
    return count >= 64 ? 0 : value << count;
}
static __forceinline uint64_t recomp_mmx_psrlq(uint64_t value, uint64_t count) {
    return count >= 64 ? 0 : value >> count;
}
static __forceinline uint64_t recomp_mmx_pshufw(uint64_t value,
                                                uint8_t control) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= ((value >> (((control >> (i * 2)) & 3u) * 16)) & 0xFFFFu) << (i * 16);
    return result;
}
static __forceinline void recomp_xmm_cvtpi2ps(float dst[4], uint64_t src) {
    dst[0] = (float)(int32_t)(uint32_t)src;
    dst[1] = (float)(int32_t)(uint32_t)(src >> 32);
}
static __forceinline uint32_t recomp_xmm_float_to_i32(float value,
                                                       unsigned truncate) {
    float rounded;
    if (!isfinite(value)) return 0x80000000u;
    rounded = truncate ? truncf(value) : nearbyintf(value);
    if (rounded < -2147483648.0f || rounded >= 2147483648.0f)
        return 0x80000000u;
    return (uint32_t)(int32_t)rounded;
}
static __forceinline uint64_t recomp_xmm_cvtps2pi(const float src[4],
                                                   unsigned truncate) {
    return recomp_xmm_float_to_i32(src[0], truncate) |
           ((uint64_t)recomp_xmm_float_to_i32(src[1], truncate) << 32);
}
#define RECOMP_RDTSC() do { \
    uint64_t _recomp_tsc = xbox_ReadTimeStampCounter(); \
    eax = (uint32_t)_recomp_tsc; \
    edx = (uint32_t)(_recomp_tsc >> 32); \
} while (0)

/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#endif /* RECOMP_TYPES_H */
