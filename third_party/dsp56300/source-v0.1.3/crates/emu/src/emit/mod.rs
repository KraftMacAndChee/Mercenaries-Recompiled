//! Cranelift IR emission for DSP56300 instructions.
//!
//! Each instruction's semantics are expressed as Cranelift IR, which is
//! compiled to native code by the JIT engine. The [`Emitter`] wraps a
//! Cranelift [`FunctionBuilder`] and emits IR for decoded instructions.

use std::mem::offset_of;

use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::{
    self, AbiParam, Block, BlockArg, InstBuilder, MemFlags, Signature, Value, types,
};
use cranelift_codegen::isa::CallConv;
use cranelift_frontend::{FunctionBuilder, Variable};

/// The calling convention used by `extern "C"` functions on the host platform.
#[cfg(target_os = "windows")]
const HOST_CALL_CONV: CallConv = CallConv::WindowsFastcall;
#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
const HOST_CALL_CONV: CallConv = CallConv::AppleAarch64;
#[cfg(not(any(
    target_os = "windows",
    all(target_os = "macos", target_arch = "aarch64")
)))]
const HOST_CALL_CONV: CallConv = CallConv::SystemV;

use crate::core::{
    DspState, InterruptState, MemSpace, MemoryMap, PowerState, RegionKind, interrupt,
    jit_read_accu24, jit_read_ssh, jit_rnd56, jit_stack_pop, jit_stack_push, jit_update_rn,
    jit_write_mem, jit_write_sp, jit_write_ssh, jit_write_ssl, reg, sr,
};
use dsp56300_core::{
    Accumulator, CondCode, Instruction, MulShiftOp, PERIPH_BASE, ParallelAlu, ParallelMoveType,
    REG_MASKS, decode, mask_pc,
};

mod agu;
mod alu;
mod bitops;
mod control;
mod flags;
mod logical;
mod loops;
mod mem;
mod moves;
mod regs;

/// Instruction length for an EA-mode encoding: mode 6 (absolute address)
/// uses a second extension word, all others are single-word.
fn inst_len_for_ea(ea_mode: u8) -> u32 {
    if (ea_mode >> 3) == 6 { 2 } else { 1 }
}
// DspState field offsets
const OFF_PC: i32 = offset_of!(DspState, pc) as i32;
const OFF_REGS: i32 = offset_of!(DspState, registers) as i32;
const OFF_LOOP_REP: i32 = offset_of!(DspState, loop_rep) as i32;
const OFF_PC_ON_REP: i32 = offset_of!(DspState, pc_on_rep) as i32;
const OFF_PC_ADVANCE: i32 = offset_of!(DspState, pc_advance) as i32;
const OFF_INTERRUPT_STATE: i32 = offset_of!(DspState, interrupts.state) as i32;

const OFF_INTERRUPT_PENDING_BITS: i32 = offset_of!(DspState, interrupts.pending_bits) as i32;

/// Registers promoted to Cranelift i32 Variables for the duration of a block.
/// Accumulator sub-registers (A0/A1/A2/B0/B1/B2) are promoted separately as
/// two i64 Variables -- see `PromotedRegs::acc`.
///
/// All architectural registers are promoted. Registers are lazily loaded on
/// first use (not at entry), and the compile-time dirty-flag system skips
/// stores for unwritten registers at block exit.
const PROMOTED_REGS: [usize; 36] = [
    reg::SR,
    reg::X0,
    reg::X1,
    reg::Y0,
    reg::Y1,
    reg::R0,
    reg::R1,
    reg::R2,
    reg::R3,
    reg::R4,
    reg::R5,
    reg::R6,
    reg::R7,
    reg::N0,
    reg::N1,
    reg::N2,
    reg::N3,
    reg::N4,
    reg::N5,
    reg::N6,
    reg::N7,
    reg::M0,
    reg::M1,
    reg::M2,
    reg::M3,
    reg::M4,
    reg::M5,
    reg::M6,
    reg::M7,
    reg::LC,
    reg::LA,
    reg::SP,
    reg::SSH,
    reg::SSL,
    reg::OMR,
    reg::TEMP,
];

/// Compile-time tracking of promoted register Variables.
struct PromotedRegs {
    /// Cranelift Variable for each promoted register, indexed by reg::* constants.
    /// None for non-promoted registers.
    vars: [Option<Variable>; 64],
    /// Compile-time dirty flags: true if the Variable was def'd since last flush.
    dirty: [bool; 64],
    /// Compile-time validity flags: true if the Variable holds the current value.
    /// When false, the next `load_reg` will lazily reload from memory.
    valid: [bool; 64],
    /// Packed i64 Variables for accumulators A and B: (ext << 48) | (mid << 24) | lo.
    /// Index 0 = A, 1 = B.
    acc: [Variable; 2],
    /// Dirty flags for accumulator i64 Variables.
    acc_dirty: [bool; 2],
    /// Validity flags for accumulator i64 Variables.
    acc_valid: [bool; 2],
}

/// Tracks which registers are lazily loaded inside a scope (entry or loop),
/// so that targeted loads can be emitted in a pre-block before the scope header.
struct DeferredScope {
    pre_block: Block,
    deferred: [bool; 64],
    acc_deferred: [bool; 2],
    /// Snapshot of `PromotedRegs::valid` at the time this scope was pushed.
    /// Used to distinguish "invalid from entry" (defer to pre-block) from
    /// "invalidated during body" (load inline after call).
    entry_valid: [bool; 64],
    entry_acc_valid: [bool; 2],
}

/// State snapshot for a conditional branch (brif -> merge).
///
/// Created by `begin_conditional()` before the brif. Each arm calls
/// `end_conditional_arm()` to flush newly-dirty registers and restore the
/// pre-branch state. After the merge, `merge_conditional()` invalidates
/// only the registers that were actually modified in any arm.
struct ConditionalState {
    saved_dirty: [bool; 64],
    saved_acc_dirty: [bool; 2],
    saved_valid: [bool; 64],
    saved_acc_valid: [bool; 2],
    /// Union of registers modified across all arms.
    modified: [bool; 64],
    modified_acc: [bool; 2],
}

/// Bit operation type for bclr/bset/btst/bchg family.
#[derive(Clone, Copy, PartialEq)]
enum BitOp {
    Clear,
    Set,
    Toggle,
    Test,
}

/// Logical operation type for AND/OR/EOR family.
enum LogicalOp {
    And,
    Or,
    Eor,
}

/// Addressing mode for bit-test branch instructions (jclr/jset/jsclr/jsset/brclr/brset/bsclr/bsset).
enum BitTestAddr {
    Pp { space: MemSpace, pp_offset: u8 },
    Qq { space: MemSpace, qq_offset: u8 },
    Aa { space: MemSpace, addr: u8 },
    Reg { reg_idx: u8 },
    Ea { space: MemSpace, ea_mode: u8 },
}

impl BitTestAddr {
    fn pp(space: MemSpace, pp_offset: u8) -> Self {
        Self::Pp { space, pp_offset }
    }
    fn qq(space: MemSpace, qq_offset: u8) -> Self {
        Self::Qq { space, qq_offset }
    }
    fn aa(space: MemSpace, addr: u8) -> Self {
        Self::Aa { space, addr }
    }
    fn reg(reg_idx: u8) -> Self {
        Self::Reg { reg_idx }
    }
    fn ea(space: MemSpace, ea_mode: u8) -> Self {
        Self::Ea { space, ea_mode }
    }
}

