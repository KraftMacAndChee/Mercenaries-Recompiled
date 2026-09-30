//! Disassembler for DSP56300 instructions.
//!
//! Produces human-readable assembly text in the format described in the
//! DSP56300 Family Manual.

use std::collections::HashMap;
use std::fmt::Write;

use dsp56300_core::{
    Accumulator, CC_NAMES, Instruction, MemSpace, MulShiftOp, ParallelAlu, ParallelMoveType,
    REGISTER_NAMES, REGISTERS_LMOVE, decode, reg,
};

/// Symbol table mapping P-space addresses to label names.
pub type SymbolTable = HashMap<u32, String>;

fn reg_name(r: usize) -> &'static str {
    if r < 64 { REGISTER_NAMES[r] } else { "?" }
}

fn acc_name(d: usize) -> &'static str {
    reg_name(if d == 0 { reg::A } else { reg::B })
}

fn signextend(bits: u32, v: u32) -> i32 {
    let shift = 32 - bits;
    ((v as i32) << shift) >> shift
}

/// Returns `"#>"` when `value` fits in the short-immediate range, signaling the
/// assembler to preserve the long encoding.  Returns `"#"` when the value
/// exceeds `short_max` (the assembler must use long anyway).
fn imm_prefix(value: u32, short_max: u32) -> &'static str {
    if value <= short_max { "#>" } else { "#" }
}

/// Returns `">"` when a 24-bit branch displacement fits in the 9-bit signed
/// range used by the short form (-256..=255), signaling force-long.
fn branch_long_prefix(disp24: u32) -> &'static str {
    let s = if disp24 & 0x800000 != 0 {
        disp24 as i32 | !0xFFFFFF
    } else {
        disp24 as i32
    };
    if (-256..=255).contains(&s) { ">" } else { "" }
}

/// Returns `">"` when an EA-mode-6 jump/jsr target address falls in a range
/// where the assembler would pick the short 12-bit form (< 0x1000).
fn jump_ea_addr_prefix(ea_mode: u32, next_word: u32) -> &'static str {
    let mode = (ea_mode >> 3) & 7;
    let is_abs = mode == 6 && (ea_mode >> 2) & 1 == 0;
    if is_abs && next_word < 0x1000 {
        ">"
    } else {
        ""
    }
}

/// Returns `">"` when an ea-mode-6 absolute address falls in a range where
/// the assembler would pick a shorter encoding (aa/pp/qq), signaling it to
/// preserve the long EA encoding instead.
fn ea_addr_prefix(ea_mode: u32, next_word: u32) -> &'static str {
    let mode = (ea_mode >> 3) & 7;
    let is_abs = mode == 6 && (ea_mode >> 2) & 1 == 0;
    if is_abs
        && (next_word <= 0x3F
            || ((0xFFFF80..=0xFFFFBF).contains(&next_word))
            || next_word >= 0xFFFFC0)
    {
        ">"
    } else {
        ""
    }
}

/// Format a signed displacement as `+N` or `-N`.
fn fmt_disp(val: i32) -> String {
    if val >= 0 {
        format!("+{}", val)
    } else {
        format!("-{}", -val)
    }
}

/// Compute an effective address string from a 6-bit ea_mode field.
/// Returns `(ea_string, is_immediate, consumed_extra_word)`.
fn calc_ea(ea_mode: u32, next_word: u32) -> (String, bool, bool) {
    let value = (ea_mode >> 3) & 7;
    let numreg = (ea_mode & 7) as usize;
    match value {
        0 => (format!("(r{})-n{}", numreg, numreg), false, false),
        1 => (format!("(r{})+n{}", numreg, numreg), false, false),
        2 => (format!("(r{})-", numreg), false, false),
        3 => (format!("(r{})+", numreg), false, false),
        4 => (format!("(r{})", numreg), false, false),
        5 => (format!("(r{}+n{})", numreg, numreg), false, false),
        6 => {
            if (ea_mode >> 2) & 1 == 0 {
                // Absolute address
                (format!("${:04x}", next_word), false, true)
            } else {
                // Immediate value
                (format!("${:06x}", next_word), true, true)
            }
        }
        7 => (format!("-(r{})", numreg), false, false),
        _ => unreachable!(),
    }
}

fn space_name(s: u32) -> char {
    if s != 0 { 'y' } else { 'x' }
}

fn space_char(s: MemSpace) -> char {
    if s == MemSpace::Y { 'y' } else { 'x' }
}

fn pp_addr(offset: u32) -> u32 {
    0xFFFFC0 + offset
}

fn qq_addr(offset: u32) -> u32 {
    0xFFFF80 + offset
}

/// Check if an EA mode is absolute (mode 6, non-immediate: 110_0xx).
fn is_ea_absolute(ea_mode: u32) -> bool {
    let mode = (ea_mode >> 3) & 7;
    mode == 6 && (ea_mode >> 2) & 1 == 0
}

/// Format a P-space target address, using a symbol name if available.
fn fmt_target(addr: u32, symbols: &SymbolTable) -> String {
    if let Some(name) = symbols.get(&addr) {
        name.clone()
    } else {
        format!("${:04x}", addr)
    }
}

/// Format a P-space target address (6-digit hex), using a symbol name if available.
fn fmt_target6(addr: u32, symbols: &SymbolTable) -> String {
    if let Some(name) = symbols.get(&addr) {
        name.clone()
    } else {
        format!("${:06x}", addr)
    }
}

/// Disassemble a single instruction.
///
/// - `pc`: program counter of the instruction
/// - `opcode`: the 24-bit instruction word
/// - `next_word`: the second word (for 2-word instructions), or 0 if unavailable
///
/// Returns `(text, instruction_length)` where `instruction_length` is 1 or 2.
pub fn disassemble(pc: u32, opcode: u32, next_word: u32) -> (String, u32) {
    static EMPTY: std::sync::LazyLock<SymbolTable> = std::sync::LazyLock::new(SymbolTable::new);
    disassemble_with_symbols(pc, opcode, next_word, &EMPTY)
}

