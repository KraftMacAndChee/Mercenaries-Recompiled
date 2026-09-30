use super::*;

impl<'a> Emitter<'a> {
    /// Mask a value to 16-bit LC width using REG_MASKS[reg::LC].
    fn mask_lc(&mut self, val: Value) -> Value {
        let lc_mask = self
            .builder
            .ins()
            .iconst(types::I32, REG_MASKS[reg::LC] as i64);
        self.builder.ins().band(val, lc_mask)
    }

    /// Returns true if the instruction is a REP variant (handled inline
    /// by `emit_block` as a Cranelift loop rather than as a block terminator).
    pub(super) fn is_rep_instruction(inst: &Instruction) -> bool {
        matches!(
            inst,
            Instruction::RepImm { .. }
                | Instruction::RepAa { .. }
                | Instruction::RepEa { .. }
                | Instruction::RepReg { .. }
        )
    }

    /// Returns true if the instruction is a DO/DOR variant that may be
    /// inlined as a Cranelift loop when the body is safe.
    pub(super) fn is_do_instruction(inst: &Instruction) -> bool {
        matches!(
            inst,
            Instruction::DoImm { .. }
                | Instruction::DoReg { .. }
                | Instruction::DoForever
                | Instruction::DoAa { .. }
                | Instruction::DoEa { .. }
                | Instruction::DorImm { .. }
                | Instruction::DorReg { .. }
                | Instruction::DorForever
                | Instruction::DorAa { .. }
                | Instruction::DorEa { .. }
        )
    }

    /// Extract the absolute loop-end address (LA) from a DO/DOR instruction.
    pub(super) fn compute_do_la(inst: &Instruction, pc: u32, next_word: u32) -> u32 {
        match inst {
            // DO variants use absolute LA from next_word.
            Instruction::DoImm { .. }
            | Instruction::DoReg { .. }
            | Instruction::DoForever
            | Instruction::DoAa { .. }
            | Instruction::DoEa { .. } => next_word & REG_MASKS[reg::LA],
            // DOR variants use relative LA: pc + next_word.
            Instruction::DorImm { .. }
            | Instruction::DorReg { .. }
            | Instruction::DorForever
            | Instruction::DorAa { .. }
            | Instruction::DorEa { .. } => mask_pc(pc.wrapping_add(next_word)),
            _ => unreachable!("not a DO instruction"),
        }
    }

    /// Check whether a DO loop body [body_start, la] can be safely inlined.
    ///
    /// Returns true only if the body contains no block terminators, no
    /// peripheral writes that may set idle, no P-memory writes, fits
    /// within the block size limit, and doesn't cross an outer DO loop
    /// boundary.
    pub(super) fn is_do_body_inlineable(
        map: &MemoryMap,
        body_start: u32,
        la: u32,
        outer_stop_pc: u32,
    ) -> bool {
        Self::is_do_body_inlineable_inner(map, body_start, la, outer_stop_pc, 0)
    }

