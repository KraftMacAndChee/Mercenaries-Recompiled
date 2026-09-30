//! Encoder: converts AST instructions into binary DSP56300 opcodes.

use crate::SymbolTable;
use crate::ast::*;
pub use dsp56300_core::encode::{EncodeError, EncodedInstruction};
use dsp56300_core::encode::{
    enc_err, l_reg_split, pack_loop_count, pack_xy_imm_offset, pm4_reg_split, word, words,
};

type Result<T> = std::result::Result<T, EncodeError>;

/// Build an encoded instruction from a primary word and an optional extension word.
fn with_ext(w0: u32, ext: Option<u32>) -> EncodedInstruction {
    match ext {
        Some(v) => words(w0, v),
        None => word(w0),
    }
}

/// Encode a single instruction into binary.
pub fn encode(inst: &Instruction, pc: u32, sym: &SymbolTable) -> Result<EncodedInstruction> {
    match inst {
        // Zero-operand
        Instruction::Nop => Ok(word(0x000000)),
        Instruction::Rts => Ok(word(0x00000C)),
        Instruction::Rti => Ok(word(0x000004)),
        Instruction::Reset => Ok(word(0x000084)),
        Instruction::Stop => Ok(word(0x000087)),
        Instruction::Wait => Ok(word(0x000086)),
        Instruction::EndDo => Ok(word(0x00008C)),
        Instruction::Illegal => Ok(word(0x000005)),

        // Single accumulator
        Instruction::Inc(acc) => Ok(word(0x000008 | (*acc as u32))),
        Instruction::Dec(acc) => Ok(word(0x00000A | (*acc as u32))),

        // Arithmetic with immediate (short 6-bit / long 24-bit forms)
        Instruction::AddImm { imm, d } => encode_imm_alu(imm, *d, 0x00, sym, pc),
        Instruction::AddLong { imm, d } => encode_long_alu(imm, *d, 0x00, sym, pc),
        Instruction::SubImm { imm, d } => encode_imm_alu(imm, *d, 0x04, sym, pc),
        Instruction::SubLong { imm, d } => encode_long_alu(imm, *d, 0x04, sym, pc),
        Instruction::CmpImm { imm, d } => encode_imm_alu(imm, *d, 0x05, sym, pc),
        Instruction::CmpLong { imm, d } => encode_long_alu(imm, *d, 0x05, sym, pc),
        Instruction::AndImm { imm, d } => encode_imm_alu(imm, *d, 0x06, sym, pc),
        Instruction::AndLong { imm, d } => encode_long_alu(imm, *d, 0x06, sym, pc),
        Instruction::OrImm { imm, d } => encode_imm_alu(imm, *d, 0x02, sym, pc),
        Instruction::OrLong { imm, d } => encode_long_alu(imm, *d, 0x02, sym, pc),
        Instruction::EorImm { imm, d } => encode_imm_alu(imm, *d, 0x03, sym, pc),
        Instruction::EorLong { imm, d } => encode_long_alu(imm, *d, 0x03, sym, pc),
        Instruction::AndI { imm, dest } => {
            let v = eval(imm, sym, pc)?;
            // Template: 00000000iiiiiiii111110EE
            // EE: 00=mr, 01=ccr, 10=omr
            Ok(word(0x0000B8 | ((v & 0xFF) << 8) | ((*dest as u32) & 3)))
        }
        Instruction::OrI { imm, dest } => {
            let v = eval(imm, sym, pc)?;
            // Template: 00000000iiiiiiii111111EE
            Ok(word(0x0000F8 | ((v & 0xFF) << 8) | ((*dest as u32) & 3)))
        }

        // Shifts
        Instruction::AslImm { shift, src, dst } => {
            let ii = eval(shift, sym, pc)?;
            // Template: 0000110000011101SiiiiiiD
            Ok(word(
                0x0C1D00 | ((*src as u32) << 7) | ((ii & 0x3F) << 1) | (*dst as u32),
            ))
        }
        Instruction::AsrImm { shift, src, dst } => {
            let ii = eval(shift, sym, pc)?;
            // Template: 0000110000011100SiiiiiiD
            Ok(word(
                0x0C1C00 | ((*src as u32) << 7) | ((ii & 0x3F) << 1) | (*dst as u32),
            ))
        }
        Instruction::LslImm { shift, dst } => {
            let ii = eval(shift, sym, pc)?;
            // Template: 000011000001111010iiiiiD
            Ok(word(0x0C1E80 | ((ii & 0x1F) << 1) | (*dst as u32)))
        }
        Instruction::LsrImm { shift, dst } => {
            let ii = eval(shift, sym, pc)?;
            // Template: 000011000001111011iiiiiD
            Ok(word(0x0C1EC0 | ((ii & 0x1F) << 1) | (*dst as u32)))
        }
        Instruction::AslReg {
            shift_reg,
            src,
            dst,
        } => {
            let sss = reg_to_sss(shift_reg)?;
            // Template: 0000110000011110010SsssD
            Ok(word(
                0x0C1E40 | ((*src as u32) << 4) | ((sss & 7) << 1) | (*dst as u32),
            ))
        }
        Instruction::AsrReg {
            shift_reg,
            src,
            dst,
        } => {
            let sss = reg_to_sss(shift_reg)?;
            // Template: 0000110000011110011SsssD
            Ok(word(
                0x0C1E60 | ((*src as u32) << 4) | ((sss & 7) << 1) | (*dst as u32),
            ))
        }
        Instruction::LslReg { shift_reg, dst } => {
            let sss = reg_to_sss(shift_reg)?;
            // Template: 00001100000111100001sssD
            Ok(word(0x0C1E10 | ((sss & 7) << 1) | (*dst as u32)))
        }
        Instruction::LsrReg { shift_reg, dst } => {
            let sss = reg_to_sss(shift_reg)?;
            // Template: 00001100000111100011sssD
            Ok(word(0x0C1E30 | ((sss & 7) << 1) | (*dst as u32)))
        }

        // Branches
        Instruction::Bcc {
            cc,
            target,
            force_long,
        } => encode_bcc(*cc, target, pc, sym, *force_long),
        Instruction::BccRn { cc, rn } => {
            // Template: 0000110100011RRR0100CCCC
            Ok(word(0x0D1840 | ((*rn as u32) << 8) | (*cc as u32)))
        }
        Instruction::Bra { target, force_long } => encode_bra(target, pc, sym, *force_long),
        Instruction::BraRn { rn } => {
            // Template: 0000110100011RRR11000000
            Ok(word(0x0D18C0 | ((*rn as u32) << 8)))
        }
        Instruction::Bsr { target, force_long } => encode_bsr(target, pc, sym, *force_long),
        Instruction::BsrRn { rn } => {
            // Template: 0000110100011RRR10000000
            Ok(word(0x0D1880 | ((*rn as u32) << 8)))
        }
        Instruction::Bscc {
            cc,
            target,
            force_long,
        } => encode_bscc(*cc, target, pc, sym, *force_long),
        Instruction::BsccRn { cc, rn } => {
            // Template: 0000110100011RRR0000CCCC
            Ok(word(0x0D1800 | ((*rn as u32) << 8) | (*cc as u32)))
        }
        Instruction::Brkcc { cc } => {
            // Template: 00000000000000100001CCCC
            Ok(word(0x000210 | (*cc as u32)))
        }
        Instruction::Jcc { cc, target } => {
            let addr = eval(target, sym, pc)?;
            // Template: 00001110CCCCaaaaaaaaaaaa
            Ok(word(0x0E0000 | ((*cc as u32) << 12) | (addr & 0xFFF)))
        }
        Instruction::JccEa { cc, ea } => encode_jcc_ea(*cc, ea, 0x0E0000, 0x0AC0A0, sym, pc),
        Instruction::Jmp {
            target,
            force_short,
        } => encode_jmp_jsr(target, *force_short, 0x0C0000, 0x0AF080, sym, pc),
        Instruction::JmpEa { ea } => {
            let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
            // Template: 0000101011MMMRRR10000000
            let w0 = 0x0AC080 | ((ea_bits as u32) << 8);
            Ok(with_ext(w0, ext))
        }
        Instruction::Jscc { cc, target } => {
            let addr = eval(target, sym, pc)?;
            // Template: 00001111CCCCaaaaaaaaaaaa
            Ok(word(0x0F0000 | ((*cc as u32) << 12) | (addr & 0xFFF)))
        }
        Instruction::JsccEa { cc, ea } => encode_jcc_ea(*cc, ea, 0x0F0000, 0x0BC0A0, sym, pc),
        Instruction::Jsr {
            target,
            force_short,
        } => encode_jmp_jsr(target, *force_short, 0x0D0000, 0x0BF080, sym, pc),
        Instruction::JsrEa { ea } => {
            let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
            // Template: 0000101111MMMRRR10000000
            let w0 = 0x0BC080 | ((ea_bits as u32) << 8);
            Ok(with_ext(w0, ext))
        }

        // Bit manipulation
        Instruction::Bchg { bit, target } => encode_bit_op(0b10, bit, target, sym, pc),
        Instruction::Bclr { bit, target } => encode_bit_op(0b00, bit, target, sym, pc),
        Instruction::Bset { bit, target } => encode_bit_op(0b01, bit, target, sym, pc),
        Instruction::Btst { bit, target } => encode_bit_op(0b11, bit, target, sym, pc),

        // Bit branch
        Instruction::Jclr { bit, target, addr } => {
            encode_bit_branch(0b000, bit, target, addr, pc, sym)
        }
        Instruction::Jset { bit, target, addr } => {
            encode_bit_branch(0b001, bit, target, addr, pc, sym)
        }
        Instruction::Jsclr { bit, target, addr } => {
            encode_bit_branch(0b010, bit, target, addr, pc, sym)
        }
        Instruction::Jsset { bit, target, addr } => {
            encode_bit_branch(0b011, bit, target, addr, pc, sym)
        }
        Instruction::Brclr { bit, target, addr } => {
            encode_bit_branch(0b100, bit, target, addr, pc, sym)
        }
        Instruction::Brset { bit, target, addr } => {
            encode_bit_branch(0b101, bit, target, addr, pc, sym)
        }
        Instruction::Bsclr { bit, target, addr } => {
            encode_bit_branch(0b110, bit, target, addr, pc, sym)
        }
        Instruction::Bsset { bit, target, addr } => {
            encode_bit_branch(0b111, bit, target, addr, pc, sym)
        }

        // Loop
        Instruction::Do { source, end_addr } => encode_do(source, end_addr, sym, pc),
        Instruction::DoForever { end_addr } => {
            let la = eval(end_addr, sym, pc)?.wrapping_sub(1) & 0xFFFFFF;
            Ok(words(0x000203, la))
        }
        Instruction::Dor { source, end_addr } => encode_dor(source, end_addr, pc, sym),
        Instruction::DorForever { end_addr } => {
            let addr = eval(end_addr, sym, pc)?.wrapping_sub(1);
            let rel = addr.wrapping_sub(pc) & 0xFFFFFF;
            Ok(words(0x000202, rel))
        }
        Instruction::Rep { source } => encode_rep(source, sym, pc),

        // Move
        Instruction::MovecReg { src, dst, w } => encode_movec_reg(src, dst, *w),
        Instruction::MovecAa {
            space,
            addr,
            reg,
            w,
        } => encode_movec_aa(*space, addr, reg, *w, sym, pc),
        Instruction::MovecEa { space, ea, reg, w } => encode_movec_ea(*space, ea, reg, *w, sym, pc),
        Instruction::MovecImm { imm, reg } => encode_movec_imm(imm, reg, sym, pc),
        Instruction::MovemEa { ea, reg, w } => encode_movem_ea(ea, reg, *w, sym, pc),
        Instruction::MovemAa { addr, reg, w } => encode_movem_aa(addr, reg, *w, sym, pc),
        Instruction::Movep23 {
            periph_space,
            periph_addr,
            ea_space,
            ea,
            w,
        } => encode_movep23(*periph_space, periph_addr, *ea_space, ea, *w, sym, pc),
        Instruction::Movep23Imm {
            periph_space,
            periph_addr,
            imm,
        } => encode_movep23_imm(*periph_space, periph_addr, imm, sym, pc),
        Instruction::MovepXQq {
            periph_addr,
            ea_space,
            ea,
            w,
        } => encode_movep_xqq(periph_addr, *ea_space, ea, *w, sym, pc),
        Instruction::MovepXQqImm { periph_addr, imm } => {
            encode_movep_xqq_imm(periph_addr, imm, sym, pc)
        }
        Instruction::Movep1 {
            periph_space,
            periph_addr,
            ea,
            w,
        } => encode_movep1(*periph_space, periph_addr, ea, *w, sym, pc),
        Instruction::Movep0 {
            periph_space,
            periph_addr,
            reg,
            w,
        } => encode_movep0(*periph_space, periph_addr, reg, *w, sym, pc),
        Instruction::MoveLongDisp {
            space,
            offset_reg,
            offset,
            reg,
            w,
        } => encode_move_long_disp(*space, *offset_reg, offset, reg, *w, sym, pc),
        Instruction::MoveShortDisp {
            space,
            offset_reg,
            offset,
            reg,
            w,
        } => encode_move_short_disp(*space, *offset_reg, offset, reg, *w, sym, pc),

        // Multiply/divide
        Instruction::MulShift {
            mnem,
            sign,
            src,
            shift,
            dst,
        } => encode_mul_shift(*mnem, *sign, src, shift, *dst, sym, pc),
        Instruction::MpyI {
            sign,
            imm,
            src,
            dst,
        } => encode_mpyi(*sign, imm, src, *dst, sym, pc),
        Instruction::MpyrI {
            sign,
            imm,
            src,
            dst,
        } => encode_imm_mul(*sign, imm, src, *dst, 0x01, sym, pc),
        Instruction::MacI {
            sign,
            imm,
            src,
            dst,
        } => encode_imm_mul(*sign, imm, src, *dst, 0x02, sym, pc),
        Instruction::MacrI {
            sign,
            imm,
            src,
            dst,
        } => encode_imm_mul(*sign, imm, src, *dst, 0x03, sym, pc),
        Instruction::Dmac {
            ss,
            sign,
            s1,
            s2,
            dst,
        } => encode_dmac(*ss, *sign, s1, s2, *dst),
        Instruction::MacSU {
            su,
            sign,
            s1,
            s2,
            dst,
        } => encode_mac_mpy_su(true, *su, *sign, s1, s2, *dst),
        Instruction::MpySU {
            su,
            sign,
            s1,
            s2,
            dst,
        } => encode_mac_mpy_su(false, *su, *sign, s1, s2, *dst),
        Instruction::Div { src, dst } => encode_div(src, *dst),
        Instruction::CmpU { src, dst } => encode_cmpu(src, *dst),
        Instruction::Norm { src, dst } => {
            // Template: 0000000111011RRR0001d101
            Ok(word(0x01D815 | ((*src as u32) << 8) | ((*dst as u32) << 3)))
        }

        // Address
        Instruction::Lua { ea, dst } => {
            let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
            let reg_idx = dst.index() as u32;
            // Template: 00000100010MMRRR000ddddd
            let w0 = 0x044000 | ((ea_bits as u32 & 0x1F) << 8) | (reg_idx & 0x1F);
            Ok(with_ext(w0, ext))
        }
        Instruction::LuaRel {
            base,
            offset,
            dst_is_n,
            dst,
        } => {
            let aa = eval(offset, sym, pc)? as i32;
            let aa7 = (aa as u32) & 0x7F;
            let aa_lo = aa7 & 0xF;
            let aa_hi = (aa7 >> 4) & 7;
            // Template: 00000100sssRRR01aaaa0nnn
            let n_bit = if *dst_is_n { 1u32 << 3 } else { 0 };
            Ok(word(
                0x040000
                    | (aa_hi << 11)
                    | ((*base as u32) << 8)
                    | (aa_lo << 4)
                    | n_bit
                    | (*dst as u32),
            ))
        }

        Instruction::LraRn { src, dst } => {
            // Template: 0000010011000RRR000ddddd
            let d = dst.index() as u32;
            Ok(word(0x04C000 | ((*src as u32) << 8) | (d & 0x1F)))
        }
        Instruction::LraDisp { target, dst } => {
            // Template: 0000010001000000010ddddd + 24-bit displacement
            let addr = eval(target, sym, pc)?;
            let rel24 = addr.wrapping_sub(pc) & 0xFFFFFF;
            let d = dst.index() as u32;
            Ok(words(0x044040 | (d & 0x1F), rel24))
        }

        // Tier 3
        Instruction::Clb { s, d } => {
            // Template: 0000110000011110000000SD
            Ok(word(0x0C1E00 | ((*s as u32) << 1) | (*d as u32)))
        }
        Instruction::Normf { src, d } => {
            // Template: 00001100000111100010sssD
            let sss = reg_to_sss(src)?;
            Ok(word(0x0C1E20 | (sss << 1) | (*d as u32)))
        }
        Instruction::Merge { src, d } => {
            // Template: 00001100000110111000sssD
            let sss = reg_to_sss(src)?;
            Ok(word(0x0C1B80 | (sss << 1) | (*d as u32)))
        }
        Instruction::ExtractReg { s1, s2, d } => {
            // Template: 0000110000011010000sSSSD
            let sss = reg_to_sss(s1)?;
            Ok(word(
                0x0C1A00 | ((*s2 as u32) << 4) | (sss << 1) | (*d as u32),
            ))
        }
        Instruction::ExtractImm { co, s2, d } => {
            // Template: 0000110000011000000s000D + control word
            let co_val = eval(co, sym, pc)? & 0xFFFFFF;
            Ok(words(0x0C1800 | ((*s2 as u32) << 4) | (*d as u32), co_val))
        }
        Instruction::ExtractuReg { s1, s2, d } => {
            // Template: 0000110000011010100sSSSD
            let sss = reg_to_sss(s1)?;
            Ok(word(
                0x0C1A80 | ((*s2 as u32) << 4) | (sss << 1) | (*d as u32),
            ))
        }
        Instruction::ExtractuImm { co, s2, d } => {
            // Template: 0000110000011000100s000D + control word
            let co_val = eval(co, sym, pc)? & 0xFFFFFF;
            Ok(words(0x0C1880 | ((*s2 as u32) << 4) | (*d as u32), co_val))
        }
        Instruction::InsertReg { s1, s2, d } => {
            // Template: 00001100000110110qqqSSSD
            let sss = reg_to_sss(s1)?;
            let qqq = reg_to_qqq(s2)?;
            Ok(word(0x0C1B00 | (qqq << 4) | (sss << 1) | (*d as u32)))
        }
        Instruction::InsertImm { co, s2, d } => {
            // Template: 00001100000110010qqq000D + control word
            let qqq = reg_to_qqq(s2)?;
            let co_val = eval(co, sym, pc)? & 0xFFFFFF;
            Ok(words(0x0C1900 | (qqq << 4) | (*d as u32), co_val))
        }
        Instruction::Debug => Ok(word(0x000200)),
        Instruction::Debugcc { cc } => {
            // Template: 00000000000000110000CCCC
            Ok(word(0x000300 | (*cc as u32)))
        }
        Instruction::Trap => Ok(word(0x000006)),
        Instruction::Trapcc { cc } => {
            // Template: 00000000000000000001CCCC
            Ok(word(0x000010 | (*cc as u32)))
        }
        Instruction::Vsl { s, i_bit, ea } => {
            // Template: 0000101S11MMMRRR110i0000
            let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
            let w0 = 0x0AC0C0
                | ((*s as u32) << 16)
                | ((ea_bits as u32 & 0x3F) << 8)
                | ((*i_bit as u32) << 4);
            Ok(with_ext(w0, ext))
        }
        Instruction::Pflush => Ok(word(0x000003)),
        Instruction::Pflushun => Ok(word(0x000001)),
        Instruction::Pfree => Ok(word(0x000002)),
        Instruction::PlockEa { ea } => encode_plock_ea(ea, 0x0BC081, sym, pc),
        Instruction::Plockr { target } => encode_plock_rel(target, 0x00000F, sym, pc),
        Instruction::PunlockEa { ea } => encode_plock_ea(ea, 0x0AC081, sym, pc),
        Instruction::Punlockr { target } => encode_plock_rel(target, 0x00000E, sym, pc),

        // Tcc
        Instruction::Tcc { cc, acc, r } => encode_tcc(*cc, acc, r),

        // Parallel
        Instruction::Parallel { alu, pmove } => encode_parallel(alu, pmove, sym, pc),
    }
}

