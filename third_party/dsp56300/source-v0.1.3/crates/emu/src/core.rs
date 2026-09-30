//! DSP56300 core state: registers, memory, and constants.

use std::ffi::c_void;

// Re-export architectural constants from dsp56300-core
pub use dsp56300_core::{
    MemSpace, PC_MASK, PERIPH_BASE, PERIPH_SIZE, REG_MASKS, mask_pc, mask_reg, reg, sr,
};

/// What kind of access a memory region provides.
#[derive(Clone, Copy)]
pub enum RegionKind {
    /// Direct buffer access: `base[addr - start + offset]`.
    /// The caller owns the buffer; it must remain valid for the DspState lifetime.
    Buffer { base: *mut u32, offset: u32 },
    /// Callback-driven access (e.g. peripheral registers).
    /// In JIT code this requires flush/reload of promoted registers.
    Callback {
        opaque: *mut c_void,
        read_fn: unsafe extern "C" fn(*mut c_void, u32) -> u32,
        write_fn: unsafe extern "C" fn(*mut c_void, u32, u32),
    },
}

// Safety: the raw pointers in RegionKind are only dereferenced during
// single-threaded DSP execution on the thread that owns the DspState.
unsafe impl Send for RegionKind {}
unsafe impl Sync for RegionKind {}

/// A contiguous region in DSP address space.
#[derive(Clone, Copy)]
pub struct MemoryRegion {
    /// First address in the region (inclusive).
    pub start: u32,
    /// One past the last address (exclusive).
    pub end: u32,
    /// How this region is accessed.
    pub kind: RegionKind,
}

/// Memory map describing the DSP address space.
///
/// Each of the three DSP address spaces (X, Y, P) has a list of regions
/// sorted by start address. The JIT emitter uses this at compile time to
/// generate inline loads for Buffer regions and indirect calls for Callback
/// regions.
#[derive(Clone, Default)]
pub struct MemoryMap {
    pub x_regions: Vec<MemoryRegion>,
    pub y_regions: Vec<MemoryRegion>,
    pub p_regions: Vec<MemoryRegion>,
}

impl MemoryMap {
    /// Return the region list for a given space.
    pub fn regions(&self, space: MemSpace) -> &[MemoryRegion] {
        match space {
            MemSpace::X => &self.x_regions,
            MemSpace::Y => &self.y_regions,
            MemSpace::P => &self.p_regions,
        }
    }

    /// Look up the region containing `addr` in the given space.
    pub fn lookup(&self, space: MemSpace, addr: u32) -> Option<&MemoryRegion> {
        self.regions(space)
            .iter()
            .find(|r| addr >= r.start && addr < r.end)
    }

    /// Read a 24-bit word from P-space at the given address.
    /// Handles both Buffer and Callback regions. Returns 0 if out of range.
    pub fn read_pram(&self, addr: u32) -> u32 {
        for region in &self.p_regions {
            if addr >= region.start && addr < region.end {
                let raw = match region.kind {
                    RegionKind::Buffer { base, offset } => {
                        let idx = (addr - region.start + offset) as usize;
                        unsafe { *base.add(idx) }
                    }
                    RegionKind::Callback {
                        opaque, read_fn, ..
                    } => unsafe { read_fn(opaque, addr) },
                };
                return raw & PC_MASK;
            }
        }
        0
    }

    /// Return the highest P-space address (exclusive) across all regions.
    pub fn p_space_end(&self) -> u32 {
        self.p_regions.iter().map(|r| r.end).max().unwrap_or(0)
    }

    /// Build a simple test map: X [0, xram.len()), Y [0, yram.len()), P [0, pram.len()).
    pub fn test(xram: &mut [u32], yram: &mut [u32], pram: &mut [u32]) -> Self {
        MemoryMap {
            x_regions: vec![MemoryRegion {
                start: 0,
                end: xram.len() as u32,
                kind: RegionKind::Buffer {
                    base: xram.as_mut_ptr(),
                    offset: 0,
                },
            }],
            y_regions: vec![MemoryRegion {
                start: 0,
                end: yram.len() as u32,
                kind: RegionKind::Buffer {
                    base: yram.as_mut_ptr(),
                    offset: 0,
                },
            }],
            p_regions: vec![MemoryRegion {
                start: 0,
                end: pram.len() as u32,
                kind: RegionKind::Buffer {
                    base: pram.as_mut_ptr(),
                    offset: 0,
                },
            }],
        }
    }
}

/// Initialization parameters for creating a new DSP state.
#[derive(Clone, Default)]
pub struct CreateInfo {
    pub memory_map: MemoryMap,
    /// Mask applied to the R, N, and M address-generation registers.
    /// Zero selects the architectural DSP56300 default ($FFFFFF).
    pub address_register_mask: u32,
}

impl From<MemoryMap> for CreateInfo {
    fn from(memory_map: MemoryMap) -> Self {
        Self {
            memory_map,
            address_register_mask: 0,
        }
    }
}

/// Power state (DSP56300FM section 8.4).
///
/// WAIT: halts the clock to the core but not peripherals. An unmasked
/// interrupt wakes the core. STOP: halts all clocks. Only an external
/// hardware RESET can restart.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PowerState {
    /// Normal operation.
    Normal = 0,
    /// WAIT instruction executed: core halted, peripherals running.
    /// Unmasked interrupt returns to Normal.
    Wait = 1,
    /// STOP instruction executed: all clocks halted.
    /// Only hardware RESET restarts.
    Stop = 2,
}

impl From<u8> for PowerState {
    fn from(v: u8) -> Self {
        match v {
            1 => Self::Wait,
            2 => Self::Stop,
            _ => Self::Normal,
        }
    }
}

/// Interrupt pipeline state (see DSP56300FM section 2.3.2).
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum InterruptState {
    /// No interrupt in progress (normal processing).
    None = 0,
    /// Fast interrupt: vector instructions being fetched/executed.
    Fast = 1,
    /// Long interrupt: JSR detected at vector, context stacked.
    Long = 2,
}

impl From<u8> for InterruptState {
    fn from(v: u8) -> Self {
        match v {
            1 => Self::Fast,
            2 => Self::Long,
            _ => Self::None,
        }
    }
}

impl std::fmt::Display for InterruptState {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::None => f.write_str("none"),
            Self::Fast => f.write_str("fast"),
            Self::Long => f.write_str("long"),
        }
    }
}

/// Interrupt pipeline state machine (DSP56300FM section 2.3.2).
///
/// The DSP56300 supports 128 interrupt vectors in a 256-word IVT.
/// Each vector occupies 2 words at address `index * 2`.
#[derive(Clone)]
pub struct InterruptPipeline {
    pub state: InterruptState,
    pub pending_bits: [u64; 2],
    pub pipeline_stage: u8,
    pub vector_addr: u32,
    pub saved_pc: u32,
    pub ipl: [i8; interrupt::COUNT],
    pub ipl_to_raise: u8,
}

impl Default for InterruptPipeline {
    fn default() -> Self {
        Self::new()
    }
}

impl InterruptPipeline {
    pub fn new() -> Self {
        let mut ipl = [-1i8; interrupt::COUNT];
        // Core interrupts are IPL 3 (non-maskable) by default
        ipl[interrupt::RESET] = 3;
        ipl[interrupt::ILLEGAL] = 3;
        ipl[interrupt::STACK_ERROR] = 3;
        ipl[interrupt::TRAP] = 3;
        Self {
            state: InterruptState::None,
            pending_bits: [0; 2],
            pipeline_stage: 0,
            vector_addr: 0xFFFF,
            saved_pc: 0xFFFF,
            ipl,
            ipl_to_raise: 0,
        }
    }

    pub fn has_pending(&self) -> bool {
        self.pending_bits[0] != 0 || self.pending_bits[1] != 0
    }

    pub fn pending(&self, i: usize) -> bool {
        self.pending_bits[i / 64] & (1u64 << (i % 64)) != 0
    }

    pub fn set_pending(&mut self, i: usize) {
        self.pending_bits[i / 64] |= 1u64 << (i % 64);
    }

    pub fn clear_pending(&mut self, i: usize) {
        self.pending_bits[i / 64] &= !(1u64 << (i % 64));
    }

