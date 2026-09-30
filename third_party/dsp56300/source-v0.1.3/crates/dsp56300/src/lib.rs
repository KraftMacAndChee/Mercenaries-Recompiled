//! DSP56300 assembler, disassembler, and JIT emulator.
//!
//! This is a convenience crate that re-exports the individual dsp56300 crates:
//!
//! - [`core`] - ISA definitions, instruction decoder/encoder, register constants
//! - [`asm`] - Assembler (source to machine code)
//! - [`disasm`] - Disassembler (machine code to text)
//! - [`emu`] - JIT emulator (Cranelift-based)

pub use dsp56300_asm as asm;
pub use dsp56300_core as core;
pub use dsp56300_disasm as disasm;
pub use dsp56300_emu as emu;
