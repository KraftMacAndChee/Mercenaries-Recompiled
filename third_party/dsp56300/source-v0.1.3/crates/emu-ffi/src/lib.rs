//! C FFI bindings for the dsp56300 crate.

/// Register file indices (6-bit DDDDDD encoding per DSP56300 instruction set).
#[repr(u32)]
pub enum CReg {
    X0 = 0x04,
    X1 = 0x05,
    Y0 = 0x06,
    Y1 = 0x07,
    A0 = 0x08,
    B0 = 0x09,
    A2 = 0x0A,
    B2 = 0x0B,
    A1 = 0x0C,
    B1 = 0x0D,
    R0 = 0x10,
    R1 = 0x11,
    R2 = 0x12,
    R3 = 0x13,
    R4 = 0x14,
    R5 = 0x15,
    R6 = 0x16,
    R7 = 0x17,
    N0 = 0x18,
    N1 = 0x19,
    N2 = 0x1A,
    N3 = 0x1B,
    N4 = 0x1C,
    N5 = 0x1D,
    N6 = 0x1E,
    N7 = 0x1F,
    M0 = 0x20,
    M1 = 0x21,
    M2 = 0x22,
    M3 = 0x23,
    M4 = 0x24,
    M5 = 0x25,
    M6 = 0x26,
    M7 = 0x27,
    Ep = 0x2A,
    Vba = 0x30,
    Sc = 0x31,
    Sz = 0x38,
    Sr = 0x39,
    Omr = 0x3A,
    Sp = 0x3B,
    Ssh = 0x3C,
    Ssl = 0x3D,
    La = 0x3E,
    Lc = 0x3F,
    Count = 0x40,
}

use dsp56300_emu::core::{
    CreateInfo, DspState, InterruptState, MemSpace, MemoryMap, MemoryRegion,
    PowerState, RegionKind, reg,
};
use dsp56300_emu::jit::JitEngine;
use std::ffi::c_void;

/// Region kind tag for C FFI.
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum CRegion {
    Buffer = 0,
    Callback = 1,
}

/// Buffer variant data for C FFI.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CBufferRegion {
    pub base: *mut u32,
    pub offset: u32,
}

/// Callback variant data for C FFI.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CCallbackRegion {
    pub opaque: *mut c_void,
    pub read: Option<unsafe extern "C" fn(*mut c_void, u32) -> u32>,
    pub write: Option<unsafe extern "C" fn(*mut c_void, u32, u32)>,
}

/// Union payload for CMemoryRegion.
#[repr(C)]
#[derive(Clone, Copy)]
pub union CRegionData {
    pub buffer: CBufferRegion,
    pub callback: CCallbackRegion,
}

/// A single memory region descriptor (C FFI).
///
/// Tagged union: check `kind` to determine which union field is valid.
#[repr(C)]
pub struct CMemoryRegion {
    pub start: u32,
    pub end: u32,
    pub kind: CRegion,
    pub data: CRegionData,
}

/// Memory region configuration for X, Y, and P address spaces (C FFI).
#[repr(C)]
pub struct CMemoryMap {
    pub x_regions: *const CMemoryRegion,
    pub x_count: u32,
    pub y_regions: *const CMemoryRegion,
    pub y_count: u32,
    pub p_regions: *const CMemoryRegion,
    pub p_count: u32,
}

/// Initialization parameters for `dsp56300_create` (C FFI).
#[repr(C)]
pub struct CCreateInfo {
    pub memory_map: CMemoryMap,
    /// R/N/M register mask. Zero selects the generic 24-bit DSP56300 width;
    /// MCPX callers set $0000FFFF.
    pub address_register_mask: u32,
}

/// Convert a C region array into a Vec<MemoryRegion>.
unsafe fn convert_regions(ptr: *const CMemoryRegion, count: u32) -> Vec<MemoryRegion> {
    if ptr.is_null() || count == 0 {
        return Vec::new();
    }
    let slice = unsafe { std::slice::from_raw_parts(ptr, count as usize) };
    slice
        .iter()
        .map(|r| {
            let kind = match r.kind {
                CRegion::Buffer => {
                    let b = unsafe { r.data.buffer };
                    RegionKind::Buffer {
                        base: b.base,
                        offset: b.offset,
                    }
                }
                CRegion::Callback => {
                    let cb = unsafe { r.data.callback };
                    RegionKind::Callback {
                        opaque: cb.opaque,
                        read_fn: cb.read.expect("callback read must not be null"),
                        write_fn: cb.write.expect("callback write must not be null"),
                    }
                }
            };
            MemoryRegion {
                start: r.start,
                end: r.end,
                kind,
            }
        })
        .collect()
}