pub fn eval(expr: &Expr, sym: &SymbolTable, pc: u32) -> Result<u32> {
    match expr {
        Expr::Literal(v) | Expr::Frac(v) => Ok(*v as u32),
        Expr::Symbol(name) => sym
            .get(name)
            .map(|&v| v as u32)
            .ok_or_else(|| enc_err(&format!("undefined symbol '{name}'"))),
        Expr::CurrentPc => Ok(pc),
        Expr::BinOp { op, lhs, rhs } => {
            let l = eval(lhs, sym, pc)? as i64;
            let r = eval(rhs, sym, pc)? as i64;
            Ok(match op {
                BinOp::Add => l.wrapping_add(r),
                BinOp::Sub => l.wrapping_sub(r),
                BinOp::Mul => l.wrapping_mul(r),
                BinOp::Div => {
                    if r == 0 {
                        return Err(enc_err("division by zero"));
                    }
                    l / r
                }
                BinOp::Shl => l << (r & 63),
                BinOp::Shr => ((l as u64) >> (r & 63)) as i64,
                BinOp::BitAnd => l & r,
                BinOp::BitOr => l | r,
            } as u32)
        }
        Expr::UnaryOp { op, operand } => {
            let v = eval(operand, sym, pc)? as i64;
            Ok(match op {
                UnaryOp::Neg => (-v) as u32,
                UnaryOp::BitNot => (!v) as u32,
            })
        }
    }
}