/// What kind of branch to take when the bit test condition is met.
enum BitTestBranch {
    /// jclr/jset: jump to absolute address.
    Jump,
    /// jsclr/jsset: jump to absolute subroutine (push PC+2 and SR).
    JumpSub { pc: u32 },
    /// brclr/brset: branch relative.
    Branch { pc: u32 },
    /// bsclr/bsset: branch relative to subroutine (push PC+2, update carry).
    BranchSub { pc: u32 },
}

/// Deferred CCR flag computation. Instead of emitting ~70+ IR instructions
/// after each ALU op, we record the inputs and only emit when SR is actually
/// read. If another ALU op overwrites the flags before SR is read, the
/// pending computation is discarded entirely.
enum PendingFlags {
    /// Standard ALU: EUNZ from result, VCL from add/sub operands.
    AluAddSub {
        result56: Value,
        source: Value,
        dest: Value,
        result_raw: Value,
        is_sub: bool,
    },
    /// EUNZ only, V cleared, C unchanged (TST, TFR-like).
    NzClearV { result56: Value },
    /// EUNZ only (caller handles V/C/L separately).
    NzOnly { result56: Value },
    /// EUNZ + V cleared + SM deferred (MPY/MPYR pattern).
    NzClearVSm { result56: Value },
    /// EUNZ + MAC overflow VL + SM deferred (MAC/MACR pattern).
    MacVlSm {
        result56: Value,
        product: Value,
        acc: Value,
    },
    /// EUNZ + set V/L from pre-computed overflow + SM deferred.
    NzVlSm { result56: Value, overflow: Value },
    /// EUNZ + SM deferred only (V/C set separately before, e.g. ASL/ASR).
    NzSm { result56: Value },
    /// EUNZ + VCL from sub operands (CMPM pattern, no SM).
    NzVclSub {
        result56: Value,
        source: Value,
        dest: Value,
        result_raw: Value,
    },
    /// EUNZ + VCL + XOR carry + OR overflow + SM (ADDL/SUBL).
    AddlSubl {
        result56: Value,
        source: Value,
        dest_shifted: Value,
        result_raw: Value,
        is_sub: bool,
        asl_carry: Value,
        asl_v: Value,
    },
    /// EUNZ + DMAC VL (no SM deferred).
    DmacVl {
        result56: Value,
        product: Value,
        acc: Value,
    },
    /// 24-bit shift/rotate flags: C, N, Z, clear V.
    Shift24 {
        carry: Value,
        n_val: Option<Value>,
        result: Value,
    },
    /// Logical ops: clear V, set N/Z from 24-bit result.
    Logical { result24: Value },
}

/// Cranelift IR emitter for DSP56300 instructions.
pub struct Emitter<'a> {
    builder: FunctionBuilder<'a>,
    state_ptr: Value,
    ptr_ty: ir::Type,
    /// Cranelift variable tracking accumulated cycle count across a block.
    total_cycles: Variable,
    /// cur_inst_len kept in a Variable; only written to memory at block exit.
    inst_len: Variable,
    /// Hot registers kept in Cranelift Variables to avoid redundant memory ops.
    promoted: PromotedRegs,
    /// Memory map for compile-time region lookups.
    map: &'a MemoryMap,
    /// Width mask for the platform's R/N/M address-generation registers.
    address_register_mask: u32,
    /// Cycle count accumulated at compile time, flushed to `total_cycles`
    /// Cranelift variable at return points and loop boundaries.
    pending_cycles: i64,
    /// Stack of deferred-load scopes. Bottom element = entry scope.
    /// Loop scopes are pushed/popped on top.
    scope_stack: Vec<DeferredScope>,
    /// The block where instruction emission begins (jumped to from entry pre-block).
    instructions_block: Block,
    /// Cranelift variable for deferred SM saturation V/L flag update.
    sm_needs_sat_var: Variable,
    /// Deferred flag computation. Set by ALU ops, flushed when SR is read.
    pending_flags: Option<PendingFlags>,
}

impl<'a> Emitter<'a> {
    /// Create a new emitter. Sets up the entry block and extracts the
    /// state pointer parameter.
    pub fn new(
        mut builder: FunctionBuilder<'a>,
        ptr_ty: ir::Type,
        map: &'a MemoryMap,
        address_register_mask: u32,
    ) -> Self {
        let total_cycles = builder.declare_var(types::I32);
        let inst_len = builder.declare_var(types::I32);
        let sm_needs_sat_var = builder.declare_var(types::I32);

        // Entry block: receives function params, defines iconst(0) placeholders,
        // then jumps to the deferred-loads block (pre_entry).
        let entry_block = builder.create_block();
        builder.append_block_params_for_function_params(entry_block);
        builder.switch_to_block(entry_block);
        builder.seal_block(entry_block);
        let state_ptr = builder.block_params(entry_block)[0];

        let zero = builder.ins().iconst(types::I32, 0);
        builder.def_var(total_cycles, zero);
        builder.def_var(inst_len, zero);
        builder.def_var(sm_needs_sat_var, zero);

        // Promote registers into Cranelift Variables with zero placeholders.
        // The iconst(0) provides a dominating SSA definition for Cranelift's
        // phi construction. Actual memory loads are deferred to the pre_entry
        // block on first use, emitted during finalize_and_return().
        let mut promoted = PromotedRegs {
            vars: [None; 64],
            dirty: [false; 64],
            valid: [false; 64],
            acc: [Variable::from_u32(0); 2], // placeholder, overwritten below
            acc_dirty: [false; 2],
            acc_valid: [false; 2],
        };
        let zero_i32 = builder.ins().iconst(types::I32, 0);
        for &idx in &PROMOTED_REGS {
            let var = builder.declare_var(types::I32);
            builder.def_var(var, zero_i32);
            promoted.vars[idx] = Some(var);
        }

        // Promote accumulators as packed i64 with zero placeholders.
        let zero_i64 = builder.ins().iconst(types::I64, 0);
        for i in 0..2 {
            let var = builder.declare_var(types::I64);
            builder.def_var(var, zero_i64);
            promoted.acc[i] = var;
        }

        // Pre-entry block: deferred register loads go here (filled at finalize).
        // Jumps to the instructions block. Same pattern as loop pre-blocks.
        let pre_entry = builder.create_block();
        builder.ins().jump(pre_entry, &[]); // terminate entry_block

        // Instructions block: all instruction code goes here.
        // Left unsealed until finalize_and_return() emits the jump from pre_entry.
        let instructions_block = builder.create_block();
        builder.switch_to_block(instructions_block);

        // Push entry scope as bottom of the scope stack.
        let entry_scope = DeferredScope {
            pre_block: pre_entry,
            deferred: [false; 64],
            acc_deferred: [false; 2],
            entry_valid: [false; 64],
            entry_acc_valid: [false; 2],
        };

        Self {
            builder,
            state_ptr,
            ptr_ty,
            total_cycles,
            inst_len,
            promoted,
            map,
            address_register_mask,
            pending_cycles: 0,
            scope_stack: vec![entry_scope],
            instructions_block,
            sm_needs_sat_var,
            pending_flags: None,
        }
    }

