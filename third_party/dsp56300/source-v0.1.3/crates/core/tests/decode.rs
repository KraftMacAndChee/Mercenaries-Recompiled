use dsp56300_core::decode::decode;
use dsp56300_core::decode::instruction_length;
use dsp56300_core::reg;
use dsp56300_core::*;

/// Build an opcode from a 24-char template and field assignments.
/// Fixed bits ('0'/'1') are set from the template; variable bits are
/// filled from the `fields` slice, MSB-first within each letter group.
fn make_opcode(tmpl: &str, fields: &[(u8, u32)]) -> u32 {
    let t = tmpl.as_bytes();
    assert_eq!(t.len(), 24);
    let mut opc: u32 = 0;
    // Set fixed bits
    for (i, &b) in t.iter().enumerate() {
        if b == b'1' {
            opc |= 1 << (23 - i);
        }
    }
    // Fill variable fields
    for &(letter, value) in fields {
        let width = t.iter().filter(|&&b| b == letter).count();
        let mut bit_idx = width as i32 - 1;
        for (i, &b) in t.iter().enumerate() {
            if b == letter && (value >> bit_idx) & 1 != 0 {
                opc |= 1 << (23 - i);
            }
            if b == letter {
                bit_idx -= 1;
            }
        }
    }
    opc
}

#[test]
fn test_make_opcode() {
    // nop: all fixed zeros
    assert_eq!(make_opcode("000000000000000000000000", &[]), 0);
    // rts: 0x00000C
    assert_eq!(make_opcode("000000000000000000001100", &[]), 0x00000C);
    // add #$3F,A: iiiiii=0x3F, d=0(A)
    assert_eq!(
        make_opcode("0000000101iiiiii1000d000", &[(b'i', 0x3F), (b'd', 0)]),
        0x017F80
    );
    // add #$3F,B: iiiiii=0x3F, d=1(B)
    assert_eq!(
        make_opcode("0000000101iiiiii1000d000", &[(b'i', 0x3F), (b'd', 1)]),
        0x017F88
    );
}

// === Fieldless variants ===

#[test]
fn test_decode_nop() {
    assert!(matches!(decode(0x000000), Instruction::Nop));
}

#[test]
fn test_decode_rts() {
    assert!(matches!(decode(0x00000C), Instruction::Rts));
}

#[test]
fn test_decode_rti() {
    assert!(matches!(decode(0x000004), Instruction::Rti));
}

#[test]
fn test_decode_reset() {
    assert!(matches!(decode(0x000084), Instruction::Reset));
}

#[test]
fn test_decode_stop() {
    assert!(matches!(decode(0x000087), Instruction::Stop));
}

#[test]
fn test_decode_wait() {
    assert!(matches!(decode(0x000086), Instruction::Wait));
}

#[test]
fn test_decode_illegal() {
    assert!(matches!(decode(0x000005), Instruction::Illegal));
}

#[test]
fn test_decode_enddo() {
    assert!(matches!(decode(0x00008C), Instruction::EndDo));
}

#[test]
fn test_decode_bra_long() {
    assert!(matches!(decode(0x0D10C0), Instruction::BraLong));
}

#[test]
fn test_decode_bsr_long() {
    assert!(matches!(decode(0x0D1080), Instruction::BsrLong));
}

// === Arithmetic immediate ===