/// Disassemble a single instruction, resolving P-space target addresses to
/// symbol names when possible.
pub fn disassemble_with_symbols(
    pc: u32,
    opcode: u32,
    next_word: u32,
    symbols: &SymbolTable,
) -> (String, u32) {
    let inst = decode::decode(opcode);
    let mut out = String::new();
    let mut len: u32 = 1;

    match inst {
        Instruction::Parallel {
            alu,
            move_type,
            opcode: op,
        } => {
            if matches!(alu, ParallelAlu::Undefined) {
                write!(out, "dc ${:06x}", opcode).unwrap();
            } else {
                let pm = disasm_parallel_move(move_type, op, next_word);
                if pm.extra_word {
                    len = 2;
                }
                if pm.text.is_empty() {
                    write!(out, "{alu}").unwrap();
                } else {
                    write!(out, "{alu} {}", pm.text).unwrap();
                }
            }
        }

        // Arithmetic with immediate
        Instruction::AddImm { imm, d } => {
            write!(out, "add #${:02x},{}", imm, acc_name(d as usize)).unwrap();
        }
        Instruction::AddLong { d } => {
            len = 2;
            write!(
                out,
                "add {}${:04x},{}",
                imm_prefix(next_word, 0x3F),
                next_word,
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::SubImm { imm, d } => {
            write!(out, "sub #${:02x},{}", imm, acc_name(d as usize)).unwrap();
        }
        Instruction::SubLong { d } => {
            len = 2;
            write!(
                out,
                "sub {}${:06x},{}",
                imm_prefix(next_word, 0x3F),
                next_word,
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::CmpImm { imm, d } => {
            write!(out, "cmp #${:02x},{}", imm, acc_name(d as usize)).unwrap();
        }
        Instruction::CmpLong { d } => {
            len = 2;
            write!(
                out,
                "cmp {}${:06x},{}",
                imm_prefix(next_word, 0x3F),
                next_word,
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::CmpU { src, d } => {
            let dstacc = if d == Accumulator::A { reg::A } else { reg::B };
            write!(out, "cmpu {},{}", reg_name(src), reg_name(dstacc)).unwrap();
        }
        Instruction::AndImm { imm, d } => {
            write!(out, "and #${:02x},{}", imm, acc_name(d as usize)).unwrap();
        }
        Instruction::AndLong { d } => {
            len = 2;
            write!(
                out,
                "and {}${:04x},{}",
                imm_prefix(next_word, 0x3F),
                next_word,
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::OrImm { imm, d } => {
            write!(out, "or #${:02x},{}", imm, acc_name(d as usize)).unwrap();
        }
        Instruction::OrLong { d } => {
            len = 2;
            write!(
                out,
                "or {}${:04x},{}",
                imm_prefix(next_word, 0x3F),
                next_word,
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::EorImm { imm, d } => {
            write!(out, "eor #${:02x},{}", imm, acc_name(d as usize)).unwrap();
        }
        Instruction::EorLong { d } => {
            len = 2;
            write!(
                out,
                "eor {}${:04x},{}",
                imm_prefix(next_word, 0x3F),
                next_word,
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::AndI { imm, dest } => {
            let dest_name = match dest {
                0 => "mr",
                1 => "ccr",
                2 => "com",
                3 => "eom",
                _ => "?",
            };
            write!(out, "andi #${:02x},{}", imm, dest_name).unwrap();
        }
        Instruction::OrI { imm, dest } => {
            let dest_name = match dest {
                0 => "mr",
                1 => "ccr",
                2 => "com",
                3 => "eom",
                _ => "?",
            };
            write!(out, "ori #${:02x},{}", imm, dest_name).unwrap();
        }

        // Shifts
        Instruction::AslImm { shift, s, d } => {
            write!(
                out,
                "asl #${:02x},{},{}",
                shift,
                acc_name(s as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::AsrImm { shift, s, d } => {
            write!(
                out,
                "asr #${:02x},{},{}",
                shift,
                acc_name(s as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::LslImm { shift, d } => {
            write!(out, "lsl #${:02x},{}", shift, acc_name(d as usize)).unwrap();
        }
        Instruction::LsrImm { shift, d } => {
            write!(out, "lsr #${:02x},{}", shift, acc_name(d as usize)).unwrap();
        }
        Instruction::AslReg { src, s, d } => {
            write!(
                out,
                "asl {},{},{}",
                reg_name(src),
                acc_name(s as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::AsrReg { src, s, d } => {
            write!(
                out,
                "asr {},{},{}",
                reg_name(src),
                acc_name(s as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::LslReg { src, d } => {
            write!(out, "lsl {},{}", reg_name(src), acc_name(d as usize)).unwrap();
        }
        Instruction::LsrReg { src, d } => {
            write!(out, "lsr {},{}", reg_name(src), acc_name(d as usize)).unwrap();
        }

        // Branches
        Instruction::Bcc { cc, addr } => {
            let target = (pc as i32 + addr) as u32 & 0xFFFFFF;
            write!(
                out,
                "b{} {}",
                CC_NAMES[cc as usize],
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BccLong { cc } => {
            len = 2;
            let prefix = branch_long_prefix(next_word);
            let target = (pc.wrapping_add(next_word)) & 0xFFFFFF;
            write!(
                out,
                "b{} {}{}",
                CC_NAMES[cc as usize],
                prefix,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::Bra { addr } => {
            let target = (pc as i32 + addr) as u32 & 0xFFFFFF;
            write!(out, "bra {}", fmt_target6(target, symbols)).unwrap();
        }
        Instruction::BraLong => {
            len = 2;
            let prefix = branch_long_prefix(next_word);
            let target = (pc.wrapping_add(next_word)) & 0xFFFFFF;
            write!(out, "bra {}{}", prefix, fmt_target6(target, symbols)).unwrap();
        }
        Instruction::Bsr { addr } => {
            let target = (pc as i32 + addr) as u32 & 0xFFFFFF;
            write!(out, "bsr {}", fmt_target6(target, symbols)).unwrap();
        }
        Instruction::BsrLong => {
            len = 2;
            let prefix = branch_long_prefix(next_word);
            let target = (pc.wrapping_add(next_word)) & 0xFFFFFF;
            write!(out, "bsr {}{}", prefix, fmt_target6(target, symbols)).unwrap();
        }
        Instruction::BccRn { cc, rn } => {
            write!(out, "b{} r{}", CC_NAMES[cc as usize], rn).unwrap();
        }
        Instruction::BraRn { rn } => {
            write!(out, "bra r{}", rn).unwrap();
        }
        Instruction::BsrRn { rn } => {
            write!(out, "bsr r{}", rn).unwrap();
        }
        Instruction::Bscc { cc, addr } => {
            let target = (pc as i32 + addr) as u32 & 0xFFFFFF;
            write!(
                out,
                "bs{} {}",
                CC_NAMES[cc as usize],
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BsccLong { cc } => {
            len = 2;
            let prefix = branch_long_prefix(next_word);
            let target = (pc.wrapping_add(next_word)) & 0xFFFFFF;
            write!(
                out,
                "bs{} {}{}",
                CC_NAMES[cc as usize],
                prefix,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BsccRn { cc, rn } => {
            write!(out, "bs{} r{}", CC_NAMES[cc as usize], rn).unwrap();
        }
        Instruction::Brkcc { cc } => {
            write!(out, "brk{}", CC_NAMES[cc as usize]).unwrap();
        }
        Instruction::Jcc { cc, addr } => {
            write!(
                out,
                "j{} {}",
                CC_NAMES[cc as usize],
                fmt_target(addr, symbols)
            )
            .unwrap();
        }
        Instruction::JccEa { cc, ea_mode } => {
            let prefix = jump_ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            let target = if extra && is_ea_absolute(ea_mode as u32) {
                fmt_target(next_word, symbols)
            } else {
                addr
            };
            write!(out, "j{} {}{}", CC_NAMES[cc as usize], prefix, target).unwrap();
        }
        Instruction::Jmp { addr } => {
            write!(out, "jmp {}", fmt_target(addr, symbols)).unwrap();
        }
        Instruction::JmpEa { ea_mode } => {
            let prefix = jump_ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            let target = if extra && is_ea_absolute(ea_mode as u32) {
                fmt_target(next_word, symbols)
            } else {
                addr
            };
            write!(out, "jmp {}{}", prefix, target).unwrap();
        }
        Instruction::Jscc { cc, addr } => {
            write!(
                out,
                "js{} {}",
                CC_NAMES[cc as usize],
                fmt_target(addr, symbols)
            )
            .unwrap();
        }
        Instruction::JsccEa { cc, ea_mode } => {
            let prefix = jump_ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            let target = if extra && is_ea_absolute(ea_mode as u32) {
                fmt_target(next_word, symbols)
            } else {
                addr
            };
            write!(out, "js{} {}{}", CC_NAMES[cc as usize], prefix, target).unwrap();
        }
        Instruction::Jsr { addr } => {
            write!(out, "jsr {}", fmt_target(addr, symbols)).unwrap();
        }
        Instruction::JsrEa { ea_mode } => {
            let prefix = jump_ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            let target = if extra && is_ea_absolute(ea_mode as u32) {
                fmt_target(next_word, symbols)
            } else {
                addr
            };
            write!(out, "jsr {}{}", prefix, target).unwrap();
        }

        // Bit manipulation (bchg/bclr/bset/btst with ea/aa/pp/reg variants)
        Instruction::BchgEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::BclrEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::BsetEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::BtstEa {
            space,
            ea_mode,
            bit_num,
        } => {
            let mnemonic = bit_mnemonic(&inst);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            let sc = space_char(space);
            write!(out, "{} #{},{}:{}{}", mnemonic, bit_num, sc, prefix, addr).unwrap();
        }
        Instruction::BchgAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::BclrAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::BsetAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::BtstAa {
            space,
            addr,
            bit_num,
        } => {
            let mnemonic = bit_mnemonic(&inst);
            let sc = space_char(space);
            write!(out, "{} #{},{}:${:04x}", mnemonic, bit_num, sc, addr).unwrap();
        }
        Instruction::BchgPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::BclrPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::BsetPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::BtstPp {
            space,
            pp_offset,
            bit_num,
        } => {
            let mnemonic = bit_mnemonic(&inst);
            let pp_addr = pp_addr(pp_offset as u32);
            let sc = space_char(space);
            write!(out, "{} #{},{}:${:06x}", mnemonic, bit_num, sc, pp_addr).unwrap();
        }
        Instruction::BchgQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::BclrQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::BsetQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::BtstQq {
            space,
            qq_offset,
            bit_num,
        } => {
            let mnemonic = bit_mnemonic(&inst);
            let qq_addr = qq_addr(qq_offset as u32);
            let sc = space_char(space);
            write!(out, "{} #{},{}:${:06x}", mnemonic, bit_num, sc, qq_addr).unwrap();
        }
        Instruction::BchgReg { reg_idx, bit_num }
        | Instruction::BclrReg { reg_idx, bit_num }
        | Instruction::BsetReg { reg_idx, bit_num }
        | Instruction::BtstReg { reg_idx, bit_num } => {
            let mnemonic = bit_mnemonic(&inst);
            write!(
                out,
                "{} #{},{}",
                mnemonic,
                bit_num,
                reg_name(reg_idx as usize)
            )
            .unwrap();
        }

        // Bit branch instructions (jclr/jset/jsclr/jsset with ea/aa/pp/reg)
        Instruction::JclrEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::JsetEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::JsclrEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::JssetEa {
            space,
            ea_mode,
            bit_num,
        } => {
            len = 2;
            let mnemonic = bitbranch_mnemonic(&inst);
            let sc = space_char(space);
            let ea_val = (ea_mode >> 3) & 7;
            if ea_val == 6 {
                // Mode 6: next_word serves as both EA address and jump target.
                let is_imm = (ea_mode >> 2) & 1 == 1;
                if is_imm {
                    // Immediate mode (110_100): test bit of immediate value.
                    write!(
                        out,
                        "{} #{},{}:#${:06x},{}",
                        mnemonic,
                        bit_num,
                        sc,
                        next_word,
                        fmt_target(next_word, symbols)
                    )
                } else {
                    // Absolute address mode (110_000): test bit of memory.
                    let prefix = if next_word <= 0x3F
                        || ((0xFFFF80..=0xFFFFBF).contains(&next_word))
                        || next_word >= 0xFFFFC0
                    {
                        ">"
                    } else {
                        ""
                    };
                    write!(
                        out,
                        "{} #{},{}:{}${:04x},{}",
                        mnemonic,
                        bit_num,
                        sc,
                        prefix,
                        next_word,
                        fmt_target(next_word, symbols)
                    )
                }
                .unwrap();
            } else {
                let (addr, _, _) = calc_ea(ea_mode as u32, next_word);
                write!(
                    out,
                    "{} #{},{}:{},{}",
                    mnemonic,
                    bit_num,
                    sc,
                    addr,
                    fmt_target(next_word, symbols)
                )
                .unwrap();
            }
        }
        Instruction::JclrAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::JsetAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::JsclrAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::JssetAa {
            space,
            addr,
            bit_num,
        } => {
            len = 2;
            let mnemonic = bitbranch_mnemonic(&inst);
            let sc = space_char(space);
            write!(
                out,
                "{} #{},{}:${:04x},{}",
                mnemonic,
                bit_num,
                sc,
                addr,
                fmt_target(next_word, symbols)
            )
            .unwrap();
        }
        Instruction::JclrPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::JsetPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::JsclrPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::JssetPp {
            space,
            pp_offset,
            bit_num,
        } => {
            len = 2;
            let mnemonic = bitbranch_mnemonic(&inst);
            let pp_addr = pp_addr(pp_offset as u32);
            let sc = space_char(space);
            write!(
                out,
                "{} #{},{}:${:06x},{}",
                mnemonic,
                bit_num,
                sc,
                pp_addr,
                fmt_target(next_word, symbols)
            )
            .unwrap();
        }
        Instruction::JclrQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::JsetQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::JsclrQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::JssetQq {
            space,
            qq_offset,
            bit_num,
        } => {
            len = 2;
            let mnemonic = bitbranch_mnemonic(&inst);
            let qq_addr = qq_addr(qq_offset as u32);
            let sc = space_char(space);
            write!(
                out,
                "{} #{},{}:${:06x},{}",
                mnemonic,
                bit_num,
                sc,
                qq_addr,
                fmt_target(next_word, symbols)
            )
            .unwrap();
        }
        Instruction::JclrReg { reg_idx, bit_num }
        | Instruction::JsetReg { reg_idx, bit_num }
        | Instruction::JsclrReg { reg_idx, bit_num }
        | Instruction::JssetReg { reg_idx, bit_num } => {
            len = 2;
            let mnemonic = bitbranch_mnemonic(&inst);
            write!(
                out,
                "{} #{},{},{}",
                mnemonic,
                bit_num,
                reg_name(reg_idx as usize),
                fmt_target(next_word, symbols)
            )
            .unwrap();
        }

        // Relative bit branch
        Instruction::BrclrEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::BrsetEa {
            space,
            ea_mode,
            bit_num,
        } => {
            len = 2;
            let mnemonic = relbranch_mnemonic(&inst);
            let sc = space_char(space);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, _) = calc_ea(ea_mode as u32, next_word);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:{}{},{}",
                mnemonic,
                bit_num,
                sc,
                prefix,
                addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BrclrAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::BrsetAa {
            space,
            addr,
            bit_num,
        } => {
            len = 2;
            let mnemonic = relbranch_mnemonic(&inst);
            let sc = space_char(space);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:${:04x},{}",
                mnemonic,
                bit_num,
                sc,
                addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BrclrPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::BrsetPp {
            space,
            pp_offset,
            bit_num,
        } => {
            len = 2;
            let mnemonic = relbranch_mnemonic(&inst);
            let pp_addr = pp_addr(pp_offset as u32);
            let sc = space_char(space);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:${:06x},{}",
                mnemonic,
                bit_num,
                sc,
                pp_addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BrclrQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::BrsetQq {
            space,
            qq_offset,
            bit_num,
        } => {
            len = 2;
            let mnemonic = relbranch_mnemonic(&inst);
            let qq_addr = qq_addr(qq_offset as u32);
            let sc = space_char(space);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:${:06x},{}",
                mnemonic,
                bit_num,
                sc,
                qq_addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BrclrReg { reg_idx, bit_num } | Instruction::BrsetReg { reg_idx, bit_num } => {
            len = 2;
            let mnemonic = relbranch_mnemonic(&inst);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{},{}",
                mnemonic,
                bit_num,
                reg_name(reg_idx as usize),
                fmt_target(target, symbols)
            )
            .unwrap();
        }

        // Bit branch to subroutine (bsclr/bsset)
        Instruction::BsclrEa {
            space,
            ea_mode,
            bit_num,
        }
        | Instruction::BssetEa {
            space,
            ea_mode,
            bit_num,
        } => {
            len = 2;
            let mnemonic = subr_branch_mnemonic(&inst);
            let sc = space_char(space);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, _) = calc_ea(ea_mode as u32, next_word);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:{}{},{}",
                mnemonic,
                bit_num,
                sc,
                prefix,
                addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BsclrAa {
            space,
            addr,
            bit_num,
        }
        | Instruction::BssetAa {
            space,
            addr,
            bit_num,
        } => {
            len = 2;
            let mnemonic = subr_branch_mnemonic(&inst);
            let sc = space_char(space);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:${:04x},{}",
                mnemonic,
                bit_num,
                sc,
                addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BsclrPp {
            space,
            pp_offset,
            bit_num,
        }
        | Instruction::BssetPp {
            space,
            pp_offset,
            bit_num,
        } => {
            len = 2;
            let mnemonic = subr_branch_mnemonic(&inst);
            let pp_addr = pp_addr(pp_offset as u32);
            let sc = space_char(space);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:${:06x},{}",
                mnemonic,
                bit_num,
                sc,
                pp_addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BsclrQq {
            space,
            qq_offset,
            bit_num,
        }
        | Instruction::BssetQq {
            space,
            qq_offset,
            bit_num,
        } => {
            len = 2;
            let mnemonic = subr_branch_mnemonic(&inst);
            let qq_addr = qq_addr(qq_offset as u32);
            let sc = space_char(space);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{}:${:06x},{}",
                mnemonic,
                bit_num,
                sc,
                qq_addr,
                fmt_target6(target, symbols)
            )
            .unwrap();
        }
        Instruction::BsclrReg { reg_idx, bit_num } | Instruction::BssetReg { reg_idx, bit_num } => {
            len = 2;
            let mnemonic = subr_branch_mnemonic(&inst);
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(
                out,
                "{} #{},{},{}",
                mnemonic,
                bit_num,
                reg_name(reg_idx as usize),
                fmt_target(target, symbols)
            )
            .unwrap();
        }

        // Loop target is LA+1 (first addr after loop body)
        Instruction::DoEa { space, ea_mode } => {
            len = 2;
            let sc = space_char(space);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, _) = calc_ea(ea_mode as u32, next_word);
            let end = (next_word + 1) & 0xFFFFFF;
            write!(
                out,
                "do {}:{}{},{}",
                sc,
                prefix,
                addr,
                fmt_target(end, symbols)
            )
            .unwrap();
        }
        Instruction::DoAa { space, addr } => {
            len = 2;
            let sc = space_char(space);
            let end = (next_word + 1) & 0xFFFFFF;
            write!(out, "do {}:${:04x},{}", sc, addr, fmt_target(end, symbols)).unwrap();
        }
        Instruction::DoImm { count } => {
            len = 2;
            let end = (next_word + 1) & 0xFFFFFF;
            write!(out, "do #${:04x},{}", count, fmt_target(end, symbols)).unwrap();
        }
        Instruction::DoReg { reg_idx } => {
            len = 2;
            let end = (next_word + 1) & 0xFFFFFF;
            write!(
                out,
                "do {},{}",
                reg_name(reg_idx as usize),
                fmt_target(end, symbols)
            )
            .unwrap();
        }
        Instruction::DoForever => {
            len = 2;
            let end = (next_word + 1) & 0xFFFFFF;
            write!(out, "do forever,{}", fmt_target(end, symbols)).unwrap();
        }
        Instruction::DorEa { space, ea_mode } => {
            len = 2;
            let sc = space_char(space);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, _) = calc_ea(ea_mode as u32, next_word);
            let target = (pc.wrapping_add(next_word).wrapping_add(1)) & 0xFFFFFF;
            write!(
                out,
                "dor {}:{}{},{}",
                sc,
                prefix,
                addr,
                fmt_target(target, symbols)
            )
            .unwrap();
        }
        Instruction::DorAa { space, addr } => {
            len = 2;
            let sc = space_char(space);
            let target = (pc.wrapping_add(next_word).wrapping_add(1)) & 0xFFFFFF;
            write!(
                out,
                "dor {}:${:04x},{}",
                sc,
                addr,
                fmt_target(target, symbols)
            )
            .unwrap();
        }
        Instruction::DorImm { count } => {
            len = 2;
            let target = (pc.wrapping_add(next_word).wrapping_add(1)) & 0xFFFFFF;
            write!(out, "dor #${:04x},{}", count, fmt_target(target, symbols)).unwrap();
        }
        Instruction::DorReg { reg_idx } => {
            len = 2;
            let target = (pc.wrapping_add(next_word).wrapping_add(1)) & 0xFFFFFF;
            write!(
                out,
                "dor {},{}",
                reg_name(reg_idx as usize),
                fmt_target(target, symbols)
            )
            .unwrap();
        }
        Instruction::DorForever => {
            len = 2;
            let target = (pc.wrapping_add(next_word).wrapping_add(1)) & 0xFFFFFF;
            write!(out, "dor forever,{}", fmt_target(target, symbols)).unwrap();
        }
        Instruction::EndDo => {
            write!(out, "enddo").unwrap();
        }
        Instruction::RepEa { space, ea_mode } => {
            let sc = space_char(space);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            write!(out, "rep {}:{}{}", sc, prefix, addr).unwrap();
        }
        Instruction::RepAa { space, addr } => {
            let sc = space_char(space);
            write!(out, "rep {}:${:04x}", sc, addr).unwrap();
        }
        Instruction::RepImm { count } => {
            write!(out, "rep #${:02x}", count).unwrap();
        }
        Instruction::RepReg { reg_idx } => {
            write!(out, "rep {}", reg_name(reg_idx as usize)).unwrap();
        }

        // Move
        Instruction::MoveLongDisp {
            space,
            w,
            offreg_idx,
            numreg,
        } => {
            len = 2;
            let sp = space_char(space);
            let offreg = reg::R0 + offreg_idx as usize;
            let numreg = numreg as usize;
            let xxxx_s = signextend(24, next_word);
            let prefix = if (-64..=63).contains(&xxxx_s) {
                ">"
            } else {
                ""
            };
            let disp = fmt_disp(xxxx_s);
            if w {
                write!(
                    out,
                    "move {}:{}({}{}),{}",
                    sp,
                    prefix,
                    reg_name(offreg),
                    disp,
                    reg_name(numreg)
                )
                .unwrap();
            } else {
                write!(
                    out,
                    "move {},{}:{}({}{})",
                    reg_name(numreg),
                    sp,
                    prefix,
                    reg_name(offreg),
                    disp
                )
                .unwrap();
            }
        }
        Instruction::MoveShortDisp {
            space,
            offset,
            w,
            offreg_idx,
            numreg,
        } => {
            let space_c = space_char(space);
            let offreg = reg::R0 + offreg_idx as usize;
            let numreg = numreg as usize;
            let xxx_s = signextend(7, offset as u32);
            let disp = fmt_disp(xxx_s);
            if w {
                write!(
                    out,
                    "move {}:({}{}),{}",
                    space_c,
                    reg_name(offreg),
                    disp,
                    reg_name(numreg)
                )
                .unwrap();
            } else {
                write!(
                    out,
                    "move {},{}:({}{})",
                    reg_name(numreg),
                    space_c,
                    reg_name(offreg),
                    disp
                )
                .unwrap();
            }
        }
        Instruction::MovecReg {
            src_reg,
            dst_reg,
            w,
        } => {
            if w {
                write!(
                    out,
                    "movec {},{}",
                    reg_name(src_reg as usize),
                    reg_name(dst_reg as usize)
                )
                .unwrap();
            } else {
                write!(
                    out,
                    "movec {},{}",
                    reg_name(dst_reg as usize),
                    reg_name(src_reg as usize)
                )
                .unwrap();
            }
        }
        Instruction::MovecAa {
            addr,
            numreg,
            w,
            space,
        } => {
            let rn = reg_name(numreg as usize);
            let s = space_char(space);
            if w {
                write!(out, "movec {}:${:04x},{}", s, addr, rn).unwrap();
            } else {
                write!(out, "movec {},{}:${:04x}", rn, s, addr).unwrap();
            }
        }
        Instruction::MovecEa {
            ea_mode,
            numreg,
            w,
            space,
        } => {
            let s = space_char(space);
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, is_imm, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            if w {
                if is_imm {
                    write!(
                        out,
                        "movec {}{},{}",
                        imm_prefix(next_word, 0xFF),
                        addr,
                        reg_name(numreg as usize)
                    )
                    .unwrap();
                } else {
                    write!(
                        out,
                        "movec {}:{}{},{}",
                        s,
                        prefix,
                        addr,
                        reg_name(numreg as usize)
                    )
                    .unwrap();
                }
            } else {
                write!(
                    out,
                    "movec {},{}:{}{}",
                    reg_name(numreg as usize),
                    s,
                    prefix,
                    addr
                )
                .unwrap();
            }
        }
        Instruction::MovecImm { imm, dest } => {
            write!(out, "movec #${:02x},{}", imm, reg_name(dest as usize)).unwrap();
        }
        Instruction::MovemEa { ea_mode, numreg, w } => {
            let prefix = ea_addr_prefix(ea_mode as u32, next_word);
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            if w {
                write!(
                    out,
                    "movem p:{}{},{}",
                    prefix,
                    addr,
                    reg_name(numreg as usize)
                )
                .unwrap();
            } else {
                write!(
                    out,
                    "movem {},p:{}{}",
                    reg_name(numreg as usize),
                    prefix,
                    addr
                )
                .unwrap();
            }
        }
        Instruction::MovemAa { addr, numreg, w } => {
            if w {
                write!(out, "movem p:${:04x},{}", addr, reg_name(numreg as usize)).unwrap();
            } else {
                write!(out, "movem {},p:${:04x}", reg_name(numreg as usize), addr).unwrap();
            }
        }
        Instruction::Movep23 {
            pp_offset,
            ea_mode,
            w,
            perspace,
            easpace,
        } => {
            let addr = pp_addr(pp_offset as u32);
            let sc_per = space_char(perspace);
            let sc_ea = space_char(easpace);
            let (ea_str, is_imm, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            if w {
                let src = if is_imm {
                    format!("#{}", ea_str)
                } else {
                    format!("{}:{}", sc_ea, ea_str)
                };
                write!(out, "movep {},{}:${:06x}", src, sc_per, addr).unwrap();
            } else {
                write!(out, "movep {}:${:06x},{}:{}", sc_per, addr, sc_ea, ea_str).unwrap();
            }
        }
        Instruction::MovepQq {
            qq_offset,
            ea_mode,
            w,
            qqspace,
            easpace,
        } => {
            let addr = qq_addr(qq_offset as u32);
            let sc_qq = space_char(qqspace);
            let sc_ea = space_char(easpace);
            let (ea_str, is_imm, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            if w {
                let src = if is_imm {
                    format!("#{}", ea_str)
                } else {
                    format!("{}:{}", sc_ea, ea_str)
                };
                write!(out, "movep {},{}:${:04x}", src, sc_qq, addr).unwrap();
            } else {
                write!(out, "movep {}:${:04x},{}:{}", sc_qq, addr, sc_ea, ea_str).unwrap();
            }
        }
        Instruction::Movep1 {
            pp_offset,
            ea_mode,
            w,
            space,
        } => {
            let addr = pp_addr(pp_offset as u32);
            let sc = space_char(space);
            let (ea_str, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            if w {
                write!(out, "movep p:{},{}:${:06x}", ea_str, sc, addr).unwrap();
            } else {
                write!(out, "movep {}:${:06x},p:{}", sc, addr, ea_str).unwrap();
            }
        }
        Instruction::Movep0 {
            pp_offset,
            reg_idx,
            w,
            space,
        } => {
            let addr = pp_addr(pp_offset as u32);
            let sc = space_char(space);
            if w {
                write!(
                    out,
                    "movep {},{}:${:06x}",
                    reg_name(reg_idx as usize),
                    sc,
                    addr
                )
                .unwrap();
            } else {
                write!(
                    out,
                    "movep {}:${:06x},{}",
                    sc,
                    addr,
                    reg_name(reg_idx as usize)
                )
                .unwrap();
            }
        }
        Instruction::MovepQqPea {
            qq_offset,
            ea_mode,
            w,
            space,
        } => {
            let addr = qq_addr(qq_offset as u32);
            let sc = space_char(space);
            let (ea_str, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            if w {
                write!(out, "movep p:{},{}:${:04x}", ea_str, sc, addr).unwrap();
            } else {
                write!(out, "movep {}:${:04x},p:{}", sc, addr, ea_str).unwrap();
            }
        }
        Instruction::MovepQqR {
            qq_offset,
            reg_idx,
            w,
            space,
        } => {
            let addr = qq_addr(qq_offset as u32);
            let sc = space_char(space);
            if w {
                write!(
                    out,
                    "movep {},{}:${:04x}",
                    reg_name(reg_idx as usize),
                    sc,
                    addr
                )
                .unwrap();
            } else {
                write!(
                    out,
                    "movep {}:${:04x},{}",
                    sc,
                    addr,
                    reg_name(reg_idx as usize)
                )
                .unwrap();
            }
        }

        // Multiply with shift: (+/-)S,#n,D
        Instruction::MulShift {
            op,
            shift,
            src,
            d,
            k,
        } => {
            let sign = if k { "-" } else { "+" };
            let mnem = match op {
                MulShiftOp::Mpy => "mpy",
                MulShiftOp::Mpyr => "mpyr",
                MulShiftOp::Mac => "mac",
                MulShiftOp::Macr => "macr",
            };
            write!(
                out,
                "{} {}{},#{},{}",
                mnem,
                sign,
                reg_name(src),
                shift,
                acc_name(d as usize)
            )
            .unwrap();
        }

        // Multiply
        Instruction::MpyI { k, d, src }
        | Instruction::MpyrI { k, d, src }
        | Instruction::MacI { k, d, src }
        | Instruction::MacrI { k, d, src } => {
            len = 2;
            let sign = if k { "-" } else { "+" };
            let mnem = match inst {
                Instruction::MpyI { .. } => "mpyi",
                Instruction::MpyrI { .. } => "mpyri",
                Instruction::MacI { .. } => "maci",
                Instruction::MacrI { .. } => "macri",
                _ => unreachable!(),
            };
            write!(
                out,
                "{} {}#${:06x},{},{}",
                mnem,
                sign,
                next_word,
                reg_name(src),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::Dmac { ss, k, d, s1, s2 } => {
            let suffix = match ss {
                0 => "ss",
                1 => "us",
                2 => "su",
                _ => "uu",
            };
            let sign = if k { "-" } else { "+" };
            write!(
                out,
                "dmac{} {}{},{},{}",
                suffix,
                sign,
                reg_name(s1),
                reg_name(s2),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::MacSU { s, k, d, s1, s2 } => {
            let suffix = if s & 1 != 0 { "uu" } else { "su" };
            let sign = if k { "-" } else { "+" };
            write!(
                out,
                "mac{} {}{},{},{}",
                suffix,
                sign,
                reg_name(s1),
                reg_name(s2),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::MpySU { s, k, d, s1, s2 } => {
            let suffix = if s & 1 != 0 { "uu" } else { "su" };
            let sign = if k { "-" } else { "+" };
            write!(
                out,
                "mpy{} {}{},{},{}",
                suffix,
                sign,
                reg_name(s1),
                reg_name(s2),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::Div { src, d } => {
            let destreg = if d == Accumulator::A { reg::A } else { reg::B };
            write!(out, "div {},{}", reg_name(src), reg_name(destreg)).unwrap();
        }

        // Address
        Instruction::Lua { ea_mode, dst_reg } => {
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            let rn = dst_reg & 7;
            let prefix = if dst_reg & 8 != 0 { 'n' } else { 'r' };
            write!(out, "lua {},{}{}", addr, prefix, rn).unwrap();
        }
        Instruction::LuaRel {
            aa,
            addr_reg,
            dst_reg,
            dest_is_n,
        } => {
            let aa_s = signextend(7, aa as u32);
            let disp = fmt_disp(aa_s);
            if dest_is_n {
                write!(out, "lua (r{}{}),n{}", addr_reg, disp, dst_reg).unwrap();
            } else {
                write!(out, "lua (r{}{}),r{}", addr_reg, disp, dst_reg).unwrap();
            }
        }
        Instruction::LraRn { addr_reg, dst_reg } => {
            write!(out, "lra r{},{}", addr_reg, reg_name(dst_reg as usize)).unwrap();
        }
        Instruction::LraDisp { dst_reg } => {
            len = 2;
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(out, "lra ${:06x},{}", target, reg_name(dst_reg as usize)).unwrap();
        }
        Instruction::Norm { rreg_idx, d } => {
            let srcreg = reg::R0 + rreg_idx as usize;
            let destreg = if d == Accumulator::A { reg::A } else { reg::B };
            write!(out, "norm {},{}", reg_name(srcreg), reg_name(destreg)).unwrap();
        }

        // Transfer conditional
        Instruction::Tcc { cc, acc, r } => {
            write!(out, "t{}", CC_NAMES[cc as usize]).unwrap();
            if let Some((src, dst)) = acc {
                write!(out, " {},{}", reg_name(src), reg_name(dst)).unwrap();
            }
            if let Some((r_src, r_dst)) = r {
                let src = reg::R0 + r_src as usize;
                let dst = reg::R0 + r_dst as usize;
                write!(out, " {},{}", reg_name(src), reg_name(dst)).unwrap();
            }
        }

        // Misc
        Instruction::Nop => write!(out, "nop").unwrap(),
        Instruction::Dec { d } => write!(out, "dec {}", acc_name(d as usize)).unwrap(),
        Instruction::Inc { d } => write!(out, "inc {}", acc_name(d as usize)).unwrap(),
        Instruction::Illegal => write!(out, "illegal").unwrap(),
        Instruction::Reset => write!(out, "reset").unwrap(),
        Instruction::Rti => write!(out, "rti").unwrap(),
        Instruction::Rts => write!(out, "rts").unwrap(),
        Instruction::Stop => write!(out, "stop").unwrap(),
        Instruction::Wait => write!(out, "wait").unwrap(),

        Instruction::Clb { s, d } => {
            write!(out, "clb {},{}", acc_name(s as usize), acc_name(d as usize)).unwrap();
        }
        Instruction::Normf { src, d } => {
            write!(out, "normf {},{}", reg_name(src), acc_name(d as usize)).unwrap();
        }
        Instruction::Merge { src, d } => {
            write!(out, "merge {},{}", reg_name(src), acc_name(d as usize)).unwrap();
        }
        Instruction::ExtractReg { s1, s2, d } => {
            write!(
                out,
                "extract {},{},{}",
                reg_name(s1),
                acc_name(s2 as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::ExtractImm { s2, d } => {
            len = 2;
            write!(
                out,
                "extract #${:06x},{},{}",
                next_word,
                acc_name(s2 as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::ExtractuReg { s1, s2, d } => {
            write!(
                out,
                "extractu {},{},{}",
                reg_name(s1),
                acc_name(s2 as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::ExtractuImm { s2, d } => {
            len = 2;
            write!(
                out,
                "extractu #${:06x},{},{}",
                next_word,
                acc_name(s2 as usize),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::InsertReg { s1, s2, d } => {
            write!(
                out,
                "insert {},{},{}",
                reg_name(s1),
                reg_name(s2),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::InsertImm { s2, d } => {
            len = 2;
            write!(
                out,
                "insert #${:06x},{},{}",
                next_word,
                reg_name(s2),
                acc_name(d as usize)
            )
            .unwrap();
        }
        Instruction::Vsl { s, ea_mode, i_bit } => {
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            write!(out, "vsl {},{},l:{}", acc_name(s as usize), i_bit, addr).unwrap();
        }
        Instruction::Debug => write!(out, "debug").unwrap(),
        Instruction::Debugcc { cc } => {
            write!(out, "debug{}", CC_NAMES[cc as usize]).unwrap();
        }
        Instruction::Trap => write!(out, "trap").unwrap(),
        Instruction::Trapcc { cc } => {
            write!(out, "trap{}", CC_NAMES[cc as usize]).unwrap();
        }
        Instruction::Pflush => write!(out, "pflush").unwrap(),
        Instruction::Pflushun => write!(out, "pflushun").unwrap(),
        Instruction::Pfree => write!(out, "pfree").unwrap(),
        Instruction::PlockEa { ea_mode } => {
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            write!(out, "plock {}", addr).unwrap();
        }
        Instruction::Plockr => {
            len = 2;
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(out, "plockr ${:06x}", target).unwrap();
        }
        Instruction::PunlockEa { ea_mode } => {
            let (addr, _, extra) = calc_ea(ea_mode as u32, next_word);
            if extra {
                len = 2;
            }
            write!(out, "punlock {}", addr).unwrap();
        }
        Instruction::Punlockr => {
            len = 2;
            let target = pc.wrapping_add(next_word) & 0xFFFFFF;
            write!(out, "punlockr ${:06x}", target).unwrap();
        }

        Instruction::Unimplemented { name, opcode: op } => {
            write!(out, "{} ; ${:06x}", name, op).unwrap();
        }
        Instruction::Unknown { opcode: op } => {
            write!(out, "dc ${:06x}", op).unwrap();
        }
    }

    (out, len)
}

/// Extract the mnemonic for bit manipulation instructions.
fn bit_mnemonic(inst: &Instruction) -> &'static str {
    match inst {
        Instruction::BchgEa { .. }
        | Instruction::BchgAa { .. }
        | Instruction::BchgPp { .. }
        | Instruction::BchgQq { .. }
        | Instruction::BchgReg { .. } => "bchg",
        Instruction::BclrEa { .. }
        | Instruction::BclrAa { .. }
        | Instruction::BclrPp { .. }
        | Instruction::BclrQq { .. }
        | Instruction::BclrReg { .. } => "bclr",
        Instruction::BsetEa { .. }
        | Instruction::BsetAa { .. }
        | Instruction::BsetPp { .. }
        | Instruction::BsetQq { .. }
        | Instruction::BsetReg { .. } => "bset",
        Instruction::BtstEa { .. }
        | Instruction::BtstAa { .. }
        | Instruction::BtstPp { .. }
        | Instruction::BtstQq { .. }
        | Instruction::BtstReg { .. } => "btst",
        _ => "?bit",
    }
}

/// Extract the mnemonic for bit-branch instructions.
fn bitbranch_mnemonic(inst: &Instruction) -> &'static str {
    match inst {
        Instruction::JclrEa { .. }
        | Instruction::JclrAa { .. }
        | Instruction::JclrPp { .. }
        | Instruction::JclrQq { .. }
        | Instruction::JclrReg { .. } => "jclr",
        Instruction::JsetEa { .. }
        | Instruction::JsetAa { .. }
        | Instruction::JsetPp { .. }
        | Instruction::JsetQq { .. }
        | Instruction::JsetReg { .. } => "jset",
        Instruction::JsclrEa { .. }
        | Instruction::JsclrAa { .. }
        | Instruction::JsclrPp { .. }
        | Instruction::JsclrQq { .. }
        | Instruction::JsclrReg { .. } => "jsclr",
        Instruction::JssetEa { .. }
        | Instruction::JssetAa { .. }
        | Instruction::JssetPp { .. }
        | Instruction::JssetQq { .. }
        | Instruction::JssetReg { .. } => "jsset",
        _ => "?bitbr",
    }
}

/// Extract the mnemonic for relative bit-branch instructions (brclr/brset).
fn relbranch_mnemonic(inst: &Instruction) -> &'static str {
    match inst {
        Instruction::BrclrEa { .. }
        | Instruction::BrclrAa { .. }
        | Instruction::BrclrPp { .. }
        | Instruction::BrclrQq { .. }
        | Instruction::BrclrReg { .. } => "brclr",
        _ => "brset",
    }
}

/// Extract the mnemonic for subroutine bit-branch instructions (bsclr/bsset).
fn subr_branch_mnemonic(inst: &Instruction) -> &'static str {
    match inst {
        Instruction::BsclrEa { .. }
        | Instruction::BsclrAa { .. }
        | Instruction::BsclrPp { .. }
        | Instruction::BsclrQq { .. }
        | Instruction::BsclrReg { .. } => "bsclr",
        _ => "bsset",
    }
}

/// Result of parallel move disassembly.
struct ParallelMoveResult {
    text: String,
    extra_word: bool,
}

/// Disassemble the parallel move portion of a parallel instruction.
fn disasm_parallel_move(
    move_type: ParallelMoveType,
    op: u32,
    next_word: u32,
) -> ParallelMoveResult {
    match move_type {
        ParallelMoveType::Pm0 => disasm_pm_0(op, next_word),
        ParallelMoveType::Pm1 => disasm_pm_1(op, next_word),
        ParallelMoveType::Pm2 | ParallelMoveType::Pm3 => disasm_pm_2(op),
        ParallelMoveType::Pm4 | ParallelMoveType::Pm5 => disasm_pm_4(op, next_word),
        ParallelMoveType::Pm8 => disasm_pm_8(op, next_word),
    }
}

fn disasm_pm_0(op: u32, next_word: u32) -> ParallelMoveResult {
    let memspace = (op >> 15) & 1;
    let numreg1 = reg::A + ((op >> 16) & 1) as usize;
    let ea_mode = (op >> 8) & 0x3F;
    let (addr, _, extra) = calc_ea(ea_mode, next_word);

    let (space, numreg2) = if memspace != 0 {
        ("y", reg::Y0)
    } else {
        ("x", reg::X0)
    };

    ParallelMoveResult {
        text: format!(
            "{},{}:{} {},{}",
            reg_name(numreg1),
            space,
            addr,
            reg_name(numreg2),
            reg_name(numreg1)
        ),
        extra_word: extra,
    }
}

fn disasm_pm_1(op: u32, next_word: u32) -> ParallelMoveResult {
    let memspace = (op >> 14) & 1;
    let write_flag = (op >> 15) & 1;
    let ea_mode = (op >> 8) & 0x3F;
    let (addr, is_imm, extra) = calc_ea(ea_mode, next_word);

    let text = if memspace == 1 {
        // Y space access
        let d2 = match (op >> 16) & 3 {
            0 => reg::Y0,
            1 => reg::Y1,
            2 => reg::A,
            3 => reg::B,
            _ => unreachable!(),
        };
        let s1 = reg::A + ((op >> 19) & 1) as usize;
        let d1 = reg::X0 + ((op >> 18) & 1) as usize;
        if write_flag != 0 {
            if is_imm {
                format!(
                    "{},{} {}{},{}",
                    reg_name(s1),
                    reg_name(d1),
                    imm_prefix(next_word, 0xFF),
                    addr,
                    reg_name(d2)
                )
            } else {
                format!(
                    "{},{} y:{},{}",
                    reg_name(s1),
                    reg_name(d1),
                    addr,
                    reg_name(d2)
                )
            }
        } else {
            format!(
                "{},{} {},y:{}",
                reg_name(s1),
                reg_name(d1),
                reg_name(d2),
                addr
            )
        }
    } else {
        // X space access
        let d1 = match (op >> 18) & 3 {
            0 => reg::X0,
            1 => reg::X1,
            2 => reg::A,
            3 => reg::B,
            _ => unreachable!(),
        };
        let s2 = reg::A + ((op >> 17) & 1) as usize;
        let d2 = reg::Y0 + ((op >> 16) & 1) as usize;
        if write_flag != 0 {
            if is_imm {
                format!(
                    "{}{},{} {},{}",
                    imm_prefix(next_word, 0xFF),
                    addr,
                    reg_name(d1),
                    reg_name(s2),
                    reg_name(d2)
                )
            } else {
                format!(
                    "x:{},{} {},{}",
                    addr,
                    reg_name(d1),
                    reg_name(s2),
                    reg_name(d2)
                )
            }
        } else {
            format!(
                "{},x:{} {},{}",
                reg_name(d1),
                addr,
                reg_name(s2),
                reg_name(d2)
            )
        }
    };

    ParallelMoveResult {
        text,
        extra_word: extra,
    }
}

fn disasm_pm_2(op: u32) -> ParallelMoveResult {
    // Check special cases based on bits 23:8
    let upper = (op >> 8) & 0xFFFF;

    if upper == 0x2000 {
        // NOP parallel move
        return ParallelMoveResult {
            text: String::new(),
            extra_word: false,
        };
    }
    if upper & 0xFFF0 == 0x2020 {
        // IFcc: 0010 0000 0010 CCCC
        let cc = (op >> 8) & 0xF;
        return ParallelMoveResult {
            text: format!("if{}", CC_NAMES[cc as usize]),
            extra_word: false,
        };
    }
    if upper & 0xFFF0 == 0x2030 {
        // IFcc.U: 0010 0000 0011 CCCC
        let cc = (op >> 8) & 0xF;
        return ParallelMoveResult {
            text: format!("if{}.u", CC_NAMES[cc as usize]),
            extra_word: false,
        };
    }
    if upper & 0xFFE0 == 0x2040 {
        // R update: ea -> Rn
        let ea_mode = (op >> 8) & 0x1F;
        let numreg = (op >> 8) & 7;
        let (addr, _, _) = calc_ea(ea_mode, 0);
        return ParallelMoveResult {
            text: format!("{},r{}", addr, numreg),
            extra_word: false,
        };
    }
    if upper & 0xFC00 == 0x2000 {
        // Register to register
        let numreg1 = ((op >> 13) & 0x1F) as usize;
        let numreg2 = ((op >> 8) & 0x1F) as usize;
        let r1 = reg_name(numreg1);
        let r2 = reg_name(numreg2);
        return ParallelMoveResult {
            text: format!("{},{}", r1, r2),
            extra_word: false,
        };
    }

    // Immediate to register
    let numreg = ((op >> 16) & 0x1F) as usize;
    let imm = (op >> 8) & 0xFF;
    ParallelMoveResult {
        text: format!("#${:02x},{}", imm, reg_name(numreg)),
        extra_word: false,
    }
}

fn disasm_pm_4(op: u32, next_word: u32) -> ParallelMoveResult {
    let mut value = ((op >> 16) & 7) | ((op >> 17) & (3 << 3));
    let ea_mode = (op >> 8) & 0x3F;

    if (value >> 2) == 0 {
        // L: memory move
        let (addr, is_imm, extra) = if op & (1 << 14) != 0 {
            calc_ea(ea_mode, next_word)
        } else {
            (format!("${:04x}", ea_mode), false, false)
        };

        let lreg_idx = (((op >> 16) & 3) | ((op >> 17) & (1 << 2))) as usize;
        let text = if op & (1 << 15) != 0 {
            if is_imm {
                format!(
                    "{}{},{}",
                    imm_prefix(next_word, 0xFF),
                    addr,
                    REGISTERS_LMOVE[lreg_idx]
                )
            } else {
                format!("l:{},{}", addr, REGISTERS_LMOVE[lreg_idx])
            }
        } else {
            format!("{},l:{}", REGISTERS_LMOVE[lreg_idx], addr)
        };
        return ParallelMoveResult {
            text,
            extra_word: extra,
        };
    }

    // X: or Y: memory move
    let memspace = (op >> 19) & 1;
    let (addr, is_imm, extra) = if op & (1 << 14) != 0 {
        calc_ea(ea_mode, next_word)
    } else {
        (format!("${:04x}", ea_mode), false, false)
    };

    value = ((op >> 16) & 7) | ((op >> 17) & (3 << 3));

    let s = space_name(memspace);
    let text = if op & (1 << 15) != 0 {
        if is_imm {
            format!(
                "{}{},{}",
                imm_prefix(next_word, 0xFF),
                addr,
                reg_name(value as usize)
            )
        } else {
            format!("{}:{},{}", s, addr, reg_name(value as usize))
        }
    } else {
        format!("{},{}:{}", reg_name(value as usize), s, addr)
    };

    ParallelMoveResult {
        text,
        extra_word: extra,
    }
}

fn disasm_pm_8(op: u32, next_word: u32) -> ParallelMoveResult {
    let numreg1 = match (op >> 18) & 3 {
        0 => reg::X0,
        1 => reg::X1,
        2 => reg::A,
        3 => reg::B,
        _ => unreachable!(),
    };
    let numreg2 = match (op >> 16) & 3 {
        0 => reg::Y0,
        1 => reg::Y1,
        2 => reg::A,
        3 => reg::B,
        _ => unreachable!(),
    };

    let mut ea_mode1 = (op >> 8) & 0x1F;
    if (ea_mode1 >> 3) == 0 {
        ea_mode1 |= 1 << 5;
    }
    let mut ea_mode2 = ((op >> 13) & 3) | (((op >> 20) & 3) << 3);
    if (ea_mode1 & (1 << 2)) == 0 {
        ea_mode2 |= 1 << 2;
    }
    if (ea_mode2 >> 3) == 0 {
        ea_mode2 |= 1 << 5;
    }

    let (addr1, _, extra1) = calc_ea(ea_mode1, next_word);
    let (addr2, _, extra2) = calc_ea(ea_mode2, next_word);

    let text = if op & (1 << 15) != 0 {
        if op & (1 << 22) != 0 {
            format!(
                "x:{},{} y:{},{}",
                addr1,
                reg_name(numreg1),
                addr2,
                reg_name(numreg2)
            )
        } else {
            format!(
                "x:{},{} {},y:{}",
                addr1,
                reg_name(numreg1),
                reg_name(numreg2),
                addr2
            )
        }
    } else if op & (1 << 22) != 0 {
        format!(
            "{},x:{} y:{},{}",
            reg_name(numreg1),
            addr1,
            addr2,
            reg_name(numreg2)
        )
    } else {
        format!(
            "{},x:{} {},y:{}",
            reg_name(numreg1),
            addr1,
            reg_name(numreg2),
            addr2
        )
    };

    ParallelMoveResult {
        text,
        extra_word: extra1 || extra2,
    }
}