/// Encode a 6-bit immediate ALU instruction (short form).
fn encode_imm_alu(
    imm: &Expr,
    d: Acc,
    op_bits: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    Ok(word(
        0x014080 | op_bits | ((v & 0x3F) << 8) | ((d as u32) << 3),
    ))
}

/// Encode a 24-bit immediate ALU instruction (long form).
fn encode_long_alu(
    imm: &Expr,
    d: Acc,
    op_bits: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    Ok(words(0x0140C0 | op_bits | ((d as u32) << 3), v & 0xFFFFFF))
}

/// Encode a 6-bit EA mode field. Returns (ea_bits, optional_extension_word).
fn encode_ea(ea: &EffectiveAddress, sym: &SymbolTable, pc: u32) -> Result<(u8, Option<u32>)> {
    Ok(match ea {
        EffectiveAddress::PostDecN(n) => (*n, None),
        EffectiveAddress::PostIncN(n) => (0b001_000 | n, None),
        EffectiveAddress::PostDec(n) => (0b010_000 | n, None),
        EffectiveAddress::PostInc(n) => (0b011_000 | n, None),
        EffectiveAddress::NoUpdate(n) => (0b100_000 | n, None),
        EffectiveAddress::IndexedN(n) => (0b101_000 | n, None),
        EffectiveAddress::AbsAddr(expr) | EffectiveAddress::ForceLongAbsAddr(expr) => {
            let v = eval(expr, sym, pc)?;
            (0b110_000, Some(v & 0xFFFFFF))
        }
        EffectiveAddress::Immediate(expr) => {
            let v = eval(expr, sym, pc)?;
            (0b110_100, Some(v & 0xFFFFFF))
        }
        EffectiveAddress::PreDec(n) => (0b111_000 | n, None),
    })
}

fn ast_space_bit(space: MemorySpace) -> u32 {
    match space {
        MemorySpace::X => 0,
        MemorySpace::Y => 1,
        _ => 0,
    }
}

// ---- Branch encoders ----

/// Encode a 9-bit signed displacement into the split field `aaaa0aaaaa`.
/// Returns the 10-bit field value (high 4 bits at [9:6], low 5 bits at [4:0]).
fn encode_disp9(disp: u32) -> u32 {
    ((disp & 0x1E0) << 1) | (disp & 0x1F)
}

/// Check if a branch displacement (target - pc, without 24-bit wrapping) fits
/// in the short form range (-256..=255). The official assembler does not wrap
/// around the 24-bit address space when checking, so branches that cross the
/// address space boundary (e.g. target=$FFFFFA at pc=0) use long form.
fn fits_short_branch(addr: u32, pc: u32) -> bool {
    let disp = addr as i32 - pc as i32;
    (-256..=255).contains(&disp)
}

fn encode_rel_branch(
    target: &Expr,
    pc: u32,
    sym: &SymbolTable,
    force_long: bool,
    short_base: u32,
    long_base: u32,
) -> Result<EncodedInstruction> {
    let addr = eval(target, sym, pc)?;
    let rel24 = addr.wrapping_sub(pc) & 0xFFFFFF;
    if !force_long && target.is_literal() && fits_short_branch(addr, pc) {
        Ok(word(short_base | encode_disp9(rel24 & 0x1FF)))
    } else {
        Ok(words(long_base, rel24))
    }
}

fn encode_bcc(
    cc: CondCode,
    target: &Expr,
    pc: u32,
    sym: &SymbolTable,
    force_long: bool,
) -> Result<EncodedInstruction> {
    encode_rel_branch(
        target,
        pc,
        sym,
        force_long,
        0x050400 | ((cc as u32) << 12),
        0x0D1040 | (cc as u32),
    )
}

fn encode_bra(
    target: &Expr,
    pc: u32,
    sym: &SymbolTable,
    force_long: bool,
) -> Result<EncodedInstruction> {
    encode_rel_branch(target, pc, sym, force_long, 0x050C00, 0x0D10C0)
}

fn encode_bsr(
    target: &Expr,
    pc: u32,
    sym: &SymbolTable,
    force_long: bool,
) -> Result<EncodedInstruction> {
    encode_rel_branch(target, pc, sym, force_long, 0x050800, 0x0D1080)
}

fn encode_bscc(
    cc: CondCode,
    target: &Expr,
    pc: u32,
    sym: &SymbolTable,
    force_long: bool,
) -> Result<EncodedInstruction> {
    encode_rel_branch(
        target,
        pc,
        sym,
        force_long,
        0x050000 | ((cc as u32) << 12),
        0x0D1000 | (cc as u32),
    )
}

/// Encode Jmp/Jsr: short 12-bit form or long 24-bit form.
fn encode_jmp_jsr(
    target: &Expr,
    force_short: bool,
    short_base: u32,
    long_base: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let addr = eval(target, sym, pc)?;
    if force_short || (target.is_literal() && addr < 0x1000) {
        Ok(word(short_base | (addr & 0xFFF)))
    } else {
        Ok(words(long_base, addr & 0xFFFFFF))
    }
}

/// Encode Jcc/Jscc EA form: try short absolute, fall back to long EA.
fn encode_jcc_ea(
    cc: CondCode,
    ea: &EffectiveAddress,
    short_base: u32,
    long_base: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    if let EffectiveAddress::AbsAddr(target) = ea {
        let addr = eval(target, sym, pc)?;
        if target.is_literal() && addr < 0x1000 {
            return Ok(word(short_base | ((cc as u32) << 12) | (addr & 0xFFF)));
        }
    }
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let w0 = long_base | ((ea_bits as u32) << 8) | (cc as u32);
    Ok(with_ext(w0, ext))
}

fn encode_plock_ea(
    ea: &EffectiveAddress,
    base: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    Ok(with_ext(base | ((ea_bits as u32 & 0x3F) << 8), ext))
}

fn encode_plock_rel(
    target: &Expr,
    base: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let addr = eval(target, sym, pc)?;
    Ok(words(base, addr.wrapping_sub(pc) & 0xFFFFFF))
}

// ---- Bit operation encoders ----

fn encode_bit_op(
    op_bits: u32,
    bit: &Expr,
    target: &BitTarget,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let numbit = eval(bit, sym, pc)?;
    let hi = (op_bits >> 1) & 1;
    let lo = op_bits & 1;

    match target {
        BitTarget::Reg(reg) => {
            let reg_idx = reg.index() as u32;
            let base = 0x0AC000 | (hi << 16);
            Ok(word(
                base | ((reg_idx & 0x3F) << 8) | (1 << 6) | (lo << 5) | (numbit & 0x1F),
            ))
        }
        BitTarget::Addr { space, addr } => {
            let addr_val = normalize_periph_addr(eval(addr, sym, pc)?);
            let s = ast_space_bit(*space);
            if addr_val >= 0xFFFFC0 {
                let offset = addr_val - 0xFFFFC0;
                let base = 0x0A8000 | (hi << 16);
                Ok(word(
                    base | ((offset & 0x3F) << 8) | (s << 6) | (lo << 5) | (numbit & 0x1F),
                ))
            } else if addr_val >= 0xFFFF80 {
                let offset = addr_val - 0xFFFF80;
                let base = 0x010000 | (hi << 14);
                Ok(word(
                    base | ((offset & 0x3F) << 8) | (s << 6) | (lo << 5) | (numbit & 0x1F),
                ))
            } else if addr_val < 0x40 {
                let base = 0x0A0000 | (hi << 16);
                Ok(word(
                    base | ((addr_val & 0x3F) << 8) | (s << 6) | (lo << 5) | (numbit & 0x1F),
                ))
            } else {
                let ea_bits = 0b110_000u32;
                let base = 0x0A4000 | (hi << 16);
                Ok(words(
                    base | (ea_bits << 8) | (s << 6) | (lo << 5) | (numbit & 0x1F),
                    addr_val & 0xFFFFFF,
                ))
            }
        }
        BitTarget::Ea { space, ea } => {
            let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
            let s = ast_space_bit(*space);
            let base = 0x0A4000 | (hi << 16);
            let w0 = base | ((ea_bits as u32) << 8) | (s << 6) | (lo << 5) | (numbit & 0x1F);
            Ok(with_ext(w0, ext))
        }
    }
}