    pub(super) fn is_do_body_inlineable_inner(
        map: &MemoryMap,
        body_start: u32,
        la: u32,
        outer_stop_pc: u32,
        depth: u32,
    ) -> bool {
        const MAX_NESTING_DEPTH: u32 = 8;
        if depth > MAX_NESTING_DEPTH {
            return false;
        }
        let p_end = map.p_space_end();
        // Body must fit within PRAM bounds.
        if la >= p_end {
            return false;
        }
        // Body must not extend past an outer DO loop boundary.
        if la + 1 > outer_stop_pc {
            return false;
        }
        // Body must start at or before LA (non-empty body).
        if body_start > la {
            return false;
        }

        let mut body_pc = body_start;
        let mut count = 0u32;
        const MAX_INLINE_LEN: u32 = 64;

        while body_pc <= la {
            if body_pc >= p_end {
                return false;
            }
            let opcode = map.read_pram(body_pc);
            let inst = decode::decode(opcode);
            let inst_len = decode::instruction_length(&inst);

            if Self::is_do_instruction(&inst) {
                // DO FOREVER cannot be inlined (infinite native loop)
                if matches!(inst, Instruction::DoForever | Instruction::DorForever) {
                    return false;
                }
                // Nested DO/DOR: recursively check if the inner body is safe.
                let nw = map.read_pram(mask_pc(body_pc + 1));
                let inner_la = Self::compute_do_la(&inst, body_pc, nw);
                if inner_la > la {
                    return false;
                }
                let inner_body_start = body_pc + 2;
                if !Self::is_do_body_inlineable_inner(
                    map,
                    inner_body_start,
                    inner_la,
                    la + 1,
                    depth + 1,
                ) {
                    return false;
                }
                let inner_count = Self::count_body_instructions(map, inner_body_start, inner_la);
                count += 1 + inner_count;
                body_pc = inner_la + 1;
                continue;
            }

            if Self::is_block_terminator(&inst) {
                return false;
            }
            if Self::needs_exit_check(&inst) {
                return false;
            }
            if Self::writes_p_memory(&inst) {
                return false;
            }

            if Self::is_rep_instruction(&inst) {
                // REP consumes itself (1 word) + the repeated instruction.
                let rep_next = body_pc + 1;
                if rep_next >= p_end {
                    return false;
                }
                let rep_opcode = map.read_pram(rep_next);
                let rep_inst = decode::decode(rep_opcode);
                let rep_len = decode::instruction_length(&rep_inst);
                // The repeated instruction must also be safe.
                if Self::is_block_terminator(&rep_inst)
                    || Self::needs_exit_check(&rep_inst)
                    || Self::writes_p_memory(&rep_inst)
                {
                    return false;
                }
                // REP + repeated instruction must fit within the loop body.
                if rep_next + rep_len > la + 1 {
                    return false;
                }
                body_pc = rep_next + rep_len;
                count += 2;
            } else {
                body_pc += inst_len;
                count += 1;
            }
        }

        // Instructions must tile [body_start, la+1) exactly.
        if body_pc != la + 1 {
            return false;
        }
        // Practical size limit.
        if count > MAX_INLINE_LEN {
            return false;
        }
        true
    }

    /// Count instructions in a DO loop body [body_start, la] for the
    /// emit_block instruction budget. Only called after is_do_body_inlineable
    /// returned true, so we know the body is well-formed.
    pub(super) fn count_body_instructions(map: &MemoryMap, body_start: u32, la: u32) -> u32 {
        let mut body_pc = body_start;
        let mut count = 0u32;
        while body_pc <= la {
            let opcode = map.read_pram(body_pc);
            let inst = decode::decode(opcode);
            if Self::is_do_instruction(&inst) {
                let nw = map.read_pram(mask_pc(body_pc + 1));
                let inner_la = Self::compute_do_la(&inst, body_pc, nw);
                let inner_body_start = body_pc + 2;
                let inner_count = Self::count_body_instructions(map, inner_body_start, inner_la);
                count += 1 + inner_count;
                body_pc = inner_la + 1;
            } else if Self::is_rep_instruction(&inst) {
                let rep_opcode = map.read_pram(mask_pc(body_pc + 1));
                let rep_inst = decode::decode(rep_opcode);
                body_pc += 1 + decode::instruction_length(&rep_inst);
                count += 2;
            } else {
                body_pc += decode::instruction_length(&inst);
                count += 1;
            }
        }
        count
    }