#[test]
fn test_decode_add_imm() {
    let opc = make_opcode("0000000101iiiiii1000d000", &[(b'i', 0x2A), (b'd', 1)]);
    match decode(opc) {
        Instruction::AddImm { imm, d } => {
            assert_eq!(imm, 0x2A);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected AddImm, got {other:?}"),
    }
}

#[test]
fn test_decode_add_long() {
    let opc = make_opcode("00000001010000001100d000", &[(b'd', 1)]);
    match decode(opc) {
        Instruction::AddLong { d } => assert_eq!(d, Accumulator::B),
        other => panic!("expected AddLong, got {other:?}"),
    }
}

#[test]
fn test_decode_sub_imm() {
    let opc = make_opcode("0000000101iiiiii1000d100", &[(b'i', 0x15), (b'd', 0)]);
    match decode(opc) {
        Instruction::SubImm { imm, d } => {
            assert_eq!(imm, 0x15);
            assert_eq!(d, Accumulator::A);
        }
        other => panic!("expected SubImm, got {other:?}"),
    }
}

#[test]
fn test_decode_sub_long() {
    let opc = make_opcode("00000001010000001100d100", &[(b'd', 0)]);
    match decode(opc) {
        Instruction::SubLong { d } => assert_eq!(d, Accumulator::A),
        other => panic!("expected SubLong, got {other:?}"),
    }
}

#[test]
fn test_decode_cmp_imm() {
    let opc = make_opcode("0000000101iiiiii1000d101", &[(b'i', 0x07), (b'd', 1)]);
    match decode(opc) {
        Instruction::CmpImm { imm, d } => {
            assert_eq!(imm, 7);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected CmpImm, got {other:?}"),
    }
}

#[test]
fn test_decode_cmp_long() {
    let opc = make_opcode("00000001010000001100d101", &[(b'd', 0)]);
    match decode(opc) {
        Instruction::CmpLong { d } => assert_eq!(d, Accumulator::A),
        other => panic!("expected CmpLong, got {other:?}"),
    }
}

#[test]
fn test_decode_and_imm() {
    let opc = make_opcode("0000000101iiiiii1000d110", &[(b'i', 0x3C), (b'd', 0)]);
    match decode(opc) {
        Instruction::AndImm { imm, d } => {
            assert_eq!(imm, 0x3C);
            assert_eq!(d, Accumulator::A);
        }
        other => panic!("expected AndImm, got {other:?}"),
    }
}

#[test]
fn test_decode_and_long() {
    let opc = make_opcode("00000001010000001100d110", &[(b'd', 1)]);
    match decode(opc) {
        Instruction::AndLong { d } => assert_eq!(d, Accumulator::B),
        other => panic!("expected AndLong, got {other:?}"),
    }
}

#[test]
fn test_decode_or_long() {
    let opc = make_opcode("00000001010000001100d010", &[(b'd', 0)]);
    match decode(opc) {
        Instruction::OrLong { d } => assert_eq!(d, Accumulator::A),
        other => panic!("expected OrLong, got {other:?}"),
    }
}

// === ALU misc ===

#[test]
fn test_decode_andi() {
    let opc = make_opcode("00000000iiiiiiii101110EE", &[(b'i', 0xF0), (b'E', 2)]);
    match decode(opc) {
        Instruction::AndI { imm, dest } => {
            assert_eq!(imm, 0xF0);
            assert_eq!(dest, 2);
        }
        other => panic!("expected AndI, got {other:?}"),
    }
}

#[test]
fn test_decode_ori() {
    let opc = make_opcode("00000000iiiiiiii111110EE", &[(b'i', 0xAB), (b'E', 1)]);
    match decode(opc) {
        Instruction::OrI { imm, dest } => {
            assert_eq!(imm, 0xAB);
            assert_eq!(dest, 1);
        }
        other => panic!("expected OrI, got {other:?}"),
    }
}

#[test]
fn test_decode_cmpu() {
    let opc = make_opcode("00001100000111111111gggd", &[(b'g', 5), (b'd', 1)]);
    match decode(opc) {
        Instruction::CmpU { src, d } => {
            assert_eq!(src, reg::Y0);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected CmpU, got {other:?}"),
    }
}

#[test]
fn test_decode_div() {
    let opc = make_opcode("000000011000000001JJd000", &[(b'J', 2), (b'd', 1)]);
    match decode(opc) {
        Instruction::Div { src, d } => {
            assert_eq!(src, reg::X1);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected Div, got {other:?}"),
    }
}

// === Shifts ===

#[test]
fn test_decode_asl_imm() {
    let opc = make_opcode(
        "0000110000011101SiiiiiiD",
        &[(b'S', 0), (b'i', 12), (b'D', 1)],
    );
    match decode(opc) {
        Instruction::AslImm { shift, s, d } => {
            assert_eq!(shift, 12);
            assert_eq!(s, Accumulator::A);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected AslImm, got {other:?}"),
    }
}

#[test]
fn test_decode_asr_imm() {
    let opc = make_opcode(
        "0000110000011100SiiiiiiD",
        &[(b'S', 1), (b'i', 7), (b'D', 0)],
    );
    match decode(opc) {
        Instruction::AsrImm { shift, s, d } => {
            assert_eq!(shift, 7);
            assert_eq!(s, Accumulator::B);
            assert_eq!(d, Accumulator::A);
        }
        other => panic!("expected AsrImm, got {other:?}"),
    }
}

#[test]
fn test_decode_lsl_imm() {
    let opc = make_opcode("000011000001111010iiiiiD", &[(b'i', 5), (b'D', 1)]);
    match decode(opc) {
        Instruction::LslImm { shift, d } => {
            assert_eq!(shift, 5);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected LslImm, got {other:?}"),
    }
}

// === Dec/Inc ===

#[test]
fn test_decode_dec() {
    let opc = make_opcode("00000000000000000000101d", &[(b'd', 1)]);
    match decode(opc) {
        Instruction::Dec { d } => assert_eq!(d, Accumulator::B),
        other => panic!("expected Dec, got {other:?}"),
    }
}

#[test]
fn test_decode_inc() {
    let opc = make_opcode("00000000000000000000100d", &[(b'd', 0)]);
    match decode(opc) {
        Instruction::Inc { d } => assert_eq!(d, Accumulator::A),
        other => panic!("expected Inc, got {other:?}"),
    }
}

// === Branches (short) ===

#[test]
fn test_decode_bcc() {
    // 9-bit signed address, CC=0x5 (HS)
    let opc = make_opcode(
        "00000101CCCC01aaaa0aaaaa",
        &[(b'C', 5), (b'a', 0b0_0001_0010)],
    );
    match decode(opc) {
        Instruction::Bcc { cc, addr } => {
            assert_eq!(cc, CondCode::from_bits(5));
            assert_eq!(addr, 0x12);
        }
        other => panic!("expected Bcc, got {other:?}"),
    }
}

#[test]
fn test_decode_bcc_negative() {
    // Negative 9-bit address: 0x1FF = -1
    let opc = make_opcode("00000101CCCC01aaaa0aaaaa", &[(b'C', 0), (b'a', 0x1FF)]);
    match decode(opc) {
        Instruction::Bcc { addr, .. } => assert_eq!(addr, -1),
        other => panic!("expected Bcc, got {other:?}"),
    }
}

#[test]
fn test_decode_bra() {
    let opc = make_opcode("00000101000011aaaa0aaaaa", &[(b'a', 0x42)]);
    match decode(opc) {
        Instruction::Bra { addr } => assert_eq!(addr, 0x42),
        other => panic!("expected Bra, got {other:?}"),
    }
}

#[test]
fn test_decode_bsr() {
    let opc = make_opcode("00000101000010aaaa0aaaaa", &[(b'a', 0x10)]);
    match decode(opc) {
        Instruction::Bsr { addr } => assert_eq!(addr, 0x10),
        other => panic!("expected Bsr, got {other:?}"),
    }
}

// === Branches (long) ===

#[test]
fn test_decode_bcc_long() {
    let opc = make_opcode("00001101000100000100CCCC", &[(b'C', 0xA)]);
    match decode(opc) {
        Instruction::BccLong { cc } => assert_eq!(cc, CondCode::from_bits(0xA)),
        other => panic!("expected BccLong, got {other:?}"),
    }
}

// === Jumps ===

#[test]
fn test_decode_jcc() {
    let opc = make_opcode("00001110CCCCaaaaaaaaaaaa", &[(b'C', 3), (b'a', 0x42)]);
    match decode(opc) {
        Instruction::Jcc { cc, addr } => {
            assert_eq!(cc, CondCode::from_bits(3));
            assert_eq!(addr, 0x42);
        }
        other => panic!("expected Jcc, got {other:?}"),
    }
}

#[test]
fn test_decode_jcc_ea() {
    // Use mode 3 (Rn)+, reg 2
    let opc = make_opcode(
        "0000101011MMMRRR1010CCCC",
        &[(b'M', 3), (b'R', 2), (b'C', 7)],
    );
    match decode(opc) {
        Instruction::JccEa { cc, ea_mode } => {
            assert_eq!(cc, CondCode::from_bits(7));
            assert_eq!(ea_mode, 0b011_010);
        }
        other => panic!("expected JccEa, got {other:?}"),
    }
}

#[test]
fn test_decode_jmp() {
    let opc = make_opcode("000011000000aaaaaaaaaaaa", &[(b'a', 0x42)]);
    match decode(opc) {
        Instruction::Jmp { addr } => assert_eq!(addr, 0x42),
        other => panic!("expected Jmp, got {other:?}"),
    }
}

#[test]
fn test_decode_jmp_ea() {
    // mode 4 (Rn), reg 5
    let opc = make_opcode("0000101011MMMRRR10000000", &[(b'M', 4), (b'R', 5)]);
    match decode(opc) {
        Instruction::JmpEa { ea_mode } => assert_eq!(ea_mode, 0b100_101),
        other => panic!("expected JmpEa, got {other:?}"),
    }
}

#[test]
fn test_decode_jscc() {
    let opc = make_opcode("00001111CCCCaaaaaaaaaaaa", &[(b'C', 2), (b'a', 0x100)]);
    match decode(opc) {
        Instruction::Jscc { cc, addr } => {
            assert_eq!(cc, CondCode::from_bits(2));
            assert_eq!(addr, 0x100);
        }
        other => panic!("expected Jscc, got {other:?}"),
    }
}

#[test]
fn test_decode_jscc_ea() {
    let opc = make_opcode(
        "0000101111MMMRRR1010CCCC",
        &[(b'M', 1), (b'R', 3), (b'C', 0xF)],
    );
    match decode(opc) {
        Instruction::JsccEa { cc, ea_mode } => {
            assert_eq!(cc, CondCode::from_bits(0xF));
            assert_eq!(ea_mode, 0b001_011);
        }
        other => panic!("expected JsccEa, got {other:?}"),
    }
}

#[test]
fn test_decode_jsr() {
    let opc = make_opcode("000011010000aaaaaaaaaaaa", &[(b'a', 0x200)]);
    match decode(opc) {
        Instruction::Jsr { addr } => assert_eq!(addr, 0x200),
        other => panic!("expected Jsr, got {other:?}"),
    }
}

#[test]
fn test_decode_jsr_ea() {
    let opc = make_opcode("0000101111MMMRRR10000000", &[(b'M', 5), (b'R', 1)]);
    match decode(opc) {
        Instruction::JsrEa { ea_mode } => assert_eq!(ea_mode, 0b101_001),
        other => panic!("expected JsrEa, got {other:?}"),
    }
}

// === Bit manipulation (BCHG/BCLR/BSET/BTST x ea/aa/pp/reg) ===

macro_rules! bit_test {
    ($name:ident, $tmpl:expr, ea, $Var:ident, $M:expr, $R:expr, $S:expr, $b:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'M', $M), (b'R', $R), (b'S', $S), (b'b', $b)]);
            match decode(opc) {
                Instruction::$Var {
                    space,
                    ea_mode,
                    bit_num,
                } => {
                    assert_eq!(space, if $S == 0 { MemSpace::X } else { MemSpace::Y });
                    assert_eq!(ea_mode, ($M << 3) | $R);
                    assert_eq!(bit_num, $b);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, aa, $Var:ident, $a:expr, $S:expr, $b:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'a', $a), (b'S', $S), (b'b', $b)]);
            match decode(opc) {
                Instruction::$Var {
                    space,
                    addr,
                    bit_num,
                } => {
                    assert_eq!(space, if $S == 0 { MemSpace::X } else { MemSpace::Y });
                    assert_eq!(addr, $a);
                    assert_eq!(bit_num, $b);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, pp, $Var:ident, $p:expr, $S:expr, $b:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'p', $p), (b'S', $S), (b'b', $b)]);
            match decode(opc) {
                Instruction::$Var {
                    space,
                    pp_offset,
                    bit_num,
                } => {
                    assert_eq!(space, if $S == 0 { MemSpace::X } else { MemSpace::Y });
                    assert_eq!(pp_offset, $p);
                    assert_eq!(bit_num, $b);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, qq, $Var:ident, $q:expr, $S:expr, $b:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'q', $q), (b'S', $S), (b'b', $b)]);
            match decode(opc) {
                Instruction::$Var {
                    space,
                    qq_offset,
                    bit_num,
                } => {
                    assert_eq!(space, if $S == 0 { MemSpace::X } else { MemSpace::Y });
                    assert_eq!(qq_offset, $q);
                    assert_eq!(bit_num, $b);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, reg, $Var:ident, $D:expr, $b:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'D', $D), (b'b', $b)]);
            match decode(opc) {
                Instruction::$Var { reg_idx, bit_num } => {
                    assert_eq!(reg_idx, $D);
                    assert_eq!(bit_num, $b);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
}

// BCHG
bit_test!(
    test_decode_bchg_ea,
    "0000101101MMMRRR0S00bbbb",
    ea,
    BchgEa,
    3,
    2,
    1,
    0xA
);
bit_test!(
    test_decode_bchg_aa,
    "0000101100aaaaaa0S00bbbb",
    aa,
    BchgAa,
    0x15,
    0,
    5
);
bit_test!(
    test_decode_bchg_pp,
    "0000101110pppppp0S00bbbb",
    pp,
    BchgPp,
    0x2A,
    1,
    7
);
bit_test!(
    test_decode_bchg_qq,
    "0000000101qqqqqq0S0bbbbb",
    qq,
    BchgQq,
    0x1A,
    1,
    0x13
);
bit_test!(
    test_decode_bchg_reg,
    "0000101111DDDDDD010bbbbb",
    reg,
    BchgReg,
    10,
    0x13
);

// BCLR
bit_test!(
    test_decode_bclr_ea,
    "0000101001MMMRRR0S00bbbb",
    ea,
    BclrEa,
    1,
    5,
    0,
    0xC
);
bit_test!(
    test_decode_bclr_aa,
    "0000101000aaaaaa0S00bbbb",
    aa,
    BclrAa,
    0x3F,
    1,
    0xF
);
bit_test!(
    test_decode_bclr_pp,
    "0000101010pppppp0S00bbbb",
    pp,
    BclrPp,
    0x10,
    0,
    3
);
bit_test!(
    test_decode_bclr_qq,
    "0000000100qqqqqq0S00bbbb",
    qq,
    BclrQq,
    0x2B,
    0,
    0xA
);
bit_test!(
    test_decode_bclr_reg,
    "0000101011DDDDDD010bbbbb",
    reg,
    BclrReg,
    0x1F,
    0x17
);

// BSET
bit_test!(
    test_decode_bset_ea,
    "0000101001MMMRRR0S1bbbbb",
    ea,
    BsetEa,
    2,
    4,
    1,
    0x11
);
bit_test!(
    test_decode_bset_aa,
    "0000101000aaaaaa0S1bbbbb",
    aa,
    BsetAa,
    0x0A,
    0,
    0x1D
);
bit_test!(
    test_decode_bset_pp,
    "0000101010pppppp0S1bbbbb",
    pp,
    BsetPp,
    0x33,
    1,
    0x0E
);
bit_test!(
    test_decode_bset_qq,
    "0000000100qqqqqq0S1bbbbb",
    qq,
    BsetQq,
    0x0F,
    1,
    0x1C
);
bit_test!(
    test_decode_bset_reg,
    "0000101011DDDDDD011bbbbb",
    reg,
    BsetReg,
    0x05,
    0x0A
);

// BTST
bit_test!(
    test_decode_btst_ea,
    "0000101101MMMRRR0S10bbbb",
    ea,
    BtstEa,
    4,
    0,
    0,
    9
);
bit_test!(
    test_decode_btst_aa,
    "0000101100aaaaaa0S10bbbb",
    aa,
    BtstAa,
    0x22,
    1,
    0xB
);
bit_test!(
    test_decode_btst_pp,
    "0000101110pppppp0S10bbbb",
    pp,
    BtstPp,
    0x08,
    0,
    0xD
);
bit_test!(
    test_decode_btst_qq,
    "0000000101qqqqqq0S10bbbb",
    qq,
    BtstQq,
    0x33,
    0,
    0x9
);
bit_test!(
    test_decode_btst_reg,
    "0000101111DDDDDD0110bbbb",
    reg,
    BtstReg,
    0x3A,
    0x0F
);

// === Bit test & jump (JCLR/JSET/JSCLR/JSSET x ea/aa/pp/reg) ===

// JCLR
bit_test!(
    test_decode_jclr_ea,
    "0000101001MMMRRR1S00bbbb",
    ea,
    JclrEa,
    3,
    1,
    0,
    6
);
bit_test!(
    test_decode_jclr_aa,
    "0000101000aaaaaa1S00bbbb",
    aa,
    JclrAa,
    0x2D,
    1,
    0xE
);
bit_test!(
    test_decode_jclr_pp,
    "0000101010pppppp1S00bbbb",
    pp,
    JclrPp,
    0x3F,
    0,
    8
);
bit_test!(
    test_decode_jclr_qq,
    "0000000110qqqqqq1S00bbbb",
    qq,
    JclrQq,
    0x12,
    1,
    0x7
);
bit_test!(
    test_decode_jclr_reg,
    "0000101011DDDDDD0000bbbb",
    reg,
    JclrReg,
    0x04,
    0x0C
);

// JSET
bit_test!(
    test_decode_jset_ea,
    "0000101001MMMRRR1S10bbbb",
    ea,
    JsetEa,
    0,
    7,
    1,
    0xA
);
bit_test!(
    test_decode_jset_aa,
    "0000101000aaaaaa1S10bbbb",
    aa,
    JsetAa,
    0x11,
    0,
    3
);
bit_test!(
    test_decode_jset_pp,
    "0000101010pppppp1S10bbbb",
    pp,
    JsetPp,
    0x20,
    1,
    0xF
);
bit_test!(
    test_decode_jset_qq,
    "0000000110qqqqqq1S10bbbb",
    qq,
    JsetQq,
    0x25,
    0,
    0xB
);
bit_test!(
    test_decode_jset_reg,
    "0000101011DDDDDD0010bbbb",
    reg,
    JsetReg,
    0x39, // SR
    7
);

// JSCLR
bit_test!(
    test_decode_jsclr_ea,
    "0000101101MMMRRR1S00bbbb",
    ea,
    JsclrEa,
    5,
    3,
    1,
    2
);
bit_test!(
    test_decode_jsclr_aa,
    "0000101100aaaaaa1S00bbbb",
    aa,
    JsclrAa,
    0x07,
    0,
    0xD
);
bit_test!(
    test_decode_jsclr_pp,
    "0000101110pppppp1S0bbbbb",
    pp,
    JsclrPp,
    0x15,
    1,
    0x1A
);
bit_test!(
    test_decode_jsclr_qq,
    "0000000111qqqqqq1S0bbbbb",
    qq,
    JsclrQq,
    0x08,
    0,
    0x1E
);
bit_test!(
    test_decode_jsclr_reg,
    "0000101111DDDDDD000bbbbb",
    reg,
    JsclrReg,
    0x22,
    0x0B
);

// JSSET
bit_test!(
    test_decode_jsset_ea,
    "0000101101MMMRRR1S10bbbb",
    ea,
    JssetEa,
    2,
    6,
    0,
    4
);
bit_test!(
    test_decode_jsset_aa,
    "0000101100aaaaaa1S10bbbb",
    aa,
    JssetAa,
    0x19,
    1,
    1
);
bit_test!(
    test_decode_jsset_pp,
    "0000101110pppppp1S1bbbbb",
    pp,
    JssetPp,
    0x0C,
    0,
    0x15
);
bit_test!(
    test_decode_jsset_qq,
    "0000000111qqqqqq1S1bbbbb",
    qq,
    JssetQq,
    0x3E,
    1,
    0x10
);
bit_test!(
    test_decode_jsset_reg,
    "0000101111DDDDDD001bbbbb",
    reg,
    JssetReg,
    0x39, // SR
    0x1F
);

// === Bit test & branch relative (BRCLR/BRSET/BSCLR/BSSET) ===

bit_test!(
    test_decode_brclr_pp,
    "0000110011pppppp0S0bbbbb",
    pp,
    BrclrPp,
    0x2F,
    1,
    0x11
);
bit_test!(
    test_decode_brclr_reg,
    "0000110011DDDDDD100bbbbb",
    reg,
    BrclrReg,
    0x0E,
    0x1C
);
bit_test!(
    test_decode_brset_pp,
    "0000110011pppppp0S1bbbbb",
    pp,
    BrsetPp,
    0x3A,
    0,
    0x07
);
bit_test!(
    test_decode_brclr_qq,
    "0000010010qqqqqq0S0bbbbb",
    qq,
    BrclrQq,
    0x1D,
    0,
    0x0E
);
bit_test!(
    test_decode_brset_qq,
    "0000010010qqqqqq0S1bbbbb",
    qq,
    BrsetQq,
    0x06,
    1,
    0x14
);
bit_test!(
    test_decode_brset_reg,
    "0000110011DDDDDD101bbbbb",
    reg,
    BrsetReg,
    0x12,
    0x19
);
bit_test!(
    test_decode_bsclr_qq,
    "0000010010qqqqqq1S0bbbbb",
    qq,
    BsclrQq,
    0x29,
    1,
    0x03
);
bit_test!(
    test_decode_bsset_qq,
    "0000010010qqqqqq1S1bbbbb",
    qq,
    BssetQq,
    0x14,
    0,
    0x1A
);

// === Loops (DO/DOR/REP) ===

macro_rules! loop_test {
    ($name:ident, $tmpl:expr, ea, $Var:ident, $M:expr, $R:expr, $S:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'M', $M), (b'R', $R), (b'S', $S)]);
            match decode(opc) {
                Instruction::$Var { space, ea_mode } => {
                    assert_eq!(space, if $S == 0 { MemSpace::X } else { MemSpace::Y });
                    assert_eq!(ea_mode, ($M << 3) | $R);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, aa, $Var:ident, $a:expr, $S:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'a', $a), (b'S', $S)]);
            match decode(opc) {
                Instruction::$Var { space, addr } => {
                    assert_eq!(space, if $S == 0 { MemSpace::X } else { MemSpace::Y });
                    assert_eq!(addr, $a);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, imm, $Var:ident, $i:expr, $h:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[(b'i', $i), (b'h', $h)]);
            match decode(opc) {
                Instruction::$Var { count } => {
                    assert_eq!(count, ($h << 8) | $i);
                }
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
    ($name:ident, $tmpl:expr, reg, $Var:ident, $letter:expr, $val:expr) => {
        #[test]
        fn $name() {
            let opc = make_opcode($tmpl, &[($letter, $val)]);
            match decode(opc) {
                Instruction::$Var { reg_idx } => assert_eq!(reg_idx, $val as u8),
                other => panic!("expected {}, got {other:?}", stringify!($Var)),
            }
        }
    };
}

// DO
loop_test!(
    test_decode_do_ea,
    "0000011001MMMRRR0S000000",
    ea,
    DoEa,
    3,
    1,
    1
);
loop_test!(
    test_decode_do_aa,
    "0000011000aaaaaa0S000000",
    aa,
    DoAa,
    0x15,
    0
);
loop_test!(
    test_decode_do_imm,
    "00000110iiiiiiii1000hhhh",
    imm,
    DoImm,
    0xAB,
    0x5
);
loop_test!(
    test_decode_do_reg,
    "0000011011DDDDDD00000000",
    reg,
    DoReg,
    b'D',
    0x0A
);

// DOR
loop_test!(
    test_decode_dor_ea,
    "0000011001MMMRRR0S010000",
    ea,
    DorEa,
    2,
    5,
    0
);
loop_test!(
    test_decode_dor_aa,
    "0000011000aaaaaa0S010000",
    aa,
    DorAa,
    0x3F,
    1
);
loop_test!(
    test_decode_dor_imm,
    "00000110iiiiiiii1001hhhh",
    imm,
    DorImm,
    0x42,
    0xC
);
loop_test!(
    test_decode_dor_reg,
    "0000011011DDDDDD00010000",
    reg,
    DorReg,
    b'D',
    0x1E
);

// REP
loop_test!(
    test_decode_rep_ea,
    "0000011001MMMRRR0S100000",
    ea,
    RepEa,
    4,
    0,
    1
);
loop_test!(
    test_decode_rep_aa,
    "0000011000aaaaaa0S100000",
    aa,
    RepAa,
    0x08,
    0
);
loop_test!(
    test_decode_rep_imm,
    "00000110iiiiiiii1010hhhh",
    imm,
    RepImm,
    0x20,
    0x0
);
loop_test!(
    test_decode_rep_reg,
    "0000011011dddddd00100000",
    reg,
    RepReg,
    b'd',
    0x12
);

// === Move variants ===

#[test]
fn test_decode_move_long() {
    // X space
    let opc = make_opcode(
        "0000101s01110RRR1WDDDDDD",
        &[(b's', 0), (b'R', 3), (b'W', 1), (b'D', 0x0A)],
    );
    match decode(opc) {
        Instruction::MoveLongDisp {
            space,
            w,
            offreg_idx,
            numreg,
        } => {
            assert_eq!(space, MemSpace::X);
            assert!(w);
            assert_eq!(offreg_idx, 3);
            assert_eq!(numreg, 0x0A);
        }
        other => panic!("expected MoveLongDisp X, got {other:?}"),
    }
    // Y space
    let opc = make_opcode(
        "0000101s01110RRR1WDDDDDD",
        &[(b's', 1), (b'R', 5), (b'W', 0), (b'D', 0x0C)],
    );
    match decode(opc) {
        Instruction::MoveLongDisp {
            space,
            w,
            offreg_idx,
            numreg,
        } => {
            assert_eq!(space, MemSpace::Y);
            assert!(!w);
            assert_eq!(offreg_idx, 5);
            assert_eq!(numreg, 0x0C);
        }
        other => panic!("expected MoveLongDisp Y, got {other:?}"),
    }
}

#[test]
fn test_decode_move_imm() {
    // X space
    let opc = make_opcode(
        "0000001aaaaaaRRR1asWDDDD",
        &[(b'a', 0x45), (b's', 0), (b'R', 2), (b'W', 0), (b'D', 0xB)],
    );
    match decode(opc) {
        Instruction::MoveShortDisp {
            space,
            offset,
            w,
            offreg_idx,
            numreg,
        } => {
            assert_eq!(space, MemSpace::X);
            assert_eq!(offset, 0x45);
            assert!(!w);
            assert_eq!(offreg_idx, 2);
            assert_eq!(numreg, 0xB);
        }
        other => panic!("expected MoveShortDisp X, got {other:?}"),
    }
    // Y space
    let opc = make_opcode(
        "0000001aaaaaaRRR1asWDDDD",
        &[(b'a', 0x12), (b's', 1), (b'R', 5), (b'W', 1), (b'D', 4)],
    );
    match decode(opc) {
        Instruction::MoveShortDisp {
            space,
            offset,
            w,
            offreg_idx,
            numreg,
        } => {
            assert_eq!(space, MemSpace::Y);
            assert_eq!(offset, 0x12);
            assert!(w);
            assert_eq!(offreg_idx, 5);
            assert_eq!(numreg, 4);
        }
        other => panic!("expected MoveShortDisp Y, got {other:?}"),
    }
}

#[test]
fn test_decode_movec_ea() {
    // mode 3 (Rn)+, reg 1, numreg via low 6 bits
    let opc = make_opcode(
        "00000101W1MMMRRR0s1ddddd",
        &[(b'W', 0), (b'M', 3), (b'R', 1), (b's', 1), (b'd', 0x04)], // d=4 = numreg=m4
    );
    match decode(opc) {
        Instruction::MovecEa {
            ea_mode,
            numreg,
            w,
            space,
        } => {
            assert_eq!(ea_mode, 0b011_001);
            assert!(!w);
            assert_eq!(space, MemSpace::Y);
            // numreg is (opc & 0x3F) -- the 's' and 'd' bits contribute
            assert_eq!(numreg, opc as u8 & 0x3F);
        }
        other => panic!("expected MovecEa, got {other:?}"),
    }
}

#[test]
fn test_decode_movec_aa() {
    let opc = make_opcode(
        "00000101W0aaaaaa0s1ddddd",
        &[(b'W', 1), (b'a', 0x2A), (b's', 0), (b'd', 0x04)], // d=4 = numreg=m4
    );
    match decode(opc) {
        Instruction::MovecAa {
            addr,
            numreg,
            w,
            space,
        } => {
            assert_eq!(addr, 0x2A);
            assert!(w);
            assert_eq!(space, MemSpace::X);
            assert_eq!(numreg, opc as u8 & 0x3F);
        }
        other => panic!("expected MovecAa, got {other:?}"),
    }
}

#[test]
fn test_decode_movec_reg() {
    // Use d=4 so dst_reg = 0b10_0100 = 36 (ssh), which is a valid register.
    let opc = make_opcode(
        "00000100W1eeeeee101ddddd",
        &[(b'W', 0), (b'e', 0x15), (b'd', 0x04)],
    );
    match decode(opc) {
        Instruction::MovecReg {
            src_reg,
            dst_reg,
            w,
        } => {
            assert_eq!(src_reg, 0x15);
            assert!(!w);
            assert_eq!(dst_reg, opc as u8 & 0x3F);
        }
        other => panic!("expected MovecReg, got {other:?}"),
    }
}

#[test]
fn test_decode_movec_imm() {
    let opc = make_opcode("00000101iiiiiiii101ddddd", &[(b'i', 0xCD), (b'd', 0x04)]); // d=4 = dest=m4
    match decode(opc) {
        Instruction::MovecImm { imm, dest } => {
            assert_eq!(imm, 0xCD);
            assert_eq!(dest, opc as u8 & 0x3F);
        }
        other => panic!("expected MovecImm, got {other:?}"),
    }
}

#[test]
fn test_decode_movem_ea() {
    let opc = make_opcode(
        "00000111W1MMMRRR10dddddd",
        &[(b'W', 1), (b'M', 2), (b'R', 6), (b'd', 0x15)],
    );
    match decode(opc) {
        Instruction::MovemEa { ea_mode, numreg, w } => {
            assert_eq!(ea_mode, 0b010_110);
            assert_eq!(numreg, 0x15);
            assert!(w);
        }
        other => panic!("expected MovemEa, got {other:?}"),
    }
}

#[test]
fn test_decode_movem_aa() {
    let opc = make_opcode(
        "00000111W0aaaaaa00dddddd",
        &[(b'W', 0), (b'a', 0x33), (b'd', 0x0C)],
    );
    match decode(opc) {
        Instruction::MovemAa { addr, numreg, w } => {
            assert_eq!(addr, 0x33);
            assert_eq!(numreg, 0x0C);
            assert!(!w);
        }
        other => panic!("expected MovemAa, got {other:?}"),
    }
}

#[test]
fn test_decode_movep23() {
    let opc = make_opcode(
        "0000100sW1MMMRRR1Spppppp",
        &[
            (b's', 1),
            (b'W', 0),
            (b'M', 3),
            (b'R', 4),
            (b'S', 0),
            (b'p', 0x2A),
        ],
    );
    match decode(opc) {
        Instruction::Movep23 {
            pp_offset,
            ea_mode,
            w,
            perspace,
            easpace,
        } => {
            assert_eq!(pp_offset, 0x2A);
            assert_eq!(ea_mode, 0b011_100);
            assert!(!w);
            assert_eq!(perspace, MemSpace::Y);
            assert_eq!(easpace, MemSpace::X);
        }
        other => panic!("expected Movep23, got {other:?}"),
    }
}

#[test]
fn test_decode_movep_xqq() {
    let opc = make_opcode(
        "00000111W1MMMRRR0Sqqqqqq",
        &[(b'W', 1), (b'M', 1), (b'R', 2), (b'S', 1), (b'q', 0x15)],
    );
    match decode(opc) {
        Instruction::MovepQq {
            qq_offset,
            ea_mode,
            w,
            qqspace,
            easpace,
        } => {
            assert_eq!(qq_offset, 0x15);
            assert_eq!(ea_mode, 0b001_010);
            assert!(w);
            assert_eq!(qqspace, MemSpace::X);
            assert_eq!(easpace, MemSpace::Y);
        }
        other => panic!("expected MovepQq, got {other:?}"),
    }
}

#[test]
fn test_decode_movep1() {
    let opc = make_opcode(
        "0000100sW1MMMRRR01pppppp",
        &[(b's', 0), (b'W', 1), (b'M', 4), (b'R', 0), (b'p', 0x1F)],
    );
    match decode(opc) {
        Instruction::Movep1 {
            pp_offset,
            ea_mode,
            w,
            space,
        } => {
            assert_eq!(pp_offset, 0x1F);
            assert_eq!(ea_mode, 0b100_000);
            assert!(w);
            assert_eq!(space, MemSpace::X);
        }
        other => panic!("expected Movep1, got {other:?}"),
    }
}

#[test]
fn test_decode_movep0() {
    let opc = make_opcode(
        "0000100sW1dddddd00pppppp",
        &[(b's', 1), (b'W', 0), (b'd', 0x15), (b'p', 0x3F)],
    );
    match decode(opc) {
        Instruction::Movep0 {
            pp_offset,
            reg_idx,
            w,
            space,
        } => {
            assert_eq!(pp_offset, 0x3F);
            assert_eq!(reg_idx, 0x15);
            assert!(!w);
            assert_eq!(space, MemSpace::Y);
        }
        other => panic!("expected Movep0, got {other:?}"),
    }
}

// === Remaining variants ===

#[test]
fn test_decode_mpyi() {
    let opc = make_opcode(
        "000000010100000111qqdk00",
        &[(b'q', 2), (b'd', 1), (b'k', 1)],
    );
    match decode(opc) {
        Instruction::MpyI { k, d, src } => {
            assert!(k);
            assert_eq!(d, Accumulator::B);
            assert_eq!(src, reg::X1);
        }
        other => panic!("expected MpyI, got {other:?}"),
    }
}

#[test]
fn test_decode_lua() {
    // mode 1 (Rn)+Nn, reg 3, dst_reg 10
    let opc = make_opcode(
        "00000100010MMRRR000ddddd",
        &[(b'M', 1), (b'R', 3), (b'd', 0x0A)],
    );
    match decode(opc) {
        Instruction::Lua { ea_mode, dst_reg } => {
            assert_eq!(ea_mode, 0b01_011);
            assert_eq!(dst_reg, 0x0A);
        }
        other => panic!("expected Lua, got {other:?}"),
    }
}

#[test]
fn test_decode_lua_rel() {
    // lua (R2 + aa), N5 -> dest_is_n=true, dst_reg low 3 bits = 5
    let opc = make_opcode(
        "0000010000aaaRRRaaaadddd",
        &[(b'a', 0x15), (b'R', 2), (b'd', 0xD)],
    );
    match decode(opc) {
        Instruction::LuaRel {
            aa,
            addr_reg,
            dst_reg,
            dest_is_n,
        } => {
            assert_eq!(aa, 0x15);
            assert_eq!(addr_reg, 2);
            assert_eq!(dst_reg, (0xD & 0x7) as u8);
            assert_eq!(dest_is_n, (0xD >> 3) & 1 != 0);
        }
        other => panic!("expected LuaRel, got {other:?}"),
    }
}

#[test]
fn test_decode_norm() {
    let opc = make_opcode("0000000111011RRR0001d101", &[(b'R', 5), (b'd', 1)]);
    match decode(opc) {
        Instruction::Norm { rreg_idx, d } => {
            assert_eq!(rreg_idx, 5);
            assert_eq!(d, Accumulator::B);
        }
        other => panic!("expected Norm, got {other:?}"),
    }
}

#[test]
fn test_decode_tcc_s1_d1() {
    // Template 1: tcc S1,D1 (acc only, bit16=0, bit11=0)
    // J=5, d=1 -> tcc_idx = (5<<1)|1 = 11 -> REGISTERS_TCC[11] = (Y0, B)
    let opc = make_opcode(
        "00000010CCCC00000JJJd000",
        &[(b'C', 8), (b'J', 5), (b'd', 1)],
    );
    match decode(opc) {
        Instruction::Tcc { cc, acc, r } => {
            assert_eq!(cc, CondCode::from_bits(8));
            assert_eq!(acc, Some((dsp56300_core::reg::Y0, dsp56300_core::reg::B)));
            assert_eq!(r, None);
        }
        other => panic!("expected Tcc, got {other:?}"),
    }
}

#[test]
fn test_decode_tcc_with_r() {
    // Template 2: tcc S1,D1 S2,D2 (acc + R reg, bit16=1)
    // Use J=4 (x0->a, tcc_idx=8) which is a valid register pair.
    let opc = make_opcode(
        "00000011CCCC0ttt0JJJdTTT",
        &[(b'C', 0), (b't', 3), (b'J', 4), (b'd', 0), (b'T', 5)],
    );
    match decode(opc) {
        Instruction::Tcc { cc, acc, r } => {
            assert_eq!(cc, CondCode::from_bits(0));
            assert_eq!(acc, Some((dsp56300_core::reg::X0, dsp56300_core::reg::A)));
            assert_eq!(r, Some((3, 5)));
        }
        other => panic!("expected Tcc, got {other:?}"),
    }
}

#[test]
fn test_decode_tcc_r_only() {
    // Template 3: tcc S2,D2 (R reg only, bit16=0, bit11=1)
    let opc = make_opcode(
        "00000010CCCC1ttt00000TTT",
        &[(b'C', 7), (b't', 2), (b'T', 6)],
    );
    match decode(opc) {
        Instruction::Tcc { cc, acc, r } => {
            assert_eq!(cc, CondCode::from_bits(7));
            assert_eq!(acc, None);
            assert_eq!(r, Some((2, 6)));
        }
        other => panic!("expected Tcc, got {other:?}"),
    }
}

// === MulShift (mpy/mpyr/mac/macr S,#n,D) ===

#[test]
fn test_decode_mul_shift_mpy() {
    // mpy +y1,#0,a: template 00000001000sssss11QQdk00
    // sssss=0, QQ=0 (Y1), d=0 (A), k=0 (+)
    let opc = make_opcode(
        "00000001000sssss11QQdk00",
        &[(b's', 0), (b'Q', 0), (b'd', 0), (b'k', 0)],
    );
    match decode(opc) {
        Instruction::MulShift {
            op,
            shift,
            src,
            d,
            k,
        } => {
            assert_eq!(op, MulShiftOp::Mpy);
            assert_eq!(shift, 0);
            assert_eq!(src, reg::Y1);
            assert_eq!(d, Accumulator::A);
            assert!(!k);
        }
        other => panic!("expected MulShift, got {other:?}"),
    }
}

#[test]
fn test_decode_mul_shift_mac() {
    // mac -x1,#17,b: sssss=17, QQ=3 (X1), d=1 (B), k=1 (-)
    let opc = make_opcode(
        "00000001000sssss11QQdk10",
        &[(b's', 17), (b'Q', 3), (b'd', 1), (b'k', 1)],
    );
    match decode(opc) {
        Instruction::MulShift {
            op,
            shift,
            src,
            d,
            k,
        } => {
            assert_eq!(op, MulShiftOp::Mac);
            assert_eq!(shift, 17);
            assert_eq!(src, reg::X1);
            assert_eq!(d, Accumulator::B);
            assert!(k);
        }
        other => panic!("expected MulShift, got {other:?}"),
    }
}

#[test]
fn test_decode_mul_shift_mpyr() {
    // mpyr +y0,#31,a: sssss=31, QQ=2 (Y0), d=0 (A), k=0
    let opc = make_opcode(
        "00000001000sssss11QQdk01",
        &[(b's', 31), (b'Q', 2), (b'd', 0), (b'k', 0)],
    );
    match decode(opc) {
        Instruction::MulShift {
            op,
            shift,
            src,
            d,
            k,
        } => {
            assert_eq!(op, MulShiftOp::Mpyr);
            assert_eq!(shift, 31);
            assert_eq!(src, reg::Y0);
            assert_eq!(d, Accumulator::A);
            assert!(!k);
        }
        other => panic!("expected MulShift, got {other:?}"),
    }
}

#[test]
fn test_decode_mul_shift_macr() {
    // macr -x0,#8,b: sssss=8, QQ=1 (X0), d=1, k=1
    let opc = make_opcode(
        "00000001000sssss11QQdk11",
        &[(b's', 8), (b'Q', 1), (b'd', 1), (b'k', 1)],
    );
    match decode(opc) {
        Instruction::MulShift {
            op,
            shift,
            src,
            d,
            k,
        } => {
            assert_eq!(op, MulShiftOp::Macr);
            assert_eq!(shift, 8);
            assert_eq!(src, reg::X0);
            assert_eq!(d, Accumulator::B);
            assert!(k);
        }
        other => panic!("expected MulShift, got {other:?}"),
    }
}

#[test]
fn test_decode_mul_shift_all_qq_values() {
    // Verify all 4 QQ register mappings decode correctly
    let expected = [reg::Y1, reg::X0, reg::Y0, reg::X1];
    for qq_val in 0..4u32 {
        let opc = make_opcode(
            "00000001000sssss11QQdk00",
            &[(b's', 5), (b'Q', qq_val), (b'd', 0), (b'k', 0)],
        );
        match decode(opc) {
            Instruction::MulShift { src, shift, .. } => {
                assert_eq!(src, expected[qq_val as usize], "QQ={qq_val}");
                assert_eq!(shift, 5);
            }
            other => panic!("expected MulShift for QQ={qq_val}, got {other:?}"),
        }
    }
}

#[test]
fn test_decode_mul_shift_is_single_word() {
    // MulShift instructions are single-word
    let opc = make_opcode(
        "00000001000sssss11QQdk00",
        &[(b's', 10), (b'Q', 0), (b'd', 0), (b'k', 0)],
    );
    assert_eq!(instruction_length(&decode(opc)), 1);
}

// === Parallel ===

#[test]
fn test_decode_parallel() {
    // opcode >= 0x100000: bits 23:20 select move type, 7:0 select ALU op
    let opc = 0x2A00C0; // bits 23:20 = 2 -> Pm2, ALU = 0xC0
    match decode(opc) {
        Instruction::Parallel {
            opcode,
            alu,
            move_type,
        } => {
            assert_eq!(opcode, opc);
            assert_eq!(alu.encode(), Some(0xC0));
            assert_eq!(move_type, ParallelMoveType::Pm2);
        }
        other => panic!("expected Parallel, got {other:?}"),
    }
}

#[test]
fn test_decode_parallel_pm0() {
    // PM0: bits 23:20 in {0, 4} with bit17=0 -> Pm0
    let opc = 0x080080; // bits 23:20 = 0, bit17 = 0
    match decode(opc) {
        Instruction::Parallel { move_type, .. } => {
            assert_eq!(move_type, ParallelMoveType::Pm0);
        }
        other => panic!("expected Parallel(Pm0), got {other:?}"),
    }
}

// === Unknown ===

#[test]
fn test_decode_unknown() {
    // 0x000003 is pflush
    assert!(matches!(decode(0x000003), Instruction::Pflush));
    // An opcode in the low range that matches nothing
    assert!(matches!(decode(0x000060), Instruction::Unknown { .. }));
}

// === Instruction length ===

#[test]
fn test_instruction_length() {
    // 1-word instructions
    assert_eq!(instruction_length(&decode(0x000000)), 1); // nop
    assert_eq!(instruction_length(&decode(0x00000C)), 1); // rts
    assert_eq!(instruction_length(&decode(0x0C0042)), 1); // jmp xxx
    assert_eq!(instruction_length(&decode(0x200080)), 1); // parallel mpy

    // 2-word: long branch/subroutine
    assert_eq!(instruction_length(&decode(0x0D1040)), 2); // bcc xxxx
    assert_eq!(instruction_length(&decode(0x0D10C0)), 2); // bra xxxx
    assert_eq!(instruction_length(&decode(0x0D1080)), 2); // bsr xxxx

    // 2-word: ALU long
    assert_eq!(instruction_length(&decode(0x0140C0)), 2); // add #xxxx,A
    assert_eq!(instruction_length(&decode(0x0140C4)), 2); // sub #xxxx,A
    assert_eq!(instruction_length(&decode(0x0140C6)), 2); // and #xxxx,A
    assert_eq!(instruction_length(&decode(0x0140C2)), 2); // or #xxxx,A
    assert_eq!(instruction_length(&decode(0x0140C5)), 2); // cmp #xxxx,A

    // 2-word: DO/DOR
    assert_eq!(instruction_length(&decode(0x060180)), 2); // do #xxx, expr
    assert_eq!(instruction_length(&decode(0x060190)), 2); // dor #xxx, label

    // 2-word: bit-test-and-jump (jclr/jset)
    assert_eq!(instruction_length(&decode(0x0A4080)), 2); // jclr ea
    assert_eq!(instruction_length(&decode(0x0A40A0)), 2); // jset ea

    // 2-word: bit-test-and-branch-relative (brclr/brset pp)
    assert_eq!(instruction_length(&decode(0x0CC000)), 2); // brclr pp
    assert_eq!(instruction_length(&decode(0x0CC020)), 2); // brset pp

    // 2-word: move X long
    assert_eq!(instruction_length(&decode(0x0A7080)), 2); // move X:(Rn + xxxx)

    // 2-word: mpyi
    assert_eq!(instruction_length(&decode(0x0141C0)), 2); // mpyi #xxxx,S,D

    // Conditional 2-word: jmp ea with mode 6 (absolute) vs mode 0
    assert_eq!(instruction_length(&decode(0x0AF080)), 2); // jmp ea, mode 6
    assert_eq!(instruction_length(&decode(0x0AC080)), 1); // jmp ea, mode 0

    // Pm1 with mode 6 (immediate/absolute from extension word)
    assert_eq!(instruction_length(&decode(0x12B400)), 2); // move #imm,x0 b,y0
    assert_eq!(instruction_length(&decode(0x15B400)), 2); // move #imm,x1 a,y1
    assert_eq!(instruction_length(&decode(0x10B000)), 2); // move x:abs,x0 a,y0
    assert_eq!(instruction_length(&decode(0x1A3000)), 2); // move a,x:abs b,y0
    // Pm1 without mode 6 should still be 1 word
    assert_eq!(instruction_length(&decode(0x10A800)), 1); // Pm1 with mode 5

    // Pm0 with mode 6 (absolute from extension word)
    assert_eq!(instruction_length(&decode(0x083000)), 2); // move a,x:abs x0,a
    // Pm0 without mode 6 should still be 1 word
    assert_eq!(instruction_length(&decode(0x081800)), 1); // Pm0 with mode 3
}