    /// Post a pending interrupt.
    pub fn add(&mut self, inter: usize) {
        if inter >= interrupt::COUNT || self.ipl[inter] == -1 {
            return;
        }
        self.set_pending(inter);
    }
}

/// Interrupt definitions (DSP56300FM Table 2-2).
///
/// The DSP56300 has 128 interrupt vectors. Each vector's address is `index * 2`.
/// The first 8 are core-defined; the rest are peripheral interrupt requests.
pub mod interrupt {
    /// Total number of interrupt sources.
    pub const COUNT: usize = 128;

    // Core interrupt indices = architectural IVT slot number.
    // Vector address = VBA + (index * 2), per Table 2-2.
    //
    // Note: The ILLEGAL instruction page (13-76) says "P:$3E" - this is a
    // DSP56000 holdover. The DSP56300 IVT is relocatable via VBA and Table 2-2
    // places ILLEGAL at VBA:$04.
    pub const RESET: usize = 0; // VBA:$00
    pub const STACK_ERROR: usize = 1; // VBA:$02
    pub const ILLEGAL: usize = 2; // VBA:$04
    pub const DEBUG: usize = 3; // VBA:$06
    pub const TRAP: usize = 4; // VBA:$08
    pub const NMI: usize = 5; // VBA:$0A

    /// Return the slot offset (low 8 bits of vector address) for interrupt index `i`.
    pub const fn vector_addr(i: usize) -> u16 {
        (i as u16) * 2
    }
}

/// Dirty bitmap for tracking PRAM writes that invalidate JIT blocks.
pub struct PramDirtyBitmap {
    /// Total number of tracked PRAM words.
    pram_size: usize,
    /// Dirty bitmap: one bit per PRAM word.
    /// Set when JIT code or DMA writes a DIFFERENT value to P-space.
    /// The run loop checks the block's range before execution.
    pub dirty: Vec<u64>,
    /// Generation counter. Bumped whenever a dirty bit is set.
    /// Cached blocks store the generation they were compiled at; if it matches,
    /// the dirty bitmap scan is skipped entirely.
    pub generation: u32,
}

impl PramDirtyBitmap {
    pub fn new(pram_size: usize) -> Self {
        Self {
            pram_size,
            dirty: vec![0u64; pram_size.div_ceil(64)],
            generation: 0,
        }
    }

    /// Mark a PRAM address as dirty and bump the generation counter.
    pub fn mark_dirty(&mut self, addr: u32) {
        let addr = addr as usize;
        if addr < self.pram_size {
            self.dirty[addr / 64] |= 1u64 << (addr % 64);
            self.generation = self.generation.wrapping_add(1);
        }
    }

    /// Check whether any dirty bit is set in the range [start_pc, end_pc).
    pub fn is_range_dirty(&self, start_pc: u32, end_pc: u32) -> bool {
        let lo = start_pc as usize;
        let hi = (end_pc as usize).min(self.pram_size);
        if lo >= hi {
            return false;
        }
        let first_word = lo / 64;
        let last_word = (hi - 1) / 64;
        if first_word == last_word {
            let mask = Self::range_mask(lo % 64, hi.wrapping_sub(first_word * 64).min(64));
            return self.dirty[first_word] & mask != 0;
        }
        // First partial word
        let first_mask = !0u64 << (lo % 64);
        if self.dirty[first_word] & first_mask != 0 {
            return true;
        }
        // Full words in the middle
        if self.dirty[(first_word + 1)..last_word]
            .iter()
            .any(|&w| w != 0)
        {
            return true;
        }
        // Last partial word
        let last_bits = hi - last_word * 64;
        let last_mask = if last_bits >= 64 {
            !0u64
        } else {
            (1u64 << last_bits) - 1
        };
        self.dirty[last_word] & last_mask != 0
    }

    /// Clear dirty bits for PRAM range [start_pc, end_pc).
    pub fn clear_dirty_range(&mut self, start_pc: u32, end_pc: u32) {
        let lo = start_pc as usize;
        let hi = (end_pc as usize).min(self.pram_size);
        if lo >= hi {
            return;
        }
        let first_word = lo / 64;
        let last_word = (hi - 1) / 64;
        if first_word == last_word {
            let mask = Self::range_mask(lo % 64, hi.wrapping_sub(first_word * 64).min(64));
            self.dirty[first_word] &= !mask;
            return;
        }
        self.dirty[first_word] &= !(!0u64 << (lo % 64));
        self.dirty[(first_word + 1)..last_word].fill(0);
        let last_bits = hi - last_word * 64;
        let last_mask = if last_bits >= 64 {
            !0u64
        } else {
            (1u64 << last_bits) - 1
        };
        self.dirty[last_word] &= !last_mask;
    }

    /// Bitmask for bits [lo_bit, hi_bit) within a single u64 word.
    pub fn range_mask(lo_bit: usize, hi_bit: usize) -> u64 {
        debug_assert!(lo_bit < 64 && hi_bit <= 64 && lo_bit < hi_bit);
        let top = if hi_bit >= 64 {
            !0u64
        } else {
            (1u64 << hi_bit) - 1
        };
        top & !((1u64 << lo_bit) - 1)
    }
}

/// DSP56300 state.
pub struct DspState {
    /// Program counter
    pub pc: u32,
    /// PC advance after instruction execution (word count added to PC)
    pub pc_advance: u32,
    /// Total cycle count
    pub cycle_count: u32,
    /// General registers (indexed by reg::* constants)
    pub registers: [u32; reg::COUNT],
    /// Hardware stack: stack\[0\] = SSH, stack\[1\] = SSL
    pub stack: [[u32; 16]; 2],

    /// True while inside a REP loop
    pub loop_rep: bool,
    /// True on the first iteration after REP (skip LC decrement)
    pub pc_on_rep: bool,

    pub interrupts: InterruptPipeline,

    /// Remaining cycle budget for `run()`
    pub cycle_budget: i32,
    /// External halt request (set by peripheral callback, checked by run loop)
    pub halt_requested: bool,
    /// Block-level exit request. Set when a condition requires the currently
    /// executing block to return to the run loop (e.g. halt_requested was
    /// set by a peripheral callback). Cleared by the run loop after each block.
    pub exit_requested: bool,
    /// Power state (WAIT/STOP). Set by WAIT/STOP instructions, checked by
    /// the run loop. WAIT is cleared on unmasked interrupt; STOP requires
    /// external RESET.
    pub power_state: PowerState,

    /// Tracks PRAM modifications for JIT block invalidation.
    pub pram_dirty: PramDirtyBitmap,

    /// Configurable memory map (set via CreateInfo at init time)
    pub map: MemoryMap,

    /// Platform-specific width of the address-generation registers.  The
    /// generic DSP56300 uses 24 bits; MCPX exposes these registers as 16-bit.
    pub address_register_mask: u32,

    /// Bitmask of unimplemented-feature warnings already printed (warn once per bit).
    warned_bits: u32,
}

impl DspState {
    /// Create a new DSP state with the given configuration.
    /// The caller owns all buffers referenced by the memory map; they must
    /// remain valid for the lifetime of this DspState.
    pub fn new(info: impl Into<CreateInfo>) -> Self {
        let info = info.into();
        let map = info.memory_map;
        let address_register_mask = match info.address_register_mask & 0x00FF_FFFF {
            0 => REG_MASKS[reg::R0],
            mask => mask,
        };
        let mut registers = [0u32; reg::COUNT];
        // M registers default to all ones (linear addressing mode) at the
        // address-register width selected by the platform profile.
        for i in 0..8 {
            registers[reg::M0 + i] = address_register_mask;
        }
        // SR reset: CP[1-0]=1 (bits 23-22), I[1-0]=1 (bits 9-8) per Fig 5-4, Section 2.3.3
        registers[reg::SR] = 0xC0_0300;
        // OMR reset: CDP[1-0]=1 (bits 8-9) per Table 5-2; mode pins (bits 0-3)
        // are loaded from external pins on hardware - caller sets as needed.
        registers[reg::OMR] = 0x0300;
        Self {
            pc: 0,
            pc_advance: 0,
            cycle_count: 0,
            registers,
            stack: [[0; 16]; 2],
            loop_rep: false,
            pc_on_rep: false,
            cycle_budget: 0,
            halt_requested: false,
            exit_requested: false,
            power_state: PowerState::Normal,
            interrupts: InterruptPipeline::new(),
            pram_dirty: PramDirtyBitmap::new(map.p_space_end() as usize),
            map,
            address_register_mask,
            warned_bits: 0,
        }
    }

