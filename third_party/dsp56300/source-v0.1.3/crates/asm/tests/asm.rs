#![allow(clippy::identity_op)]

use dsp56300_asm::ast::{Expr, MemorySpace};
use dsp56300_asm::*;

fn roundtrip(asm: &str, pc: u32) {
    let result =
        assemble_line(asm, pc).unwrap_or_else(|e| panic!("assemble '{}' failed: {}", asm, e));
    let (disasm, _len) = dsp56300_disasm::disassemble(pc, result.word0, result.word1.unwrap_or(0));
    assert_eq!(
        disasm,
        asm,
        "round-trip failed for '{}': assembled to {:06x}{}, disassembled to '{}'",
        asm,
        result.word0,
        result
            .word1
            .map(|w| format!(" {:06x}", w))
            .unwrap_or_default(),
        disasm
    );
}

// --- Zero-operand instructions ---

#[test]
fn test_nop() {
    roundtrip("nop", 0);
}

#[test]
fn test_rts() {
    roundtrip("rts", 0);
}

#[test]
fn test_rti() {
    roundtrip("rti", 0);
}

#[test]
fn test_reset() {
    roundtrip("reset", 0);
}

#[test]
fn test_stop() {
    roundtrip("stop", 0);
}

#[test]
fn test_wait() {
    roundtrip("wait", 0);
}

#[test]
fn test_enddo() {
    roundtrip("enddo", 0);
}

#[test]
fn test_illegal() {
    roundtrip("illegal", 0);
}

// --- Single accumulator ---

#[test]
fn test_inc_a() {
    roundtrip("inc a", 0);
}

#[test]
fn test_inc_b() {
    roundtrip("inc b", 0);
}

#[test]
fn test_dec_a() {
    roundtrip("dec a", 0);
}

#[test]
fn test_dec_b() {
    roundtrip("dec b", 0);
}

// --- Immediate ALU ---

#[test]
fn test_add_imm() {
    roundtrip("add #$3f,a", 0);
}

#[test]
fn test_add_imm_b() {
    roundtrip("add #$3f,b", 0);
}

// --- Jumps ---

#[test]
fn test_jmp() {
    roundtrip("jmp $0042", 0);
}

#[test]
fn test_jsr() {
    roundtrip("jsr $0042", 0);
}

// --- Bit operations ---

#[test]
fn test_bclr_pp() {
    roundtrip("bclr #3,x:$ffffc5", 0);
}

#[test]
fn test_bset_reg() {
    roundtrip("bset #5,sr", 0);
}

// --- Bit branch ---

#[test]
fn test_jclr_pp() {
    roundtrip("jclr #0,x:$ffffc5,$1234", 0);
}

// --- Loop ---

#[test]
fn test_rep_imm() {
    roundtrip("rep #$20", 0);
}

// --- Movec ---

#[test]
fn test_movec_imm() {
    roundtrip("movec #$02,sr", 0);
}

// --- andi/ori ---

#[test]
fn test_andi() {
    roundtrip("andi #$fe,mr", 0);
}

#[test]
fn test_ori() {
    roundtrip("ori #$03,ccr", 0);
}

// --- Tcc ---

#[test]
fn test_tcc() {
    roundtrip("tcc b,a", 0);
}

#[test]
fn test_tcc_with_r() {
    roundtrip("tcc b,a r2,r6", 0);
}

#[test]
fn test_tcc_r_only() {
    roundtrip("tcc r2,r6", 0);
}

// --- MulShift (mpy/mpyr/mac/macr S,#n,D) ---

#[test]
fn test_mpy_shift() {
    roundtrip("mpy +x0,#0,a", 0);
    roundtrip("mpy -x0,#0,a", 0);
    roundtrip("mpy +y1,#31,b", 0);
}

#[test]
fn test_mpyr_shift() {
    roundtrip("mpyr +x1,#31,a", 0);
    roundtrip("mpyr -y0,#10,b", 0);
}

#[test]
fn test_mac_shift() {
    roundtrip("mac +x0,#5,a", 0);
    roundtrip("mac -y1,#17,b", 0);
}

#[test]
fn test_macr_shift() {
    roundtrip("macr +y0,#8,a", 0);
    roundtrip("macr -x1,#0,b", 0);
}

#[test]
fn test_mul_shift_all_registers() {
    // Verify all 4 QQ registers encode correctly
    roundtrip("mpy +x0,#5,a", 0);
    roundtrip("mpy +y0,#5,a", 0);
    roundtrip("mpy +x1,#5,a", 0);
    roundtrip("mpy +y1,#5,a", 0);
}

#[test]
fn test_mul_shift_encoding_values() {
    // Verify specific encoding for mpy +x0,#0,a
    // MulShift QQ: Y1=0, X0=1, Y0=2, X1=3
    let result = assemble_line("mpy +x0,#0,a", 0).unwrap();
    assert_eq!(result.word0, 0x0100D0);
    assert!(result.word1.is_none());

    // mac -y1,#17,b -> sssss=17, QQ=0, d=1, k=1, op=10
    let result = assemble_line("mac -y1,#17,b", 0).unwrap();
    assert_eq!(result.word0, 0x0111CE);

    // mpyr +x1,#31,a -> sssss=31, QQ=3, d=0, k=0, op=01
    let result = assemble_line("mpyr +x1,#31,a", 0).unwrap();
    assert_eq!(result.word0, 0x011FF1);

    // macr -y0,#8,b -> sssss=8, QQ=2, d=1, k=1, op=11
    let result = assemble_line("macr -y0,#8,b", 0).unwrap();
    assert_eq!(result.word0, 0x0108EF);
}

// --- Div ---

#[test]
fn test_div() {
    roundtrip("div x0,a", 0);
}

// --- Sub/Cmp/And immediate ---

#[test]
fn test_sub_imm() {
    roundtrip("sub #$10,a", 0);
}

#[test]
fn test_cmp_imm() {
    roundtrip("cmp #$20,b", 0);
}

#[test]
fn test_and_imm() {
    roundtrip("and #$0f,a", 0);
}

// --- Bit ops: all operations x reg form ---

#[test]
fn test_bchg_reg() {
    roundtrip("bchg #3,sr", 0);
}

#[test]
fn test_btst_reg() {
    roundtrip("btst #7,sr", 0);
}

#[test]
fn test_bclr_reg() {
    roundtrip("bclr #15,omr", 0);
}

// --- Bit ops: aa form ---

#[test]
fn test_bset_aa() {
    roundtrip("bset #5,x:$0020", 0);
}

#[test]
fn test_bclr_aa() {
    roundtrip("bclr #3,x:$0010", 0);
}

// --- Shifts ---

#[test]
fn test_asl_imm() {
    roundtrip("asl #$01,a,a", 0);
}

#[test]
fn test_asr_imm() {
    roundtrip("asr #$01,a,a", 0);
}

// --- Branches ---

#[test]
fn test_jcc() {
    roundtrip("jcc $0100", 0);
}

#[test]
fn test_jscc() {
    roundtrip("jscc $0100", 0);
}

// --- Loops ---

#[test]
fn test_do_imm() {
    roundtrip("do #$0010,$0051", 0);
}

#[test]
fn test_do_reg() {
    roundtrip("do lc,$0051", 0);
}

#[test]
fn test_rep_reg() {
    roundtrip("rep lc", 0);
}

// --- Movec ---

#[test]
fn test_movec_reg() {
    roundtrip("movec sr,r0", 0);
}

// --- Div/cmpu/norm ---

#[test]
fn test_cmpu() {
    roundtrip("cmpu x0,a", 0);
}

#[test]
fn test_norm() {
    roundtrip("norm r0,a", 0);
}

// --- Parallel ALU ---

#[test]
fn test_parallel_move() {
    roundtrip("move", 0);
}

#[test]
fn test_parallel_clr_a() {
    roundtrip("clr a", 0);
}

#[test]
fn test_parallel_add_x0_a() {
    roundtrip("add x0,a", 0);
}

#[test]
fn test_parallel_mac() {
    roundtrip("mac +x0,x0,a", 0);
}

// --- Data-driven exhaustive round-trip test ---

/// Test round-trip by disassembling a known opcode, then reassembling.
fn roundtrip_opcode(pc: u32, w0: u32, w1: u32) {
    let (disasm, _len) = dsp56300_disasm::disassemble(pc, w0, w1);

    // Skip instructions that disassemble to "???" (invalid) or "dc $xxxx"
    if disasm.starts_with("???") || disasm.starts_with("dc ") {
        return;
    }

    let result = match assemble_line(&disasm, pc) {
        Ok(r) => r,
        Err(e) => {
            panic!(
                "assemble failed for '{}' (from {:06x} {:06x}): {}",
                disasm, w0, w1, e
            );
        }
    };

    // Verify semantic equivalence: re-disassemble and compare text.
    // The assembler may choose a shorter encoding (e.g. short Jcc vs long
    // JccEa), so bitwise comparison is too strict.
    let (redisasm, _) = dsp56300_disasm::disassemble(pc, result.word0, result.word1.unwrap_or(0));
    assert_eq!(
        redisasm,
        disasm,
        "semantic roundtrip failed for '{disasm}' (orig {:06x} {:06x} -> asm {:06x} {:06x} -> '{redisasm}')",
        w0,
        w1,
        result.word0,
        result.word1.unwrap_or(0)
    );
}

#[test]
fn test_sweep_zero_operand() {
    // nop, rts, rti, reset, stop, wait, enddo, illegal
    for opcode in [
        0x000000, 0x00000C, 0x000004, 0x000084, 0x000087, 0x000086, 0x00008C, 0x000005,
    ] {
        roundtrip_opcode(0, opcode, 0);
    }
}

#[test]
fn test_sweep_inc_dec() {
    // inc a, inc b, dec a, dec b
    for opcode in [0x000008, 0x000009, 0x00000A, 0x00000B] {
        roundtrip_opcode(0, opcode, 0);
    }
}

#[test]
fn test_sweep_imm_alu() {
    // add #xx,a/b, sub #xx,a/b, cmp #xx,a/b, and #xx,a/b
    for imm in [0u32, 1, 0x1F, 0x3F] {
        for d in [0u32, 1] {
            let add = 0x014080 | (imm << 8) | (d << 3);
            roundtrip_opcode(0, add, 0);
            let sub = 0x014084 | (imm << 8) | (d << 3);
            roundtrip_opcode(0, sub, 0);
            let cmp = 0x014085 | (imm << 8) | (d << 3);
            roundtrip_opcode(0, cmp, 0);
            let and = 0x014086 | (imm << 8) | (d << 3);
            roundtrip_opcode(0, and, 0);
        }
    }
}

#[test]
fn test_sweep_andi_ori() {
    // andi #xx,mr/ccr/omr
    for val in [0x00u32, 0xFE, 0xFF] {
        for ee in 0..3u32 {
            roundtrip_opcode(0, 0x0000B8 | (val << 8) | ee, 0);
            roundtrip_opcode(0, 0x0000F8 | (val << 8) | ee, 0);
        }
    }
}

#[test]
fn test_sweep_bit_ops_reg() {
    // bclr/bset/bchg/btst #b,reg
    // Use SR (0x39) as test register
    let reg = 0x39u32;
    for bit in [0u32, 5, 15] {
        roundtrip_opcode(0, 0x0AC040 | (reg << 8) | bit, 0); // bclr
        roundtrip_opcode(0, 0x0AC060 | (reg << 8) | bit, 0); // bset
        roundtrip_opcode(0, 0x0BC040 | (reg << 8) | bit, 0); // bchg
        if bit < 16 {
            roundtrip_opcode(0, 0x0BC060 | (reg << 8) | bit, 0); // btst (4-bit)
        }
    }
}

#[test]
fn test_sweep_bit_ops_pp() {
    // bclr/bset/bchg/btst #b,x:$ffffxx
    let pp = 5u32; // offset from $ffffc0
    for bit in [0u32, 3, 15] {
        for s in [0u32, 1] {
            roundtrip_opcode(0, 0x0A8000 | (pp << 8) | (s << 6) | bit, 0); // bclr
            roundtrip_opcode(0, 0x0A8000 | (pp << 8) | (s << 6) | (1 << 5) | bit, 0); // bset
            roundtrip_opcode(0, 0x0B8000 | (pp << 8) | (s << 6) | bit, 0); // bchg
            roundtrip_opcode(0, 0x0B8000 | (pp << 8) | (s << 6) | (1 << 5) | bit, 0); // btst
        }
    }
}

#[test]
fn test_sweep_jmp_jsr() {
    for addr in [0u32, 0x42, 0xFFF] {
        roundtrip_opcode(0, 0x0C0000 | addr, 0); // jmp
        roundtrip_opcode(0, 0x0D0000 | addr, 0); // jsr
    }
}

#[test]
fn test_sweep_jcc() {
    // Jcc abs (0x0AF0xx + ext word) - all condition codes
    for cc in 0..16u32 {
        roundtrip_opcode(0, 0x0AF0A0 | cc, 0x100);
    }
    // Jscc abs (0x0BF0xx + ext word) - all condition codes
    for cc in 0..16u32 {
        roundtrip_opcode(0, 0x0BF0A0 | cc, 0x100);
    }
}

#[test]
fn test_sweep_do_rep() {
    // do #imm,p:addr
    for count in [1u32, 16, 255] {
        let lo = count & 0xFF;
        let hi = (count >> 8) & 0xF;
        roundtrip_opcode(0, 0x060080 | (lo << 8) | hi, 0x0050);
    }

    // rep #imm
    for count in [1u32, 0x20, 0xFF] {
        let lo = count & 0xFF;
        let hi = (count >> 8) & 0xF;
        roundtrip_opcode(0, 0x0600A0 | (lo << 8) | hi, 0);
    }
}

#[test]
fn test_sweep_movec_imm() {
    // movec #xx,reg (reg idx 0x20-0x3F, i.e. SR=0x39, OMR=0x3A, etc)
    for val in [0u32, 2, 0xFF] {
        for reg in [0x39u32, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F] {
            roundtrip_opcode(0, 0x0500A0 | (val << 8) | reg, 0);
        }
    }
}

#[test]
fn test_sweep_parallel_alu() {
    // Test parallel ALU operations, skipping undefined entries
    for alu_byte in 0..=0xFFu32 {
        let alu = dsp56300_core::decode_parallel_alu(alu_byte as u8);
        if matches!(alu, dsp56300_core::ParallelAlu::Undefined) {
            continue;
        }
        let opcode = 0x200000 | alu_byte;
        roundtrip_opcode(0, opcode, 0);
    }
}

#[test]
fn test_sweep_parallel_xy_mem() {
    // clr a x:(r0)+,x0
    let w0 = (4 << 16) | (1 << 15) | (1 << 14) | (0b11_000 << 8) | 0x13u32;
    roundtrip_opcode(0, w0, 0);
}

#[test]
fn test_sweep_tcc() {
    // tcc with various condition codes
    for cc in [0u32, 1, 8, 15] {
        // b,a (tcc_idx = 0 typically, src=B dst=A)
        roundtrip_opcode(0, 0x020000 | (cc << 12), 0);
    }
}