fn encode_bit_branch(
    op_code: u32,
    bit: &Expr,
    target: &BitTarget,
    addr: &Expr,
    pc: u32,
    sym: &SymbolTable,
) -> Result<EncodedInstruction> {
    let numbit = eval(bit, sym, pc)? & 0x1F;
    let is_relative = op_code >= 4; // brclr/brset/bsclr/bsset are relative

    let addr_val = eval(addr, sym, pc)?;
    let ext_word = if is_relative {
        addr_val.wrapping_sub(pc) & 0xFFFFFF
    } else {
        addr_val & 0xFFFFFF
    };

    let set_bit = op_code & 1;
    let bs = if op_code >= 6 { 1u32 } else { 0 };

    if is_relative {
        match target {
            BitTarget::Addr { space, addr } => {
                let addr_val = eval(addr, sym, pc)?;
                let s = ast_space_bit(*space);
                if addr_val >= 0xFFFFC0 {
                    let offset = addr_val - 0xFFFFC0;
                    Ok(words(
                        0x0CC000
                            | (bs << 16)
                            | ((offset & 0x3F) << 8)
                            | (s << 6)
                            | (set_bit << 5)
                            | numbit,
                        ext_word,
                    ))
                } else if addr_val >= 0xFFFF80 {
                    let offset = addr_val - 0xFFFF80;
                    Ok(words(
                        0x048000
                            | (bs << 7)
                            | ((offset & 0x3F) << 8)
                            | (s << 6)
                            | (set_bit << 5)
                            | numbit,
                        ext_word,
                    ))
                } else if addr_val < 0x40 {
                    Ok(words(
                        0x0C8000
                            | (bs << 16)
                            | ((addr_val & 0x3F) << 8)
                            | (1 << 7)
                            | (s << 6)
                            | (set_bit << 5)
                            | numbit,
                        ext_word,
                    ))
                } else if addr_val == ext_word {
                    let ea_bits = 0b110_000u32;
                    Ok(words(
                        0x0C8000 | (bs << 16) | (ea_bits << 8) | (s << 6) | (set_bit << 5) | numbit,
                        ext_word,
                    ))
                } else {
                    Err(enc_err("bit branch address must be in pp, qq, or aa range"))
                }
            }
            BitTarget::Ea { space, ea } => {
                let (ea_bits, _) = encode_ea(ea, sym, pc)?;
                let s = ast_space_bit(*space);
                Ok(words(
                    0x0C8000
                        | (bs << 16)
                        | ((ea_bits as u32) << 8)
                        | (s << 6)
                        | (set_bit << 5)
                        | numbit,
                    ext_word,
                ))
            }
            BitTarget::Reg(reg) => {
                let reg_idx = reg.index() as u32;
                Ok(words(
                    0x0CC080 | (bs << 16) | ((reg_idx & 0x3F) << 8) | (set_bit << 5) | numbit,
                    ext_word,
                ))
            }
        }
    } else {
        let js = if (op_code & 2) != 0 { 1u32 << 16 } else { 0 };

        match target {
            BitTarget::Addr { space, addr } => {
                let addr_val = normalize_periph_addr(eval(addr, sym, pc)?);
                let s = ast_space_bit(*space);
                if addr_val >= 0xFFFFC0 {
                    let offset = addr_val - 0xFFFFC0;
                    Ok(words(
                        0x0A8080 | js | ((offset & 0x3F) << 8) | (s << 6) | (set_bit << 5) | numbit,
                        ext_word,
                    ))
                } else if addr_val >= 0xFFFF80 {
                    let offset = addr_val - 0xFFFF80;
                    let js_qq = if (op_code & 2) != 0 { 1u32 << 14 } else { 0 };
                    Ok(words(
                        0x018080
                            | js_qq
                            | ((offset & 0x3F) << 8)
                            | (s << 6)
                            | (set_bit << 5)
                            | numbit,
                        ext_word,
                    ))
                } else if addr_val < 0x40 {
                    Ok(words(
                        0x0A0080
                            | js
                            | ((addr_val & 0x3F) << 8)
                            | (s << 6)
                            | (set_bit << 5)
                            | numbit,
                        ext_word,
                    ))
                } else if addr_val == ext_word {
                    let ea_bits = 0b110_000u32;
                    Ok(words(
                        0x0A4080 | js | (ea_bits << 8) | (s << 6) | (set_bit << 5) | numbit,
                        ext_word,
                    ))
                } else {
                    Err(enc_err("bit branch address must be in pp, qq, or aa range"))
                }
            }
            BitTarget::Ea { space, ea } => {
                let (ea_bits, _) = encode_ea(ea, sym, pc)?;
                let s = ast_space_bit(*space);
                Ok(words(
                    0x0A4080 | js | ((ea_bits as u32) << 8) | (s << 6) | (set_bit << 5) | numbit,
                    ext_word,
                ))
            }
            BitTarget::Reg(reg) => {
                let reg_idx = reg.index() as u32;
                Ok(words(
                    0x0AC000 | js | ((reg_idx & 0x3F) << 8) | (set_bit << 5) | numbit,
                    ext_word,
                ))
            }
        }
    }
}

// ---- Loop encoders ----

fn encode_do(
    source: &LoopSource,
    end_addr: &Expr,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let end = eval(end_addr, sym, pc)?.wrapping_sub(1) & 0xFFFFFF;
    encode_do_dor_inner(source, end, 0x00, "do", sym, pc)
}

fn encode_dor(
    source: &LoopSource,
    end_addr: &Expr,
    pc: u32,
    sym: &SymbolTable,
) -> Result<EncodedInstruction> {
    let end = eval(end_addr, sym, pc)?.wrapping_sub(1) & 0xFFFFFF;
    let rel = end.wrapping_sub(pc) & 0xFFFFFF;
    encode_do_dor_inner(source, rel, 0x10, "dor", sym, pc)
}

/// Shared implementation for DO/DOR encoding. `ext_word` is the end-address
/// word (absolute for DO, relative for DOR). `off` is 0x00 for DO, 0x10 for DOR.
fn encode_do_dor_inner(
    source: &LoopSource,
    ext_word: u32,
    off: u32,
    mnem: &str,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    match source {
        LoopSource::Imm(imm) => {
            let v = eval(imm, sym, pc)?;
            let (lo, hi) = pack_loop_count(v);
            Ok(words(0x060080 | off | (lo << 8) | hi, ext_word))
        }
        LoopSource::Reg(reg) => {
            let idx = reg.index() as u32;
            Ok(words(0x06C000 | off | ((idx & 0x3F) << 8), ext_word))
        }
        LoopSource::Aa { space, addr } => {
            let a = eval(addr, sym, pc)?;
            let s = ast_space_bit(*space);
            if a <= 0x3F {
                Ok(words(
                    0x060000 | off | ((a & 0x3F) << 8) | (s << 6),
                    ext_word,
                ))
            } else if a == ext_word {
                let ea_bits = 0b110_000u32;
                Ok(words(0x064000 | off | (ea_bits << 8) | (s << 6), ext_word))
            } else {
                Err(enc_err(&format!(
                    "{mnem}: absolute address exceeds 6-bit aa range ($0000-$003f)"
                )))
            }
        }
        LoopSource::Ea { space, ea } => {
            let s = ast_space_bit(*space);
            if let EffectiveAddress::AbsAddr(expr) | EffectiveAddress::ForceLongAbsAddr(expr) = ea {
                let a = eval(expr, sym, pc)?;
                if a <= 0x3F {
                    return Ok(words(
                        0x060000 | off | ((a & 0x3F) << 8) | (s << 6),
                        ext_word,
                    ));
                }
            }
            let (ea_bits, _) = encode_ea(ea, sym, pc)?;
            Ok(words(
                0x064000 | off | ((ea_bits as u32) << 8) | (s << 6),
                ext_word,
            ))
        }
    }
}

fn encode_rep(source: &RepSource, sym: &SymbolTable, pc: u32) -> Result<EncodedInstruction> {
    match source {
        RepSource::Imm(imm) => {
            let v = eval(imm, sym, pc)?;
            let (lo, hi) = pack_loop_count(v);
            Ok(word(0x0600A0 | (lo << 8) | hi))
        }
        RepSource::Reg(reg) => {
            let idx = reg.index() as u32;
            Ok(word(0x06C020 | ((idx & 0x3F) << 8)))
        }
        RepSource::Aa { space, addr } => {
            let a = eval(addr, sym, pc)?;
            let s = ast_space_bit(*space);
            if a <= 0x3F {
                Ok(word(0x060020 | ((a & 0x3F) << 8) | (s << 6)))
            } else {
                let ea_bits = 0b110_000u32;
                Ok(words(0x064020 | (ea_bits << 8) | (s << 6), a & 0xFFFFFF))
            }
        }
        RepSource::Ea { space, ea } => {
            let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
            let s = ast_space_bit(*space);
            let w0 = 0x064020 | ((ea_bits as u32) << 8) | (s << 6);
            Ok(with_ext(w0, ext))
        }
    }
}

// ---- Move encoders ----

fn is_l_composite(reg: &Register) -> bool {
    matches!(
        reg,
        Register::A10
            | Register::B10
            | Register::RegX
            | Register::RegY
            | Register::Ab
            | Register::Ba
    )
}

fn encode_movec_reg(src: &Register, dst: &Register, _w: bool) -> Result<EncodedInstruction> {
    if is_l_composite(src) || is_l_composite(dst) {
        return Err(enc_err("L-move register not valid in movec"));
    }
    let src_idx = src.index() as u32;
    let dst_idx = dst.index() as u32;
    // Template: 00000100W1eeeeee101ddddd
    // W=0: src in ddddd (0x20+), dst in eeeeee. W=1: vice versa.
    if src_idx >= 0x20 {
        // W=0: src goes to ddddd (numreg1), dst goes to eeeeee (numreg2)
        Ok(word(0x0440A0 | ((dst_idx & 0x3F) << 8) | (src_idx & 0x1F)))
    } else {
        // W=1: dst goes to ddddd (numreg1), src goes to eeeeee (numreg2)
        Ok(word(
            0x0440A0 | (1 << 15) | ((src_idx & 0x3F) << 8) | (dst_idx & 0x1F),
        ))
    }
}