    #[inline]
    pub fn register_mask(&self, index: usize) -> u32 {
        if (reg::R0..=reg::M7).contains(&index) {
            self.address_register_mask
        } else {
            REG_MASKS[index]
        }
    }

    // Stack operations

    const OMR_XYS: u32 = 1 << 16;
    const OMR_EUN: u32 = 1 << 17;
    const OMR_EOV: u32 = 1 << 18;
    const OMR_WRP: u32 = 1 << 19;
    const OMR_SEN: u32 = 1 << 20;
    const EXTENDED_STACK_RESIDENT: u32 = 14;

    #[inline]
    fn stack_extension_space(&self) -> MemSpace {
        if self.registers[reg::OMR] & Self::OMR_XYS != 0 {
            MemSpace::Y
        } else {
            MemSpace::X
        }
    }

    pub fn stack_push(&mut self, ssh_val: u32, ssl_val: u32) {
        self.stack_push_inner(ssh_val, Some(ssl_val));
    }

    fn stack_push_ssh(&mut self, ssh_val: u32) {
        self.stack_push_inner(ssh_val, None);
    }

    fn stack_push_inner(&mut self, ssh_val: u32, ssl_val: Option<u32>) {
        if self.registers[reg::OMR] & Self::OMR_SEN != 0 {
            let sp = self.registers[reg::SP] & 0x00FF_FFFF;
            let sz = self.registers[reg::SZ] & 0x00FF_FFFF;
            if sp == sz {
                if self.registers[reg::OMR] & Self::OMR_EOV == 0 {
                    self.interrupts.add(interrupt::STACK_ERROR);
                }
                self.registers[reg::OMR] |= Self::OMR_EOV;
            }

            let new_sp = sp.wrapping_add(1) & 0x00FF_FFFF;
            self.registers[reg::SP] = new_sp;
            let idx = (new_sp & 0xF) as usize;
            self.stack[0][idx] = ssh_val & REG_MASKS[reg::SSH];
            if let Some(ssl) = ssl_val {
                self.stack[1][idx] = ssl & REG_MASKS[reg::SSL];
            }

            let sc = self.registers[reg::SC] & 0x1F;
            if sc < Self::EXTENDED_STACK_RESIDENT {
                self.registers[reg::SC] = sc + 1;
            } else {
                let lru_idx = (new_sp.wrapping_sub(Self::EXTENDED_STACK_RESIDENT) & 0xF) as usize;
                let ep = self.registers[reg::EP] & 0x00FF_FFFF;
                let space = self.stack_extension_space();
                self.write_memory(space, ep, self.stack[0][lru_idx]);
                self.write_memory(
                    space,
                    ep.wrapping_add(1) & 0x00FF_FFFF,
                    self.stack[1][lru_idx],
                );
                self.registers[reg::EP] = ep.wrapping_add(2) & 0x00FF_FFFF;
                self.registers[reg::OMR] |= Self::OMR_WRP;
            }

            self.registers[reg::SSH] = self.stack[0][idx];
            self.registers[reg::SSL] = self.stack[1][idx];
            return;
        }

        let stack_error = self.registers[reg::SP] & (1 << 4);
        let underflow = self.registers[reg::SP] & (1 << 5);
        let stack = (self.registers[reg::SP] & 0xF) + 1;
        if stack_error == 0 && (stack & (1 << 4)) != 0 {
            self.interrupts.add(interrupt::STACK_ERROR);
        }

        self.registers[reg::SP] = (underflow | stack_error | stack) & 0x3F;
        let idx = (stack & 0xF) as usize;
        if stack != 0 {
            self.stack[0][idx] = ssh_val & REG_MASKS[reg::SSH];
            if let Some(ssl) = ssl_val {
                self.stack[1][idx] = ssl & REG_MASKS[reg::SSL];
            }
        }
        self.registers[reg::SSH] = self.stack[0][idx];
        self.registers[reg::SSL] = self.stack[1][idx];
    }

    pub fn stack_pop(&mut self) -> (u32, u32) {
        if self.registers[reg::OMR] & Self::OMR_SEN != 0 {
            let sp = self.registers[reg::SP] & 0x00FF_FFFF;
            if sp == 0 {
                if self.registers[reg::OMR] & Self::OMR_EUN == 0 {
                    self.interrupts.add(interrupt::STACK_ERROR);
                }
                self.registers[reg::OMR] |= Self::OMR_EUN;
            }

            let idx = (sp & 0xF) as usize;
            let ssh = self.stack[0][idx];
            let ssl = self.stack[1][idx];
            let new_sp = sp.wrapping_sub(1) & 0x00FF_FFFF;
            self.registers[reg::SP] = new_sp;

            let sc = self.registers[reg::SC] & 0x1F;
            if sc == 2 && sp > sc {
                let ep = self.registers[reg::EP].wrapping_sub(2) & 0x00FF_FFFF;
                let space = self.stack_extension_space();
                let refill_idx = (new_sp.wrapping_sub(1) & 0xF) as usize;
                self.stack[0][refill_idx] = self.read_memory(space, ep);
                self.stack[1][refill_idx] =
                    self.read_memory(space, ep.wrapping_add(1) & 0x00FF_FFFF);
                self.registers[reg::EP] = ep;
            } else if sc > 0 {
                self.registers[reg::SC] = sc - 1;
            }

            let new_idx = (new_sp & 0xF) as usize;
            self.registers[reg::SSH] = self.stack[0][new_idx];
            self.registers[reg::SSL] = self.stack[1][new_idx];
            return (ssh, ssl);
        }

        let current_idx = (self.registers[reg::SP] & 0xF) as usize;
        let ssh = self.stack[0][current_idx];
        let ssl = self.stack[1][current_idx];
        let stack_error = self.registers[reg::SP] & (1 << 4);
        let underflow = self.registers[reg::SP] & (1 << 5);
        let stack = (self.registers[reg::SP] & 0xF).wrapping_sub(1);
        if stack_error == 0 && (stack & (1 << 4)) != 0 {
            self.interrupts.add(interrupt::STACK_ERROR);
        }

        self.registers[reg::SP] = (underflow | stack_error | stack) & 0x3F;
        let idx = (stack & 0xF) as usize;
        self.registers[reg::SSH] = self.stack[0][idx];
        self.registers[reg::SSL] = self.stack[1][idx];
        (ssh, ssl)
    }

    /// Warn once per unimplemented feature bit when guest code enables it.
    #[inline]
    fn check_unimplemented_modes(&mut self) {
        // Fast path: combined mask of all unimplemented SR bits we care about.
        const SR_UNIMPL: u32 = (1 << sr::SC) | (1 << sr::SA) | (1 << sr::DM);
        const OMR_UNIMPL: u32 = 1 << 7; // MS
        let sr = self.registers[reg::SR];
        let omr = self.registers[reg::OMR];
        if (sr & SR_UNIMPL) | (omr & OMR_UNIMPL) == 0 {
            return;
        }
        self.check_unimplemented_modes_slow();
    }

    #[cold]
    fn check_unimplemented_modes_slow(&mut self) {
        let sr = self.registers[reg::SR];
        let omr = self.registers[reg::OMR];
        // Each warning bit corresponds to one unimplemented feature.
        // We only print once per feature (sticky in warned_bits).
        const W_SC: u32 = 1 << 0;
        const W_SA: u32 = 1 << 1;
        const W_DM: u32 = 1 << 2;
        const W_MS: u32 = 1 << 3;

        let mut new_warnings = 0u32;

        if sr & (1 << sr::SC) != 0 && self.warned_bits & W_SC == 0 {
            eprintln!(
                "WARNING: DSP56300 16-bit compatibility mode (SC, SR bit 13) enabled at PC=${:06X} - not implemented",
                self.pc
            );
            new_warnings |= W_SC;
        }
        if sr & (1 << sr::SA) != 0 && self.warned_bits & W_SA == 0 {
            eprintln!(
                "WARNING: DSP56300 16-bit arithmetic mode (SA, SR bit 17) enabled at PC=${:06X} - not implemented",
                self.pc
            );
            new_warnings |= W_SA;
        }
        if sr & (1 << sr::DM) != 0 && self.warned_bits & W_DM == 0 {
            eprintln!(
                "WARNING: DSP56300 double-precision multiply mode (DM, SR bit 14) enabled at PC=${:06X} - not implemented",
                self.pc
            );
            new_warnings |= W_DM;
        }
        if omr & (1 << 7) != 0 && self.warned_bits & W_MS == 0 {
            eprintln!(
                "WARNING: DSP56300 memory switch mode (MS, OMR bit 7) enabled at PC=${:06X} - not implemented",
                self.pc
            );
            new_warnings |= W_MS;
        }
        self.warned_bits |= new_warnings;
    }

