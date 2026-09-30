//! DSP56300 emulator with a JIT execution engine.
//!
//! Manages the Cranelift JIT module, block cache, and run loop. Provides
//! compilation and execution of individual instructions and basic blocks.

use std::collections::HashMap;
use std::io::Write;

use cranelift_codegen::ir::{AbiParam, types};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::Module;

use crate::core::{DspState, InterruptState, MemoryMap, PowerState, REG_MASKS, interrupt, reg, sr};
use crate::emit::Emitter;
use dsp56300_core::{Instruction, decode, mask_pc};

/// Maximum instructions per basic block.
const MAX_BLOCK_LEN: u32 = 128;

/// Compiled function signature: takes a pointer to DspState, returns cycles.
type CompiledFn = unsafe fn(*mut DspState) -> i32;

/// A compiled basic block.
#[derive(Clone, Copy)]
struct CompiledBlock {
    func: CompiledFn,
    /// PC after the last instruction in this block (exclusive end of code range).
    end_pc: u32,
    /// PRAM generation at compilation time. When this matches the current
    /// pram_dirty.generation, the dirty bitmap scan is skipped (the block is known clean).
    generation: u32,
}

/// Code cache: flat array indexed by start_pc.
///
/// A flat array eliminates all HashMap hashing overhead that was the dominant
/// cost in the run loop (SipHash on every lookup).
///
/// When a DO loop boundary (stop_pc < cached block's end_pc) would truncate a
/// block, the entry is evicted and recompiled with the tighter boundary.
struct CodeCache {
    blocks: Vec<Option<CompiledBlock>>,
}

impl CodeCache {
    fn new(pram_size: usize) -> Self {
        Self {
            blocks: vec![None; pram_size],
        }
    }

    /// Invalidate all cached blocks (e.g. when P-memory changes).
    fn invalidate_all(&mut self) {
        self.blocks.fill(None);
    }

    /// Invalidate only blocks whose code range [start_pc, end_pc) overlaps [lo, hi].
    fn invalidate_range(&mut self, lo: u32, hi: u32) {
        let lo = lo as usize;
        let hi = (hi as usize).min(self.blocks.len().saturating_sub(1));
        for pc in 0..self.blocks.len() {
            if let Some(block) = &self.blocks[pc]
                && pc <= hi
                && (block.end_pc as usize) > lo
            {
                self.blocks[pc] = None;
            }
        }
    }
}

/// JIT compilation engine.
pub struct JitEngine {
    module: Option<JITModule>,
    ctx: cranelift_codegen::Context,
    func_ctx: FunctionBuilderContext,
    ptr_ty: cranelift_codegen::ir::Type,
    cache: CodeCache,
    /// Cache for single-instruction compilation (used by execute_one).
    /// Key: (pc, opcode, next_word) -> (compiled function, instruction length).
    instr_cache: HashMap<(u32, u32, u32), (CompiledFn, u32)>,
    /// Perf map file for profiling JIT blocks with `perf record` (Linux only).
    #[cfg(target_os = "linux")]
    perf_map: Option<std::fs::File>,
    /// Number of PRAM words (determines cache and profile array sizes).
    pram_size: usize,
    /// Block execution profiler: \[hit_count, total_cycles\] per PC.
    block_profile: Option<Vec<(u64, u64)>>,
    /// Platform-specific width mask for R/N/M address registers.
    address_register_mask: u32,
}

impl JitEngine {
    pub fn new(pram_size: usize) -> Self {
        Self::new_with_address_register_mask(pram_size, REG_MASKS[reg::R0])
    }

    pub fn new_with_address_register_mask(
        pram_size: usize,
        address_register_mask: u32,
    ) -> Self {
        let module = Self::new_module();
        let ptr_ty = module.isa().pointer_type();
        let ctx = module.make_context();
        let func_ctx = FunctionBuilderContext::new();

        Self {
            module: Some(module),
            ctx,
            func_ctx,
            ptr_ty,
            cache: CodeCache::new(pram_size),
            instr_cache: HashMap::new(),
            #[cfg(target_os = "linux")]
            perf_map: None,
            pram_size,
            block_profile: None,
            address_register_mask,
        }
    }

