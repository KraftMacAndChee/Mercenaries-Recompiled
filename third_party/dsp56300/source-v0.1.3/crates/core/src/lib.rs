//! DSP56300 ISA definitions: architectural constants, register indices, status
//! register bits, instruction decoder, encoder, and parallel ALU types.

pub mod decode;
pub mod encode;

// DSP56300 architectural constants
pub const PERIPH_BASE: u32 = 0xFFFF80;
pub const PERIPH_SIZE: usize = 128;
pub const PC_MASK: u32 = 0xFFFFFF;

/// Mask a value to the 24-bit program address space.
#[inline(always)]
pub const fn mask_pc(v: u32) -> u32 {
    v & PC_MASK
}

// Register file indices (6-bit DDDDDD encoding per DSP56300 instruction set).
pub mod reg {
    pub const X0: usize = 0x04;
    pub const X1: usize = 0x05;
    pub const Y0: usize = 0x06;
    pub const Y1: usize = 0x07;
    pub const A0: usize = 0x08;
    pub const B0: usize = 0x09;
    pub const A2: usize = 0x0a;
    pub const B2: usize = 0x0b;
    pub const A1: usize = 0x0c;
    pub const B1: usize = 0x0d;
    pub const A: usize = 0x0e;
    pub const B: usize = 0x0f;

    pub const R0: usize = 0x10;
    pub const R1: usize = 0x11;
    pub const R2: usize = 0x12;
    pub const R3: usize = 0x13;
    pub const R4: usize = 0x14;
    pub const R5: usize = 0x15;
    pub const R6: usize = 0x16;
    pub const R7: usize = 0x17;

    pub const N0: usize = 0x18;
    pub const N1: usize = 0x19;
    pub const N2: usize = 0x1a;
    pub const N3: usize = 0x1b;
    pub const N4: usize = 0x1c;
    pub const N5: usize = 0x1d;
    pub const N6: usize = 0x1e;
    pub const N7: usize = 0x1f;

    pub const M0: usize = 0x20;
    pub const M1: usize = 0x21;
    pub const M2: usize = 0x22;
    pub const M3: usize = 0x23;
    pub const M4: usize = 0x24;
    pub const M5: usize = 0x25;
    pub const M6: usize = 0x26;
    pub const M7: usize = 0x27;

    pub const EP: usize = 0x2a;

    pub const VBA: usize = 0x30;
    pub const SC: usize = 0x31;

    pub const SZ: usize = 0x38;
    pub const SR: usize = 0x39;
    pub const OMR: usize = 0x3a;
    pub const SP: usize = 0x3b;
    pub const SSH: usize = 0x3c;
    pub const SSL: usize = 0x3d;
    pub const LA: usize = 0x3e;
    pub const LC: usize = 0x3f;

    pub const NULL: usize = 0x00;
    // Internal temporary register used by REP to save/restore LC (manual page 13-160:
    // "LC -> TEMP ... TEMP -> LC"). Uses reserved EEE slot 0x2B.
    pub const TEMP: usize = 0x2b;

    pub const COUNT: usize = 0x40;
}

/// QQ register mapping for standard multiply instructions (mpyi/maci/etc).
pub fn qq_reg(qq: u8) -> usize {
    match qq {
        0 => reg::X0,
        1 => reg::Y0,
        2 => reg::X1,
        3 => reg::Y1,
        _ => unreachable!(),
    }
}

/// QQ register mapping for MulShift instructions (mpy/mpyr/mac/macr S,#n,D).
/// Different from the standard qq mapping used by mpyi/maci/etc.
pub fn qq_reg_mulshift(qq: u8) -> usize {
    match qq {
        0 => reg::Y1,
        1 => reg::X0,
        2 => reg::Y0,
        3 => reg::X1,
        _ => unreachable!(),
    }
}

/// QQQQ register pair mapping for DMAC/MpySU/MacSU instructions.
/// 4-bit field per Table 12-16 "Data ALU Multiply Operands Encoding 4".
/// All 16 values (0x0-0xF) are valid.
pub fn qqqq_regs(qqqq: u8) -> Option<(usize, usize)> {
    match qqqq {
        0x0 => Some((reg::X0, reg::X0)),
        0x1 => Some((reg::Y0, reg::Y0)),
        0x2 => Some((reg::X1, reg::X0)),
        0x3 => Some((reg::Y1, reg::Y0)),
        0x4 => Some((reg::X0, reg::Y1)),
        0x5 => Some((reg::Y0, reg::X0)),
        0x6 => Some((reg::X1, reg::Y0)),
        0x7 => Some((reg::Y1, reg::X1)),
        0x8 => Some((reg::X1, reg::X1)),
        0x9 => Some((reg::Y1, reg::Y1)),
        0xA => Some((reg::X0, reg::X1)),
        0xB => Some((reg::Y0, reg::Y1)),
        0xC => Some((reg::Y1, reg::X0)),
        0xD => Some((reg::X0, reg::Y0)),
        0xE => Some((reg::Y0, reg::X1)),
        0xF => Some((reg::X1, reg::Y1)),
        _ => None,
    }
}

/// SSS register mapping (Table 12-13, S1 column): 3-bit field, values 0-1 reserved.
pub fn sss_reg(sss: u8) -> Option<usize> {
    match sss {
        2 => Some(reg::A1),
        3 => Some(reg::B1),
        4 => Some(reg::X0),
        5 => Some(reg::Y0),
        6 => Some(reg::X1),
        7 => Some(reg::Y1),
        _ => None,
    }
}

/// QQQ register mapping (Table 12-13, S2 column): 3-bit field, values 0-1 reserved.
/// Differs from sss: 2=A0, 3=B0 (vs A1, B1 for sss).
pub fn qqq_reg(qqq: u8) -> Option<usize> {
    match qqq {
        2 => Some(reg::A0),
        3 => Some(reg::B0),
        4 => Some(reg::X0),
        5 => Some(reg::Y0),
        6 => Some(reg::X1),
        7 => Some(reg::Y1),
        _ => None,
    }
}

/// GGG register mapping for CMPU: 3-bit field, values 1-3 reserved.
/// Value 0 selects the opposite accumulator from `d`.
pub fn ggg_reg(ggg: u8, d: Accumulator) -> Option<usize> {
    match ggg {
        0 => Some(match d {
            Accumulator::A => reg::B,
            Accumulator::B => reg::A,
        }),
        4 => Some(reg::X0),
        5 => Some(reg::Y0),
        6 => Some(reg::X1),
        7 => Some(reg::Y1),
        _ => None,
    }
}

// Status register bit positions
pub mod sr {
    pub const C: u32 = 0;
    pub const V: u32 = 1;
    pub const Z: u32 = 2;
    pub const N: u32 = 3;
    pub const U: u32 = 4;
    pub const E: u32 = 5;
    pub const L: u32 = 6;
    pub const S: u32 = 7;
    pub const I0: u32 = 8;
    pub const I1: u32 = 9;
    pub const S0: u32 = 10;
    pub const S1: u32 = 11;
    pub const SC: u32 = 13;
    pub const DM: u32 = 14;
    pub const LF: u32 = 15;
    pub const FV: u32 = 16;
    pub const SA: u32 = 17;
    pub const CE: u32 = 19;
    pub const SM: u32 = 20;
    pub const RM: u32 = 21;
}