#[test]
fn test_parallel_pm1_x() {
    // Pm1 Class I read: x:(r0)+,x0 a,y0
    let w0 = (1 << 20) | (0 << 18) | (0 << 17) | (0 << 16) | (1 << 15) | (0b011_000 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);

    // Pm1 Class I write: x0,x:(r0)+ a,y0
    let w0 = (1 << 20) | (0 << 18) | (0 << 17) | (0 << 16) | (0 << 15) | (0b011_000 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);

    // Pm1 Class I: x:(r0)+,a b,y1
    let w0 = (1 << 20) | (2 << 18) | (1 << 17) | (1 << 16) | (1 << 15) | (0b011_000 << 8) | 0x00;
    roundtrip_opcode(0, w0, 0);
}

#[test]
fn test_parallel_pm1_x_imm() {
    // Pm1 Class I immediate: #$12,x0 a,y0
    let w0 = (1 << 20) | (0 << 18) | (0 << 17) | (0 << 16) | (1 << 15) | (0x12 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);
}

#[test]
fn test_parallel_pm1_y() {
    // Pm1 Class II read: a,x0 y:(r0)+,y0
    let w0 = (1 << 20)
        | (0 << 19)
        | (0 << 18)
        | (0 << 16)
        | (1 << 15)
        | (1 << 14)
        | (0b011_000 << 8)
        | 0x13;
    roundtrip_opcode(0, w0, 0);

    // Pm1 Class II write: a,x0 y0,y:(r0)+
    let w0 = (1 << 20)
        | (0 << 19)
        | (0 << 18)
        | (0 << 16)
        | (0 << 15)
        | (1 << 14)
        | (0b011_000 << 8)
        | 0x13;
    roundtrip_opcode(0, w0, 0);
}

#[test]
fn test_parallel_pm1_y_imm() {
    // Pm1 Class II imm: a,x0 #$12,y0
    let w0 =
        (1 << 20) | (0 << 19) | (0 << 18) | (0 << 16) | (1 << 15) | (1 << 14) | (0x12 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);
}

#[test]
fn test_parallel_pm0() {
    // Pm0 (X:R Class II): a,x:(r0)+ x0,a
    let w0 = 0x080000 | (0 << 16) | (0 << 15) | (0b011_000 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);

    // Pm0 (R:Y Class II): b,y:(r4)+ y0,b
    let w0 = 0x080000 | (1 << 16) | (1 << 15) | (0b011_100 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);
}

#[test]
fn test_parallel_l_abs() {
    // Pm4 L absolute: l:$0010,a
    let w0 = (1 << 19) | (0 << 16) | (1 << 15) | (0x10 << 8) | 0x13;
    roundtrip_opcode(0, w0, 0);
}

// --- Branch register variants ---

#[test]
fn test_bra_rn() {
    // BraRn: 0000110100011RRR11000000, RRR=2
    roundtrip_opcode(0, 0x0D1AC0, 0);
}

#[test]
fn test_bsr_rn() {
    // BsrRn: 0000110100011RRR10000000, RRR=3
    roundtrip_opcode(0, 0x0D1B80, 0);
}

#[test]
fn test_bcc_rn() {
    // BccRn: 0000110100011RRR0100CCCC, RRR=1, CC=8(CS)
    roundtrip_opcode(0, 0x0D1948, 0);
}

#[test]
fn test_bscc_rn() {
    // BsccRn: 0000110100011RRR0000CCCC, RRR=0, CC=1(GE)
    roundtrip_opcode(0, 0x0D1801, 0);
}

// --- Bit-test branch set variants ---

#[test]
fn test_brset_pp() {
    // BrsetPp: 0000110010pppppp1S1bbbbb, pp=5, S=0, bit=3
    roundtrip_opcode(0, 0x0C8523, 0x0010);
}

#[test]
fn test_brset_aa() {
    // BrsetAa: 0000110010aaaaaa1S1bbbbb, aa=0x10, S=0, bit=3
    // (absolute 6-bit addr form)
    roundtrip_opcode(0, 0x0C9023, 0x0010);
}

#[test]
fn test_bsset_pp() {
    // BssetPp: 0000110110pppppp1S1bbbbb, pp=5, S=0, bit=3
    roundtrip_opcode(0, 0x0D8523, 0x0010);
}

#[test]
fn test_bsset_reg() {
    // BssetReg: 0000110110DDDDDD001bbbbb, D=0x39(SR), bit=0
    roundtrip_opcode(0, 0x0DB920, 0x0010);
}

// --- Jset variants ---

#[test]
fn test_jset_reg() {
    // JsetReg: 0000101010DDDDDD001bbbbb, D=0x39(SR), bit=0
    roundtrip_opcode(0, 0x0AB920, 0x0100);
}

#[test]
fn test_jset_ea() {
    // JsetEa: 0000101010MMMRRR1S1bbbbb, MMM=100(Rn), RRR=0, S=0, bit=3
    roundtrip_opcode(0, 0x0AA023, 0x0100);
}

#[test]
fn test_jset_qq() {
    // JsetQq: 0000000110qqqqqq1S1bbbbb, qq=5, S=0, bit=3
    roundtrip_opcode(0, 0x018523, 0x0100);
}

// --- Loop constructs ---

#[test]
fn test_do_forever() {
    // DoForever: 000000000000011000000010 = 0x000602
    roundtrip_opcode(0, 0x000602, 0x0050);
}

#[test]
fn test_dor_forever() {
    // DorForever: 000000000000001000000010 = 0x000202
    roundtrip_opcode(0, 0x000202, 0x0010);
}

// --- Brkcc ---

#[test]
fn test_brkcc() {
    // Brkcc: 00000000000000100001CCCC, CC=8(CS)
    roundtrip_opcode(0, 0x000218, 0);
}

// --- LRA variants ---

#[test]
fn test_lra_rn() {
    // LraRn: 0000010011000RRR000ddddd, RRR=2, D=r0(0x10)
    roundtrip_opcode(0, 0x04C410, 0);
}

#[test]
fn test_lra_disp() {
    // LraDisp: 0000010001000000010ddddd, D=r0(0x10)
    roundtrip_opcode(0, 0x044050, 0x0100);
}

// --- ALU/bit manipulation ---

#[test]
fn test_clb() {
    // Clb: 0000110000011110000000SD, S=0(A), D=1(B)
    roundtrip_opcode(0, 0x0C1E01, 0);
}

#[test]
fn test_normf() {
    // Normf: 00001100000111100010sssD, sss=2(x0), D=0(A)
    roundtrip_opcode(0, 0x0C1E24, 0);
}

#[test]
fn test_merge() {
    // Merge: 00001100000110111000sssD, sss=2(x0), D=0(A)
    roundtrip_opcode(0, 0x0C1B84, 0);
}

#[test]
fn test_extract_reg() {
    // ExtractReg: 0000110000011010000sSSSD, s=0(A), SSS=2(x0), D=1(B)
    roundtrip_opcode(0, 0x0C1A05, 0);
}

#[test]
fn test_extract_imm() {
    // ExtractImm: 0000110000011000000s000D, s=0(A), D=1(B)
    roundtrip_opcode(0, 0x0C1801, 0x180001);
}

#[test]
fn test_extractu_reg() {
    // ExtractuReg: 0000110000011010100sSSSD, s=0(A), SSS=2(x0), D=1(B)
    roundtrip_opcode(0, 0x0C1A85, 0);
}

#[test]
fn test_extractu_imm() {
    // ExtractuImm: 0000110000011000100s000D, s=0(A), D=1(B)
    roundtrip_opcode(0, 0x0C1881, 0x180001);
}

#[test]
fn test_insert_reg() {
    // InsertReg: 00001100000110110qqqSSSD, SSS=2(x0), qqq=2(x0), D=1(B)
    roundtrip_opcode(0, 0x0C1B25, 0);
}

#[test]
fn test_insert_imm() {
    // InsertImm: 00001100000110010qqq000D, qqq=2(x0), D=1(B)
    roundtrip_opcode(0, 0x0C1921, 0x180001);
}

// --- Misc ---

#[test]
fn test_vsl() {
    // Vsl: 0000101S11MMMRRR110i0000, S=0, MMM=100(Rn), RRR=0, i=0
    roundtrip_opcode(0, 0x0AE0C0, 0x001234);
}

#[test]
fn test_debug() {
    roundtrip_opcode(0, 0x000200, 0);
}

#[test]
fn test_debugcc() {
    // Debugcc: 00000000000000110000CCCC, CC=8(CS)
    roundtrip_opcode(0, 0x000308, 0);
}

#[test]
fn test_trap() {
    roundtrip_opcode(0, 0x000006, 0);
}

#[test]
fn test_trapcc() {
    // Trapcc: 00000000000000000001CCCC, CC=8(CS)
    roundtrip_opcode(0, 0x000018, 0);
}

#[test]
fn test_pflush() {
    roundtrip_opcode(0, 0x000003, 0);
}

#[test]
fn test_pflushun() {
    roundtrip_opcode(0, 0x000001, 0);
}

#[test]
fn test_pfree() {
    roundtrip_opcode(0, 0x000002, 0);
}

#[test]
fn test_plock_ea() {
    // PlockEa: 0000101111MMMRRR10000001, MMM=100(Rn), RRR=0
    roundtrip_opcode(0, 0x0BE081, 0);
}

#[test]
fn test_plock_ea_abs() {
    // PlockEa: MMM=110(absolute), RRR=000
    roundtrip_opcode(0, 0x0BF081, 0x001234);
}

#[test]
fn test_plockr() {
    // Plockr: 000000000000000000001111
    roundtrip_opcode(0, 0x00000F, 0x0100);
}

#[test]
fn test_punlock_ea() {
    // PunlockEa: 0000101011MMMRRR10000001, MMM=100(Rn), RRR=0
    roundtrip_opcode(0, 0x0AE081, 0);
}

#[test]
fn test_punlockr() {
    // Punlockr: 000000000000000000001110
    roundtrip_opcode(0, 0x00000E, 0x0100);
}

/// Exhaustive template sweep: generate sample opcodes from all 188 decoder
/// templates and verify round-trip (disassemble => assemble => compare).
#[test]
fn test_template_sweep() {
    let templates = dsp56300_core::decode::opcode_templates();
    let mut passed = 0;
    let mut skipped = 0;
    let mut failed_names = Vec::new();

    // Fill patterns to try for variable bits, to avoid register-0 artifacts.
    // We try multiple patterns and use the first that produces non-artifact disassembly.
    let fill_patterns: &[u32] = &[
        0x000000, // all zeros (original behavior)
        0x249249, // every 3rd bit -- gives register index 1 for 3-bit fields
        0x492492, // offset pattern -- gives register index 2 for 3-bit fields
        0x111111, // every 4th bit
        0x555555, // alternating bits
        0x040404, // bit 2 in each byte -- targets registers x0/m4 etc.
        0x080808, // bit 3 in each byte
        0x101010, // bit 4 in each byte
    ];

    for tmpl in &templates {
        if !tmpl.has_decode {
            skipped += 1;
            continue;
        }

        let variable_bits = !tmpl.mask & 0xFFFFFF;

        // Try fill patterns to find a non-artifact disassembly that round-trips.
        let mut found = false;
        for &fill in fill_patterns {
            let w0 = tmpl.match_val | (fill & variable_bits);
            let w1 = 0x000100u32;
            let (disasm, _len) = dsp56300_disasm::disassemble(0x100, w0, w1);

            if disasm.starts_with("???") || disasm.starts_with("dc ") {
                continue;
            }

            // The decoder may normalize don't-care bits (e.g. Lua bit 4 of ddddd),
            // so re-disassemble the assembled result and verify text roundtrip.
            match assemble_line(&disasm, 0x100) {
                Ok(result) => {
                    let (redisasm, _) = dsp56300_disasm::disassemble(
                        0x100,
                        result.word0,
                        result.word1.unwrap_or(0),
                    );
                    if redisasm != disasm {
                        failed_names.push(format!(
                            "{}: roundtrip mismatch: '{}' -> {:06x} -> '{}' (orig {:06x})",
                            tmpl.name, disasm, result.word0, redisasm, w0
                        ));
                    } else {
                        passed += 1;
                    }
                    found = true;
                    break;
                }
                Err(_) => {
                    // Encoding rejected (e.g. #0 shift) -- try next fill pattern
                    continue;
                }
            }
        }

        if !found
            && !failed_names
                .last()
                .is_some_and(|f| f.starts_with(tmpl.name))
        {
            skipped += 1;
        }
    }

    if !failed_names.is_empty() {
        eprintln!(
            "Template sweep: {passed} passed, {} failed, {skipped} skipped:\n{}",
            failed_names.len(),
            failed_names.join("\n")
        );
        panic!(
            "{} templates failed round-trip (see stderr for details)",
            failed_names.len()
        );
    }

    eprintln!(
        "Template sweep: {passed} passed, {skipped} skipped (total {})",
        templates.len()
    );

    // Ensure we tested a meaningful number (104 templates have decoders and are testable)
    assert!(
        passed >= 100,
        "Only {passed} templates passed (expected >= 100)"
    );
}

#[test]
fn test_exhaustive_roundtrip() {
    use dsp56300_core::Instruction;
    use dsp56300_core::decode;
    use rayon::prelude::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    let pc = 0x1000;
    let next_word = 0x001234u32;
    let tested = AtomicU32::new(0);
    let skipped = AtomicU32::new(0);

    let failures: Vec<String> = (0u32..0x1000000)
        .into_par_iter()
        .filter_map(|opcode| {
            let inst = decode::decode(opcode);
            let _len = decode::instruction_length(&inst);

            if matches!(
                inst,
                Instruction::Unknown { .. } | Instruction::Unimplemented { .. }
            ) {
                skipped.fetch_add(1, Ordering::Relaxed);
                return None;
            }

            let (disasm, _) = dsp56300_disasm::disassemble(pc, opcode, next_word);

            if disasm.starts_with("dc ") {
                skipped.fetch_add(1, Ordering::Relaxed);
                return None;
            }

            tested.fetch_add(1, Ordering::Relaxed);

            match assemble_line(&disasm, pc) {
                Ok(result) => {
                    let (redisasm, _) = dsp56300_disasm::disassemble(
                        pc,
                        result.word0,
                        result.word1.unwrap_or(next_word),
                    );
                    if redisasm != disasm {
                        Some(format!(
                            "${:06x}: roundtrip mismatch: '{}' -> {:06x} -> '{}' (orig {:06x})",
                            opcode, disasm, result.word0, redisasm, opcode,
                        ))
                    } else {
                        None
                    }
                }
                Err(e) => Some(format!("${:06x}: asm error: {} ('{}')", opcode, e, disasm)),
            }
        })
        .collect();

    if !failures.is_empty() {
        for f in &failures {
            eprintln!("  {}", f);
        }
    }

    let tested = tested.load(Ordering::Relaxed);
    let skipped = skipped.load(Ordering::Relaxed);
    eprintln!(
        "Exhaustive roundtrip: {} tested, {} skipped, {} failures",
        tested,
        skipped,
        failures.len(),
    );

    assert!(
        failures.is_empty(),
        "{} opcodes failed byte-exact roundtrip",
        failures.len(),
    );
}

// ---- Two-pass assembly tests ----