    /// Post-execution PC update: handles REP iteration, PC advancement,
    /// and DO loop end-of-loop checks.
    pub fn advance_pc(&mut self) {
        self.check_unimplemented_modes();
        // REP handling
        if self.loop_rep {
            if !self.pc_on_rep {
                self.registers[reg::LC] =
                    self.registers[reg::LC].wrapping_sub(1) & REG_MASKS[reg::LC];
                if self.registers[reg::LC] > 0 {
                    self.pc_advance = 0; // stay on instruction
                } else {
                    self.loop_rep = false;
                    self.registers[reg::LC] = self.registers[reg::TEMP];
                }
            } else {
                // First call after REP instruction:
                // REP with LC=0 repeats 65,536 times (page 13-160)
                if self.registers[reg::LC] == 0 {
                    self.registers[reg::LC] = 0x10000;
                }
                self.pc_on_rep = false;
            }
        }

        self.pc = mask_pc(self.pc + self.pc_advance);

        // DO loop end-of-loop check
        if (self.registers[reg::SR] & (1 << sr::LF)) != 0
            && self.pc == mask_pc(self.registers[reg::LA] + 1)
        {
            self.registers[reg::LC] = self.registers[reg::LC].wrapping_sub(1) & REG_MASKS[reg::LC];
            if self.registers[reg::LC] == 0 && (self.registers[reg::SR] & (1 << sr::FV)) == 0 {
                // End of loop: pop saved PC+SR, restore LF+FV, pop saved LA+LC
                let (_saved_pc, saved_sr) = self.stack_pop();
                let lf_fv_mask = (1 << sr::LF) | (1 << sr::FV);
                self.registers[reg::SR] =
                    (self.registers[reg::SR] & !lf_fv_mask) | (saved_sr & lf_fv_mask);
                let (la, lc) = self.stack_pop();
                self.registers[reg::LA] = la;
                self.registers[reg::LC] = lc;
            } else {
                // Loop again: jump to loop start address (SSH)
                self.pc = self.registers[reg::SSH];
            }
        }
    }

    pub fn process_pending_interrupts(&mut self) {
        // REP is not interruptible
        if self.loop_rep {
            return;
        }

        // Handle interrupt pipeline if an interrupt is in flight
        if self.interrupts.state == InterruptState::Fast {
            match self.interrupts.pipeline_stage {
                5 => {
                    self.interrupts.pipeline_stage -= 1;
                    return;
                }
                4 => {
                    // Save PC, jump to interrupt vector
                    self.interrupts.saved_pc = self.pc;
                    self.pc = self.interrupts.vector_addr;

                    // Read instruction at vector to detect fast vs long
                    let instr = self.read_memory(MemSpace::P, self.pc);
                    self.detect_long_interrupt(instr);

                    self.interrupts.pipeline_stage -= 1;
                    return;
                }
                3 => {
                    // Second instruction prefetch (if 2-word instruction)
                    if self.pc == mask_pc(self.interrupts.vector_addr + 1) {
                        let instr = self.read_memory(MemSpace::P, self.pc);
                        self.detect_long_interrupt(instr);
                    }
                    self.interrupts.pipeline_stage -= 1;
                    return;
                }
                2 => {
                    // Fast interrupt: restore saved PC after 2-word vector
                    if self.interrupts.state != InterruptState::Long
                        && self.pc == mask_pc(self.interrupts.vector_addr + 2)
                    {
                        self.pc = self.interrupts.saved_pc;
                    }
                    self.interrupts.pipeline_stage -= 1;
                    return;
                }
                1 => {
                    self.interrupts.pipeline_stage -= 1;
                    return;
                }
                0 => {
                    // Pipeline complete, re-enable interrupts
                    self.interrupts.saved_pc = 0xFFFF;
                    self.interrupts.vector_addr = 0xFFFF;
                    self.interrupts.state = InterruptState::None;
                }
                _ => return,
            }
        }

        if !self.interrupts.has_pending() {
            return;
        }

        // Arbitrate: find highest-priority unmasked interrupt
        let ipl_sr = ((self.registers[reg::SR] >> sr::I0) & 0x3) as i8;
        let mut index: Option<usize> = None;
        let mut ipl_to_raise: i8 = -1;

        for i in 0..interrupt::COUNT {
            if !self.interrupts.pending(i) {
                continue;
            }
            // Level 3 always wins (non-maskable)
            if self.interrupts.ipl[i] == 3 {
                index = Some(i);
                break;
            }
            // Skip masked interrupts
            if self.interrupts.ipl[i] < ipl_sr {
                continue;
            }
            // Pick highest IPL
            if self.interrupts.ipl[i] > ipl_to_raise {
                index = Some(i);
                ipl_to_raise = self.interrupts.ipl[i];
            }
        }

        let Some(idx) = index else { return };

        // Dispatch: clear pending, start pipeline
        self.interrupts.clear_pending(idx);

        let new_ipl = (self.interrupts.ipl[idx] + 1).min(3);
        // Vector address = VBA[23:8] | slot_offset[7:0] (per Section 5.4.4.4)
        let vba = self.registers[reg::VBA] & 0xFFFF00;
        self.interrupts.vector_addr = vba | interrupt::vector_addr(idx) as u32;
        self.interrupts.pipeline_stage = 5;
        self.interrupts.state = InterruptState::Fast;
        self.interrupts.ipl_to_raise = new_ipl as u8;
    }

    /// Detect whether the instruction at the interrupt vector is a long
    /// interrupt handler (contains a JSR). If so, push context to stack
    /// and update SR. Called during pipeline stages 4 and 3.
    ///
    /// Per Section 2.3.2.5: "Any Jump To Subroutine (JSR) instruction makes
    /// the interrupt long (for example, JScc, BSSET, and so on.)"
    fn detect_long_interrupt(&mut self, instr: u32) {
        use dsp56300_core::{Instruction, decode};
        let is_long = matches!(
            decode::decode(instr),
            Instruction::Jsr { .. }
                | Instruction::JsrEa { .. }
                | Instruction::Jscc { .. }
                | Instruction::JsccEa { .. }
                | Instruction::Bsr { .. }
                | Instruction::BsrLong
                | Instruction::BsrRn { .. }
                | Instruction::Bscc { .. }
                | Instruction::BsccLong { .. }
                | Instruction::BsccRn { .. }
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
        );

        if is_long && self.interrupts.state != InterruptState::Long {
            self.interrupts.state = InterruptState::Long;
            self.stack_push(self.interrupts.saved_pc, self.registers[reg::SR]);
            // Manual Section 2.3.2.5: clear LF, S1, S0, SA, and set IPL.
            // FV is NOT cleared (not listed in the manual).
            let clear_mask = (1 << sr::LF)
                | (1 << sr::S1)
                | (1 << sr::S0)
                | (1 << sr::I0)
                | (1 << sr::I1)
                | (1 << sr::SA);
            self.registers[reg::SR] &= !clear_mask;
            self.registers[reg::SR] |= (self.interrupts.ipl_to_raise as u32) << sr::I0;
        }
    }

    // Memory access