    /// Create a fresh Cranelift JIT module.
    fn new_module() -> JITModule {
        let mut flag_builder = settings::builder();
        let _ = flag_builder.set("opt_level", "none");
        let _ = flag_builder.set("enable_verifier", "false");
        let _ = flag_builder.set("unwind_info", "false");
        let _ = flag_builder.set("regalloc_algorithm", "single_pass");
        let isa_builder = cranelift_native::builder().unwrap();
        let isa = isa_builder
            .finish(settings::Flags::new(flag_builder))
            .unwrap();
        let builder = JITBuilder::with_isa(isa, cranelift_module::default_libcall_names());
        JITModule::new(builder)
    }

    /// Enable perf map output for `perf record` profiling (Linux only).
    /// Creates `/tmp/perf-<pid>.map`.
    #[cfg(target_os = "linux")]
    pub fn enable_perf_map(&mut self) {
        if self.perf_map.is_none() {
            self.perf_map =
                std::fs::File::create(format!("/tmp/perf-{}.map", std::process::id())).ok();
        }
    }

    /// No-op on non-Linux platforms.
    #[cfg(not(target_os = "linux"))]
    pub fn enable_perf_map(&mut self) {}

    /// Enable block execution profiling (hit counts and cycle totals per PC).
    pub fn enable_profiling(&mut self) {
        if self.block_profile.is_none() {
            self.block_profile = Some(vec![(0u64, 0u64); self.pram_size]);
        }
    }

    pub fn is_profiling(&self) -> bool {
        self.block_profile.is_some()
    }

    /// Number of compiled blocks in the cache.
    pub fn block_count(&self) -> usize {
        self.cache.blocks.iter().filter(|b| b.is_some()).count()
    }

    /// Number of cached single-instruction compilations.
    pub fn instr_cache_count(&self) -> usize {
        self.instr_cache.len()
    }

    /// Iterate over compiled blocks: yields (start_pc, end_pc, num_words).
    pub fn block_sizes(&self) -> Vec<(u32, u32, u32)> {
        self.cache
            .blocks
            .iter()
            .enumerate()
            .filter_map(|(pc, b)| {
                b.as_ref()
                    .map(|b| (pc as u32, b.end_pc, b.end_pc - pc as u32))
            })
            .collect()
    }

    /// Invalidate all cached blocks and release compiled code memory.
    pub fn invalidate_cache(&mut self) {
        self.cache.invalidate_all();
        self.instr_cache.clear();
        if let Some(old) = self.module.replace(Self::new_module()) {
            unsafe { old.free_memory() };
        }
    }

    /// Invalidate block cache only, keeping the instruction cache and
    /// Cranelift module intact. Use when PRAM layout changes but the
    /// same opcodes are likely to recur (e.g. fuzzing).
    pub fn invalidate_blocks(&mut self) {
        self.cache.invalidate_all();
    }

    /// Invalidate only blocks whose code overlaps P-memory range [lo, hi].
    pub fn invalidate_range(&mut self, lo: u32, hi: u32) {
        self.cache.invalidate_range(lo, hi);
        self.instr_cache.retain(|&(pc, _, _), _| pc < lo || pc > hi);
    }