#[test]
fn test_label_backward_ref() {
    // jmp to a label defined before it (long form for symbol refs)
    let source = "org p:$0000\nstart: nop\njmp p:start";
    let result = assemble(source).unwrap();
    assert_eq!(result.segments.len(), 1);
    let words = &result.segments[0].words;
    assert_eq!(words[0], 0x000000); // nop
    assert_eq!(words[1], 0x0AF080); // jmp (long)
    assert_eq!(words[2], 0x000000); // target addr $0000
}

#[test]
fn test_label_forward_ref() {
    // jmp to a label defined after it (long form for symbol refs)
    let source = "org p:$0000\njmp p:target\nnop\ntarget: nop";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // jmp is 2 words (long), nop at pc=2, target at pc=3
    assert_eq!(words[0], 0x0AF080); // jmp (long)
    assert_eq!(words[1], 0x000003); // target addr
    assert_eq!(words[2], 0x000000); // nop
    assert_eq!(words[3], 0x000000); // nop (target)
}

#[test]
fn test_equ_constant() {
    let source = "count: equ $10\norg p:$0000\nrep #count";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // rep #$10 => 0x061080 | (lo << 8) | hi = 0x061080 | (0x10 << 8) | 0 = 0x062080
    let (disasm, _) = dsp56300_disasm::disassemble(0, words[0], 0);
    assert_eq!(disasm, "rep #$10");
}

#[test]
fn test_do_with_label() {
    let source = "org p:$0000\ndo #$10,p:loop_end\nnop\nloop_end: nop";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // do #$10 -- do is 2 words, nop at pc=2, loop_end at pc=3
    // Extension word stores LA = loop_end - 1 = $0002 (last instruction in loop)
    let (disasm, _) = dsp56300_disasm::disassemble(0, words[0], words[1]);
    assert_eq!(disasm, "do #$0010,$0003");
}

#[test]
fn test_jsr_label() {
    let source = "org p:$0000\njsr p:sub\nnop\nsub: nop\nrts";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // jsr sub -- long form (2 words), nop at pc=2, sub at pc=3
    assert_eq!(words[0], 0x0BF080); // jsr (long)
    assert_eq!(words[1], 0x000003); // target addr
    assert_eq!(words[2], 0x000000); // nop
    assert_eq!(words[3], 0x000000); // nop (sub)
    assert_eq!(words[4], 0x00000C); // rts
}

#[test]
fn test_dc_directive() {
    let source = "org p:$0000\ndc $123456,$789abc";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    assert_eq!(words[0], 0x123456);
    assert_eq!(words[1], 0x789ABC);
}

#[test]
fn test_ds_directive() {
    let source = "org p:$0000\ndc $111111\nds 3\ndc $222222";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    assert_eq!(words[0], 0x111111);
    assert_eq!(words[1], 0); // ds fills with 0
    assert_eq!(words[2], 0);
    assert_eq!(words[3], 0);
    assert_eq!(words[4], 0x222222);
}

#[test]
fn test_org_multiple_segments() {
    let source = "org p:$0000\nnop\norg p:$0100\nnop";
    let result = assemble(source).unwrap();
    assert_eq!(result.segments.len(), 2);
    assert_eq!(result.segments[0].org, 0);
    assert_eq!(result.segments[0].words, vec![0x000000]);
    assert_eq!(result.segments[1].org, 0x100);
    assert_eq!(result.segments[1].words, vec![0x000000]);
}

#[test]
fn test_section_ignored() {
    // section/endsec should be parsed and ignored
    let source = "section foo\norg p:$0000\nnop\nendsec";
    let result = assemble(source).unwrap();
    assert_eq!(result.segments[0].words, vec![0x000000]);
}

#[test]
fn test_xref_xdef_ignored() {
    let source = "xref foo,bar\nxdef baz\norg p:$0000\nnop";
    let result = assemble(source).unwrap();
    assert_eq!(result.segments[0].words, vec![0x000000]);
}

#[test]
fn test_hash_gt_force_long() {
    // #> prefix forces long (2-word) encoding even when value fits in short
    let source = "org p:$0000\nadd #>$10,a";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    assert_eq!(words.len(), 2);
    let (disasm, len) = dsp56300_disasm::disassemble(0, words[0], words[1]);
    assert_eq!(len, 2);
    assert_eq!(disasm, "add #>$0010,a");
}

#[test]
fn test_label_on_same_line() {
    let source = "org p:$0000\nstart: jmp p:start";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // jmp start (long form for symbol refs)
    assert_eq!(words[0], 0x0AF080); // jmp (long)
    assert_eq!(words[1], 0x000000); // target addr $0000
}

#[test]
fn test_equ_used_in_org() {
    let source = "base: equ $100\norg p:base\nnop";
    let result = assemble(source).unwrap();
    assert_eq!(result.segments[0].org, 0x100);
}