    /// Read a 24-bit word from the specified memory space.
    pub fn read_memory(&self, space: MemSpace, addr: u32) -> u32 {
        let regions = match space {
            MemSpace::X => &self.map.x_regions,
            MemSpace::Y => &self.map.y_regions,
            MemSpace::P => &self.map.p_regions,
        };
        for region in regions {
            if addr >= region.start && addr < region.end {
                let raw = match region.kind {
                    RegionKind::Buffer { base, offset } => {
                        let idx = (addr - region.start + offset) as usize;
                        unsafe { *base.add(idx) }
                    }
                    RegionKind::Callback {
                        opaque, read_fn, ..
                    } => unsafe { read_fn(opaque, addr) },
                };
                return raw & 0x00FF_FFFF;
            }
        }
        0
    }

    // Address register update

    /// Update address register Rn based on M register mode.
    pub fn update_rn(&mut self, numreg: usize, modifier: i32) {
        let r_mask = self.address_register_mask;
        let m_reg = self.registers[reg::M0 + numreg] & self.address_register_mask;
        if m_reg == self.address_register_mask {
            // Linear addressing (M = all ones at the selected address width).
            let value = (self.registers[reg::R0 + numreg] as i32).wrapping_add(modifier);
            self.registers[reg::R0 + numreg] = (value as u32) & r_mask;
        } else if m_reg == 0 {
            self.update_rn_bitreverse(numreg);
        } else if (m_reg & 0xC000) == 0x8000 {
            // Multiple wrap-around modulo: bit 15=1, bit 14=0.
            // Modulo M (power of 2) stored as M-1 in bits 13:0.
            // Unlike standard modulo, supports |Nn| > M (multiple wraps).
            let modulo = (m_reg & 0x3FFF) + 1; // M, power of 2
            let r_val = self.registers[reg::R0 + numreg];
            // Base address = Rn with modulo-sized block bits cleared
            let base = r_val & !(modulo - 1);
            // Offset within block
            let offset = r_val & (modulo - 1);
            // New offset = (offset + modifier) mod M, using wrapping arithmetic
            let new_offset = (offset as i32).wrapping_add(modifier) as u32 & (modulo - 1);
            self.registers[reg::R0 + numreg] = (base | new_offset) & r_mask;
        } else if m_reg <= 0x7FFFFF {
            self.update_rn_modulo(numreg, modifier);
        }
        // else: reserved M register values, do nothing
    }

    /// Bit-reverse carry address update.
    fn update_rn_bitreverse(&mut self, numreg: usize) {
        let r_mask = self.address_register_mask;
        let n_val = self.registers[reg::N0 + numreg] & self.address_register_mask;

        // Count trailing zeros to determine number of bits to reverse.
        // revbits = trailing_zeros(N) + 1, capped at 24 (full 24-bit reversal when N=0).
        let revbits: u32 = if n_val == 0 {
            24
        } else {
            n_val.trailing_zeros() + 1
        }
        .min(self.address_register_mask.count_ones());

        let r_reg = self.registers[reg::R0 + numreg] & r_mask;

        // Reverse lower revbits of Rn
        let high_mask = r_mask.wrapping_shl(revbits) & r_mask;
        let mut value = r_reg & high_mask;
        for i in 0..revbits {
            if r_reg & (1u32 << i) != 0 {
                value |= 1u32 << (revbits - i - 1);
            }
        }

        let revmask = 1u32.wrapping_shl(revbits).wrapping_sub(1);
        value = (value + 1) & revmask;

        // Combine with high bits of Rn
        let r_new = (r_reg & high_mask) | value;

        // Reverse back
        let mut result = r_new & high_mask;
        for i in 0..revbits {
            if r_new & (1u32 << i) != 0 {
                result |= 1u32 << (revbits - i - 1);
            }
        }

        self.registers[reg::R0 + numreg] = result & r_mask;
    }

    /// Modulo address update.
    fn update_rn_modulo(&mut self, numreg: usize, mut modifier: i32) {
        let r_mask = self.address_register_mask;
        let modulo = (self.registers[reg::M0 + numreg] & self.address_register_mask)
            .wrapping_add(1);
        let orig_modifier = modifier;
        let mut bufsize: u32 = 1;
        let mut bufmask: u32 = r_mask;
        while bufsize < modulo {
            bufsize <<= 1;
            bufmask <<= 1;
        }
        bufmask &= r_mask;

        let lobound = self.registers[reg::R0 + numreg] & bufmask;
        let hibound = lobound.wrapping_add(modulo).wrapping_sub(1) & r_mask;

        let mut r_reg = self.registers[reg::R0 + numreg] as i32;

        if orig_modifier > (modulo as i32) {
            let bs = bufsize as i32;
            while modifier > bs {
                r_reg = r_reg.wrapping_add(bufsize as i32);
                modifier = modifier.wrapping_sub(bufsize as i32);
            }
            while modifier < -bs {
                r_reg = r_reg.wrapping_sub(bufsize as i32);
                modifier = modifier.wrapping_add(bufsize as i32);
            }
        }

        r_reg = r_reg.wrapping_add(modifier);

        if orig_modifier != (modulo as i32) {
            if r_reg > (hibound as i32) {
                r_reg = r_reg.wrapping_sub(modulo as i32);
            } else if r_reg < (lobound as i32) {
                r_reg = r_reg.wrapping_add(modulo as i32);
            }
        }

        self.registers[reg::R0 + numreg] = (r_reg as u32) & r_mask;
    }

    /// Write a 24-bit word to the specified memory space.
    pub fn write_memory(&mut self, space: MemSpace, addr: u32, value: u32) {
        let value = value & 0x00FF_FFFF;
        let regions = match space {
            MemSpace::X => &self.map.x_regions,
            MemSpace::Y => &self.map.y_regions,
            MemSpace::P => &self.map.p_regions,
        };
        for region in regions {
            if addr >= region.start && addr < region.end {
                match region.kind {
                    RegionKind::Buffer { base, offset } => {
                        let idx = (addr - region.start + offset) as usize;
                        unsafe {
                            *base.add(idx) = value;
                        }
                    }
                    RegionKind::Callback {
                        opaque, write_fn, ..
                    } => unsafe {
                        write_fn(opaque, addr, value);
                    },
                }
                return;
            }
        }
    }
}

// extern "C" helpers callable from JIT-compiled code

/// Update address register Rn with modulo/bit-reverse support.
/// Called from JIT-compiled code when M\[numreg\] != $FFFFFF (non-linear mode).
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_update_rn(state: *mut DspState, numreg: u32, modifier: i32) {
    let state = unsafe { &mut *state };
    state.update_rn(numreg as usize, modifier);
}

/// Write to SSH register: increments SP and writes SSH, but leaves SSL untouched.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_write_ssh(state: *mut DspState, value: u32) {
    unsafe { &mut *state }.stack_push_ssh(value);
}

/// Read SSH register with stack pop semantics.
/// Returns the popped SSH value.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_read_ssh(state: *mut DspState) -> u32 {
    let state = unsafe { &mut *state };
    let (ssh, _ssl) = state.stack_pop();
    ssh
}

/// Write to SSL register: update stack\[1\]\[SP\].
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_write_ssl(state: *mut DspState, value: u32) {
    let state = unsafe { &mut *state };
    let idx = (state.registers[reg::SP] & 0xF) as usize;
    let extended = state.registers[reg::OMR] & DspState::OMR_SEN != 0;
    let value = if !extended && idx == 0 {
        0
    } else {
        value & REG_MASKS[reg::SSL]
    };
    state.stack[1][idx] = value;
    state.registers[reg::SSL] = value;
}

/// Write to SP register: update SP and recompute SSH/SSL from stack.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_write_sp(state: *mut DspState, value: u32) {
    let state = unsafe { &mut *state };
    let extended = state.registers[reg::OMR] & DspState::OMR_SEN != 0;
    let mask = if extended {
        0x00FF_FFFF
    } else {
        REG_MASKS[reg::SP]
    };
    if !extended {
        let stack_error = state.registers[reg::SP] & (3 << 4);
        if stack_error == 0 && (value & (3 << 4)) != 0 {
            state.interrupts.add(interrupt::STACK_ERROR);
        }
    }
    state.registers[reg::SP] = value & mask;
    // Recompute SSH/SSL from the current stack[SP] position.
    let idx = (state.registers[reg::SP] & 0xF) as usize;
    state.registers[reg::SSH] = state.stack[0][idx];
    state.registers[reg::SSL] = state.stack[1][idx];
}