    /// Dump block execution profile to a file, sorted by total cycles descending.
    /// Each line: pc_range, hits, total_cycles, avg_cycles, disassembly of first instruction.
    pub fn dump_profile(&self, map: &MemoryMap, path: &str) {
        let Some(ref profile) = self.block_profile else {
            return;
        };
        let mut entries: Vec<(u32, u64, u64)> = profile
            .iter()
            .enumerate()
            .filter(|(_, (hits, _))| *hits > 0)
            .map(|(pc, (hits, cycles))| (pc as u32, *hits, *cycles))
            .collect();
        entries.sort_by_key(|a| std::cmp::Reverse(a.2));

        let total_cycles: u64 = entries.iter().map(|(_, _, c)| c).sum();
        let mut f = match std::fs::File::create(path) {
            Ok(f) => f,
            Err(_) => return,
        };
        let _ = writeln!(
            f,
            "{:<20} {:>10} {:>14} {:>8} {:>6}  first_insn",
            "block", "hits", "cycles", "avg", "pct"
        );
        let _ = writeln!(f, "{}", "-".repeat(80));
        for (pc, hits, cycles) in &entries {
            let end_pc = self.cache.blocks[*pc as usize]
                .as_ref()
                .map(|b| b.end_pc)
                .unwrap_or(*pc + 1);
            let pct = (*cycles as f64 / total_cycles as f64) * 100.0;
            let _ = writeln!(
                f,
                "{:04x}..{:04x} ({:2} insn)  {:>10} {:>14} {:>8} {:>5.1}%",
                pc,
                end_pc,
                end_pc - pc,
                hits,
                cycles,
                cycles / hits.max(&1),
                pct,
            );
        }
        let _ = writeln!(f, "\ntotal_cycles: {}", total_cycles);

        // Dump raw P-space words for offline disassembly
        let _ = writeln!(f, "\n\n{}", "=".repeat(80));
        let _ = writeln!(f, "P-SPACE DUMP OF TOP 20 BLOCKS");
        let _ = writeln!(f, "{}", "=".repeat(80));
        let p_end = map.p_space_end();
        for (pc, _hits, cycles) in entries.iter().take(20) {
            let end_pc = self.cache.blocks[*pc as usize]
                .as_ref()
                .map(|b| b.end_pc)
                .unwrap_or(*pc + 1);
            let pct = (*cycles as f64 / total_cycles as f64) * 100.0;
            let _ = writeln!(
                f,
                "\n=== Block {:04x}..{:04x} ({} words, {:.1}%, {} cycles) ===",
                pc,
                end_pc,
                end_pc - pc,
                pct,
                cycles,
            );
            for addr in *pc..end_pc.min(p_end) {
                let _ = writeln!(f, "P {:04X} {:06X}", addr, map.read_pram(addr));
            }
        }
    }

    /// Get a cached compiled instruction or compile and cache it.
    ///
    /// Cache key optimizations to maximize sharing:
    /// - PC-independent instructions use `pc_key=0` (one compilation serves all addresses)
    /// - Single-word instructions use `nw_key=0` (next_word is irrelevant)
    /// - PC-dependent instructions (branches, DO loops, LRA) include the actual PC
    pub fn get_or_compile_instruction(
        &mut self,
        pc: u32,
        opcode: u32,
        next_word: u32,
        map: &MemoryMap,
    ) -> (CompiledFn, u32) {
        let inst = decode::decode(opcode);
        let inst_len = decode::instruction_length(&inst);
        let pc_key = if Self::instruction_uses_pc(&inst) {
            pc
        } else {
            0
        };
        let nw_key = if inst_len > 1 { next_word } else { 0 };
        let key = (pc_key, opcode, nw_key);
        if let Some(&entry) = self.instr_cache.get(&key) {
            return entry;
        }
        let func = self.compile_instruction(&inst, pc, next_word, map);
        self.instr_cache.insert(key, (func, inst_len));
        (func, inst_len)
    }

    /// Returns true if the instruction's compiled code depends on PC.
    fn instruction_uses_pc(inst: &Instruction) -> bool {
        matches!(
            inst,
            Instruction::Bra { .. }
                | Instruction::BraLong
                | Instruction::BraRn { .. }
                | Instruction::Bcc { .. }
                | Instruction::BccLong { .. }
                | Instruction::BccRn { .. }
                | Instruction::Bsr { .. }
                | Instruction::BsrLong
                | Instruction::BsrRn { .. }
                | Instruction::Bscc { .. }
                | Instruction::BsccLong { .. }
                | Instruction::BsccRn { .. }
                | Instruction::Jsr { .. }
                | Instruction::JsrEa { .. }
                | Instruction::Jscc { .. }
                | Instruction::JsccEa { .. }
                | Instruction::DoImm { .. }
                | Instruction::DoReg { .. }
                | Instruction::DoAa { .. }
                | Instruction::DoEa { .. }
                | Instruction::DoForever
                | Instruction::DorImm { .. }
                | Instruction::DorReg { .. }
                | Instruction::DorAa { .. }
                | Instruction::DorEa { .. }
                | Instruction::DorForever
                | Instruction::LraRn { .. }
                | Instruction::LraDisp { .. }
                | Instruction::BrclrEa { .. }
                | Instruction::BrclrAa { .. }
                | Instruction::BrclrPp { .. }
                | Instruction::BrclrQq { .. }
                | Instruction::BrclrReg { .. }
                | Instruction::BrsetEa { .. }
                | Instruction::BrsetAa { .. }
                | Instruction::BrsetPp { .. }
                | Instruction::BrsetQq { .. }
                | Instruction::BrsetReg { .. }
                | Instruction::BsclrEa { .. }
                | Instruction::BsclrAa { .. }
                | Instruction::BsclrPp { .. }
                | Instruction::BsclrQq { .. }
                | Instruction::BsclrReg { .. }
                | Instruction::BssetEa { .. }
                | Instruction::BssetAa { .. }
                | Instruction::BssetPp { .. }
                | Instruction::BssetQq { .. }
                | Instruction::BssetReg { .. }
                | Instruction::JsclrEa { .. }
                | Instruction::JsclrAa { .. }
                | Instruction::JsclrPp { .. }
                | Instruction::JsclrQq { .. }
                | Instruction::JsclrReg { .. }
                | Instruction::JssetEa { .. }
                | Instruction::JssetAa { .. }
                | Instruction::JssetPp { .. }
                | Instruction::JssetQq { .. }
                | Instruction::JssetReg { .. }
        )
    }