#[test]
fn test_end_directive() {
    let source = "org p:$0000\nnop\nend\nnop";
    let result = assemble(source).unwrap();
    // Only the first nop should be assembled
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_jclr_with_label() {
    let source = "org p:$0000\njclr #0,x:$ffffc5,p:target\nnop\ntarget: nop";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // jclr is 2 words: word0 is opcode, word1 is target address
    let (disasm, _) = dsp56300_disasm::disassemble(0, words[0], words[1]);
    assert_eq!(disasm, "jclr #0,x:$ffffc5,$0003");
}

#[test]
fn test_bcc_with_label() {
    // bcc uses relative offset (long form), bcc is 2 words, nop is 1 word, target at pc=3
    let source = "org p:$0000\nbcc p:target\nnop\ntarget: nop";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // Long form bcc: word0 = 0x0D1040 (cc=0), word1 = relative offset 3
    assert_eq!(words[0], 0x0D1040); // bcc long form, cc=cc(0)
    assert_eq!(words[1], 3); // offset = target(3) - pc(0)
}

#[test]
fn test_include_expand() {
    // Test include expansion using temp files
    let dir = std::env::temp_dir().join("dsp56300_asm_test_include");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();

    let inc_path = dir.join("inc.asm");
    std::fs::write(&inc_path, "nop\n").unwrap();

    let main_path = dir.join("main.asm");
    std::fs::write(&main_path, "org p:$0000\ninclude 'inc.asm'\nnop\n").unwrap();

    let result = assemble_file(&main_path).unwrap();
    assert_eq!(result.segments[0].words, vec![0x000000, 0x000000]);

    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn test_parallel_reg_to_reg() {
    // clr a a,b => RegToReg { src: A, dst: B }
    roundtrip("clr a a,b", 0);
}

#[test]
fn test_parallel_ea_update() {
    // move (r0)+,r0
    roundtrip("move (r0)+,r0", 0);
    roundtrip("move (r0)-,r0", 0);
}

#[test]
fn test_parallel_imm_to_reg() {
    // add x0,a #$12,x0 => ImmToReg
    roundtrip("add x0,a #$12,x0", 0);
    roundtrip("add x0,a #$ff,r0", 0);
}

#[test]
fn test_parallel_xy_mem_read() {
    // clr a x:(r0)+,x0 => XYMem read
    roundtrip("clr a x:(r0)+,x0", 0);
    // Y space
    roundtrip("clr a y:(r4)+,y0", 0);
}

#[test]
fn test_parallel_xy_mem_write() {
    // clr a a,x:(r0)+ => XYMem write
    roundtrip("clr a a,x:(r0)+", 0);
    roundtrip("clr a b,y:(r4)+", 0);
}

#[test]
fn test_parallel_xy_abs_read() {
    // clr a x:$0010,x0 => XYAbs read
    roundtrip("clr a x:$0010,x0", 0);
}

#[test]
fn test_parallel_xy_abs_write() {
    // clr a a,x:$0010 => XYAbs write
    roundtrip("clr a a,x:$0010", 0);
}

#[test]
fn test_parallel_xy_double() {
    // clr a x:(r0)+,x0 y:(r4)+,y0 => XYDouble read/read
    roundtrip("clr a x:(r0)+,x0 y:(r4)+,y0", 0);
}

#[test]
fn test_parallel_xy_double_write() {
    roundtrip("clr a x:(r0)+,a y:(r4)+,y0", 0);
}

#[test]
fn test_parallel_l_mem() {
    // clr a l:(r0)+,a => LMem read
    roundtrip("clr a l:(r0)+,a", 0);
    // L mem write
    roundtrip("clr a a,l:(r0)+", 0);
}

#[test]
fn test_parallel_l_abs_encode() {
    // clr a l:$0010,a => LAbs read
    roundtrip("clr a l:$0010,a", 0);
    // L abs write
    roundtrip("clr a a,l:$0010", 0);
}

#[test]
fn test_parallel_l_imm() {
    roundtrip("clr a #$123456,a10", 0);
}

#[test]
fn test_parallel_pm1_x_ea_encode() {
    // x:(r0)+,x0 a,y0 => XReg
    roundtrip("clr a x:(r0)+,x0 a,y0", 0);
    // Write direction: a,x:(r0)+ b,y0
    // This encodes as: x_reg write, s2=b, d2=y0
}

#[test]
fn test_parallel_pm1_x_imm_encode() {
    roundtrip("clr a #>$000012,x0 a,y0", 0);
}

#[test]
fn test_parallel_pm1_y_ea_encode() {
    // a,x0 y:(r0)+,y0 => RegY
    roundtrip("clr a a,x0 y:(r0)+,y0", 0);
    // Write direction: a,x0 y0,y:(r0)+
    roundtrip("clr a a,x0 y0,y:(r0)+", 0);
}

#[test]
fn test_parallel_pm1_y_imm_encode() {
    roundtrip("clr a a,x0 #>$000012,y0", 0);
}

#[test]
fn test_parallel_pm0_encode() {
    roundtrip("clr a a,x:(r0)+ x0,a", 0);
    roundtrip("clr a b,y:(r4)+ y0,b", 0);
}

// ---- EA modes ----

#[test]
fn test_ea_post_inc_n() {
    roundtrip("movec x:(r0)+n0,sr", 0);
}

#[test]
fn test_ea_no_update() {
    roundtrip("lua (r0+0),r1", 0);
}

#[test]
fn test_ea_indexed_n() {
    roundtrip("lua (r0)+n0,r1", 0);
}

#[test]
fn test_ea_pre_dec() {
    // movem -(r0),r1
    // Use a context where pre-dec is valid
    roundtrip("movem p:-(r0),r0", 0);
}

#[test]
fn test_ea_abs_addr() {
    // jcc with small absolute address uses short Jcc encoding
    let result = assemble_line("jcc p:$0100", 0).unwrap();
    assert_eq!(result.word0, 0x0E0100); // Jcc short (CC=0, addr=$100)
    assert_eq!(result.word1, None);

    // jcc with large address still uses long JccEa encoding
    let result = assemble_line("jcc p:$1234", 0).unwrap();
    assert_eq!(result.word0, 0x0AF0A0); // Jcc long
    assert_eq!(result.word1, Some(0x1234));
}

// ---- Movep variants ----

#[test]
fn test_movep23_pp_read() {
    roundtrip("movep x:$ffffc5,x:(r0)+", 0);
}

#[test]
fn test_movep23_pp_write() {
    roundtrip("movep x:(r0)+,x:$ffffc5", 0);
}

#[test]
fn test_movep23_qq_read() {
    roundtrip("movep x:$ffff80,x:(r0)+", 0);
}

#[test]
fn test_movep23_qq_write() {
    roundtrip("movep x:(r0)+,x:$ffff80", 0);
}

#[test]
fn test_movep1_read() {
    roundtrip("movep p:(r0)+,x:$ffffc5", 0);
}

#[test]
fn test_movep1_write() {
    roundtrip("movep x:$ffffc5,p:(r0)+", 0);
}

#[test]
fn test_movep0_read() {
    roundtrip("movep x:$ffffc5,x0", 0);
}

#[test]
fn test_movep0_write() {
    roundtrip("movep x0,x:$ffffc5", 0);
}

// ---- Movec variations ----

#[test]
fn test_movec_reg_w1() {
    // When src is not in 0x20-0x3F range, uses W=1 path
    roundtrip("movec r0,sr", 0);
}

#[test]
fn test_movec_aa() {
    roundtrip("movec x:$0020,sr", 0);
    roundtrip("movec sr,x:$0020", 0);
}

#[test]
fn test_movec_ea() {
    roundtrip("movec x:(r0)+,sr", 0);
    roundtrip("movec sr,x:(r0)+", 0);
}

// ---- Movem ----

#[test]
fn test_movem_ea() {
    roundtrip("movem p:(r0)+,r0", 0);
    roundtrip("movem r0,p:(r0)+", 0);
}

#[test]
fn test_movem_aa() {
    roundtrip("movem p:$0020,r0", 0);
    roundtrip("movem r0,p:$0020", 0);
}

#[test]
fn test_movem_aa_promotes_to_ea() {
    // Address $3F is the max for 6-bit aa mode (1 word)
    let r = assemble_line("movem p:$003f,r0", 0).unwrap();
    assert!(r.word1.is_none(), "addr $3F should use 1-word aa mode");

    // Address $40 exceeds aa range and must promote to 2-word ea mode
    let r = assemble_line("movem p:$0040,r0", 0).unwrap();
    assert_eq!(
        r.word1,
        Some(0x000040),
        "addr $40 should promote to ea mode"
    );
    roundtrip("movem p:$0040,r0", 0);

    // Large address roundtrips correctly
    roundtrip("movem p:$29718,x0", 0);
    roundtrip("movem x0,p:$29718", 0);
}

#[test]
fn test_movec_aa_promotes_to_ea() {
    // Address $3F is the max for 6-bit aa mode (1 word)
    let r = assemble_line("movec x:$003f,sr", 0).unwrap();
    assert!(r.word1.is_none(), "addr $3F should use 1-word aa mode");

    // Address $40 exceeds aa range and must promote to 2-word ea mode
    let r = assemble_line("movec x:$0040,sr", 0).unwrap();
    assert_eq!(
        r.word1,
        Some(0x000040),
        "addr $40 should promote to ea mode"
    );
    roundtrip("movec x:$0040,sr", 0);

    // Large address roundtrips correctly
    roundtrip("movec x:$2fa5d,m0", 0);
    roundtrip("movec m0,x:$2fa5d", 0);
}

// ---- Div/cmpu/mpyi register variants ----

#[test]
fn test_div_y0() {
    roundtrip("div y0,a", 0);
}

#[test]
fn test_div_x1() {
    roundtrip("div x1,a", 0);
}

#[test]
fn test_div_y1() {
    roundtrip("div y1,a", 0);
}

#[test]
fn test_div_b() {
    roundtrip("div x0,b", 0);
}

#[test]
fn test_cmpu_variants() {
    roundtrip("cmpu x0,a", 0);
    roundtrip("cmpu y0,a", 0);
    roundtrip("cmpu x1,a", 0);
    roundtrip("cmpu y1,a", 0);
    roundtrip("cmpu b,a", 0);
    roundtrip("cmpu a,b", 0);
}

#[test]
fn test_mpyi() {
    roundtrip("mpyi +#$000010,x0,a", 0);
    roundtrip("mpyi +#$000010,y0,a", 0);
    roundtrip("mpyi +#$000010,x1,a", 0);
    roundtrip("mpyi +#$000010,y1,a", 0);
    roundtrip("mpyi -#$000010,x0,a", 0);
}

// ---- Bit branch ----

#[test]
fn test_jset_pp() {
    roundtrip("jset #0,x:$ffffc5,$1234", 0);
}

#[test]
fn test_jsclr_pp() {
    roundtrip("jsclr #0,x:$ffffc5,$1234", 0);
}

#[test]
fn test_jsset_pp() {
    roundtrip("jsset #0,x:$ffffc5,$1234", 0);
}

#[test]
fn test_jclr_aa() {
    roundtrip("jclr #0,x:$0020,$1234", 0);
}

#[test]
fn test_jset_aa() {
    roundtrip("jset #0,x:$0020,$1234", 0);
}

#[test]
fn test_jclr_aa_promotes_to_ea() {
    // Address $3F is max for 6-bit aa mode
    let r = assemble_line("jclr #0,x:$003f,p:$003f", 0).unwrap();
    assert!(r.word1.is_some()); // always 2-word
    // Re-disassemble to verify aa form
    let (d, _) = dsp56300_disasm::disassemble(0, r.word0, r.word1.unwrap());
    assert_eq!(d, "jclr #0,x:$003f,$003f");

    // Address $40 exceeds aa range; promotes to EA when addr == target
    roundtrip("jclr #0,x:$0040,$0040", 0);
    roundtrip("jsclr #14,y:$0053,$0053", 0);
    roundtrip("jset #5,x:$0080,$0080", 0);
    roundtrip("jsset #7,y:$006e,$006e", 0);
}

#[test]
fn test_jclr_ea() {
    roundtrip("jclr #0,x:(r0)+,$1234", 0);
}

#[test]
fn test_jclr_reg() {
    roundtrip("jclr #0,sr,$1234", 0);
}

#[test]
fn test_brclr_pp() {
    roundtrip("brclr #0,x:$ffffc5,$000010", 0);
}

#[test]
fn test_brset_reg() {
    roundtrip("brset #3,sr,$0010", 0);
}

// ---- Bit ops: ea form ----

#[test]
fn test_bclr_ea() {
    roundtrip("bclr #3,x:(r0)+", 0);
}

#[test]
fn test_bset_ea() {
    roundtrip("bset #5,y:(r4)-", 0);
}

#[test]
fn test_bchg_ea() {
    roundtrip("bchg #7,x:(r0)+", 0);
}

#[test]
fn test_btst_ea() {
    roundtrip("btst #3,x:(r0)+", 0);
}

// ---- DO/DOR/REP variants ----

#[test]
fn test_do_aa() {
    roundtrip("do x:$0020,$0051", 0);
}

#[test]
fn test_do_ea() {
    roundtrip("do x:(r0)+,$0051", 0);
}

#[test]
fn test_dor_imm() {
    roundtrip("dor #$0010,$0051", 0);
}

#[test]
fn test_dor_reg() {
    roundtrip("dor lc,$0051", 0);
}

#[test]
fn test_rep_aa() {
    roundtrip("rep x:$0020", 0);
}

#[test]
fn test_rep_ea() {
    roundtrip("rep x:(r0)+", 0);
}

#[test]
fn test_do_aa_rejects_large_addr() {
    // $3F is max for 6-bit aa
    assemble_line("do x:$003f,p:$0050", 0).unwrap();
    // $40 exceeds aa range
    assert!(assemble_line("do x:$0040,p:$0050", 0).is_err());
}

#[test]
fn test_dor_aa_rejects_large_addr() {
    assemble_line("dor x:$003f,p:$0050", 0).unwrap();
    assert!(assemble_line("dor x:$0040,p:$0050", 0).is_err());
}

#[test]
fn test_rep_aa_promotes_to_ea() {
    // $003f fits in aa range (1-word)
    let r = assemble_line("rep x:$003f", 0).unwrap();
    assert!(r.word1.is_none());
    // $0040 exceeds aa range, promotes to ea absolute (2-word)
    let r = assemble_line("rep x:$0040", 0).unwrap();
    assert_eq!(r.word1, Some(0x000040));
}

#[test]
fn test_bclr_aa_promotes_to_ea() {
    // $003f fits in aa range (1-word)
    let r = assemble_line("bclr #0,x:$003f", 0).unwrap();
    assert!(r.word1.is_none());
    // $0040 exceeds aa range, promotes to ea absolute (2-word)
    let r = assemble_line("bclr #0,x:$0040", 0).unwrap();
    assert_eq!(r.word1, Some(0x000040));
}

// ---- Long-form ALU ----

#[test]
fn test_add_long() {
    roundtrip("add #$123456,a", 0);
}

#[test]
fn test_sub_long() {
    roundtrip("sub #$123456,b", 0);
}

#[test]
fn test_cmp_long() {
    roundtrip("cmp #$123456,a", 0);
}

#[test]
fn test_and_long() {
    roundtrip("and #$123456,a", 0);
}

#[test]
fn test_or_long() {
    roundtrip("or #$123456,a", 0);
}

// ---- Lua ----

#[test]
fn test_lua_ea() {
    roundtrip("lua (r0)+,r1", 0);
}

#[test]
fn test_lua_rel() {
    roundtrip("lua (r0+16),r1", 0);
    roundtrip("lua (r0+16),n1", 0);
}

// ---- Lsl ----

#[test]
fn test_lsl_imm() {
    roundtrip("lsl #$01,a", 0);
}

// ---- Jmp/Jsr EA forms ----

#[test]
fn test_jmp_ea() {
    roundtrip("jmp (r0)+", 0);
}

#[test]
fn test_jsr_ea() {
    roundtrip("jsr (r0)+", 0);
}

#[test]
fn test_jcc_ea() {
    roundtrip("jcc (r0)+", 0);
}

#[test]
fn test_jscc_ea() {
    roundtrip("jscc (r0)+", 0);
}

// ---- MoveShortDisp ----

#[test]
fn test_move_xy_imm() {
    roundtrip("move x:(r0+16),x0", 0);
}

// ---- Bra/Bsr ----

#[test]
fn test_bra_long() {
    let result = assemble_line("bra p:$000100", 0).unwrap();
    assert_eq!(result.word0, 0x0D10C0);
    assert_eq!(result.word1.unwrap(), 0x100);
}

#[test]
fn test_bsr_long() {
    let result = assemble_line("bsr p:$000100", 0).unwrap();
    assert_eq!(result.word0, 0x0D1080);
    assert_eq!(result.word1.unwrap(), 0x100);
}

#[test]
fn test_current_pc_in_bcc() {
    // "bcc p:*" would use CurrentPc -- but the disassembler outputs the absolute address.
    // Use a two-pass program instead:
    let source = "org p:$0010\nbcc p:$000010";
    let result = assemble(source).unwrap();
    let words = &result.segments[0].words;
    // bcc short form to self: displacement = 0
    // Template: 00000101CCCC01aaaa0aaaaa (CC=0, disp=0)
    assert_eq!(words.len(), 1);
    assert_eq!(words[0], 0x050400);
}

// ---- Error paths ----

#[test]
fn test_undefined_symbol_error() {
    let source = "org p:$0000\njmp p:undefined_label";
    assert!(assemble(source).is_err());
}

#[test]
fn test_empty_program() {
    let result = assemble("").unwrap();
    assert!(result.segments.is_empty());
}

#[test]
fn test_comments_only() {
    let result = assemble("; this is a comment\n; another comment").unwrap();
    assert!(result.segments.is_empty());
}

// --- L-move composite registers are not valid in movec ---

#[test]
fn test_reg_b10() {
    assert!(assemble_line("movec b10,r0", 0).is_err());
}

#[test]
fn test_reg_x_y_aliases() {
    assert!(assemble_line("movec x,r0", 0).is_err());
    assert!(assemble_line("movec y,r0", 0).is_err());
}

#[test]
fn test_reg_ab_ba() {
    assert!(assemble_line("movec ab,r0", 0).is_err());
    assert!(assemble_line("movec ba,r0", 0).is_err());
}

#[test]
fn test_movec_large_imm() {
    // Can't roundtrip(): disassembler omits destination register for immediate EA movec
    let result = assemble_line("movec #$001234,r0", 0).unwrap();
    assert_eq!(result.word0, 0x05F430, "word0: movec imm->r0 encoding");
    assert_eq!(result.word1, Some(0x001234), "word1: immediate value");
}

#[test]
fn test_movep_imm() {
    roundtrip("movep #$000012,x:$ffffc0", 0);
}

#[test]
fn test_move_x_long_read() {
    let result = assemble_line("move x:(r0+$100),a", 0).unwrap();
    assert_eq!(
        result.word0, 0x0A70CE,
        "word0: MoveLongDisp read x:(r0+xxxx),a"
    );
    assert_eq!(result.word1, Some(0x000100), "word1: displacement");
}

#[test]
fn test_move_x_long_write() {
    let result = assemble_line("move a,x:(r0+$100)", 0).unwrap();
    assert_eq!(
        result.word0, 0x0A708E,
        "word0: MoveLongDisp write a,x:(r0+xxxx)"
    );
    assert_eq!(result.word1, Some(0x000100), "word1: displacement");
}

#[test]
fn test_move_x_long_neg_offset() {
    let result = assemble_line("move x:(r0-$100),a", 0).unwrap();
    assert_eq!(
        result.word0, 0x0A70CE,
        "word0: MoveLongDisp read x:(r0+xxxx),a"
    );
    assert_eq!(
        result.word1,
        Some(0xFFFF00),
        "word1: negative displacement (24-bit)"
    );
}

#[test]
fn test_mpyi_no_sign() {
    roundtrip("mpyi +#$000020,x0,a", 0);
}

#[test]
fn test_lua_indexed_n() {
    roundtrip("lua (r0)+n0,r1", 0);
}

#[test]
fn test_lua_rel_neg() {
    roundtrip("lua (r0-16),r1", 0);
}

#[test]
fn test_lua_post_inc() {
    roundtrip("lua (r0)+,r1", 0);
}

#[test]
fn test_lua_post_dec() {
    roundtrip("lua (r0)-,r1", 0);
}

#[test]
fn test_lua_no_update() {
    roundtrip("lua (r0+0),r1", 0);
}

#[test]
fn test_jmp_bare_ea() {
    // jmp (r0) should produce the same encoding as jmp p:(r0)
    let bare = assemble_line("jmp (r0)", 0).unwrap();
    let explicit = assemble_line("jmp p:(r0)", 0).unwrap();
    assert_eq!(bare.word0, explicit.word0);
}

#[test]
fn test_jsr_bare_ea() {
    let bare = assemble_line("jsr (r0)", 0).unwrap();
    let explicit = assemble_line("jsr p:(r0)", 0).unwrap();
    assert_eq!(bare.word0, explicit.word0);
}

#[test]
fn test_label_only_line() {
    let result = assemble("org p:$0\nlabel:\nnop\n").unwrap();
    assert_eq!(result.segments.len(), 1);
    // label at address 0, nop at address 0
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_label_on_instruction() {
    let result = assemble("org p:$0\nstart: nop\n").unwrap();
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_parse_error_display() {
    let err = parser::ParseError {
        line: 42,
        msg: "test error".to_string(),
    };
    assert_eq!(format!("{}", err), "line 42: test error");
}

#[test]
fn test_parallel_xy_double_read_read() {
    roundtrip("move x:(r2),x0 y:(r4),y0", 0);
}

#[test]
fn test_parallel_xy_double_write_read() {
    roundtrip("move x:(r2),a y:(r4),y1", 0);
}

#[test]
fn test_parallel_reg_y_write() {
    roundtrip("move x:(r2)-n2,b a,y1", 0);
}

#[test]
fn test_parallel_reg_y_read() {
    roundtrip("move x:(r2)-n2,a a,y1", 0);
}

#[test]
fn test_parse_error_invalid_instruction() {
    // A bare unknown ident is now valid as a label-without-colon.
    // Use a truly invalid token sequence to test error reporting.
    let result = assemble("org p:$0\n,,\n");
    assert!(result.is_err());
    let err_msg = format!("{}", result.unwrap_err());
    assert!(
        err_msg.contains("line"),
        "Error should include line: {}",
        err_msg
    );
}

#[test]
fn test_dor_aa() {
    roundtrip("dor x:$0020,$0101", 0);
}

#[test]
fn test_dor_ea() {
    roundtrip("dor x:(r0)+,$0101", 0);
}

#[test]
fn test_do_with_forward_label() {
    let src = "org p:$0\ndo #10,end_loop\nnop\nend_loop:\n";
    let result = assemble(src).unwrap();
    assert!(!result.segments.is_empty());
}

#[test]
fn test_section_with_label() {
    let src = "section code\norg p:$0\nnop\nendsec\n";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_xref_multiple() {
    let src = "xref foo,bar,baz\norg p:$0\nnop\n";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_xdef_multiple() {
    let src = "xdef foo,bar\norg p:$0\nnop\n";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_global_directive() {
    let src = "global myvar\norg p:$0\nnop\n";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words.len(), 1);
}

#[test]
fn test_bcc_conditional_path() {
    // bge (b-prefix with "ge" cc) -- tests parse_conditional line 546
    roundtrip("bge $000100", 0);
}

#[test]
fn test_jscc_addr() {
    // jsge p:$100 -- tests jscc with address (line 559)
    roundtrip("jsge $0100", 0);
}

#[test]
fn test_jcc_addr_not_ea() {
    // jge p:$100 -- tests jcc with address (line 569)
    roundtrip("jge $0100", 0);
}

#[test]
fn test_lua_rel_n_dest() {
    roundtrip("lua (r0+16),n1", 0);
}

#[test]
fn test_token_display() {
    use crate::token::Token;
    assert_eq!(format!("{}", Token::XMem), "x:");
    assert_eq!(format!("{}", Token::YMem), "y:");
    assert_eq!(format!("{}", Token::PMem), "p:");
    assert_eq!(format!("{}", Token::LMem), "l:");
    assert_eq!(format!("{}", Token::Hex(0xff)), "$ff");
    assert_eq!(format!("{}", Token::Dec(42)), "42");
    assert_eq!(format!("{}", Token::Hash), "#");
    assert_eq!(format!("{}", Token::Comma), ",");
    assert_eq!(format!("{}", Token::LParen), "(");
    assert_eq!(format!("{}", Token::RParen), ")");
    assert_eq!(format!("{}", Token::Plus), "+");
    assert_eq!(format!("{}", Token::Minus), "-");
    assert_eq!(format!("{}", Token::Colon), ":");
    assert_eq!(format!("{}", Token::Gt), ">");
    assert_eq!(
        format!("{}", Token::StringLit("foo.asm".to_string())),
        "'foo.asm'"
    );
    assert_eq!(format!("{}", Token::Newline), "\\n");
    assert_eq!(format!("{}", Token::NewlineCrLf), "\\n");
    assert_eq!(format!("{}", Token::Comment), ";...");
    assert_eq!(format!("{}", Token::Ident("nop".to_string())), "nop");
}

#[test]
fn test_token_stream() {
    use crate::token::TokenStream;
    let mut ts = TokenStream::new("nop ; comment\n  rts");
    assert_eq!(ts.line(), 1);
    assert!(!ts.at_eol());
    assert!(ts.peek().is_some());
    let tok = ts.next();
    assert!(tok.is_some());
    assert!(ts.at_eol());
    assert!(ts.skip_newlines());
    assert_eq!(ts.line(), 2);
    // eat_ident
    let ident = ts.eat_ident();
    assert_eq!(ident, Some("rts".to_string()));
    assert!(ts.at_eol());
    assert!(ts.peek().is_none());
}

#[test]
fn test_token_stream_eat() {
    use crate::token::{Token, TokenStream};
    let mut ts = TokenStream::new("#$ff");
    assert!(ts.eat(&Token::Hash));
    assert!(!ts.eat(&Token::Hash)); // already consumed
}

#[test]
fn test_token_stream_save_restore() {
    use crate::token::TokenStream;
    let mut ts = TokenStream::new("nop rts");
    let pos = ts.pos();
    ts.next(); // consume nop
    ts.set_pos(pos); // restore
    let ident = ts.eat_ident();
    assert_eq!(ident, Some("nop".to_string()));
}

#[test]
fn test_token_stream_source() {
    use crate::token::TokenStream;
    let src = "nop";
    let ts = TokenStream::new(src);
    assert_eq!(ts.source(), src);
}

#[test]
fn test_expr_debug() {
    // Exercise Debug formatting for Expr variants
    assert!(format!("{:?}", Expr::Literal(42)).contains("Literal"));
    assert!(format!("{:?}", Expr::Literal(-1)).contains("-1"));
    assert!(format!("{:?}", Expr::Symbol("foo".to_string())).contains("foo"));
    assert!(format!("{:?}", Expr::CurrentPc).contains("CurrentPc"));
}

// --- Assembler error formatting ---

#[test]
fn test_parallel_ifcc() {
    // IFcc: add a,b ifcc => 0x202000 | (cc=0 << 8) | alu_byte
    roundtrip("add a,b ifcc", 0);
    roundtrip("clr a ifge", 0);
    roundtrip("asl a ifne", 0);
    roundtrip("add x0,a ifgt", 0);
}

#[test]
fn test_parallel_ifcc_u() {
    // IFcc.U: add a,b ifcc.u => 0x203000 | (cc << 8) | alu_byte
    roundtrip("add a,b ifcc.u", 0);
    roundtrip("clr a ifge.u", 0);
    roundtrip("asl a ifne.u", 0);
    roundtrip("add x0,a ifgt.u", 0);
}

#[test]
fn test_assemble_error_display() {
    let err = AssembleError::Encode {
        line: 5,
        err: encode::EncodeError {
            msg: "test".to_string(),
        },
    };
    assert_eq!(format!("{}", err), "line 5: encode error: test");

    let err2 = AssembleError::Parse(parser::ParseError {
        line: 3,
        msg: "bad".to_string(),
    });
    assert_eq!(format!("{}", err2), "line 3: bad");
}

#[test]
fn test_expr_add() {
    let src = "base: equ $10\norg p:$0\nrep #base+$05";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$15");
}

#[test]
fn test_expr_sub() {
    let src = "base: equ $20\norg p:$0\nrep #base-$05";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$1b");
}

#[test]
fn test_expr_mul() {
    let src = "base: equ $04\norg p:$0\nrep #base*$03";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$0c");
}

#[test]
fn test_expr_div() {
    let src = "base: equ $10\norg p:$0\nrep #base/$04";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$04");
}

#[test]
fn test_expr_shl() {
    let src = "base: equ $01\norg p:$0\nrep #base<<4";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$10");
}

#[test]
fn test_expr_shr() {
    let src = "base: equ $80\norg p:$0\nrep #base>>4";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$08");
}

#[test]
fn test_expr_bitand() {
    let src = "base: equ $FF\norg p:$0\nrep #base&$0F";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$0f");
}

#[test]
fn test_expr_bitor() {
    let src = "base: equ $F0\norg p:$0\nrep #base|$0F";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$ff");
}

#[test]
fn test_expr_neg() {
    // Unary negation: -$10 in a 24-bit context = $FFFFF0
    let src = "org p:$0\ndc -$10";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words[0], 0xFFFFF0);
}

#[test]
fn test_expr_bitnot() {
    // Bitwise NOT: ~$FF = 0xFFFF00 in 24-bit context
    let src = "org p:$0\ndc ~$FF";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words[0], 0xFFFF00);
}

#[test]
fn test_expr_current_pc() {
    // * refers to current PC
    let src = "org p:$100\ndc *";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words[0], 0x000100);
}

#[test]
fn test_expr_parens() {
    // Parenthesized sub-expression
    let src = "base: equ $02\norg p:$0\nrep #(base+$01)*$04";
    let result = assemble(src).unwrap();
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.segments[0].words[0], 0);
    assert_eq!(disasm, "rep #$0c");
}

#[test]
fn test_expr_frac() {
    // Fractional literal 0.5 = 0x400000 in Q23
    let src = "org p:$0\ndc 0.5";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words[0], 0x400000);
}

#[test]
fn test_expr_neg_frac() {
    // -0.5 in Q23 = 0xC00000 (24-bit)
    let src = "org p:$0\ndc -0.5";
    let result = assemble(src).unwrap();
    assert_eq!(result.segments[0].words[0], 0xC00000);
}

#[test]
fn test_expr_div_by_zero() {
    let src = "org p:$0\ndc $10/0";
    assert!(assemble(src).is_err());
}

#[test]
fn test_move_y_long_read() {
    let result = assemble_line("move y:(r0+$100),a", 0).unwrap();
    assert!(result.word1.is_some());
    assert_eq!(result.word1.unwrap(), 0x000100);
}

#[test]
fn test_move_y_long_write() {
    let result = assemble_line("move a,y:(r0+$100)", 0).unwrap();
    assert!(result.word1.is_some());
    assert_eq!(result.word1.unwrap(), 0x000100);
}

#[test]
fn test_parallel_xy_double_x_write_y_read() {
    // x0,x:(r0)+ y:(r4)+,y0
    let result = assemble_line("clr a x0,x:(r0)+ y:(r4)+,y0", 0).unwrap();
    assert_ne!(result.word0, 0);
}

#[test]
fn test_parallel_xy_double_x_write_y_write() {
    // x0,x:(r0)+ y0,y:(r4)+
    let result = assemble_line("clr a x0,x:(r0)+ y0,y:(r4)+", 0).unwrap();
    assert_ne!(result.word0, 0);
}

#[test]
fn test_parallel_xy_double_x_read_y_write() {
    // x:(r0)+,x0 y0,y:(r4)+
    let result = assemble_line("clr a x:(r0)+,x0 y0,y:(r4)+", 0).unwrap();
    assert_ne!(result.word0, 0);
}

#[test]
fn test_movec_x1() {
    roundtrip("movec x1,sr", 0);
}

#[test]
fn test_movec_y1() {
    roundtrip("movec y1,sr", 0);
}

#[test]
fn test_movec_a0() {
    roundtrip("movec a0,sr", 0);
}

#[test]
fn test_movec_a1() {
    roundtrip("movec a1,sr", 0);
}

#[test]
fn test_movec_a2() {
    roundtrip("movec a2,sr", 0);
}

#[test]
fn test_movec_b1() {
    roundtrip("movec b1,sr", 0);
}

#[test]
fn test_movec_b2() {
    roundtrip("movec b2,sr", 0);
}

#[test]
fn test_movec_n_reg() {
    roundtrip("movec n0,sr", 0);
    roundtrip("movec n4,sr", 0);
}

#[test]
fn test_movec_mr_ccr() {
    // MR and CCR map to SR sub-fields
    roundtrip("andi #$fe,mr", 0);
    roundtrip("ori #$03,ccr", 0);
}

#[test]
fn test_movep_qq_imm() {
    roundtrip("movep #$000012,x:$ffff80", 0);
}

#[test]
fn test_movep_yqq_imm() {
    roundtrip("movep #$000012,y:$ffff80", 0);
}

#[test]
fn test_movep23_yqq_read() {
    roundtrip("movep y:$ffff80,x:(r0)+", 0);
}

#[test]
fn test_movep23_yqq_write() {
    roundtrip("movep x:(r0)+,y:$ffff80", 0);
}

#[test]
fn test_ea_indexed_n_in_move() {
    // Test IndexedN EA mode through a movec instruction
    roundtrip("movec x:(r0)+n0,sr", 0);
}

#[test]
fn test_try_eval_const_binop() {
    use crate::ast::{BinOp, Expr};
    let e = Expr::BinOp {
        op: BinOp::Add,
        lhs: Box::new(Expr::Literal(10)),
        rhs: Box::new(Expr::Literal(20)),
    };
    assert_eq!(e.try_eval_const(), Some(30));

    let e = Expr::BinOp {
        op: BinOp::Sub,
        lhs: Box::new(Expr::Literal(20)),
        rhs: Box::new(Expr::Literal(5)),
    };
    assert_eq!(e.try_eval_const(), Some(15));

    let e = Expr::BinOp {
        op: BinOp::Mul,
        lhs: Box::new(Expr::Literal(3)),
        rhs: Box::new(Expr::Literal(7)),
    };
    assert_eq!(e.try_eval_const(), Some(21));

    let e = Expr::BinOp {
        op: BinOp::Div,
        lhs: Box::new(Expr::Literal(20)),
        rhs: Box::new(Expr::Literal(4)),
    };
    assert_eq!(e.try_eval_const(), Some(5));

    // Div by zero returns None
    let e = Expr::BinOp {
        op: BinOp::Div,
        lhs: Box::new(Expr::Literal(20)),
        rhs: Box::new(Expr::Literal(0)),
    };
    assert_eq!(e.try_eval_const(), None);

    let e = Expr::BinOp {
        op: BinOp::Shl,
        lhs: Box::new(Expr::Literal(1)),
        rhs: Box::new(Expr::Literal(4)),
    };
    assert_eq!(e.try_eval_const(), Some(16));

    let e = Expr::BinOp {
        op: BinOp::Shr,
        lhs: Box::new(Expr::Literal(128)),
        rhs: Box::new(Expr::Literal(4)),
    };
    assert_eq!(e.try_eval_const(), Some(8));

    let e = Expr::BinOp {
        op: BinOp::BitAnd,
        lhs: Box::new(Expr::Literal(0xFF)),
        rhs: Box::new(Expr::Literal(0x0F)),
    };
    assert_eq!(e.try_eval_const(), Some(0x0F));

    let e = Expr::BinOp {
        op: BinOp::BitOr,
        lhs: Box::new(Expr::Literal(0xF0)),
        rhs: Box::new(Expr::Literal(0x0F)),
    };
    assert_eq!(e.try_eval_const(), Some(0xFF));
}

#[test]
fn test_try_eval_const_unaryop() {
    use crate::ast::{Expr, UnaryOp};
    let e = Expr::UnaryOp {
        op: UnaryOp::Neg,
        operand: Box::new(Expr::Literal(10)),
    };
    assert_eq!(e.try_eval_const(), Some(-10));

    let e = Expr::UnaryOp {
        op: UnaryOp::BitNot,
        operand: Box::new(Expr::Literal(0xFF)),
    };
    assert_eq!(e.try_eval_const(), Some(!0xFF_i64));
}

#[test]
fn test_is_data_alu() {
    use crate::ast::Register;
    assert!(Register::X0.is_data_alu());
    assert!(Register::X1.is_data_alu());
    assert!(Register::Y0.is_data_alu());
    assert!(Register::Y1.is_data_alu());
    assert!(Register::A.is_data_alu());
    assert!(Register::B.is_data_alu());
    assert!(!Register::R(0).is_data_alu());
    assert!(!Register::Sr.is_data_alu());
}

#[test]
fn test_parallel_xy_imm_write() {
    // XYMem write with Immediate EA
    roundtrip_opcode(0, 0x40B413, 0x000012);
}

#[test]
fn test_jcc_short_encoding() {
    // JccEa with addr < 0x1000 -> short form (12-bit)
    roundtrip("jcc $0800", 0);
}

#[test]
fn test_jscc_short_encoding() {
    // JsccEa with addr < 0x1000 -> short form
    roundtrip("jscc $0400", 0);
}

#[test]
fn test_jcc_long_encoding() {
    // JccEa with addr >= 0x1000 -> long form (2 words, EA mode 6)
    roundtrip("jcc $1234", 0);
}

#[test]
fn test_jscc_long_encoding() {
    // JsccEa with addr >= 0x1000 -> long form
    roundtrip("jscc $1234", 0);
}

#[test]
fn test_current_pc_expr() {
    // jmp p:* uses Expr::CurrentPc, eval returns pc
    let result = assemble_line("jmp p:*", 0).unwrap();
    // CurrentPc is not a literal, so Jmp uses long form
    assert_eq!(result.word0, 0x0AF080);
    assert_eq!(result.word1, Some(0));
}

#[test]
fn test_current_pc_nonzero() {
    let result = assemble_line("jmp p:*", 0x100).unwrap();
    assert_eq!(result.word0, 0x0AF080);
    assert_eq!(result.word1, Some(0x100));
}

#[test]
fn test_division_by_zero() {
    let result = assemble_line("add #(1/0),a", 0);
    assert!(result.is_err());
    let err = result.unwrap_err();
    assert!(err.to_string().contains("division by zero"), "{}", err);
}

#[test]
fn test_unary_bitnot() {
    // ~$0 encodes as AddLong since it's not a literal
    let result = assemble_line("add #~$0,a", 0).unwrap();
    assert_eq!(result.word1, Some(0xFFFFFF));
}

#[test]
fn test_unary_neg_non_literal() {
    // -(*) with pc=0
    let result = assemble_line("add #-(*),a", 0).unwrap();
    assert_eq!(result.word0 & 0x3F00, 0x0000); // immediate = 0
}

#[test]
fn test_empty_lines() {
    // Empty lines produce Statement::Empty in both passes
    let result = assemble("  nop\n\n  nop\n").unwrap();
    assert_eq!(result.segments.len(), 1);
    assert_eq!(result.segments[0].words.len(), 2);
}

#[test]
fn test_instruction_size_jcc_ea() {
    // Multi-line program exercises instruction_size for JccEa in pass1
    let result = assemble("  jcc p:$1234\nlabel:\n  nop\n").unwrap();
    assert_eq!(result.segments.len(), 1);
    assert_eq!(result.segments[0].words.len(), 3);
}

#[test]
fn test_instruction_size_plock() {
    // PlockEa with register EA -> size 1
    let result = assemble("  plock (r0)\n  nop\n").unwrap();
    assert_eq!(result.segments[0].words.len(), 2);
}

#[test]
fn test_parser_single_lt_in_expr() {
    // Single '<' is not a valid operator (need '<<')
    let result = assemble_line("add #($FF<2),a", 0);
    assert!(result.is_err());
}

#[test]
fn test_parser_single_gt_in_expr() {
    // Single '>' is not a valid operator (need '>>')
    let result = assemble_line("add #($FF>2),a", 0);
    assert!(result.is_err());
}

#[test]
fn test_parser_ea_invalid_after_rn() {
    let result = assemble_line("jmp p:(r0*n0)", 0);
    assert!(result.is_err());
}

// ---- Warning tests ----

fn get_warnings(asm: &str) -> Vec<AssembleWarning> {
    let result = assemble(asm).unwrap();
    result.warnings
}

#[test]
fn test_warn_bit_number_out_of_range() {
    let ws = get_warnings("bset #24,x:$ffe0");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange)
    );
    let ws = get_warnings("bset #31,x:$ffe0");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange)
    );
    // bit 23 is valid (last valid bit in 24-bit word)
    let ws = get_warnings("bset #23,x:$ffe0");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange)
    );
}