/// Convert a CCreateInfo into a MemoryMap.
unsafe fn convert_create_info(info: *const CCreateInfo) -> CreateInfo {
    let info = unsafe { &*info };
    let m = &info.memory_map;
    CreateInfo {
        memory_map: MemoryMap {
            x_regions: unsafe { convert_regions(m.x_regions, m.x_count) },
            y_regions: unsafe { convert_regions(m.y_regions, m.y_count) },
            p_regions: unsafe { convert_regions(m.p_regions, m.p_count) },
        },
        address_register_mask: info.address_register_mask,
    }
}

/// Opaque handle to a DSP instance with JIT engine.
pub struct DspJit {
    state: DspState,
    jit: JitEngine,
}

/// FFI-safe memory space enum.
#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CMemSpace {
    X = 0,
    Y = 1,
    P = 2,
}

impl CMemSpace {
    fn to_core(self) -> MemSpace {
        match self {
            CMemSpace::X => MemSpace::X,
            CMemSpace::Y => MemSpace::Y,
            CMemSpace::P => MemSpace::P,
        }
    }
}

/// Create a new DSP JIT instance.
///
/// The `info` parameter provides the memory map and callback configuration.
/// Buffer pointers in the map must remain valid for the lifetime of the
/// returned `DspJit`.
///
/// # Safety
/// All buffer and callback pointers in `info` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_create(info: *const CCreateInfo) -> *mut DspJit {
    let create_info = if info.is_null() {
        CreateInfo::default()
    } else {
        unsafe { convert_create_info(info) }
    };

    let pram_size = create_info.memory_map.p_space_end() as usize;
    let state = DspState::new(create_info);
    let address_register_mask = state.address_register_mask;

    let dsp = Box::new(DspJit {
        state,
        jit: JitEngine::new_with_address_register_mask(pram_size, address_register_mask),
    });
    Box::into_raw(dsp)
}

/// Destroy a DSP JIT instance.
///
/// # Safety
/// `dsp` must be a valid pointer returned by `dsp56300_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_destroy(dsp: *mut DspJit) {
    if !dsp.is_null() {
        drop(unsafe { Box::from_raw(dsp) });
    }
}

/// Dump block execution profile to `path`. Enables profiling on first call.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
/// `path` must be a valid null-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_dump_profile(dsp: *mut DspJit, path: *const std::ffi::c_char) {
    let dsp = unsafe { &mut *dsp };
    dsp.jit.enable_profiling();
    let path = unsafe { std::ffi::CStr::from_ptr(path) }.to_str();
    let fallback = std::env::temp_dir().join("dsp_profile.txt");
    let path = path.unwrap_or_else(|_| fallback.to_str().unwrap());
    dsp.jit.dump_profile(&dsp.state.map, path);
}

/// Enable perf map output for `perf record` profiling.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_enable_perf_map(dsp: *mut DspJit) {
    let dsp = unsafe { &mut *dsp };
    dsp.jit.enable_perf_map();
}

/// Reset the DSP to its initial state, preserving memory map and callbacks.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_reset(dsp: *mut DspJit) {
    let dsp = unsafe { &mut *dsp };
    let map = std::mem::take(&mut dsp.state.map);
    let address_register_mask = dsp.state.address_register_mask;
    dsp.state = DspState::new(CreateInfo {
        memory_map: map,
        address_register_mask,
    });
    dsp.jit.invalidate_cache();
}

/// Execute a single instruction.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_step(dsp: *mut DspJit) {
    let dsp = unsafe { &mut *dsp };
    dsp.state.execute_one(&mut dsp.jit);
}

/// Run for the given number of cycles.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_run(dsp: *mut DspJit, cycles: i32) {
    let dsp = unsafe { &mut *dsp };
    dsp.state.run(&mut dsp.jit, cycles);
}

