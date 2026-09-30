use std::hint::black_box;
use std::time::Instant;

use dsp56300_core::reg;
use dsp56300_emu::core::{DspState, MemoryMap};
use dsp56300_emu::jit::JitEngine;

const PRAM_SIZE: usize = 4096;
const XRAM_SIZE: usize = 4096;
const YRAM_SIZE: usize = 4096;

const SIXCOMB_LOD: &str = include_str!("../tests/data/sixcomb.lod");
const HF_COMP: u32 = 0x40;
const SENTINEL: u32 = 0x0FFF;

fn load_lod(pram: &mut [u32], xram: &mut [u32], yram: &mut [u32], text: &str) {
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

fn setup_sixcomb() -> (
    JitEngine,
    [u32; XRAM_SIZE],
    [u32; YRAM_SIZE],
    [u32; PRAM_SIZE],
) {
    let jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    load_lod(&mut pram, &mut xram, &mut yram, SIXCOMB_LOD);
    pram[SENTINEL as usize] = 0x000087; // WAIT
    (jit, xram, yram, pram)
}

fn init_state(s: &mut DspState) {
    s.registers[reg::SR] = 0xC00300;
    s.stack_push(SENTINEL, 0);
    s.pc = HF_COMP;
}

fn bench<F: FnMut() -> T, T>(name: &str, iters: u32, mut f: F) {
    for _ in 0..3 {
        black_box(f());
    }
    let start = Instant::now();
    for _ in 0..iters {
        black_box(f());
    }
    let elapsed = start.elapsed();
    let per_iter = elapsed / iters;
    println!("{name}: {per_iter:?}/iter ({iters} iters, {elapsed:?} total)");
}

fn main() {
    println!("dsp56300 JIT benchmarks");
    println!("=======================\n");

    bench("jit_engine_new", 100, || JitEngine::new(PRAM_SIZE));

    // Cold compilation of sixcomb hf_comp
    bench("compile_cold", 100, || {
        let (mut jit, mut xram, mut yram, mut pram) = setup_sixcomb();
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        init_state(&mut s);
        s.run(&mut jit, 1000);
    });

    // Warm execution of sixcomb hf_comp (blocks already compiled)
    {
        let (mut jit, mut xram, mut yram, mut pram) = setup_sixcomb();
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        init_state(&mut s);
        s.run(&mut jit, 1000);

        bench("execute_warm_1k_cycles", 10000, || {
            s.pc = HF_COMP;
            s.stack_push(SENTINEL, 0);
            s.power_state = dsp56300_emu::core::PowerState::Normal;
            s.run(&mut jit, 1000);
        });
    }

    // Single instruction cold compile + execute
    bench("execute_one_cold", 10000, || {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        pram[0] = 0x200013; // clr a
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        s.execute_one(&mut jit);
    });

    // Same opcode at different PC — tests PC-independent cache sharing
    {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        pram[0] = 0x200013; // clr a at PC=0
        pram[100] = 0x200013; // clr a at PC=100 (same opcode, same next_word=0)
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        s.execute_one(&mut jit); // compile for PC=0
        s.pc = 100;
        let start = Instant::now();
        s.execute_one(&mut jit); // PC=100: should be cache hit
        let elapsed = start.elapsed();
        println!("execute_one_shared_cache: {elapsed:?} (same opcode at different PC)");
    }

    // Single instruction cached execute
    {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        pram[0] = 0x200013; // clr a
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        s.execute_one(&mut jit);

        bench("execute_one_warm", 100000, || {
            s.pc = 0;
            s.execute_one(&mut jit);
        });
    }

    // Cache invalidation + recompilation
    {
        let (mut jit, mut xram, mut yram, mut pram) = setup_sixcomb();
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        init_state(&mut s);
        s.run(&mut jit, 1000);

        bench("invalidate_recompile", 100, || {
            jit.invalidate_blocks();
            s.pc = HF_COMP;
            s.stack_push(SENTINEL, 0);
            s.power_state = dsp56300_emu::core::PowerState::Normal;
            s.run(&mut jit, 1000);
        });
    }

    // Breakdown analysis
    {
        let (mut jit, mut xram, mut yram, mut pram) = setup_sixcomb();
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        init_state(&mut s);

        let start = Instant::now();
        s.run(&mut jit, 1000);
        let total = start.elapsed();

        let block_count = jit.block_count();
        let instr_count = jit.instr_cache_count();
        println!("\ncompile_cold breakdown:");
        println!("  blocks compiled: {block_count}");
        println!("  instructions cached: {instr_count}");
        println!("  total: {total:?}");
        if block_count > 0 {
            println!("  per block: {:?}", total / block_count as u32);
        }
        if instr_count > 0 {
            println!("  per instruction: {:?}", total / instr_count as u32);
        }
        println!("  block sizes (start..end, words):");
        for (start, end, size) in jit.block_sizes() {
            println!("    ${start:04X}..${end:04X}: {size} words");
        }
    }
}
