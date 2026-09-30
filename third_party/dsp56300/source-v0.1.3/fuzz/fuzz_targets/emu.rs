#![no_main]

use std::cell::RefCell;

use libfuzzer_sys::fuzz_target;
use dsp56300_emu::core::{DspState, MemoryMap, MemoryRegion, RegionKind, reg};
use dsp56300_emu::jit::JitEngine;

const PRAM_SIZE: usize = 32;
const XRAM_SIZE: usize = 256;
const YRAM_SIZE: usize = 256;

/// Reset interval: replace the Cranelift module every N iterations to
/// bound mmap'd code memory growth while keeping the instruction cache
/// warm for repeated opcodes.
const MODULE_RESET_INTERVAL: u32 = 256;

thread_local! {
    static JIT: RefCell<(JitEngine, u32)> = RefCell::new((JitEngine::new(PRAM_SIZE), 0));
}

/// Input format:
///   Bytes 0..1:   flags (u16 LE) -- bit 0: use execute_one vs run()
///   Bytes 2..5:   SR (u32 LE, masked to safe bits)
///   Bytes 6..8:   A1 (24-bit LE)
///   Bytes 9..11:  B1 (24-bit LE)
///   Bytes 12..14: X0 (24-bit LE)
///   Bytes 15..17: Y0 (24-bit LE)
///   Bytes 18..20: R0 (24-bit LE)
///   Bytes 21..23: N0 (24-bit LE)
///   Bytes 24..26: M0 (24-bit LE, 0 = keep default 0xFFFF)
///   Bytes 27:     SP (low nibble, 0..15)
///   Bytes 28:     LC (u8)
///   Bytes 29..N:  PRAM words (3 bytes each, up to 16)
const HEADER_SIZE: usize = 29;

// Disable ASAN leak detection: Cranelift's JIT module uses mmap'd memory
// that ASAN's leak checker incorrectly flags as a leak.
#[unsafe(no_mangle)]
pub extern "C" fn __asan_default_options() -> *const u8 {
    b"detect_leaks=0\0".as_ptr()
}

fn read_u24_le(data: &[u8], off: usize) -> u32 {
    (data[off] as u32) | ((data[off + 1] as u32) << 8) | ((data[off + 2] as u32) << 16)
}

fuzz_target!(|data: &[u8]| {
    if data.len() < HEADER_SIZE + 3 {
        return;
    }

    // Parse header
    let flags = u16::from_le_bytes([data[0], data[1]]);
    let use_execute_one = flags & 1 != 0;

    // Mask SR to safe bits: CCR (0-7), S0/S1 (10-11), LF (15).
    // Leave I0/I1 (interrupt mask) and T (trace) at 0.
    let sr_raw = u32::from_le_bytes([data[2], data[3], data[4], data[5]]);
    let sr_val = sr_raw & 0x8CFF;

    let a1 = read_u24_le(data, 6);
    let b1 = read_u24_le(data, 9);
    let x0 = read_u24_le(data, 12);
    let y0 = read_u24_le(data, 15);
    let r0 = read_u24_le(data, 18);
    let n0 = read_u24_le(data, 21);
    let m0_raw = read_u24_le(data, 24);
    let sp = (data[27] & 0x0F) as u32;
    let lc = data[28] as u32;

    // Parse PRAM words
    let pram_data = &data[HEADER_SIZE..];
    let word_count = (pram_data.len() / 3).min(16);

    let mut xram = vec![0u32; XRAM_SIZE];
    let mut yram = vec![0u32; YRAM_SIZE];
    let mut pram = vec![0u32; PRAM_SIZE];

    for i in 0..word_count {
        pram[i] = read_u24_le(pram_data, i * 3);
    }

    let map = MemoryMap {
        x_regions: vec![MemoryRegion {
            start: 0,
            end: XRAM_SIZE as u32,
            kind: RegionKind::Buffer {
                base: xram.as_mut_ptr(),
                offset: 0,
            },
        }],
        y_regions: vec![MemoryRegion {
            start: 0,
            end: YRAM_SIZE as u32,
            kind: RegionKind::Buffer {
                base: yram.as_mut_ptr(),
                offset: 0,
            },
        }],
        p_regions: vec![MemoryRegion {
            start: 0,
            end: PRAM_SIZE as u32,
            kind: RegionKind::Buffer {
                base: pram.as_mut_ptr(),
                offset: 0,
            },
        }],
    };

    let mut state = DspState::new(map);

    // Apply fuzzer-controlled register state
    state.registers[reg::SR] = sr_val;
    state.registers[reg::A1] = a1;
    state.registers[reg::B1] = b1;
    state.registers[reg::X0] = x0;
    state.registers[reg::Y0] = y0;
    state.registers[reg::R0] = r0;
    state.registers[reg::N0] = n0;
    if m0_raw != 0 {
        state.registers[reg::M0] = m0_raw;
    }
    state.registers[reg::SP] = sp;
    state.registers[reg::LC] = lc;

    JIT.with(|cell| {
        let (jit, count) = &mut *cell.borrow_mut();
        *count += 1;
        if *count % MODULE_RESET_INTERVAL == 0 {
            jit.invalidate_cache();
        } else {
            jit.invalidate_blocks();
        }
        if use_execute_one {
            let mut total = 0i32;
            while total < 100 {
                total += state.execute_one(jit);
            }
        } else {
            state.run(jit, 100);
        }
    });
});