/// Architectural register masks applied when writing registers.
///
/// Each entry is the bitmask of valid bits for the corresponding `reg::*` index.
/// Writes through `mask_reg()` or `store_reg()` are ANDed with this mask.
/// This is the **single source of truth** for register widths.
pub const REG_MASKS: [u32; reg::COUNT] = {
    let mut m = [0xFFFF_FFFFu32; reg::COUNT];
    m[reg::X0] = 0x00FF_FFFF;
    m[reg::X1] = 0x00FF_FFFF;
    m[reg::Y0] = 0x00FF_FFFF;
    m[reg::Y1] = 0x00FF_FFFF;
    m[reg::A0] = 0x00FF_FFFF;
    m[reg::B0] = 0x00FF_FFFF;
    m[reg::A2] = 0x0000_00FF;
    m[reg::B2] = 0x0000_00FF;
    m[reg::A1] = 0x00FF_FFFF;
    m[reg::B1] = 0x00FF_FFFF;
    m[reg::A] = 0x00FF_FFFF;
    m[reg::B] = 0x00FF_FFFF;
    let mut i = 0;
    while i < 8 {
        m[reg::R0 + i] = 0x00FF_FFFF;
        m[reg::N0 + i] = 0x00FF_FFFF;
        m[reg::M0 + i] = 0x00FF_FFFF;
        i += 1;
    }
    m[reg::EP] = 0x00FF_FFFF; // 24-bit extension pointer
    m[reg::VBA] = 0x00FF_FF00; // VBA[7:0] are read-only and always cleared
    m[reg::SC] = 0x0000_001F; // 5-bit stack counter
    m[reg::SZ] = 0x00FF_FFFF; // 24-bit stack size
    m[reg::SR] = 0x00FB_EFFF; // bits 12 and 18 reserved
    m[reg::OMR] = 0x00FF_FFDF; // bit 5 reserved
    m[reg::SP] = 0x0000_003F; // 6-bit in non-extended mode
    m[reg::SSH] = 0x00FF_FFFF;
    m[reg::SSL] = 0x00FF_FFFF;
    m[reg::LA] = 0x00FF_FFFF;
    m[reg::LC] = 0x00FF_FFFF;
    m[reg::TEMP] = 0x00FF_FFFF;
    m[reg::NULL] = 0;
    m
};

/// Mask a value to the architectural width of register `r`.
#[inline(always)]
pub const fn mask_reg(r: usize, v: u32) -> u32 {
    v & REG_MASKS[r]
}

/// Register names (indexed by register number, matching `reg::*` constants).
pub const REGISTER_NAMES: [&str; 64] = [
    "", "", "", "", "x0", "x1", "y0", "y1", "a0", "b0", "a2", "b2", "a1", "b1", "a", "b", "r0",
    "r1", "r2", "r3", "r4", "r5", "r6", "r7", "n0", "n1", "n2", "n3", "n4", "n5", "n6", "n7", "m0",
    "m1", "m2", "m3", "m4", "m5", "m6", "m7", "", "", "ep", "", "", "", "", "", "vba", "sc", "",
    "", "", "", "", "", "sz", "sr", "omr", "sp", "ssh", "ssl", "la", "lc",
];

/// Condition code names (indexed by 4-bit CCCC field).
pub const CC_NAMES: [&str; 16] = [
    "cc", "ge", "ne", "pl", "nn", "ec", "lc", "gt", "cs", "lt", "eq", "mi", "nr", "es", "ls", "le",
];

/// Long-move register names for L: parallel moves.
pub const REGISTERS_LMOVE: [&str; 8] = ["a10", "b10", "x", "y", "a", "b", "ab", "ba"];

/// TCC register pairs: [src, dst] indexed by (opcode>>3) & 0xF.
pub const REGISTERS_TCC: [[usize; 2]; 16] = [
    [reg::B, reg::A],
    [reg::A, reg::B],
    [reg::NULL, reg::NULL],
    [reg::NULL, reg::NULL],
    [reg::NULL, reg::NULL],
    [reg::NULL, reg::NULL],
    [reg::NULL, reg::NULL],
    [reg::NULL, reg::NULL],
    [reg::X0, reg::A],
    [reg::X0, reg::B],
    [reg::Y0, reg::A],
    [reg::Y0, reg::B],
    [reg::X1, reg::A],
    [reg::X1, reg::B],
    [reg::Y1, reg::A],
    [reg::Y1, reg::B],
];

// Memory spaces
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum MemSpace {
    X = 0,
    Y = 1,
    P = 2,
}

impl MemSpace {
    /// Convert from opcode bit (0 = X, 1 = Y). Panics on invalid values.
    pub fn xy(bit: u32) -> Self {
        match bit {
            0 => Self::X,
            1 => Self::Y,
            _ => unreachable!(),
        }
    }
}

/// Accumulator selector (bit d in many encodings).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Accumulator {
    A = 0,
    B = 1,
}

/// Operation type for MulShift (S,#n,D) instructions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MulShiftOp {
    Mpy = 0,
    Mpyr = 1,
    Mac = 2,
    Macr = 3,
}

/// Condition code (4-bit CCCC field).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum CondCode {
    CC = 0, // carry clear (HS)
    GE = 1,
    NE = 2,
    PL = 3,
    NN = 4,
    EC = 5,
    LC = 6,
    GT = 7,
    CS = 8, // carry set (LO)
    LT = 9,
    EQ = 10,
    MI = 11,
    NR = 12,
    ES = 13,
    LS = 14,
    LE = 15,
}

impl CondCode {
    pub fn from_bits(bits: u32) -> Self {
        assert!(bits < 16);
        // Safety: all values 0-15 are valid
        unsafe { std::mem::transmute(bits as u8) }
    }
}

/// Decoded parallel ALU operation (bits 7:0 of a parallel instruction).
///
/// The encoding is defined in DSP56300FM Tables 12-19 and 12-20:
/// - Non-multiply (0x00-0x7F): `0 JJJ D kkk`
/// - Multiply (0x80-0xFF): `1 QQQ d kkk`
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParallelAlu {
    /// 0x00: No ALU operation (move-only parallel)
    Move,

    // --- 0x01-0x0F: JJJ=000, acc-to-acc (source is opposite of d) ---
    TfrAcc {
        src: Accumulator,
        d: Accumulator,
    },
    Addr {
        src: Accumulator,
        d: Accumulator,
    },
    Tst {
        d: Accumulator,
    },
    CmpAcc {
        src: Accumulator,
        d: Accumulator,
    },
    Subr {
        src: Accumulator,
        d: Accumulator,
    },
    CmpmAcc {
        src: Accumulator,
        d: Accumulator,
    },

    // --- 0x10-0x1F: JJJ=001, acc-to-acc ops ---
    AddAcc {
        src: Accumulator,
        d: Accumulator,
    },
    Rnd {
        d: Accumulator,
    },
    Addl {
        src: Accumulator,
        d: Accumulator,
    },
    Clr {
        d: Accumulator,
    },
    SubAcc {
        src: Accumulator,
        d: Accumulator,
    },
    /// 0x1D: always max a,b
    Max,
    /// 0x15: always maxm a,b
    Maxm,
    Subl {
        src: Accumulator,
        d: Accumulator,
    },
    Not {
        d: Accumulator,
    },

    // --- 0x20-0x3F: JJJ=01x, X/Y register pair source ---
    AddXY {
        hi: usize,
        lo: usize,
        d: Accumulator,
    },
    Adc {
        hi: usize,
        lo: usize,
        d: Accumulator,
    },
    SubXY {
        hi: usize,
        lo: usize,
        d: Accumulator,
    },
    Sbc {
        hi: usize,
        lo: usize,
        d: Accumulator,
    },
    Asr {
        d: Accumulator,
    },
    Lsr {
        d: Accumulator,
    },
    Abs {
        d: Accumulator,
    },
    Ror {
        d: Accumulator,
    },
    Asl {
        d: Accumulator,
    },
    Lsl {
        d: Accumulator,
    },
    Neg {
        d: Accumulator,
    },
    Rol {
        d: Accumulator,
    },

    // --- 0x40-0x7F: JJJ=1xx, single register source (X0/Y0/X1/Y1) ---
    AddReg {
        src: usize,
        d: Accumulator,
    },
    TfrReg {
        src: usize,
        d: Accumulator,
    },
    Or {
        src: usize,
        d: Accumulator,
    },
    Eor {
        src: usize,
        d: Accumulator,
    },
    SubReg {
        src: usize,
        d: Accumulator,
    },
    CmpReg {
        src: usize,
        d: Accumulator,
    },
    And {
        src: usize,
        d: Accumulator,
    },
    CmpmReg {
        src: usize,
        d: Accumulator,
    },

    // --- 0x80-0xFF: multiply/MAC (1 QQQ d kkk) ---
    Mpy {
        negate: bool,
        s1: usize,
        s2: usize,
        d: Accumulator,
    },
    Mpyr {
        negate: bool,
        s1: usize,
        s2: usize,
        d: Accumulator,
    },
    Mac {
        negate: bool,
        s1: usize,
        s2: usize,
        d: Accumulator,
    },
    Macr {
        negate: bool,
        s1: usize,
        s2: usize,
        d: Accumulator,
    },

    /// Undefined encoding (0x04, 0x08, 0x0C, 0x15)
    Undefined,
}

