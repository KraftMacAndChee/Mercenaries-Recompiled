// Integration tests targeting uncovered paths in core/src/lib.rs.

use dsp56300_core::{Accumulator, ParallelAlu, qqq_reg, qqqq_regs, reg, sss_reg};

// === qqqq_regs ===

#[test]
fn qqqq_regs_valid_values() {
    // All 8 valid combinations and their expected (s1, s2) register pairs.
    assert_eq!(qqqq_regs(0x0), Some((reg::X0, reg::X0)));
    assert_eq!(qqqq_regs(0x1), Some((reg::Y0, reg::Y0)));
    assert_eq!(qqqq_regs(0x2), Some((reg::X1, reg::X0)));
    assert_eq!(qqqq_regs(0x3), Some((reg::Y1, reg::Y0)));
    assert_eq!(qqqq_regs(0x4), Some((reg::X0, reg::Y1)));
    assert_eq!(qqqq_regs(0x5), Some((reg::Y0, reg::X0)));
    assert_eq!(qqqq_regs(0x6), Some((reg::X1, reg::Y0)));
    assert_eq!(qqqq_regs(0x7), Some((reg::Y1, reg::X1)));
}

#[test]
fn qqqq_regs_upper_values() {
    // Values 8-15 are valid per Table 12-16 Encoding 4.
    assert_eq!(qqqq_regs(0x8), Some((reg::X1, reg::X1)));
    assert_eq!(qqqq_regs(0x9), Some((reg::Y1, reg::Y1)));
    assert_eq!(qqqq_regs(0xA), Some((reg::X0, reg::X1)));
    assert_eq!(qqqq_regs(0xB), Some((reg::Y0, reg::Y1)));
    assert_eq!(qqqq_regs(0xC), Some((reg::Y1, reg::X0)));
    assert_eq!(qqqq_regs(0xD), Some((reg::X0, reg::Y0)));
    assert_eq!(qqqq_regs(0xE), Some((reg::Y0, reg::X1)));
    assert_eq!(qqqq_regs(0xF), Some((reg::X1, reg::Y1)));
    // Value 16+ should return None
    assert_eq!(qqqq_regs(16), None);
}

// === sss_reg ===

#[test]
fn sss_reg_reserved_values() {
    // Values 0 and 1 are reserved.
    assert_eq!(sss_reg(0), None);
    assert_eq!(sss_reg(1), None);
}

#[test]
fn sss_reg_all_valid_values() {
    assert_eq!(sss_reg(2), Some(reg::A1));
    assert_eq!(sss_reg(3), Some(reg::B1));
    assert_eq!(sss_reg(4), Some(reg::X0));
    assert_eq!(sss_reg(5), Some(reg::Y0));
    assert_eq!(sss_reg(6), Some(reg::X1));
    assert_eq!(sss_reg(7), Some(reg::Y1));
}

// === qqq_reg ===

#[test]
fn qqq_reg_reserved_values() {
    // Values 0 and 1 are reserved.
    assert_eq!(qqq_reg(0), None);
    assert_eq!(qqq_reg(1), None);
}

#[test]
fn qqq_reg_all_valid_values() {
    assert_eq!(qqq_reg(2), Some(reg::A0));
    assert_eq!(qqq_reg(3), Some(reg::B0));
    assert_eq!(qqq_reg(4), Some(reg::X0));
    assert_eq!(qqq_reg(5), Some(reg::Y0));
    assert_eq!(qqq_reg(6), Some(reg::X1));
    assert_eq!(qqq_reg(7), Some(reg::Y1));
}

// === ParallelAlu::dest_accumulator ===

#[test]
fn dest_accumulator_max() {
    // Max always writes to B.
    assert_eq!(ParallelAlu::Max.dest_accumulator(), Some(Accumulator::B));
}