    /// Emit a REP instruction as an inline Cranelift loop. The repeated
    /// instruction (next in pram) executes LC times inside a native loop.
    ///
    /// This is only used by `emit_block()`. `compile_instruction()` still
    /// uses the original `emit_rep_*` functions which set `loop_rep` for
    /// step-by-step debugging via `postexecute_update_pc`.
    pub(super) fn emit_rep_inline(&mut self, rep_inst: &Instruction, rep_pc: u32) {
        // 1. Emit REP setup: save TEMP, compute LC
        let old_lc = self.load_reg(reg::LC);
        self.store_reg(reg::TEMP, old_lc);
        self.set_cycles(5); // REP overhead

        let lc_val = self.emit_rep_lc_value(rep_inst);

        // REP with LC=0: repeat 65,536 times (page 13-160)
        let zero = self.builder.ins().iconst(types::I32, 0);
        let is_zero = self.builder.ins().icmp(IntCC::Equal, lc_val, zero);
        let big = self.builder.ins().iconst(types::I32, 0x10000);
        let lc_init = self.builder.ins().select(is_zero, big, lc_val);
        self.store_reg(reg::LC, lc_init);

        // 2. Decode the next instruction (the one to repeat)
        let next_pc = rep_pc + 1;
        let next_opcode = self.map.read_pram(next_pc);
        let next_next_word = self.map.read_pram(mask_pc(next_pc + 1));
        let next_inst = decode::decode(next_opcode);

        // 3. Create Cranelift loop with deferred pre-loop block
        let pre_loop = self.builder.create_block();
        let loop_header = self.builder.create_block();
        let loop_exit = self.builder.create_block();

        self.flush_pending_cycles(); // flush pre-REP cycles before entering loop
        self.builder.ins().jump(pre_loop, &[]);
        self.builder.switch_to_block(loop_header);
        // Don't seal loop_header yet - back-edge pending

        // 4. Push loop scope and emit the repeated instruction
        self.push_loop_scope(pre_loop);
        self.emit_instruction(&next_inst, next_pc, next_next_word);

        // 5. Decrement LC, check if done
        self.flush_pending_cycles(); // flush body cycles once per iteration
        self.emit_lc_decrement_and_branch(loop_header, loop_exit);

        // 6. Pop loop scope and emit pre-loop block with targeted loads
        self.pop_loop_scope(loop_header);

        // 7. Switch to loop exit
        self.builder.switch_to_block(loop_exit);
        self.builder.seal_block(loop_exit);

        // 8. Restore LC from TEMP
        let saved_lc = self.load_reg(reg::TEMP);
        self.store_reg(reg::LC, saved_lc);
    }

    /// Emit a DO/DOR instruction as an inline Cranelift loop. The loop body
    /// [do_pc+2, la] executes LC times inside a native loop with no per-
    /// iteration run-loop overhead.
    ///
    /// Only called from `emit_block()` after `is_do_body_inlineable` returns
    /// true. The caller must advance `pc` past the entire loop (to `la + 1`).
    pub(super) fn emit_do_inline(&mut self, do_inst: &Instruction, do_pc: u32, la: u32) {
        // 1. DO setup: push stack, set LA/LC/LF.
        let overhead_cycles: i64 = 5; // all DO/DOR variants: Table A-1
        self.set_inst_len(2);
        self.set_cycles(overhead_cycles);

        let lc_val = self.emit_do_lc_value(do_inst);
        let la_val = self.builder.ins().iconst(types::I32, la as i64);
        let forever = matches!(do_inst, Instruction::DoForever | Instruction::DorForever);
        self.emit_do_setup(la_val, lc_val, do_pc + 2, forever);

        // 2. Create Cranelift loop with deferred pre-loop block.
        let pre_loop = self.builder.create_block();
        let loop_header = self.builder.create_block();
        let loop_exit = self.builder.create_block();
        let after_loop = self.builder.create_block();

        // Check for DO annul (LC=0): skip body entirely
        self.flush_pending_cycles(); // flush pre-DO cycles before entering loop
        self.emit_do_annul_check(lc_val, forever, do_pc, la, after_loop);
        self.builder.ins().jump(pre_loop, &[]);
        self.builder.switch_to_block(loop_header);
        // Don't seal loop_header yet -- back-edge coming.

        // 3. Push loop scope and emit all body instructions [do_pc+2, la].
        self.push_loop_scope(pre_loop);
        let body_start = do_pc + 2;
        let mut body_pc = body_start;
        while body_pc <= la {
            let opcode = self.map.read_pram(body_pc);
            let nw = self.map.read_pram(mask_pc(body_pc + 1));
            let inst = decode::decode(opcode);

            if Self::is_do_instruction(&inst) {
                let inner_la = Self::compute_do_la(&inst, body_pc, nw);
                self.emit_do_inline(&inst, body_pc, inner_la);
                body_pc = inner_la + 1;
            } else if Self::is_rep_instruction(&inst) {
                self.emit_rep_inline(&inst, body_pc);
                let rep_next = body_pc + 1;
                let rep_opcode = self.map.read_pram(rep_next);
                let rep_inst = decode::decode(rep_opcode);
                let rep_len = decode::instruction_length(&rep_inst);
                body_pc = rep_next + rep_len;
            } else {
                self.emit_instruction(&inst, body_pc, nw);
                body_pc += decode::instruction_length(&inst);
            }
        }

        // 4. Decrement LC, check loop continuation.
        self.flush_pending_cycles(); // flush body cycles once per iteration
        self.emit_lc_decrement_and_branch(loop_header, loop_exit);

        // 5. Pop loop scope and emit pre-loop block with targeted loads.
        self.pop_loop_scope(loop_header);

        // 6. Switch to loop exit.
        self.builder.switch_to_block(loop_exit);
        self.builder.seal_block(loop_exit);

        // 7. Loop exit cleanup: pop stack, restore LA/LC/LF.
        self.emit_enddo_cleanup();
        self.builder.ins().jump(after_loop, &[]);

        // 8. Merge point (reached from loop exit or annul skip).
        self.builder.switch_to_block(after_loop);
        self.builder.seal_block(after_loop);
    }