/// QQQ register pair mapping for parallel multiply instructions (Table 12-16, Encoding 1).
/// QQQ is bits 6:4 of the ALU byte (0x80-0xFF).
fn qqq_mpy_regs(qqq: u8) -> (usize, usize) {
    match qqq {
        0 => (reg::X0, reg::X0),
        1 => (reg::Y0, reg::Y0),
        2 => (reg::X1, reg::X0),
        3 => (reg::Y1, reg::Y0),
        4 => (reg::X0, reg::Y1),
        5 => (reg::Y0, reg::X0),
        6 => (reg::X1, reg::Y0),
        7 => (reg::Y1, reg::X1),
        _ => unreachable!(),
    }
}

/// JJ register mapping for parallel ALU instructions (0x40-0x7F).
const JJ_REGS: [usize; 4] = [reg::X0, reg::Y0, reg::X1, reg::Y1];

/// Decode the parallel ALU byte (bits 7:0) into a structured `ParallelAlu`.
pub fn decode_parallel_alu(b: u8) -> ParallelAlu {
    use Accumulator::*;
    use ParallelAlu::*;

    if b == 0x00 {
        return Move;
    }

    if b & 0x80 != 0 {
        // Multiply: 1 QQQ d kkk
        let qqq = (b >> 4) & 7;
        let d = if (b >> 3) & 1 == 0 { A } else { B };
        let negate = (b >> 2) & 1 == 1;
        let op = b & 3;
        let (s1, s2) = qqq_mpy_regs(qqq);
        return match op {
            0 => Mpy { negate, s1, s2, d },
            1 => Mpyr { negate, s1, s2, d },
            2 => Mac { negate, s1, s2, d },
            3 => Macr { negate, s1, s2, d },
            _ => unreachable!(),
        };
    }

    // Non-multiply: 0 JJJ D kkk
    let jjj = (b >> 4) & 7;
    let d = if (b >> 3) & 1 == 0 { A } else { B };
    let src = match d {
        A => B,
        B => A,
    };
    let kkk = b & 7;

    match jjj {
        0 => match kkk {
            1 => TfrAcc { src, d },
            2 => Addr { src, d },
            3 => Tst { d },
            5 => CmpAcc { src, d },
            6 => Subr { src, d },
            7 => CmpmAcc { src, d },
            _ => Undefined, // 0, 4 (but 0x00=Move handled above, 0x04/0x08/0x0C are undefined)
        },
        1 => match kkk {
            0 => AddAcc { src, d },
            1 => Rnd { d },
            2 => Addl { src, d },
            3 => Clr { d },
            4 => SubAcc { src, d },
            5 => {
                if b == 0x1D {
                    Max
                } else if b == 0x15 {
                    Maxm
                } else {
                    Undefined
                }
            }
            6 => Subl { src, d },
            7 => Not { d },
            _ => unreachable!(),
        },
        2 => {
            // X register pair source (x1:x0)
            match kkk {
                0 => AddXY {
                    hi: reg::X1,
                    lo: reg::X0,
                    d,
                },
                1 => Adc {
                    hi: reg::X1,
                    lo: reg::X0,
                    d,
                },
                2 => Asr { d },
                3 => Lsr { d },
                4 => SubXY {
                    hi: reg::X1,
                    lo: reg::X0,
                    d,
                },
                5 => Sbc {
                    hi: reg::X1,
                    lo: reg::X0,
                    d,
                },
                6 => Abs { d },
                7 => Ror { d },
                _ => unreachable!(),
            }
        }
        3 => {
            // Y register pair source (y1:y0)
            match kkk {
                0 => AddXY {
                    hi: reg::Y1,
                    lo: reg::Y0,
                    d,
                },
                1 => Adc {
                    hi: reg::Y1,
                    lo: reg::Y0,
                    d,
                },
                2 => Asl { d },
                3 => Lsl { d },
                4 => SubXY {
                    hi: reg::Y1,
                    lo: reg::Y0,
                    d,
                },
                5 => Sbc {
                    hi: reg::Y1,
                    lo: reg::Y0,
                    d,
                },
                6 => Neg { d },
                7 => Rol { d },
                _ => unreachable!(),
            }
        }
        4..=7 => {
            // Single register source: JJ = jjj[1:0]
            let jj = (jjj & 3) as usize;
            let src = JJ_REGS[jj];
            match kkk {
                0 => AddReg { src, d },
                1 => TfrReg { src, d },
                2 => Or { src, d },
                3 => Eor { src, d },
                4 => SubReg { src, d },
                5 => CmpReg { src, d },
                6 => And { src, d },
                7 => CmpmReg { src, d },
                _ => unreachable!(),
            }
        }
        _ => unreachable!(),
    }
}

impl ParallelAlu {
    /// Encode back to the ALU byte. Returns `None` for `Undefined`.
    pub fn encode(&self) -> Option<u8> {
        use ParallelAlu::*;

        Some(match *self {
            Move => 0x00,

            // JJJ=000
            TfrAcc { d, .. } => 0x01 | ((d as u8) << 3),
            Addr { d, .. } => 0x02 | ((d as u8) << 3),
            Tst { d } => 0x03 | ((d as u8) << 3),
            CmpAcc { d, .. } => 0x05 | ((d as u8) << 3),
            Subr { d, .. } => 0x06 | ((d as u8) << 3),
            CmpmAcc { d, .. } => 0x07 | ((d as u8) << 3),

            // JJJ=001
            AddAcc { d, .. } => 0x10 | ((d as u8) << 3),
            Rnd { d } => 0x11 | ((d as u8) << 3),
            Addl { d, .. } => 0x12 | ((d as u8) << 3),
            Clr { d } => 0x13 | ((d as u8) << 3),
            SubAcc { d, .. } => 0x14 | ((d as u8) << 3),
            Max => 0x1D,
            Maxm => 0x15,
            Subl { d, .. } => 0x16 | ((d as u8) << 3),
            Not { d } => 0x17 | ((d as u8) << 3),

            // JJJ=010 (X pair)
            AddXY { hi, d, .. } if hi == reg::X1 => 0x20 | ((d as u8) << 3),
            Adc { hi, d, .. } if hi == reg::X1 => 0x21 | ((d as u8) << 3),
            Asr { d } => 0x22 | ((d as u8) << 3),
            Lsr { d } => 0x23 | ((d as u8) << 3),
            SubXY { hi, d, .. } if hi == reg::X1 => 0x24 | ((d as u8) << 3),
            Sbc { hi, d, .. } if hi == reg::X1 => 0x25 | ((d as u8) << 3),
            Abs { d } => 0x26 | ((d as u8) << 3),
            Ror { d } => 0x27 | ((d as u8) << 3),

            // JJJ=011 (Y pair)
            AddXY { d, .. } => 0x30 | ((d as u8) << 3),
            Adc { d, .. } => 0x31 | ((d as u8) << 3),
            Asl { d } => 0x32 | ((d as u8) << 3),
            Lsl { d } => 0x33 | ((d as u8) << 3),
            SubXY { d, .. } => 0x34 | ((d as u8) << 3),
            Sbc { d, .. } => 0x35 | ((d as u8) << 3),
            Neg { d } => 0x36 | ((d as u8) << 3),
            Rol { d } => 0x37 | ((d as u8) << 3),

            // JJJ=1xx (single register)
            AddReg { src, d } => 0x40 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            TfrReg { src, d } => 0x41 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            Or { src, d } => 0x42 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            Eor { src, d } => 0x43 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            SubReg { src, d } => 0x44 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            CmpReg { src, d } => 0x45 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            And { src, d } => 0x46 | (jj_encode(src)? << 4) | ((d as u8) << 3),
            CmpmReg { src, d } => 0x47 | (jj_encode(src)? << 4) | ((d as u8) << 3),

            // Multiply
            Mpy { negate, s1, s2, d } => {
                0x80 | (qqq_mpy_encode(s1, s2)? << 4) | ((d as u8) << 3) | ((negate as u8) << 2)
            }
            Mpyr { negate, s1, s2, d } => {
                0x81 | (qqq_mpy_encode(s1, s2)? << 4) | ((d as u8) << 3) | ((negate as u8) << 2)
            }
            Mac { negate, s1, s2, d } => {
                0x82 | (qqq_mpy_encode(s1, s2)? << 4) | ((d as u8) << 3) | ((negate as u8) << 2)
            }
            Macr { negate, s1, s2, d } => {
                0x83 | (qqq_mpy_encode(s1, s2)? << 4) | ((d as u8) << 3) | ((negate as u8) << 2)
            }

            Undefined => return None,
        })
    }