#[test]
fn test_warn_bit_number_brset() {
    let ws = get_warnings("brset #25,x:$ffff80,$0002");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange)
    );
}

#[test]
fn test_warn_duplicate_destination() {
    // add x,a with PM storing a to mem -- NO duplicate dest
    let ws = get_warnings("add x,a a,x:(r0)+");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "write to mem should not trigger duplicate dest"
    );
    // add x,a with PM reading mem into a -- duplicate dest
    let ws = get_warnings("add x,a x:(r0)+,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "PM reading into A while ALU writes A should warn"
    );
}

#[test]
fn test_warn_duplicate_dest_sub_register() {
    // ALU writes A, PM reads into a0 -- overlap
    let ws = get_warnings("add x,a x:(r0)+,a0");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination)
    );
    // ALU writes A, PM reads into ab -- overlap (composite uses both A and B)
    let ws = get_warnings("add x,a x:(r0)+,ab");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination)
    );
}

#[test]
fn test_warn_no_duplicate_dest_for_move() {
    // "move" ALU (byte 0) has no ALU dest -- no duplicate warning
    let ws = get_warnings("move x:(r0)+,a");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination)
    );
}

#[test]
fn test_warn_ssh_as_loop_count() {
    let ws = get_warnings("do ssh,$0010");
    assert!(ws.iter().any(|w| w.kind == WarningKind::SshAsLoopCount));
}