    /// Compute the LC value for a REP instruction (from immediate, register,
    /// or memory). Returns a Cranelift Value with the 16-bit loop count.
    pub(super) fn emit_rep_lc_value(&mut self, inst: &Instruction) -> Value {
        match inst {
            Instruction::RepImm { count } => self.builder.ins().iconst(types::I32, *count as i64),
            Instruction::RepReg { reg_idx } => {
                let val = self.read_reg_for_move(*reg_idx as usize);
                self.mask_lc(val)
            }
            Instruction::RepAa { space, addr } => self.read_mem(*space, *addr as u32),
            Instruction::RepEa { space, ea_mode } => {
                let (ea_addr, _) = self.emit_calc_ea(*ea_mode as u32);
                self.read_mem_dyn(*space, ea_addr)
            }
            _ => unreachable!("not a REP instruction"),
        }
    }

    /// Compute the LC value for a DO/DOR instruction (from immediate, register,
    /// or memory). Returns a Cranelift Value with the 16-bit loop count.
    pub(super) fn emit_do_lc_value(&mut self, inst: &Instruction) -> Value {
        match inst {
            Instruction::DoImm { count } | Instruction::DorImm { count } => {
                self.builder.ins().iconst(types::I32, *count as i64)
            }
            Instruction::DoForever | Instruction::DorForever => {
                // DO FOREVER: LC is not updated - load current value to preserve it
                self.load_reg(reg::LC)
            }
            Instruction::DoReg { reg_idx } | Instruction::DorReg { reg_idx } => {
                let mut val = self.read_reg_for_move(*reg_idx as usize);
                // Manual page 13-56: "For the DO SP, expr instruction, the actual
                // value loaded into LC is SP before DO, incremented by one."
                if *reg_idx as usize == reg::SP {
                    let one = self.builder.ins().iconst(types::I32, 1);
                    val = self.builder.ins().iadd(val, one);
                }
                self.mask_lc(val)
            }
            Instruction::DoAa { space, addr } => {
                let mem_val = self.read_mem(*space, *addr as u32);
                self.mask_lc(mem_val)
            }
            Instruction::DoEa { space, ea_mode } | Instruction::DorEa { space, ea_mode } => {
                let (ea_addr, _) = self.emit_calc_ea(*ea_mode as u32);
                let mem_val = self.read_mem_dyn(*space, ea_addr);
                self.mask_lc(mem_val)
            }
            Instruction::DorAa { space, addr } => {
                let mem_val = self.read_mem(*space, *addr as u32);
                self.mask_lc(mem_val)
            }
            _ => unreachable!("not a DO instruction"),
        }
    }

    /// Common REP setup: save LC to TEMP, set loop_rep and pc_on_rep.
    pub(super) fn emit_rep_setup(&mut self) {
        let old_lc = self.load_reg(reg::LC);
        self.store_reg(reg::TEMP, old_lc);
        let one = self.builder.ins().iconst(types::I8, 1);
        self.store_bool(OFF_LOOP_REP, one);
        self.store_bool(OFF_PC_ON_REP, one);
    }

    pub(super) fn emit_rep_imm(&mut self, count: u16) {
        self.set_inst_len(1);
        self.set_cycles(5);
        self.emit_rep_setup();
        let cv = self.builder.ins().iconst(types::I32, count as i64);
        self.store_reg(reg::LC, cv);
    }