    /// Compile a single decoded instruction into a callable function.
    pub fn compile_instruction(
        &mut self,
        inst: &Instruction,
        pc: u32,
        next_word: u32,
        map: &MemoryMap,
    ) -> CompiledFn {
        self.ctx
            .func
            .signature
            .params
            .push(AbiParam::new(self.ptr_ty));
        self.ctx
            .func
            .signature
            .returns
            .push(AbiParam::new(types::I32));

        {
            let builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);
            let mut emitter = Emitter::new(
                builder,
                self.ptr_ty,
                map,
                self.address_register_mask,
            );
            emitter.emit_instruction(inst, pc, next_word);
            emitter.finalize_and_return();
        }

        self.finalize_function(&format!("dsp_inst_{:04x}", pc))
    }

    /// Compile a basic block starting at `start_pc`.
    /// `stop_pc` is the address at which the block must end (LA+1 for DO
    /// loops, `u32::MAX` when no loop is active).
    fn compile_block(
        &mut self,
        start_pc: u32,
        stop_pc: u32,
        generation: u32,
        map: &MemoryMap,
    ) -> CompiledBlock {
        self.ctx
            .func
            .signature
            .params
            .push(AbiParam::new(self.ptr_ty));
        self.ctx
            .func
            .signature
            .returns
            .push(AbiParam::new(types::I32));

        let end_pc;
        {
            let builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.func_ctx);
            let mut emitter = Emitter::new(
                builder,
                self.ptr_ty,
                map,
                self.address_register_mask,
            );
            end_pc = emitter.emit_block(start_pc, MAX_BLOCK_LEN, stop_pc);
            emitter.finalize_and_return();
        }

        let label = format!("dsp_block_{:04x}_{:04x}", start_pc, end_pc);
        let func = self.finalize_function(&label);
        CompiledBlock {
            func,
            end_pc,
            generation,
        }
    }

    /// Finalize the current Cranelift function and return a callable pointer.
    fn finalize_function(&mut self, _label: &str) -> CompiledFn {
        let module = self.module.as_mut().unwrap();
        let func_id = module
            .declare_anonymous_function(&self.ctx.func.signature)
            .unwrap();
        module.define_function(func_id, &mut self.ctx).unwrap();
        #[cfg(target_os = "linux")]
        let code_size = self.ctx.compiled_code().unwrap().code_buffer().len();
        module.clear_context(&mut self.ctx);
        module.finalize_definitions().unwrap();

        let code_ptr = module.get_finalized_function(func_id);

        #[cfg(target_os = "linux")]
        if let Some(f) = &mut self.perf_map {
            let _ = writeln!(f, "{:x} {:x} {}", code_ptr as usize, code_size, _label);
        }

        // Safety: code_ptr points to JIT-compiled code with signature fn(*mut DspState) -> i32
        unsafe { std::mem::transmute::<*const u8, CompiledFn>(code_ptr) }
    }
}

impl Drop for JitEngine {
    fn drop(&mut self) {
        // JITModule leaks mmap'd code on drop; explicitly free it.
        if let Some(module) = self.module.take() {
            unsafe { module.free_memory() };
        }
    }
}

impl Default for JitEngine {
    fn default() -> Self {
        Self::new(0)
    }
}