#[test]
fn test_warn_ssh_source_and_dest() {
    let ws = get_warnings("move ssh,ssh");
    assert!(ws.iter().any(|w| w.kind == WarningKind::SshSourceAndDest));
}

#[test]
fn test_no_warn_ssh_one_direction() {
    let ws = get_warnings("move ssh,r0");
    assert!(!ws.iter().any(|w| w.kind == WarningKind::SshSourceAndDest));
}

#[test]
fn test_warn_shift_count_out_of_range() {
    // 56-bit accumulators, so max meaningful shift is 55
    let ws = get_warnings("asl #56,a,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::ShiftCountOutOfRange)
    );
    // 55 is valid
    let ws = get_warnings("asl #55,a,a");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::ShiftCountOutOfRange)
    );
}

#[test]
fn test_warn_post_update_on_dest() {
    // Post-increment on r0, but r0 is not a PM destination -- no warning
    let ws = get_warnings("add x,a x:(r0)+,x0");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::PostUpdateOnDestination)
    );
}

#[test]
fn test_warn_movem_post_update_on_dest() {
    // movem p:(r0)+,r0 -- post-update on r0 which is also destination
    let ws = get_warnings("movem p:(r0)+,r0");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::PostUpdateOnDestination)
    );
    // movem p:(r0)+,r1 -- different register, no warning
    let ws = get_warnings("movem p:(r0)+,r1");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::PostUpdateOnDestination)
    );
}

#[test]
fn test_warn_mul_shift_out_of_range() {
    // mpy with shift >= 24 should warn
    let ws = get_warnings("mpy +x0,#24,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange)
    );
    // shift 23 is valid
    let ws = get_warnings("mpy +x0,#23,a");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange)
    );
}

#[test]
fn test_warn_interrupt_vector() {
    // 2-word instruction at odd address in vector table
    let ws = get_warnings("  org p:$0001\n  jmp $1000\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // 2-word instruction at even address -- ok (fits in slot)
    let ws = get_warnings("  org p:$0000\n  jmp $1000\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // Outside vector table (>= $100) -- no warning
    let ws = get_warnings("  org p:$0101\n  jmp $1000\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // Prohibited instructions in vector table: DO, REP, MOVEM
    let ws = get_warnings("  org p:$0000\n  rep #10\n  nop\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // MOVEM with control register -- prohibited
    let ws = get_warnings("  org p:$0004\n  movem p:(r0)+,la\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // MOVEM with non-control register -- not prohibited
    let ws = get_warnings("  org p:$0004\n  movem p:(r0)+,r0\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // MOVEC writing to LA -- prohibited
    let ws = get_warnings("  org p:$0008\n  movec #$100,la\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // MOVEC writing to x0 -- not prohibited
    let ws = get_warnings("  org p:$0008\n  movec #$100,x0\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // Same instruction outside vector table (>= $100) -- no warning
    let ws = get_warnings("  org p:$0100\n  movec #$100,la\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // MOVEP writing to control register (peripheral->register) -- prohibited
    let ws = get_warnings("  org p:$0004\n  movep y:$ffffa0,la\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
    // MOVEP reading from control register (register->peripheral) -- NOT prohibited
    let ws = get_warnings("  org p:$0004\n  movep la,y:$ffffa0\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector)
    );
}

// ---- Warning tests: uncovered parallel-move warning paths ----

// --- DuplicateDestination: RegToReg parallel move ---

#[test]
fn test_warn_dup_dest_reg_to_reg() {
    // PM RegToReg dst=a1 overlaps ALU dest a
    let ws = get_warnings("add b,a  a,a1");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "RegToReg: dst a1 overlaps ALU dest a"
    );
    // PM RegToReg dst=a overlaps ALU dest a
    let ws = get_warnings("add b,a  x0,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "RegToReg: dst a overlaps ALU dest a"
    );
    // No overlap when src/dst are both B registers while ALU dest is A
    let ws = get_warnings("add b,a  b,b0");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "RegToReg: dst b0 does not overlap ALU dest a"
    );
}

// --- DuplicateDestination: ImmToReg parallel move ---

#[test]
fn test_warn_dup_dest_imm_to_reg() {
    // PM ImmToReg dst=a1 overlaps ALU dest a
    let ws = get_warnings("add b,a  #$10,a1");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "ImmToReg: dst a1 overlaps ALU dest a"
    );
    // PM ImmToReg dst=a overlaps ALU dest a
    let ws = get_warnings("add b,a  #$10,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "ImmToReg: dst a overlaps ALU dest a"
    );
}

// --- DuplicateDestination: XYAbs write parallel move ---

#[test]
fn test_warn_dup_dest_xy_abs_write() {
    // PM XYAbs write: dst=a1 overlaps ALU dest a
    let ws = get_warnings("add b,a  x:$100,a1");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "XYAbs write: dst a1 overlaps ALU dest a"
    );
}

// --- DuplicateDestination: LAbs write parallel move ---

#[test]
fn test_warn_dup_dest_labs_write() {
    // PM LAbs write: dst=a overlaps ALU dest a
    let ws = get_warnings("add b,a  l:$100,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "LAbs write: dst a overlaps ALU dest a"
    );
}

// --- DuplicateDestination: LImm parallel move (strict overlap check) ---

#[test]
fn test_warn_dup_dest_limm_strict() {
    // LImm dst=a overlaps (a/a1/a10 are strict matches for accumulator A)
    let ws = get_warnings("add b,a  #$123456,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "LImm: dst a overlaps ALU dest a (strict)"
    );
    // LImm dst=a10: a10 is a strict match for accumulator A
    let ws = get_warnings("add b,a  #$123456,a10");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "LImm: dst a10 overlaps ALU dest a (strict)"
    );
    // LImm dst=b10: b10 does NOT overlap with accumulator A -> no DuplicateDestination,
    // but it IS an invalid PM4 destination -> gets InvalidPm4Destination instead
    let ws = get_warnings("add b,a  #$123456,b10");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "LImm: dst b10 does not overlap ALU dest a (strict)"
    );
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InvalidPm4Destination),
        "LImm: dst b10 is invalid PM4 destination"
    );
}

// --- DuplicateDestination: XImmReg parallel move ---

#[test]
fn test_warn_dup_dest_ximm_reg() {
    // XImmReg: x_reg=a overlaps ALU dest a
    let ws = get_warnings("add b,a  #$12,a  b,y0");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "XImmReg: x_reg=a overlaps ALU dest a"
    );
}

// --- DuplicateDestination: RegY parallel move ---

#[test]
fn test_warn_dup_dest_regy() {
    // RegY: y_reg=a in read direction -> y_reg is destination, overlaps ALU dest a
    let ws = get_warnings("add b,a  b,x0  y:(r0)+,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "RegY: y_reg=a overlaps ALU dest a"
    );
}

// --- DuplicateDestination: XYDouble x_reg == y_reg both writing ---

#[test]
fn test_warn_dup_dest_xydouble_same_reg() {
    // XYDouble both reading into the same register a -> x_reg==y_reg -> DuplicateDestination
    let ws = get_warnings("clr a  x:(r0)+,a  y:(r4)+,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "XYDouble: x_reg==y_reg==a (both write) triggers DuplicateDestination"
    );
    // Different destination registers should not warn for x_reg==y_reg duplicate,
    // though ALU-vs-PM overlap may still fire (use move which has no ALU dest)
    let ws = get_warnings("move  x:(r0)+,x0  y:(r4)+,y0");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "XYDouble: x_reg=x0, y_reg=y0 (different) should not warn DuplicateDestination"
    );
}

// --- DuplicateDestination: Pm0 with simple EA ---

#[test]
fn test_warn_dup_dest_pm0_simple_ea() {
    // Pm0 PostInc (simple EA): acc=a overlaps ALU dest a -> warns
    let ws = get_warnings("add b,a  a,x:(r0)+  x0,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "Pm0 PostInc: acc=a overlaps ALU dest a"
    );
    // Pm0 PostDec (simple EA): should also warn
    let ws = get_warnings("add b,a  a,x:(r0)-  x0,a");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "Pm0 PostDec: acc=a overlaps ALU dest a"
    );
    // Pm0 PreDec (NOT simple): should NOT warn (official assembler doesn't flag these)
    let ws = get_warnings("add b,a  a,x:-(r0)  x0,a");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "Pm0 PreDec: not a simple EA, no DuplicateDestination warning"
    );
}

// --- InvalidPm4Destination: XYAbs write with composite register ---

#[test]
fn test_warn_invalid_pm4_dest_xy_abs() {
    // XYAbs write with a10 destination (composite) -- ALU dest is b so no overlap
    let ws = get_warnings("add b,b  x:$100,a10");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InvalidPm4Destination),
        "XYAbs write: a10 is an invalid PM4 destination"
    );
    // x is an invalid PM4 destination
    let ws = get_warnings("add b,b  x:$100,x");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InvalidPm4Destination),
        "XYAbs write: x (composite) is an invalid PM4 destination"
    );
}

// --- InvalidPm4Destination: LImm with composite register that doesn't overlap ALU dest ---

#[test]
fn test_warn_invalid_pm4_dest_limm() {
    // LImm with x composite register (doesn't overlap ALU dest a) -> InvalidPm4Destination
    let ws = get_warnings("add b,a  #$123456,x");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InvalidPm4Destination),
        "LImm: x (composite) is an invalid PM4 destination"
    );
    // LImm with y composite register
    let ws = get_warnings("add b,a  #$123456,y");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InvalidPm4Destination),
        "LImm: y (composite) is an invalid PM4 destination"
    );
}

// --- SshSourceAndDest in parallel RegToReg move ---

#[test]
fn test_warn_ssh_source_and_dest_parallel() {
    // SSH as both source and destination in a parallel RegToReg move
    let ws = get_warnings("add b,a  ssh,ssh");
    assert!(
        ws.iter().any(|w| w.kind == WarningKind::SshSourceAndDest),
        "Parallel RegToReg ssh,ssh should warn SshSourceAndDest"
    );
}

// --- InstructionInInterruptVector: additional prohibited instructions ---

#[test]
fn test_warn_interrupt_vector_do() {
    // DO instruction in interrupt vector table (pc < $40) -> prohibited
    let ws = get_warnings("  org p:$0000\n  do #10,$0004\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "DO instruction at P:$0000 should warn InstructionInInterruptVector"
    );
}

#[test]
fn test_warn_interrupt_vector_movem_control() {
    // MOVEM storing a control register (SSH) at interrupt vector address
    let ws = get_warnings("  org p:$0010\n  movem ssh,p:$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "MOVEM with SSH at P:$0010 should warn InstructionInInterruptVector"
    );
    // MOVEM with non-control register r0 at interrupt vector - not prohibited
    let ws = get_warnings("  org p:$0010\n  movem r0,p:$100\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "MOVEM with r0 at P:$0010 should not warn"
    );
}

#[test]
fn test_warn_interrupt_vector_movec_reg_write() {
    // MOVEC writing to a control register (la) at interrupt vector
    let ws = get_warnings("  org p:$0010\n  movec r0,la\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "MOVEC r0,la at P:$0010 should warn (writing control reg la)"
    );
    // MOVEC reading from SSH to r0 - r0 is not a control register, not prohibited
    let ws = get_warnings("  org p:$0010\n  movec ssh,r0\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "MOVEC ssh,r0 at P:$0010 should not warn (r0 is not a control reg)"
    );
}

#[test]
fn test_warn_interrupt_vector_bclr_sr() {
    // BCLR targeting SR at interrupt vector address -> prohibited
    let ws = get_warnings("  org p:$0010\n  bclr #5,sr\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "BCLR #5,sr at P:$0010 should warn (SR is a control reg)"
    );
    // BCLR targeting OMR (not in prohibited list) -> not prohibited
    let ws = get_warnings("  org p:$0010\n  bclr #5,omr\n");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::InstructionInInterruptVector),
        "BCLR #5,omr at P:$0010 should not warn (OMR is not a prohibited reg)"
    );
}

#[test]
fn test_warn_interrupt_vector_ssh_source_and_dest() {
    // MOVEC ssh,ssh at pc=0 triggers SshSourceAndDest
    let ws = get_warnings("  org p:$0000\n  movec ssh,ssh\n");
    assert!(
        ws.iter().any(|w| w.kind == WarningKind::SshSourceAndDest),
        "MOVEC ssh,ssh at P:$0000 should warn SshSourceAndDest"
    );
}

#[test]
fn test_parallel_imm_to_reg_sizes() {
    // Bare literal <= $FF -> short form (no extension word, 1-word instruction)
    let r = assemble_line("add b,a  #$10,x0", 0).unwrap();
    assert!(
        r.word1.is_none(),
        "ImmToReg #$10: bare literal <= 0xFF should use 1-word short form"
    );
    // Bare literal > $FF -> long form (extension word, 2-word instruction)
    let r = assemble_line("add b,a  #$100,x0", 0).unwrap();
    assert!(
        r.word1.is_some(),
        "ImmToReg #$100: bare literal > 0xFF should use 2-word long form"
    );
}

// --- instruction_size for EA-based instructions ---

#[test]
fn test_instruction_size_jmp_ea() {
    // JmpEa with register EA (no extension) -> 1 word
    let r = assemble_line("jmp (r0)", 0).unwrap();
    assert!(r.word1.is_none(), "jmp (r0) should be 1 word");
    // JmpEa with absolute addr -> 2 words
    let r = assemble_line("jmp p:$1234", 0).unwrap();
    assert!(r.word1.is_some(), "jmp p:$1234 should be 2 words");
}