    /// The destination accumulator of this ALU operation, if any.
    /// Returns `None` for `Move`, `Undefined`, and comparison/test ops (`Cmp*`, `Cmpm*`, `Tst`).
    pub fn dest_accumulator(&self) -> Option<Accumulator> {
        use ParallelAlu::*;
        match *self {
            Move | Undefined => None,
            // Comparisons and test only set condition codes
            CmpAcc { .. } | CmpmAcc { .. } | Tst { .. } | CmpReg { .. } | CmpmReg { .. } => None,
            // Max/Maxm always write to B (with A as source)
            Max | Maxm => Some(Accumulator::B),
            // All other variants have a `d` field
            TfrAcc { d, .. }
            | Addr { d, .. }
            | Subr { d, .. }
            | AddAcc { d, .. }
            | Rnd { d }
            | Addl { d, .. }
            | Clr { d }
            | SubAcc { d, .. }
            | Subl { d, .. }
            | Not { d }
            | AddXY { d, .. }
            | Adc { d, .. }
            | SubXY { d, .. }
            | Sbc { d, .. }
            | Asr { d }
            | Lsr { d }
            | Abs { d }
            | Ror { d }
            | Asl { d }
            | Lsl { d }
            | Neg { d }
            | Rol { d }
            | AddReg { d, .. }
            | TfrReg { d, .. }
            | Or { d, .. }
            | Eor { d, .. }
            | SubReg { d, .. }
            | And { d, .. }
            | Mpy { d, .. }
            | Mpyr { d, .. }
            | Mac { d, .. }
            | Macr { d, .. } => Some(d),
        }
    }

    /// Parse from the display text format (inverse of `Display`).
    ///
    /// Accepts strings like `"add x,a"`, `"mpy +x0,y1,b"`, `"move"`, etc.
    pub fn from_text(text: &str) -> Option<ParallelAlu> {
        use ParallelAlu::*;

        fn parse_acc(s: &str) -> Option<Accumulator> {
            match s {
                "a" => Some(Accumulator::A),
                "b" => Some(Accumulator::B),
                _ => None,
            }
        }

        fn parse_jj_reg(s: &str) -> Option<usize> {
            match s {
                "x0" => Some(reg::X0),
                "y0" => Some(reg::Y0),
                "x1" => Some(reg::X1),
                "y1" => Some(reg::Y1),
                _ => None,
            }
        }

        fn parse_xy_pair(s: &str) -> Option<(usize, usize)> {
            match s {
                "x" => Some((reg::X1, reg::X0)),
                "y" => Some((reg::Y1, reg::Y0)),
                _ => None,
            }
        }

        fn parse_qqq_pair(s1: &str, s2: &str) -> Option<(usize, usize)> {
            let r1 = parse_jj_reg(s1)?;
            let r2 = parse_jj_reg(s2)?;
            // Validate it's a legal QQQ combination
            match (r1, r2) {
                (reg::X0, reg::X0)
                | (reg::Y0, reg::Y0)
                | (reg::X1, reg::X0)
                | (reg::Y1, reg::Y0)
                | (reg::X0, reg::Y1)
                | (reg::Y0, reg::X0)
                | (reg::X1, reg::Y0)
                | (reg::Y1, reg::X1) => Some((r1, r2)),
                _ => None,
            }
        }

        if text == "move" {
            return Some(Move);
        }

        let (mnemonic, operands) = text.split_once(' ')?;
        let ops: Vec<&str> = operands.split(',').collect();

        match mnemonic {
            // Unary ops (1 operand = accumulator)
            "tst" | "rnd" | "clr" | "not" | "asr" | "lsr" | "abs" | "ror" | "asl" | "lsl"
            | "neg" | "rol" => {
                let d = parse_acc(ops.first()?)?;
                match mnemonic {
                    "tst" => Some(Tst { d }),
                    "rnd" => Some(Rnd { d }),
                    "clr" => Some(Clr { d }),
                    "not" => Some(Not { d }),
                    "asr" => Some(Asr { d }),
                    "lsr" => Some(Lsr { d }),
                    "abs" => Some(Abs { d }),
                    "ror" => Some(Ror { d }),
                    "asl" => Some(Asl { d }),
                    "lsl" => Some(Lsl { d }),
                    "neg" => Some(Neg { d }),
                    "rol" => Some(Rol { d }),
                    _ => unreachable!(),
                }
            }

            // Binary ops: src,d -- source determines variant
            "tfr" | "add" | "sub" | "cmp" | "cmpm" | "addr" | "subr" | "addl" | "subl" | "adc"
            | "sbc" | "or" | "eor" | "and" => {
                if ops.len() != 2 {
                    return None;
                }
                let d = parse_acc(ops[1])?;
                let src_str = ops[0];

                // Try accumulator source (acc-to-acc)
                if let Some(src) = parse_acc(src_str) {
                    return match mnemonic {
                        "tfr" => Some(TfrAcc { src, d }),
                        "addr" => Some(Addr { src, d }),
                        "cmp" => Some(CmpAcc { src, d }),
                        "subr" => Some(Subr { src, d }),
                        "cmpm" => Some(CmpmAcc { src, d }),
                        "add" => Some(AddAcc { src, d }),
                        "addl" => Some(Addl { src, d }),
                        "sub" => Some(SubAcc { src, d }),
                        "subl" => Some(Subl { src, d }),
                        _ => None,
                    };
                }

                // Try XY pair source (x/y)
                if let Some((hi, lo)) = parse_xy_pair(src_str) {
                    return match mnemonic {
                        "add" => Some(AddXY { hi, lo, d }),
                        "adc" => Some(Adc { hi, lo, d }),
                        "sub" => Some(SubXY { hi, lo, d }),
                        "sbc" => Some(Sbc { hi, lo, d }),
                        _ => None,
                    };
                }

                // Try single register source (x0/y0/x1/y1)
                if let Some(src) = parse_jj_reg(src_str) {
                    return match mnemonic {
                        "add" => Some(AddReg { src, d }),
                        "tfr" => Some(TfrReg { src, d }),
                        "or" => Some(Or { src, d }),
                        "eor" => Some(Eor { src, d }),
                        "sub" => Some(SubReg { src, d }),
                        "cmp" => Some(CmpReg { src, d }),
                        "and" => Some(And { src, d }),
                        "cmpm" => Some(CmpmReg { src, d }),
                        _ => None,
                    };
                }

                None
            }

            // Special: max a,b / maxm a,b
            "max" | "maxm" => {
                if ops.len() == 2 && ops[0] == "a" && ops[1] == "b" {
                    Some(if mnemonic == "max" { Max } else { Maxm })
                } else {
                    None
                }
            }

            // Multiply: mpy/mpyr/mac/macr [+/-]s1,s2,d
            "mpy" | "mpyr" | "mac" | "macr" => {
                if ops.len() != 3 {
                    return None;
                }
                let d = parse_acc(ops[2])?;
                let (negate, s1_str) = if let Some(rest) = ops[0].strip_prefix('-') {
                    (true, rest)
                } else if let Some(rest) = ops[0].strip_prefix('+') {
                    (false, rest)
                } else {
                    return None;
                };
                let (s1, s2) = parse_qqq_pair(s1_str, ops[1])?;
                match mnemonic {
                    "mpy" => Some(Mpy { negate, s1, s2, d }),
                    "mpyr" => Some(Mpyr { negate, s1, s2, d }),
                    "mac" => Some(Mac { negate, s1, s2, d }),
                    "macr" => Some(Macr { negate, s1, s2, d }),
                    _ => unreachable!(),
                }
            }

            _ => None,
        }
    }