/// Push a complete 48-bit system-stack entry from JIT-compiled code.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_stack_push(state: *mut DspState, ssh: u32, ssl: u32) {
    unsafe { &mut *state }.stack_push(ssh, ssl);
}

/// Pop a complete 48-bit system-stack entry for JIT-compiled code.
/// SSH occupies bits 31:0 and SSL occupies bits 63:32 of the return value.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_stack_pop(state: *mut DspState) -> u64 {
    let (ssh, ssl) = unsafe { &mut *state }.stack_pop();
    (ssh as u64) | ((ssl as u64) << 32)
}

/// Read memory. Called from JIT-compiled code for runtime-computed addresses.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_read_mem(state: *mut DspState, space: MemSpace, address: u32) -> u32 {
    unsafe { &*state }.read_memory(space, address)
}

/// Write memory. Called from JIT-compiled code for runtime-computed addresses.
/// P-space writes include dirty bitmap tracking for JIT cache invalidation.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_write_mem(
    state: *mut DspState,
    space: MemSpace,
    address: u32,
    value: u32,
) {
    let state = unsafe { &mut *state };
    if space == MemSpace::P {
        let masked = value & 0x00FF_FFFF;
        let old = state.read_memory(MemSpace::P, address);
        if old != masked {
            state.write_memory(MemSpace::P, address, masked);
            state.pram_dirty.mark_dirty(address);
        }
    } else {
        state.write_memory(space, address, value)
    }
}

/// Round a 56-bit accumulator value.
/// Implements convergent rounding (round-to-even) with three scaling modes.
/// The value is stored as a 56-bit signed integer in an i64:
///   bits 55:48 = A2 (sign extension), bits 47:24 = A1, bits 23:0 = A0.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_rnd56(state: *mut DspState, val: i64) -> i64 {
    let state = unsafe { &*state };
    let sr = state.registers[reg::SR];
    let convergent = sr & (1 << sr::RM) == 0; // RM=0: convergent, RM=1: two's complement

    let (r2, r1, r0) = if sr & (1 << sr::S0) != 0 {
        // Scaling mode S0: round at bit 24 (A1 boundary)
        let sum = val.wrapping_add(1 << 24); // add rnd_const = {0, 1, 0}
        let s0 = (sum & 0xFF_FFFF) as u32;
        let mut s1 = ((sum >> 24) & 0xFF_FFFF) as u32;
        let s2 = ((sum >> 48) & 0xFF) as u32;
        if convergent && s0 == 0 && (s1 & 1) == 0 {
            s1 &= 0xFF_FFFF - 0x3;
        }
        s1 &= 0xFF_FFFE;
        (s2, s1, 0u32)
    } else if sr & (1 << sr::S1) != 0 {
        // Scaling mode S1: round at bit 22
        let sum = val.wrapping_add(1 << 22);
        let mut s0 = (sum & 0xFF_FFFF) as u32;
        let s1 = ((sum >> 24) & 0xFF_FFFF) as u32;
        let s2 = ((sum >> 48) & 0xFF) as u32;
        if convergent && (s0 & 0x7F_FFFF) == 0 {
            s0 = 0;
        }
        s0 &= 0x80_0000;
        (s2, s1, s0)
    } else {
        // No scaling: round at bit 23 (A0/A1 boundary)
        let sum = val.wrapping_add(1 << 23);
        let s0 = (sum & 0xFF_FFFF) as u32;
        let mut s1 = ((sum >> 24) & 0xFF_FFFF) as u32;
        let s2 = ((sum >> 48) & 0xFF) as u32;
        if convergent && s0 == 0 {
            s1 &= 0xFF_FFFE;
        }
        (s2, s1, 0u32)
    };

    ((r2 as i64) << 48) | ((r1 as i64) << 24) | (r0 as i64)
}

/// Update E, U, N, Z flags in SR from a 56-bit accumulator value.
/// Handles all three scaling modes (S1:S0 in SR).
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_update_nz(state: *mut DspState, acc_val: i64) {
    let state = unsafe { &mut *state };
    let sr = state.registers[reg::SR];

    let reg0 = ((acc_val >> 48) & 0xFF) as u32; // extension byte
    let reg1 = ((acc_val >> 24) & 0xFF_FFFF) as u32; // MSP

    let scaling = (sr >> sr::S0) & 3;

    let (e, u) = match scaling {
        0 => {
            // No scaling
            let val_e = ((reg0 << 1) | (reg1 >> 23)) & 0x1FF;
            let e = val_e != 0 && val_e != 0x1FF;
            let bits = reg1 & 0xC0_0000;
            let u = bits == 0 || bits == 0xC0_0000;
            (e, u)
        }
        1 => {
            // Scale down (S1:S0=01)
            let e = reg0 != 0 && reg0 != 0xFF;
            let val = ((reg0 << 1) | (reg1 >> 23)) & 3;
            let u = val == 0 || val == 3;
            (e, u)
        }
        2 => {
            // Scale up (S1:S0=10)
            let val_e = ((reg0 << 2) | (reg1 >> 22)) & 0x3FF;
            let e = val_e != 0 && val_e != 0x3FF;
            let bits = reg1 & 0x60_0000;
            let u = bits == 0 || bits == 0x60_0000;
            (e, u)
        }
        _ => (false, false), // scaling=3: no change
    };

    let n = (acc_val >> 55) & 1 != 0;
    let z = (acc_val & 0x00FF_FFFF_FFFF_FFFF) == 0;

    let clear_mask = !((1u32 << sr::E) | (1u32 << sr::U) | (1u32 << sr::N) | (1u32 << sr::Z));
    let mut new_sr = sr & clear_mask;
    if e {
        new_sr |= 1 << sr::E;
    }
    if u {
        new_sr |= 1 << sr::U;
    }
    if n {
        new_sr |= 1 << sr::N;
    }
    if z {
        new_sr |= 1 << sr::Z;
    }
    state.registers[reg::SR] = new_sr;
}

/// Arithmetic Saturation Mode: clamp a 56-bit accumulator if SM=1.
/// Returns the result with bit 56 set if saturation was needed (needs_sat flag).
/// Bits 55:0 contain the (possibly clamped) value.
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
pub unsafe extern "C" fn jit_saturate_sm(state: *mut DspState, val: i64) -> i64 {
    let state = unsafe { &*state };
    let sr = state.registers[reg::SR];
    if sr & (1 << sr::SM) == 0 {
        return val; // SM=0: no saturation, needs_sat=0 (bit 56 clear)
    }
    let b55 = (val >> 55) & 1;
    let b48 = (val >> 48) & 1;
    let b47 = (val >> 47) & 1;
    let mismatch = (b55 ^ b48) | (b48 ^ b47);
    if mismatch == 0 {
        return val; // No saturation needed
    }
    // Saturate: bit 55 = 0 -> max positive, 1 -> max negative
    let saturated = if b55 != 0 {
        0x00FF_8000_0000_0000_u64 as i64
    } else {
        0x0000_7FFF_FFFF_FFFF_i64
    };
    saturated | (1i64 << 56) // Set needs_sat flag in bit 56
}

