//! Instruction decoder for DSP56300 24-bit instruction words.
//!
//! Decodes raw opcode words into structured [`Instruction`] values.

use crate::{
    Accumulator, CondCode, Instruction, MemSpace, MulShiftOp, ParallelMoveType, REGISTER_NAMES,
    REGISTERS_TCC, decode_parallel_alu, ggg_reg, qq_reg, qq_reg_mulshift, qqq_reg, qqqq_regs,
    sss_reg,
};

// --- Opcode table machinery ---

/// A non-parallel opcode table entry.
struct OpcodeEntry {
    mask: u32,
    match_val: u32,
    name: &'static str,
    /// Whether this entry needs MMMRRR addressing mode validation.
    /// Auto-detected from the presence of 'M' in the template string.
    has_mmmrrr: bool,
    /// Decode function: extracts fields and returns Instruction.
    /// `None` means the instruction is recognized but not yet implemented.
    decode: Option<fn(u32) -> Instruction>,
}

/// Convert a 24-char template string (like "0000000101iiiiii1000d000")
/// into a (mask, match) pair. '0' and '1' are fixed bits; anything else
/// is a variable field.
const fn template_to_mask_match(tmpl: &str) -> (u32, u32) {
    let bytes = tmpl.as_bytes();
    assert!(bytes.len() == 24);
    let mut mask: u32 = 0;
    let mut match_val: u32 = 0;
    let mut i = 0;
    while i < 24 {
        let bit = 23 - i;
        match bytes[i] {
            b'0' => {
                mask |= 1 << bit;
            }
            b'1' => {
                mask |= 1 << bit;
                match_val |= 1 << bit;
            }
            _ => {} // variable bit
        }
        i += 1;
    }
    (mask, match_val)
}

/// Check whether a template contains MMMRRR addressing mode fields.
const fn tmpl_has_mmmrrr(tmpl: &str) -> bool {
    let bytes = tmpl.as_bytes();
    let mut i = 0;
    while i < 24 {
        if bytes[i] == b'M' {
            return true;
        }
        i += 1;
    }
    false
}

/// Match predicate for MMMRRR field validation.
fn match_mmmrrr(op: u32) -> bool {
    let rrr = (op >> 8) & 0x7;
    let mmm = (op >> 11) & 0x7;
    if mmm == 0x6 {
        rrr == 0x0 || rrr == 0x4
    } else {
        true
    }
}

/// Normalize mode-6 ea_mode don't-care bits in an opcode.
///
/// In mode-6 (MMM=110, bits [13:11]), only RRR bit 2 (bit 10) is significant:
///   0 = absolute address, 1 = immediate data.
/// Bits [9:8] (RRR[1:0]) are don't-care on hardware; normalize them to 0.
///
/// Additionally, if bit 15 = 0 (write direction), writing to an immediate
/// address is nonsensical. Normalize RRR[2] (bit 10) to 0 (absolute).
fn normalize_mode6_ea(opcode: u32) -> u32 {
    let mmm = (opcode >> 11) & 7;
    if mmm != 6 {
        return opcode;
    }
    let mut opcode = opcode;
    opcode &= !0x0300;
    if opcode & (1 << 15) == 0 {
        opcode &= !0x0400;
    }
    opcode
}

// --- Template-driven field extraction ---
//
// These extract fields from a 24-bit opcode using the position of named letters
// in the template string. LLVM constant-folds the template scan when the template
// is a compile-time constant, producing the same shift-and-mask code as hand-written
// extraction.

/// Extract bits from `op` at positions where `letter` appears in `tmpl`.
/// Template is 24 chars, MSB-first. Result is packed MSB-first (leftmost
/// occurrence becomes the highest bit of the result).
#[inline(always)]
fn tmpl_field(tmpl: &str, letter: u8, op: u32) -> u32 {
    let t = tmpl.as_bytes();
    let mut result: u32 = 0;
    let mut i = 0;
    while i < 24 {
        if t[i] == letter {
            result = (result << 1) | ((op >> (23 - i)) & 1);
        }
        i += 1;
    }
    result
}

/// Extract two letter groups and concatenate (hi bits above lo bits).
#[inline(always)]
fn tmpl_field2(tmpl: &str, hi: u8, lo: u8, op: u32) -> u32 {
    let lo_val = tmpl_field(tmpl, lo, op);
    let t = tmpl.as_bytes();
    let mut lo_width: u32 = 0;
    let mut i = 0;
    while i < 24 {
        if t[i] == lo {
            lo_width += 1;
        }
        i += 1;
    }
    (tmpl_field(tmpl, hi, op) << lo_width) | lo_val
}

/// Extract space select (1-bit letter -> MemSpace).
#[inline(always)]
fn tmpl_space(tmpl: &str, letter: u8, op: u32) -> MemSpace {
    MemSpace::xy(tmpl_field(tmpl, letter, op))
}

/// Extract ea_mode from M+R letters (typically 6-bit, 5-bit for LUA).
#[inline(always)]
fn tmpl_ea(tmpl: &str, op: u32) -> u8 {
    tmpl_field2(tmpl, b'M', b'R', op) as u8
}

/// Extract bit number from 'b' letter (4 or 5 bits depending on template).
#[inline(always)]
fn tmpl_bit(tmpl: &str, op: u32) -> u8 {
    tmpl_field(tmpl, b'b', op) as u8
}

/// Extract absolute address from 'a' letter.
#[inline(always)]
fn tmpl_addr(tmpl: &str, op: u32) -> u8 {
    tmpl_field(tmpl, b'a', op) as u8
}

/// Extract peripheral offset from 'p' letter.
#[inline(always)]
fn tmpl_pp(tmpl: &str, op: u32) -> u8 {
    tmpl_field(tmpl, b'p', op) as u8
}

/// Extract quick offset from 'q' letter.
#[inline(always)]
fn tmpl_qq(tmpl: &str, op: u32) -> u8 {
    tmpl_field(tmpl, b'q', op) as u8
}

/// Extract register index from a given letter.
#[inline(always)]
fn tmpl_reg(tmpl: &str, letter: u8, op: u32) -> u8 {
    tmpl_field(tmpl, letter, op) as u8
}

/// Extract 1-bit accumulator select from a given letter.
#[inline(always)]
fn tmpl_acc(tmpl: &str, letter: u8, op: u32) -> Accumulator {
    if tmpl_field(tmpl, letter, op) == 0 {
        Accumulator::A
    } else {
        Accumulator::B
    }
}

/// Extract condition code from 'C' letter (4-bit).
#[inline(always)]
fn tmpl_cc(tmpl: &str, op: u32) -> CondCode {
    CondCode::from_bits(tmpl_field(tmpl, b'C', op))
}

/// Extract read/write direction from 'W' letter (1-bit bool).
#[inline(always)]
fn tmpl_w(tmpl: &str, op: u32) -> bool {
    tmpl_field(tmpl, b'W', op) != 0
}

/// Extract 12-bit loop count from 'i' (high) + 'h' (low) letters.
#[inline(always)]
fn tmpl_count(tmpl: &str, op: u32) -> u16 {
    tmpl_field2(tmpl, b'h', b'i', op) as u16
}

/// Extract immediate from 'i' letter.
#[inline(always)]
fn tmpl_imm(tmpl: &str, op: u32) -> u8 {
    tmpl_field(tmpl, b'i', op) as u8
}

