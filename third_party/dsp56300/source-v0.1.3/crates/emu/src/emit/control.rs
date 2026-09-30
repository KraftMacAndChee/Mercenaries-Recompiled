use super::{inst_len_for_ea, *};

impl<'a> Emitter<'a> {
    pub(super) fn emit_nop(&mut self) {
        self.set_inst_len(1);
        self.set_cycles(1);
    }

    pub(super) fn emit_illegal(&mut self) {
        self.set_inst_len(1);
        self.set_cycles(5);
        self.emit_add_interrupt(interrupt::ILLEGAL);
    }

    pub(super) fn emit_trapcc(&mut self, cc: CondCode) {
        let taken = self.eval_cc_bool(cc);

        let trap_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, trap_blk, &[], merge_blk, &[]);

        self.builder.switch_to_block(trap_blk);
        self.builder.seal_block(trap_blk);
        self.emit_add_interrupt(interrupt::TRAP);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.end_conditional_arm(&mut cond_state);
    }

    /// Emit IR to post an interrupt (equivalent to `add_interrupt(inter)`).
    /// Sets bit `inter` in `pending_bits[word]`.
    pub(super) fn emit_add_interrupt(&mut self, inter: usize) {
        let word = inter / 64;
        let bit = inter % 64;
        let offset = OFF_INTERRUPT_PENDING_BITS + (word as i32) * 8;
        let pending = self
            .builder
            .ins()
            .load(types::I64, Self::flags(), self.state_ptr, offset);
        let mask = self.builder.ins().iconst(types::I64, 1i64 << bit);
        let new_pending = self.builder.ins().bor(pending, mask);
        self.builder
            .ins()
            .store(Self::flags(), new_pending, self.state_ptr, offset);
    }

    pub(super) fn emit_jmp(&mut self, addr: u32) {
        self.set_inst_len(0);
        self.set_cycles(3);
        let v = self.builder.ins().iconst(types::I32, addr as i64);
        self.store_pc(v);
    }

    pub(super) fn emit_jcc(&mut self, cc: CondCode, addr: u32) {
        self.set_cycles(4);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let a = self.builder.ins().iconst(types::I32, addr as i64);
        self.store_pc(a);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(1);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_jsr(&mut self, addr: u32, pc: u32) {
        self.set_inst_len(0);
        self.set_cycles(3);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 1) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let a = self.builder.ins().iconst(types::I32, addr as i64);
        self.store_pc(a);
    }

    pub(super) fn emit_rts(&mut self) {
        self.set_inst_len(0);
        self.set_cycles(3);
        let (ssh, _ssl) = self.stack_pop();
        self.store_pc(ssh);
    }

    pub(super) fn emit_rti(&mut self) {
        self.set_inst_len(0);
        self.set_cycles(3);
        let (ssh, ssl) = self.stack_pop();
        self.store_pc(ssh);
        self.store_reg(reg::SR, ssl);
    }

    pub(super) fn emit_bra(&mut self, offset: i32, pc: u32) {
        self.set_inst_len(0);
        self.set_cycles(4);
        let target = mask_pc((pc as i32 + offset) as u32);
        let v = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(v);
    }

    pub(super) fn emit_bcc(&mut self, cc: CondCode, offset: i32, pc: u32) {
        self.set_cycles(4);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let target = mask_pc((pc as i32 + offset) as u32);
        let a = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(a);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(1);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_bra_long(&mut self, pc: u32, next_word: u32) {
        self.set_inst_len(0);
        self.set_cycles(4);
        let target = mask_pc(pc.wrapping_add(next_word));
        let v = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(v);
    }

    pub(super) fn emit_bcc_long(&mut self, cc: CondCode, pc: u32, next_word: u32) {
        self.set_cycles(5);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let target = mask_pc(pc.wrapping_add(next_word));
        let a = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(a);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(2);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_bsr(&mut self, offset: i32, pc: u32) {
        self.set_inst_len(0);
        self.set_cycles(4);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 1) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let target = mask_pc((pc as i32 + offset) as u32);
        let v = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(v);
    }

    pub(super) fn emit_bsr_long(&mut self, pc: u32, next_word: u32) {
        self.set_inst_len(0);
        self.set_cycles(5);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 2) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let target = mask_pc(pc.wrapping_add(next_word));
        let v = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(v);
    }

    pub(super) fn emit_bscc(&mut self, cc: CondCode, offset: i32, pc: u32) {
        self.set_cycles(4);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 1) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let target = mask_pc((pc as i32 + offset) as u32);
        let a = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(a);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(1);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_bscc_long(&mut self, cc: CondCode, pc: u32, next_word: u32) {
        self.set_cycles(5);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 2) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let target = mask_pc(pc.wrapping_add(next_word));
        let a = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(a);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(2);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_bscc_rn(&mut self, cc: CondCode, rn: u8, pc: u32) {
        self.set_cycles(4);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 1) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let rn_val = self.load_reg(reg::R0 + rn as usize);
        let pc_val = self.builder.ins().iconst(types::I32, pc as i64);
        let target = self.builder.ins().iadd(pc_val, rn_val);
        let target = self.mask24(target);
        self.store_pc(target);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(1);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_bra_rn(&mut self, rn: u8, pc: u32) {
        self.set_inst_len(0);
        self.set_cycles(4);
        let rn_val = self.load_reg(reg::R0 + rn as usize);
        let pc_val = self.builder.ins().iconst(types::I32, pc as i64);
        let target = self.builder.ins().iadd(pc_val, rn_val);
        let target = self.mask24(target);
        self.store_pc(target);
    }

    pub(super) fn emit_bcc_rn(&mut self, cc: CondCode, rn: u8, pc: u32) {
        self.set_cycles(4);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let rn_val = self.load_reg(reg::R0 + rn as usize);
        let pc_val = self.builder.ins().iconst(types::I32, pc as i64);
        let target = self.builder.ins().iadd(pc_val, rn_val);
        let target = self.mask24(target);
        self.store_pc(target);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(1);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_bsr_rn(&mut self, rn: u8, pc: u32) {
        self.set_inst_len(0);
        self.set_cycles(4);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 1) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        let rn_val = self.load_reg(reg::R0 + rn as usize);
        let pc_val = self.builder.ins().iconst(types::I32, pc as i64);
        let target = self.builder.ins().iadd(pc_val, rn_val);
        let target = self.mask24(target);
        self.store_pc(target);
    }

    pub(super) fn emit_jmp_ea(&mut self, ea_mode: u8, next_word: u32) {
        self.set_inst_len(0);
        self.set_cycles(3);
        let (addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);
        self.store_pc(addr);
    }

    pub(super) fn emit_jsr_ea(&mut self, ea_mode: u8, pc: u32, next_word: u32) {
        self.set_inst_len(0);
        self.set_cycles(3);
        let (addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);
        // Return address: for mode 6 (2-word), ret = pc+2; otherwise pc+1
        let mode = (ea_mode >> 3) & 0x7;
        let ret_offset = if mode == 6 { 2 } else { 1 };
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + ret_offset) as i64);
        self.emit_interrupt_aware_stack_push(ret);
        self.store_pc(addr);
    }

    pub(super) fn emit_jcc_ea(&mut self, cc: CondCode, ea_mode: u8, next_word: u32) {
        self.set_cycles(4);
        let (addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        self.store_pc(addr);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(inst_len_for_ea(ea_mode));
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_jscc(&mut self, cc: CondCode, addr: u32, pc: u32) {
        self.set_cycles(4);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let ret_pc = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + 1) as i64);
        self.emit_interrupt_aware_stack_push(ret_pc);
        let target = self.builder.ins().iconst(types::I32, addr as i64);
        self.store_pc(target);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(1);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_jscc_ea(&mut self, cc: CondCode, ea_mode: u8, pc: u32, next_word: u32) {
        self.set_cycles(4);
        let (ea_addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);
        let taken = self.eval_cc_bool(cc);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mode = (ea_mode >> 3) & 0x7;
        let ret_offset = if mode == 6 { 2 } else { 1 };

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);
        let ret_pc = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(pc + ret_offset) as i64);
        self.emit_interrupt_aware_stack_push(ret_pc);
        self.store_pc(ea_addr);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(if mode == 6 { 2 } else { 1 });
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }

    pub(super) fn emit_wait(&mut self) {
        self.set_inst_len(1);
        self.set_cycles(10);
        let val = self
            .builder
            .ins()
            .iconst(types::I8, PowerState::Wait as i64);
        let off = offset_of!(DspState, power_state) as i32;
        self.builder
            .ins()
            .store(Self::flags(), val, self.state_ptr, off);
    }

    pub(super) fn emit_reset(&mut self) {
        self.set_inst_len(1);
        self.set_cycles(7);
    }

    pub(super) fn emit_stop(&mut self) {
        self.set_inst_len(1);
        self.set_cycles(10);
        let val = self
            .builder
            .ins()
            .iconst(types::I8, PowerState::Stop as i64);
        let off = offset_of!(DspState, power_state) as i32;
        self.builder
            .ins()
            .store(Self::flags(), val, self.state_ptr, off);
    }

    /// Emit a NOP-like instruction (just sets length and cycles, no side effects).
    pub(super) fn emit_nop_like(&mut self, len: u32) {
        self.set_inst_len(len);
        self.set_cycles(1);
    }

    // IFcc / IFcc.U: conditional ALU execution
    // IFcc:   0010 0000 0010 CCCC alu -- CCR never updated
    // IFcc.U: 0010 0000 0011 CCCC alu -- CCR updated if cc true
    pub(super) fn emit_ifcc(&mut self, opcode: u32, alu: &ParallelAlu, update_ccr: bool) {
        let cc_bits = (opcode >> 8) & 0xF;
        let cc = CondCode::from_bits(cc_bits);

        // Evaluate condition BEFORE ALU (uses current CCR)
        let cond = self.eval_cc(cc);

        // Save accumulators and SR before ALU op
        let save_a = self.load_acc(Accumulator::A);
        let save_b = self.load_acc(Accumulator::B);
        let save_sr = self.load_reg(reg::SR);

        // Execute ALU op (unconditionally -- modifies accumulators and flags)
        self.emit_parallel_alu(alu);

        // For IFcc: always restore SR (CCR never updated)
        // For IFcc.U: restore SR only if condition is false
        if update_ccr {
            // IFcc.U: conditionally update CCR
            let post_sr = self.load_reg(reg::SR);
            let sr_result = self.builder.ins().select(cond, post_sr, save_sr);
            self.store_reg(reg::SR, sr_result);
        } else {
            // IFcc: never update CCR
            self.store_reg(reg::SR, save_sr);
        }

        // Conditionally restore accumulators (if cc false, undo ALU writes)
        let post_a = self.load_acc(Accumulator::A);
        let a_result = self.builder.ins().select(cond, post_a, save_a);
        self.store_acc(Accumulator::A, a_result);

        let post_b = self.load_acc(Accumulator::B);
        let b_result = self.builder.ins().select(cond, post_b, save_b);
        self.store_acc(Accumulator::B, b_result);
    }

    /// Read the operand value for a bit-test instruction given the addressing mode.
    ///
    /// `pop_ssh`: when true and the register is SSH, perform a stack pop
    /// (decrement SP).  The manual specifies this for BRCLR/BRSET/BSCLR/BSSET
    /// (p.13-26,29,32,35) but NOT for JCLR/JSET/JSCLR/JSSET or BCLR/BSET/
    /// BCHG/BTST.
    pub(super) fn read_bit_test_operand(&mut self, addr: &BitTestAddr, pop_ssh: bool) -> Value {
        match *addr {
            BitTestAddr::Pp { space, pp_offset } => {
                self.read_mem(space, 0xFFFFC0u32 + pp_offset as u32)
            }
            BitTestAddr::Qq { space, qq_offset } => {
                self.read_mem(space, PERIPH_BASE + qq_offset as u32)
            }
            BitTestAddr::Aa { space, addr } => self.read_mem(space, addr as u32),
            BitTestAddr::Reg { reg_idx } => {
                if pop_ssh && reg_idx as usize == reg::SSH {
                    // BR*/BS* variants pop SSH per the manual.
                    self.emit_call_extern_ret(jit_read_ssh as *const () as usize)
                } else {
                    // Plain register access - no move side effects.
                    self.load_reg(reg_idx as usize)
                }
            }
            BitTestAddr::Ea { space, ea_mode } => {
                let (ea_addr, _) = self.emit_calc_ea(ea_mode as u32);
                self.read_mem_dyn(space, ea_addr)
            }
        }
    }

    /// Unified bit-test-and-branch emitter for jclr/jset, jsclr/jsset,
    /// brclr/brset, bsclr/bsset.
    pub(super) fn emit_bit_test_branch(
        &mut self,
        addr: &BitTestAddr,
        bit_num: u8,
        next_word: u32,
        test_set: bool,
        branch: BitTestBranch,
    ) {
        let cycles = match &branch {
            BitTestBranch::Jump | BitTestBranch::JumpSub { .. } => 4,
            BitTestBranch::Branch { .. } | BitTestBranch::BranchSub { .. } => 5,
        };
        self.set_cycles(cycles);
        let pop_ssh = matches!(
            branch,
            BitTestBranch::Branch { .. } | BitTestBranch::BranchSub { .. }
        );
        let val = self.read_bit_test_operand(addr, pop_ssh);

        let mask = self.builder.ins().iconst(types::I32, 1i64 << bit_num);
        let masked = self.builder.ins().band(val, mask);
        let zero = self.builder.ins().iconst(types::I32, 0);
        let cc = if test_set {
            IntCC::NotEqual
        } else {
            IntCC::Equal
        };
        let cond = self.builder.ins().icmp(cc, masked, zero);

        let taken_blk = self.builder.create_block();
        let not_taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(cond, taken_blk, &[], not_taken_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);

        // Push return address if subroutine variant
        match &branch {
            BitTestBranch::JumpSub { pc } | BitTestBranch::BranchSub { pc } => {
                let return_addr = self
                    .builder
                    .ins()
                    .iconst(types::I32, mask_pc(*pc + 2) as i64);
                self.emit_interrupt_aware_stack_push(return_addr);
            }
            _ => {}
        }

        // Compute target address
        let target = match &branch {
            BitTestBranch::Jump | BitTestBranch::JumpSub { .. } => next_word,
            BitTestBranch::Branch { pc } | BitTestBranch::BranchSub { pc } => {
                mask_pc(pc.wrapping_add(next_word))
            }
        };
        let target_val = self.builder.ins().iconst(types::I32, target as i64);
        self.store_pc(target_val);
        self.set_inst_len(0);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(not_taken_blk);
        self.builder.seal_block(not_taken_blk);
        self.set_inst_len(2);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }
}
