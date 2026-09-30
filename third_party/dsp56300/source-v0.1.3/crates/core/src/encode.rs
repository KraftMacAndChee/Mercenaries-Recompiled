//! Instruction encoding primitives for DSP56300.
//!
//! All functions take primitive types (u32, u8, bool) -- no AST dependency.
//! The assembler crate extracts fields from its AST and delegates here.

use crate::MemSpace;

/// Assembled output: one or two 24-bit words.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EncodedInstruction {
    pub word0: u32,
    pub word1: Option<u32>,
}

/// Encoding error.
#[derive(Debug)]
pub struct EncodeError {
    pub msg: String,
}

impl std::fmt::Display for EncodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "encode error: {}", self.msg)
    }
}

impl std::error::Error for EncodeError {}

pub type Result<T> = std::result::Result<T, EncodeError>;

pub fn enc_err(msg: &str) -> EncodeError {
    EncodeError {
        msg: msg.to_string(),
    }
}

// ---- Constructors ----

/// Single-word instruction.
pub fn word(w0: u32) -> EncodedInstruction {
    EncodedInstruction {
        word0: w0 & 0xFFFFFF,
        word1: None,
    }
}

/// Two-word instruction.
pub fn words(w0: u32, w1: u32) -> EncodedInstruction {
    EncodedInstruction {
        word0: w0 & 0xFFFFFF,
        word1: Some(w1 & 0xFFFFFF),
    }
}

// ---- Pure bit-packing helpers ----

/// Memory space bit: X=0, Y=1.
pub fn space_bit(space: MemSpace) -> u32 {
    match space {
        MemSpace::X => 0,
        MemSpace::Y => 1,
        MemSpace::P => 0,
    }
}

/// Encode a 9-bit relative address split across bits 9:6 and 4:0 (bit 5 = 0).
/// Template pattern: `aaaa0aaaaa` at bits 9:0.
pub fn encode_rel9(rel9: u32) -> u32 {
    ((rel9 & 0x1E0) << 1) | (rel9 & 0x1F)
}

/// Encode a 6-bit EA mode field from mode enum value and register number.
/// EA modes (MMM/RRR encoding):
/// - 0b000_rrr: (Rn)-Nn
/// - 0b001_rrr: (Rn)+Nn
/// - 0b010_rrr: (Rn)-
/// - 0b011_rrr: (Rn)+
/// - 0b100_rrr: (Rn)
/// - 0b101_rrr: (Rn+Nn)
/// - 0b110_000: absolute address (extension word)
/// - 0b110_100: immediate (extension word)
/// - 0b111_rrr: -(Rn)
pub fn encode_ea_mode(mode: u8, reg: u8) -> u8 {
    (mode << 3) | (reg & 7)
}

/// Pack a 12-bit loop count into the split iiiiiiii/hhhh encoding.
/// Returns (lo8, hi4) for the template `iiiiiiii...hhhh`.
pub fn pack_loop_count(count: u32) -> (u32, u32) {
    (count & 0xFF, (count >> 8) & 0xF)
}

/// Encode Pm4/Pm5 register value split: 5-bit register index split into
/// val_hi (bits 4:3) and val_lo (bits 2:0).
pub fn pm4_reg_split(reg_idx: u32) -> (u32, u32) {
    let v = reg_idx & 0x1F;
    ((v >> 3) & 3, v & 7)
}

/// Encode L-move register index from the REGISTERS_LMOVE table index (0-7).
/// Returns (bit2, bits10) for the split encoding.
pub fn l_reg_split(lreg_idx: u32) -> (u32, u32) {
    ((lreg_idx >> 2) & 1, lreg_idx & 3)
}

/// Encode MoveXYImm offset: 7-bit signed offset split into hi6 and lo1.
/// Returns (xxx_hi, xxx_lo) where xxx = ((op >> 11) & 0x3F) << 1 | ((op >> 6) & 1).
pub fn pack_xy_imm_offset(off7: u32) -> (u32, u32) {
    ((off7 >> 1) & 0x3F, off7 & 1)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::MemSpace;

    #[test]
    fn test_space_bit() {
        assert_eq!(space_bit(MemSpace::X), 0);
        assert_eq!(space_bit(MemSpace::Y), 1);
        assert_eq!(space_bit(MemSpace::P), 0);
    }

    #[test]
    fn test_encode_rel9() {
        // All lower 5 bits set, upper 4 clear -> value passes through unchanged.
        assert_eq!(encode_rel9(0x1F), 0x1F);
        // All upper 4 bits set (bits 8..5), lower 5 clear -> shifted up by 1.
        assert_eq!(encode_rel9(0x1E0), 0x3C0);
        // All 9 bits set -> lower 5 in place, upper 4 shifted up by 1 (bit 5 = 0).
        assert_eq!(encode_rel9(0x1FF), 0x3DF);
        // Zero round-trips.
        assert_eq!(encode_rel9(0), 0);
    }

    #[test]
    fn test_encode_ea_mode() {
        assert_eq!(encode_ea_mode(0, 0), 0b000_000);
        assert_eq!(encode_ea_mode(3, 2), 0b011_010);
        assert_eq!(encode_ea_mode(7, 7), 0b111_111);
        // reg is masked to 3 bits.
        assert_eq!(encode_ea_mode(1, 0b1001), 0b001_001);
    }
}
