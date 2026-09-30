"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: shared double g_fp_stack[8] with g_fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import struct

from .disasm import Instruction, Operand
from .config import is_code_address, is_data_address, va_to_file_offset


# ── Operand formatting ──────────────────────────────────────

def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Segment registers → constants
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"0 /* seg:{name} */"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    # Segment registers → no-op
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"/* mov {name}, {value_expr} - segment register */;"

    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size):
    """Return the MEM macro name for a given operand size."""
    return {1: "MEM8", 2: "MEM16", 4: "MEM32", 8: "MEM64"}.get(size, "MEM32")


def _smem_accessor(size):
    """Return the signed MEM macro for a given operand size."""
    return {1: "SMEM8", 2: "SMEM16", 4: "SMEM32", 8: "SMEM64"}.get(size, "SMEM32")


def _fmt_mem(op):
    """Format a memory operand as a C expression (the address computation)."""
    parts = []
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    if op.mem_disp:
        if op.mem_disp < 0:
            # Negative displacement - but we stored unsigned, check sign
            if op.mem_disp > 0x80000000:
                # Actually negative (two's complement)
                signed_disp = op.mem_disp - 0x100000000
                if parts:
                    parts.append(f"- {_fmt_imm(-signed_disp)}")
                else:
                    parts.append(_fmt_imm(op.mem_disp))
            else:
                parts.append(_fmt_imm(op.mem_disp))
        else:
            parts.append(_fmt_imm(op.mem_disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


def _fmt_operand_read(op):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op)
    return "/* unknown operand */"

def _operand_width(op):
    """Return an integer operand's architectural width in bytes."""
    if op.type == "mem":
        return op.mem_size
    if op.type == "reg":
        if op.reg in ("al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"):
            return 1
        if op.reg in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            return 2
        return 4
    return None


def _make_sized_cmp(cmp_macro, lhs, rhs, width):
    """Format a comparison with x86's operand-width truncation/sign rules."""
    if width not in (1, 2):
        return f"{cmp_macro}({lhs}, {rhs})"
    operators = {
        "CMP_EQ": "==", "CMP_NE": "!=", "CMP_B": "<", "CMP_AE": ">=",
        "CMP_BE": "<=", "CMP_A": ">", "CMP_L": "<", "CMP_GE": ">=",
        "CMP_LE": "<=", "CMP_G": ">",
    }
    signed = cmp_macro in ("CMP_L", "CMP_GE", "CMP_LE", "CMP_G")
    c_type = f"int{width * 8}_t" if signed else f"uint{width * 8}_t"
    return f"(({c_type})({lhs}) {operators[cmp_macro]} ({c_type})({rhs}))"


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
    "jp":   (None,       None,      "parity"),
    "jnp":  (None,       None,      "not parity"),
    "jecxz": (None,      None,      "ecx is zero"),
    "jcxz":  (None,      None,      "cx is zero"),
}

# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
    "cmpxchg", "lock cmpxchg",  # Compare-and-exchange sets ZF
    "xadd",              # Exchange-and-add sets flags
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "mul", "div", "idiv",  # Flags partially undefined
    "rdtsc", "cpuid",      # Special instructions
    "lock xadd",           # Lock prefix - complex flag behavior
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    "pushfd", "popfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    "cvtpi2ps", "cvtps2pi", "cvttps2pi",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