/// Check if a halt has been requested.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_halt_requested(dsp: *const DspJit) -> bool {
    let dsp = unsafe { &*dsp };
    dsp.state.halt_requested
}

/// Request (or clear) a halt. When `halt` is true, also sets
/// `exit_requested` so the currently executing block returns to the
/// run loop.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_set_halt_requested(dsp: *mut DspJit, halt: bool) {
    let dsp = unsafe { &mut *dsp };
    dsp.state.halt_requested = halt;
    if halt {
        dsp.state.exit_requested = true;
    }
}

/// Get the current cycle count.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_cycle_count(dsp: *const DspJit) -> u32 {
    let dsp = unsafe { &*dsp };
    dsp.state.cycle_count
}

/// Set the cycle count.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_set_cycle_count(dsp: *mut DspJit, count: u32) {
    let dsp = unsafe { &mut *dsp };
    dsp.state.cycle_count = count;
}

/// Invalidate the JIT block cache. Call when P-memory is modified externally.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_invalidate_cache(dsp: *mut DspJit) {
    let dsp = unsafe { &mut *dsp };
    dsp.jit.invalidate_cache();
}

/// Read a word from DSP memory.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_read_memory(
    dsp: *const DspJit,
    space: CMemSpace,
    addr: u32,
) -> u32 {
    let dsp = unsafe { &*dsp };
    dsp.state.read_memory(space.to_core(), addr)
}

/// Write a word to DSP memory.
///
/// If writing to P-space, automatically marks the dirty bitmap for JIT
/// cache invalidation. Also logs writes for shadow comparison mode.
///
/// # Safety
/// `dsp` must be a valid pointer to a `DspJit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_write_memory(
    dsp: *mut DspJit,
    space: CMemSpace,
    addr: u32,
    value: u32,
) {
    let dsp = unsafe { &mut *dsp };
    let s = space.to_core();
    if s == MemSpace::P {
        let masked = value & 0x00FF_FFFF;
        let old = dsp.state.map.read_pram(addr);
        if old != masked {
            dsp.state.pram_dirty.mark_dirty(addr);
        }
    }
    dsp.state.write_memory(s, addr, value);
}

// ================================================================
// State accessors for sync / engine switching
// ================================================================

/// Interrupt pipeline state for save/restore.
#[repr(C)]
pub struct CInterruptState {
    pub pending_bits: [u64; 2],
    pub pipeline_stage: u8,
    pub vector_addr: u32,
    pub saved_pc: u32,
    pub state: u8,
    pub ipl: [i8; 128],
    pub ipl_to_raise: u8,
}

/// Bulk state for save/restore, including registers and stack.
#[repr(C)]
pub struct CDspState {
    pub pc: u32,
    pub pc_advance: u32,
    pub pc_on_rep: bool,
    pub cycle_count: u32,
    pub cycle_budget: i32,
    pub interrupts: CInterruptState,
    pub loop_rep: bool,
    pub halt_requested: bool,
    pub power_state: u8,
    pub registers: [u32; 64],
    pub stack: [[u32; 16]; 2],
}

/// Copy all scalar sync state from the JIT into `out`.
///
/// # Safety
/// `dsp` and `out` must be valid non-null pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_get_state(dsp: *const DspJit, out: *mut CDspState) {
    let dsp = unsafe { &*dsp };
    let out = unsafe { &mut *out };
    let s = &dsp.state;
    out.pc = s.pc;
    out.pc_advance = s.pc_advance;
    out.pc_on_rep = s.pc_on_rep;
    out.cycle_count = s.cycle_count;
    out.loop_rep = s.loop_rep;
    out.cycle_budget = s.cycle_budget;
    out.interrupts.ipl = s.interrupts.ipl;
    out.interrupts.state = s.interrupts.state as u8;
    out.interrupts.vector_addr = s.interrupts.vector_addr;
    out.interrupts.saved_pc = s.interrupts.saved_pc;
    out.interrupts.ipl_to_raise = s.interrupts.ipl_to_raise;
    out.interrupts.pipeline_stage = s.interrupts.pipeline_stage;
    out.interrupts.pending_bits = s.interrupts.pending_bits;
    out.halt_requested = s.halt_requested;
    out.power_state = s.power_state as u8;
    out.registers = s.registers;
    out.stack = s.stack;
}