    /// Whether this is a logical or shift operation (writes only A1/B1, not full accumulator).
    pub fn is_logical_or_shift(&self) -> bool {
        use ParallelAlu::*;
        matches!(
            self,
            And { .. }
                | Or { .. }
                | Eor { .. }
                | Not { .. }
                | Asl { .. }
                | Asr { .. }
                | Lsl { .. }
                | Lsr { .. }
                | Rol { .. }
                | Ror { .. }
        )
    }
}

/// Encode a register index to the JJ field (2-bit, 0x40-0x7F range).
fn jj_encode(src: usize) -> Option<u8> {
    match src {
        reg::X0 => Some(0),
        reg::Y0 => Some(1),
        reg::X1 => Some(2),
        reg::Y1 => Some(3),
        _ => None,
    }
}

/// Encode (s1, s2) register pair to the QQQ field (3-bit) for parallel multiply.
fn qqq_mpy_encode(s1: usize, s2: usize) -> Option<u8> {
    match (s1, s2) {
        (reg::X0, reg::X0) => Some(0),
        (reg::Y0, reg::Y0) => Some(1),
        (reg::X1, reg::X0) => Some(2),
        (reg::Y1, reg::Y0) => Some(3),
        (reg::X0, reg::Y1) => Some(4),
        (reg::Y0, reg::X0) => Some(5),
        (reg::X1, reg::Y0) => Some(6),
        (reg::Y1, reg::X1) => Some(7),
        _ => None,
    }
}

/// Display name for a register pair used as X or Y source in parallel ALU.
fn xy_pair_name(hi: usize) -> &'static str {
    match hi {
        reg::X1 => "x",
        reg::Y1 => "y",
        _ => unreachable!(),
    }
}

impl std::fmt::Display for ParallelAlu {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        use ParallelAlu::*;

        match *self {
            Move => write!(f, "move"),

            TfrAcc { src, d } => write!(f, "tfr {},{}", acc_name(src), acc_name(d)),
            Addr { src, d } => write!(f, "addr {},{}", acc_name(src), acc_name(d)),
            Tst { d } => write!(f, "tst {}", acc_name(d)),
            CmpAcc { src, d } => write!(f, "cmp {},{}", acc_name(src), acc_name(d)),
            Subr { src, d } => write!(f, "subr {},{}", acc_name(src), acc_name(d)),
            CmpmAcc { src, d } => write!(f, "cmpm {},{}", acc_name(src), acc_name(d)),

            AddAcc { src, d } => write!(f, "add {},{}", acc_name(src), acc_name(d)),
            Rnd { d } => write!(f, "rnd {}", acc_name(d)),
            Addl { src, d } => write!(f, "addl {},{}", acc_name(src), acc_name(d)),
            Clr { d } => write!(f, "clr {}", acc_name(d)),
            SubAcc { src, d } => write!(f, "sub {},{}", acc_name(src), acc_name(d)),
            Max => write!(f, "max a,b"),
            Maxm => write!(f, "maxm a,b"),
            Subl { src, d } => write!(f, "subl {},{}", acc_name(src), acc_name(d)),
            Not { d } => write!(f, "not {}", acc_name(d)),

            AddXY { hi, d, .. } => write!(f, "add {},{}", xy_pair_name(hi), acc_name(d)),
            Adc { hi, d, .. } => write!(f, "adc {},{}", xy_pair_name(hi), acc_name(d)),
            SubXY { hi, d, .. } => write!(f, "sub {},{}", xy_pair_name(hi), acc_name(d)),
            Sbc { hi, d, .. } => write!(f, "sbc {},{}", xy_pair_name(hi), acc_name(d)),
            Asr { d } => write!(f, "asr {}", acc_name(d)),
            Lsr { d } => write!(f, "lsr {}", acc_name(d)),
            Abs { d } => write!(f, "abs {}", acc_name(d)),
            Ror { d } => write!(f, "ror {}", acc_name(d)),
            Asl { d } => write!(f, "asl {}", acc_name(d)),
            Lsl { d } => write!(f, "lsl {}", acc_name(d)),
            Neg { d } => write!(f, "neg {}", acc_name(d)),
            Rol { d } => write!(f, "rol {}", acc_name(d)),

            AddReg { src, d } => write!(f, "add {},{}", REGISTER_NAMES[src], acc_name(d)),
            TfrReg { src, d } => write!(f, "tfr {},{}", REGISTER_NAMES[src], acc_name(d)),
            Or { src, d } => write!(f, "or {},{}", REGISTER_NAMES[src], acc_name(d)),
            Eor { src, d } => write!(f, "eor {},{}", REGISTER_NAMES[src], acc_name(d)),
            SubReg { src, d } => write!(f, "sub {},{}", REGISTER_NAMES[src], acc_name(d)),
            CmpReg { src, d } => write!(f, "cmp {},{}", REGISTER_NAMES[src], acc_name(d)),
            And { src, d } => write!(f, "and {},{}", REGISTER_NAMES[src], acc_name(d)),
            CmpmReg { src, d } => write!(f, "cmpm {},{}", REGISTER_NAMES[src], acc_name(d)),

            Mpy { negate, s1, s2, d } => {
                write!(
                    f,
                    "mpy {}{},{},{}",
                    if negate { "-" } else { "+" },
                    REGISTER_NAMES[s1],
                    REGISTER_NAMES[s2],
                    acc_name(d),
                )
            }
            Mpyr { negate, s1, s2, d } => {
                write!(
                    f,
                    "mpyr {}{},{},{}",
                    if negate { "-" } else { "+" },
                    REGISTER_NAMES[s1],
                    REGISTER_NAMES[s2],
                    acc_name(d),
                )
            }
            Mac { negate, s1, s2, d } => {
                write!(
                    f,
                    "mac {}{},{},{}",
                    if negate { "-" } else { "+" },
                    REGISTER_NAMES[s1],
                    REGISTER_NAMES[s2],
                    acc_name(d),
                )
            }
            Macr { negate, s1, s2, d } => {
                write!(
                    f,
                    "macr {}{},{},{}",
                    if negate { "-" } else { "+" },
                    REGISTER_NAMES[s1],
                    REGISTER_NAMES[s2],
                    acc_name(d),
                )
            }

            Undefined => write!(f, "undefined"),
        }
    }
}