impl DspState {
    /// Execute exactly one instruction at the current PC.
    /// Returns cycles consumed. Does not touch cycle_budget.
    pub fn execute_one(&mut self, jit: &mut JitEngine) -> i32 {
        self.step_one(jit)
    }

    /// Inner instruction execution: compile/lookup, execute, advance PC,
    /// service interrupts, tick cycle counter. Returns cycles consumed.
    fn step_one(&mut self, jit: &mut JitEngine) -> i32 {
        let opcode = self.map.read_pram(self.pc);
        let next_word = self.map.read_pram(mask_pc(self.pc + 1));

        let (func, inst_len) =
            jit.get_or_compile_instruction(self.pc, opcode, next_word, &self.map);
        self.pc_advance = inst_len;

        let consumed = unsafe { func(self as *mut DspState) };

        self.advance_pc();
        self.process_pending_interrupts();
        self.cycle_count += consumed as u32;

        consumed
    }

    /// Run the DSP for the given number of cycles using basic block JIT.
    ///
    /// Compiles and caches basic blocks, executing them until the cycle
    /// budget is exhausted or the DSP enters idle state.
    ///
    /// REP and inlineable DO/DOR loops are compiled as inline Cranelift loops
    /// inside blocks. Non-inlineable DO loops use block-boundary compilation:
    /// blocks end at LA+1 and the run loop handles loop-back/exit.
    pub fn run(&mut self, jit: &mut JitEngine, cycles: i32) {
        self.cycle_budget += cycles;

        while self.cycle_budget > 0 && !self.halt_requested {
            // STOP: all clocks halted, nothing happens until external RESET.
            if self.power_state == PowerState::Stop {
                self.cycle_budget = 0;
                break;
            }

            // WAIT: core halted, peripherals running. An unmasked interrupt
            // wakes the core. We check for pending interrupts: if one can
            // fire, transition back to Normal and resume execution.
            if self.power_state == PowerState::Wait {
                if self.interrupts.has_pending() {
                    let ipl_sr = ((self.registers[reg::SR] >> sr::I0) & 0x3) as i8;
                    let can_wake = (0..interrupt::COUNT).any(|i| {
                        self.interrupts.pending(i)
                            && (self.interrupts.ipl[i] == 3 || self.interrupts.ipl[i] >= ipl_sr)
                    });
                    if can_wake {
                        self.power_state = PowerState::Normal;
                    } else {
                        self.cycle_budget = 0;
                        break;
                    }
                } else {
                    self.cycle_budget = 0;
                    break;
                }
            }

            // During interrupt pipeline processing, fall back to single-step
            // so pipeline stages advance per-instruction.
            if self.interrupts.state != InterruptState::None {
                let consumed = self.step_one(jit);
                self.cycle_budget -= consumed;
                continue;
            }

            let pc = self.pc;

            // PC outside the configured PRAM: fall back to single-step.
            if pc as usize >= jit.pram_size {
                let consumed = self.step_one(jit);
                self.cycle_budget -= consumed;
                continue;
            }

            let stop_pc = if (self.registers[reg::SR] & (1 << sr::LF)) != 0 {
                mask_pc(self.registers[reg::LA] + 1)
            } else {
                u32::MAX
            };

            // Evict if dirty or if a DO loop boundary falls within the block.
            // When stop_pc <= pc, the loop boundary is behind us (e.g. subroutine
            // called from inside a DO loop) and doesn't affect this block.
            if let Some(block) = &mut jit.cache.blocks[pc as usize] {
                let needs_evict = (stop_pc > pc && stop_pc < block.end_pc)
                    || (block.generation != self.pram_dirty.generation
                        && self.pram_dirty.is_range_dirty(pc, block.end_pc));
                if needs_evict {
                    jit.cache.blocks[pc as usize] = None;
                } else {
                    block.generation = self.pram_dirty.generation;
                }
            }

            if jit.cache.blocks[pc as usize].is_none() {
                let block = jit.compile_block(pc, stop_pc, self.pram_dirty.generation, &self.map);
                self.pram_dirty.clear_dirty_range(pc, block.end_pc);
                jit.cache.blocks[pc as usize] = Some(block);
            }

            let block = jit.cache.blocks[pc as usize].unwrap();
            let consumed = unsafe { (block.func)(self as *mut DspState) };
            self.exit_requested = false;

            if let Some(ref mut profile) = jit.block_profile {
                profile[pc as usize].0 += 1;
                profile[pc as usize].1 += consumed as u64;
            }

            self.cycle_count += consumed as u32;
            self.cycle_budget -= consumed;

            if (self.registers[reg::SR] & (1 << sr::LF)) != 0
                && self.pc == mask_pc(self.registers[reg::LA] + 1)
            {
                self.registers[reg::LC] =
                    self.registers[reg::LC].wrapping_sub(1) & REG_MASKS[reg::LC];
                if self.registers[reg::LC] == 0 && (self.registers[reg::SR] & (1 << sr::FV)) == 0 {
                    let (_saved_pc, saved_sr) = self.stack_pop();
                    let lf_fv_mask = (1 << sr::LF) | (1 << sr::FV);
                    self.registers[reg::SR] =
                        (self.registers[reg::SR] & !lf_fv_mask) | (saved_sr & lf_fv_mask);
                    let (la, lc) = self.stack_pop();
                    self.registers[reg::LA] = la;
                    self.registers[reg::LC] = lc;
                } else {
                    self.pc = self.registers[reg::SSH];
                }
            }

            self.process_pending_interrupts();
        }
    }
}