/// Apply all scalar sync state from `src` into the JIT.
///
/// # Safety
/// `dsp` and `src` must be valid non-null pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dsp56300_set_state(dsp: *mut DspJit, src: *const CDspState) {
    let dsp = unsafe { &mut *dsp };
    let src = unsafe { &*src };
    let s = &mut dsp.state;
    s.pc = src.pc;
    s.pc_advance = src.pc_advance;
    s.pc_on_rep = src.pc_on_rep;
    s.cycle_count = src.cycle_count;
    s.loop_rep = src.loop_rep;
    s.cycle_budget = src.cycle_budget;
    s.interrupts.ipl = src.interrupts.ipl;
    s.interrupts.state = InterruptState::from(src.interrupts.state);
    s.interrupts.vector_addr = src.interrupts.vector_addr;
    s.interrupts.saved_pc = src.interrupts.saved_pc;
    s.interrupts.ipl_to_raise = src.interrupts.ipl_to_raise;
    s.interrupts.pipeline_stage = src.interrupts.pipeline_stage;
    s.interrupts.pending_bits = src.interrupts.pending_bits;
    s.halt_requested = src.halt_requested;
    s.power_state = PowerState::from(src.power_state);
    s.registers = src.registers;
    for index in reg::R0..=reg::M7 {
        s.registers[index] &= s.address_register_mask;
    }
    s.stack = src.stack;
}

// Compiled by build.rs from examples/quickstart.c; linked into the lib test
// binary via cargo:rustc-link-lib (build.rs link directives apply to lib tests
// but not to integration tests for staticlib crates).
#[cfg(test)]
unsafe extern "C" {
    fn quickstart_test() -> i32;
}

#[cfg(test)]
mod tests {
    use super::*;
    pub const PRAM_SIZE: usize = 4096;
    pub const XRAM_SIZE: usize = 4096;
    pub const YRAM_SIZE: usize = 2048;

    /// Helper: create a DspJit via FFI with xram/yram/pram buffer regions.
    /// Buffers are leaked (acceptable in tests).
    unsafe fn make_dsp() -> *mut DspJit {
        unsafe { make_dsp_with_address_register_mask(0) }
    }

    unsafe fn make_dsp_with_address_register_mask(
        address_register_mask: u32,
    ) -> *mut DspJit {
        let xram = Box::leak(vec![0u32; XRAM_SIZE].into_boxed_slice());
        let yram = Box::leak(vec![0u32; YRAM_SIZE].into_boxed_slice());
        let pram = Box::leak(vec![0u32; PRAM_SIZE].into_boxed_slice());

        let x_regions = [CMemoryRegion {
            start: 0,
            end: XRAM_SIZE as u32,
            kind: CRegion::Buffer,
            data: CRegionData {
                buffer: CBufferRegion {
                    base: xram.as_mut_ptr(),
                    offset: 0,
                },
            },
        }];
        let y_regions = [CMemoryRegion {
            start: 0,
            end: YRAM_SIZE as u32,
            kind: CRegion::Buffer,
            data: CRegionData {
                buffer: CBufferRegion {
                    base: yram.as_mut_ptr(),
                    offset: 0,
                },
            },
        }];
        let p_regions = [CMemoryRegion {
            start: 0,
            end: PRAM_SIZE as u32,
            kind: CRegion::Buffer,
            data: CRegionData {
                buffer: CBufferRegion {
                    base: pram.as_mut_ptr(),
                    offset: 0,
                },
            },
        }];

        let info = CCreateInfo {
            memory_map: CMemoryMap {
                x_regions: x_regions.as_ptr(),
                x_count: 1,
                y_regions: y_regions.as_ptr(),
                y_count: 1,
                p_regions: p_regions.as_ptr(),
                p_count: 1,
            },
            address_register_mask,
        };
        unsafe { dsp56300_create(&info) }
    }

    #[test]
    fn test_c_state_layout_matches_generated_header() {
        assert_eq!(std::mem::size_of::<CInterruptState>(), 160);
        assert_eq!(std::mem::size_of::<CDspState>(), 576);
        assert_eq!(std::mem::offset_of!(CDspState, interrupts), 24);
        assert_eq!(std::mem::offset_of!(CDspState, registers), 188);
        assert_eq!(std::mem::offset_of!(CDspState, stack), 444);
    }