/// Read accumulator as 24-bit value with scaling, limiting, and S flag update.
///
/// Returns the 24-bit result in bits \[23:0\]. Bit 24 is the `no_limit` flag
/// (1 = value was not clamped). Updates SR.L and SR.S in-place.
///
/// `acc_idx`: 0 = A, 1 = B (index into sub-register triples)
///
/// # Safety
/// `state` must be a valid pointer to a `DspState`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jit_read_accu24(state: *mut DspState, acc_idx: u32) -> u32 {
    let state = unsafe { &mut *state };
    let (a2, a1, a0) = if acc_idx == 0 {
        (
            state.registers[reg::A2],
            state.registers[reg::A1],
            state.registers[reg::A0],
        )
    } else {
        (
            state.registers[reg::B2],
            state.registers[reg::B1],
            state.registers[reg::B0],
        )
    };

    let sr = state.registers[reg::SR];
    let scaling = (sr >> sr::S0) & 3;

    // Apply data shifter
    let combined = (a2 << 24) | a1;
    let value = match scaling {
        1 => combined >> 1,                      // scale down
        2 => (combined << 1) | ((a0 >> 23) & 1), // scale up
        _ => combined,                           // no scaling
    } & 0x00FF_FFFF;

    // Limiting check
    let ok_pos = a2 == 0 && value <= 0x7F_FFFF;
    let ok_neg = a2 == 0xFF && value >= 0x80_0000;
    let mut no_limit = ok_pos || ok_neg;

    // Scale-up fix: bit 47 (A1[23]) must match A2 sign at extension boundary
    if scaling == 2 {
        let a1_bit23 = (a1 >> 23) & 1;
        let ok_pos_s2 = a2 == 0 && value <= 0x7F_FFFF && a1_bit23 == 0;
        let ok_neg_s2 = a2 == 0xFF && value >= 0x80_0000 && a1_bit23 != 0;
        no_limit = ok_pos_s2 || ok_neg_s2;
    }

    let result = if no_limit {
        value
    } else {
        // Clamp: negative (A2 bit 7 set) -> 0x800000, positive -> 0x7FFFFF
        if a2 & 0x80 != 0 { 0x80_0000 } else { 0x7F_FFFF }
    };

    // Update SR: set L if limited, compute S flag (sticky data growth)
    let mut new_sr = sr;
    if !no_limit {
        new_sr |= 1 << sr::L;
    }

    // S flag: adjacent bits differ in the unscaled accumulator
    let acc_packed =
        ((a2 as u64 & 0xFF) << 48) | ((a1 as u64 & 0xFF_FFFF) << 24) | (a0 as u64 & 0xFF_FFFF);
    let s_bit = match scaling {
        1 => ((acc_packed >> 45) ^ (acc_packed >> 44)) & 1, // scale down
        2 => ((acc_packed >> 47) ^ (acc_packed >> 46)) & 1, // scale up
        _ => ((acc_packed >> 46) ^ (acc_packed >> 45)) & 1, // no scaling
    };
    if s_bit != 0 {
        new_sr |= 1 << sr::S;
    }
    state.registers[reg::SR] = new_sr;

    result | (if no_limit { 1 << 24 } else { 0 })
}