fn acc_name(a: Accumulator) -> &'static str {
    match a {
        Accumulator::A => "a",
        Accumulator::B => "b",
    }
}

/// Parallel move type (bits 23:20 of a parallel instruction).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ParallelMoveType {
    /// No parallel data move
    Pm0 = 0,
    /// Register-to-register
    Pm1 = 1,
    /// Address register update (ea)
    Pm2 = 2,
    /// X memory with short displacement
    Pm3 = 3,
    /// X memory and register
    Pm4 = 4,
    /// XY memory double move
    Pm5 = 5,
    /// Long displacement
    Pm8 = 8,
}

impl ParallelMoveType {
    pub fn from_bits(bits: u32) -> Self {
        match bits {
            0 => Self::Pm0,
            1 => Self::Pm1,
            2 => Self::Pm2,
            3 => Self::Pm3,
            4 => Self::Pm4,
            5..=7 => Self::Pm5,
            8..=15 => Self::Pm8,
            _ => unreachable!(),
        }
    }
}

/// Decoded DSP56300 instruction.
#[derive(Debug, Clone)]
pub enum Instruction {
    // === Parallel move + ALU ===
    Parallel {
        alu: ParallelAlu,
        move_type: ParallelMoveType,
        /// Full 24-bit opcode for field extraction by the emitter
        opcode: u32,
    },

    // === Arithmetic with immediate ===
    AddImm {
        imm: u8,
        d: Accumulator,
    },
    AddLong {
        d: Accumulator,
    },
    SubImm {
        imm: u8,
        d: Accumulator,
    },
    SubLong {
        d: Accumulator,
    },
    CmpImm {
        imm: u8,
        d: Accumulator,
    },
    CmpLong {
        d: Accumulator,
    },
    CmpU {
        src: usize,
        d: Accumulator,
    },
    AndImm {
        imm: u8,
        d: Accumulator,
    },
    AndLong {
        d: Accumulator,
    },
    OrLong {
        d: Accumulator,
    },
    OrImm {
        imm: u8,
        d: Accumulator,
    },
    EorImm {
        imm: u8,
        d: Accumulator,
    },
    EorLong {
        d: Accumulator,
    },
    AndI {
        imm: u8,
        dest: u8,
    },
    OrI {
        imm: u8,
        dest: u8,
    },

    // === Shifts ===
    AslImm {
        shift: u8,
        s: Accumulator,
        d: Accumulator,
    },
    AsrImm {
        shift: u8,
        s: Accumulator,
        d: Accumulator,
    },
    LslImm {
        shift: u8,
        d: Accumulator,
    },
    LsrImm {
        shift: u8,
        d: Accumulator,
    },
    AslReg {
        src: usize,
        s: Accumulator,
        d: Accumulator,
    },
    AsrReg {
        src: usize,
        s: Accumulator,
        d: Accumulator,
    },
    LslReg {
        src: usize,
        d: Accumulator,
    },
    LsrReg {
        src: usize,
        d: Accumulator,
    },

    // === Branches ===
    Bcc {
        cc: CondCode,
        addr: i32,
    },
    BccLong {
        cc: CondCode,
    },
    Bra {
        addr: i32,
    },
    BraLong,
    Bsr {
        addr: i32,
    },
    BsrLong,
    BccRn {
        cc: CondCode,
        rn: u8,
    },
    BraRn {
        rn: u8,
    },
    BsrRn {
        rn: u8,
    },
    Bscc {
        cc: CondCode,
        addr: i32,
    },
    BsccLong {
        cc: CondCode,
    },
    BsccRn {
        cc: CondCode,
        rn: u8,
    },
    Brkcc {
        cc: CondCode,
    },
    Jcc {
        cc: CondCode,
        addr: u32,
    },
    JccEa {
        cc: CondCode,
        ea_mode: u8,
    },
    Jmp {
        addr: u32,
    },
    JmpEa {
        ea_mode: u8,
    },
    Jscc {
        cc: CondCode,
        addr: u32,
    },
    JsccEa {
        cc: CondCode,
        ea_mode: u8,
    },
    Jsr {
        addr: u32,
    },
    JsrEa {
        ea_mode: u8,
    },

    // === Bit manipulation ===
    BchgEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BchgAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BchgPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BchgQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BchgReg {
        reg_idx: u8,
        bit_num: u8,
    },
    BclrEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BclrAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BclrPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BclrQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BclrReg {
        reg_idx: u8,
        bit_num: u8,
    },
    BsetEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BsetAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BsetPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BsetQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BsetReg {
        reg_idx: u8,
        bit_num: u8,
    },
    BtstEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BtstAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BtstPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BtstQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BtstReg {
        reg_idx: u8,
        bit_num: u8,
    },

    // === Bit branch ===
    BrclrEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BrclrAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BrclrPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BrclrQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BrclrReg {
        reg_idx: u8,
        bit_num: u8,
    },
    BrsetEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BrsetAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BrsetPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BrsetQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BrsetReg {
        reg_idx: u8,
        bit_num: u8,
    },

    // === Bit branch to subroutine ===
    BsclrEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BsclrAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BsclrPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BsclrQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BsclrReg {
        reg_idx: u8,
        bit_num: u8,
    },
    BssetEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    BssetAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    BssetPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    BssetQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    BssetReg {
        reg_idx: u8,
        bit_num: u8,
    },

    JclrEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    JclrAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    JclrPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    JclrQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    JclrReg {
        reg_idx: u8,
        bit_num: u8,
    },
    JsetEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    JsetAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    JsetPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    JsetQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    JsetReg {
        reg_idx: u8,
        bit_num: u8,
    },
    JsclrEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    JsclrAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    JsclrPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    JsclrQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    JsclrReg {
        reg_idx: u8,
        bit_num: u8,
    },
    JssetEa {
        space: MemSpace,
        ea_mode: u8,
        bit_num: u8,
    },
    JssetAa {
        space: MemSpace,
        addr: u8,
        bit_num: u8,
    },
    JssetPp {
        space: MemSpace,
        pp_offset: u8,
        bit_num: u8,
    },
    JssetQq {
        space: MemSpace,
        qq_offset: u8,
        bit_num: u8,
    },
    JssetReg {
        reg_idx: u8,
        bit_num: u8,
    },

    // === Loop ===
    DoEa {
        space: MemSpace,
        ea_mode: u8,
    },
    DoAa {
        space: MemSpace,
        addr: u8,
    },
    DoImm {
        count: u16,
    },
    DoReg {
        reg_idx: u8,
    },
    DoForever,
    DorEa {
        space: MemSpace,
        ea_mode: u8,
    },
    DorAa {
        space: MemSpace,
        addr: u8,
    },
    DorImm {
        count: u16,
    },
    DorReg {
        reg_idx: u8,
    },
    DorForever,
    EndDo,
    RepEa {
        space: MemSpace,
        ea_mode: u8,
    },
    RepAa {
        space: MemSpace,
        addr: u8,
    },
    RepImm {
        count: u16,
    },
    RepReg {
        reg_idx: u8,
    },