#[test]
fn dest_accumulator_unary_with_d() {
    // Unary ops return the d field.
    assert_eq!(
        ParallelAlu::Clr { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Clr { d: Accumulator::B }.dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Rnd { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Addl {
            src: Accumulator::B,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Subl {
            src: Accumulator::A,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Not { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Asr { d: Accumulator::B }.dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Lsr { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Abs { d: Accumulator::B }.dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Ror { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Asl { d: Accumulator::B }.dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Lsl { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Neg { d: Accumulator::B }.dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Rol { d: Accumulator::A }.dest_accumulator(),
        Some(Accumulator::A)
    );
}

#[test]
fn dest_accumulator_acc_ops_with_d() {
    assert_eq!(
        ParallelAlu::TfrAcc {
            src: Accumulator::B,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Addr {
            src: Accumulator::A,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Subr {
            src: Accumulator::B,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::AddAcc {
            src: Accumulator::B,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::SubAcc {
            src: Accumulator::A,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
}

#[test]
fn dest_accumulator_xy_pair_ops() {
    assert_eq!(
        ParallelAlu::AddXY {
            hi: reg::X1,
            lo: reg::X0,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Adc {
            hi: reg::Y1,
            lo: reg::Y0,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::SubXY {
            hi: reg::X1,
            lo: reg::X0,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Sbc {
            hi: reg::Y1,
            lo: reg::Y0,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
}

#[test]
fn dest_accumulator_single_reg_ops() {
    assert_eq!(
        ParallelAlu::AddReg {
            src: reg::X0,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::TfrReg {
            src: reg::Y1,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Or {
            src: reg::X1,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Eor {
            src: reg::Y0,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::SubReg {
            src: reg::X0,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::And {
            src: reg::Y1,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
}

#[test]
fn dest_accumulator_multiply_ops() {
    assert_eq!(
        ParallelAlu::Mpy {
            negate: false,
            s1: reg::X0,
            s2: reg::X0,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Mpyr {
            negate: true,
            s1: reg::Y0,
            s2: reg::Y0,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
    assert_eq!(
        ParallelAlu::Mac {
            negate: false,
            s1: reg::X1,
            s2: reg::X0,
            d: Accumulator::A
        }
        .dest_accumulator(),
        Some(Accumulator::A)
    );
    assert_eq!(
        ParallelAlu::Macr {
            negate: true,
            s1: reg::Y1,
            s2: reg::Y0,
            d: Accumulator::B
        }
        .dest_accumulator(),
        Some(Accumulator::B)
    );
}

#[test]
fn dest_accumulator_comparison_ops_return_none() {
    // Comparison and test ops only set condition codes -- no destination accumulator.
    assert_eq!(
        ParallelAlu::CmpAcc {
            src: Accumulator::A,
            d: Accumulator::B
        }
        .dest_accumulator(),
        None
    );
    assert_eq!(
        ParallelAlu::CmpmAcc {
            src: Accumulator::B,
            d: Accumulator::A
        }
        .dest_accumulator(),
        None
    );
    assert_eq!(
        ParallelAlu::Tst { d: Accumulator::A }.dest_accumulator(),
        None
    );
    assert_eq!(
        ParallelAlu::CmpReg {
            src: reg::X0,
            d: Accumulator::B
        }
        .dest_accumulator(),
        None
    );
    assert_eq!(
        ParallelAlu::CmpmReg {
            src: reg::Y1,
            d: Accumulator::A
        }
        .dest_accumulator(),
        None
    );
}

#[test]
fn dest_accumulator_move_returns_none() {
    assert_eq!(ParallelAlu::Move.dest_accumulator(), None);
}

#[test]
fn dest_accumulator_undefined_returns_none() {
    assert_eq!(ParallelAlu::Undefined.dest_accumulator(), None);
}

// === ParallelAlu::from_text ===

#[test]
fn from_text_move() {
    assert_eq!(ParallelAlu::from_text("move"), Some(ParallelAlu::Move));
}

#[test]
fn from_text_unary_ops() {
    assert_eq!(
        ParallelAlu::from_text("clr a"),
        Some(ParallelAlu::Clr { d: Accumulator::A })
    );
    assert_eq!(
        ParallelAlu::from_text("clr b"),
        Some(ParallelAlu::Clr { d: Accumulator::B })
    );
    assert_eq!(
        ParallelAlu::from_text("tst a"),
        Some(ParallelAlu::Tst { d: Accumulator::A })
    );
    assert_eq!(
        ParallelAlu::from_text("rnd b"),
        Some(ParallelAlu::Rnd { d: Accumulator::B })
    );
    assert_eq!(
        ParallelAlu::from_text("not a"),
        Some(ParallelAlu::Not { d: Accumulator::A })
    );
    assert_eq!(
        ParallelAlu::from_text("asr b"),
        Some(ParallelAlu::Asr { d: Accumulator::B })
    );
    assert_eq!(
        ParallelAlu::from_text("lsr a"),
        Some(ParallelAlu::Lsr { d: Accumulator::A })
    );
    assert_eq!(
        ParallelAlu::from_text("abs b"),
        Some(ParallelAlu::Abs { d: Accumulator::B })
    );
    assert_eq!(
        ParallelAlu::from_text("ror a"),
        Some(ParallelAlu::Ror { d: Accumulator::A })
    );
    assert_eq!(
        ParallelAlu::from_text("asl b"),
        Some(ParallelAlu::Asl { d: Accumulator::B })
    );
    assert_eq!(
        ParallelAlu::from_text("lsl a"),
        Some(ParallelAlu::Lsl { d: Accumulator::A })
    );
    assert_eq!(
        ParallelAlu::from_text("neg b"),
        Some(ParallelAlu::Neg { d: Accumulator::B })
    );
    assert_eq!(
        ParallelAlu::from_text("rol a"),
        Some(ParallelAlu::Rol { d: Accumulator::A })
    );
}

#[test]
fn from_text_binary_acc_to_acc() {
    assert_eq!(
        ParallelAlu::from_text("add b,a"),
        Some(ParallelAlu::AddAcc {
            src: Accumulator::B,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("sub a,b"),
        Some(ParallelAlu::SubAcc {
            src: Accumulator::A,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("tfr b,a"),
        Some(ParallelAlu::TfrAcc {
            src: Accumulator::B,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("cmp a,b"),
        Some(ParallelAlu::CmpAcc {
            src: Accumulator::A,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("cmpm b,a"),
        Some(ParallelAlu::CmpmAcc {
            src: Accumulator::B,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("addr b,a"),
        Some(ParallelAlu::Addr {
            src: Accumulator::B,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("subr a,b"),
        Some(ParallelAlu::Subr {
            src: Accumulator::A,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("addl b,a"),
        Some(ParallelAlu::Addl {
            src: Accumulator::B,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("subl a,b"),
        Some(ParallelAlu::Subl {
            src: Accumulator::A,
            d: Accumulator::B
        })
    );
}

#[test]
fn from_text_binary_xy_pair() {
    assert_eq!(
        ParallelAlu::from_text("add x,a"),
        Some(ParallelAlu::AddXY {
            hi: reg::X1,
            lo: reg::X0,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("add y,b"),
        Some(ParallelAlu::AddXY {
            hi: reg::Y1,
            lo: reg::Y0,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("sub x,b"),
        Some(ParallelAlu::SubXY {
            hi: reg::X1,
            lo: reg::X0,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("adc x,a"),
        Some(ParallelAlu::Adc {
            hi: reg::X1,
            lo: reg::X0,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("sbc y,b"),
        Some(ParallelAlu::Sbc {
            hi: reg::Y1,
            lo: reg::Y0,
            d: Accumulator::B
        })
    );
}

#[test]
fn from_text_binary_single_reg() {
    assert_eq!(
        ParallelAlu::from_text("add x0,a"),
        Some(ParallelAlu::AddReg {
            src: reg::X0,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("add y0,b"),
        Some(ParallelAlu::AddReg {
            src: reg::Y0,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("add x1,a"),
        Some(ParallelAlu::AddReg {
            src: reg::X1,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("add y1,b"),
        Some(ParallelAlu::AddReg {
            src: reg::Y1,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("tfr x0,b"),
        Some(ParallelAlu::TfrReg {
            src: reg::X0,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("or y0,a"),
        Some(ParallelAlu::Or {
            src: reg::Y0,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("eor x1,b"),
        Some(ParallelAlu::Eor {
            src: reg::X1,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("sub y1,a"),
        Some(ParallelAlu::SubReg {
            src: reg::Y1,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("cmp x0,b"),
        Some(ParallelAlu::CmpReg {
            src: reg::X0,
            d: Accumulator::B
        })
    );
    assert_eq!(
        ParallelAlu::from_text("and y0,a"),
        Some(ParallelAlu::And {
            src: reg::Y0,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("cmpm x1,b"),
        Some(ParallelAlu::CmpmReg {
            src: reg::X1,
            d: Accumulator::B
        })
    );
}

#[test]
fn from_text_max() {
    assert_eq!(ParallelAlu::from_text("max a,b"), Some(ParallelAlu::Max));
}

#[test]
fn from_text_multiply() {
    // "mpy +y0,x0,a": parse_qqq_pair("y0","x0") -> (Y0,X0) which is a valid QQQ combination.
    assert_eq!(
        ParallelAlu::from_text("mpy +y0,x0,a"),
        Some(ParallelAlu::Mpy {
            negate: false,
            s1: reg::Y0,
            s2: reg::X0,
            d: Accumulator::A
        })
    );
    assert_eq!(
        ParallelAlu::from_text("mpy -y0,x0,b"),
        Some(ParallelAlu::Mpy {
            negate: true,
            s1: reg::Y0,
            s2: reg::X0,
            d: Accumulator::B
        })
    );
    // "mpyr +x1,x0,a": (X1,X0) is a valid QQQ combination.
    assert_eq!(
        ParallelAlu::from_text("mpyr +x1,x0,a"),
        Some(ParallelAlu::Mpyr {
            negate: false,
            s1: reg::X1,
            s2: reg::X0,
            d: Accumulator::A
        })
    );
    // "mac +y1,x1,b": (Y1,X1) is a valid QQQ combination.
    assert_eq!(
        ParallelAlu::from_text("mac +y1,x1,b"),
        Some(ParallelAlu::Mac {
            negate: false,
            s1: reg::Y1,
            s2: reg::X1,
            d: Accumulator::B
        })
    );
    // "macr -y0,y0,a": (Y0,Y0) is a valid QQQ combination.
    assert_eq!(
        ParallelAlu::from_text("macr -y0,y0,a"),
        Some(ParallelAlu::Macr {
            negate: true,
            s1: reg::Y0,
            s2: reg::Y0,
            d: Accumulator::A
        })
    );
}

// === from_text invalid / None-returning paths ===

#[test]
fn from_text_invalid_mnemonic() {
    assert_eq!(ParallelAlu::from_text("bogus"), None);
    assert_eq!(ParallelAlu::from_text(""), None);
    assert_eq!(ParallelAlu::from_text("xyz a,b"), None);
}

#[test]
fn from_text_mnemonic_without_operands() {
    // "add" with no space -- split_once(' ') returns None for mnemonic-only.
    assert_eq!(ParallelAlu::from_text("add"), None);
    assert_eq!(ParallelAlu::from_text("clr"), None);
}

#[test]
fn from_text_wrong_operand_count() {
    // Binary op needs exactly 2 operands.
    assert_eq!(ParallelAlu::from_text("add a"), None); // only 1 operand
    assert_eq!(ParallelAlu::from_text("add a,b,a"), None); // 3 operands
}

#[test]
fn from_text_mpy_missing_sign() {
    // mpy/mpyr/mac/macr require a leading +/- on the first source register.
    assert_eq!(ParallelAlu::from_text("mpy x0,y0,a"), None);
    assert_eq!(ParallelAlu::from_text("mac x1,x0,b"), None);
}

#[test]
fn from_text_mpy_invalid_qqq_pair() {
    // x0,y0 is not a valid QQQ combination (only y0,x0 is valid in that direction).
    assert_eq!(ParallelAlu::from_text("mpy +x0,y0,a"), None);
    // x0,x1 is also not a valid QQQ combination.
    assert_eq!(ParallelAlu::from_text("mpy +x0,x1,a"), None);
}

#[test]
fn from_text_invalid_register_in_jj_position() {
    // "a0" is not a JJ register (x0/y0/x1/y1).
    assert_eq!(ParallelAlu::from_text("add a0,b"), None);
}

#[test]
fn from_text_invalid_accumulator() {
    // "c" is not a valid accumulator name.
    assert_eq!(ParallelAlu::from_text("clr c"), None);
    assert_eq!(ParallelAlu::from_text("add x0,c"), None);
}

#[test]
fn from_text_max_wrong_operands() {
    assert_eq!(ParallelAlu::from_text("max b,a"), None); // correct is "max a,b"
    assert_eq!(ParallelAlu::from_text("max a"), None);
}

#[test]
fn from_text_binary_invalid_xy_mnemonic() {
    // "tfr x,a" -- x is a valid xy pair but tfr doesn't support XY pairs.
    assert_eq!(ParallelAlu::from_text("tfr x,a"), None);
}

#[test]
fn from_text_parse_jj_reg_invalid() {
    // Trigger the _ => None branch of parse_jj_reg via an acc-to-acc mnemonic with bad reg.
    assert_eq!(ParallelAlu::from_text("or z0,a"), None);
}

#[test]
fn from_text_parse_qqq_pair_invalid_registers() {
    // Both registers are valid JJ regs but the combination is not a legal QQQ pair.
    assert_eq!(
        ParallelAlu::from_text("mpy +x0,x0,a"),
        Some(ParallelAlu::Mpy {
            negate: false,
            s1: reg::X0,
            s2: reg::X0,
            d: Accumulator::A
        })
    );
    // x1,y1 is not a valid QQQ pair.
    assert_eq!(ParallelAlu::from_text("mpy +x1,y1,a"), None);
}