#[test]
fn test_instruction_size_jsr_ea() {
    let r = assemble_line("jsr (r0)", 0).unwrap();
    assert!(r.word1.is_none(), "jsr (r0) should be 1 word");
}

#[test]
fn test_instruction_size_lua() {
    // Lua with register EA (PostInc, no extension) -> 1 word
    let r = assemble_line("lua (r0)+,r1", 0).unwrap();
    assert!(r.word1.is_none(), "lua (r0)+,r1 should be 1 word");
}

#[test]
fn test_instruction_size_bit_test_ea() {
    // Bit op with EA (absolute addr) -> 2 words
    let r = assemble_line("bclr #3,x:$0040", 0).unwrap();
    assert!(
        r.word1.is_some(),
        "bclr with abs addr > $3f should be 2 words"
    );
    // Bit op with reg target -> 1 word
    let r = assemble_line("bclr #3,omr", 0).unwrap();
    assert!(r.word1.is_none(), "bclr with reg target should be 1 word");
}

// --- reg_overlaps_acc_strict: LImm does not warn for A0/B0/A2/B2 ---

#[test]
fn test_limm_strict_no_warn_for_a0() {
    // LImm uses strict overlap check. The LImm form (#$xxxxxx,a10) is a 24-bit L-move.
    // For accumulator A: strict overlap matches A, A1, A10 only (not A0, A2).
    // a10 IS a strict match (composite mantissa+exponent), so it DOES warn.
    // b10 does NOT overlap A -> no DuplicateDestination, but gets InvalidPm4Destination.
    let ws = get_warnings("add b,a  #$123456,b10");
    assert!(
        !ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "LImm: b10 does not strictly overlap ALU dest a (no DuplicateDestination)"
    );
    // b10 is a composite register that is an invalid PM4 dest
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::InvalidPm4Destination),
        "LImm: b10 triggers InvalidPm4Destination"
    );
}

// 1a: IndexedN addressing (r0+n0) in non-lua context
#[test]
fn test_indexed_n_in_bit_ea() {
    // bclr with EA using IndexedN mode
    roundtrip("bclr #3,x:(r0)+n0", 0);
}

// 1b: Force-long absolute address >addr
#[test]
fn test_force_long_abs_movec() {
    // movec x:>$10,sr -- force long absolute addressing
    let result = assemble_line("movec x:>$10,sr", 0).unwrap();
    // Force-long should produce a 2-word encoding
    assert!(
        result.word1.is_some(),
        "force-long x:>$10 should be 2 words"
    );
}

#[test]
fn test_jmp_force_long() {
    // jmp >$100 -- force long for jump targets
    let result = assemble_line("jmp >$100", 0).unwrap();
    assert!(result.word1.is_some(), "jmp >$100 should use long EA form");
}

#[test]
fn test_jsr_force_long() {
    let result = assemble_line("jsr >$100", 0).unwrap();
    assert!(result.word1.is_some(), "jsr >$100 should use long EA form");
}

// 1c: BitTarget::Ea with immediate
#[test]
fn test_bclr_abs_addr() {
    // bclr on absolute memory address (non-pp, non-aa range)
    let result = assemble_line("bclr #5,x:$1234", 0).unwrap();
    assert!(
        result.word1.is_some(),
        "bclr on large addr should be 2-word"
    );
}

#[test]
fn test_btst_abs_addr() {
    let result = assemble_line("btst #3,y:$100", 0).unwrap();
    assert!(
        result.word1.is_some(),
        "btst on large addr should be 2-word"
    );
}

// 1d: BitTarget::Ea with absolute address in jclr
#[test]
fn test_jclr_abs_addr() {
    roundtrip("jclr #0,x:$0040,$0040", 0);
}

// 1e: Movec with absolute address EA, movem with absolute address
#[test]
fn test_movec_abs_addr_ea_read() {
    // movec x:$1234,sr -- movec with absolute address that exceeds aa range
    roundtrip("movec x:$1234,sr", 0);
}

#[test]
fn test_movec_abs_addr_ea_write() {
    roundtrip("movec sr,x:$1234", 0);
}

#[test]
fn test_movem_abs_ea_read() {
    // movem p:$100,r0 -- movem with address that exceeds aa range -> ea form
    roundtrip("movem p:$0100,r0", 0);
}

#[test]
fn test_movem_abs_ea_write() {
    roundtrip("movem r0,p:$0100", 0);
}

#[test]
fn test_movem_force_long_read() {
    // movem p:>$20,r0 -- force long even for small address
    let result = assemble_line("movem p:>$20,r0", 0).unwrap();
    assert!(
        result.word1.is_some(),
        "movem p:>$20 should force 2-word encoding"
    );
}

#[test]
fn test_movem_force_long_write() {
    let result = assemble_line("movem r0,p:>$20", 0).unwrap();
    assert!(
        result.word1.is_some(),
        "movem r0,p:>$20 should force 2-word encoding"
    );
}

// 1f: Character literal syntax
#[test]
fn test_char_literal() {
    // move #'A',x0 -- character literal as immediate (ASCII 0x41)
    let result = assemble_line("movec #'A',x0", 0).unwrap();
    // 'A' = 0x41; MovecImm encoding
    let (disasm, _) = dsp56300_disasm::disassemble(0, result.word0, result.word1.unwrap_or(0));
    assert!(
        disasm.contains("$41") || disasm.contains("$0041"),
        "char literal 'A' should encode as 0x41, got: {}",
        disasm
    );
}

// 1g: norm instruction (already covered by test_norm, but add R register variants)
#[test]
fn test_norm_r7() {
    roundtrip("norm r7,b", 0);
}

// 1h: AbsAddr in dual moves
#[test]
fn test_dual_move_abs_x_read() {
    // x:$addr,reg second_move -- dual move with absolute X address (read side)
    let result = assemble_line("clr a x:$0010,x0 a,y:(r4)", 0).unwrap();
    assert_ne!(result.word0, 0);
}

#[test]
fn test_dual_move_abs_x_write() {
    // reg,x:$addr second_move -- dual move with absolute X address (write side)
    let result = assemble_line("clr a a,x:$0010 y:(r4)+,y0", 0).unwrap();
    assert_ne!(result.word0, 0);
}

// 1i: XYMem with immediate EA (force-long immediate in parallel move)
#[test]
fn test_parallel_imm_force_long() {
    // #>$10,x0 -- force-long for a value that would normally fit in PM3 short form.
    // Without '>', $10 fits in 8 bits and uses 1-word PM3 encoding.
    // With '>', it must use the 2-word long form.
    let short = assemble_line("clr a #$10,x0", 0).unwrap();
    assert!(short.word1.is_none(), "short form should be 1 word");

    let long = assemble_line("clr a #>$10,x0", 0).unwrap();
    assert!(
        long.word1.is_some(),
        "force-long immediate should produce 2-word encoding"
    );
    assert_eq!(long.word1.unwrap(), 0x000010);
}

// 1k: vsl instruction
#[test]
fn test_vsl_assemble() {
    // vsl a,0,(r0) -- vsl with i_bit=0
    let result = assemble_line("vsl a,0,(r0)", 0).unwrap();
    assert_ne!(result.word0, 0);
}

#[test]
fn test_vsl_i_bit_1() {
    let result = assemble_line("vsl a,1,(r0)", 0).unwrap();
    assert_ne!(result.word0, 0);
}

// 2a: tcc register pair errors
#[test]
fn test_tcc_second_pair_src_not_r() {
    // tcc b,a r0,x0 -- second pair dst must be R register
    assert!(
        assemble_line("tcc b,a x0,r0", 0).is_err(),
        "tcc second pair src must be R register"
    );
}

#[test]
fn test_tcc_second_pair_dst_not_r() {
    assert!(
        assemble_line("tcc b,a r0,x0", 0).is_err(),
        "tcc second pair dst must be R register"
    );
}

// 2b: norm source must be R register
#[test]
fn test_norm_non_r_register() {
    assert!(
        assemble_line("norm x0,a", 0).is_err(),
        "norm source must be R register"
    );
}

// 2d: Expected x: or y: for bit target
#[test]
fn test_bclr_p_space_error() {
    assert!(
        assemble_line("bclr #5,p:$100", 0).is_err(),
        "p: space not valid for bit target"
    );
}

// 2e: Expected mr, ccr, or omr
#[test]
fn test_andi_bad_dest() {
    assert!(
        assemble_line("andi #$FF,x0", 0).is_err(),
        "andi only accepts mr, ccr, or omr"
    );
}

#[test]
fn test_ori_bad_dest() {
    assert!(
        assemble_line("ori #$FF,x0", 0).is_err(),
        "ori only accepts mr, ccr, or omr"
    );
}

// 2g: Multiply source validation
#[test]
fn test_mpy_bad_pair() {
    // mpy x0,x1,a -- x0,x1 is not a valid QQQ pair (only x1,x0 is)
    // But commutation may fix it. Use a truly invalid pair:
    // mpy x0,a -- wrong number of operands for parallel multiply
    assert!(
        assemble_line("mpy x0,a", 0).is_err(),
        "mpy with wrong operand count should error"
    );
}

// 2h: Mnemonic doesn't accept source type
#[test]
fn test_adc_reject_single_reg() {
    // adc only accepts x/y pair source, not single register
    assert!(
        assemble_line("adc x0,a", 0).is_err(),
        "adc does not accept single register source"
    );
}

#[test]
fn test_or_reject_acc_source() {
    // or doesn't accept accumulator source (only register)
    assert!(
        assemble_line("or b,a", 0).is_err(),
        "or does not accept accumulator source"
    );
}

#[test]
fn test_sbc_reject_single_reg() {
    assert!(
        assemble_line("sbc x0,a", 0).is_err(),
        "sbc does not accept single register source"
    );
}

#[test]
fn test_eor_reject_acc_source() {
    assert!(
        assemble_line("eor b,a", 0).is_err(),
        "eor does not accept accumulator source"
    );
}

#[test]
fn test_addr_reject_xy_pair() {
    // addr only accepts accumulator, not x/y pair
    assert!(
        assemble_line("addr x,a", 0).is_err(),
        "addr does not accept x/y pair source"
    );
}

#[test]
fn test_addl_reject_register() {
    // addl only accepts accumulator source
    assert!(
        assemble_line("addl x0,a", 0).is_err(),
        "addl does not accept register source"
    );
}

// 2i: Expected accumulator
#[test]
fn test_add_missing_dest() {
    // add x0 -- missing destination accumulator (no comma)
    assert!(
        assemble_line("add x0", 0).is_err(),
        "add x0 missing destination should error"
    );
}

// 3a: Sign backtrack in multiply
#[test]
fn test_mpy_sign_backtrack() {
    // mpy without sign prefix -- the parser may try + then backtrack
    roundtrip("mpy +x0,x0,a", 0);
    roundtrip("mpy -x0,x0,a", 0);
}

// 3b: Empty statement / label-only lines / comment-only lines
#[test]
fn test_empty_line() {
    let result = assemble("\n").unwrap();
    assert!(result.segments.is_empty());
}

#[test]
fn test_comment_only_line() {
    let result = assemble("  ; comment\n").unwrap();
    assert!(result.segments.is_empty());
}

#[test]
fn test_label_only_no_colon() {
    // A bare identifier as a label (without colon) followed by newline
    let result = assemble("org p:$0\nmylabel\nnop\n").unwrap();
    assert!(!result.segments.is_empty());
}

// Force-long in bra/bsr
#[test]
fn test_bra_force_long() {
    let result = assemble_line("bra >$100", 0).unwrap();
    assert!(result.word1.is_some(), "bra >$100 should use long form");
}

#[test]
fn test_bsr_force_long() {
    let result = assemble_line("bsr >$100", 0).unwrap();
    assert!(result.word1.is_some(), "bsr >$100 should use long form");
}

// jcc/jscc force-long
#[test]
fn test_jcc_force_long() {
    let result = assemble_line("jcc >$10", 0).unwrap();
    assert!(result.word1.is_some(), "jcc >$10 should use long EA form");
}

#[test]
fn test_jscc_force_long() {
    let result = assemble_line("jscc >$10", 0).unwrap();
    assert!(result.word1.is_some(), "jscc >$10 should use long EA form");
}

// bcc/bscc force-long
#[test]
fn test_bcc_force_long() {
    let result = assemble_line("bcc >$100", 0).unwrap();
    assert!(result.word1.is_some(), "bcc >$100 should use long form");
}

#[test]
fn test_bscc_force_long() {
    let result = assemble_line("bscc >$100", 0).unwrap();
    assert!(result.word1.is_some(), "bscc >$100 should use long form");
}

// EA update destination must be R register
#[test]
fn test_ea_update_non_r_dest() {
    assert!(
        assemble_line("move (r0)+,x0", 0).is_err(),
        "EA update destination must be R register"
    );
}

// vsl with invalid bit
#[test]
fn test_vsl_bad_bit() {
    assert!(
        assemble_line("vsl a,2,(r0)", 0).is_err(),
        "vsl bit must be 0 or 1"
    );
}

// movec force-long read from memory
#[test]
fn test_movec_force_long_read() {
    let result = assemble_line("movec x:>$20,sr", 0).unwrap();
    assert!(
        result.word1.is_some(),
        "movec x:>$20 should force 2-word encoding"
    );
}

#[test]
fn test_movec_force_long_write() {
    let result = assemble_line("movec sr,x:>$20", 0).unwrap();
    assert!(
        result.word1.is_some(),
        "movec sr,x:>$20 should force 2-word encoding"
    );
}

// Unknown parallel ALU mnemonic
#[test]
fn test_unknown_parallel_mnemonic() {
    assert!(
        assemble_line("xyz x0,a", 0).is_err(),
        "unknown mnemonic should error"
    );
}

// do/dor forever
#[test]
fn test_do_forever_roundtrip() {
    roundtrip("do forever,$0051", 0);
}

#[test]
fn test_dor_forever_roundtrip() {
    roundtrip("dor forever,$0051", 0);
}

// Parallel Pm0 write direction
#[test]
fn test_parallel_pm0_write_direction() {
    // a,x:(r0)+ x0,a -- Pm0 pattern
    roundtrip("clr a a,x:(r0)+ x0,a", 0);
}

// RegY write direction in dual moves
#[test]
fn test_dual_move_reg_y_write_direction() {
    // s1,d1 d2,y:(ea) form
    let result = assemble_line("clr a a,x0 y0,y:(r4)+", 0).unwrap();
    assert_ne!(result.word0, 0);
}

// XImmReg parallel move (immediate + second pair)
#[test]
fn test_ximm_reg_dual() {
    roundtrip("clr a #>$000012,x0 a,y0", 0);
}

// Bit branch (brclr/brset) with absolute address bit target
#[test]
fn test_brclr_abs_addr() {
    roundtrip("brclr #0,x:$0020,$000010", 0);
}

#[test]
fn test_brset_abs_addr() {
    roundtrip("brset #0,y:$0020,$000010", 0);
}

