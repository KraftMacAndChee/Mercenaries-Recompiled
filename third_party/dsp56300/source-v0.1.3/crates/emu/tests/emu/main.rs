#![allow(unused_assignments)] // test arrays are read via raw pointers stored in DspState
use dsp56300_emu::core::{
    DspState, InterruptState, MemSpace, MemoryMap, MemoryRegion, PERIPH_BASE, PERIPH_SIZE,
    PowerState, PramDirtyBitmap, REG_MASKS, RegionKind, interrupt, jit_write_mem, reg, sr,
};
use dsp56300_emu::jit::JitEngine;

pub const PRAM_SIZE: usize = 4096;
pub const XRAM_SIZE: usize = 4096;
pub const YRAM_SIZE: usize = 2048;

pub fn run_one(state: &mut DspState, jit: &mut JitEngine) -> i32 {
    state.execute_one(jit)
}

/// Load an a56-format program file into memory arrays.
pub fn load_a56_program(pram: &mut [u32], xram: &mut [u32], yram: &mut [u32], text: &str) {
    for line in text.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 3 {
            continue;
        }
        let Ok(addr) = u32::from_str_radix(parts[1], 16).map(|a| a as usize) else {
            continue;
        };
        let Ok(val) = u32::from_str_radix(parts[2], 16) else {
            continue;
        };
        match parts[0] {
            "P" if addr < pram.len() => pram[addr] = val,
            "X" if addr < xram.len() => xram[addr] = val,
            "Y" if addr < yram.len() => yram[addr] = val,
            _ => {}
        }
    }
}

mod agu;
mod alu;
mod bitops;
mod control;
mod infra;
mod logical;
mod loops;
mod moves;
mod sixcomb;