    // === Move ===
    MoveLongDisp {
        space: MemSpace,
        w: bool,
        offreg_idx: u8,
        numreg: u8,
    },
    MoveShortDisp {
        space: MemSpace,
        offset: u8,
        w: bool,
        offreg_idx: u8,
        numreg: u8,
    },
    MovecEa {
        ea_mode: u8,
        numreg: u8,
        w: bool,
        space: MemSpace,
    },
    MovecAa {
        addr: u8,
        numreg: u8,
        w: bool,
        space: MemSpace,
    },
    MovecReg {
        src_reg: u8,
        dst_reg: u8,
        w: bool,
    },
    MovecImm {
        imm: u8,
        dest: u8,
    },
    MovemEa {
        ea_mode: u8,
        numreg: u8,
        w: bool,
    },
    MovemAa {
        addr: u8,
        numreg: u8,
        w: bool,
    },
    Movep23 {
        pp_offset: u8,
        ea_mode: u8,
        w: bool,
        perspace: MemSpace,
        easpace: MemSpace,
    },
    MovepQq {
        qq_offset: u8,
        ea_mode: u8,
        w: bool,
        qqspace: MemSpace,
        easpace: MemSpace,
    },
    Movep1 {
        pp_offset: u8,
        ea_mode: u8,
        w: bool,
        space: MemSpace,
    },
    Movep0 {
        pp_offset: u8,
        reg_idx: u8,
        w: bool,
        space: MemSpace,
    },
    MovepQqPea {
        qq_offset: u8,
        ea_mode: u8,
        w: bool,
        space: MemSpace,
    },
    MovepQqR {
        qq_offset: u8,
        reg_idx: u8,
        w: bool,
        space: MemSpace,
    },

    // === Multiply ===
    /// MPY/MPYR/MAC/MACR (+/-)S,#n,D -- multiply source by 2^-n
    MulShift {
        op: MulShiftOp,
        shift: u8,
        src: usize,
        d: Accumulator,
        k: bool,
    },
    MpyI {
        k: bool,
        d: Accumulator,
        src: usize,
    },
    MpyrI {
        k: bool,
        d: Accumulator,
        src: usize,
    },
    MacI {
        k: bool,
        d: Accumulator,
        src: usize,
    },
    MacrI {
        k: bool,
        d: Accumulator,
        src: usize,
    },
    Dmac {
        ss: u8,
        k: bool,
        d: Accumulator,
        s1: usize,
        s2: usize,
    },
    MacSU {
        s: u8,
        k: bool,
        d: Accumulator,
        s1: usize,
        s2: usize,
    },
    MpySU {
        s: u8,
        k: bool,
        d: Accumulator,
        s1: usize,
        s2: usize,
    },
    Div {
        src: usize,
        d: Accumulator,
    },

    // === Address ===
    Lua {
        ea_mode: u8,
        dst_reg: u8,
    },
    LuaRel {
        aa: u8,
        addr_reg: u8,
        dst_reg: u8,
        dest_is_n: bool,
    },
    LraRn {
        addr_reg: u8,
        dst_reg: u8,
    },
    LraDisp {
        dst_reg: u8,
    },
    Norm {
        rreg_idx: u8,
        d: Accumulator,
    },

    // === Transfer conditional ===
    Tcc {
        cc: CondCode,
        acc: Option<(usize, usize)>,
        r: Option<(u8, u8)>,
    },

    // === Misc ===
    Nop,
    Dec {
        d: Accumulator,
    },
    Inc {
        d: Accumulator,
    },
    Illegal,
    Reset,
    Rti,
    Rts,
    Stop,
    Wait,

    // === Tier 3: Specialized ===
    Clb {
        s: Accumulator,
        d: Accumulator,
    },
    Normf {
        src: usize,
        d: Accumulator,
    },
    Debug,
    Debugcc {
        cc: CondCode,
    },
    Trap,
    Trapcc {
        cc: CondCode,
    },

    // Bit-field manipulation
    Merge {
        src: usize,
        d: Accumulator,
    },
    ExtractReg {
        s1: usize,
        s2: Accumulator,
        d: Accumulator,
    },
    ExtractImm {
        s2: Accumulator,
        d: Accumulator,
    },
    ExtractuReg {
        s1: usize,
        s2: Accumulator,
        d: Accumulator,
    },
    ExtractuImm {
        s2: Accumulator,
        d: Accumulator,
    },
    InsertReg {
        s1: usize,
        s2: usize,
        d: Accumulator,
    },
    InsertImm {
        s2: usize,
        d: Accumulator,
    },

    // Viterbi
    Vsl {
        s: Accumulator,
        ea_mode: u8,
        i_bit: u8,
    },

    // Cache control (treated as NOPs)
    Pflush,
    Pflushun,
    Pfree,
    PlockEa {
        ea_mode: u8,
    },
    Plockr,
    PunlockEa {
        ea_mode: u8,
    },
    Punlockr,

    /// Instruction recognized but not yet implemented.
    Unimplemented {
        name: &'static str,
        opcode: u32,
    },

    /// Unknown/invalid encoding.
    Unknown {
        opcode: u32,
    },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parallel_alu_exhaustive_roundtrip() {
        let undefined_bytes = [0x04, 0x08, 0x0C];
        for b in 0..=255u8 {
            let decoded = decode_parallel_alu(b);
            if undefined_bytes.contains(&b) {
                assert!(
                    matches!(decoded, ParallelAlu::Undefined),
                    "byte 0x{b:02X}: expected Undefined, got {decoded:?}"
                );
                assert_eq!(decoded.encode(), None);
            } else {
                assert!(
                    !matches!(decoded, ParallelAlu::Undefined),
                    "byte 0x{b:02X}: unexpected Undefined"
                );
                assert_eq!(
                    decoded.encode(),
                    Some(b),
                    "byte 0x{b:02X}: encode roundtrip failed (decoded as {decoded:?})"
                );
            }
        }
    }