fn encode_movec_aa(
    space: MemorySpace,
    addr: &Expr,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    if is_l_composite(reg) {
        return Err(enc_err("L-move register not valid in movec"));
    }
    let a = eval(addr, sym, pc)?;
    let s = ast_space_bit(space);
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 15 } else { 0 };
    if a > 0x3F {
        // Address exceeds 6-bit aa range; promote to EA absolute-address form.
        // Template: 00000101W1MMMRRR0s1ddddd  with MMMRRR=110_000 (abs addr)
        let ea_bits = 0b110_000u32;
        return Ok(words(
            0x054020 | (ea_bits << 8) | w_bit | (s << 6) | (reg_idx & 0x1F),
            a & 0xFFFFFF,
        ));
    }
    // Template: 00000101W0aaaaaa0s1ddddd
    Ok(word(
        0x050020 | ((a & 0x3F) << 8) | w_bit | (s << 6) | (reg_idx & 0x1F),
    ))
}

fn encode_movec_ea(
    space: MemorySpace,
    ea: &EffectiveAddress,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    if is_l_composite(reg) {
        return Err(enc_err("L-move register not valid in movec"));
    }
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let s = ast_space_bit(space);
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 15 } else { 0 };
    // Template: 00000101W1MMMRRR0s1ddddd
    let w0 = 0x054020 | ((ea_bits as u32) << 8) | w_bit | (s << 6) | (reg_idx & 0x1F);
    Ok(with_ext(w0, ext))
}

fn encode_movec_imm(
    imm: &Expr,
    reg: &Register,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    if is_l_composite(reg) {
        return Err(enc_err("L-move register not valid in movec"));
    }
    let v = eval(imm, sym, pc)?;
    let reg_idx = reg.index() as u32;
    // Template: 00000101iiiiiiii101ddddd
    Ok(word(0x0500A0 | ((v & 0xFF) << 8) | (reg_idx & 0x3F)))
}

fn encode_movem_ea(
    ea: &EffectiveAddress,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 15 } else { 0 };
    // Template: 00000111W1MMMRRR10dddddd
    let w0 = 0x074080 | ((ea_bits as u32) << 8) | w_bit | (reg_idx & 0x3F);
    Ok(with_ext(w0, ext))
}

fn encode_movem_aa(
    addr: &Expr,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let a = eval(addr, sym, pc)?;
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 15 } else { 0 };
    if a > 0x3F {
        // Address exceeds 6-bit aa range; promote to EA absolute-address form.
        // Template: 00000111W1MMMRRR10dddddd  with MMMRRR=110_000 (abs addr)
        let ea_bits = 0b110_000u32;
        return Ok(words(
            0x074080 | (ea_bits << 8) | w_bit | (reg_idx & 0x3F),
            a & 0xFFFFFF,
        ));
    }
    Ok(word(
        0x070000 | ((a & 0x3F) << 8) | w_bit | (reg_idx & 0x3F),
    ))
}

/// Normalize 16-bit DSP56001 peripheral addresses to 24-bit DSP56300 equivalents.
/// The a56 assembler targets DSP56001 which has a 16-bit address space where
/// peripherals live at $FF80-$FFFF. DSP56300 uses $FFFF80-$FFFFFF.
fn normalize_periph_addr(pa: u32) -> u32 {
    if (0xFF80..0x10000).contains(&pa) {
        pa | 0xFF0000
    } else {
        pa
    }
}

fn encode_movep23(
    periph_space: MemorySpace,
    periph_addr: &Expr,
    ea_space: MemorySpace,
    ea: &EffectiveAddress,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let pa = normalize_periph_addr(eval(periph_addr, sym, pc)?);

    // Ambiguous absolute-address operands: swap if ea is in peripheral range.
    if pa < 0xFFFF80 {
        if let EffectiveAddress::AbsAddr(ea_expr) | EffectiveAddress::ForceLongAbsAddr(ea_expr) = ea
        {
            let ea_val = normalize_periph_addr(eval(ea_expr, sym, pc)?);
            if ea_val >= 0xFFFF80 {
                let swapped_ea = EffectiveAddress::ForceLongAbsAddr(periph_addr.clone());
                return encode_movep23(ea_space, ea_expr, periph_space, &swapped_ea, true, sym, pc);
            }
        }
        return Err(enc_err("movep peripheral address must be >= $ffff80"));
    }

    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let w_bit = if w { 1u32 << 15 } else { 0 };

    if pa >= 0xFFFFC0 {
        let offset = pa - 0xFFFFC0;
        let ps = ast_space_bit(periph_space);
        let es = ast_space_bit(ea_space);
        let w0 =
            0x084080 | (ps << 16) | ((ea_bits as u32) << 8) | w_bit | (es << 6) | (offset & 0x3F);
        Ok(with_ext(w0, ext))
    } else {
        let offset = pa - 0xFFFF80;
        let es = ast_space_bit(ea_space);
        let w0 = if periph_space == MemorySpace::X {
            0x074000 | ((ea_bits as u32) << 8) | w_bit | (es << 6) | (offset & 0x3F)
        } else {
            0x070080 | ((ea_bits as u32) << 8) | w_bit | (es << 6) | (offset & 0x3F)
        };
        Ok(with_ext(w0, ext))
    }
}

fn encode_movep23_imm(
    periph_space: MemorySpace,
    periph_addr: &Expr,
    imm: &Expr,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let pa = normalize_periph_addr(eval(periph_addr, sym, pc)?);
    let v = eval(imm, sym, pc)?;
    let ps = ast_space_bit(periph_space);
    if pa >= 0xFFFFC0 {
        let offset = pa - 0xFFFFC0;
        let ea_bits: u32 = 0b110_100;
        let w0 = 0x084080 | (ps << 16) | (ea_bits << 8) | (1 << 15) | (offset & 0x3F);
        Ok(words(w0, v & 0xFFFFFF))
    } else if pa >= 0xFFFF80 {
        let offset = pa - 0xFFFF80;
        let ea_bits: u32 = 0b110_100;
        let w0 = if periph_space == MemorySpace::X {
            0x074000 | (ea_bits << 8) | (1 << 15) | (offset & 0x3F)
        } else {
            0x070080 | (ea_bits << 8) | (1 << 15) | (offset & 0x3F)
        };
        Ok(words(w0, v & 0xFFFFFF))
    } else {
        Err(enc_err("movep immediate target must be peripheral address"))
    }
}

fn encode_movep_xqq(
    periph_addr: &Expr,
    ea_space: MemorySpace,
    ea: &EffectiveAddress,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let pa = eval(periph_addr, sym, pc)?;
    let offset = periph_offset_qq(pa)?;
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let es = ast_space_bit(ea_space);
    let w_bit = if w { 1u32 << 15 } else { 0 };
    // Template: 00000111W1MMMRRR0Sqqqqqq
    let w0 = 0x074000 | ((ea_bits as u32) << 8) | w_bit | (es << 6) | (offset & 0x3F);
    Ok(with_ext(w0, ext))
}

fn encode_movep_xqq_imm(
    periph_addr: &Expr,
    imm: &Expr,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let pa = eval(periph_addr, sym, pc)?;
    let offset = periph_offset_qq(pa)?;
    let v = eval(imm, sym, pc)?;
    let ea_bits: u32 = 0b110_100;
    // Template: 00000111W1MMMRRR0Sqqqqqq
    let w0 = 0x074000 | (ea_bits << 8) | (1 << 15) | (offset & 0x3F);
    Ok(words(w0, v & 0xFFFFFF))
}

fn encode_movep1(
    periph_space: MemorySpace,
    periph_addr: &Expr,
    ea: &EffectiveAddress,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let pa = normalize_periph_addr(eval(periph_addr, sym, pc)?);
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let w_bit = if w { 1u32 << 15 } else { 0 };

    if pa >= 0xFFFFC0 {
        // pp form: Template 0000100sW1MMMRRR01pppppp
        let offset = pa - 0xFFFFC0;
        let ps = ast_space_bit(periph_space);
        let w0 = 0x084040 | (ps << 16) | ((ea_bits as u32) << 8) | w_bit | (offset & 0x3F);
        Ok(with_ext(w0, ext))
    } else if pa >= 0xFFFF80 {
        // qq form: Template 000000001WMMMRRR0sqqqqqq (W at bit 14)
        let offset = pa - 0xFFFF80;
        let ps = ast_space_bit(periph_space);
        let w14 = if w { 1u32 << 14 } else { 0 };
        let w0 = 0x008000 | ((ea_bits as u32) << 8) | w14 | (ps << 6) | (offset & 0x3F);
        Ok(with_ext(w0, ext))
    } else {
        Err(enc_err(
            "movep peripheral address must be >= $ffff80 for P:ea form",
        ))
    }
}

fn encode_movep0(
    periph_space: MemorySpace,
    periph_addr: &Expr,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let pa = normalize_periph_addr(eval(periph_addr, sym, pc)?);
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 15 } else { 0 };

    if pa >= 0xFFFFC0 {
        // pp form: Template 0000100sW1dddddd00pppppp
        let offset = pa - 0xFFFFC0;
        let ps = ast_space_bit(periph_space);
        Ok(word(
            0x084000 | (ps << 16) | ((reg_idx & 0x3F) << 8) | w_bit | (offset & 0x3F),
        ))
    } else if pa >= 0xFFFF80 {
        // qq form: X:qq Template 00000100W1dddddd1q0qqqqq
        //          Y:qq Template 00000100W1dddddd0q1qqqqq
        let offset = pa - 0xFFFF80;
        let q_hi = (offset >> 5) & 1; // bit 5 of offset -> bit 6 of opcode
        let q_lo = offset & 0x1F; // bits 4-0 of offset -> bits 4-0 of opcode
        let w0 = if periph_space == MemorySpace::X {
            0x044080 | ((reg_idx & 0x3F) << 8) | w_bit | (q_hi << 6) | q_lo
        } else {
            0x044020 | ((reg_idx & 0x3F) << 8) | w_bit | (q_hi << 6) | q_lo
        };
        Ok(word(w0))
    } else {
        Err(enc_err(
            "movep peripheral address must be >= $ffff80 for R form",
        ))
    }
}