/// Extract 9-bit sign-extended address from 'a' letter (for bcc/bra/bsr short).
#[inline(always)]
fn tmpl_sext9(tmpl: &str, op: u32) -> i32 {
    let val = tmpl_field(tmpl, b'a', op);
    if val & (1 << 8) != 0 {
        (val | 0xFFFFFE00) as i32
    } else {
        val as i32
    }
}

fn dec_tcc(op: u32) -> Instruction {
    let bit16 = (op >> 16) & 1 != 0;
    let bit11 = (op >> 11) & 1 != 0;
    // Template 1: bit16=0, bit11=0 -> acc only
    // Template 2: bit16=1          -> acc + R reg
    // Template 3: bit16=0, bit11=1 -> R reg only
    let acc = if !bit11 {
        let tcc_idx = ((op >> 3) & 0xF) as usize;
        let pair = REGISTERS_TCC[tcc_idx];
        Some((pair[0], pair[1]))
    } else {
        None
    };
    let r = if bit16 || bit11 {
        Some((((op >> 8) & 0x7) as u8, (op & 0x7) as u8))
    } else {
        None
    };
    Instruction::Tcc {
        cc: CondCode::from_bits((op >> 12) & 0xF),
        acc,
        r,
    }
}

/// Build an opcode table entry at compile time.
/// MMMRRR validation is auto-detected from the template string.
const fn entry(
    tmpl: &str,
    name: &'static str,
    decode: Option<fn(u32) -> Instruction>,
) -> OpcodeEntry {
    let (mask, match_val) = template_to_mask_match(tmpl);
    OpcodeEntry {
        mask,
        match_val,
        name,
        has_mmmrrr: tmpl_has_mmmrrr(tmpl),
        decode,
    }
}