    /// ENDDO cleanup: pop (PC, SR), restore LF+FV from saved SR, pop (LA, LC).
    pub(super) fn emit_enddo_cleanup(&mut self) {
        let (_saved_pc, saved_sr) = self.stack_pop();
        let sr_val = self.load_reg(reg::SR);
        let lf_fv_mask = (1u32 << sr::LF) | (1u32 << sr::FV);
        let mask = self.builder.ins().iconst(types::I32, lf_fv_mask as i64);
        let inv_mask = self.builder.ins().iconst(types::I32, !lf_fv_mask as i64);
        let sr_without = self.builder.ins().band(sr_val, inv_mask);
        let saved_flags = self.builder.ins().band(saved_sr, mask);
        let sr_new = self.builder.ins().bor(sr_without, saved_flags);
        self.store_reg(reg::SR, sr_new);
        let (la, lc) = self.stack_pop();
        self.store_reg(reg::LA, la);
        self.store_reg(reg::LC, lc);
    }

    pub(super) fn emit_enddo(&mut self) {
        self.set_inst_len(1);
        self.set_cycles(1);
        self.emit_enddo_cleanup();
    }

    /// Common DO loop setup: push LA/LC, set LA, push ret_pc/SR, set LF (+FV for forever), set LC.
    ///
    /// For non-forever DO with LC=0 and SC=0, the loop should be annulled
    /// (DOR p.13-61, Table 5-1 bit 13). Callers handle this via
    /// `emit_do_annul_check`.
    pub(super) fn emit_do_setup(
        &mut self,
        la_val: Value,
        lc_val: Value,
        ret_pc: u32,
        forever: bool,
    ) {
        let old_la = self.load_reg(reg::LA);
        let old_lc = self.load_reg(reg::LC);
        self.stack_push(old_la, old_lc);
        self.store_reg(reg::LA, la_val);
        let ret = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(ret_pc) as i64);
        let sr_val = self.load_reg(reg::SR);
        self.stack_push(ret, sr_val);
        let sr_new = if forever {
            let flags = (1u32 << sr::LF) | (1u32 << sr::FV);
            let flag_bits = self.builder.ins().iconst(types::I32, flags as i64);
            self.builder.ins().bor(sr_val, flag_bits)
        } else {
            // Set LF, clear FV (a regular DO nested inside DO FOREVER must not
            // inherit the FV=1 from the outer loop).
            let set_lf = self
                .builder
                .ins()
                .iconst(types::I32, (1u32 << sr::LF) as i64);
            let sr_with_lf = self.builder.ins().bor(sr_val, set_lf);
            let clear_fv = self
                .builder
                .ins()
                .iconst(types::I32, !(1u32 << sr::FV) as i64);
            self.builder.ins().band(sr_with_lf, clear_fv)
        };
        self.store_reg(reg::SR, sr_new);
        self.store_reg(reg::LC, lc_val);
    }

    /// Emit a conditional annul check for DO with LC=0.
    ///
    /// If `lc_val == 0`, undoes the DO setup (pops stack, restores LF/FV),
    /// sets `inst_len` to skip past the loop body (to LA+1), and jumps to
    /// `annul_target`. Otherwise falls through.
    /// No-op for DO FOREVER (forever flag set).
    ///
    /// Per DOR page 13-61: "If the LC initial value is zero [...] the DO
    /// loop is not executed." (SC=0 behavior; SC=1 not implemented.)
    pub(super) fn emit_do_annul_check(
        &mut self,
        lc_val: Value,
        forever: bool,
        _do_pc: u32,
        la: u32,
        annul_target: Block,
    ) {
        if forever {
            return;
        }
        let zero = self.builder.ins().iconst(types::I32, 0);
        let lc_is_zero = self.builder.ins().icmp(IntCC::Equal, lc_val, zero);

        let continue_block = self.builder.create_block();
        let annul_block = self.builder.create_block();
        self.builder
            .ins()
            .brif(lc_is_zero, annul_block, &[], continue_block, &[]);

        // Annul block: undo DO setup, jump to LA+1 using the branch pattern
        // (store_pc + inst_len=0) so the block JIT path picks up the correct
        // target address instead of falling through to the loop body.
        self.builder.switch_to_block(annul_block);
        self.builder.seal_block(annul_block);
        self.emit_enddo_cleanup();
        let target = self
            .builder
            .ins()
            .iconst(types::I32, mask_pc(la + 1) as i64);
        self.store_pc(target);
        self.set_inst_len(0);
        self.builder.ins().jump(annul_target, &[]);

        self.builder.switch_to_block(continue_block);
        self.builder.seal_block(continue_block);
    }

    /// Decrement LC, mask it, store it, and branch back to loop_header or to loop_exit.
    fn emit_lc_decrement_and_branch(&mut self, loop_header: Block, loop_exit: Block) {
        let lc = self.load_reg(reg::LC);
        let one = self.builder.ins().iconst(types::I32, 1);
        let new_lc = self.builder.ins().isub(lc, one);
        let new_lc = self.mask_lc(new_lc);
        self.store_reg(reg::LC, new_lc);
        let zero = self.builder.ins().iconst(types::I32, 0);
        let done = self.builder.ins().icmp(IntCC::Equal, new_lc, zero);
        self.builder
            .ins()
            .brif(done, loop_exit, &[], loop_header, &[]);
    }

    /// Shared tail for non-forever DO/DOR: setup, annul check, merge block.
    fn emit_do_tail(&mut self, la_val: Value, lc_val: Value, pc: u32, la: u32) {
        let merge = self.builder.create_block();
        self.emit_do_setup(la_val, lc_val, pc + 2, false);
        self.emit_do_annul_check(lc_val, false, pc, la, merge);
        self.builder.ins().jump(merge, &[]);
        self.builder.switch_to_block(merge);
        self.builder.seal_block(merge);
    }

    /// Compute LA for DO (absolute) or DOR (relative).
    fn compute_la(pc: u32, next_word: u32, relative: bool) -> u32 {
        if relative {
            mask_pc(pc.wrapping_add(next_word))
        } else {
            next_word & REG_MASKS[reg::LA]
        }
    }

    fn emit_do_or_dor_imm(&mut self, count: u16, pc: u32, next_word: u32, relative: bool) {
        self.set_inst_len(2);
        self.set_cycles(5);
        let la = Self::compute_la(pc, next_word, relative);
        let la_val = self.builder.ins().iconst(types::I32, la as i64);
        let lc_val = self.builder.ins().iconst(types::I32, count as i64);
        self.emit_do_tail(la_val, lc_val, pc, la);
    }

    pub(super) fn emit_do_imm(&mut self, count: u16, pc: u32, next_word: u32) {
        self.emit_do_or_dor_imm(count, pc, next_word, false);
    }

    pub(super) fn emit_dor_imm(&mut self, count: u16, pc: u32, next_word: u32) {
        self.emit_do_or_dor_imm(count, pc, next_word, true);
    }

    fn emit_do_or_dor_forever(&mut self, pc: u32, next_word: u32, relative: bool) {
        self.set_inst_len(2);
        // DO FOREVER is 4 cycles, DOR FOREVER is 5 cycles.
        self.set_cycles(if relative { 5 } else { 4 });
        let la = Self::compute_la(pc, next_word, relative);
        let la_val = self.builder.ins().iconst(types::I32, la as i64);
        let lc_val = self.load_reg(reg::LC);
        self.emit_do_setup(la_val, lc_val, pc + 2, true);
    }

    pub(super) fn emit_do_forever(&mut self, pc: u32, next_word: u32) {
        self.emit_do_or_dor_forever(pc, next_word, false);
    }

    pub(super) fn emit_dor_forever(&mut self, pc: u32, next_word: u32) {
        self.emit_do_or_dor_forever(pc, next_word, true);
    }

    fn emit_do_or_dor_reg(&mut self, reg_idx: u8, pc: u32, next_word: u32, relative: bool) {
        self.set_inst_len(2);
        self.set_cycles(5);
        let la = Self::compute_la(pc, next_word, relative);
        let la_val = self.builder.ins().iconst(types::I32, la as i64);
        let numreg = reg_idx as usize;
        let mut val = self.read_reg_for_move(numreg);
        // Manual page 13-56: "For the DO SP, expr instruction, the actual value
        // that is loaded into the LC is the value of SP before the DO instruction
        // executes, incremented by one."
        if numreg == reg::SP {
            let one = self.builder.ins().iconst(types::I32, 1);
            val = self.builder.ins().iadd(val, one);
        }
        let lc_val = self.mask_lc(val);
        self.emit_do_tail(la_val, lc_val, pc, la);
    }

    pub(super) fn emit_do_reg(&mut self, reg_idx: u8, pc: u32, next_word: u32) {
        self.emit_do_or_dor_reg(reg_idx, pc, next_word, false);
    }

    pub(super) fn emit_dor_reg(&mut self, reg_idx: u8, pc: u32, next_word: u32) {
        self.emit_do_or_dor_reg(reg_idx, pc, next_word, true);
    }

    pub(super) fn emit_rep_reg(&mut self, reg_idx: u8) {
        self.set_inst_len(1);
        self.set_cycles(5);
        self.emit_rep_setup();
        let numreg = reg_idx as usize;
        let lc_val = self.read_reg_for_move(numreg);
        let lc_masked = self.mask_lc(lc_val);
        self.store_reg(reg::LC, lc_masked);
    }

    fn emit_do_or_dor_aa(
        &mut self,
        space: MemSpace,
        addr: u8,
        pc: u32,
        next_word: u32,
        relative: bool,
    ) {
        self.set_inst_len(2);
        self.set_cycles(5);
        let la = Self::compute_la(pc, next_word, relative);
        let la_val = self.builder.ins().iconst(types::I32, la as i64);
        let addr = addr as u32;
        let mem_val = self.read_mem(space, addr);
        let lc_val = self.mask_lc(mem_val);
        self.emit_do_tail(la_val, lc_val, pc, la);
    }

    pub(super) fn emit_do_aa(&mut self, space: MemSpace, addr: u8, pc: u32, next_word: u32) {
        self.emit_do_or_dor_aa(space, addr, pc, next_word, false);
    }

    pub(super) fn emit_dor_aa(&mut self, space: MemSpace, addr: u8, pc: u32, next_word: u32) {
        self.emit_do_or_dor_aa(space, addr, pc, next_word, true);
    }

    fn emit_do_or_dor_ea(
        &mut self,
        space: MemSpace,
        ea_mode: u8,
        pc: u32,
        next_word: u32,
        relative: bool,
    ) {
        self.set_inst_len(2);
        self.set_cycles(5);
        let la = Self::compute_la(pc, next_word, relative);
        let la_val = self.builder.ins().iconst(types::I32, la as i64);
        let (ea_addr, _) = self.emit_calc_ea(ea_mode as u32);
        let mem_val = self.read_mem_dyn(space, ea_addr);
        let lc_val = self.mask_lc(mem_val);
        self.emit_do_tail(la_val, lc_val, pc, la);
    }

    pub(super) fn emit_do_ea(&mut self, space: MemSpace, ea_mode: u8, pc: u32, next_word: u32) {
        self.emit_do_or_dor_ea(space, ea_mode, pc, next_word, false);
    }

    pub(super) fn emit_dor_ea(&mut self, space: MemSpace, ea_mode: u8, pc: u32, next_word: u32) {
        self.emit_do_or_dor_ea(space, ea_mode, pc, next_word, true);
    }

    pub(super) fn emit_rep_aa(&mut self, space: MemSpace, addr: u8) {
        self.set_inst_len(1);
        self.set_cycles(5);
        self.emit_rep_setup();
        let addr = addr as u32;
        let mem_val = self.read_mem(space, addr);
        self.store_reg(reg::LC, mem_val);
    }

    pub(super) fn emit_rep_ea(&mut self, space: MemSpace, ea_mode: u8) {
        self.set_inst_len(1);
        self.set_cycles(5);
        self.emit_rep_setup();
        let (ea_addr, _) = self.emit_calc_ea(ea_mode as u32);
        let mem_val = self.read_mem_dyn(space, ea_addr);
        self.store_reg(reg::LC, mem_val);
    }

    pub(super) fn emit_brkcc(&mut self, cc: CondCode) {
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
        // Read current LA before cleanup pops it
        let la = self.load_reg(reg::LA);
        let one = self.builder.ins().iconst(types::I32, 1);
        let target = self.builder.ins().iadd(la, one);
        self.emit_enddo_cleanup();
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
}