/// Map register to 3-bit sss encoding (Table 12-13).
fn reg_to_sss(reg: &Register) -> Result<u32> {
    match reg {
        Register::A1 => Ok(2),
        Register::B1 => Ok(3),
        Register::X0 => Ok(4),
        Register::Y0 => Ok(5),
        Register::X1 => Ok(6),
        Register::Y1 => Ok(7),
        _ => Err(enc_err("shift register must be a1/b1/x0/y0/x1/y1")),
    }
}

/// Map register to 3-bit qqq encoding (Table 12-13, S2 column).
/// qqq differs from sss: 2=A0, 3=B0 (vs A1, B1 for sss).
fn reg_to_qqq(reg: &Register) -> Result<u32> {
    match reg {
        Register::A0 => Ok(2),
        Register::B0 => Ok(3),
        Register::X0 => Ok(4),
        Register::Y0 => Ok(5),
        Register::X1 => Ok(6),
        Register::Y1 => Ok(7),
        _ => Err(enc_err("insert source must be a0/b0/x0/y0/x1/y1")),
    }
}

/// Compute 6-bit qq offset from a peripheral address ($FFFF80-$FFFFBF).
fn periph_offset_qq(addr: u32) -> Result<u32> {
    let addr = normalize_periph_addr(addr);
    if (0xFFFF80..0xFFFFC0).contains(&addr) {
        Ok(addr - 0xFFFF80)
    } else {
        Err(enc_err(
            "peripheral address must be in $ffff80-$ffffbf for qq form",
        ))
    }
}

fn encode_move_long_disp(
    space: MemorySpace,
    offset_reg: u8,
    offset: &Expr,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let off = eval(offset, sym, pc)? as i32;
    let off24 = (off as u32) & 0xFFFFFF;
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 6 } else { 0 };
    // Template: 0000101s01110RRR1WDDDDDD + 24-bit extension (s=0 for X, s=1 for Y)
    let base = if space == MemorySpace::X {
        0x0A7080
    } else {
        0x0B7080
    };
    Ok(words(
        base | ((offset_reg as u32) << 8) | w_bit | (reg_idx & 0x3F),
        off24,
    ))
}

fn encode_move_short_disp(
    space: MemorySpace,
    offset_reg: u8,
    offset: &Expr,
    reg: &Register,
    w: bool,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let off = eval(offset, sym, pc)? as i32;
    let off7 = (off as u32) & 0x7F;
    let reg_idx = reg.index() as u32;
    let w_bit = if w { 1u32 << 4 } else { 0 };
    let (xxx_hi, xxx_lo) = pack_xy_imm_offset(off7);
    let is_y = if space == MemorySpace::Y { 1u32 } else { 0 };
    Ok(word(
        0x020080
            | (xxx_hi << 11)
            | ((offset_reg as u32) << 8)
            | (xxx_lo << 6)
            | (is_y << 5)
            | w_bit
            | (reg_idx & 0xF),
    ))
}

// ---- Multiply/divide encoders ----

fn encode_mpyi(
    sign: Sign,
    imm: &Expr,
    src: &Register,
    dst: Acc,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    let k = if sign == Sign::Minus { 1u32 } else { 0 };
    let qq = match *src {
        Register::X0 => 0,
        Register::Y0 => 1,
        Register::X1 => 2,
        Register::Y1 => 3,
        _ => return Err(enc_err("mpyi source must be x0/y0/x1/y1")),
    };
    // Template: 000000010100000111qqdk00
    Ok(words(
        0x0141C0 | (qq << 4) | ((dst as u32) << 3) | (k << 2),
        v & 0xFFFFFF,
    ))
}

fn reg_to_qq(src: &Register) -> Result<u32> {
    match *src {
        Register::X0 => Ok(0),
        Register::Y0 => Ok(1),
        Register::X1 => Ok(2),
        Register::Y1 => Ok(3),
        _ => Err(enc_err("source must be x0/y0/x1/y1")),
    }
}

fn regs_to_qqqq(s1: &Register, s2: &Register) -> Result<u32> {
    match (*s1, *s2) {
        (Register::X0, Register::X0) => Ok(0x0),
        (Register::Y0, Register::Y0) => Ok(0x1),
        (Register::X1, Register::X0) => Ok(0x2),
        (Register::Y1, Register::Y0) => Ok(0x3),
        (Register::X0, Register::Y1) => Ok(0x4),
        (Register::Y0, Register::X0) => Ok(0x5),
        (Register::X1, Register::Y0) => Ok(0x6),
        (Register::Y1, Register::X1) => Ok(0x7),
        (Register::X1, Register::X1) => Ok(0x8),
        (Register::Y1, Register::Y1) => Ok(0x9),
        (Register::X0, Register::X1) => Ok(0xA),
        (Register::Y0, Register::Y1) => Ok(0xB),
        (Register::Y1, Register::X0) => Ok(0xC),
        (Register::X0, Register::Y0) => Ok(0xD),
        (Register::Y0, Register::X1) => Ok(0xE),
        (Register::X1, Register::Y1) => Ok(0xF),
        _ => Err(enc_err("invalid QQQQ register pair")),
    }
}

/// Encode MPYRI/MACI/MACRI (immediate multiply variants).
/// `kind`: 0x00=mpyi, 0x01=mpyri, 0x02=maci, 0x03=macri
fn encode_imm_mul(
    sign: Sign,
    imm: &Expr,
    src: &Register,
    dst: Acc,
    kind: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    let k = if sign == Sign::Minus { 1u32 } else { 0 };
    let qq = reg_to_qq(src)?;
    // Template: 000000010100000111qqdk{kind:2}
    Ok(words(
        0x0141C0 | (qq << 4) | ((dst as u32) << 3) | (k << 2) | kind,
        v & 0xFFFFFF,
    ))
}

fn encode_dmac(
    ss: u8,
    sign: Sign,
    s1: &Register,
    s2: &Register,
    dst: Acc,
) -> Result<EncodedInstruction> {
    let k = if sign == Sign::Minus { 1u32 } else { 0 };
    let qqqq = regs_to_qqqq(s1, s2)?;
    let d = dst as u32;
    // Template: 000000010010010s1sdkQQQQ
    // ss bits at positions 8 and 6 (non-contiguous)
    let ss_hi = ((ss as u32) >> 1) & 1; // bit 1 of ss -> position 8
    let ss_lo = (ss as u32) & 1; // bit 0 of ss -> position 6
    Ok(word(
        0x012480 | (ss_hi << 8) | (ss_lo << 6) | (d << 5) | (k << 4) | qqqq,
    ))
}

fn encode_mac_mpy_su(
    is_mac: bool,
    su: bool,
    sign: Sign,
    s1: &Register,
    s2: &Register,
    dst: Acc,
) -> Result<EncodedInstruction> {
    let k = if sign == Sign::Minus { 1u32 } else { 0 };
    let qqqq = regs_to_qqqq(s1, s2)?;
    let d = dst as u32;
    let s = if su { 0u32 } else { 1 };
    // MAC(su,uu): 00000001001001101sdkQQQQ
    // MPY(su,uu): 00000001001001111sdkQQQQ
    let base = if is_mac { 0x012680 } else { 0x012780 };
    Ok(word(base | (s << 6) | (d << 5) | (k << 4) | qqqq))
}

fn encode_div(src: &Register, dst: Acc) -> Result<EncodedInstruction> {
    let jj = match *src {
        Register::X0 => 0,
        Register::Y0 => 1,
        Register::X1 => 2,
        Register::Y1 => 3,
        _ => return Err(enc_err("div source must be x0/y0/x1/y1")),
    };
    // Template: 000000011000000001JJd000
    Ok(word(0x018040 | (jj << 4) | ((dst as u32) << 3)))
}

fn encode_cmpu(src: &Register, dst: Acc) -> Result<EncodedInstruction> {
    let d = dst as u32;
    let ggg = match *src {
        Register::A | Register::B => {
            if (*src == Register::A && dst == Acc::B) || (*src == Register::B && dst == Acc::A) {
                0
            } else {
                return Err(enc_err("cmpu: can't compare accumulator with itself"));
            }
        }
        Register::X0 => 4,
        Register::Y0 => 5,
        Register::X1 => 6,
        Register::Y1 => 7,
        _ => return Err(enc_err("cmpu source must be a/b/x0/y0/x1/y1")),
    };
    // Template: 00001100000111111111gggd
    Ok(word(0x0C1FF0 | ((ggg & 7) << 1) | d))
}

fn encode_tcc(
    cc: CondCode,
    acc: &Option<(Register, Register)>,
    r: &Option<(u8, u8)>,
) -> Result<EncodedInstruction> {
    let (r_src_bits, r_dst_bits) = match r {
        Some((s, d)) => (*s as u32, *d as u32),
        None => (0, 0),
    };

    match acc {
        // Template 3: R-register-only (no accumulator pair)
        // 00000010CCCC1ttt00000TTT
        None => Ok(word(
            0x020000 | ((cc as u32) << 12) | (1u32 << 11) | (r_src_bits << 8) | r_dst_bits,
        )),
        // Templates 1 and 2: accumulator pair (+/- R register pair)
        Some((s1, d1)) => {
            use dsp56300_core::REGISTERS_TCC;
            let src_idx = s1.index();
            let dst_idx = d1.index();
            let tcc_idx = REGISTERS_TCC
                .iter()
                .position(|pair| pair[0] == src_idx && pair[1] == dst_idx)
                .ok_or_else(|| enc_err("invalid tcc register pair"))?;

            let r_bit = if r.is_some() { 1u32 << 16 } else { 0 };
            // Template 1: 00000010CCCC00000JJJd000
            // Template 2: 00000011CCCC0ttt0JJJdTTT
            Ok(word(
                0x020000
                    | ((cc as u32) << 12)
                    | r_bit
                    | (r_src_bits << 8)
                    | ((tcc_idx as u32) << 3)
                    | r_dst_bits,
            ))
        }
    }
}