impl Default for DspState {
    fn default() -> Self {
        Self::new(MemoryMap::default())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    pub const PRAM_SIZE: usize = 4096;
    pub const XRAM_SIZE: usize = 4096;
    pub const YRAM_SIZE: usize = 2048;

    #[test]
    fn test_new_state_is_zeroed() {
        let state = DspState::new(MemoryMap::default());
        assert_eq!(state.pc, 0);
        assert_eq!(state.cycle_count, 0);
        assert!(!state.halt_requested);
        assert_eq!(state.registers[reg::A1], 0);
    }

    #[test]
    fn test_memory_read_write() {
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut state = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        state.write_memory(MemSpace::X, 3, 0x123456);
        assert_eq!(state.read_memory(MemSpace::X, 3), 0x123456);

        state.write_memory(MemSpace::Y, 100, 0xABCDEF);
        assert_eq!(state.read_memory(MemSpace::Y, 100), 0xABCDEF);

        state.write_memory(MemSpace::P, 0x40, 0x0AF080);
        assert_eq!(state.read_memory(MemSpace::P, 0x40), 0x0AF080);
    }

    #[test]
    fn test_memory_24bit_mask() {
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut state = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        state.write_memory(MemSpace::X, 0, 0xFF123456);
        assert_eq!(state.read_memory(MemSpace::X, 0), 0x123456);
    }

    #[test]
    fn test_out_of_bounds_read_returns_zero() {
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let state = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        assert_eq!(state.read_memory(MemSpace::X, XRAM_SIZE as u32 + 100), 0);
        assert_eq!(state.read_memory(MemSpace::Y, YRAM_SIZE as u32 + 100), 0);
        assert_eq!(state.read_memory(MemSpace::P, PRAM_SIZE as u32 + 100), 0);
    }

    // Bit-reverse addressing

    #[test]
    fn test_bitreverse_basic() {
        // M0=0 means bit-reverse mode. N0 determines reversal width.
        // N0=8 -> 4 bits (trailing zeros in 8=0b1000 -> revbits=3+1=4).
        // R0=0 -> reversed increment: 0->8->4->12->2->...
        let mut state = DspState::new(MemoryMap::default());
        state.registers[reg::M0] = 0;
        state.registers[reg::N0] = 8;
        state.registers[reg::R0] = 0;

        // Step through the bit-reverse sequence
        let expected = [8, 4, 12, 2, 10, 6, 14, 1];
        for &exp in &expected {
            state.update_rn(0, 1); // modifier is ignored for bit-reverse
            assert_eq!(
                state.registers[reg::R0],
                exp,
                "expected R0={exp} after bit-reverse step"
            );
        }
    }

    #[test]
    fn test_bitreverse_n0_4() {
        // N0=4=0b100 -> revbits=2+1=3, reversing 3 bits
        // Sequence from 0: 4,2,6,1,5,3,7,0
        let mut state = DspState::new(MemoryMap::default());
        state.registers[reg::M0] = 0;
        state.registers[reg::N0] = 4;
        state.registers[reg::R0] = 0;

        let expected = [4, 2, 6, 1, 5, 3, 7, 0];
        for &exp in &expected {
            state.update_rn(0, 1);
            assert_eq!(state.registers[reg::R0], exp);
        }
    }

    // Interrupt priority selection

    #[test]
    fn test_interrupt_priority_level3_wins() {
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        // Place NOP at interrupt vector for ILLEGAL (0x3E, 0x3F)
        pram[0x3E] = 0x000000; // nop
        pram[0x3F] = 0x000000; // nop
        // Level 3 (non-maskable) should always be dispatched regardless of IPL mask
        let mut state = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        state.interrupts.vector_addr = 0xFFFF;
        state.interrupts.saved_pc = 0xFFFF;
        // Set IPL mask to max (I0=1, I1=1 -> IPL=3)
        state.registers[reg::SR] = 3 << sr::I0;
        // Set up a level-3 interrupt (ILLEGAL, ipl=3)
        state.interrupts.ipl[interrupt::ILLEGAL] = 3;
        state.interrupts.add(interrupt::ILLEGAL);

        assert!(state.interrupts.has_pending());
        state.process_pending_interrupts();
        // Interrupt should be dispatched (pipeline started)
        assert!(!state.interrupts.has_pending());
        assert_eq!(state.interrupts.state, InterruptState::Fast);
    }

    #[test]
    fn test_interrupt_masked_by_ipl() {
        // TRAP at IPL 1 should be masked when SR IPL mask is 2
        let mut state = DspState::new(MemoryMap::default());
        state.interrupts.vector_addr = 0xFFFF;
        state.interrupts.saved_pc = 0xFFFF;
        state.registers[reg::SR] = 2 << sr::I0;
        state.interrupts.ipl[interrupt::TRAP] = 1;
        state.interrupts.add(interrupt::TRAP);

        state.process_pending_interrupts();
        assert!(state.interrupts.has_pending());
        assert!(state.interrupts.pending(interrupt::TRAP));
    }

    #[test]
    fn test_interrupt_highest_priority_wins() {
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        // Place NOPs at ILLEGAL vector so dispatch succeeds
        let vec_addr = interrupt::vector_addr(interrupt::ILLEGAL) as usize;
        pram[vec_addr] = 0x000000;
        pram[vec_addr + 1] = 0x000000;
        let mut state = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        state.interrupts.vector_addr = 0xFFFF;
        state.interrupts.saved_pc = 0xFFFF;
        state.registers[reg::SR] = 0;
        // TRAP at IPL 1, ILLEGAL at IPL 2
        state.interrupts.ipl[interrupt::TRAP] = 1;
        state.interrupts.ipl[interrupt::ILLEGAL] = 2;
        state.interrupts.add(interrupt::TRAP);
        state.interrupts.add(interrupt::ILLEGAL);

        state.process_pending_interrupts();
        // ILLEGAL (IPL 2) should be dispatched first
        assert!(!state.interrupts.pending(interrupt::ILLEGAL));
        assert!(state.interrupts.pending(interrupt::TRAP));
        assert!(state.interrupts.has_pending());
    }

    // jit_rnd56 convergent rounding

    fn pack_acc56(a2: u32, a1: u32, a0: u32) -> i64 {
        ((a2 as i64 & 0xFF) << 48) | ((a1 as i64 & 0xFF_FFFF) << 24) | (a0 as i64 & 0xFF_FFFF)
    }

    #[test]
    fn test_rnd56_default_round_up() {
        let mut state = DspState::new(MemoryMap::default());
        // A0 > 0x800000 -> round up
        let val = pack_acc56(0x00, 0x400000, 0x800001);
        let result = unsafe { jit_rnd56(&mut state as *mut DspState, val) };
        let r1 = ((result >> 24) & 0xFF_FFFF) as u32;
        let r0 = (result & 0xFF_FFFF) as u32;
        assert_eq!(r1, 0x400001, "A1 should round up");
        assert_eq!(r0, 0, "A0 should be cleared");
    }

    #[test]
    fn test_rnd56_default_round_down() {
        let mut state = DspState::new(MemoryMap::default());
        // A0 < 0x800000 -> round down (truncate)
        let val = pack_acc56(0x00, 0x400000, 0x7FFFFF);
        let result = unsafe { jit_rnd56(&mut state as *mut DspState, val) };
        let r1 = ((result >> 24) & 0xFF_FFFF) as u32;
        assert_eq!(r1, 0x400000, "A1 should stay");
    }

    #[test]
    fn test_rnd56_convergent_half_even() {
        let mut state = DspState::new(MemoryMap::default());
        // Exactly half (A0=0x800000), A1 bit 0 = 0 -> round down (convergent)
        let val = pack_acc56(0x00, 0x400000, 0x800000);
        let result = unsafe { jit_rnd56(&mut state as *mut DspState, val) };
        let r1 = ((result >> 24) & 0xFF_FFFF) as u32;
        assert_eq!(r1, 0x400000, "even A1 -> round down");
    }

    #[test]
    fn test_rnd56_convergent_half_odd() {
        let mut state = DspState::new(MemoryMap::default());
        // Exactly half (A0=0x800000), A1 bit 0 = 1 -> round up (convergent)
        let val = pack_acc56(0x00, 0x400001, 0x800000);
        let result = unsafe { jit_rnd56(&mut state as *mut DspState, val) };
        let r1 = ((result >> 24) & 0xFF_FFFF) as u32;
        assert_eq!(r1, 0x400002, "odd A1 -> round up");
    }

    #[test]
    fn test_rnd56_s0_scaling() {
        let mut state = DspState::new(MemoryMap::default());
        state.registers[reg::SR] = 1 << sr::S0;
        let val = pack_acc56(0x00, 0x400000, 0x800000);
        let result = unsafe { jit_rnd56(&mut state as *mut DspState, val) };
        // S0 mode rounds at bit 24 (A1 boundary) - result should clear A0
        let r0 = (result & 0xFF_FFFF) as u32;
        assert_eq!(r0, 0);
    }

    #[test]
    fn test_rnd56_s1_scaling() {
        let mut state = DspState::new(MemoryMap::default());
        state.registers[reg::SR] = 1 << sr::S1;
        let val = pack_acc56(0x00, 0x400001, 0xC00000);
        let result = unsafe { jit_rnd56(&mut state as *mut DspState, val) };
        // S1 mode rounds at bit 22
        let r0 = (result & 0xFF_FFFF) as u32;
        // A0 should be masked to just bit 23
        assert_eq!(
            r0 & 0x7F_FFFF,
            0,
            "lower 23 bits of A0 should be cleared in S1 mode"
        );
    }

    // ---- read_pram / p_space_end ----

    #[test]
    fn test_read_pram_buffer() {
        let mut pram = [0u32; 4];
        pram[0] = 0xABCDEF;
        pram[1] = 0xFFFFFF;
        pram[2] = 0x123456;
        let map = MemoryMap {
            p_regions: vec![MemoryRegion {
                start: 0,
                end: 4,
                kind: RegionKind::Buffer {
                    base: pram.as_mut_ptr(),
                    offset: 0,
                },
            }],
            ..Default::default()
        };
        assert_eq!(map.read_pram(0), 0xABCDEF);
        assert_eq!(map.read_pram(1), 0xFFFFFF);
        assert_eq!(map.read_pram(2), 0x123456);
        assert_eq!(map.read_pram(4), 0); // out of range
        assert_eq!(map.p_space_end(), 4);
    }

    #[test]
    fn test_read_pram_callback() {
        unsafe extern "C" fn cb_read(_opaque: *mut std::ffi::c_void, addr: u32) -> u32 {
            // Return addr * 0x111 as a recognizable pattern
            addr * 0x111
        }
        unsafe extern "C" fn cb_write(_opaque: *mut std::ffi::c_void, _addr: u32, _val: u32) {}
        let map = MemoryMap {
            p_regions: vec![MemoryRegion {
                start: 0,
                end: 8,
                kind: RegionKind::Callback {
                    opaque: std::ptr::null_mut(),
                    read_fn: cb_read,
                    write_fn: cb_write,
                },
            }],
            ..Default::default()
        };
        assert_eq!(map.read_pram(0), 0);
        assert_eq!(map.read_pram(3), 0x333);
        assert_eq!(map.read_pram(7), 0x777);
        assert_eq!(map.read_pram(8), 0); // out of range
        assert_eq!(map.p_space_end(), 8);
    }

    #[test]
    fn test_read_pram_masks_to_24_bits() {
        unsafe extern "C" fn cb_read(_opaque: *mut std::ffi::c_void, _addr: u32) -> u32 {
            0xFF123456 // upper byte should be masked off
        }
        unsafe extern "C" fn cb_write(_opaque: *mut std::ffi::c_void, _addr: u32, _val: u32) {}
        let map = MemoryMap {
            p_regions: vec![MemoryRegion {
                start: 0,
                end: 1,
                kind: RegionKind::Callback {
                    opaque: std::ptr::null_mut(),
                    read_fn: cb_read,
                    write_fn: cb_write,
                },
            }],
            ..Default::default()
        };
        assert_eq!(map.read_pram(0), 0x123456);
    }

    #[test]
    fn test_read_pram_empty_map() {
        let map = MemoryMap::default();
        assert_eq!(map.read_pram(0), 0);
        assert_eq!(map.p_space_end(), 0);
    }

    #[test]
    fn test_interrupt_state_from() {
        assert!(matches!(InterruptState::from(0), InterruptState::None));
        assert!(matches!(InterruptState::from(1), InterruptState::Fast));
        assert!(matches!(InterruptState::from(2), InterruptState::Long));
        assert!(matches!(InterruptState::from(255), InterruptState::None));
    }

    #[test]
    fn test_interrupt_state_display() {
        assert_eq!(format!("{}", InterruptState::None), "none");
        assert_eq!(format!("{}", InterruptState::Fast), "fast");
        assert_eq!(format!("{}", InterruptState::Long), "long");
    }

    #[test]
    fn test_dirty_range_middle_words() {
        let mut cache = PramDirtyBitmap::new(4096);
        // Mark a single bit in the middle word (word index 1, bit 64+10=74)
        cache.mark_dirty(74);
        assert!(cache.is_range_dirty(0, 192));
        assert!(!cache.is_range_dirty(0, 64));
        assert!(!cache.is_range_dirty(128, 192));
    }

    #[test]
    fn test_clear_dirty_range_multi_word() {
        let mut cache = PramDirtyBitmap::new(4096);
        // Mark dirty bits across 3 words
        cache.mark_dirty(10); // word 0
        cache.mark_dirty(74); // word 1
        cache.mark_dirty(140); // word 2
        assert!(cache.is_range_dirty(0, 192));
        // Clear the whole range
        cache.clear_dirty_range(0, 192);
        assert!(!cache.is_range_dirty(0, 192));
    }

    #[test]
    fn test_modulo_negative_wrap() {
        // M0 = 3 means modulo 4. Large negative modifier triggers the
        // while (modifier < -bufsize) loop.
        let mut state = DspState::new(MemoryMap::default());
        state.registers[reg::M0] = 3; // modulo = M+1 = 4
        state.registers[reg::R0] = 6; // start address
        state.registers[reg::N0] = 0;
        state.update_rn(0, -10);
        let r = state.registers[reg::R0];
        assert!(r <= 7, "R0 should be within modulo buffer bounds, got {r}");
    }
}