def _make_condition(jcc, flag_setter, flag_ops):
    """
    Generate a C condition expression for a jcc based on what set the flags.
    Returns (cond_expr, description) or None.
    """
    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None
    cmp_macro, test_macro, desc = cond_info

    if len(flag_ops) >= 2:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
    elif len(flag_ops) == 1:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = None
    else:
        lhs = None
        rhs = None

    # SAHF loads SF/ZF/AF/PF/CF directly from AH.
    if flag_setter == "sahf":
        ah = "HI8(eax)"
        sahf_conditions = {
            "ja": f"(({ah} & 0x41u) == 0)",
            "jnbe": f"(({ah} & 0x41u) == 0)",
            "jae": f"(({ah} & 0x01u) == 0)",
            "jnb": f"(({ah} & 0x01u) == 0)",
            "jnc": f"(({ah} & 0x01u) == 0)",
            "jb": f"(({ah} & 0x01u) != 0)",
            "jnae": f"(({ah} & 0x01u) != 0)",
            "jc": f"(({ah} & 0x01u) != 0)",
            "jbe": f"(({ah} & 0x41u) != 0)",
            "jna": f"(({ah} & 0x41u) != 0)",
            "je": f"(({ah} & 0x40u) != 0)",
            "jz": f"(({ah} & 0x40u) != 0)",
            "jne": f"(({ah} & 0x40u) == 0)",
            "jnz": f"(({ah} & 0x40u) == 0)",
            "js": f"(({ah} & 0x80u) != 0)",
            "jns": f"(({ah} & 0x80u) == 0)",
            "jp": f"(({ah} & 0x04u) != 0)",
            "jnp": f"(({ah} & 0x04u) == 0)",
        }
        condition = sahf_conditions.get(jcc)
        return (condition, desc) if condition else None

    # ── FPU compare-to-EFLAGS: no standard operands ──
    if flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi",
                        "fucomip", "fcomi"):
        fpu_cmp_map = {
            "ja": ">", "jnbe": ">",
            "jae": ">=", "jnb": ">=", "jnc": ">=",
            "jb": "<", "jnae": "<", "jc": "<",
            "jbe": "<=", "jna": "<=",
            "je": "==", "jz": "==",
            "jne": "!=", "jnz": "!=",
        }
        op = fpu_cmp_map.get(jcc)
        if op:
            return f"(_fpu_cmp {op} 0) /* {flag_setter} */", desc
        if jcc == "jp":
            return "(_fpu_cmp == 2) /* fpu: unordered/NaN */", desc
        if jcc == "jnp":
            return "(_fpu_cmp != 2) /* fpu: ordered */", desc
        return None

    # If no operands available for other flag-setters, can't generate condition
    if lhs is None:
        return None

    # ── comiss/ucomiss: float comparison, sets CF/ZF/PF ──
    if flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd"):
        def _sse_op(op):
            if op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            return _fmt_operand_read(op)
        a = _sse_op(flag_ops[0]) if len(flag_ops) >= 1 else "0.0f"
        b = _sse_op(flag_ops[1]) if len(flag_ops) >= 2 else "0.0f"
        # COMISS/UCOMISS set ZF/PF/CF to 111 for unordered operands.
        # Preserve those unsigned x86 conditions instead of relying on the
        # host language's all-false NaN comparisons.
        unordered = f"(isnan((double)({a})) || isnan((double)({b})))"
        if jcc in ("ja", "jnbe"):
            return f"(!{unordered} && ({a} > {b}))", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"(!{unordered} && ({a} >= {b}))", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({unordered} || ({a} < {b}))", desc
        if jcc in ("jbe", "jna"):
            return f"({unordered} || ({a} <= {b}))", desc
        if jcc in ("je", "jz"):
            return f"({unordered} || ({a} == {b}))", desc
        if jcc in ("jne", "jnz"):
            return f"(!{unordered} && ({a} != {b}))", desc
        if jcc == "jp":
            return unordered, desc
        if jcc == "jnp":
            return f"!{unordered}", desc
        return None

    # ── cmp: flags from (a - b), operands unchanged ──
    if flag_setter == "cmp":
        if cmp_macro:
            return _make_sized_cmp(
                cmp_macro, lhs, rhs, _operand_width(flag_ops[0])
            ), desc
        if jcc == "js":
            width = _operand_width(flag_ops[0])
            if width in (1, 2):
                bits = width * 8
                return (f"((int{bits}_t)((uint{bits}_t)({lhs}) - "
                        f"(uint{bits}_t)({rhs})) < 0)"), desc
            return f"((int32_t)({lhs} - {rhs}) < 0)", desc
        if jcc == "jns":
            width = _operand_width(flag_ops[0])
            if width in (1, 2):
                bits = width * 8
                return (f"((int{bits}_t)((uint{bits}_t)({lhs}) - "
                        f"(uint{bits}_t)({rhs})) >= 0)"), desc
            return f"((int32_t)({lhs} - {rhs}) >= 0)", desc
        if jcc in ("jp", "jnp"):
            parity = f"EVEN_PARITY8((uint8_t)({lhs} - {rhs}))"
            return (parity if jcc == "jp" else f"!{parity}"), desc
        return None

    # ── test: flags from (a & b), operands unchanged ──
    if flag_setter == "test":
        if test_macro:
            return f"{test_macro}({lhs}, {rhs})", desc
        if cmp_macro:
            return _make_sized_cmp(
                cmp_macro, f"({lhs} & {rhs})", "0",
                _operand_width(flag_ops[0])
            ), desc
        if jcc == "js":
            width = _operand_width(flag_ops[0])
            if width in (1, 2):
                bits = width * 8
                return (f"((int{bits}_t)(uint{bits}_t)({lhs} & {rhs}) "
                        "< 0)"), desc
            return f"((int32_t)({lhs} & {rhs}) < 0)", desc
        if jcc == "jns":
            width = _operand_width(flag_ops[0])
            if width in (1, 2):
                bits = width * 8
                return (f"((int{bits}_t)(uint{bits}_t)({lhs} & {rhs}) "
                        ">= 0)"), desc
            return f"((int32_t)({lhs} & {rhs}) >= 0)", desc
        if jcc == "jo":
            return "0", desc  # OF=0 after test
        if jcc == "jno":
            return "1", desc
        if jcc in ("jp", "jnp"):
            parity = f"EVEN_PARITY8((uint8_t)({lhs} & {rhs}))"
            return (parity if jcc == "jp" else f"!{parity}"), desc
        return None

    # ── sub: a = a - b, flags from (a_orig - b) ──
    if flag_setter == "sub":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        # Ordered: reconstruct original a = result + b
        if cmp_macro and rhs:
            return f"{cmp_macro}((uint32_t){lhs} + (uint32_t){rhs}, (uint32_t){rhs})", desc
        if jcc in ("jb", "jnae"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── add: a = a + b, flags from result ──
    if flag_setter == "add":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({lhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── adc/sbb: result-based (like add/sub but with carry) ──
    if flag_setter in ("adc", "sbb"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── and/or/xor: result-based, CF=0, OF=0 ──
    if flag_setter in ("and", "or", "xor"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jns", "jge"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc == "jle":
            return f"((int32_t){lhs} <= 0)", desc
        if jcc == "jg":
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            return "0", desc  # CF=0 after and/or/xor
        if jcc in ("jbe", "jna"):
            # JBE is CF || ZF. Logical instructions clear CF, so this is
            # exactly the zero condition—not an always-false branch.
            return f"({lhs} == 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return "1", desc
        if jcc in ("ja", "jnbe"):
            # JA is !CF && !ZF. Logical instructions clear CF, so this is
            # exactly the non-zero condition.
            return f"({lhs} != 0)", desc
        return None

    # ── dec/inc: result-based, CF unchanged ──
    if flag_setter in ("dec", "inc"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jle", "jg", "jge"):
            cast = "(int32_t)" + lhs
            op = {"jl": "<", "jle": "<=", "jg": ">", "jge": ">="}[jcc]
            return f"({cast} {op} 0)", desc
        return None

    # ── neg: flags from (0 - a_orig), result is -a ──
    if flag_setter == "neg":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            # CF=1 unless original was 0
            return f"({lhs} != 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} == 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        return None

    # ── shift: result-based ──
    if flag_setter in ("shl", "shr", "sar"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── shld/shrd: double-precision shift, result-based ──
    if flag_setter in ("shld", "shrd"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── rol/ror/rcl/rcr: rotation, only CF/OF affected ──
    if flag_setter in ("rol", "ror", "rcl", "rcr"):
        # ZF/SF not modified by rotations - can't resolve most conditions
        return None

    # ── bsf/bsr: bit scan, ZF set if source is zero ──
    if flag_setter in ("bsf", "bsr"):
        if rhs is None:
            return None
        if jcc in ("je", "jz"):
            return f"({rhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({rhs} != 0)", desc
        return None

    # ── bt/bts/btr/btc: bit test, sets CF ──
    if flag_setter in ("bt", "bts", "btr", "btc"):
        if rhs is None:
            return None
        if jcc in ("jb", "jnae", "jc"):
            return f"(({lhs} >> ({rhs} & 31)) & 1)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"!(({lhs} >> ({rhs} & 31)) & 1)", desc
        return None

    # ── cmpxchg: compares accumulator with dest, sets ZF on match ──
    if flag_setter in ("cmpxchg", "lock cmpxchg"):
        if jcc in ("je", "jz"):
            return "(_flags != 0)", desc
        if jcc in ("jne", "jnz"):
            return "(_flags == 0)", desc
        return None

    # ── xadd: exchange and add, flags from addition ──
    if flag_setter == "xadd":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        return None

    # ── repe cmps* / repne scas*: string comparison ──
    if "cmps" in flag_setter or "scas" in flag_setter:
        if jcc in ("je", "jz"):
            return "(_flags != 0) /* REP string comparison ZF */", desc
        if jcc in ("jne", "jnz"):
            return "(_flags == 0) /* REP string comparison !ZF */", desc
        return None

    return None


def _make_sse_lahf_value(flag_setter, flag_ops):
    """Encode COMISS/UCOMISS ZF/PF/CF as the AH value produced by LAHF."""
    if (flag_setter not in ("comiss", "comisd", "ucomiss", "ucomisd")
            or len(flag_ops) < 2):
        return None

    def _operand(op):
        if op.type == "reg":
            return op.reg
        if op.type == "mem":
            accessor = "MEMD" if op.mem_size == 8 else "MEMF"
            return f"{accessor}({_fmt_mem(op)})"
        return _fmt_operand_read(op)

    a = _operand(flag_ops[0])
    b = _operand(flag_ops[1])
    return (
        f"((isnan((double)({a})) || isnan((double)({b}))) ? 0x45u : "
        f"(({a}) < ({b})) ? 0x01u : (({a}) == ({b})) ? 0x40u : 0u)"
    )


def _make_setcc_value(setcc_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a SETcc instruction."""
    cc = setcc_mnemonic[3:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


def _make_cmovcc_cond(cmov_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a CMOVcc instruction."""
    cc = cmov_mnemonic[4:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


_COMPLEMENT_CONDITION_CODES = {
    "e": "ne", "ne": "e",
    "z": "nz", "nz": "z",
    "b": "ae", "ae": "b",
    "nae": "nb", "nb": "nae",
    "be": "a", "a": "be",
    "na": "nbe", "nbe": "na",
    "l": "ge", "ge": "l",
    "le": "g", "g": "le",
    "s": "ns", "ns": "s",
}


def _condition_code(mnemonic):
    """Return the condition-code suffix shared by Jcc/SETcc/CMOVcc."""
    if mnemonic.startswith("cmov"):
        return mnemonic[4:]
    if mnemonic.startswith("set"):
        return mnemonic[3:]
    if mnemonic.startswith("j"):
        return mnemonic[1:]
    return None


def _snapshot_condition_test(consumer_mnemonic, snapshot_consumer):
    """Reuse a snapshotted boolean for the same or complementary condition."""
    consumer_cc = _condition_code(consumer_mnemonic)
    snapshot_cc = _condition_code(snapshot_consumer)
    if not consumer_cc or not snapshot_cc:
        return None
    if consumer_cc == snapshot_cc:
        return "_flags != 0"
    if _COMPLEMENT_CONDITION_CODES.get(snapshot_cc) == consumer_cc:
        return "_flags == 0"
    return None


# ── Pattern matching for flag-setter + jcc ────────────────────

def _emit_cond_goto(cond_expr, jcc, desc, target, lifter):
    """Emit a conditional goto or call for a jump target."""
    if target is None:
        return f"if ({cond_expr}) {{ /* {jcc}: {desc} - indirect */ }}"
    if lifter and lifter._is_external_target(target):
        name = lifter._call_target_name(target)
        return (f"if ({cond_expr}) {{ {name}(); return; }}"
                f" /* {jcc}: {desc} */")
    return f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    result = _make_condition(second.mnemonic, first.mnemonic, first.operands)
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+0x10]    8D 6C 24 10
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
_SEH_PROLOG_MARKERS = (b"\x64\xa1\x00\x00\x00\x00", b"\x8d\x6c\x24\x10")
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate __SEH_prolog / __SEH_epilog in the target binary.

    Returns (prolog_addr, epilog_addr); either may be None if not found, which
    is normal for a title whose CRT does not use them.
    """
    from .config import va_to_file_offset

    prolog = epilog = None

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (prolog is None and size <= _SEH_PROLOG_MAX_SIZE
                and all(m in body for m in _SEH_PROLOG_MARKERS)):
            prolog = addr
        elif (epilog is None and size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilog = addr

        if prolog is not None and epilog is not None:
            break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  SEH helpers: __SEH_prolog {fmt(prolog)}, "
              f"__SEH_epilog {fmt(epilog)}", file=sys.stderr)

    return prolog, epilog


class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self._fp_top = 0  # FPU stack top index
        self._direction_step = 1  # CLD by ABI at function entry
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prolog, found_epilog = detect_seh_helpers(self.func_db, xbe_data)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.SEH_PROLOG = seh_prolog
        self.SEH_EPILOG = seh_epilog

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        if addr in self.func_db:
            name = self.func_db[addr].get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)
        if m in ("cmpxchg", "lock cmpxchg"):
            return self._lift_cmpxchg(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, m)
        if m == "shr":
            return self._lift_shift(insn, ops, m)
        if m == "sar":
            return self._lift_shift(insn, ops, m)
        if m in ("rol", "ror", "rcl", "rcr"):
            return self._lift_rotate(insn, ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            return self._lift_rep_string(insn, m)
        # Capstone exposes the implicit ES:[EDI], [ESI] operands for the
        # one-byte MOVS opcodes.  In particular, x86 string MOVSD and SSE2
        # scalar MOVSD share a mnemonic, so distinguish them by the absence
        # of an XMM register rather than by operand count alone.
        string_ops = ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                      "lodsb", "lodsd", "lodsw")
        is_xmm_form = any(op.type == "reg" and op.reg.startswith("xmm")
                          for op in ops)
        if m in string_ops and not is_xmm_form:
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        if m == "int3":
            return ["__debugbreak(); /* int3 */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            self._direction_step = 1 if m == "cld" else -1
            return [f"/* {m} - direction flag */"]
        if m == "lahf":
            return ["/* lahf - load AH from flags (used in FPU compare idiom) */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "bt":
            if len(ops) >= 2:
                return [f"/* bt {_fmt_operand_read(ops[0])}, {_fmt_operand_read(ops[1])} - bit test */"]
            return [f"/* bt {insn.op_str} */"]
        if m == "emms":
            return ["/* emms - empty MMX state */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE (scalar float) ──
        if m in ("movss", "movsd", "movaps", "movups", "movlps", "movhps",
                 "movlhps", "movhlps",
                 "addss", "subss", "mulss", "divss", "sqrtss",
                 "addsd", "subsd", "mulsd", "divsd", "sqrtsd",
                 "minss", "maxss", "minsd", "maxsd",
                 "comiss", "comisd", "ucomiss", "ucomisd",
                 "cvtsi2ss", "cvtss2si", "cvttss2si",
                 "cvtsi2sd", "cvtsd2si", "cvttsd2si",
                 "cvtss2sd", "cvtsd2ss",
                 "xorps", "xorpd", "andps", "orps",
                 "movd", "movq", "movntq",
                 "shufps", "unpcklps", "unpckhps",
                 "addps", "subps", "mulps", "divps",
                 "minps", "maxps", "rsqrtss", "rcpss",
                 "sqrtps", "rsqrtps", "rcpps",
                 "cmpneqps", "cmpeqps", "cmpltps", "cmpleps",
                 "movmskps",
                 "pand", "pandn", "por", "pxor", "pcmpgtd",
                 "pcmpgtb", "pcmpgtw", "pcmpeqb", "pcmpeqw",
                 "paddb", "paddw", "paddd", "psubb", "psubw", "psubd",
                 "pmullw", "pmaddwd", "pavgb",
                 "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq",
                 "psraw", "psrad", "pshufw",
                 "punpcklbw", "punpcklwd", "punpckldq",
                 "punpckhbw", "punpckhwd", "punpckhdq",
                 "packsswb", "packssdw", "packuswb",
                 "cvtpi2ps", "cvtps2pi", "cvttps2pi"):
            return self._lift_sse(insn, m, ops)

        # -- CPU timing --
        if m == "rdtsc":
            return ["RECOMP_RDTSC();"]

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Unhandled ──
        return [f"/* TODO: {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(ops[1].mem_size)
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    def _lift_cmpxchg(self, insn, ops):
        """Lift CMPXCHG, including its accumulator update and ZF result.

        The original Xbox has one CPU, so guest instructions execute
        serially on the recomp CPU thread. We still preserve the complete
        architectural operation: compare AL/AX/EAX with the destination,
        write the source on success, or load the destination into the
        accumulator on failure. Materializing ZF before either write is
        important because a following JNE must not recompare the mutated EAX.
        """
        if len(ops) < 2:
            return ["/* cmpxchg: bad operands */"]
        width = _operand_width(ops[0]) or _operand_width(ops[1]) or 4
        if width not in (1, 2, 4):
            return [f"/* TODO: cmpxchg {insn.op_str} - unsupported width */"]
        c_type = f"uint{width * 8}_t"
        accumulator = {1: "LO8(eax)", 2: "LO16(eax)", 4: "eax"}[width]
        dest = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        write_dest = _fmt_operand_write(ops[0], f"({c_type})_cmpxchg_src")
        if width == 1:
            write_accumulator = "SET_LO8(eax, _cmpxchg_dest);"
        elif width == 2:
            write_accumulator = "SET_LO16(eax, _cmpxchg_dest);"
        else:
            write_accumulator = "eax = _cmpxchg_dest;"
        return [
            f"{{ {c_type} _cmpxchg_dest = ({c_type})({dest});",
            f"{c_type} _cmpxchg_acc = ({c_type})({accumulator});",
            f"{c_type} _cmpxchg_src = ({c_type})({src});",
            "_flags = (_cmpxchg_dest == _cmpxchg_acc);",
            f"if (_flags) {{ {write_dest} }}",
            f"else {{ {write_accumulator} }} }}",
        ]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            # Segment register pop → discard from stack
            if r in ("fs", "gs", "cs", "ds", "es", "ss"):
                return [f"{{ uint32_t _tmp; POP32(esp, _tmp); }} /* pop {r} - segment register */"]
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "0") + " /* xor self */"]
        if m in ("add", "sub") and getattr(self, "track_carry", False):
            operation = "+" if m == "add" else "-"
            carry = ("((uint64_t)_alu_dst + (uint64_t)_alu_src > 0xFFFFFFFFull)"
                     if m == "add" else "(_alu_dst < _alu_src)")
            return [
                f"{{ uint32_t _alu_dst = (uint32_t)({dst});",
                f"  uint32_t _alu_src = (uint32_t)({src});",
                f"  _cf = {carry};",
                _fmt_operand_write(ops[0], f"_alu_dst {operation} _alu_src") + " }",
            ]
        expr = f"{dst} {c_op} {src}"
        return [_fmt_operand_write(ops[0], expr)]

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        delta = "1"
        op_char = "+" if m == "inc" else "-"
        # For sub-registers (al, cl, etc.), use the SET macro instead of ++
        if ops[0].type == "reg" and ops[0].reg in (
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
            return [f"{val}{'++' if m == 'inc' else '--'};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op_char} {delta}")]

    def _lift_neg(self, insn, ops):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [
            f"{{ uint32_t _neg_value = (uint32_t)({val});",
            "  _cf = (_neg_value != 0);",
            _fmt_operand_write(ops[0], "(uint32_t)(-(int32_t)_neg_value)") + " }",
        ]

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_sbb(self, insn, ops):
        """SBB: subtract with borrow. Common idiom: sbb reg, reg → -CF (0 or -1)."""
        if len(ops) < 2:
            return ["/* sbb: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # sbb reg, reg is a common idiom: result is 0 or 0xFFFFFFFF depending on CF
        if ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "_cf ? 0xFFFFFFFF : 0") + " /* sbb self (CF extend) */"]
        return [
            f"{{ uint32_t _sbb_dst = (uint32_t)({dst});",
            f"  uint32_t _sbb_src = (uint32_t)({src});",
            "  uint32_t _sbb_carry = (uint32_t)_cf;",
            "  _cf = ((uint64_t)_sbb_src + _sbb_carry > _sbb_dst);",
            _fmt_operand_write(ops[0], "_sbb_dst - _sbb_src - _sbb_carry")
            + " } /* sbb */",
        ]

    def _lift_adc(self, insn, ops):
        """ADC: add with carry."""
        if len(ops) < 2:
            return ["/* adc: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _adc_dst = (uint32_t)({dst});",
            f"  uint32_t _adc_src = (uint32_t)({src});",
            "  uint32_t _adc_carry = (uint32_t)_cf;",
            "  uint64_t _adc_wide = (uint64_t)_adc_dst + _adc_src + _adc_carry;",
            "  _cf = (_adc_wide > 0xFFFFFFFFull);",
            _fmt_operand_write(ops[0], "(uint32_t)_adc_wide") + " } /* adc */",
        ]

    def _lift_shld(self, insn, ops):
        """SHLD: double-precision shift left."""
        if len(ops) < 3:
            return [f"/* shld: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} << {cnt}) | ({src} >> (32 - {cnt}))") + " /* shld */"]

    def _lift_shrd(self, insn, ops):
        """SHRD: double-precision shift right."""
        if len(ops) < 3:
            return [f"/* shrd: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} >> {cnt}) | ({src} << (32 - {cnt}))") + " /* shrd */"]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        return [f"/* {m}: unhandled */"]

    def _lift_shift(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        if not getattr(self, "track_carry", False):
            if m in ("shl", "sal"):
                return [_fmt_operand_write(ops[0], f"{dst} << {cnt}")]
            if m == "shr":
                return [_fmt_operand_write(ops[0], f"{dst} >> {cnt}")]
            return [_fmt_operand_write(
                ops[0], f"(uint32_t)((int32_t){dst} >> {cnt})")]

        width = _operand_width(ops[0])
        if width not in (1, 2, 4):
            return [f"/* {m}: unsupported width */"]
        bits = width * 8
        mask = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}[width]
        lines = [
            f"{{ uint32_t _shift_value = (uint32_t)({dst}) & {mask};",
            f"  uint32_t _shift_count = (uint32_t)({cnt}) & 0x1Fu;",
            "  while (_shift_count-- != 0u) {",
        ]
        if m in ("shl", "sal"):
            lines.extend([
                f"  uint32_t _shift_next_cf = "
                f"(_shift_value >> {bits - 1}u) & 1u;",
                f"  _shift_value = (_shift_value << 1u) & {mask};",
                "  _cf = (int)_shift_next_cf;",
            ])
        elif m == "shr":
            lines.extend([
                "  uint32_t _shift_next_cf = _shift_value & 1u;",
                "  _shift_value >>= 1u;",
                "  _cf = (int)_shift_next_cf;",
            ])
        else:
            lines.extend([
                "  uint32_t _shift_next_cf = _shift_value & 1u;",
                f"  const uint32_t _shift_sign = "
                f"_shift_value & (1u << {bits - 1}u);",
                "  _shift_value = (_shift_value >> 1u) | _shift_sign;",
                "  _cf = (int)_shift_next_cf;",
            ])
        lines.append("  }")
        lines.append(_fmt_operand_write(ops[0], "_shift_value") +
                     f" }} /* {m} */")
        return lines
    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        if m in ("rol", "ror"):
            func = "ROL32" if m == "rol" else "ROR32"
            return [_fmt_operand_write(ops[0], f"{func}({dst}, {cnt})")]

        width = _operand_width(ops[0])
        if width not in (1, 2, 4):
            return [f"/* {m}: unsupported width */"]
        bits = width * 8
        mask = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}[width]
        if m == "rcr":
            step = [
                "  uint32_t _rot_next_cf = _rot_value & 1u;",
                f"  _rot_value = (_rot_value >> 1u) | "
                f"(((uint32_t)_cf & 1u) << {bits - 1}u);",
                "  _cf = (int)_rot_next_cf;",
            ]
        else:
            step = [
                f"  uint32_t _rot_next_cf = "
                f"(_rot_value >> {bits - 1}u) & 1u;",
                f"  _rot_value = ((_rot_value << 1u) & {mask}) | "
                "((uint32_t)_cf & 1u);",
                "  _cf = (int)_rot_next_cf;",
            ]
        lines = [
            f"{{ uint32_t _rot_value = (uint32_t)({dst}) & {mask};",
            f"  uint32_t _rot_count = ((uint32_t)({cnt}) & 0x1Fu) % {bits + 1}u;",
            "  while (_rot_count-- != 0u) {",
        ]
        lines.extend(step)
        lines.append("  }")
        lines.append(_fmt_operand_write(ops[0], "_rot_value") + f" }} /* {m} */")
        return lines

    # ── Compare / Test (standalone) ──

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])

        return [f"(void)0; /* cmp {lhs}, {rhs} - flags set for next jcc */"]

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])

        return [f"(void)0; /* test {lhs}, {rhs} - flags set for next jcc */"]

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOG = None
    SEH_EPILOG = None

    def _lift_call(self, insn, ops):
        # x86 'call' pushes return address then jumps.
        # With global esp, we push a dummy return address (0) then call.
        # The callee's 'ret' will pop it back off.
        if insn.call_target:
            name = self._call_target_name(insn.call_target)
            lines = [f"PUSH32(esp, 0); {name}(); /* call 0x{insn.call_target:08X} */"]
            # After __SEH_prolog/__SEH_epilog, read back the frame pointer.
            if insn.call_target in (self.SEH_PROLOG, self.SEH_EPILOG):
                lines.append("ebp = g_seh_ebp; /* read back frame from SEH helper */")
            return lines
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            # Mark indirect calls for post-processing by _fixup_icall_esp_save
            return [f"PUSH32(esp, 0); RECOMP_ICALL_SAFE({target}, _icall_esp); /* indirect call */"]
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # x86 'ret' pops return address from stack.
        # 'ret N' also pops N extra bytes (stdcall cleanup).
        # If this function IS __SEH_prolog or __SEH_epilog, bridge ebp
        # so the caller can read back the frame pointer.
        prefix = ""
        if self.func_start in (self.SEH_PROLOG, self.SEH_EPILOG):
            prefix = "g_seh_ebp = ebp; "
        if len(ops) >= 1 and ops[0].type == "imm":
            n = ops[0].imm
            return [f"{prefix}esp += {4 + n}; return; /* ret {n} */"]
        return [f"{prefix}esp += 4; return; /* ret */"]

    def _is_external_target(self, addr):
        """Check if a jump target is outside the current function."""
        return not (self.func_start <= addr < self.func_end)

    def _read_jump_table(self, table_va, max_entries=256):
        """Read 32-bit jump table entries from the XBE at a given VA.
        Returns list of target addresses. Stops when an entry is not a
        valid code address or max_entries is reached."""
        if not self.xbe_data:
            return []
        offset = va_to_file_offset(table_va)
        if offset is None:
            return []
        targets = []
        for i in range(max_entries):
            o = offset + i * 4
            if o + 4 > len(self.xbe_data):
                break
            val = struct.unpack_from('<I', self.xbe_data, o)[0]
            if not is_code_address(val):
                break
            targets.append(val)
        return targets

    def _analyze_switch_table(self, ops):
        """Return verified local targets from an indexed indirect jump table.

        Native jump tables are not self-describing. Reading consecutive dwords
        can therefore continue into adjacent constants that merely resemble code
        pointers. Requiring every decoded dword to remain inside the current
        function rejects a valid local switch as soon as that happens (the retail
        Mercenaries D3D state compiler is one example). Local targets are still
        exact instruction addresses and are always safe to lower to local gotos;
        the generated fallback dispatch preserves any genuinely external target.
        Require at least two local table entries to avoid treating an ordinary
        indirect jump through a single function pointer as a switch.  The
        entries need not be distinct: optimized native switches commonly fold
        several cases onto one shared local epilogue.
        """
        if not ops or ops[0].type != "mem":
            return []
        op = ops[0]
        # Need a table base (displacement) and an index register.
        if not op.mem_disp or not (op.mem_index or op.mem_base):
            return []
        targets = self._read_jump_table(op.mem_disp)
        local_targets = [
            target for target in targets
            if self.func_start <= target < self.func_end
        ]
        if len(local_targets) >= 2:
            return local_targets
        return []

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            if self._is_external_target(insn.jump_target):
                # Tail call - no return address push (reuses current frame's)
                # Bridge ebp so the target function can inherit our frame pointer.
                name = self._call_target_name(insn.jump_target)
                return [f"g_seh_ebp = ebp; {name}(); return; /* tail jmp 0x{insn.jump_target:08X} */"]
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            # Detect intra-function switch tables (computed gotos)
            switch_targets = self._analyze_switch_table(ops)
            if switch_targets:
                target_expr = _fmt_operand_read(ops[0])
                unique_targets = sorted(set(switch_targets))
                lines = [f"{{ uint32_t _jt = {target_expr}; /* switch: {len(switch_targets)} entries, {len(unique_targets)} targets */"]
                for t in unique_targets:
                    lines.append(f"if (_jt == 0x{t:08X}u) goto loc_{t:08X};")
                lines.append(f"g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }}")
                return lines
            target = _fmt_operand_read(ops[0])
            return [f"g_seh_ebp = ebp; RECOMP_ITAIL({target}); return; /* indirect tail jmp */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (no flag-setter tracked)."""
        target = insn.jump_target
        jcc = insn.mnemonic

        # jecxz/jcxz: jump if ecx/cx is zero (not flag-based)
        if jcc in ("jecxz", "jcxz"):
            cond = "ecx == 0" if jcc == "jecxz" else "LO16(ecx) == 0"
            if target:
                if self._is_external_target(target):
                    name = self._call_target_name(target)
                    return [f"if ({cond}) {{ {name}(); return; }} /* {jcc} */"]
                return [f"if ({cond}) goto loc_{target:08X}; /* {jcc} */"]
            return [f"/* {jcc} - no target */"]

        cond_info = COND_MAP.get(jcc)
        desc = cond_info[2] if cond_info else jcc
        if target:
            if self._is_external_target(target):
                name = self._call_target_name(target)
                return [f"if (_flags /* {jcc}: {desc} */) {{ {name}(); return; }}"]
            return [f"if (_flags /* {jcc}: {desc} */) goto loc_{target:08X};"]
        return [f"/* {jcc}: {desc} - no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    def _lift_rep_string(self, insn, m):
        advance = "+=" if self._direction_step > 0 else "-="
        if "movsb" in m:
            if self._direction_step < 0:
                return [
                    "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); }",
                    "esi -= ecx; edi -= ecx; ecx = 0; /* rep movsb */",
                ]
            return ["XBOX_REP_MOVS(edi, esi, ecx, 1u);",
                    "esi += ecx; edi += ecx; ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            if self._direction_step < 0:
                return [
                    "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); }",
                    "esi -= ecx * 4; edi -= ecx * 4; ecx = 0; /* rep movsd */",
                ]
            return ["XBOX_REP_MOVS(edi, esi, ecx, 4u);",
                    "esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */"]
        if "movsw" in m:
            if self._direction_step < 0:
                return [
                    "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi - _i*2) = MEM16(esi - _i*2); }",
                    "esi -= ecx * 2; edi -= ecx * 2; ecx = 0; /* rep movsw */",
                ]
            return ["XBOX_REP_MOVS(edi, esi, ecx, 2u);",
                    "esi += ecx * 2; edi += ecx * 2; ecx = 0; /* rep movsw */"]
        if "stosb" in m:
            if self._direction_step < 0:
                return [
                    "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = LO8(eax); }",
                    "edi -= ecx; ecx = 0; /* rep stosb */",
                ]
            return ["memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx);",
                    "edi += ecx; ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            if self._direction_step < 0:
                return [
                    "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = eax; }",
                    "edi -= ecx * 4; ecx = 0; /* rep stosd */",
                ]
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }",
                "edi += ecx * 4; ecx = 0; /* rep stosd */"
            ]
        if "stosw" in m:
            if self._direction_step < 0:
                return [
                    "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi - _i*2) = LO16(eax); }",
                    "edi -= ecx * 2; ecx = 0; /* rep stosw */",
                ]
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi + _i*2) = LO16(eax); }",
                "edi += ecx * 2; ecx = 0; /* rep stosw */"
            ]
        if "cmpsb" in m or "cmpsw" in m or "cmpsd" in m:
            width = 1 if "cmpsb" in m else (2 if "cmpsw" in m else 4)
            mem = {1: "MEM8", 2: "MEM16", 4: "MEM32"}[width]
            stop = "_flags != 0" if m.startswith("repne ") else "_flags == 0"
            return [
                "while (ecx != 0u) {",
                f"    uint32_t _cmps_lhs = {mem}(esi);",
                f"    uint32_t _cmps_rhs = {mem}(edi);",
                "    _flags = (_cmps_lhs == _cmps_rhs);",
                f"    esi {advance} {width}u; edi {advance} {width}u; --ecx;",
                f"    if ({stop}) break;",
                f"}} /* {m} */",
            ]
        if "scasb" in m or "scasw" in m or "scasd" in m:
            width = 1 if "scasb" in m else (2 if "scasw" in m else 4)
            mem = {1: "MEM8", 2: "MEM16", 4: "MEM32"}[width]
            acc = {1: "LO8(eax)", 2: "LO16(eax)", 4: "eax"}[width]
            stop = "_flags != 0" if m.startswith("repne ") else "_flags == 0"
            return [
                "while (ecx != 0u) {",
                f"    uint32_t _scas_lhs = {acc};",
                f"    uint32_t _scas_rhs = {mem}(edi);",
                "    _flags = (_scas_lhs == _scas_rhs);",
                f"    edi {advance} {width}u; --ecx;",
                f"    if ({stop}) break;",
                f"}} /* {m} */",
            ]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi++; edi++; /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += 4; edi += 4; /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi++; /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += 4; /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi++; /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += 4; /* lodsd */"]
        if m == "movsw":
            return ["MEM16(edi) = MEM16(esi); esi += 2; edi += 2; /* movsw */"]
        if m == "stosw":
            return ["MEM16(edi) = LO16(eax); edi += 2; /* stosw */"]
        if m == "lodsw":
            return ["SET_LO16(eax, MEM16(esi)); esi += 2; /* lodsw */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──

    def _lift_sse(self, insn, m, ops):
        """Translate SSE instructions to C float operations."""
        nops = len(ops)
        if nops < 1:
            return [f"/* {m}: no operands */"]

        def _mmx_reg_name(op):
            if op.type == "reg" and op.reg.startswith("mm"):
                return f"g_{op.reg}"
            return None

        def _mmx_read(op):
            reg = _mmx_reg_name(op)
            if reg is not None:
                return reg
            if op.type == "mem":
                return f"MEM64({_fmt_mem(op)})"
            return _fmt_operand_read(op)

        def _mmx_write(op, val):
            reg = _mmx_reg_name(op)
            if reg is not None:
                return f"{reg} = (uint64_t)({val});"
            if op.type == "mem":
                return f"MEM64({_fmt_mem(op)}) = (uint64_t)({val});"
            return _fmt_operand_write(op, f"(uint32_t)({val})")

        # SSE register names map to shared architectural runtime state
        def _sse_read(op):
            if op.type == "reg":
                return op.reg  # xmm0, xmm1, etc.
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            elif op.type == "imm":
                return _fmt_imm(op.imm)
            return f"/* sse_read? */"

        def _sse_write(op, val):
            if op.type == "reg":
                return f"{op.reg} = {val};"
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)}) = {val};"
                return f"MEMF({_fmt_mem(op)}) = {val};"
            return f"/* sse_write? */;"

        def _sse_vec_reg(op):
            if op.type == "reg" and op.reg.startswith("xmm"):
                return f"{op.reg}v"
            return None

        def _sse_vec_move(dst, src, mnemonic):
            dst_reg = _sse_vec_reg(dst)
            src_reg = _sse_vec_reg(src)
            if dst_reg is not None and src_reg is not None:
                return f"recomp_xmm_copy({dst_reg}, {src_reg}); /* {mnemonic} */"
            if dst_reg is not None and src.type == "mem":
                return f"recomp_xmm_load({dst_reg}, {_fmt_mem(src)}); /* {mnemonic} */"
            if dst.type == "mem" and src_reg is not None:
                return f"recomp_xmm_store({_fmt_mem(dst)}, {src_reg}); /* {mnemonic} */"
            return f"/* {mnemonic} {insn.op_str}: unsupported packed move */"

        def _sse_packed_binary(dst, src, mnemonic, c_op):
            dst_reg = _sse_vec_reg(dst)
            src_reg = _sse_vec_reg(src)
            if dst_reg is None:
                return f"/* {mnemonic} {insn.op_str}: destination is not XMM */"
            if src_reg is not None:
                return f"RECOMP_XMM_BINARY_RR({dst_reg}, {src_reg}, {c_op}); /* {mnemonic} */"
            if src.type == "mem":
                return f"RECOMP_XMM_BINARY_RM({dst_reg}, {_fmt_mem(src)}, {c_op}); /* {mnemonic} */"
            return f"/* {mnemonic} {insn.op_str}: unsupported packed source */"

        # ── Moves ──
        # MMX MOVQ/MOVNTQ preserve the full 64-bit payload. XMM MOVQ also
        # preserves all architectural effects, including zeroing bits 127:64.
        if m in ("movq", "movntq") and nops >= 2:
            if _mmx_reg_name(ops[0]) is not None or _mmx_reg_name(ops[1]) is not None:
                return [_mmx_write(ops[0], _mmx_read(ops[1])) + f" /* {m} */"]
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            if m == "movq" and dst is not None and src is not None:
                return [f"recomp_xmm_movq_copy({dst}, {src}); /* movq */"]
            if m == "movq" and dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_movq_load({dst}, {_fmt_mem(ops[1])}); /* movq */"]
            if m == "movq" and ops[0].type == "mem" and src is not None:
                return [f"recomp_xmm_movlps_store({_fmt_mem(ops[0])}, {src}); /* movq */"]

        if m in ("movaps", "movups"):
            if nops >= 2:
                return [_sse_vec_move(ops[0], ops[1], m)]
            return [f"/* {m} {insn.op_str} */"]

        if m == "movss" and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_loadss({dst}, {_fmt_mem(ops[1])}); /* movss */"]
            if dst is not None and src is not None:
                return [f"{ops[0].reg} = {ops[1].reg}; /* movss */"]
            if ops[0].type == "mem" and src is not None:
                return [f"MEMF({_fmt_mem(ops[0])}) = {ops[1].reg}; /* movss */"]
            return [f"/* movss {insn.op_str}: unsupported operands */"]

        if m == "movsd" and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_loadsd({dst}, {_fmt_mem(ops[1])}); /* movsd */"]
            if dst is not None and src is not None:
                return [f"recomp_xmm_copysd({dst}, {src}); /* movsd */"]
            if ops[0].type == "mem" and src is not None:
                return [f"recomp_xmm_storesd({_fmt_mem(ops[0])}, {src}); /* movsd */"]
            return [f"/* movsd {insn.op_str}: unsupported operands */"]

        if m in ("movlps", "movhps") and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            high = m == "movhps"
            if dst is not None and ops[1].type == "mem":
                helper = "recomp_xmm_movhps_load" if high else "recomp_xmm_movlps_load"
                return [f"{helper}({dst}, {_fmt_mem(ops[1])}); /* {m} */"]
            if ops[0].type == "mem" and src is not None:
                helper = "recomp_xmm_movhps_store" if high else "recomp_xmm_movlps_store"
                return [f"{helper}({_fmt_mem(ops[0])}, {src}); /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]

        if m in ("movlhps", "movhlps") and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            if dst is not None and src is not None:
                return [f"recomp_xmm_{m}({dst}, {src}); /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]

        if m == "movd":
            if nops >= 2:
                dst = _sse_vec_reg(ops[0])
                src = _sse_vec_reg(ops[1])
                if dst is not None:
                    value = (f"recomp_xmm_get_u32({src})" if src is not None
                             else _fmt_operand_read(ops[1]))
                    return [f"recomp_xmm_set_u32({dst}, (uint32_t)({value})); /* movd */"]
                if src is not None:
                    return [_fmt_operand_write(ops[0], f"recomp_xmm_get_u32({src})")
                            + " /* movd */"]
                return [_fmt_operand_write(ops[0], _fmt_operand_read(ops[1]))
                        + " /* movd */"]
            return [f"/* movd {insn.op_str} */"]

        # ── Arithmetic ──
        if m in ("addss", "addsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} + {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("subss", "subsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} - {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("mulss", "mulsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} * {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("divss", "divsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} / {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("sqrtss", "sqrtsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})") + f" /* {m} */"]
        if m in ("minss", "minsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} < {b} ? {a} : {b})") + f" /* {m} */"]
        if m in ("maxss", "maxsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} > {b} ? {a} : {b})") + f" /* {m} */"]

        # ── Packed arithmetic ──
        if m in ("addps", "subps", "mulps", "divps"):
            if nops >= 2:
                c_op = {"addps": "+", "subps": "-", "mulps": "*", "divps": "/"}[m]
                return [_sse_packed_binary(ops[0], ops[1], m, c_op)]

        # ── Conversions ──
        if m == "cvtsi2ss":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(float)(int32_t){src}") + " /* cvtsi2ss */"]
        if m in ("cvtss2si", "cvttss2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtsi2sd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(double)(int32_t){src}") + " /* cvtsi2sd */"]
        if m in ("cvtsd2si", "cvttsd2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtss2sd":
            if nops >= 2:
                return [_sse_write(ops[0], f"(double){_sse_read(ops[1])}") + " /* cvtss2sd */"]
        if m == "cvtsd2ss":
            if nops >= 2:
                return [_sse_write(ops[0], f"(float){_sse_read(ops[1])}") + " /* cvtsd2ss */"]

        # ── Comparison ──
        if m in ("comiss", "comisd", "ucomiss", "ucomisd"):
            if nops >= 2:
                return [f"/* {m} {_sse_read(ops[0])}, {_sse_read(ops[1])} - sets EFLAGS */"]

        # ── Bitwise ──
        if m in ("xorps", "xorpd", "andps", "orps") and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            if (m in ("xorps", "xorpd") and dst is not None and
                    src is not None and ops[0].reg == ops[1].reg):
                return [f"recomp_xmm_zero({dst}); /* {m} self = zero */"]
            operation = 0 if m in ("xorps", "xorpd") else (1 if m == "andps" else 2)
            if dst is not None and src is not None:
                return [f"recomp_xmm_bitwise({dst}, {src}, {operation}u); /* {m} */"]
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_bitwise_mem({dst}, {_fmt_mem(ops[1])}, "
                        f"{operation}u); /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]

        # ── Packed min/max ──
        if m in ("minps", "maxps"):
            if nops >= 2:
                dst = _sse_vec_reg(ops[0])
                src = _sse_vec_reg(ops[1])
                if dst is not None and src is not None:
                    return [f"recomp_xmm_{m}({dst}, {src}); /* {m} */"]
                if dst is not None and ops[1].type == "mem":
                    return [f"recomp_xmm_{m}_mem({dst}, {_fmt_mem(ops[1])}); /* {m} */"]
                return [f"/* {m} {insn.op_str}: unsupported packed operands */"]

        # ── Reciprocal / rsqrt ──
        if m == "rsqrtss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})") + " /* rsqrtss */"]
        if m == "rcpss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}") + " /* rcpss */"]

        # ── Packed sqrt / reciprocal / rsqrt ──
        if m in ("sqrtps", "rsqrtps", "rcpps") and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            operation = {"sqrtps": 0, "rsqrtps": 1, "rcpps": 2}[m]
            if dst is not None and src is not None:
                return [f"recomp_xmm_unary_ps({dst}, {src}, {operation}u); /* {m} */"]
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_unary_ps_mem({dst}, {_fmt_mem(ops[1])}, "
                        f"{operation}u); /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]

        # ── Packed comparison ──
        if m in ("cmpneqps", "cmpeqps", "cmpltps", "cmpleps") and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            predicate = {"cmpneqps": 0, "cmpeqps": 1,
                         "cmpltps": 2, "cmpleps": 3}[m]
            if dst is not None and src is not None:
                return [f"recomp_xmm_cmp_ps({dst}, {src}, {predicate}u); /* {m} */"]
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_cmp_ps_mem({dst}, {_fmt_mem(ops[1])}, "
                        f"{predicate}u); /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]

        # ── Move mask ──
        if m == "movmskps" and nops >= 2:
            src = _sse_vec_reg(ops[1])
            if src is not None:
                return [_fmt_operand_write(ops[0], f"recomp_xmm_movmskps({src})")
                        + " /* movmskps */"]
            return [f"/* movmskps {insn.op_str}: source is not XMM */"]

        # ── MMX / integer SIMD ──
        if m in ("pand", "pandn", "por", "pxor", "pcmpgtd",
                 "pcmpgtb", "pcmpgtw", "pcmpeqb", "pcmpeqw",
                 "paddb", "paddw", "paddd", "psubb", "psubw", "psubd",
                 "pmullw", "pmaddwd", "pavgb"):
            if nops >= 2:
                dst = _mmx_reg_name(ops[0])
                if dst is not None:
                    src = _mmx_read(ops[1])
                    if m == "pand":
                        value = f"{dst} & {src}"
                    elif m == "pandn":
                        value = f"~{dst} & {src}"
                    elif m == "por":
                        value = f"{dst} | {src}"
                    elif m == "pxor":
                        value = f"{dst} ^ {src}"
                    else:
                        helper = {
                            "pcmpgtd": "recomp_mmx_pcmpgtd",
                            "pcmpgtb": "recomp_mmx_pcmpgtb",
                            "pcmpgtw": "recomp_mmx_pcmpgtw",
                            "pcmpeqb": "recomp_mmx_pcmpeqb",
                            "pcmpeqw": "recomp_mmx_pcmpeqw",
                            "paddb": "recomp_mmx_paddb",
                            "paddw": "recomp_mmx_paddw",
                            "paddd": "recomp_mmx_paddd",
                            "psubb": "recomp_mmx_psubb",
                            "psubw": "recomp_mmx_psubw",
                            "psubd": "recomp_mmx_psubd",
                            "pmullw": "recomp_mmx_pmullw",
                            "pmaddwd": "recomp_mmx_pmaddwd",
                            "pavgb": "recomp_mmx_pavgb",
                        }[m]
                        value = f"{helper}({dst}, {src})"
                    return [_mmx_write(ops[0], value) + f" /* {m} */"]
                return [f"/* {m} {insn.op_str}: destination is not MMX */"]
        if m in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq",
                 "psraw", "psrad") and nops >= 2:
            dst = _mmx_reg_name(ops[0])
            if dst is not None:
                count = (_fmt_imm(ops[1].imm) if ops[1].type == "imm"
                         else _mmx_read(ops[1]))
                return [_mmx_write(
                    ops[0], f"recomp_mmx_{m}({dst}, (uint64_t)({count}))"
                ) + f" /* {m} */"]
            return [f"/* {m} {insn.op_str}: destination is not MMX */"]
        if m in ("punpcklbw", "punpcklwd", "punpckldq",
                 "punpckhbw", "punpckhwd", "punpckhdq",
                 "packsswb", "packssdw", "packuswb") and nops >= 2:
            dst = _mmx_reg_name(ops[0])
            if dst is not None:
                return [_mmx_write(
                    ops[0], f"recomp_mmx_{m}({dst}, {_mmx_read(ops[1])})"
                ) + f" /* {m} */"]
            return [f"/* {m} {insn.op_str}: destination is not MMX */"]
        if m == "pshufw" and nops >= 3:
            dst = _mmx_reg_name(ops[0])
            if dst is not None:
                return [_mmx_write(
                    ops[0], f"recomp_mmx_pshufw({_mmx_read(ops[1])}, "
                            f"(uint8_t){_fmt_imm(ops[2].imm)})"
                ) + " /* pshufw */"]
            return [f"/* pshufw {insn.op_str}: destination is not MMX */"]
        if m == "cvtpi2ps" and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            if dst is not None:
                return [f"recomp_xmm_cvtpi2ps({dst}, {_mmx_read(ops[1])}); "
                        "/* cvtpi2ps */"]
            return [f"/* cvtpi2ps {insn.op_str}: destination is not XMM */"]
        if m in ("cvtps2pi", "cvttps2pi") and nops >= 2:
            dst = _mmx_reg_name(ops[0])
            src = _sse_vec_reg(ops[1])
            if dst is not None and src is not None:
                truncate = "1u" if m == "cvttps2pi" else "0u"
                return [_mmx_write(
                    ops[0], f"recomp_xmm_cvtps2pi({src}, {truncate})"
                ) + f" /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]
        # ── Shuffle/unpack ──
        if m == "shufps" and nops >= 3:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            control = _fmt_imm(ops[2].imm)
            if dst is not None and src is not None:
                return [f"recomp_xmm_shufps({dst}, {src}, (uint8_t){control}); /* shufps */"]
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_shufps_mem({dst}, {_fmt_mem(ops[1])}, "
                        f"(uint8_t){control}); /* shufps */"]
            return [f"/* shufps {insn.op_str}: unsupported operands */"]
        if m in ("unpcklps", "unpckhps") and nops >= 2:
            dst = _sse_vec_reg(ops[0])
            src = _sse_vec_reg(ops[1])
            if dst is not None and src is not None:
                return [f"recomp_xmm_{m}({dst}, {src}); /* {m} */"]
            if dst is not None and ops[1].type == "mem":
                return [f"recomp_xmm_{m}_mem({dst}, {_fmt_mem(ops[1])}); /* {m} */"]
            return [f"/* {m} {insn.op_str}: unsupported operands */"]

        return [f"/* SSE: {m} {insn.op_str} */"]

    # ── FPU (x87) ──

    def _lift_fpu(self, insn, m, ops):
        """Basic FPU instruction translation using shared x87 state."""
        # FPU is complex. We translate common patterns to double operations.
        # Full accuracy would require an x87 stack emulator.

        def x87_reg_index(op):
            if op.type != "reg" or not op.reg:
                return None
            reg = op.reg.replace(" ", "").lower()
            if reg.startswith("st(") and reg.endswith(")"):
                reg = reg[3:-1]
            elif reg.startswith("st"):
                reg = reg[2:]
            try:
                index = int(reg)
            except ValueError:
                return None
            return index if 0 <= index <= 7 else None

        if m == "fld":
            if len(ops) >= 1:
                if ops[0].type == "mem":
                    if ops[0].mem_size == 4:
                        return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                    elif ops[0].mem_size == 8:
                        return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld */"]
                reg_index = x87_reg_index(ops[0])
                if reg_index is not None:
                    # Read before the push changes TOP. This is particularly
                    # important for FLD ST(0), which duplicates the old top.
                    return [f"{{ double _fpu_t = fp_st({reg_index}); fp_push(_fpu_t); }}"
                            f" /* fld {insn.op_str} */"]
            return [f"/* fld {insn.op_str} */"]

        if m in ("fst", "fstp"):
            pop_code = " fp_pop();" if m == "fstp" else ""
            if len(ops) >= 1 and ops[0].type == "mem":
                if ops[0].mem_size == 4:
                    return [f"MEMF({_fmt_mem(ops[0])}) = (float)fp_top();{pop_code} /* {m} */"]
                elif ops[0].mem_size == 8:
                    return [f"MEMD({_fmt_mem(ops[0])}) = fp_top();{pop_code} /* {m} */"]
            if len(ops) >= 1 and ops[0].type == "reg":
                reg_index = x87_reg_index(ops[0])
                if reg_index is not None:
                    if reg_index == 0:
                        return [f"{pop_code.strip()} /* {m} {insn.op_str} */" if pop_code
                                else f"/* {m} {insn.op_str} */"]
                    return [f"fp_st({reg_index}) = fp_top();{pop_code}"
                            f" /* {m} {insn.op_str} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fild":
            if len(ops) >= 1 and ops[0].type == "mem":
                smem = _smem_accessor(ops[0].mem_size)
                return [f"fp_push((double){smem}({_fmt_mem(ops[0])})); /* fild */"]
            return [f"/* fild {insn.op_str} */"]

        if m in ("fist", "fistp"):
            pop_code = " fp_pop();" if m == "fistp" else ""
            if len(ops) >= 1 and ops[0].type == "mem":
                mem_acc = _mem_accessor(ops[0].mem_size)
                convert = {2: "X87_FIST16", 4: "X87_FIST32", 8: "X87_FIST64"}.get(
                    ops[0].mem_size, "X87_FIST32")
                return [f"{mem_acc}({_fmt_mem(ops[0])}) = {convert}(fp_top());"
                        f"{pop_code} /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if (m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr")
                and len(ops) >= 1 and ops[0].type == "mem"):
            imem = f"(double){_smem_accessor(ops[0].mem_size)}({_fmt_mem(ops[0])})"
            if m == "fiadd":
                return [f"fp_top() += {imem}; /* fiadd memory */"]
            if m == "fisub":
                return [f"fp_top() -= {imem}; /* fisub memory */"]
            if m == "fisubr":
                return [f"fp_top() = {imem} - fp_top(); /* fisubr memory */"]
            if m == "fimul":
                return [f"fp_top() *= {imem}; /* fimul memory */"]
            if m == "fidiv":
                return [f"fp_top() /= {imem}; /* fidiv memory */"]
            return [f"fp_top() = {imem} / fp_top(); /* fidivr memory */"]

        if (m in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr")
                and len(ops) >= 1 and ops[0].type == "mem"):
            mem = (f"MEMD({_fmt_mem(ops[0])})" if ops[0].mem_size == 8
                   else f"MEMF({_fmt_mem(ops[0])})")
            if m == "fadd":
                return [f"fp_top() += {mem}; /* fadd memory */"]
            if m == "fsub":
                return [f"fp_top() -= {mem}; /* fsub memory */"]
            if m == "fsubr":
                return [f"fp_top() = {mem} - fp_top(); /* fsubr memory */"]
            if m == "fmul":
                return [f"fp_top() *= {mem}; /* fmul memory */"]
            if m == "fdiv":
                return [f"fp_top() /= {mem}; /* fdiv memory */"]
            return [f"fp_top() = {mem} / fp_top(); /* fdivr memory */"]

        register_arithmetic = {
            "fadd": "+",
            "fsub": "-",
            "fsubr": "r-",
            "fmul": "*",
            "fdiv": "/",
            "fdivr": "r/",
        }
        if m in register_arithmetic:
            # D8 C0+i forms have one explicit source and an implicit ST(0)
            # destination. DC C0+i forms expose both destination and source.
            # Neither form pops the x87 stack; only the P-suffixed opcodes do.
            if len(ops) >= 2:
                dst = x87_reg_index(ops[0])
                src = x87_reg_index(ops[1])
            else:
                dst = 0
                src = x87_reg_index(ops[0]) if ops else 1
            if dst is None:
                dst = 0
            if src is None:
                src = 1
            lhs = "fp_top()" if dst == 0 else f"fp_st({dst})"
            rhs = "fp_top()" if src == 0 else f"fp_st({src})"
            op = register_arithmetic[m]
            if op == "r-":
                return [f"{lhs} = {rhs} - {lhs}; /* {m} */"]
            if op == "r/":
                return [f"{lhs} = {rhs} / {lhs}; /* {m} */"]
            return [f"{lhs} {op}= {rhs}; /* {m} */"]

        popping_arithmetic = {
            "faddp": "+",
            "fsubp": "-",
            "fsubrp": "r-",
            "fmulp": "*",
            "fdivp": "/",
            "fdivrp": "r/",
        }
        if m in popping_arithmetic:
            dst = x87_reg_index(ops[0]) if ops else 1
            if dst is None:
                dst = 1
            lhs = f"fp_st({dst})"
            op = popping_arithmetic[m]
            if op == "r-":
                expression = f"{lhs} = fp_top() - {lhs};"
            elif op == "r/":
                expression = f"{lhs} = fp_top() / {lhs};"
            else:
                expression = f"{lhs} {op}= fp_top();"
            return [f"{expression} fp_pop(); /* {m} */"]
        if m == "fchs":
            return [f"fp_top() = -fp_top(); /* fchs */"]
        if m == "fabs":
            return [f"fp_top() = fabs(fp_top()); /* fabs */"]
        if m == "fsqrt":
            return [f"fp_top() = sqrt(fp_top()); /* fsqrt */"]
        if m == "fsin":
            return ["fp_top() = sin(fp_top()); /* fsin */"]
        if m == "fcos":
            return ["fp_top() = cos(fp_top()); /* fcos */"]
        if m == "fsincos":
            # FSINCOS replaces the original ST(0) with sin(x), then pushes
            # cos(x). The resulting architectural order is ST(0) = cos(x),
            # ST(1) = sin(x). Preserve x before changing the shared stack.
            return ["{ double _fpu_x = fp_top(); fp_top() = sin(_fpu_x); "
                    "fp_push(cos(_fpu_x)); } /* fsincos */"]
        if m == "fptan":
            # FPTAN replaces ST(0) with tan(x), then pushes 1.0. Callers
            # conventionally discard that new ST(0) with FSTP ST(0).
            return ["{ double _fpu_x = fp_top(); fp_top() = tan(_fpu_x); "
                    "fp_push(1.0); } /* fptan */"]
        if m == "fpatan":
            # ST(1) = atan2(ST(1), ST(0)), followed by one stack pop.
            return ["fp_st1() = atan2(fp_st1(), fp_top()); fp_pop(); /* fpatan */"]
        if m == "fyl2x":
            # ST(1) = ST(1) * log2(ST(0)), followed by one stack pop.
            # DirectSound uses this to convert a source sample-rate ratio into
            # the MCPX 4.12 logarithmic pitch value.
            return ["fp_st1() *= log2(fp_top()); fp_pop(); /* fyl2x */"]
        if m == "f2xm1":
            return ["fp_top() = exp2(fp_top()) - 1.0; /* f2xm1 */"]
        if m == "frndint":
            return ["fp_top() = x87_round_integral(fp_top()); /* frndint */"]
        if m == "fscale":
            return ["fp_top() *= exp2(trunc(fp_st1())); /* fscale */"]
        if m == "fxch":
            reg_index = x87_reg_index(ops[-1]) if len(ops) >= 1 else 1
            if reg_index is None:
                reg_index = 1
            if reg_index == 0:
                return [f"/* fxch {insn.op_str} (self) */"]
            return [f"{{ double _t = fp_top(); fp_top() = fp_st({reg_index}); "
                    f"fp_st({reg_index}) = _t; }} /* fxch {insn.op_str} */"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            # x87 reports C3/C2/C0 = 111 for unordered, 001 for less,
            # 100 for equal, and 000 for greater.  FNSTSW consumers depend on
            # those bits, and the P-suffixed forms must pop architecturally.
            pop_count = 2 if m.endswith("pp") else (1 if m.endswith("p") else 0)
            pop_code = " fp_pop();" * pop_count
            rhs = "fp_st1()"
            if len(ops) >= 1 and ops[0].type == "mem":
                rhs = (f"MEMD({_fmt_mem(ops[0])})" if ops[0].mem_size == 8
                       else f"MEMF({_fmt_mem(ops[0])})")
            elif len(ops) >= 1 and ops[-1].type == "reg":
                reg_index = x87_reg_index(ops[-1])
                if reg_index is not None:
                    rhs = "fp_top()" if reg_index == 0 else f"fp_st({reg_index})"
            return [
                f"{{ double _fpu_a = fp_top(); double _fpu_b = {rhs}; "
                "_fpu_cmp = (isnan(_fpu_a) || isnan(_fpu_b)) ? 2 : "
                "(_fpu_a < _fpu_b) ? -1 : (_fpu_a > _fpu_b) ? 1 : 0; "
                "g_x87_status_word = (uint16_t)((g_x87_status_word & ~0x4500u) | "
                "((_fpu_cmp == 2) ? 0x4500u : (_fpu_cmp < 0) ? 0x0100u : "
                "(_fpu_cmp == 0) ? 0x4000u : 0u));"
                f"{pop_code} }} /* {m} {insn.op_str} */"
            ]
        if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
            # These set EFLAGS directly (CF, ZF, PF) from FPU comparison.
            pops = m.endswith("pi") or m.endswith("ip")
            pop_code = " fp_pop();" if pops else ""
            reg_index = x87_reg_index(ops[-1]) if ops else 1
            if reg_index is None:
                reg_index = 1
            rhs = "fp_top()" if reg_index == 0 else f"fp_st({reg_index})"
            return [
                f"{{ double _fpu_a = fp_top(); double _fpu_b = {rhs}; "
                "_fpu_cmp = (isnan(_fpu_a) || isnan(_fpu_b)) ? 2 : "
                "(_fpu_a < _fpu_b) ? -1 : (_fpu_a > _fpu_b) ? 1 : 0;"
                f"{pop_code} }} /* {m} */"
            ]
        if m == "fnstsw":
            if len(ops) >= 1:
                return [_fmt_operand_write(ops[0], "g_x87_status_word")
                        + f" /* fnstsw {insn.op_str} */"]
            return [f"/* fnstsw {insn.op_str} - missing destination */"]
        if m == "fnstcw":
            if len(ops) >= 1:
                return [_fmt_operand_write(ops[0], "g_x87_control_word")
                        + f" /* fnstcw {insn.op_str} */"]
            return [f"/* fnstcw {insn.op_str} - missing destination */"]
        if m == "fldcw":
            if len(ops) >= 1:
                return [f"g_x87_control_word = (uint16_t){_fmt_operand_read(ops[0])};"
                        f" /* fldcw {insn.op_str} */"]
            return [f"/* fldcw {insn.op_str} - missing source */"]
        if m == "fnclex":
            return ["g_x87_status_word &= 0x7F00u; /* fnclex */"]
        if m == "fprem":
            return ["fp_top() = fmod(fp_top(), fp_st1()); "
                    "g_x87_status_word &= (uint16_t)~0x0400u; /* fprem complete */"]
        if m == "fldz":
            return [f"fp_push(0.0); /* fldz */"]
        if m == "fld1":
            return [f"fp_push(1.0); /* fld1 */"]
        if m == "fldpi":
            return ["fp_push(3.141592653589793238462643383279502884); /* fldpi */"]
        if m == "fldln2":
            return ["fp_push(0.693147180559945309417232121458176568); /* fldln2 */"]
        if m == "fldlg2":
            return ["fp_push(0.301029995663981195213738894724493027); /* fldlg2 */"]
        if m == "fldl2e":
            return ["fp_push(1.44269504088896340735992468100189214); /* fldl2e */"]

        return [f"/* FPU: {m} {insn.op_str} */"]


def lift_basic_block(lifter, bb, flag_state=None):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from
                    a preceding block, or None

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    # Track the last instruction that set flags
    if flag_state:
        last_flag_setter, last_flag_ops = flag_state
    else:
        last_flag_setter = None
        last_flag_ops = []

    # A basic-block boundary does not change EFLAGS. If the next block starts
    # with flag-neutral register restores before consuming inherited flags,
    # reconstructing the condition at the Jcc would read the restored values
    # rather than the operands used by the original CMP/TEST. Snapshot the
    # inherited condition at block entry, while those operands are still live.
    if (last_flag_setter and
            last_flag_setter not in ("snapshot_condition", "snapshot_sse_lahf")):
        register_family = {
            "al": "eax", "ah": "eax", "ax": "eax", "eax": "eax",
            "bl": "ebx", "bh": "ebx", "bx": "ebx", "ebx": "ebx",
            "cl": "ecx", "ch": "ecx", "cx": "ecx", "ecx": "ecx",
            "dl": "edx", "dh": "edx", "dx": "edx", "edx": "edx",
            "si": "esi", "esi": "esi", "di": "edi", "edi": "edi",
            "sp": "esp", "esp": "esp", "bp": "ebp", "ebp": "ebp",
        }
        compared_regs = set()
        memory_address_regs = set()
        producer_reads_memory = False
        for op in last_flag_ops:
            if getattr(op, "type", None) == "reg" and getattr(op, "reg", None):
                compared_regs.add(register_family.get(op.reg, op.reg))
            elif getattr(op, "type", None) == "mem":
                producer_reads_memory = True
                if getattr(op, "mem_base", None):
                    memory_address_regs.add(register_family.get(
                        op.mem_base, op.mem_base))
                if getattr(op, "mem_index", None):
                    memory_address_regs.add(register_family.get(
                        op.mem_index, op.mem_index))

        consumer = None
        dependency_overwritten = False
        prefix_length = 0
        for middle in insns:
            if (middle.is_cond_jump or
                    middle.mnemonic.startswith(("set", "cmov")) or
                    middle.mnemonic == "lahf"):
                consumer = middle
                break
            if middle.mnemonic not in _EFLAGS_PRESERVE:
                break
            prefix_length += 1
            if middle.mnemonic in ("push", "pop", "pushad", "popad",
                                   "pushf", "pushfd", "leave"):
                dependency_overwritten |= (
                    "esp" in compared_regs or "esp" in memory_address_regs)
            if middle.operands:
                destination = middle.operands[0]
                if (getattr(destination, "type", None) == "reg" and
                        getattr(destination, "reg", None)):
                    written = register_family.get(destination.reg,
                                                  destination.reg)
                    dependency_overwritten |= (
                        written in compared_regs or
                        written in memory_address_regs)
                elif (producer_reads_memory and
                      getattr(destination, "type", None) == "mem"):
                    dependency_overwritten = True

        if dependency_overwritten and consumer is not None:
            result = None
            if consumer.is_cond_jump:
                result = _make_condition(
                    consumer.mnemonic, last_flag_setter, last_flag_ops)
            elif consumer.mnemonic.startswith("set"):
                condition = _make_setcc_value(
                    consumer.mnemonic, last_flag_setter, last_flag_ops)
                if condition:
                    result = (condition, consumer.mnemonic)
            elif consumer.mnemonic.startswith("cmov"):
                condition = _make_cmovcc_cond(
                    consumer.mnemonic, last_flag_setter, last_flag_ops)
                if condition:
                    result = (condition, consumer.mnemonic)
            if result:
                condition, description = result
                stmts.append(
                    f"_flags = ({condition}); "
                    f"/* preserve inherited {last_flag_setter} flags across "
                    f"{prefix_length} instruction(s) */")
                last_flag_setter = "snapshot_condition"
                last_flag_ops = [description, consumer.mnemonic]

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        match = try_match_cmp_jcc(insns, i, lifter=lifter)
        if match:
            stmt, consumed = match
            stmts.append(stmt)
            # Preserve the flag-setter from the cmp/test since jcc
            # doesn't modify flags - subsequent jcc can reuse them
            flag_insn = insns[i]
            last_flag_setter = flag_insn.mnemonic
            last_flag_ops = list(flag_insn.operands)
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # EFLAGS-preserving instructions can overwrite a register used to
        # reconstruct the prior condition.  For example, retail MSVC emits
        # ``dec ecx; mov [local], ecx; mov ecx, [bound]; jne``: JNE consumes
        # DEC's ZF, not the reloaded ECX.  Snapshot the condition before any
        # intervening write to one of its register dependencies.
        if curr.mnemonic in FLAG_SETTERS or curr.mnemonic in _EFLAGS_SETTERS:
            next_idx = i + 1
            register_family = {
                "al": "eax", "ah": "eax", "ax": "eax", "eax": "eax",
                "bl": "ebx", "bh": "ebx", "bx": "ebx", "ebx": "ebx",
                "cl": "ecx", "ch": "ecx", "cx": "ecx", "ecx": "ecx",
                "dl": "edx", "dh": "edx", "dx": "edx", "edx": "edx",
                "si": "esi", "esi": "esi", "di": "edi", "edi": "edi",
                "sp": "esp", "esp": "esp", "bp": "ebp", "ebp": "ebp",
            }
            compared_regs = set()
            memory_address_regs = set()
            for op in curr.operands:
                if op.type == "reg" and op.reg:
                    compared_regs.add(register_family.get(op.reg, op.reg))
                elif op.type == "mem":
                    if op.mem_base:
                        memory_address_regs.add(register_family.get(
                            op.mem_base, op.mem_base))
                    if op.mem_index:
                        memory_address_regs.add(register_family.get(
                            op.mem_index, op.mem_index))
            overwrites_compared_reg = False
            overwrites_memory_address_reg = False
            while (next_idx < len(insns)
                   and (insns[next_idx].mnemonic in _EFLAGS_PRESERVE
                        or (insns[next_idx].mnemonic.startswith("f")
                            and insns[next_idx].mnemonic not in (
                                "fcomi", "fcomip", "fucomi", "fucomip",
                                "fucompi")))
                   and insns[next_idx].mnemonic != 'lahf'):
                middle = insns[next_idx]
                # PUSH/POP preserve arithmetic flags but change ESP even
                # though their explicit operand names a different register
                # (or memory). Stack-relative TEST/CMP must use the address
                # at the producer, not the adjusted stack at the branch.
                if middle.mnemonic in ("push", "pop", "pushad", "popad",
                                       "pushf", "pushfd", "leave"):
                    overwrites_compared_reg |= "esp" in compared_regs
                    overwrites_memory_address_reg |= "esp" in memory_address_regs
                if middle.operands and middle.operands[0].type == "reg":
                    written = register_family.get(middle.operands[0].reg,
                                                  middle.operands[0].reg)
                    if written in compared_regs:
                        overwrites_compared_reg = True
                    if written in memory_address_regs:
                        overwrites_memory_address_reg = True
                next_idx += 1
            consumer = insns[next_idx] if next_idx < len(insns) else None
            # LAHF consumes the comparison flags, not the producer operands.
            # Snapshot SSE flags when a flag-neutral instruction separates the
            # compare and LAHF: that instruction may overwrite an XMM operand
            # (as retail RsLight::RecalculateLight does with xmm0).
            needs_sse_lahf_snapshot = (
                curr.mnemonic in ("comiss", "comisd", "ucomiss", "ucomisd")
                and consumer is not None
                and consumer.mnemonic == "lahf"
                and next_idx > i + 1
            )
            if needs_sse_lahf_snapshot:
                value = _make_sse_lahf_value(curr.mnemonic, curr.operands)
                if value is not None:
                    stmts.extend(lifter.lift_instruction(curr))
                    stmts.append(
                        f"_flags = (int)({value}); "
                        f"/* preserve {curr.mnemonic} flags for lahf across "
                        f"{next_idx - i - 1} instruction(s) */")
                    last_flag_setter = "snapshot_sse_lahf"
                    last_flag_ops = [curr.mnemonic]
                    i += 1
                    continue
            # SBB/ADC consume CF even when ordinary data-movement instructions
            # separate them from CMP/TEST. Materialize carry only for that
            # exact producer/consumer chain; enabling it for every comparison
            # in a function perturbs unrelated lifted control flow.
            needs_carry_snapshot = (
                getattr(lifter, "track_carry", False)
                and consumer is not None
                and consumer.mnemonic in ("sbb", "adc")
                and curr.mnemonic in ("cmp", "test")
            )
            if needs_carry_snapshot:
                lhs = _fmt_operand_read(curr.operands[0])
                rhs = _fmt_operand_read(curr.operands[1])
                if curr.mnemonic == "cmp":
                    width = (_operand_width(curr.operands[0])
                             or _operand_width(curr.operands[1]) or 4)
                    c_type = f"uint{width * 8}_t"
                    stmts.append(
                        f"_cf = (({c_type})({lhs}) < ({c_type})({rhs})); "
                        f"/* preserve cmp carry across "
                        f"{next_idx - i - 1} instruction(s) */")
                else:
                    stmts.append(
                        f"_cf = 0; /* preserve test carry across "
                        f"{next_idx - i - 1} instruction(s) */")
                last_flag_setter = curr.mnemonic
                last_flag_ops = list(curr.operands)
                i += 1
                continue
            needs_memory_snapshot = (
                consumer is not None
                and curr.mnemonic in ("cmp", "test", "comiss", "comisd",
                                      "ucomiss", "ucomisd")
                and (consumer.is_cond_jump
                     or consumer.mnemonic.startswith(("set", "cmov")))
                and overwrites_memory_address_reg
            )
            if ((overwrites_compared_reg or needs_memory_snapshot)
                    and consumer is not None):
                result = None
                if consumer.is_cond_jump:
                    result = _make_condition(
                        consumer.mnemonic, curr.mnemonic, curr.operands)
                elif consumer.mnemonic.startswith("set"):
                    cond = _make_setcc_value(
                        consumer.mnemonic, curr.mnemonic, curr.operands)
                    if cond:
                        result = (cond, consumer.mnemonic)
                elif consumer.mnemonic.startswith("cmov"):
                    cond = _make_cmovcc_cond(
                        consumer.mnemonic, curr.mnemonic, curr.operands)
                    if cond:
                        result = (cond, consumer.mnemonic)
                if result:
                    cond_expr, desc = result
                    stmts.extend(lifter.lift_instruction(curr))
                    stmts.append(
                        f"_flags = ({cond_expr}); "
                        f"/* preserve {curr.mnemonic} flags across "
                        f"{next_idx - i - 1} instruction(s) */")
                    last_flag_setter = "snapshot_condition"
                    last_flag_ops = [desc, consumer.mnemonic]
                    i += 1
                    continue
        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and last_flag_setter == "snapshot_condition":
            desc = last_flag_ops[0] if last_flag_ops else "snapshotted condition"
            snapshot_consumer = (
                last_flag_ops[1] if len(last_flag_ops) > 1 else curr.mnemonic)
            snapshot_test = _snapshot_condition_test(
                curr.mnemonic, snapshot_consumer) or "_flags != 0"
            target = curr.jump_target
            stmt = _emit_cond_goto(
                snapshot_test, curr.mnemonic, desc, target, lifter)
            stmts.append(stmt)
            i += 1
            continue

        if (curr.mnemonic.startswith("set")
                and last_flag_setter == "snapshot_condition"
                and len(curr.operands) >= 1):
            snapshot_consumer = (
                last_flag_ops[1] if len(last_flag_ops) > 1 else curr.mnemonic)
            snapshot_test = _snapshot_condition_test(
                curr.mnemonic, snapshot_consumer) or "_flags != 0"
            stmts.append(
                _fmt_operand_write(curr.operands[0],
                                   f"({snapshot_test}) ? 1 : 0")
                + f" /* {curr.mnemonic} */")
            i += 1
            continue

        if (curr.mnemonic.startswith("cmov")
                and last_flag_setter == "snapshot_condition"
                and len(curr.operands) >= 2):
            src = _fmt_operand_read(curr.operands[1])
            snapshot_consumer = (
                last_flag_ops[1] if len(last_flag_ops) > 1 else curr.mnemonic)
            snapshot_test = _snapshot_condition_test(
                curr.mnemonic, snapshot_consumer) or "_flags != 0"
            stmts.append(
                f"if ({snapshot_test}) "
                + _fmt_operand_write(curr.operands[0], src)
                + f" /* {curr.mnemonic} */")
            i += 1
            continue

        if curr.is_cond_jump and last_flag_setter:
            result = _make_condition(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if result:
                cond_expr, desc = result
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic in ("sete", "setne", "setb", "setae", "setbe",
                              "seta", "setl", "setge", "setle", "setg",
                              "sets", "setns")
                and last_flag_setter and len(curr.operands) >= 1):
            cond = _make_setcc_value(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        if (curr.mnemonic in ("cmove", "cmovne", "cmovb", "cmovae",
                              "cmovbe", "cmova", "cmovl", "cmovge",
                              "cmovle", "cmovg", "cmovs", "cmovns")
                and last_flag_setter and len(curr.operands) >= 2):
            cond = _make_cmovcc_cond(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # A separated SSE compare was snapshotted before a flag-neutral
        # instruction overwrote one of its operands. LAHF now materializes
        # that architectural snapshot rather than reconstructing stale data.
        if (curr.mnemonic == "lahf"
                and last_flag_setter == "snapshot_sse_lahf"):
            source = last_flag_ops[0] if last_flag_ops else "SSE compare"
            stmts.append(
                f"SET_HI8(eax, (uint32_t)_flags); "
                f"/* lahf from snapshotted {source} */")
            i += 1
            continue

        # LAHF materializes the current arithmetic flags in AH.  MSVC uses
        # UCOMISS/LAHF/TEST AH,44h/JP to detect every result except ordered
        # equality.  The old no-op LAHF left stale AH and made JP constant.
        if (curr.mnemonic == "lahf"
                and last_flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd")
                and len(last_flag_ops) >= 2):
            value = _make_sse_lahf_value(last_flag_setter, last_flag_ops)
            stmts.append(f"SET_HI8(eax, {value}); /* lahf from {last_flag_setter} */")
            i += 1
            continue

        # FCOMI/FUCOMI set ZF/PF/CF directly.  A following LAHF must snapshot
        # those flags before TEST AH,44h; leaving AH stale makes ordinary
        # numeric Lua table keys look like NaN.
        if (curr.mnemonic == "lahf"
                and last_flag_setter in ("fcomi", "fcomip", "fcompi",
                                         "fucomi", "fucomip", "fucompi")):
            value = ("((_fpu_cmp == 2) ? 0x45u : "
                     "(_fpu_cmp < 0) ? 0x01u : "
                     "(_fpu_cmp == 0) ? 0x40u : 0u)")
            stmts.append(
                f"SET_HI8(eax, {value}); /* lahf from {last_flag_setter} */")
            i += 1
            continue

        # Lift the instruction normally
        results = lifter.lift_instruction(insns[i])
        stmts.extend(results)

        # Track flag-setting instructions
        if curr.mnemonic in FLAG_SETTERS:
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _FLAGS_UNDEFINED:
            # Flags are undefined after these - clear tracking
            last_flag_setter = None
            last_flag_ops = []
        elif curr.mnemonic in _EFLAGS_SETTERS:
            # Additional flag-setting instructions
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _EFLAGS_PRESERVE:
            pass  # These don't affect EFLAGS
        elif curr.mnemonic in ("fcompi", "fcomip", "fucomi", "fucompi",
                                "fucomip", "fcomi"):
            # FPU compare-to-EFLAGS: sets CF, ZF, PF directly
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic == "sahf":
            # sahf loads AH into flags - typically after fnstsw ax
            # in the fcomp/fnstsw/sahf pattern for FPU comparisons
            last_flag_setter = "sahf"
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic.startswith("f") or curr.mnemonic.startswith("cmov"):
            pass  # FPU and already-handled CMOVcc
        elif curr.mnemonic.startswith("j"):
            pass  # Jumps don't set flags
        elif curr.mnemonic.startswith("set"):
            pass  # SETcc doesn't set flags
        elif curr.mnemonic.startswith("rep"):
            # rep movsb/movsd = data copy, preserves flags
            # repe cmpsb/repne scasb = comparison, sets flags
            rest = curr.op_str.strip() if hasattr(curr, 'op_str') else ""
            raw_m = curr.mnemonic
            if "cmps" in raw_m or "scas" in raw_m:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            elif "cmps" in rest or "scas" in rest:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            else:
                pass  # rep movs/stos = data movement, flags preserved
        else:
            # Unknown instruction - conservatively clear flag state
            last_flag_setter = None
            last_flag_ops = []

        i += 1

    out_flag_state = (last_flag_setter, last_flag_ops) if last_flag_setter else None
    return stmts, out_flag_state