    #[test]
    fn test_lifecycle() {
        unsafe {
            let dsp = make_dsp();
            assert!(!dsp.is_null());
            assert_eq!((*dsp).state.pc, 0);
            assert!(!dsp56300_halt_requested(dsp));
            assert_eq!(dsp56300_cycle_count(dsp), 0);
            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_mcpx_address_register_profile_masks_jit_writes() {
        unsafe {
            // move y:(r2+n2),n1. The generic DSP56300 preserves all 24 bits;
            // MCPX exposes R/N/M as 16-bit registers and must retain only 006D.
            const MOVE_Y_R2_N2_TO_N1: u32 = 0x79EA00;

            let generic = make_dsp();
            dsp56300_write_memory(generic, CMemSpace::Y, 0, 0x20006D);
            dsp56300_write_memory(generic, CMemSpace::P, 0, MOVE_Y_R2_N2_TO_N1);
            dsp56300_step(generic);
            assert_eq!((*generic).state.registers[reg::N1], 0x20006D);
            dsp56300_destroy(generic);

            let mcpx = make_dsp_with_address_register_mask(0x0000FFFF);
            dsp56300_write_memory(mcpx, CMemSpace::Y, 0, 0x20006D);
            dsp56300_write_memory(mcpx, CMemSpace::P, 0, MOVE_Y_R2_N2_TO_N1);
            dsp56300_step(mcpx);
            assert_eq!((*mcpx).state.registers[reg::N1], 0x006D);
            assert_eq!((*mcpx).state.registers[reg::M0], 0xFFFF);
            dsp56300_reset(mcpx);
            assert_eq!((*mcpx).state.address_register_mask, 0xFFFF);
            assert_eq!((*mcpx).state.registers[reg::M0], 0xFFFF);
            dsp56300_destroy(mcpx);
        }
    }

    #[test]
    fn test_destroy_null() {
        unsafe { dsp56300_destroy(std::ptr::null_mut()) };
    }

    #[test]
    fn test_getters_setters() {
        unsafe {
            let dsp = make_dsp();

            dsp56300_set_halt_requested(dsp, true);
            assert!(dsp56300_halt_requested(dsp));
            dsp56300_set_halt_requested(dsp, false);
            assert!(!dsp56300_halt_requested(dsp));

            dsp56300_set_cycle_count(dsp, 42);
            assert_eq!(dsp56300_cycle_count(dsp), 42);

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_memory_read_write() {
        unsafe {
            let dsp = make_dsp();

            dsp56300_write_memory(dsp, CMemSpace::X, 0x10, 0x123456);
            assert_eq!(dsp56300_read_memory(dsp, CMemSpace::X, 0x10), 0x123456);

            dsp56300_write_memory(dsp, CMemSpace::Y, 0x20, 0xABCDEF);
            assert_eq!(dsp56300_read_memory(dsp, CMemSpace::Y, 0x20), 0xABCDEF);

            dsp56300_write_memory(dsp, CMemSpace::P, 0x00, 0x0C0042);
            assert_eq!(dsp56300_read_memory(dsp, CMemSpace::P, 0x00), 0x0C0042);

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_pram_write_marks_dirty() {
        unsafe {
            let dsp = make_dsp();

            assert_eq!((*dsp).state.pram_dirty.generation, 0);
            dsp56300_write_memory(dsp, CMemSpace::P, 100, 0x000042);
            assert_eq!((*dsp).state.pram_dirty.generation, 1);
            assert_ne!(
                (&(*dsp).state.pram_dirty.dirty)[100 / 64] & (1u64 << (100 % 64)),
                0
            );

            // Same value again -> no generation bump
            dsp56300_write_memory(dsp, CMemSpace::P, 100, 0x000042);
            assert_eq!((*dsp).state.pram_dirty.generation, 1);

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_cycle_budget() {
        unsafe {
            let dsp = make_dsp();
            (*dsp).state.cycle_budget = 100;
            assert_eq!((*dsp).state.cycle_budget, 100);
            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_step_executes_instruction() {
        unsafe {
            let dsp = make_dsp();
            dsp56300_write_memory(dsp, CMemSpace::P, 0, 0x0C0042); // jmp $42
            dsp56300_step(dsp);
            assert_eq!((*dsp).state.pc, 0x42);
            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_run_budget() {
        unsafe {
            let dsp = make_dsp();
            dsp56300_write_memory(dsp, CMemSpace::P, 0, 0x000000); // nop
            dsp56300_write_memory(dsp, CMemSpace::P, 1, 0x000000); // nop
            dsp56300_write_memory(dsp, CMemSpace::P, 2, 0x0C0000); // jmp $0
            dsp56300_run(dsp, 10);
            assert_eq!((*dsp).state.pc, 0);
            assert!((*dsp).state.cycle_budget <= 0);
            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_reset_preserves_map() {
        unsafe {
            unsafe extern "C" fn dummy_read(_: *mut c_void, _: u32) -> u32 {
                0
            }
            unsafe extern "C" fn dummy_write(_: *mut c_void, _: u32, _: u32) {}

            let pram = Box::leak(vec![0u32; PRAM_SIZE].into_boxed_slice());
            let x_regions = [
                CMemoryRegion {
                    start: 0,
                    end: XRAM_SIZE as u32,
                    kind: CRegion::Buffer,
                    data: CRegionData {
                        buffer: CBufferRegion {
                            base: Box::leak(vec![0u32; XRAM_SIZE].into_boxed_slice()).as_mut_ptr(),
                            offset: 0,
                        },
                    },
                },
                CMemoryRegion {
                    start: 0xFFFF80,
                    end: 0x1000000,
                    kind: CRegion::Callback,
                    data: CRegionData {
                        callback: CCallbackRegion {
                            opaque: std::ptr::null_mut(),
                            read: Some(dummy_read),
                            write: Some(dummy_write),
                        },
                    },
                },
            ];
            let p_regions = [CMemoryRegion {
                start: 0,
                end: PRAM_SIZE as u32,
                kind: CRegion::Buffer,
                data: CRegionData {
                    buffer: CBufferRegion {
                        base: pram.as_mut_ptr(),
                        offset: 0,
                    },
                },
            }];
            let info = CCreateInfo {
                memory_map: CMemoryMap {
                    x_regions: x_regions.as_ptr(),
                    x_count: 2,
                    y_regions: std::ptr::null(),
                    y_count: 0,
                    p_regions: p_regions.as_ptr(),
                    p_count: 1,
                },
                address_register_mask: 0,
            };
            let dsp = dsp56300_create(&info);

            // Step to modify state
            dsp56300_write_memory(dsp, CMemSpace::P, 0, 0x0C0042);
            dsp56300_step(dsp);
            assert_eq!((*dsp).state.pc, 0x42);

            // Reset preserves map
            dsp56300_reset(dsp);
            assert_eq!((*dsp).state.pc, 0);
            assert_eq!((*dsp).state.map.x_regions.len(), 2);

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_invalidate_cache() {
        unsafe {
            let dsp = make_dsp();
            dsp56300_write_memory(dsp, CMemSpace::P, 0, 0x000000); // nop
            dsp56300_step(dsp);

            dsp56300_invalidate_cache(dsp);
            dsp56300_write_memory(dsp, CMemSpace::P, 1, 0x000000); // nop
            dsp56300_step(dsp);
            assert_eq!((*dsp).state.pc, 2);

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_peripheral_callback() {
        unsafe extern "C" fn test_read(_opaque: *mut c_void, _addr: u32) -> u32 {
            0x42
        }
        unsafe extern "C" fn test_write(_opaque: *mut c_void, _addr: u32, _val: u32) {}

        unsafe {
            let x_regions = [CMemoryRegion {
                start: 0xFFFF80,
                end: 0x1000000,
                kind: CRegion::Callback,
                data: CRegionData {
                    callback: CCallbackRegion {
                        opaque: std::ptr::null_mut(),
                        read: Some(test_read),
                        write: Some(test_write),
                    },
                },
            }];
            let info = CCreateInfo {
                memory_map: CMemoryMap {
                    x_regions: x_regions.as_ptr(),
                    x_count: 1,
                    y_regions: std::ptr::null(),
                    y_count: 0,
                    p_regions: std::ptr::null(),
                    p_count: 0,
                },
                address_register_mask: 0,
            };
            let dsp = dsp56300_create(&info);

            // Callback region is in the map -- read dispatches through it
            assert_eq!(dsp56300_read_memory(dsp, CMemSpace::X, 0xFFFF80), 0x42);

            // Map survives reset
            dsp56300_reset(dsp);
            assert!(!(*dsp).state.map.x_regions.is_empty());
            assert_eq!(dsp56300_read_memory(dsp, CMemSpace::X, 0xFFFF80), 0x42);

            // No callback when created without one
            let dsp2 = make_dsp();
            assert_eq!(dsp56300_read_memory(dsp2, CMemSpace::X, 0xFFFF80), 0);
            dsp56300_destroy(dsp2);

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_c_quickstart() {
        assert_eq!(unsafe { quickstart_test() }, 0);
    }

    // ---- get_state / set_state roundtrip ----

    #[test]
    fn test_get_set_state_roundtrip() {
        unsafe {
            let dsp = make_dsp();

            // Modify some state
            (*dsp).state.pc = 0x42;
            (*dsp).state.cycle_count = 1000;
            (*dsp).state.halt_requested = true;
            (*dsp).state.loop_rep = true;

            let mut cs: CDspState = std::mem::zeroed();
            dsp56300_get_state(dsp, &mut cs);
            assert_eq!(cs.pc, 0x42);
            assert_eq!(cs.cycle_count, 1000);
            assert!(cs.halt_requested);
            assert!(cs.loop_rep);

            let dsp2 = make_dsp();
            dsp56300_set_state(dsp2, &cs);
            assert_eq!((*dsp2).state.pc, 0x42);
            assert_eq!((*dsp2).state.cycle_count, 1000);
            assert!((*dsp2).state.halt_requested);
            assert!((*dsp2).state.loop_rep);

            dsp56300_destroy(dsp2);
            dsp56300_destroy(dsp);
        }
    }

    // ---- profiling / perf map FFI ----

    #[test]
    fn test_enable_perf_map() {
        unsafe {
            let dsp = make_dsp();
            // Just verify it doesn't panic; perf_map is private
            dsp56300_enable_perf_map(dsp);
            dsp56300_write_memory(dsp, CMemSpace::P, 0, 0x0C0000); // jmp $0
            dsp56300_step(dsp);
            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_dump_profile() {
        unsafe {
            let dsp = make_dsp();
            // Write a simple loop and run it
            dsp56300_write_memory(dsp, CMemSpace::P, 0, 0x000000); // nop
            dsp56300_write_memory(dsp, CMemSpace::P, 1, 0x0C0000); // jmp $0
            dsp56300_run(dsp, 10);

            // Dump profile to temp file
            let tmp = std::env::temp_dir().join("dsp_test_profile.txt");
            let path = std::ffi::CString::new(tmp.to_str().unwrap()).unwrap();
            dsp56300_dump_profile(dsp, path.as_ptr());

            // Verify file was created
            let contents = std::fs::read_to_string(&tmp).unwrap();
            assert!(!contents.is_empty());
            std::fs::remove_file(&tmp).ok();

            dsp56300_destroy(dsp);
        }
    }

    #[test]
    fn test_state_registers_and_stack() {
        unsafe {
            let dsp = make_dsp();

            // Write registers and stack via set_state
            let mut cs: CDspState = std::mem::zeroed();
            dsp56300_get_state(dsp, &mut cs);
            cs.registers[dsp56300_core::reg::A1] = 0x123456;
            cs.stack[0][0] = 0x42;
            cs.stack[1][0] = 0x99;
            dsp56300_set_state(dsp, &cs);

            // Verify via internal state
            assert_eq!((*dsp).state.registers[dsp56300_core::reg::A1], 0x123456);
            assert_eq!((*dsp).state.stack[0][0], 0x42);
            assert_eq!((*dsp).state.stack[1][0], 0x99);

            // Verify round-trip via get_state
            let mut cs2: CDspState = std::mem::zeroed();
            dsp56300_get_state(dsp, &mut cs2);
            assert_eq!(cs2.registers[dsp56300_core::reg::A1], 0x123456);
            assert_eq!(cs2.stack[0][0], 0x42);
            assert_eq!(cs2.stack[1][0], 0x99);

            dsp56300_destroy(dsp);
        }
    }
}