#[cfg(test)]
#[allow(unused_assignments)] // pram writes are read through raw pointers in DspState
mod tests {
    use super::*;
    use crate::core::{MemoryMap, reg};

    const PRAM_SIZE: usize = 4096;
    const XRAM_SIZE: usize = 4096;
    const YRAM_SIZE: usize = 2048;

    fn run_one(state: &mut DspState, jit: &mut JitEngine) -> i32 {
        state.execute_one(jit)
    }

    #[test]
    fn test_block_cache_hit() {
        // Run the same block twice to verify block cache works.
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        pram[0] = 0x000008; // inc A (1 cycle)
        pram[1] = 0x0C0000; // jmp $0 (3 cycles)
        // Block = [inc, jmp] = 4 cycles. Budget 9: 3 blocks.
        s.run(&mut jit, 9);
        assert!(jit.cache.blocks[0].is_some());
        assert_eq!(s.cycle_count, 12); // 3 blocks x 4 cycles
        assert_eq!(s.registers[reg::A0], 3);
    }

    #[test]
    fn test_invalidate_cache() {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        pram[0] = 0x000000; // nop
        pram[1] = 0x000000;
        run_one(&mut s, &mut jit);
        assert!(!jit.instr_cache.is_empty());
        jit.invalidate_cache();
        assert!(jit.cache.blocks.iter().all(|b| b.is_none()));
        assert!(jit.instr_cache.is_empty());
    }

    #[test]
    fn test_invalidate_range() {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        // Use JSCLR (PC-dependent: embeds pc+2 as return address) at two PCs.
        // jsclr #0,X0,$100: opcode=0x0BC400, next_word=0x000100
        pram[0x00] = 0x0BC400;
        pram[0x01] = 0x000100;
        pram[0x10] = 0x0BC400;
        pram[0x11] = 0x000100;
        s.registers[reg::X0] = 0x000001; // bit 0 set -> not taken
        s.pc = 0;
        run_one(&mut s, &mut jit); // compiles jsclr at pc=0
        s.pc = 0x10;
        run_one(&mut s, &mut jit); // compiles jsclr at pc=0x10

        // Both should be cached with their actual PCs (PC-dependent)
        assert!(jit.instr_cache.keys().any(|&(pc, _, _)| pc == 0x00));
        assert!(jit.instr_cache.keys().any(|&(pc, _, _)| pc == 0x10));

        // Invalidate range [0, 1] - should only affect instruction at PC=0
        jit.invalidate_range(0, 1);
        assert!(!jit.instr_cache.keys().any(|&(pc, _, _)| pc == 0x00));
        assert!(jit.instr_cache.keys().any(|&(pc, _, _)| pc == 0x10));
    }