    /// Emit IR for a single decoded instruction.
    pub fn emit_instruction(&mut self, inst: &Instruction, pc: u32, next_word: u32) {
        match inst {
            Instruction::Nop => self.emit_nop(),
            Instruction::AddImm { imm, d } => self.emit_add_imm(*imm, *d),
            Instruction::SubImm { imm, d } => self.emit_sub_imm(*imm, *d),
            Instruction::Inc { d } => self.emit_inc(*d),
            Instruction::Dec { d } => self.emit_dec(*d),
            Instruction::AndI { imm, dest } => self.emit_andi(*imm, *dest),
            Instruction::OrI { imm, dest } => self.emit_ori(*imm, *dest),
            Instruction::Jmp { addr } => self.emit_jmp(*addr),
            Instruction::Jcc { cc, addr } => self.emit_jcc(*cc, *addr),
            Instruction::Jsr { addr } => self.emit_jsr(*addr, pc),
            Instruction::Rts => self.emit_rts(),
            Instruction::Rti => self.emit_rti(),
            Instruction::Bra { addr } => self.emit_bra(*addr, pc),
            Instruction::Bcc { cc, addr } => self.emit_bcc(*cc, *addr, pc),
            Instruction::MovecImm { imm, dest } => self.emit_movec_imm(*imm, *dest),
            Instruction::MovecReg {
                src_reg,
                dst_reg,
                w,
            } => self.emit_movec_reg(*src_reg, *dst_reg, *w),
            Instruction::BclrPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_op_pp(*space, *pp_offset, *bit_num, BitOp::Clear),
            Instruction::BclrQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_op_qq(*space, *qq_offset, *bit_num, BitOp::Clear),
            Instruction::BsetPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_op_pp(*space, *pp_offset, *bit_num, BitOp::Set),
            Instruction::BsetQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_op_qq(*space, *qq_offset, *bit_num, BitOp::Set),
            Instruction::BtstPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_op_pp(*space, *pp_offset, *bit_num, BitOp::Test),
            Instruction::BtstQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_op_qq(*space, *qq_offset, *bit_num, BitOp::Test),
            Instruction::BclrReg { reg_idx, bit_num } => {
                self.emit_bit_op_reg(*reg_idx, *bit_num, BitOp::Clear)
            }
            Instruction::BsetReg { reg_idx, bit_num } => {
                self.emit_bit_op_reg(*reg_idx, *bit_num, BitOp::Set)
            }
            Instruction::JclrPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Jump,
            ),
            Instruction::JclrQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Jump,
            ),
            Instruction::JsetPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Jump,
            ),
            Instruction::JsetQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Jump,
            ),
            Instruction::DoImm { count } => self.emit_do_imm(*count, pc, next_word),
            Instruction::DoForever => self.emit_do_forever(pc, next_word),
            Instruction::RepImm { count } => self.emit_rep_imm(*count),
            Instruction::EndDo => self.emit_enddo(),
            Instruction::Movep0 {
                pp_offset,
                reg_idx,
                w,
                space,
            } => self.emit_movep_0(*space, *pp_offset, *reg_idx, *w),
            Instruction::Movep1 {
                pp_offset,
                ea_mode,
                w,
                space,
            } => self.emit_movep_1(*space, *pp_offset, *ea_mode, *w, next_word),
            Instruction::Movep23 {
                pp_offset,
                ea_mode,
                w,
                perspace,
                easpace,
            } => self.emit_movep_23(*pp_offset, *ea_mode, *w, *perspace, *easpace, next_word),
            Instruction::AddLong { d } => self.emit_add_long(*d, next_word),
            Instruction::SubLong { d } => self.emit_sub_long(*d, next_word),
            Instruction::CmpLong { d } => self.emit_cmp_long(*d, next_word),
            Instruction::CmpImm { imm, d } => self.emit_cmp_imm(*imm, *d),
            Instruction::AndImm { imm, d } => {
                self.emit_logical_op_acc(*d, *imm as i64, 1, LogicalOp::And)
            }
            Instruction::AndLong { d } => {
                self.emit_logical_op_acc(*d, next_word as i64, 2, LogicalOp::And)
            }
            Instruction::OrImm { imm, d } => {
                self.emit_logical_op_acc(*d, *imm as i64, 1, LogicalOp::Or)
            }
            Instruction::OrLong { d } => {
                self.emit_logical_op_acc(*d, next_word as i64, 2, LogicalOp::Or)
            }
            Instruction::EorImm { imm, d } => {
                self.emit_logical_op_acc(*d, *imm as i64, 1, LogicalOp::Eor)
            }
            Instruction::EorLong { d } => {
                self.emit_logical_op_acc(*d, next_word as i64, 2, LogicalOp::Eor)
            }
            Instruction::BraLong => self.emit_bra_long(pc, next_word),
            Instruction::BccLong { cc } => self.emit_bcc_long(*cc, pc, next_word),
            Instruction::BsrLong => self.emit_bsr_long(pc, next_word),
            Instruction::Bsr { addr } => self.emit_bsr(*addr, pc),
            Instruction::BccRn { cc, rn } => self.emit_bcc_rn(*cc, *rn, pc),
            Instruction::BraRn { rn } => self.emit_bra_rn(*rn, pc),
            Instruction::BsrRn { rn } => self.emit_bsr_rn(*rn, pc),
            Instruction::Bscc { cc, addr } => self.emit_bscc(*cc, *addr, pc),
            Instruction::BsccLong { cc } => self.emit_bscc_long(*cc, pc, next_word),
            Instruction::BsccRn { cc, rn } => self.emit_bscc_rn(*cc, *rn, pc),
            Instruction::Brkcc { cc } => self.emit_brkcc(*cc),
            Instruction::MoveLongDisp {
                space,
                w,
                offreg_idx,
                numreg,
            } => self.emit_move_long_disp(*space, *w, *offreg_idx, *numreg, next_word),
            Instruction::Tcc { cc, acc, r } => self.emit_tcc(*cc, *acc, *r),
            Instruction::Lua { ea_mode, dst_reg } => self.emit_lua(*ea_mode, *dst_reg),
            Instruction::LuaRel {
                aa,
                addr_reg,
                dst_reg,
                dest_is_n,
            } => self.emit_lua_rel(*aa, *addr_reg, *dst_reg, *dest_is_n),
            Instruction::LraRn { addr_reg, dst_reg } => self.emit_lra_rn(*addr_reg, *dst_reg, pc),
            Instruction::LraDisp { dst_reg } => self.emit_lra_disp(*dst_reg, pc, next_word),
            Instruction::DoReg { reg_idx } => self.emit_do_reg(*reg_idx, pc, next_word),
            Instruction::RepReg { reg_idx } => self.emit_rep_reg(*reg_idx),
            Instruction::JmpEa { ea_mode } => self.emit_jmp_ea(*ea_mode, next_word),
            Instruction::JsrEa { ea_mode } => self.emit_jsr_ea(*ea_mode, pc, next_word),
            Instruction::JccEa { cc, ea_mode } => self.emit_jcc_ea(*cc, *ea_mode, next_word),
            Instruction::MovecEa {
                ea_mode,
                numreg,
                w,
                space,
            } => self.emit_movec_ea(*ea_mode, *numreg, *w, *space, next_word),
            Instruction::MovecAa {
                addr,
                numreg,
                w,
                space,
            } => self.emit_movec_aa(*addr, *numreg, *w, *space),
            Instruction::MovemEa { ea_mode, numreg, w } => {
                self.emit_movem_ea(*ea_mode, *numreg, *w, next_word)
            }
            Instruction::AslImm { shift, s, d } => self.emit_asl_imm(*shift, *s, *d),
            Instruction::AsrImm { shift, s, d } => self.emit_asr_imm(*shift, *s, *d),
            Instruction::AslReg { src, s, d } => self.emit_asl_reg(*src, *s, *d),
            Instruction::AsrReg { src, s, d } => self.emit_asr_reg(*src, *s, *d),
            Instruction::JclrReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Jump,
            ),
            Instruction::JsetReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Jump,
            ),
            Instruction::BclrAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_op_aa(*space, *addr, *bit_num, BitOp::Clear),
            Instruction::BsetAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_op_aa(*space, *addr, *bit_num, BitOp::Set),
            Instruction::BtstAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_op_aa(*space, *addr, *bit_num, BitOp::Test),
            Instruction::BtstReg { reg_idx, bit_num } => {
                self.emit_bit_op_reg(*reg_idx, *bit_num, BitOp::Test)
            }
            Instruction::BchgPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_op_pp(*space, *pp_offset, *bit_num, BitOp::Toggle),
            Instruction::BchgQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_op_qq(*space, *qq_offset, *bit_num, BitOp::Toggle),
            Instruction::BchgAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_op_aa(*space, *addr, *bit_num, BitOp::Toggle),
            Instruction::BchgReg { reg_idx, bit_num } => {
                self.emit_bit_op_reg(*reg_idx, *bit_num, BitOp::Toggle)
            }
            Instruction::JclrAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Jump,
            ),
            Instruction::JsetAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Jump,
            ),
            Instruction::JsclrPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JsclrQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JsclrAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                false,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JsclrReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                false,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JssetPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JssetQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JssetAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                true,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JssetReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                true,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::BrclrEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrclrAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrclrPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrclrQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrclrReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrsetEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrsetAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrsetPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrsetQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BrsetReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Branch { pc },
            ),
            Instruction::BsclrEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                false,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BsclrAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                false,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BsclrPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BsclrQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                false,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BsclrReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                false,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BssetEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                true,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BssetAa {
                space,
                addr,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::aa(*space, *addr),
                *bit_num,
                next_word,
                true,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BssetPp {
                space,
                pp_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::pp(*space, *pp_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BssetQq {
                space,
                qq_offset,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::qq(*space, *qq_offset),
                *bit_num,
                next_word,
                true,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::BssetReg { reg_idx, bit_num } => self.emit_bit_test_branch(
                &BitTestAddr::reg(*reg_idx),
                *bit_num,
                next_word,
                true,
                BitTestBranch::BranchSub { pc },
            ),
            Instruction::Jscc { cc, addr } => self.emit_jscc(*cc, *addr, pc),
            Instruction::DoAa { space, addr } => self.emit_do_aa(*space, *addr, pc, next_word),
            Instruction::DoEa { space, ea_mode } => {
                self.emit_do_ea(*space, *ea_mode, pc, next_word)
            }
            Instruction::DorImm { count } => self.emit_dor_imm(*count, pc, next_word),
            Instruction::DorReg { reg_idx } => self.emit_dor_reg(*reg_idx, pc, next_word),
            Instruction::DorForever => self.emit_dor_forever(pc, next_word),
            Instruction::DorAa { space, addr } => self.emit_dor_aa(*space, *addr, pc, next_word),
            Instruction::DorEa { space, ea_mode } => {
                self.emit_dor_ea(*space, *ea_mode, pc, next_word)
            }
            Instruction::RepAa { space, addr } => self.emit_rep_aa(*space, *addr),
            Instruction::RepEa { space, ea_mode } => self.emit_rep_ea(*space, *ea_mode),
            Instruction::MoveShortDisp {
                space,
                offset,
                w,
                offreg_idx,
                numreg,
            } => self.emit_move_short_disp(*offset, *w, *offreg_idx, *numreg, *space),
            Instruction::MovemAa { addr, numreg, w } => self.emit_movem_aa(*addr, *numreg, *w),
            Instruction::LslImm { shift, d } => self.emit_lsl_imm(*shift, *d),
            Instruction::LsrImm { shift, d } => self.emit_lsr_imm(*shift, *d),
            Instruction::LslReg { src, d } => self.emit_lsl_reg(*src, *d),
            Instruction::LsrReg { src, d } => self.emit_lsr_reg(*src, *d),
            Instruction::CmpU { src, d } => self.emit_cmpu(*src, *d),
            Instruction::MulShift {
                op,
                shift,
                src,
                d,
                k,
            } => self.emit_mul_shift(*op, *shift, *src, *d, *k),
            Instruction::MpyI { k, d, src } => self.emit_mpyi(*k, *d, *src, next_word),
            Instruction::MpyrI { k, d, src } => self.emit_mpyri(*k, *d, *src, next_word),
            Instruction::MacI { k, d, src } => self.emit_maci(*k, *d, *src, next_word),
            Instruction::MacrI { k, d, src } => self.emit_macri(*k, *d, *src, next_word),
            Instruction::Dmac { ss, k, d, s1, s2 } => self.emit_dmac(*ss, *k, *d, *s1, *s2),
            Instruction::MacSU { s, k, d, s1, s2 } => self.emit_mac_su(*s, *k, *d, *s1, *s2),
            Instruction::MpySU { s, k, d, s1, s2 } => self.emit_mpy_su(*s, *k, *d, *s1, *s2),
            Instruction::Div { src, d } => self.emit_div(*src, *d),
            Instruction::Norm { rreg_idx, d } => self.emit_norm(*rreg_idx, *d),
            Instruction::BclrEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_op_ea(*space, *ea_mode, *bit_num, BitOp::Clear, next_word),
            Instruction::BsetEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_op_ea(*space, *ea_mode, *bit_num, BitOp::Set, next_word),
            Instruction::BtstEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_op_ea(*space, *ea_mode, *bit_num, BitOp::Test, next_word),
            Instruction::BchgEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_op_ea(*space, *ea_mode, *bit_num, BitOp::Toggle, next_word),
            Instruction::JclrEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                false,
                BitTestBranch::Jump,
            ),
            Instruction::JsetEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                true,
                BitTestBranch::Jump,
            ),
            Instruction::JsclrEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                false,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JssetEa {
                space,
                ea_mode,
                bit_num,
            } => self.emit_bit_test_branch(
                &BitTestAddr::ea(*space, *ea_mode),
                *bit_num,
                next_word,
                true,
                BitTestBranch::JumpSub { pc },
            ),
            Instruction::JsccEa { cc, ea_mode } => self.emit_jscc_ea(*cc, *ea_mode, pc, next_word),
            Instruction::MovepQq {
                qq_offset,
                ea_mode,
                w,
                qqspace,
                easpace,
            } => self.emit_movep_qq(*qq_offset, *ea_mode, *w, *qqspace, *easpace, next_word),
            Instruction::MovepQqPea {
                qq_offset,
                ea_mode,
                w,
                space,
            } => self.emit_movep_qq_pea(*qq_offset, *ea_mode, *w, *space, next_word),
            Instruction::MovepQqR {
                qq_offset,
                reg_idx,
                w,
                space,
            } => self.emit_movep_qq_r(*qq_offset, *reg_idx, *w, *space),
            Instruction::Reset => self.emit_reset(),
            Instruction::Stop => self.emit_stop(),
            Instruction::Parallel {
                alu,
                move_type,
                opcode,
            } => self.emit_parallel(*opcode, *move_type, alu, next_word),
            Instruction::Wait => self.emit_wait(),
            Instruction::Illegal => self.emit_illegal(),
            Instruction::Clb { s, d } => self.emit_clb(*s, *d),
            Instruction::Normf { src, d } => self.emit_normf(*src, *d),
            Instruction::Merge { src, d } => self.emit_merge(*src, *d),
            Instruction::ExtractReg { s1, s2, d } => {
                self.emit_extract(Some(*s1), *s2, *d, next_word, false)
            }
            Instruction::ExtractImm { s2, d } => self.emit_extract(None, *s2, *d, next_word, false),
            Instruction::ExtractuReg { s1, s2, d } => {
                self.emit_extract(Some(*s1), *s2, *d, next_word, true)
            }
            Instruction::ExtractuImm { s2, d } => self.emit_extract(None, *s2, *d, next_word, true),
            Instruction::InsertReg { s1, s2, d } => self.emit_insert(Some(*s1), *s2, *d, next_word),
            Instruction::InsertImm { s2, d } => self.emit_insert(None, *s2, *d, next_word),
            Instruction::Vsl { s, ea_mode, i_bit } => {
                self.emit_vsl(*s, *ea_mode, *i_bit, next_word)
            }
            Instruction::Debug => self.emit_nop_like(1),
            Instruction::Debugcc { .. } => {
                self.set_inst_len(1);
                self.set_cycles(5);
            }
            Instruction::Trap => {
                self.set_inst_len(1);
                self.set_cycles(9);
                self.emit_add_interrupt(interrupt::TRAP);
            }
            Instruction::Trapcc { cc } => {
                self.set_inst_len(1);
                self.set_cycles(9);
                self.emit_trapcc(*cc);
            }
            Instruction::Pflush | Instruction::Pflushun | Instruction::Pfree => {
                self.emit_nop_like(decode::instruction_length(inst))
            }
            Instruction::Plockr | Instruction::Punlockr => {
                self.set_inst_len(decode::instruction_length(inst));
                self.set_cycles(4);
            }
            Instruction::PlockEa { ea_mode, .. } | Instruction::PunlockEa { ea_mode, .. } => {
                self.set_inst_len(inst_len_for_ea(*ea_mode));
                self.set_cycles(2);
                // Evaluate EA for address register side effects (e.g., post-increment)
                self.emit_calc_ea_ext(*ea_mode as u32, next_word);
            }
            _ => {
                let len = decode::instruction_length(inst);
                self.set_inst_len(len);
                self.set_cycles(2);
            }
        }
    }

    /// Push a new loop scope onto the scope stack.
    fn push_loop_scope(&mut self, pre_loop: Block) {
        self.scope_stack.push(DeferredScope {
            pre_block: pre_loop,
            deferred: [false; 64],
            acc_deferred: [false; 2],
            entry_valid: self.promoted.valid,
            entry_acc_valid: self.promoted.acc_valid,
        });
    }

    /// Pop a loop scope, emit targeted loads in its pre-block, and seal
    /// the loop header. Deferred registers that were already invalid at the
    /// parent scope's entry are propagated upward instead of loaded here.
    fn pop_loop_scope(&mut self, loop_header: Block) {
        let ctx = self.scope_stack.pop().unwrap();

        // Snapshot parent's entry_valid to avoid borrow conflict.
        let parent_entry_valid = self.scope_stack.last().unwrap().entry_valid;
        let parent_entry_acc_valid = self.scope_stack.last().unwrap().entry_acc_valid;

        self.builder.switch_to_block(ctx.pre_block);
        self.builder.seal_block(ctx.pre_block);

        for &idx in &PROMOTED_REGS {
            if ctx.deferred[idx] {
                if !parent_entry_valid[idx] {
                    // Was also invalid before parent scope -> propagate up.
                    self.scope_stack.last_mut().unwrap().deferred[idx] = true;
                } else {
                    // Was valid at parent entry, invalidated between parent
                    // and this loop -> load from memory in this pre-block.
                    let val = self.load_u32(Self::reg_offset(idx));
                    self.builder.def_var(self.promoted.vars[idx].unwrap(), val);
                }
            }
        }
        for (i, &acc) in [Accumulator::A, Accumulator::B].iter().enumerate() {
            if ctx.acc_deferred[i] {
                if !parent_entry_acc_valid[i] {
                    self.scope_stack.last_mut().unwrap().acc_deferred[i] = true;
                } else {
                    self.reload_acc(acc);
                }
            }
        }
        self.builder.ins().jump(loop_header, &[]);

        self.builder.seal_block(loop_header);
    }

    /// Emit a return instruction and finalize the function.
    /// Pops the entry scope and emits deferred loads in the entry pre-block.
    pub fn finalize_and_return(mut self) {
        // Flush any deferred flag computation before writing registers to memory.
        self.flush_pending_flags();
        // Finalize the current (instructions) block.
        self.flush_all_to_memory();
        self.flush_pending_cycles();
        let cycles = self.builder.use_var(self.total_cycles);
        self.builder.ins().return_(&[cycles]);

        // Pop the entry scope and emit deferred loads in the entry pre-block.
        let entry_ctx = self.scope_stack.pop().unwrap();
        debug_assert!(self.scope_stack.is_empty());
        self.builder.switch_to_block(entry_ctx.pre_block);
        self.builder.seal_block(entry_ctx.pre_block);
        for &idx in &PROMOTED_REGS {
            if entry_ctx.deferred[idx] {
                let val = self.load_u32(Self::reg_offset(idx));
                self.builder.def_var(self.promoted.vars[idx].unwrap(), val);
            }
        }
        for (i, &acc) in [Accumulator::A, Accumulator::B].iter().enumerate() {
            if entry_ctx.acc_deferred[i] {
                self.reload_acc(acc);
            }
        }
        self.builder.ins().jump(self.instructions_block, &[]);
        self.builder.seal_block(self.instructions_block);
        self.builder.finalize();
    }

    /// Returns true if the instruction is a block terminator (branches,
    /// loops, wait, or anything that disrupts sequential control flow).
    fn is_block_terminator(inst: &Instruction) -> bool {
        matches!(
            inst,
            Instruction::Jmp { .. }
                | Instruction::Jcc { .. }
                | Instruction::Jsr { .. }
                | Instruction::Rts
                | Instruction::Rti
                | Instruction::Bra { .. }
                | Instruction::Bcc { .. }
                | Instruction::BraLong
                | Instruction::BsrLong
                | Instruction::BccLong { .. }
                | Instruction::DoImm { .. }
                | Instruction::DoEa { .. }
                | Instruction::DoAa { .. }
                | Instruction::DoReg { .. }
                | Instruction::DoForever
                | Instruction::DorImm { .. }
                | Instruction::DorReg { .. }
                | Instruction::DorForever
                | Instruction::DorAa { .. }
                | Instruction::DorEa { .. }
                | Instruction::EndDo
                | Instruction::JclrPp { .. }
                | Instruction::JclrQq { .. }
                | Instruction::JsetPp { .. }
                | Instruction::JsetQq { .. }
                | Instruction::JclrReg { .. }
                | Instruction::JsetReg { .. }
                | Instruction::JclrEa { .. }
                | Instruction::JsetEa { .. }
                | Instruction::JclrAa { .. }
                | Instruction::JsetAa { .. }
                | Instruction::JsclrPp { .. }
                | Instruction::JsclrQq { .. }
                | Instruction::JsclrAa { .. }
                | Instruction::JsclrReg { .. }
                | Instruction::JsclrEa { .. }
                | Instruction::JssetPp { .. }
                | Instruction::JssetQq { .. }
                | Instruction::JssetAa { .. }
                | Instruction::JssetReg { .. }
                | Instruction::JssetEa { .. }
                | Instruction::BccRn { .. }
                | Instruction::BraRn { .. }
                | Instruction::BsrRn { .. }
                | Instruction::Bscc { .. }
                | Instruction::BsccLong { .. }
                | Instruction::BsccRn { .. }
                | Instruction::Brkcc { .. }
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
                | Instruction::Jscc { .. }
                | Instruction::JsccEa { .. }
                | Instruction::JmpEa { .. }
                | Instruction::JsrEa { .. }
                | Instruction::JccEa { .. }
                | Instruction::Bsr { .. }
                | Instruction::Wait
                | Instruction::Stop
                | Instruction::Illegal
        )
    }

    /// Returns true if this instruction needs a block-exit check injected
    /// after it. Currently this covers peripheral-space writes, whose
    /// callbacks may set `halt_requested`.
    fn needs_exit_check(inst: &Instruction) -> bool {
        matches!(
            inst,
            Instruction::Movep0 { .. }
                | Instruction::Movep1 { .. }
                | Instruction::Movep23 { .. }
                | Instruction::MovepQq { .. }
                | Instruction::MovepQqPea { .. }
                | Instruction::MovepQqR { .. }
                | Instruction::BsetPp { .. }
                | Instruction::BclrPp { .. }
                | Instruction::BchgPp { .. }
                | Instruction::BsetQq { .. }
                | Instruction::BclrQq { .. }
                | Instruction::BchgQq { .. }
        )
    }

    /// Returns true if the instruction writes to P-space memory.
    /// Such instructions must end the block so the next instruction is
    /// compiled fresh from the (potentially modified) PRAM.
    fn writes_p_memory(inst: &Instruction) -> bool {
        match inst {
            // movem: w == false means write register -> P memory
            Instruction::MovemEa { w, .. } | Instruction::MovemAa { w, .. } => !w,
            // movep_1 / movep_qq_pea: w == false means write peripheral -> P:ea
            Instruction::Movep1 { w, .. } | Instruction::MovepQqPea { w, .. } => !w,
            _ => false,
        }
    }

    /// Emit a basic block of instructions starting at `start_pc`.
    ///
    /// Decodes instructions from `pram` until a block terminator is found,
    /// `max_len` instructions are reached, or `stop_pc` is reached. The
    /// compiled block fully manages PC: after return, `state.pc` is set to
    /// the correct next address (branch target or fallthrough).
    ///
    /// `stop_pc` is the DO-loop boundary (LA+1). Pass `u32::MAX` when not
    /// inside a DO loop.
    ///
    /// REP instructions are compiled as inline Cranelift loops rather than
    /// being block terminators. The repeated instruction executes LC times
    /// inside a native loop with no per-iteration run-loop overhead.
    ///
    /// Returns `(inst_count, end_pc)`.
    pub fn emit_block(&mut self, start_pc: u32, max_len: u32, stop_pc: u32) -> u32 {
        let p_end = self.map.p_space_end();
        let mut pc = start_pc;
        let mut count = 0u32;
        let mut ended_with_terminator = false;

        loop {
            if count >= max_len || pc >= p_end {
                break;
            }

            let opcode = self.map.read_pram(pc);
            let next_word = self.map.read_pram(mask_pc(pc + 1));

            let inst = decode::decode(opcode);
            let inst_len = decode::instruction_length(&inst);

            // REP: compile as inline Cranelift loop instead of block terminator
            if Self::is_rep_instruction(&inst) {
                self.emit_rep_inline(&inst, pc);
                // REP consumes itself (1 word) + the repeated instruction
                let rep_inst_pc = pc + 1;
                let rep_opcode = self.map.read_pram(rep_inst_pc);
                let rep_inst = decode::decode(rep_opcode);
                let rep_inst_len = decode::instruction_length(&rep_inst);
                pc += 1 + rep_inst_len;
                count += 2; // REP + repeated instruction
            } else if Self::is_do_instruction(&inst) {
                // DO/DOR: try to inline as Cranelift loop if body is safe.
                // DO FOREVER cannot be inlined (would create an infinite native loop).
                let is_forever = matches!(inst, Instruction::DoForever | Instruction::DorForever);
                let la = Self::compute_do_la(&inst, pc, next_word);
                if !is_forever && Self::is_do_body_inlineable(self.map, pc + 2, la, stop_pc) {
                    self.emit_do_inline(&inst, pc, la);
                    // Count body instructions for the max_len budget.
                    let body_count = Self::count_body_instructions(self.map, pc + 2, la);
                    count += 1 + body_count; // +1 for the DO itself
                    pc = la + 1;
                } else {
                    // Body not inlineable -- fall back to block terminator.
                    self.emit_instruction(&inst, pc, next_word);
                    pc += inst_len;
                    ended_with_terminator = true;
                    break;
                }
            } else {
                let is_terminator = Self::is_block_terminator(&inst);
                let check_exit = Self::needs_exit_check(&inst);

                // Emit the instruction IR
                self.emit_instruction(&inst, pc, next_word);

                count += 1;
                pc += inst_len;

                // After peripheral-writing instructions, check exit_requested.
                // The peripheral callback sets exit_requested (via
                // set_halt_requested) when it needs the block to return
                // to the run loop.
                if check_exit && !is_terminator {
                    // Flush accumulated cycles BEFORE the branch so both
                    // the early-return and continue paths see the correct
                    // total_cycles in Cranelift. (pending_cycles is a Rust
                    // compile-time accumulator; flushing only in early_ret
                    // would reset it to 0 for the continue path too, losing
                    // cycles on the normal path.)
                    self.flush_pending_cycles();

                    let off_exit = offset_of!(DspState, exit_requested) as i32;
                    let exit_val =
                        self.builder
                            .ins()
                            .load(types::I8, Self::flags(), self.state_ptr, off_exit);

                    let early_ret = self.builder.create_block();
                    let cont = self.builder.create_block();
                    self.builder.ins().brif(exit_val, early_ret, &[], cont, &[]);

                    // Early-return block: flush promoted regs, store next PC, return.
                    self.builder.switch_to_block(early_ret);
                    self.builder.seal_block(early_ret);
                    self.flush_all_to_memory();
                    let ret_cycles = self.builder.use_var(self.total_cycles);
                    let next_pc_val = self.builder.ins().iconst(types::I32, pc as i64);
                    self.store_pc(next_pc_val);
                    self.builder.ins().return_(&[ret_cycles]);

                    // Continue block.
                    self.builder.switch_to_block(cont);
                    self.builder.seal_block(cont);
                }

                if is_terminator || pc >= stop_pc || Self::writes_p_memory(&inst) {
                    ended_with_terminator = true;
                    break;
                }
            }
        }

        // Set final PC.
        //
        // Branch instructions write target PC to state and set inst_len=0.
        // Non-branch instructions set inst_len>0 but don't write PC.
        // Conditional branches: taken sets inst_len=0 + writes PC,
        //   not-taken sets inst_len>0 + doesn't write PC.
        //
        // Strategy: read inst_len. If 0, PC was set by a branch. If >0,
        // this is either a non-branch terminator or end-of-block; set PC
        // to the fallthrough address.
        if ended_with_terminator {
            let cur_il = self.builder.use_var(self.inst_len);
            let fallthrough = self.builder.ins().iconst(types::I32, pc as i64);
            let zero = self.builder.ins().iconst(types::I32, 0);
            let branch_taken = self.builder.ins().icmp(IntCC::Equal, cur_il, zero);
            let already_set_pc = self.load_u32(OFF_PC);
            let final_pc = self
                .builder
                .ins()
                .select(branch_taken, already_set_pc, fallthrough);
            self.store_pc(final_pc);
        } else {
            // Hit max_len or end of pram without a terminator.
            let fallthrough = self.builder.ins().iconst(types::I32, pc as i64);
            self.store_pc(fallthrough);
        }

        pc
    }

    // helpers: DspState field access

    fn flags() -> MemFlags {
        MemFlags::trusted()
    }

    fn reg_offset(idx: usize) -> i32 {
        OFF_REGS + (idx as i32) * 4
    }

    fn load_u32(&mut self, off: i32) -> Value {
        self.builder
            .ins()
            .load(types::I32, Self::flags(), self.state_ptr, off)
    }
    fn store_u32(&mut self, off: i32, v: Value) {
        self.builder
            .ins()
            .store(Self::flags(), v, self.state_ptr, off);
    }
    fn store_bool(&mut self, off: i32, v: Value) {
        self.builder
            .ins()
            .store(Self::flags(), v, self.state_ptr, off);
    }

    fn set_inst_len(&mut self, len: u32) {
        let v = self.builder.ins().iconst(types::I32, len as i64);
        self.builder.def_var(self.inst_len, v);
    }
    /// Add instruction cycle cost to the block's running total.
    fn set_cycles(&mut self, n: i64) {
        self.pending_cycles += n;
    }

    /// Flush compile-time accumulated cycles into the Cranelift `total_cycles`
    /// variable. Called at return points and loop boundaries.
    fn flush_pending_cycles(&mut self) {
        if self.pending_cycles > 0 {
            let prev = self.builder.use_var(self.total_cycles);
            let add = self.builder.ins().iconst(types::I32, self.pending_cycles);
            let new = self.builder.ins().iadd(prev, add);
            self.builder.def_var(self.total_cycles, new);
            self.pending_cycles = 0;
        }
    }

    // helpers: CCR updates

    /// Shift a bit-0 i32 value to specified bit position.
    fn shift_to_bit(&mut self, val: Value, flag: u32) -> Value {
        let shift = self.builder.ins().iconst(types::I32, flag as i64);
        self.builder.ins().ishl(val, shift)
    }

    /// Extract single bit from an i32 value.
    fn extract_bit(&mut self, sr_val: Value, bit: u32) -> Value {
        let c = self.builder.ins().iconst(types::I32, bit as i64);
        let shifted = self.builder.ins().ushr(sr_val, c);
        let one = self.builder.ins().iconst(types::I32, 1);
        self.builder.ins().band(shifted, one)
    }

    /// Extract a single bit from an i64 value as an i32 (0 or 1).
    fn extract_bit_i64(&mut self, val: Value, bit: u32) -> Value {
        let shift = self.builder.ins().iconst(types::I32, bit as i64);
        let shifted = self.builder.ins().ushr(val, shift);
        let reduced = self.builder.ins().ireduce(types::I32, shifted);
        let one = self.builder.ins().iconst(types::I32, 1);
        self.builder.ins().band(reduced, one)
    }

    /// Mask an i32 value to 24 bits.
    fn mask24(&mut self, val: Value) -> Value {
        let mask = self.builder.ins().iconst(types::I32, 0x00FFFFFF);
        self.builder.ins().band(val, mask)
    }

    // helpers: stack

    fn stack_push(&mut self, ssh_val: Value, ssl_val: Value) {
        self.flush_stack_promoted();
        let fn_ptr = self
            .builder
            .ins()
            .iconst(self.ptr_ty, jit_stack_push as *const () as usize as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty));
        sig.params.push(AbiParam::new(types::I32));
        sig.params.push(AbiParam::new(types::I32));
        let sig_ref = self.builder.import_signature(sig);
        self.builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, ssh_val, ssl_val]);
        self.invalidate_stack_promoted();
    }

    fn stack_pop(&mut self) -> (Value, Value) {
        self.flush_stack_promoted();
        let fn_ptr = self
            .builder
            .ins()
            .iconst(self.ptr_ty, jit_stack_pop as *const () as usize as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty));
        sig.returns.push(AbiParam::new(types::I64));
        let sig_ref = self.builder.import_signature(sig);
        let call = self
            .builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr]);
        let packed = self.builder.inst_results(call)[0];
        self.invalidate_stack_promoted();
        let ssh = self.builder.ins().ireduce(types::I32, packed);
        let ssl64 = self.builder.ins().ushr_imm(packed, 32);
        let ssl = self.builder.ins().ireduce(types::I32, ssl64);
        (ssh, ssl)
    }

    // helpers: condition codes

    /// Evaluate condition code, returning i32 (1=true, 0=false).
    fn eval_cc(&mut self, cc: CondCode) -> Value {
        let sr_val = self.load_reg(reg::SR);
        let one = self.builder.ins().iconst(types::I32, 1);

        match cc {
            CondCode::CC | CondCode::CS => {
                let c = self.extract_bit(sr_val, sr::C);
                if cc == CondCode::CC {
                    self.builder.ins().bxor(c, one)
                } else {
                    c
                }
            }
            CondCode::EQ | CondCode::NE => {
                let z = self.extract_bit(sr_val, sr::Z);
                if cc == CondCode::EQ {
                    z
                } else {
                    self.builder.ins().bxor(z, one)
                }
            }
            CondCode::PL | CondCode::MI => {
                let n = self.extract_bit(sr_val, sr::N);
                if cc == CondCode::PL {
                    self.builder.ins().bxor(n, one)
                } else {
                    n
                }
            }
            CondCode::GE | CondCode::LT => {
                let n = self.extract_bit(sr_val, sr::N);
                let v = self.extract_bit(sr_val, sr::V);
                let nxv = self.builder.ins().bxor(n, v);
                if cc == CondCode::GE {
                    self.builder.ins().bxor(nxv, one)
                } else {
                    nxv
                }
            }
            CondCode::GT | CondCode::LE => {
                let n = self.extract_bit(sr_val, sr::N);
                let v = self.extract_bit(sr_val, sr::V);
                let z = self.extract_bit(sr_val, sr::Z);
                let nxv = self.builder.ins().bxor(n, v);
                let expr = self.builder.ins().bor(z, nxv);
                if cc == CondCode::GT {
                    self.builder.ins().bxor(expr, one)
                } else {
                    expr
                }
            }
            CondCode::NN | CondCode::NR => {
                let z = self.extract_bit(sr_val, sr::Z);
                let u = self.extract_bit(sr_val, sr::U);
                let e = self.extract_bit(sr_val, sr::E);
                let not_u = self.builder.ins().bxor(u, one);
                let not_u_and_e = self.builder.ins().band(not_u, e);
                let expr = self.builder.ins().bor(z, not_u_and_e);
                if cc == CondCode::NN {
                    self.builder.ins().bxor(expr, one)
                } else {
                    expr
                }
            }
            CondCode::EC | CondCode::ES => {
                let e = self.extract_bit(sr_val, sr::E);
                if cc == CondCode::EC {
                    self.builder.ins().bxor(e, one)
                } else {
                    e
                }
            }
            CondCode::LC | CondCode::LS => {
                let l = self.extract_bit(sr_val, sr::L);
                if cc == CondCode::LC {
                    self.builder.ins().bxor(l, one)
                } else {
                    l
                }
            }
        }
    }

    /// Evaluate condition code, returning an i8 boolean (Cranelift `brif`-compatible).
    fn eval_cc_bool(&mut self, cc: CondCode) -> Value {
        let cond = self.eval_cc(cc);
        let zero = self.builder.ins().iconst(types::I32, 0);
        self.builder.ins().icmp(IntCC::NotEqual, cond, zero)
    }

    // instruction emitters

    /// Emit a call to jit_read_accu24: flushes only the accumulator sub-regs
    /// and SR, calls the helper, invalidates SR (modified by helper).
    /// Returns the raw u32 result (bits 23:0 = value, bit 24 = no_limit).
    fn emit_call_read_accu24(&mut self, acc_idx: u32) -> Value {
        // Flush pending CCR flags so SR is up-to-date
        self.flush_pending_flags();

        // Flush only the sub-registers the helper reads + SR
        let regs = if acc_idx == 0 {
            [reg::A2, reg::A1, reg::A0]
        } else {
            [reg::B2, reg::B1, reg::B0]
        };
        for &r in &regs {
            self.flush_reg(r);
            self.promoted.dirty[r] = false;
        }
        self.flush_reg(reg::SR);
        self.promoted.dirty[reg::SR] = false;

        // Also flush the packed accumulator — the helper reads sub-regs from
        // state directly, but load_acc may have been called earlier putting
        // the accumulator into the i64 Variable without writing sub-regs back.
        self.flush_acc_to_memory(if acc_idx == 0 {
            Accumulator::A
        } else {
            Accumulator::B
        });

        let fn_addr = jit_read_accu24 as *const () as usize;
        let fn_ptr = self.builder.ins().iconst(self.ptr_ty, fn_addr as i64);
        let acc_val = self.builder.ins().iconst(types::I32, acc_idx as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty)); // *mut DspState
        sig.params.push(AbiParam::new(types::I32)); // acc_idx
        sig.returns.push(AbiParam::new(types::I32)); // result
        let sig_ref = self.builder.import_signature(sig);
        let call = self
            .builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, acc_val]);

        // Invalidate SR (helper modifies L and S flags)
        self.promoted.valid[reg::SR] = false;
        if let Some(scope) = self.scope_stack.last_mut() {
            scope.entry_valid[reg::SR] = true;
        }

        self.builder.inst_results(call)[0]
    }

    /// Emit a call to an extern "C" fn(*mut DspState, u32) helper.
    /// Flushes/reloads promoted registers (used by SSH/SSL/SP helpers).
    fn emit_call_extern_val(&mut self, fn_addr: usize, val: Value) {
        self.flush_promoted();
        let fn_ptr = self.builder.ins().iconst(self.ptr_ty, fn_addr as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty)); // *mut DspState
        sig.params.push(AbiParam::new(types::I32)); // value
        let sig_ref = self.builder.import_signature(sig);
        self.builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, val]);
        self.invalidate_promoted();
    }

    /// Emit a call to an extern "C" fn(*mut DspState, i64) helper that only
    /// modifies SR. Flushes and invalidates only SR, not all promoted registers.
    fn emit_call_sr_helper_i64(&mut self, fn_addr: usize, val: Value) {
        self.flush_reg(reg::SR);
        self.promoted.dirty[reg::SR] = false;
        let fn_ptr = self.builder.ins().iconst(self.ptr_ty, fn_addr as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty)); // *mut DspState
        sig.params.push(AbiParam::new(types::I64)); // acc_val
        let sig_ref = self.builder.import_signature(sig);
        self.builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, val]);
        // Invalidate SR so next load_reg(SR) reloads from memory.
        // Mark entry_valid=true so the reload is inline (not deferred to
        // the pre-block, which would read the stale pre-call value).
        self.promoted.valid[reg::SR] = false;
        if let Some(scope) = self.scope_stack.last_mut() {
            scope.entry_valid[reg::SR] = true;
        }
    }

    /// Emit a call to an extern "C" fn(*mut DspState) -> u32 helper.
    /// Flushes/reloads promoted registers (used by jit_read_ssh).
    fn emit_call_extern_ret(&mut self, fn_addr: usize) -> Value {
        self.flush_promoted();
        let fn_ptr = self.builder.ins().iconst(self.ptr_ty, fn_addr as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty)); // *mut DspState
        sig.returns.push(AbiParam::new(types::I32)); // return u32
        let sig_ref = self.builder.import_signature(sig);
        let call = self
            .builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr]);
        self.invalidate_promoted();
        self.builder.inst_results(call)[0]
    }

    /// Emit a conditional stack push for JSR/BSR: if interrupt_state != LONG,
    /// push (ret_addr, sr) to stack; else set interrupt_state = Fast.
    fn emit_interrupt_aware_stack_push(&mut self, ret_addr: Value) {
        let int_state = self.builder.ins().load(
            types::I8,
            Self::flags(),
            self.state_ptr,
            OFF_INTERRUPT_STATE,
        );
        let long_val = self
            .builder
            .ins()
            .iconst(types::I8, InterruptState::Long as i64);
        let is_long = self.builder.ins().icmp(IntCC::Equal, int_state, long_val);

        let normal_blk = self.builder.create_block();
        let long_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(is_long, long_blk, &[], normal_blk, &[]);

        // Normal path: push stack
        self.builder.switch_to_block(normal_blk);
        self.builder.seal_block(normal_blk);
        let sr_val = self.load_reg(reg::SR);
        self.stack_push(ret_addr, sr_val);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        // Long interrupt path: skip push, set state = Fast
        self.builder.switch_to_block(long_blk);
        self.builder.seal_block(long_blk);
        let disabled = self
            .builder
            .ins()
            .iconst(types::I8, InterruptState::Fast as i64);
        self.builder
            .ins()
            .store(Self::flags(), disabled, self.state_ptr, OFF_INTERRUPT_STATE);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }
}