/// Opcode table entry macro. All entries use `op!(template, name, ...)`.
macro_rules! op {
    // --- Core forms ---
    ($tmpl:expr, $name:expr) => {
        entry($tmpl, $name, None)
    };
    ($tmpl:expr, $name:expr, fn $decode:expr) => {
        entry($tmpl, $name, Some($decode))
    };
    ($tmpl:expr, $name:expr, |$t:ident, $opc:ident| $V:ident { $($field:ident : $extract:expr),* $(,)? }) => {{
        const $t: &str = $tmpl;
        fn decode($opc: u32) -> Instruction {
            Instruction::$V { $($field: $extract,)* }
        }
        entry($tmpl, $name, Some(decode))
    }};
    // Block-body form: closure body is an arbitrary block returning Instruction
    ($tmpl:expr, $name:expr, |$t:ident, $opc:ident| $body:block) => {{
        const $t: &str = $tmpl;
        fn decode($opc: u32) -> Instruction $body
        entry($tmpl, $name, Some(decode))
    }};
    // --- Bit-op shapes (bchg/bclr/bset/btst/jclr/jset/jsclr/jsset/brclr/brset) ---
    ($tmpl:expr, $name:expr, bit ea, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            space: tmpl_space(T, b'S', opc),
            ea_mode: tmpl_ea(T, opc),
            bit_num: tmpl_bit(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, bit aa, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            space: tmpl_space(T, b'S', opc),
            addr: tmpl_addr(T, opc),
            bit_num: tmpl_bit(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, bit pp, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            space: tmpl_space(T, b'S', opc),
            pp_offset: tmpl_pp(T, opc),
            bit_num: tmpl_bit(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, bit qq, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            space: tmpl_space(T, b'S', opc),
            qq_offset: tmpl_qq(T, opc),
            bit_num: tmpl_bit(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, bit reg, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            reg_idx: tmpl_reg(T, b'D', opc),
            bit_num: tmpl_bit(T, opc),
        })
    };
    // --- Loop shapes (DO/DOR/REP) ---
    ($tmpl:expr, $name:expr, loop ea, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            space: tmpl_space(T, b'S', opc),
            ea_mode: tmpl_ea(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, loop aa, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            space: tmpl_space(T, b'S', opc),
            addr: tmpl_addr(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, loop imm, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            count: tmpl_count(T, opc),
        })
    };
    ($tmpl:expr, $name:expr, loop reg, $V:ident, $letter:expr) => {
        op!($tmpl, $name, |T, opc| $V {
            reg_idx: tmpl_reg(T, $letter, opc),
        })
    };
    // --- ALU shapes ---
    ($tmpl:expr, $name:expr, alu_imm, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            imm: tmpl_imm(T, opc),
            d: tmpl_acc(T, b'd', opc),
        })
    };
    ($tmpl:expr, $name:expr, alu_long, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V {
            d: tmpl_acc(T, b'd', opc),
        })
    };
    // --- Simple shapes ---
    ($tmpl:expr, $name:expr, cc, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { cc: tmpl_cc(T, opc) })
    };
    ($tmpl:expr, $name:expr, cc rn, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { cc: tmpl_cc(T, opc), rn: tmpl_field(T, b'R', opc) as u8 })
    };
    ($tmpl:expr, $name:expr, cc ea, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { cc: tmpl_cc(T, opc), ea_mode: tmpl_ea(T, opc) })
    };
    ($tmpl:expr, $name:expr, ea, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { ea_mode: tmpl_ea(T, opc) })
    };
    ($tmpl:expr, $name:expr, acc, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { d: tmpl_acc(T, b'd', opc) })
    };
    ($tmpl:expr, $name:expr, rn, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { rn: tmpl_field(T, b'R', opc) as u8 })
    };
    ($tmpl:expr, $name:expr, shift imm, $V:ident) => {
        op!($tmpl, $name, |T, opc| $V { shift: tmpl_imm(T, opc), d: tmpl_acc(T, b'D', opc) })
    };
    ($tmpl:expr, $name:expr, shift reg, $V:ident) => {
        op!($tmpl, $name, |T, opc| {
            match sss_reg(tmpl_field(T, b's', opc) as u8) {
                Some(src) => Instruction::$V { src, d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        })
    };
    ($tmpl:expr, $name:expr, mulshift, $op:expr) => {
        op!($tmpl, $name, |T, opc| MulShift { op: $op, shift: tmpl_field(T, b's', opc) as u8, src: qq_reg_mulshift(tmpl_field(T, b'Q', opc) as u8), d: tmpl_acc(T, b'd', opc), k: tmpl_field(T, b'k', opc) != 0 })
    };
    // --- Fieldless decode (catch-all for bare variant names) ---
    ($tmpl:expr, $name:expr, $V:ident) => {{
        fn decode(_opc: u32) -> Instruction {
            Instruction::$V
        }
        entry($tmpl, $name, Some(decode))
    }};
}

#[rustfmt::skip]
const OPCODE_TABLE: [OpcodeEntry; 185] = [
        op!("0000000101iiiiii1000d000", "add #xx, D", alu_imm, AddImm),
        op!("00000001010000001100d000", "add #xxxx, D", alu_long, AddLong),
        op!("0000000101iiiiii1000d110", "and #xx, D", alu_imm, AndImm),
        op!("00000001010000001100d110", "and #xxxx, D", alu_long, AndLong),
        op!("00000000iiiiiiii101110EE", "andi #xx, D", |T, opc| AndI { imm: tmpl_imm(T, opc), dest: tmpl_reg(T, b'E', opc) }),
        op!("0000110000011101SiiiiiiD", "asl #ii, S2, D", |T, opc| AslImm { shift: tmpl_imm(T, opc), s: tmpl_acc(T, b'S', opc), d: tmpl_acc(T, b'D', opc) }),
        op!("0000110000011110010SsssD", "asl S1, S2, D", |T, opc| {
            match sss_reg(tmpl_field(T, b's', opc) as u8) {
                Some(src) => Instruction::AslReg { src, s: tmpl_acc(T, b'S', opc), d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("0000110000011100SiiiiiiD", "asr #ii, S2, D", |T, opc| AsrImm { shift: tmpl_imm(T, opc), s: tmpl_acc(T, b'S', opc), d: tmpl_acc(T, b'D', opc) }),
        op!("0000110000011110011SsssD", "asr S1, S2, D", |T, opc| {
            match sss_reg(tmpl_field(T, b's', opc) as u8) {
                Some(src) => Instruction::AsrReg { src, s: tmpl_acc(T, b'S', opc), d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("00001101000100000100CCCC", "bcc xxxx", cc, BccLong),
        op!("00000101CCCC01aaaa0aaaaa", "bcc xxx", |T, opc| Bcc { cc: tmpl_cc(T, opc), addr: tmpl_sext9(T, opc) }),
        op!("0000110100011RRR0100CCCC", "bcc Rn", cc rn, BccRn),
        op!("0000101101MMMRRR0S00bbbb", "bchg #n, [X or Y]:ea", bit ea, BchgEa),
        op!("0000101100aaaaaa0S00bbbb", "bchg #n, [X or Y]:aa", bit aa, BchgAa),
        op!("0000101110pppppp0S00bbbb", "bchg #n, [X or Y]:pp", bit pp, BchgPp),
        op!("0000000101qqqqqq0S0bbbbb", "bchg #n, [X or Y]:qq", bit qq, BchgQq),
        op!("0000101111DDDDDD010bbbbb", "bchg #n, D", bit reg, BchgReg),
        op!("0000101001MMMRRR0S00bbbb", "bclr #n, [X or Y]:ea", bit ea, BclrEa),
        op!("0000101000aaaaaa0S00bbbb", "bclr #n, [X or Y]:aa", bit aa, BclrAa),
        op!("0000101010pppppp0S00bbbb", "bclr #n, [X or Y]:pp", bit pp, BclrPp),
        op!("0000000100qqqqqq0S00bbbb", "bclr #n, [X or Y]:qq", bit qq, BclrQq),
        op!("0000101011DDDDDD010bbbbb", "bclr #n, D", bit reg, BclrReg),
        op!("000011010001000011000000", "bra xxxx", BraLong),
        op!("00000101000011aaaa0aaaaa", "bra xxx", |T, opc| Bra { addr: tmpl_sext9(T, opc) }),
        op!("0000110100011RRR11000000", "bra Rn", rn, BraRn),
        op!("0000110010MMMRRR0S0bbbbb", "brclr #n, [X or Y]:ea, xxxx", bit ea, BrclrEa),
        op!("0000110010aaaaaa1S0bbbbb", "brclr #n, [X or Y]:aa, xxxx", bit aa, BrclrAa),
        op!("0000110011pppppp0S0bbbbb", "brclr #n, [X or Y]:pp, xxxx", bit pp, BrclrPp),
        op!("0000010010qqqqqq0S0bbbbb", "brclr #n, [X or Y]:qq, xxxx", bit qq, BrclrQq),
        op!("0000110011DDDDDD100bbbbb", "brclr #n, S, xxxx", bit reg, BrclrReg),
        op!("00000000000000100001CCCC", "brkcc", cc, Brkcc),
        op!("0000110010MMMRRR0S1bbbbb", "brset #n, [X or Y]:ea, xxxx", bit ea, BrsetEa),
        op!("0000110010aaaaaa1S1bbbbb", "brset #n, [X or Y]:aa, xxxx", bit aa, BrsetAa),
        op!("0000110011pppppp0S1bbbbb", "brset #n, [X or Y]:pp, xxxx", bit pp, BrsetPp),
        op!("0000010010qqqqqq0S1bbbbb", "brset #n, [X or Y]:qq, xxxx", bit qq, BrsetQq),
        op!("0000110011DDDDDD101bbbbb", "brset #n, S, xxxx", bit reg, BrsetReg),
        op!("00001101000100000000CCCC", "bscc xxxx", cc, BsccLong),
        op!("00000101CCCC00aaaa0aaaaa", "bscc xxx", |T, opc| Bscc { cc: tmpl_cc(T, opc), addr: tmpl_sext9(T, opc) }),
        op!("0000110100011RRR0000CCCC", "bscc Rn", cc rn, BsccRn),
        op!("0000110110MMMRRR0S0bbbbb", "bsclr #n, [X or Y]:ea, xxxx", bit ea, BsclrEa),
        op!("0000110110aaaaaa1S0bbbbb", "bsclr #n, [X or Y]:aa, xxxx", bit aa, BsclrAa),
        op!("0000010010qqqqqq1S0bbbbb", "bsclr #n, [X or Y]:qq, xxxx", bit qq, BsclrQq),
        op!("0000110111pppppp0S0bbbbb", "bsclr #n, [X or Y]:pp, xxxx", bit pp, BsclrPp),
        op!("0000110111DDDDDD100bbbbb", "bsclr #n, S, xxxx", bit reg, BsclrReg),
        op!("0000101001MMMRRR0S1bbbbb", "bset #n, [X or Y]:ea", bit ea, BsetEa),
        op!("0000101000aaaaaa0S1bbbbb", "bset #n, [X or Y]:aa", bit aa, BsetAa),
        op!("0000101010pppppp0S1bbbbb", "bset #n, [X or Y]:pp", bit pp, BsetPp),
        op!("0000000100qqqqqq0S1bbbbb", "bset #n, [X or Y]:qq", bit qq, BsetQq),
        op!("0000101011DDDDDD011bbbbb", "bset #n, D", bit reg, BsetReg),
        op!("000011010001000010000000", "bsr xxxx", BsrLong),
        op!("00000101000010aaaa0aaaaa", "bsr xxx", |T, opc| Bsr { addr: tmpl_sext9(T, opc) }),
        op!("0000110100011RRR10000000", "bsr Rn", rn, BsrRn),
        op!("0000110110MMMRRR0S1bbbbb", "bsset #n, [X or Y]:ea, xxxx", bit ea, BssetEa),
        op!("0000110110aaaaaa1S1bbbbb", "bsset #n, [X or Y]:aa, xxxx", bit aa, BssetAa),
        op!("0000110111pppppp0S1bbbbb", "bsset #n, [X or Y]:pp, xxxx", bit pp, BssetPp),
        op!("0000010010qqqqqq1S1bbbbb", "bsset #n, [X or Y]:qq, xxxx", bit qq, BssetQq),
        op!("0000110111DDDDDD101bbbbb", "bsset #n, S, xxxx", bit reg, BssetReg),
        // DSP56300FM p.13-41 encoding diagrams show bit 4 as fixed 0 (4-bit bbbb),
        // but the instruction fields table says "Bit number [0-23]" and the official
        // Motorola asm56300.exe encodes bit numbers 16-23 using bit 4 = 1.
        // The encoding diagram is errata; the field is 5 bits (bbbbb) like BCHG/BCLR/BSET.
        op!("0000101101MMMRRR0S1bbbbb", "btst #n, [X or Y]:ea", bit ea, BtstEa),
        op!("0000101100aaaaaa0S1bbbbb", "btst #n, [X or Y]:aa", bit aa, BtstAa),
        op!("0000101110pppppp0S1bbbbb", "btst #n, [X or Y]:pp", bit pp, BtstPp),
        op!("0000000101qqqqqq0S1bbbbb", "btst #n, [X or Y]:qq", bit qq, BtstQq),
        op!("0000101111DDDDDD011bbbbb", "btst #n, D", bit reg, BtstReg),
        op!("0000110000011110000000SD", "clb S, D", |T, opc| Clb { s: tmpl_acc(T, b'S', opc), d: tmpl_acc(T, b'D', opc) }),
        op!("0000000101iiiiii1000d101", "cmp #xx, S2", alu_imm, CmpImm),
        op!("00000001010000001100d101", "cmp #xxxx, S2", alu_long, CmpLong),
        op!("00001100000111111111gggd", "cmpu S1, S2", |T, opc| {
            let d = tmpl_acc(T, b'd', opc);
            match ggg_reg(tmpl_field(T, b'g', opc) as u8, d) {
                Some(src) => Instruction::CmpU { src, d },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("000000000000001000000000", "debug", Debug),
        op!("00000000000000110000CCCC", "debugcc", cc, Debugcc),
        op!("00000000000000000000101d", "dec D", acc, Dec),
        op!("000000011000000001JJd000", "div S, D", |T, opc| Div { src: qq_reg(tmpl_field(T, b'J', opc) as u8), d: tmpl_acc(T, b'd', opc) }),
        op!("000000010010010s1sdkQQQQ", "dmac(ss,su,uu) S1, S2, D", |T, opc| {
            match qqqq_regs(tmpl_field(T, b'Q', opc) as u8) {
                Some((s1, s2)) => Instruction::Dmac { ss: ((tmpl_field(T, b's', opc) as u8) & 0x3), k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), s1, s2 },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("0000011001MMMRRR0S000000", "do [X or Y]:ea, expr", loop ea, DoEa),
        op!("0000011000aaaaaa0S000000", "do [X or Y]:aa, expr", loop aa, DoAa),
        op!("00000110iiiiiiii1000hhhh", "do #xxx, expr", loop imm, DoImm),
        op!("0000011011DDDDDD00000000", "do S, expr", loop reg, DoReg, b'D'),
        op!("000000000000001000000011", "do forever, expr", fn |_| Instruction::DoForever),
        op!("0000011001MMMRRR0S010000", "dor [X or Y]:ea, label", loop ea, DorEa),
        op!("0000011000aaaaaa0S010000", "dor [X or Y]:aa, label", loop aa, DorAa),
        op!("00000110iiiiiiii1001hhhh", "dor #xxx, label", loop imm, DorImm),
        op!("0000011011DDDDDD00010000", "dor S, label", loop reg, DorReg, b'D'),
        op!("000000000000001000000010", "dor forever, label", fn |_| Instruction::DorForever),
        op!("000000000000000010001100", "enddo", EndDo),
        op!("0000000101iiiiii1000d011", "eor #xx, D", alu_imm, EorImm),
        op!("00000001010000001100d011", "eor #xxxx, D", alu_long, EorLong),
        op!("0000110000011010000sSSSD", "extract S1, S2, D", |T, opc| {
            match sss_reg(tmpl_field(T, b'S', opc) as u8) {
                Some(s1) => Instruction::ExtractReg { s1, s2: tmpl_acc(T, b's', opc), d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("0000110000011000000s000D", "extract #CO, S2, D", |T, opc| ExtractImm { s2: tmpl_acc(T, b's', opc), d: tmpl_acc(T, b'D', opc) }),
        op!("0000110000011010100sSSSD", "extractu S1, S2, D", |T, opc| {
            match sss_reg(tmpl_field(T, b'S', opc) as u8) {
                Some(s1) => Instruction::ExtractuReg { s1, s2: tmpl_acc(T, b's', opc), d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("0000110000011000100s000D", "extractu #CO, S2, D", |T, opc| ExtractuImm { s2: tmpl_acc(T, b's', opc), d: tmpl_acc(T, b'D', opc) }),
        op!("000000000000000000000101", "illegal", Illegal),
        op!("00000000000000000000100d", "inc D", acc, Inc),
        op!("00001100000110110qqqSSSD", "insert S1, S2, D", |T, opc| {
            match (sss_reg(tmpl_field(T, b'S', opc) as u8), qqq_reg(tmpl_field(T, b'q', opc) as u8)) {
                (Some(s1), Some(s2)) => Instruction::InsertReg { s1, s2, d: tmpl_acc(T, b'D', opc) },
                _ => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("00001100000110010qqq000D", "insert #CO, S2, D", |T, opc| {
            match qqq_reg(tmpl_field(T, b'q', opc) as u8) {
                Some(s2) => Instruction::InsertImm { s2, d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("00001110CCCCaaaaaaaaaaaa", "jcc xxx", |T, opc| Jcc { cc: tmpl_cc(T, opc), addr: tmpl_field(T, b'a', opc) }),
        op!("0000101011MMMRRR1010CCCC", "jcc ea", cc ea, JccEa),
        op!("0000101001MMMRRR1S00bbbb", "jclr #n, [X or Y]:ea, xxxx", bit ea, JclrEa),
        op!("0000101000aaaaaa1S00bbbb", "jclr #n, [X or Y]:aa, xxxx", bit aa, JclrAa),
        op!("0000101010pppppp1S00bbbb", "jclr #n, [X or Y]:pp, xxxx", bit pp, JclrPp),
        op!("0000000110qqqqqq1S00bbbb", "jclr #n, [X or Y]:qq, xxxx", bit qq, JclrQq),
        op!("0000101011DDDDDD0000bbbb", "jclr #n, S, xxxx", bit reg, JclrReg),
        op!("0000101011MMMRRR10000000", "jmp ea", ea, JmpEa),
        op!("000011000000aaaaaaaaaaaa", "jmp xxx", |T, opc| Jmp { addr: tmpl_field(T, b'a', opc) }),
        op!("00001111CCCCaaaaaaaaaaaa", "jscc xxx", |T, opc| Jscc { cc: tmpl_cc(T, opc), addr: tmpl_field(T, b'a', opc) }),
        op!("0000101111MMMRRR1010CCCC", "jscc ea", cc ea, JsccEa),
        op!("0000101101MMMRRR1S00bbbb", "jsclr #n, [X or Y]:ea, xxxx", bit ea, JsclrEa),
        op!("0000101100aaaaaa1S00bbbb", "jsclr #n, [X or Y]:aa, xxxx", bit aa, JsclrAa),
        op!("0000101110pppppp1S0bbbbb", "jsclr #n, [X or Y]:pp, xxxx", bit pp, JsclrPp),
        op!("0000000111qqqqqq1S0bbbbb", "jsclr #n, [X or Y]:qq, xxxx", bit qq, JsclrQq),
        op!("0000101111DDDDDD000bbbbb", "jsclr #n, S, xxxx", bit reg, JsclrReg),
        op!("0000101001MMMRRR1S10bbbb", "jset #n, [X or Y]:ea, xxxx", bit ea, JsetEa),
        op!("0000101000aaaaaa1S10bbbb", "jset #n, [X or Y]:aa, xxxx", bit aa, JsetAa),
        op!("0000101010pppppp1S10bbbb", "jset #n, [X or Y]:pp, xxxx", bit pp, JsetPp),
        op!("0000000110qqqqqq1S10bbbb", "jset #n, [X or Y]:qq, xxxx", bit qq, JsetQq),
        op!("0000101011DDDDDD0010bbbb", "jset #n, S, xxxx", bit reg, JsetReg),
        op!("0000101111MMMRRR10000000", "jsr ea", ea, JsrEa),
        op!("000011010000aaaaaaaaaaaa", "jsr xxx", |T, opc| Jsr { addr: tmpl_field(T, b'a', opc) }),
        op!("0000101101MMMRRR1S10bbbb", "jsset #n, [X or Y]:ea, xxxx", bit ea, JssetEa),
        op!("0000101100aaaaaa1S10bbbb", "jsset #n, [X or Y]:aa, xxxx", bit aa, JssetAa),
        op!("0000101110pppppp1S1bbbbb", "jsset #n, [X or Y]:pp, xxxx", bit pp, JssetPp),
        op!("0000000111qqqqqq1S1bbbbb", "jsset #n, [X or Y]:qq, xxxx", bit qq, JssetQq),
        op!("0000101111DDDDDD001bbbbb", "jsset #n, S, xxxx", bit reg, JssetReg),
        op!("0000010011000RRR000ddddd", "lra Rn, D", |T, opc| LraRn { addr_reg: tmpl_field(T, b'R', opc) as u8, dst_reg: tmpl_reg(T, b'd', opc) }),
        op!("0000010001000000010ddddd", "lra xxxx, D", |T, opc| LraDisp { dst_reg: tmpl_reg(T, b'd', opc) }),
        op!("000011000001111010iiiiiD", "lsl #ii, D", shift imm, LslImm),
        op!("00001100000111100001sssD", "lsl S, D", shift reg, LslReg),
        op!("000011000001111011iiiiiD", "lsr #ii, D", shift imm, LsrImm),
        op!("00001100000111100011sssD", "lsr S, D", shift reg, LsrReg),
        op!("00000100010MMRRR000ddddd", "lua ea, D", |T, opc| Lua { ea_mode: tmpl_ea(T, opc), dst_reg: tmpl_reg(T, b'd', opc) & 0xF }),
        op!("0000010000aaaRRRaaaadddd", "lua (Rn + aa), D", |T, opc| LuaRel { aa: tmpl_field(T, b'a', opc) as u8, addr_reg: tmpl_field(T, b'R', opc) as u8, dst_reg: (opc & 0x7) as u8, dest_is_n: (opc >> 3) & 1 != 0 }),
        op!("00000001000sssss11QQdk10", "mac S, #n, D", mulshift, MulShiftOp::Mac),
        op!("000000010100000111qqdk10", "maci #xxxx, S, D", |T, opc| MacI { k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), src: qq_reg(tmpl_field(T, b'q', opc) as u8) }),
        op!("00000001001001101sdkQQQQ", "mac(su,uu) S1, S2, D", |T, opc| {
            match qqqq_regs(tmpl_field(T, b'Q', opc) as u8) {
                Some((s1, s2)) => Instruction::MacSU { s: tmpl_field(T, b's', opc) as u8, k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), s1, s2 },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("00000001000sssss11QQdk11", "macr S, #n, D", mulshift, MulShiftOp::Macr),
        op!("000000010100000111qqdk11", "macri #xxxx, S, D", |T, opc| MacrI { k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), src: qq_reg(tmpl_field(T, b'q', opc) as u8) }),
        op!("00001100000110111000sssD", "merge S, D", |T, opc| {
            match sss_reg(tmpl_field(T, b's', opc) as u8) {
                Some(src) => Instruction::Merge { src, d: tmpl_acc(T, b'D', opc) },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("0000001aaaaaaRRR1asWDDDD", "move [X or Y]:(Rn + xxx) <-> R", |T, opc| MoveShortDisp { space: tmpl_space(T, b's', opc), offset: tmpl_field(T, b'a', opc) as u8, w: tmpl_w(T, opc), offreg_idx: tmpl_field(T, b'R', opc) as u8, numreg: tmpl_reg(T, b'D', opc) }),
        op!("0000101s01110RRR1WDDDDDD", "move [X or Y]:(Rn + xxxx) <-> R", |T, opc| MoveLongDisp { space: tmpl_space(T, b's', opc), w: tmpl_w(T, opc), offreg_idx: tmpl_field(T, b'R', opc) as u8, numreg: tmpl_reg(T, b'D', opc) }),
        op!("00000101W1MMMRRR0s1ddddd", "movec [X or Y]:ea <-> R", |T, opc| MovecEa { ea_mode: tmpl_ea(T, opc), numreg: (opc & 0x3F) as u8, w: tmpl_w(T, opc), space: tmpl_space(T, b's', opc) }),
        op!("00000101W0aaaaaa0s1ddddd", "movec [X or Y]:aa <-> R", |T, opc| MovecAa { addr: tmpl_addr(T, opc), numreg: (opc & 0x3F) as u8, w: tmpl_w(T, opc), space: tmpl_space(T, b's', opc) }),
        op!("00000100W1eeeeee101ddddd", "movec R1, R2", |T, opc| MovecReg { src_reg: tmpl_field(T, b'e', opc) as u8, dst_reg: (opc & 0x3F) as u8, w: tmpl_w(T, opc) }),
        op!("00000101iiiiiiii101ddddd", "movec #xx, D1", |T, opc| MovecImm { imm: tmpl_imm(T, opc), dest: (opc & 0x3F) as u8 }),
        op!("00000111W1MMMRRR10dddddd", "movem P:ea <-> R", |T, opc| MovemEa { ea_mode: tmpl_ea(T, opc), numreg: tmpl_reg(T, b'd', opc), w: tmpl_w(T, opc) }),
        op!("00000111W0aaaaaa00dddddd", "movem P:ea <-> R", |T, opc| MovemAa { addr: tmpl_addr(T, opc), numreg: tmpl_reg(T, b'd', opc), w: tmpl_w(T, opc) }),
        op!("0000100sW1MMMRRR1Spppppp", "movep [X or Y]:ea <-> [X or Y]:pp", |T, opc| Movep23 { pp_offset: tmpl_pp(T, opc), ea_mode: tmpl_ea(T, opc), w: tmpl_w(T, opc), perspace: tmpl_space(T, b's', opc), easpace: tmpl_space(T, b'S', opc) }),
        op!("00000111W1MMMRRR0Sqqqqqq", "movep [X or Y]:ea <-> X:qq", |T, opc| MovepQq { qq_offset: tmpl_qq(T, opc), ea_mode: tmpl_ea(T, opc), w: tmpl_w(T, opc), qqspace: MemSpace::X, easpace: tmpl_space(T, b'S', opc) }),
        op!("00000111W0MMMRRR1Sqqqqqq", "movep [X or Y]:ea <-> Y:qq", |T, opc| MovepQq { qq_offset: tmpl_qq(T, opc), ea_mode: tmpl_ea(T, opc), w: tmpl_w(T, opc), qqspace: MemSpace::Y, easpace: tmpl_space(T, b'S', opc) }),
        op!("0000100sW1MMMRRR01pppppp", "movep [X or Y]:pp <-> P:ea", |T, opc| Movep1 { pp_offset: tmpl_pp(T, opc), ea_mode: tmpl_ea(T, opc), w: tmpl_w(T, opc), space: tmpl_space(T, b's', opc) }),
        op!("000000001WMMMRRR0sqqqqqq", "movep [X or Y]:qq <-> P:ea", |T, opc| MovepQqPea { qq_offset: tmpl_qq(T, opc), ea_mode: tmpl_ea(T, opc), w: tmpl_w(T, opc), space: tmpl_space(T, b's', opc) }),
        op!("0000100sW1dddddd00pppppp", "movep [X or Y]:pp <-> R", |T, opc| Movep0 { pp_offset: tmpl_pp(T, opc), reg_idx: tmpl_reg(T, b'd', opc), w: tmpl_w(T, opc), space: tmpl_space(T, b's', opc) }),
        op!("00000100W1dddddd1q0qqqqq", "movep X:qq <-> R", |T, opc| MovepQqR { qq_offset: tmpl_qq(T, opc), reg_idx: tmpl_reg(T, b'd', opc), w: tmpl_w(T, opc), space: MemSpace::X }),
        op!("00000100W1dddddd0q1qqqqq", "movep Y:qq <-> R", |T, opc| MovepQqR { qq_offset: tmpl_qq(T, opc), reg_idx: tmpl_reg(T, b'd', opc), w: tmpl_w(T, opc), space: MemSpace::Y }),
        op!("00000001000sssss11QQdk00", "mpy S, #n, D", mulshift, MulShiftOp::Mpy),
        op!("00000001001001111sdkQQQQ", "mpy(su,uu) S1, S2, D", |T, opc| {
            match qqqq_regs(tmpl_field(T, b'Q', opc) as u8) {
                Some((s1, s2)) => Instruction::MpySU { s: tmpl_field(T, b's', opc) as u8, k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), s1, s2 },
                None => Instruction::Unknown { opcode: opc },
            }
        }),
        op!("000000010100000111qqdk00", "mpyi #xxxx, S, D", |T, opc| MpyI { k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), src: qq_reg(tmpl_field(T, b'q', opc) as u8) }),
        op!("00000001000sssss11QQdk01", "mpyr S, #n, D", mulshift, MulShiftOp::Mpyr),
        op!("000000010100000111qqdk01", "mpyri #xxxx, S, D", |T, opc| MpyrI { k: tmpl_field(T, b'k', opc) != 0, d: tmpl_acc(T, b'd', opc), src: qq_reg(tmpl_field(T, b'q', opc) as u8) }),
        op!("000000000000000000000000", "nop", Nop),
        op!("0000000111011RRR0001d101", "norm Rn, D", |T, opc| Norm { rreg_idx: tmpl_field(T, b'R', opc) as u8, d: tmpl_acc(T, b'd', opc) }),
        op!("00001100000111100010sssD", "normf S, D", shift reg, Normf),
        op!("0000000101iiiiii1000d010", "or #xx, D", alu_imm, OrImm),
        op!("00000001010000001100d010", "or #xxxx, D", alu_long, OrLong),
        op!("00000000iiiiiiii111110EE", "ori #xx, D", |T, opc| OrI { imm: tmpl_imm(T, opc), dest: tmpl_reg(T, b'E', opc) }),
        op!("000000000000000000000011", "pflush", Pflush),
        op!("000000000000000000000001", "pflushun", Pflushun),
        op!("000000000000000000000010", "pfree", Pfree),
        op!("0000101111MMMRRR10000001", "plock ea", ea, PlockEa),
        op!("000000000000000000001111", "plockr xxxx", Plockr),
        op!("0000101011MMMRRR10000001", "punlock ea", ea, PunlockEa),
        op!("000000000000000000001110", "punlockr xxxx", Punlockr),
        op!("0000011001MMMRRR0S100000", "rep [X or Y]:ea", loop ea, RepEa),
        op!("0000011000aaaaaa0S100000", "rep [X or Y]:aa", loop aa, RepAa),
        op!("00000110iiiiiiii1010hhhh", "rep #xxx", loop imm, RepImm),
        op!("0000011011dddddd00100000", "rep S", loop reg, RepReg, b'd'),
        op!("000000000000000010000100", "reset", Reset),
        op!("000000000000000000000100", "rti", Rti),
        op!("000000000000000000001100", "rts", Rts),
        op!("000000000000000010000111", "stop", Stop),
        op!("0000000101iiiiii1000d100", "sub #xx, D", alu_imm, SubImm),
        op!("00000001010000001100d100", "sub #xxxx, D", alu_long, SubLong),
        op!("00000010CCCC00000JJJd000", "tcc S1, D1", fn dec_tcc),
        op!("00000011CCCC0ttt0JJJdTTT", "tcc S1,D1 S2,D2", fn dec_tcc),
        op!("00000010CCCC1ttt00000TTT", "tcc S2, D2", fn dec_tcc),
        op!("000000000000000000000110", "trap", Trap),
        op!("00000000000000000001CCCC", "trapcc", cc, Trapcc),
        op!("0000101S11MMMRRR110i0000", "vsl", |T, opc| Vsl { s: tmpl_acc(T, b'S', opc), ea_mode: tmpl_ea(T, opc), i_bit: tmpl_field(T, b'i', opc) as u8 }),
        op!("000000000000000010000110", "wait", Wait),
];

// --- Prefix dispatch table ---
// Entries are grouped by the 4-bit prefix (bits 19:16 of the opcode).
// Built at compile time from OPCODE_TABLE mask/match values.

const MAX_PER_BUCKET: usize = 34;

struct PrefixBucket {
    /// Indices into OPCODE_TABLE, in original table order.
    indices: [u8; MAX_PER_BUCKET],
    len: u8,
}

struct PrefixTable {
    buckets: [PrefixBucket; 16],
}

const fn build_prefix_table() -> PrefixTable {
    const EMPTY: PrefixBucket = PrefixBucket {
        indices: [0; MAX_PER_BUCKET],
        len: 0,
    };
    let mut table = PrefixTable {
        buckets: [EMPTY; 16],
    };
    let mut i = 0;
    while i < OPCODE_TABLE.len() {
        let top4_mask = (OPCODE_TABLE[i].mask >> 16) & 0xF;
        let top4_match = (OPCODE_TABLE[i].match_val >> 16) & 0xF;
        let mut prefix: u32 = 0;
        while prefix < 16 {
            if (prefix & top4_mask) == top4_match {
                let b = &mut table.buckets[prefix as usize];
                b.indices[b.len as usize] = i as u8;
                b.len += 1;
            }
            prefix += 1;
        }
        i += 1;
    }
    table
}

static PREFIX_TABLE: PrefixTable = build_prefix_table();

/// Info about a non-parallel opcode table entry (for testing/coverage).
pub struct OpcodeInfo {
    pub mask: u32,
    pub match_val: u32,
    pub name: &'static str,
    pub has_decode: bool,
}

/// Returns info for each non-parallel opcode table entry.
/// Used by differential tests to systematically generate valid encodings.
pub fn opcode_templates() -> Vec<OpcodeInfo> {
    OPCODE_TABLE
        .iter()
        .map(|e| OpcodeInfo {
            mask: e.mask,
            match_val: e.match_val,
            name: e.name,
            has_decode: e.decode.is_some(),
        })
        .collect()
}

/// Normalize mode-6 ea_mode don't-care bits for instructions with MMMRRR ea
/// fields. For mode-6 (MMM=110): RRR[1:0] are don't-care, clear them.
/// For P: space ea forms (Movep1, MovepQqPea, PlockEa, PunlockEa, Vsl),
/// RRR[2] is also don't-care (absolute/immediate produce same address), so
/// clear it too.
fn normalize_ea_mode(inst: &mut Instruction) {
    fn norm_lo(ea: &mut u8) {
        if (*ea >> 3) & 7 == 6 {
            *ea &= !0x03;
        }
    }
    fn norm_all(ea: &mut u8) {
        if (*ea >> 3) & 7 == 6 {
            *ea &= !0x07;
        }
    }
    match inst {
        Instruction::Movep1 { ea_mode, .. }
        | Instruction::MovepQqPea { ea_mode, .. }
        | Instruction::PlockEa { ea_mode, .. }
        | Instruction::PunlockEa { ea_mode, .. }
        | Instruction::Vsl { ea_mode, .. }
        | Instruction::MovemEa { ea_mode, .. }
        | Instruction::JmpEa { ea_mode, .. }
        | Instruction::JsrEa { ea_mode, .. }
        | Instruction::JccEa { ea_mode, .. }
        | Instruction::JsccEa { ea_mode, .. }
        | Instruction::DoEa { ea_mode, .. }
        | Instruction::DorEa { ea_mode, .. }
        | Instruction::RepEa { ea_mode, .. }
        | Instruction::BchgEa { ea_mode, .. }
        | Instruction::BclrEa { ea_mode, .. }
        | Instruction::BsetEa { ea_mode, .. }
        | Instruction::BtstEa { ea_mode, .. }
        | Instruction::JclrEa { ea_mode, .. }
        | Instruction::JsetEa { ea_mode, .. }
        | Instruction::JsclrEa { ea_mode, .. }
        | Instruction::JssetEa { ea_mode, .. }
        | Instruction::BrclrEa { ea_mode, .. }
        | Instruction::BrsetEa { ea_mode, .. }
        | Instruction::BsclrEa { ea_mode, .. }
        | Instruction::BssetEa { ea_mode, .. } => norm_all(ea_mode),
        // RRR[1:0] are don't-care, but RRR[2] (abs vs imm) matters for reads.
        // For writes (W=false), immediate is nonsensical so normalize RRR[2] too.
        Instruction::Movep23 { ea_mode, w, .. }
        | Instruction::MovepQq { ea_mode, w, .. }
        | Instruction::MovecEa { ea_mode, w, .. } => {
            if *w {
                norm_lo(ea_mode);
            } else {
                norm_all(ea_mode);
            }
        }
        _ => {}
    }
}

/// Returns true if the instruction references a register index that maps to an
/// empty name in REGISTER_NAMES (reserved/undefined register slots).  The
/// decoder treats such encodings as invalid and falls back to `Unknown`.
fn has_invalid_register(inst: &Instruction) -> bool {
    fn bad(r: u8) -> bool {
        (r as usize) < REGISTER_NAMES.len() && REGISTER_NAMES[r as usize].is_empty()
    }
    match inst {
        Instruction::MovemAa { numreg, .. } | Instruction::MovemEa { numreg, .. } => bad(*numreg),
        Instruction::MovecAa { numreg, .. } | Instruction::MovecEa { numreg, .. } => bad(*numreg),
        Instruction::MovecImm { dest, .. } => bad(*dest),
        Instruction::MovecReg {
            src_reg, dst_reg, ..
        } => bad(*src_reg) || bad(*dst_reg),
        Instruction::MoveLongDisp { numreg, .. } => bad(*numreg),
        Instruction::MoveShortDisp { numreg, .. } => bad(*numreg),
        Instruction::JclrReg { reg_idx, .. }
        | Instruction::JsetReg { reg_idx, .. }
        | Instruction::JsclrReg { reg_idx, .. }
        | Instruction::JssetReg { reg_idx, .. } => bad(*reg_idx),
        Instruction::BrclrReg { reg_idx, .. }
        | Instruction::BrsetReg { reg_idx, .. }
        | Instruction::BsclrReg { reg_idx, .. }
        | Instruction::BssetReg { reg_idx, .. } => bad(*reg_idx),
        Instruction::DoReg { reg_idx, .. }
        | Instruction::DorReg { reg_idx, .. }
        | Instruction::RepReg { reg_idx, .. } => bad(*reg_idx),
        // andi/ori: EE field (dest) 0-3 are valid (mr, ccr, com, eom).
        Instruction::AndI { dest, .. } | Instruction::OrI { dest, .. } => *dest > 3,
        // tcc: REGISTERS_TCC entries 2-7 have NULL register pairs.
        Instruction::Tcc { acc, .. } => matches!(acc, Some((0, _) | (_, 0))),
        // lua: dst_reg uses a 4-bit R/N encoding, bit 4 is don't-care (normalized in decoder).
        // lra: dst_reg indexes REGISTER_NAMES; invalid indices produce empty names.
        Instruction::LraRn { dst_reg, .. } | Instruction::LraDisp { dst_reg, .. } => bad(*dst_reg),
        // bchg/bclr/bset/btst reg: DDDDDD indexes REGISTER_NAMES.
        Instruction::BchgReg { reg_idx, .. }
        | Instruction::BclrReg { reg_idx, .. }
        | Instruction::BsetReg { reg_idx, .. }
        | Instruction::BtstReg { reg_idx, .. } => bad(*reg_idx),
        // movep0/movepQqR: dddddd indexes REGISTER_NAMES.
        Instruction::Movep0 { reg_idx, .. } | Instruction::MovepQqR { reg_idx, .. } => {
            bad(*reg_idx)
        }
        // dmac: ss=01 is reserved per Table 12-16.
        Instruction::Dmac { ss, .. } => *ss == 0b01,
        _ => false,
    }
}

/// Decode a 24-bit DSP56300 instruction word.
pub fn decode(opcode: u32) -> Instruction {
    if opcode >= 0x100000 {
        // Parallel move + ALU instruction
        let move_bits = (opcode >> 20) & 0xF;
        let mut opcode = opcode;

        // Normalize don't-care bits in mode-6 ea_mode for parallel moves.
        // Pm1 always uses bits [13:8] as ea_mode (MMMRRR).
        // Pm4/Pm5 (move_bits 4-7) use them when bit 14 is set (long ea form).
        // Pm8 (move_bits 8-15, XY moves) has a different bit layout.
        let has_standard_ea = match move_bits {
            1 => true,
            4..=7 => opcode & (1 << 14) != 0,
            _ => false,
        };
        if has_standard_ea {
            opcode = normalize_mode6_ea(opcode);
        }

        // For Pm4/Pm5 X:/Y: with mode-6 immediate (RRR=4),
        // the memspace bit (bit 19) is irrelevant; normalize to 0 (X space).
        if (4..=7).contains(&move_bits) && has_standard_ea {
            let mmm = (opcode >> 11) & 7;
            let rrr = (opcode >> 8) & 7;
            let value = ((opcode >> 16) & 7) | ((opcode >> 17) & (3 << 3));
            // X:/Y: case (not L:) with mode-6 immediate
            if value >> 2 != 0 && mmm == 6 && rrr == 4 {
                opcode &= !(1 << 19);
            }
        }

        // Reject PM2 register-to-register moves with invalid register indices.
        if move_bits == 2 || move_bits == 3 {
            let upper = (opcode >> 8) & 0xFFFF;
            let is_reg_to_reg = upper != 0x2000              // not NOP
                && upper & 0xFFF0 != 0x2020                  // not IFcc
                && upper & 0xFFF0 != 0x2030                  // not IFcc.U
                && upper & 0xFFE0 != 0x2040                  // not R update
                && upper & 0xFC00 == 0x2000; // is reg-to-reg
            if is_reg_to_reg {
                let r1 = ((opcode >> 13) & 0x1F) as usize;
                let r2 = ((opcode >> 8) & 0x1F) as usize;
                if (r1 < REGISTER_NAMES.len() && REGISTER_NAMES[r1].is_empty())
                    || (r2 < REGISTER_NAMES.len() && REGISTER_NAMES[r2].is_empty())
                {
                    return Instruction::Unknown { opcode };
                }
            }
        }

        let alu_bits = opcode & 0xFF;
        return Instruction::Parallel {
            alu: decode_parallel_alu(alu_bits as u8),
            move_type: ParallelMoveType::from_bits(move_bits),
            opcode,
        };
    }

    // PM0 (X:R Class II / R:Y Class II): 0000 100d xSmm mrrr aaaa aaaa
    // bits 23:17 = 0000100, bit 14 = 0 (distinguishes from non-parallel movep)
    if (opcode & 0xFE4000) == 0x080000 {
        // Pm0 always writes to ea. Normalize mode-6 don't-care bits.
        // Note: bit 15 in Pm0 is memspace (not W flag), but Pm0 always
        // writes to ea, so mode-6 immediate is always nonsensical.
        let mut opcode = normalize_mode6_ea(opcode);
        if (opcode >> 11) & 7 == 6 {
            opcode &= !0x0400; // force absolute (clear RRR[2])
        }
        return Instruction::Parallel {
            alu: decode_parallel_alu((opcode & 0xFF) as u8),
            move_type: ParallelMoveType::Pm0,
            opcode,
        };
    }

    // Prefix dispatch: use bits 19:16 to select a small bucket of candidates.
    let prefix = ((opcode >> 16) & 0xF) as usize;
    let bucket = &PREFIX_TABLE.buckets[prefix];
    let mut i = 0;
    while i < bucket.len as usize {
        let entry = &OPCODE_TABLE[bucket.indices[i] as usize];
        if (opcode & entry.mask) == entry.match_val {
            if entry.has_mmmrrr && !match_mmmrrr(opcode) {
                i += 1;
                continue;
            }
            return match entry.decode {
                Some(f) => {
                    let mut inst = f(opcode);
                    if has_invalid_register(&inst) {
                        Instruction::Unknown { opcode }
                    } else {
                        normalize_ea_mode(&mut inst);
                        inst
                    }
                }
                None => Instruction::Unimplemented {
                    name: entry.name,
                    opcode,
                },
            };
        }
        i += 1;
    }

    Instruction::Unknown { opcode }
}

/// Returns whether the instruction fetches a second word (2-word instruction).
pub fn instruction_length(inst: &Instruction) -> u32 {
    match inst {
        // 2-word instructions (fetch next word as immediate/address)
        Instruction::AddLong { .. }
        | Instruction::SubLong { .. }
        | Instruction::CmpLong { .. }
        | Instruction::AndLong { .. }
        | Instruction::OrLong { .. }
        | Instruction::EorLong { .. }
        | Instruction::BccLong { .. }
        | Instruction::BraLong
        | Instruction::BsrLong
        | Instruction::MoveLongDisp { .. }
        | Instruction::MpyI { .. }
        | Instruction::MpyrI { .. }
        | Instruction::MacI { .. }
        | Instruction::MacrI { .. }
        | Instruction::DoEa { .. }
        | Instruction::DoAa { .. }
        | Instruction::DoImm { .. }
        | Instruction::DoReg { .. }
        | Instruction::DoForever
        | Instruction::DorEa { .. }
        | Instruction::DorAa { .. }
        | Instruction::DorImm { .. }
        | Instruction::DorReg { .. }
        | Instruction::DorForever
        | Instruction::JclrEa { .. }
        | Instruction::JclrAa { .. }
        | Instruction::JclrPp { .. }
        | Instruction::JclrQq { .. }
        | Instruction::JclrReg { .. }
        | Instruction::JsetEa { .. }
        | Instruction::JsetAa { .. }
        | Instruction::JsetPp { .. }
        | Instruction::JsetQq { .. }
        | Instruction::JsetReg { .. }
        | Instruction::JsclrEa { .. }
        | Instruction::JsclrAa { .. }
        | Instruction::JsclrPp { .. }
        | Instruction::JsclrQq { .. }
        | Instruction::JsclrReg { .. }
        | Instruction::JssetEa { .. }
        | Instruction::JssetAa { .. }
        | Instruction::JssetPp { .. }
        | Instruction::JssetQq { .. }
        | Instruction::JssetReg { .. }
        | Instruction::BrclrEa { .. }
        | Instruction::BrclrAa { .. }
        | Instruction::BrclrPp { .. }
        | Instruction::BrclrQq { .. }
        | Instruction::BrclrReg { .. }
        | Instruction::BrsetEa { .. }
        | Instruction::BrsetAa { .. }
        | Instruction::BrsetPp { .. }
        | Instruction::BrsetQq { .. }
        | Instruction::BrsetReg { .. }
        | Instruction::BsccLong { .. }
        | Instruction::BsclrEa { .. }
        | Instruction::BsclrAa { .. }
        | Instruction::BsclrPp { .. }
        | Instruction::BsclrQq { .. }
        | Instruction::BsclrReg { .. }
        | Instruction::BssetEa { .. }
        | Instruction::BssetAa { .. }
        | Instruction::BssetPp { .. }
        | Instruction::BssetQq { .. }
        | Instruction::BssetReg { .. }
        | Instruction::LraDisp { .. }
        | Instruction::ExtractImm { .. }
        | Instruction::ExtractuImm { .. }
        | Instruction::InsertImm { .. }
        | Instruction::Plockr
        | Instruction::Punlockr => 2,

        // Conditionally 2-word: EA-based instructions where mode 6 (absolute
        // address) uses the second word. MMM field is at bits 13:11 of opcode.
        Instruction::Movep23 { ea_mode, .. }
        | Instruction::Movep1 { ea_mode, .. }
        | Instruction::MovepQq { ea_mode, .. }
        | Instruction::MovepQqPea { ea_mode, .. }
        | Instruction::MovecEa { ea_mode, .. }
        | Instruction::MovemEa { ea_mode, .. }
            if (ea_mode >> 3) == 6 =>
        {
            2
        }
        Instruction::JmpEa { ea_mode }
        | Instruction::JsrEa { ea_mode }
        | Instruction::JccEa { ea_mode, .. }
        | Instruction::JsccEa { ea_mode, .. }
        | Instruction::BclrEa { ea_mode, .. }
        | Instruction::BsetEa { ea_mode, .. }
        | Instruction::BtstEa { ea_mode, .. }
        | Instruction::BchgEa { ea_mode, .. }
        | Instruction::PlockEa { ea_mode }
        | Instruction::PunlockEa { ea_mode }
        | Instruction::Vsl { ea_mode, .. }
            if (ea_mode >> 3) == 6 =>
        {
            2
        }

        // Parallel instructions with mode 6 (absolute/immediate from extension word)
        Instruction::Parallel {
            opcode, move_type, ..
        } => {
            let ea_mode6 = |op: u32| (op >> 11) & 7 == 6;
            let pm5_mode6 = |op: u32| (op >> 14) & 1 == 1 && ea_mode6(op);
            match move_type {
                // Pm0 and Pm1 always use bits [13:11] as EA mode
                ParallelMoveType::Pm0 | ParallelMoveType::Pm1 if ea_mode6(*opcode) => 2,
                ParallelMoveType::Pm5 if pm5_mode6(*opcode) => 2,
                // Pm4 covers both L: moves (0100_x0xx) and X:/Y: moves; both support mode 6
                ParallelMoveType::Pm4 if pm5_mode6(*opcode) => 2,
                _ => 1,
            }
        }

        // Everything else is 1 word
        _ => 1,
    }
}