    #[test]
    fn test_invalidate_range_block_cache() {
        // Compile blocks via run(), then invalidate a range and verify
        // only overlapping blocks are evicted.
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

        // Chain: JMP $10 -> JMP $20 -> JMP $30 -> JMP-to-self
        // Each JMP creates a block at its PC.
        pram[0x00] = 0x0C0010; // JMP $10
        pram[0x10] = 0x0C0020; // JMP $20
        pram[0x20] = 0x0C0030; // JMP $30
        pram[0x30] = 0x0C0030; // JMP $30 (loop here, budget exhausts)

        s.run(&mut jit, 20);

        assert!(jit.cache.blocks[0x00].is_some());
        assert!(jit.cache.blocks[0x10].is_some());
        assert!(jit.cache.blocks[0x20].is_some());
        assert!(jit.cache.blocks[0x30].is_some());

        // Invalidate range [0x08, 0x15]: should evict blocks at 0x00 and 0x10.
        // Block at 0x00 (end_pc=0x01): pc=0 <= hi=0x15 and end_pc=1 > lo=0x08? No,
        // end_pc=1 is NOT > 0x08. So block at 0x00 should survive.
        // Block at 0x10 (end_pc=0x11): pc=0x10 <= 0x15 and end_pc=0x11 > 0x08. Evicted.
        // Block at 0x20: pc=0x20 > hi=0x15. Survives.
        // Block at 0x30: pc=0x30 > hi=0x15. Survives.
        jit.invalidate_range(0x08, 0x15);

        assert!(jit.cache.blocks[0x00].is_some());
        assert!(jit.cache.blocks[0x10].is_none());
        assert!(jit.cache.blocks[0x20].is_some());
        assert!(jit.cache.blocks[0x30].is_some());
    }

    #[test]
    fn test_dirty_bit_eviction() {
        // Compile a block, mark its PRAM dirty, then run again. The run loop
        // should detect the stale block and recompile it.
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

        // First: INC A, JMP $0 -> A0 increments each iteration.
        pram[0] = 0x000008; // INC A
        pram[1] = 0x0C0000; // JMP $0

        // Run for a few cycles to compile and cache the block.
        s.run(&mut jit, 6);
        let a0_first = s.registers[reg::A0];
        assert!(a0_first > 0);
        assert!(jit.cache.blocks[0].is_some());
        let gen_before = jit.cache.blocks[0].unwrap().generation;

        // Overwrite PRAM and mark dirty. Replace INC with DEC.
        pram[0] = 0x00000A; // DEC A
        s.pram_dirty.mark_dirty(0);

        // Run again. The run loop should detect the dirty block and recompile.
        s.registers[reg::A0] = 10;
        s.registers[reg::A1] = 0;
        s.registers[reg::A2] = 0;
        s.pc = 0;
        s.cycle_count = 0;
        s.run(&mut jit, 6);

        // Block was recompiled with new generation.
        assert!(jit.cache.blocks[0].is_some());
        assert_ne!(jit.cache.blocks[0].unwrap().generation, gen_before);
        // DEC should have decremented A0 from 10.
        assert!(s.registers[reg::A0] < 10);
    }

    #[test]
    fn test_profiling_enable_and_counters() {
        let mut jit = JitEngine::new(PRAM_SIZE);
        assert!(!jit.is_profiling());

        jit.enable_profiling();
        assert!(jit.is_profiling());

        // Run some instructions to accumulate profile data.
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        pram[0] = 0x000000; // NOP
        pram[1] = 0x0C0000; // JMP $0

        s.run(&mut jit, 10);

        // Profile should have recorded hits at PC 0.
        let profile = jit.block_profile.as_ref().unwrap();
        assert!(profile[0].0 > 0, "expected hits > 0");
        assert!(profile[0].1 > 0, "expected cycles > 0");
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn test_perf_map() {
        let mut jit = JitEngine::new(PRAM_SIZE);
        jit.enable_perf_map();
        assert!(jit.perf_map.is_some());

        // Compile a block to trigger a perf map write.
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        pram[0] = 0x0C0000; // JMP $0
        run_one(&mut s, &mut jit);

        // Verify the perf map file was created.
        let path = format!("/tmp/perf-{}.map", std::process::id());
        assert!(std::path::Path::new(&path).exists());
        let contents = std::fs::read_to_string(&path).unwrap();
        assert!(!contents.is_empty());
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn test_jit_engine_default() {
        let jit = JitEngine::default();
        assert!(!jit.is_profiling());
        #[cfg(target_os = "linux")]
        assert!(jit.perf_map.is_none());
    }
}