fn encode_mul_shift(
    mnem: MulShiftMnem,
    sign: Sign,
    src: &Register,
    shift: &Expr,
    dst: Acc,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    // MulShift QQ encoding differs from the standard qq mapping:
    // QQ=0:Y1, QQ=1:X0, QQ=2:Y0, QQ=3:X1
    let qq: u32 = match src {
        Register::Y1 => 0,
        Register::X0 => 1,
        Register::Y0 => 2,
        Register::X1 => 3,
        _ => return Err(enc_err("mul_shift source must be x0/y0/x1/y1")),
    };
    let d = dst as u32;
    let k = if sign == Sign::Minus { 1u32 } else { 0 };
    let n = eval(shift, sym, pc)?;
    if n > 31 {
        return Err(enc_err("mul_shift: shift amount must be 0-31"));
    }
    let op_bits: u32 = match mnem {
        MulShiftMnem::Mpy => 0b00,
        MulShiftMnem::Mpyr => 0b01,
        MulShiftMnem::Mac => 0b10,
        MulShiftMnem::Macr => 0b11,
    };
    // Template: 00000001000sssss11QQdk??
    //           bit16=1, bits12:8=sssss, bits7:6=11, bits5:4=QQ, bit3=d, bit2=k, bits1:0=op
    Ok(word(
        0x010000 | (n << 8) | (0b11 << 6) | (qq << 4) | (d << 3) | (k << 2) | op_bits,
    ))
}

// ---- Parallel instruction encoder ----

fn encode_parallel(
    alu: &ParallelAlu,
    pmove: &ParallelMove,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let alu_byte =
        alu.encode()
            .ok_or_else(|| enc_err(&format!("undefined ALU operation: '{alu}'")))? as u32;

    match pmove {
        ParallelMove::None => {
            // Pm2 NOP: bits 23:8 = 0x2000
            Ok(word(0x200000 | alu_byte))
        }
        ParallelMove::RegToReg { src, dst } => {
            // Pm2 register to register
            let src_idx = (src.index() as u32) & 0x1F;
            let dst_idx = (dst.index() as u32) & 0x1F;
            Ok(word(0x200000 | (src_idx << 13) | (dst_idx << 8) | alu_byte))
        }
        ParallelMove::EaUpdate { ea, dst: _ } => {
            // Pm2 EA update
            let (ea_bits, _) = encode_ea(ea, sym, pc)?;
            Ok(word(0x204000 | ((ea_bits as u32 & 0x1F) << 8) | alu_byte))
        }
        ParallelMove::ImmToReg { imm, dst } => {
            let v = eval(imm, sym, pc)?;
            let v24 = v & 0xFFFFFF;
            let reg_idx = (dst.index() as u32) & 0x1F;
            // PM3 short form: only for direct literal/frac tokens (not symbols
            // or expressions).  a56 always uses PM4 for symbol references.
            // For bare literals: PM3 when value fits in 8 bits.
            // For fractional literals on data ALU regs: PM3 when MSB-aligned.
            let is_bare_lit = matches!(imm, Expr::Literal(_));
            let is_frac = matches!(imm, Expr::Frac(_));
            // PM3 short form places 8-bit immediate in MSBs for "full" data ALU
            // registers (a, b, x0, x1, y0, y1) but NOT for accumulator parts
            // (a0/a1/a2/b0/b1/b2) where it's zero-extended.
            let is_msb_reg = matches!(
                dst,
                Register::A
                    | Register::B
                    | Register::X0
                    | Register::X1
                    | Register::Y0
                    | Register::Y1
            );
            let (pm3_ok, pm3_byte) = if is_bare_lit && v24 <= 0xFF {
                (true, v24 as u32)
            } else if imm.is_literal() && is_msb_reg && (v24 & 0xFFFF) == 0 {
                // MSB-aligned: value = byte << 16, fits in PM3 short form
                (true, (v24 >> 16) as u32)
            } else if is_frac && !dst.is_data_alu() && v24 <= 0xFF {
                (true, v24 as u32)
            } else {
                (false, 0)
            };
            if pm3_ok {
                Ok(word(
                    ((reg_idx | 0x20) << 16) | ((pm3_byte & 0xFF) << 8) | alu_byte,
                ))
            } else {
                // Upgrade to Pm4 long form (24-bit immediate via extension word)
                let (val_hi, val_lo) = pm4_reg_split(reg_idx);
                let w0 = (1 << 22)
                    | (val_hi << 20)
                    | (val_lo << 16)
                    | (1 << 15) // W=1 (read immediate to register)
                    | (1 << 14) // EA mode
                    | (0x34 << 8) // Immediate EA bits
                    | alu_byte;
                Ok(words(w0, v24))
            }
        }
        ParallelMove::XYMem {
            space,
            ea,
            reg,
            write,
        } => encode_parallel_xy_mem(*space, ea, reg, *write, alu_byte, sym, pc),
        ParallelMove::XYAbs {
            space,
            addr,
            reg,
            write,
            force_short,
        } => encode_parallel_xy_abs(*space, addr, reg, *write, *force_short, alu_byte, sym, pc),
        ParallelMove::XYDouble {
            x_ea,
            x_reg,
            y_ea,
            y_reg,
            x_write,
            y_write,
        } => encode_parallel_xy_double(
            x_ea, x_reg, y_ea, y_reg, *x_write, *y_write, alu_byte, sym, pc,
        ),
        ParallelMove::LMem { ea, reg, write } => {
            encode_parallel_l_mem(ea, reg, *write, alu_byte, sym, pc)
        }
        ParallelMove::LAbs {
            addr,
            reg,
            write,
            force_short,
        } => encode_parallel_l_abs(addr, reg, *write, *force_short, alu_byte, sym, pc),
        ParallelMove::LImm { imm, reg } => encode_parallel_l_imm(imm, reg, alu_byte, sym, pc),
        ParallelMove::XReg {
            ea,
            x_reg,
            s2,
            d2,
            write,
        } => encode_parallel_pm1_x_ea(ea, x_reg, s2, d2, *write, alu_byte, sym, pc),
        ParallelMove::XImmReg { imm, x_reg, s2, d2 } => {
            encode_parallel_pm1_x_imm(imm, x_reg, s2, d2, alu_byte, sym, pc)
        }
        ParallelMove::RegY {
            s1,
            d1,
            ea,
            y_reg,
            write,
        } => encode_parallel_pm1_y_ea(s1, d1, ea, y_reg, *write, alu_byte, sym, pc),
        ParallelMove::RegYImm { s1, d1, imm, y_reg } => {
            encode_parallel_pm1_y_imm(s1, d1, imm, y_reg, alu_byte, sym, pc)
        }
        ParallelMove::Pm0 {
            acc,
            space,
            ea,
            data_reg,
        } => encode_parallel_pm0(acc, *space, ea, data_reg, alu_byte, sym, pc),
        ParallelMove::Ifcc { cc } => {
            // IFcc: 0010 0000 0010 CCCC alu_byte
            let cc_bits = *cc as u32;
            Ok(word(0x202000 | (cc_bits << 8) | alu_byte))
        }
        ParallelMove::IfccU { cc } => {
            // IFcc.U: 0010 0000 0011 CCCC alu_byte
            let cc_bits = *cc as u32;
            Ok(word(0x203000 | (cc_bits << 8) | alu_byte))
        }
    }
}

fn encode_parallel_xy_mem(
    space: MemorySpace,
    ea: &EffectiveAddress,
    reg: &Register,
    write: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let s = ast_space_bit(space);
    let reg_idx = reg.index() as u32;
    let w_bit = if write { 1u32 << 15 } else { 0 };

    let (val_hi, val_lo) = pm4_reg_split(reg_idx);

    let w0 = (1 << 22) // Pm4/Pm5 class bit
        | (val_hi << 20)
        | (s << 19)
        | (val_lo << 16)
        | w_bit
        | (1 << 14) // EA mode (vs absolute)
        | ((ea_bits as u32) << 8)
        | alu_byte;

    Ok(with_ext(w0, ext))
}

#[allow(clippy::too_many_arguments)]
fn encode_parallel_xy_abs(
    space: MemorySpace,
    addr: &Expr,
    reg: &Register,
    write: bool,
    force_short: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let addr_val = eval(addr, sym, pc)?;
    let s = ast_space_bit(space);
    let reg_idx = reg.index() as u32;
    let w_bit = if write { 1u32 << 15 } else { 0 };

    let (val_hi, val_lo) = pm4_reg_split(reg_idx);

    // Use short aa form when: force_short (<), or literal address that fits in 6 bits.
    // Must match pmove_ext_size() to keep pass1/pass2 consistent.
    let use_short = force_short || (addr.as_u32().is_some() && (addr_val & 0xFFFFFF) <= 0x3F);
    if use_short {
        // Short absolute (aa): 1-word, bit 14 = 0
        let w0 = (1 << 22)
            | (val_hi << 20)
            | (s << 19)
            | (val_lo << 16)
            | w_bit
            | ((addr_val & 0x3F) << 8)
            | alu_byte;
        Ok(word(w0))
    } else {
        // Long absolute: 2-word with EA mode (bit 14 = 1), ea = AbsAddr (110_000)
        let w0 = (1 << 22)
            | (val_hi << 20)
            | (s << 19)
            | (val_lo << 16)
            | w_bit
            | (1 << 14)
            | (0b110_000 << 8)
            | alu_byte;
        Ok(words(w0, addr_val & 0xFFFFFF))
    }
}

fn encode_parallel_l_mem(
    ea: &EffectiveAddress,
    reg: &Register,
    write: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let w_bit = if write { 1u32 << 15 } else { 0 };
    let lreg_idx = l_reg_idx(reg)?;
    let (bit2, bits10) = l_reg_split(lreg_idx);
    let w0 = (1 << 22) // Pm4/Pm5 class bit
        | (bit2 << 19)
        | (bits10 << 16)
        | w_bit
        | (1 << 14) // EA mode
        | ((ea_bits as u32) << 8)
        | alu_byte;

    Ok(with_ext(w0, ext))
}