// Bit branch with ea target
#[test]
fn test_jset_ea_abs() {
    // jset with an absolute address for the bit target (large addr -> ea form)
    roundtrip("jset #5,x:$0040,$0040", 0);
}

// Parallel move: Y memory immediate in RegYImm
#[test]
fn test_parallel_reg_y_imm() {
    roundtrip("clr a a,x0 #>$000012,y0", 0);
}

// Ifcc parallel move
#[test]
fn test_ifcc_parallel() {
    // Various condition codes
    roundtrip("add x0,a ifcs", 0);
    roundtrip("sub x0,a ifmi", 0);
}

// Ifcc.U parallel move
#[test]
fn test_ifcc_u_parallel() {
    roundtrip("add x0,a ifcs.u", 0);
    roundtrip("sub x0,a ifmi.u", 0);
}

// lsr instruction forms
#[test]
fn test_lsr_imm() {
    roundtrip("lsr #$01,a", 0);
}

#[test]
fn test_lsr_reg() {
    // lsr with register source: uses sss-class register
    roundtrip("lsr x0,a", 0);
}

#[test]
fn test_lsl_reg() {
    roundtrip("lsl x0,a", 0);
}

// asl/asr register forms
#[test]
fn test_asl_reg() {
    roundtrip("asl x0,a,a", 0);
}

#[test]
fn test_asr_reg() {
    roundtrip("asr x0,a,a", 0);
}

// Dmac variants (use valid QQQQ pairs: x0,x0 / y0,y0 / x1,x0 / y1,y0 etc.)
#[test]
fn test_dmacss() {
    roundtrip("dmacss +x0,x0,a", 0);
}

#[test]
fn test_dmacsu() {
    roundtrip("dmacsu +x0,x0,a", 0);
}

#[test]
fn test_dmacuu() {
    roundtrip("dmacuu +x0,x0,a", 0);
}

// Mac/Mpy SU/UU variants (use valid QQQQ pairs)
#[test]
fn test_macsu() {
    roundtrip("macsu +x0,x0,a", 0);
}

#[test]
fn test_mpysu() {
    roundtrip("mpysu +x0,x0,a", 0);
}

#[test]
fn test_mpyuu() {
    roundtrip("mpyuu +x0,x0,a", 0);
}

// macri / mpyri
#[test]
fn test_mpyri() {
    roundtrip("mpyri +#$000010,x0,a", 0);
}

#[test]
fn test_maci() {
    roundtrip("maci +#$000010,x0,a", 0);
}

#[test]
fn test_macri() {
    roundtrip("macri +#$000010,x0,a", 0);
}

// jmp/jsr with force-short
#[test]
fn test_jmp_force_short() {
    // jmp <$42 -- force short addressing
    let result = assemble_line("jmp <$42", 0).unwrap();
    assert!(result.word1.is_none(), "jmp <$42 should use short form");
}

#[test]
fn test_jsr_force_short() {
    let result = assemble_line("jsr <$42", 0).unwrap();
    assert!(result.word1.is_none(), "jsr <$42 should use short form");
}

// Parallel L-mem absolute write
#[test]
fn test_parallel_l_abs_write() {
    roundtrip("clr a a,l:$0010", 0);
}

// EaUpdate bare (no comma/dest)
#[test]
fn test_ea_update_bare() {
    // move (r0)+ -- bare EA update, inferred register
    // Disassembler outputs "move (r0)+,r0", so just verify it assembles
    let result = assemble_line("move (r0)+", 0).unwrap();
    let explicit = assemble_line("move (r0)+,r0", 0).unwrap();
    assert_eq!(
        result.word0, explicit.word0,
        "bare EA update should match explicit form"
    );
}

// Multiple parallel multiply forms (using valid QQQ pairs)
#[test]
fn test_parallel_mpy_all_forms() {
    roundtrip("mpy +x0,x0,a", 0);
    roundtrip("mpyr -x0,x0,a", 0);
    roundtrip("mac +x0,x0,a", 0);
    roundtrip("macr -x0,x0,a", 0);
}

// Plock / punlock with absolute address
#[test]
fn test_plock_abs() {
    let result = assemble_line("plock $1234", 0).unwrap();
    assert!(result.word1.is_some() || result.word0 != 0);
}

#[test]
fn test_punlock_abs() {
    let result = assemble_line("punlock $1234", 0).unwrap();
    assert!(result.word1.is_some() || result.word0 != 0);
}

// Force-short memory address modifier in memspace
#[test]
fn test_memspace_force_short() {
    // x:< prefix (force-short) -- should be accepted and ignored
    let result = assemble_line("bclr #3,x:<$20", 0).unwrap();
    assert!(result.word1.is_none(), "x:< should use short form");
}

#[test]
fn test_memspace_force_io_short() {
    // x:<< prefix (force IO-short) -- should be accepted
    let result = assemble_line("bclr #3,x:<<$05", 0).unwrap();
    assert!(result.word1.is_none());
}

// A1: PostUpdateOnDestination warning for parallel X-mem read into address register

#[test]
fn test_warn_post_update_on_dest_parallel() {
    // x:(r0)+,r0 -- post-update cannot occur on r0 (also a move destination)
    let ws = get_warnings("  clr a  x:(r0)+,r0\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::PostUpdateOnDestination),
        "x:(r0)+,r0 should warn PostUpdateOnDestination"
    );
}

// A2: XYDouble same-register DuplicateDestination (both X and Y write to 'a')

#[test]
fn test_warn_xydouble_both_write_to_a() {
    // x:(r0)+,a y:(r4)+,a -- X and Y both writing 'a' -> DuplicateDestination
    let ws = get_warnings("  clr a  x:(r0)+,a  y:(r4)+,a\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "XYDouble with x_reg==y_reg==a should warn DuplicateDestination"
    );
}

// A3: DuplicateDestination for LAbs and XReg write PM types

#[test]
fn test_warn_dup_dest_labs_a() {
    // l:$100,a with ALU dest a -> DuplicateDestination
    let ws = get_warnings("  add b,a  l:$100,a\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "LAbs write: l:$100,a overlaps ALU dest a"
    );
}

#[test]
fn test_warn_dup_dest_xreg_a() {
    // x:(r0)+,a a,y0 -- XReg x_reg=a overlaps ALU dest a
    let ws = get_warnings("  add b,a  x:(r0)+,a  a,y0\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination),
        "XReg: x_reg=a overlaps ALU dest a"
    );
}

// A4: BitNumberOutOfRange for additional bit-op mnemonic variants

#[test]
fn test_warn_bchg_bit_out_of_range() {
    let ws = get_warnings("  bchg #24,x:$ffe0\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "bchg #24 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_btst_bit_out_of_range() {
    let ws = get_warnings("  btst #30,x:$ffe0\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "btst #30 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_jclr_bit_out_of_range() {
    let ws = get_warnings("  jclr #25,x:$ffe0,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "jclr #25 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_jset_bit_out_of_range() {
    let ws = get_warnings("  jset #25,x:$ffe0,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "jset #25 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_jsclr_bit_out_of_range() {
    let ws = get_warnings("  jsclr #24,x:$ffffc5,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "jsclr #24 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_jsset_bit_out_of_range() {
    let ws = get_warnings("  jsset #24,x:$ffffc5,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "jsset #24 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_brclr_bit_out_of_range() {
    // brclr/brset require pp/qq/aa or register address form
    let ws = get_warnings("  brclr #24,x:$ffffc5,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "brclr #24 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_brset_bit_out_of_range() {
    let ws = get_warnings("  brset #24,x:$ffffc5,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "brset #24 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_bsclr_bit_out_of_range() {
    let ws = get_warnings("  bsclr #24,x:$ffffc5,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "bsclr #24 should warn BitNumberOutOfRange"
    );
}

#[test]
fn test_warn_bsset_bit_out_of_range() {
    let ws = get_warnings("  bsset #24,x:$ffffc5,$100\n");
    assert!(
        ws.iter()
            .any(|w| w.kind == WarningKind::BitNumberOutOfRange),
        "bsset #24 should warn BitNumberOutOfRange"
    );
}

// A5: Instruction sizes via assemble_line

#[test]
fn test_size_move_short_disp() {
    // MoveShortDisp is always 1 word (short displacement encoded in word0)
    let r = assemble_line("move x:(r0+16),x0", 0).unwrap();
    assert!(r.word1.is_none(), "MoveShortDisp should be 1 word");
}

#[test]
fn test_size_ifcc() {
    // Ifcc parallel move is always 1 word
    let r = assemble_line("add x0,a ifcs", 0).unwrap();
    assert!(r.word1.is_none(), "Ifcc parallel should be 1 word");
}

#[test]
fn test_size_ea_update() {
    // EaUpdate (bare register EA update in parallel move) is 1 word
    let r = assemble_line("clr a (r0)+", 0).unwrap();
    assert!(r.word1.is_none(), "EaUpdate should be 1 word");
}

#[test]
fn test_size_jscc_ea() {
    // JsccEa with register EA is 1 word
    let r = assemble_line("jscc (r0)", 0).unwrap();
    assert!(r.word1.is_none(), "JsccEa with reg EA should be 1 word");
}

#[test]
fn test_size_lua() {
    // Lua with PostInc EA (no extension) is 1 word
    let r = assemble_line("lua (r0)+,r1", 0).unwrap();
    assert!(r.word1.is_none(), "lua (r0)+,r1 should be 1 word");
}

#[test]
fn test_size_movep_pp() {
    // movep with pp peripheral address is 1 word
    let r = assemble_line("movep x:$ffe0,x0", 0).unwrap();
    assert!(r.word1.is_none(), "movep with pp addr should be 1 word");
}

#[test]
fn test_size_bchg_ea() {
    // bchg with EA absolute address (outside aa range) is 2 words
    let r = assemble_line("bchg #5,x:$1234", 0).unwrap();
    assert!(r.word1.is_some(), "bchg with abs EA addr should be 2 words");
    assert_eq!(r.word1.unwrap(), 0x1234);
}

// A6: assemble_line with empty string returns Err

#[test]
fn test_assemble_line_empty_string() {
    let r = assemble_line("", 0);
    assert!(r.is_err(), "assemble_line(\"\") should return Err");
}

// A7: ImmToReg with symbol reference forces long (2-word) form

#[test]
fn test_imm_to_reg_symbol_forces_long() {
    // #val where val is a symbol: pmove_ext_size returns 1 (long form)
    let r = assemble("val equ $123\n  clr a  #val,x0\n").unwrap();
    assert_eq!(
        r.segments[0].words.len(),
        2,
        "symbol reference in ImmToReg should produce 2-word instruction"
    );
    assert_eq!(
        r.segments[0].words[1], 0x123,
        "extension word should be symbol value"
    );
}

// A8: Comment-only lines produce Statement::Empty; nop still assembles

#[test]
fn test_comment_only_then_nop() {
    let r = assemble("  ; just a comment\n  nop\n").unwrap();
    assert_eq!(r.segments.len(), 1, "should have one segment");
    assert_eq!(
        r.segments[0].words.len(),
        1,
        "comment-only line produces no words; nop produces 1"
    );
    assert_eq!(r.segments[0].words[0], 0x000000, "nop encoding");
}

// --- Mismatched Rn/Nn register pairs must be rejected ---

#[test]
fn test_mismatched_rn_nn_indexed() {
    // (r1+n3) -- indexed EA with mismatched N register
    assert!(assemble_line("move x:(r1+n3),a", 0).is_err());
    // (r0+n0) -- matching pair should succeed
    assert!(assemble_line("move x:(r0+n0),a", 0).is_ok());
}

#[test]
fn test_mismatched_rn_nn_post_inc() {
    // (r2)+n5 -- post-increment with mismatched N register
    assert!(assemble_line("move x:(r2)+n5,a", 0).is_err());
    // (r2)+n2 -- matching pair should succeed
    assert!(assemble_line("move x:(r2)+n2,a", 0).is_ok());
}

#[test]
fn test_mismatched_rn_nn_post_dec() {
    // (r3)-n7 -- post-decrement with mismatched N register
    assert!(assemble_line("move x:(r3)-n7,a", 0).is_err());
    // (r3)-n3 -- matching pair should succeed
    assert!(assemble_line("move x:(r3)-n3,a", 0).is_ok());
}

#[test]
fn test_mismatched_rn_nn_lua() {
    // lua (r1+n3),r0 -- LUA indexed with mismatched N register
    assert!(assemble_line("lua (r1+n3),r0", 0).is_err());
    // lua (r1+n1),r0 -- matching pair should succeed
    assert!(assemble_line("lua (r1+n1),r0", 0).is_ok());
}

#[test]
fn test_mismatched_rn_nn_lua_post_inc() {
    // lua (r4)+n6,r0 -- LUA post-increment with mismatched N register
    assert!(assemble_line("lua (r4)+n6,r0", 0).is_err());
    // lua (r4)+n4,r0 -- matching pair should succeed
    assert!(assemble_line("lua (r4)+n4,r0", 0).is_ok());
}

#[test]
fn test_mismatched_rn_nn_lua_post_dec() {
    // lua (r5)-n2,r0 -- LUA post-decrement with mismatched N register
    assert!(assemble_line("lua (r5)-n2,r0", 0).is_err());
    // lua (r5)-n5,r0 -- matching pair should succeed
    assert!(assemble_line("lua (r5)-n5,r0", 0).is_ok());
}

// --- Symbol table exposed in AssembleResult ---

#[test]
fn test_symbols_in_result() {
    let r = assemble(
        "
        org p:$0010
foo:    nop
bar:    nop
        org p:$0020
baz:    nop
myconst: equ $1234
        ",
    )
    .unwrap();
    assert_eq!(r.symbols["foo"], 0x0010);
    assert_eq!(r.symbols["bar"], 0x0011);
    assert_eq!(r.symbols["baz"], 0x0020);
    assert_eq!(r.symbols["myconst"], 0x1234); // parser lowercases all identifiers
}

#[test]
fn test_jcc_forward_label_address() {
    // Regression: encode_jcc_ea used to shorten label-based Jcc to 1-word
    // when the resolved address < 0x1000, but instruction_size always counted 2.
    // This caused all subsequent labels to be offset by 1 word per shortened Jcc.
    let r = assemble(
        "
        org p:$0000
        jgt target
        nop
        nop
target  rts
        ",
    )
    .unwrap();
    // target should be at $0004: jgt(2 words) + nop + nop
    assert_eq!(r.symbols["target"], 0x0004);
    // Verify the jgt extension word contains the correct target address
    let seg = r
        .segments
        .iter()
        .find(|s| s.space == MemorySpace::P)
        .unwrap();
    assert_eq!(
        seg.words[1], 0x000004,
        "jgt extension word should point to target"
    );
}

#[test]
fn test_multiple_jcc_forward_labels() {
    // Two forward-reference conditional jumps: each must be 2 words
    let r = assemble(
        "
        org p:$0000
        jpl mid
        nop
mid     jgt done
        nop
done    rts
        ",
    )
    .unwrap();
    // jpl(2) + nop(1) = 3 -> mid at $0003
    assert_eq!(r.symbols["mid"], 0x0003);
    // jgt(2) + nop(1) = 3 -> done at $0003 + 3 = $0006
    assert_eq!(r.symbols["done"], 0x0006);
}