    #[test]
    fn test_parallel_alu_display_all() {
        // Full 256-entry display table, 8 columns matching DSP56300FM Tables 12-19 through 12-21.
        // Non-multiply rows: one row per JJJ+D combination (kkk=0..7).
        // Multiply rows: one row per QQQ+d combination (kkk=0..7).
        #[rustfmt::skip]
        const EXPECTED: [&str; 256] = [
            "move",       "tfr b,a",   "addr b,a",  "tst a",     "undefined",  "cmp b,a",    "subr b,a",  "cmpm b,a",  // 0x00-0x07: JJJ=000, D=a
            "undefined",  "tfr a,b",   "addr a,b",  "tst b",     "undefined",  "cmp a,b",    "subr a,b",  "cmpm a,b",  // 0x08-0x0F: JJJ=000, D=b
            "add b,a",    "rnd a",     "addl b,a",  "clr a",     "sub b,a",    "maxm a,b",   "subl b,a",  "not a",     // 0x10-0x17: JJJ=001, D=a
            "add a,b",    "rnd b",     "addl a,b",  "clr b",     "sub a,b",    "max a,b",    "subl a,b",  "not b",     // 0x18-0x1F: JJJ=001, D=b
            "add x,a",    "adc x,a",   "asr a",     "lsr a",     "sub x,a",    "sbc x,a",    "abs a",     "ror a",     // 0x20-0x27: JJJ=010, D=a
            "add x,b",    "adc x,b",   "asr b",     "lsr b",     "sub x,b",    "sbc x,b",    "abs b",     "ror b",     // 0x28-0x2F: JJJ=010, D=b
            "add y,a",    "adc y,a",   "asl a",     "lsl a",     "sub y,a",    "sbc y,a",    "neg a",     "rol a",     // 0x30-0x37: JJJ=011, D=a
            "add y,b",    "adc y,b",   "asl b",     "lsl b",     "sub y,b",    "sbc y,b",    "neg b",     "rol b",     // 0x38-0x3F: JJJ=011, D=b
            "add x0,a",   "tfr x0,a",  "or x0,a",   "eor x0,a",  "sub x0,a",   "cmp x0,a",   "and x0,a",  "cmpm x0,a", // 0x40-0x47: JJ=00 X0, D=a
            "add x0,b",   "tfr x0,b",  "or x0,b",   "eor x0,b",  "sub x0,b",   "cmp x0,b",   "and x0,b",  "cmpm x0,b", // 0x48-0x4F: JJ=00 X0, D=b
            "add y0,a",   "tfr y0,a",  "or y0,a",   "eor y0,a",  "sub y0,a",   "cmp y0,a",   "and y0,a",  "cmpm y0,a", // 0x50-0x57: JJ=01 Y0, D=a
            "add y0,b",   "tfr y0,b",  "or y0,b",   "eor y0,b",  "sub y0,b",   "cmp y0,b",   "and y0,b",  "cmpm y0,b", // 0x58-0x5F: JJ=01 Y0, D=b
            "add x1,a",   "tfr x1,a",  "or x1,a",   "eor x1,a",  "sub x1,a",   "cmp x1,a",   "and x1,a",  "cmpm x1,a", // 0x60-0x67: JJ=10 X1, D=a
            "add x1,b",   "tfr x1,b",  "or x1,b",   "eor x1,b",  "sub x1,b",   "cmp x1,b",   "and x1,b",  "cmpm x1,b", // 0x68-0x6F: JJ=10 X1, D=b
            "add y1,a",   "tfr y1,a",  "or y1,a",   "eor y1,a",  "sub y1,a",   "cmp y1,a",   "and y1,a",  "cmpm y1,a", // 0x70-0x77: JJ=11 Y1, D=a
            "add y1,b",   "tfr y1,b",  "or y1,b",   "eor y1,b",  "sub y1,b",   "cmp y1,b",   "and y1,b",  "cmpm y1,b", // 0x78-0x7F: JJ=11 Y1, D=b
            "mpy +x0,x0,a",  "mpyr +x0,x0,a",  "mac +x0,x0,a",  "macr +x0,x0,a",  "mpy -x0,x0,a",  "mpyr -x0,x0,a",  "mac -x0,x0,a",  "macr -x0,x0,a", // 0x80-0x87: QQQ=000 x0*x0, d=a
            "mpy +x0,x0,b",  "mpyr +x0,x0,b",  "mac +x0,x0,b",  "macr +x0,x0,b",  "mpy -x0,x0,b",  "mpyr -x0,x0,b",  "mac -x0,x0,b",  "macr -x0,x0,b", // 0x88-0x8F: QQQ=000 x0*x0, d=b
            "mpy +y0,y0,a",  "mpyr +y0,y0,a",  "mac +y0,y0,a",  "macr +y0,y0,a",  "mpy -y0,y0,a",  "mpyr -y0,y0,a",  "mac -y0,y0,a",  "macr -y0,y0,a", // 0x90-0x97: QQQ=001 y0*y0, d=a
            "mpy +y0,y0,b",  "mpyr +y0,y0,b",  "mac +y0,y0,b",  "macr +y0,y0,b",  "mpy -y0,y0,b",  "mpyr -y0,y0,b",  "mac -y0,y0,b",  "macr -y0,y0,b", // 0x98-0x9F: QQQ=001 y0*y0, d=b
            "mpy +x1,x0,a",  "mpyr +x1,x0,a",  "mac +x1,x0,a",  "macr +x1,x0,a",  "mpy -x1,x0,a",  "mpyr -x1,x0,a",  "mac -x1,x0,a",  "macr -x1,x0,a", // 0xA0-0xA7: QQQ=010 x1*x0, d=a
            "mpy +x1,x0,b",  "mpyr +x1,x0,b",  "mac +x1,x0,b",  "macr +x1,x0,b",  "mpy -x1,x0,b",  "mpyr -x1,x0,b",  "mac -x1,x0,b",  "macr -x1,x0,b", // 0xA8-0xAF: QQQ=010 x1*x0, d=b
            "mpy +y1,y0,a",  "mpyr +y1,y0,a",  "mac +y1,y0,a",  "macr +y1,y0,a",  "mpy -y1,y0,a",  "mpyr -y1,y0,a",  "mac -y1,y0,a",  "macr -y1,y0,a", // 0xB0-0xB7: QQQ=011 y1*y0, d=a
            "mpy +y1,y0,b",  "mpyr +y1,y0,b",  "mac +y1,y0,b",  "macr +y1,y0,b",  "mpy -y1,y0,b",  "mpyr -y1,y0,b",  "mac -y1,y0,b",  "macr -y1,y0,b", // 0xB8-0xBF: QQQ=011 y1*y0, d=b
            "mpy +x0,y1,a",  "mpyr +x0,y1,a",  "mac +x0,y1,a",  "macr +x0,y1,a",  "mpy -x0,y1,a",  "mpyr -x0,y1,a",  "mac -x0,y1,a",  "macr -x0,y1,a", // 0xC0-0xC7: QQQ=100 x0*y1, d=a
            "mpy +x0,y1,b",  "mpyr +x0,y1,b",  "mac +x0,y1,b",  "macr +x0,y1,b",  "mpy -x0,y1,b",  "mpyr -x0,y1,b",  "mac -x0,y1,b",  "macr -x0,y1,b", // 0xC8-0xCF: QQQ=100 x0*y1, d=b
            "mpy +y0,x0,a",  "mpyr +y0,x0,a",  "mac +y0,x0,a",  "macr +y0,x0,a",  "mpy -y0,x0,a",  "mpyr -y0,x0,a",  "mac -y0,x0,a",  "macr -y0,x0,a", // 0xD0-0xD7: QQQ=101 y0*x0, d=a
            "mpy +y0,x0,b",  "mpyr +y0,x0,b",  "mac +y0,x0,b",  "macr +y0,x0,b",  "mpy -y0,x0,b",  "mpyr -y0,x0,b",  "mac -y0,x0,b",  "macr -y0,x0,b", // 0xD8-0xDF: QQQ=101 y0*x0, d=b
            "mpy +x1,y0,a",  "mpyr +x1,y0,a",  "mac +x1,y0,a",  "macr +x1,y0,a",  "mpy -x1,y0,a",  "mpyr -x1,y0,a",  "mac -x1,y0,a",  "macr -x1,y0,a", // 0xE0-0xE7: QQQ=110 x1*y0, d=a
            "mpy +x1,y0,b",  "mpyr +x1,y0,b",  "mac +x1,y0,b",  "macr +x1,y0,b",  "mpy -x1,y0,b",  "mpyr -x1,y0,b",  "mac -x1,y0,b",  "macr -x1,y0,b", // 0xE8-0xEF: QQQ=110 x1*y0, d=b
            "mpy +y1,x1,a",  "mpyr +y1,x1,a",  "mac +y1,x1,a",  "macr +y1,x1,a",  "mpy -y1,x1,a",  "mpyr -y1,x1,a",  "mac -y1,x1,a",  "macr -y1,x1,a", // 0xF0-0xF7: QQQ=111 y1*x1, d=a
            "mpy +y1,x1,b",  "mpyr +y1,x1,b",  "mac +y1,x1,b",  "macr +y1,x1,b",  "mpy -y1,x1,b",  "mpyr -y1,x1,b",  "mac -y1,x1,b",  "macr -y1,x1,b", // 0xF8-0xFF: QQQ=111 y1*x1, d=b
        ];

        for (i, expected) in EXPECTED.iter().enumerate() {
            assert_eq!(
                decode_parallel_alu(i as u8).to_string(),
                *expected,
                "byte 0x{i:02X}"
            );
        }
    }

    #[test]
    fn test_parallel_alu_from_text_roundtrip() {
        for b in 0..=255u8 {
            let alu = decode_parallel_alu(b);
            if matches!(alu, ParallelAlu::Undefined) {
                continue;
            }
            let text = alu.to_string();
            let parsed = ParallelAlu::from_text(&text).unwrap_or_else(|| {
                panic!("from_text failed for {:?} (0x{b:02X}): \"{text}\"", alu)
            });
            assert_eq!(
                parsed, alu,
                "from_text roundtrip mismatch for 0x{b:02X}: \"{text}\" -> {parsed:?} != {alu:?}"
            );
        }
    }
}