#[allow(clippy::too_many_arguments)]
fn encode_parallel_xy_double(
    x_ea: &EffectiveAddress,
    x_reg: &Register,
    y_ea: &EffectiveAddress,
    y_reg: &Register,
    x_write: bool,
    y_write: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    // Pm8: Dual X:Y move
    // This is complex -- bits 23:20 encode the move type (8-15)
    // x_reg: bits 19:18 (x0=0, x1=1, a=2, b=3)
    // y_reg: bits 17:16 (y0=0, y1=1, a=2, b=3)
    let x_reg_bits = match *x_reg {
        Register::X0 => 0u32,
        Register::X1 => 1,
        Register::A => 2,
        Register::B => 3,
        _ => return Err(enc_err("pm8 x register must be x0/x1/a/b")),
    };
    let y_reg_bits = match *y_reg {
        Register::Y0 => 0u32,
        Register::Y1 => 1,
        Register::A => 2,
        Register::B => 3,
        _ => return Err(enc_err("pm8 y register must be y0/y1/a/b")),
    };

    let (x_ea_bits, x_ext) = encode_ea(x_ea, sym, pc)?;
    let (y_ea_bits, y_ext) = encode_ea(y_ea, sym, pc)?;

    let w_bit = if x_write { 1u32 << 15 } else { 0 };
    let dir_bit = if y_write { 1u32 << 22 } else { 0 };

    // EA encoding for pm8 is different from standard.
    // X uses 5-bit MMRRR. Y uses only 4 bits: mm (2 bits at 21:20) and rr (2 bits
    // at 14:13). Bit 2 of the Y register (bank select) is implicit -- it is the
    // complement of X EA bit 2, since X and Y must use different register banks.
    let ea1 = (x_ea_bits as u32) & 0x1F;
    let ea2_lo = (y_ea_bits as u32) & 0x3;
    let ea2_hi = ((y_ea_bits as u32) >> 3) & 0x3;

    let w0 = (1 << 23) // pm8 marker (bit 23 set)
        | dir_bit
        | (ea2_hi << 20)
        | (x_reg_bits << 18)
        | (y_reg_bits << 16)
        | w_bit
        | (ea2_lo << 13)
        | (ea1 << 8)
        | alu_byte;

    let has_ext = x_ext.is_some() || y_ext.is_some();
    if has_ext {
        Ok(words(w0, x_ext.or(y_ext).unwrap_or(0)))
    } else {
        Ok(word(w0))
    }
}

fn l_reg_idx(reg: &Register) -> Result<u32> {
    Ok(match *reg {
        Register::A10 => 0,
        Register::B10 => 1,
        Register::RegX => 2,
        Register::RegY => 3,
        Register::A | Register::A0 | Register::A1 | Register::A2 => 4,
        Register::B | Register::B0 | Register::B1 | Register::B2 => 5,
        Register::Ab => 6,
        Register::Ba => 7,
        _ => return Err(enc_err("invalid L-move register")),
    })
}

fn encode_parallel_l_abs(
    addr: &Expr,
    reg: &Register,
    write: bool,
    force_short: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let addr_val = eval(addr, sym, pc)?;
    let w_bit = if write { 1u32 << 15 } else { 0 };
    let lreg = l_reg_idx(reg)?;
    let (bit2, bits10) = l_reg_split(lreg);

    let use_short = force_short || (addr.as_u32().is_some() && (addr_val & 0xFFFFFF) <= 0x3F);
    if use_short {
        // Short absolute (aa): 1-word, bit 14 = 0
        let w0 =
            (1 << 22) | (bit2 << 19) | (bits10 << 16) | w_bit | ((addr_val & 0x3F) << 8) | alu_byte;
        Ok(word(w0))
    } else {
        // Long absolute: 2-word with EA mode (bit 14 = 1), ea = AbsAddr (110_000)
        let w0 = (1 << 22)
            | (bit2 << 19)
            | (bits10 << 16)
            | w_bit
            | (1 << 14)
            | (0b110_000 << 8)
            | alu_byte;
        Ok(words(w0, addr_val & 0xFFFFFF))
    }
}

fn encode_parallel_l_imm(
    imm: &Expr,
    reg: &Register,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    let lreg = l_reg_idx(reg)?;
    let (bit2, bits10) = l_reg_split(lreg);
    let w0 = (1 << 22) // Pm4/Pm5 class bit
        | (bit2 << 19) | (bits10 << 16) | (1 << 15) | (1 << 14) | (0x34 << 8) | alu_byte;
    Ok(words(w0, v & 0xFFFFFF))
}

fn pm1_d1_bits(reg: &Register, is_x: bool) -> Result<u32> {
    if is_x {
        // X space d1: x0=0, x1=1, a=2, b=3 (bits 19:18)
        Ok(match *reg {
            Register::X0 => 0,
            Register::X1 => 1,
            Register::A => 2,
            Register::B => 3,
            _ => return Err(enc_err("pm1 x d1 must be x0/x1/a/b")),
        })
    } else {
        // Y space d2: y0=0, y1=1, a=2, b=3 (bits 17:16)
        Ok(match *reg {
            Register::Y0 => 0,
            Register::Y1 => 1,
            Register::A => 2,
            Register::B => 3,
            _ => return Err(enc_err("pm1 y d2 must be y0/y1/a/b")),
        })
    }
}

fn pm1_acc_bit(reg: &Register) -> Result<u32> {
    Ok(match *reg {
        Register::A => 0,
        Register::B => 1,
        _ => return Err(enc_err("pm1 source/dest must be a or b")),
    })
}

#[allow(clippy::too_many_arguments)]
fn encode_parallel_pm1_x_ea(
    ea: &EffectiveAddress,
    x_reg: &Register,
    s2: &Register,
    d2: &Register,
    write: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let d1 = pm1_d1_bits(x_reg, true)?;
    let s2_bit = pm1_acc_bit(s2)?;
    let d2_bit = match *d2 {
        Register::Y0 => 0u32,
        Register::Y1 => 1,
        _ => return Err(enc_err("pm1 x d2 must be y0/y1")),
    };
    let w_bit = if write { 1u32 << 15 } else { 0 };
    // Pm1 X space: bits 23:20=0001, bit 14=0 (X space)
    // bits 19:18=d1, bit 17=s2, bit 16=d2, bit 15=W, bits 13:8=EA
    let w0 = (1 << 20)
        | (d1 << 18)
        | (s2_bit << 17)
        | (d2_bit << 16)
        | w_bit
        | ((ea_bits as u32) << 8)
        | alu_byte;
    Ok(with_ext(w0, ext))
}

fn encode_parallel_pm1_x_imm(
    imm: &Expr,
    x_reg: &Register,
    s2: &Register,
    d2: &Register,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    let d1 = pm1_d1_bits(x_reg, true)?;
    let s2_bit = pm1_acc_bit(s2)?;
    let d2_bit = match *d2 {
        Register::Y0 => 0u32,
        Register::Y1 => 1,
        _ => return Err(enc_err("pm1 x imm d2 must be y0/y1")),
    };
    // Pm1 X immediate: EA mode 0x34 (immediate), value in word1, W=1
    let w0 = (1 << 20)
        | (d1 << 18)
        | (s2_bit << 17)
        | (d2_bit << 16)
        | (1 << 15)
        | (0x34 << 8)
        | alu_byte;
    Ok(words(w0, v as u32 & 0xFFFFFF))
}

#[allow(clippy::too_many_arguments)]
fn encode_parallel_pm1_y_ea(
    s1: &Register,
    d1: &Register,
    ea: &EffectiveAddress,
    y_reg: &Register,
    write: bool,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let s1_bit = pm1_acc_bit(s1)?;
    let d1_bit = match *d1 {
        Register::X0 => 0u32,
        Register::X1 => 1,
        _ => return Err(enc_err("pm1 y d1 must be x0/x1")),
    };
    let d2 = pm1_d1_bits(y_reg, false)?;
    let w_bit = if write { 1u32 << 15 } else { 0 };
    // Pm1 Y space: bits 23:20=0001, bit 14=1 (Y space)
    // bit 19=s1, bit 18=d1, bits 17:16=d2, bit 15=W, bits 13:8=EA
    let w0 = (1 << 20)
        | (s1_bit << 19)
        | (d1_bit << 18)
        | (d2 << 16)
        | w_bit
        | (1 << 14)
        | ((ea_bits as u32) << 8)
        | alu_byte;
    Ok(with_ext(w0, ext))
}

fn encode_parallel_pm1_y_imm(
    s1: &Register,
    d1: &Register,
    imm: &Expr,
    y_reg: &Register,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let v = eval(imm, sym, pc)?;
    let s1_bit = pm1_acc_bit(s1)?;
    let d1_bit = match *d1 {
        Register::X0 => 0u32,
        Register::X1 => 1,
        _ => return Err(enc_err("pm1 y imm d1 must be x0/x1")),
    };
    let d2 = pm1_d1_bits(y_reg, false)?;
    // Pm1 Y immediate: EA mode 0x34 (immediate), value in word1, W=1, bit 14=1
    let w0 = (1 << 20)
        | (s1_bit << 19)
        | (d1_bit << 18)
        | (d2 << 16)
        | (1 << 15)
        | (1 << 14)
        | (0x34 << 8)
        | alu_byte;
    Ok(words(w0, v as u32 & 0xFFFFFF))
}

fn encode_parallel_pm0(
    acc: &Register,
    space: MemorySpace,
    ea: &EffectiveAddress,
    _data_reg: &Register,
    alu_byte: u32,
    sym: &SymbolTable,
    pc: u32,
) -> Result<EncodedInstruction> {
    let (ea_bits, ext) = encode_ea(ea, sym, pc)?;
    let acc_bit = pm1_acc_bit(acc)?;
    let space_bit = if space == MemorySpace::Y { 1u32 } else { 0 };
    // Pm0 (X:R/R:Y Class II): 0000 100d xSmm mrrr aaaa aaaa
    // bits 23:17=0000100, bit 16=acc(A/B), bit 15=space(X/Y), bits 13:8=EA
    let w0 = 0x080000 | (acc_bit << 16) | (space_bit << 15) | ((ea_bits as u32) << 8) | alu_byte;
    Ok(with_ext(w0, ext))
}
