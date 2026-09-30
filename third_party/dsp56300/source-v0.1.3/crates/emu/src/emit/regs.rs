use super::*;

impl<'a> Emitter<'a> {
    // helpers: register access

    pub(super) fn load_reg(&mut self, idx: usize) -> Value {
        if idx == reg::SR {
            self.flush_pending_flags();
        }
        if let Some(var) = self.promoted.vars[idx] {
            if !self.promoted.valid[idx] {
                let scope = self.scope_stack.last_mut().unwrap();
                if !scope.entry_valid[idx] {
                    // Was invalid before this scope started -> defer to pre-block.
                    scope.deferred[idx] = true;
                } else {
                    // Was valid at scope entry, invalidated during body (e.g.
                    // after an external call) -> inline load from memory.
                    let val = self.load_u32(Self::reg_offset(idx));
                    self.builder.def_var(var, val);
                }
                self.promoted.valid[idx] = true;
            }
            self.builder.use_var(var)
        } else {
            match idx {
                reg::A0 | reg::A1 | reg::A2 | reg::B0 | reg::B1 | reg::B2 => {
                    self.load_acc_subreg(idx)
                }
                _ => self.load_u32(Self::reg_offset(idx)),
            }
        }
    }

    /// Extract a single sub-register from the promoted i64 accumulator Variable.
    fn load_acc_subreg(&mut self, idx: usize) -> Value {
        let (acc, extract): (Accumulator, fn(&mut Self, Value) -> Value) = match idx {
            reg::A0 => (Accumulator::A, Self::extract_acc_lo),
            reg::A1 => (Accumulator::A, Self::extract_acc_mid),
            reg::A2 => (Accumulator::A, Self::extract_acc_hi),
            reg::B0 => (Accumulator::B, Self::extract_acc_lo),
            reg::B1 => (Accumulator::B, Self::extract_acc_mid),
            reg::B2 => (Accumulator::B, Self::extract_acc_hi),
            _ => unreachable!(),
        };
        let acc_val = self.load_acc(acc);
        extract(self, acc_val)
    }

    pub(super) fn store_reg(&mut self, idx: usize, val: Value) {
        if idx == reg::SR {
            // Discard pending flags — the direct SR write supersedes them.
            self.pending_flags = None;
        }
        let mask = if (reg::R0..=reg::M7).contains(&idx) {
            self.address_register_mask
        } else {
            REG_MASKS[idx]
        };
        let v = if mask != 0xFFFFFFFF {
            let m = self.builder.ins().iconst(types::I32, mask as i64);
            self.builder.ins().band(val, m)
        } else {
            val
        };
        if let Some(var) = self.promoted.vars[idx] {
            self.builder.def_var(var, v);
            self.promoted.valid[idx] = true;
            self.promoted.dirty[idx] = true;
        } else {
            match idx {
                reg::A0 | reg::A1 | reg::A2 | reg::B0 | reg::B1 | reg::B2 => {
                    self.store_acc_subreg(idx, v);
                }
                _ => self.store_u32(Self::reg_offset(idx), v),
            }
        }
    }

    /// Insert a single sub-register value into the promoted i64 accumulator Variable.
    fn store_acc_subreg(&mut self, idx: usize, val: Value) {
        let (acc, shift, field_mask): (Accumulator, u32, u64) = match idx {
            reg::A0 => (Accumulator::A, 0, 0x00FFFFFF),
            reg::A1 => (Accumulator::A, 24, 0x00FFFFFF),
            reg::A2 => (Accumulator::A, 48, 0xFF),
            reg::B0 => (Accumulator::B, 0, 0x00FFFFFF),
            reg::B1 => (Accumulator::B, 24, 0x00FFFFFF),
            reg::B2 => (Accumulator::B, 48, 0xFF),
            _ => unreachable!(),
        };
        let cur = self.load_acc(acc);

        // Clear the target field.
        let clear_mask = !(field_mask << shift);
        let clear = self.builder.ins().iconst(types::I64, clear_mask as i64);
        let cleared = self.builder.ins().band(cur, clear);

        // Shift and mask the new value into position.
        let val64 = self.builder.ins().uextend(types::I64, val);
        let positioned = if shift > 0 {
            let fm = self.builder.ins().iconst(types::I64, field_mask as i64);
            let masked = self.builder.ins().band(val64, fm);
            let c = self.builder.ins().iconst(types::I32, shift as i64);
            self.builder.ins().ishl(masked, c)
        } else {
            let fm = self.builder.ins().iconst(types::I64, field_mask as i64);
            self.builder.ins().band(val64, fm)
        };

        let result = self.builder.ins().bor(cleared, positioned);
        self.store_acc(acc, result);
    }

    pub(super) fn store_pc(&mut self, v: Value) {
        let masked = self.mask24(v);
        self.store_u32(OFF_PC, masked);
    }

    // helpers: promoted register flush/reload

    /// Flush a single promoted register to memory if dirty.
    /// Does NOT clear the dirty flag -- safe for use before conditional
    /// branches where one path may re-dirty the register.
    pub(super) fn flush_reg(&mut self, idx: usize) {
        if self.promoted.dirty[idx] {
            let val = self.builder.use_var(self.promoted.vars[idx].unwrap());
            self.store_u32(Self::reg_offset(idx), val);
        }
    }

    /// Flush all dirty promoted registers to memory. Clears dirty flags.
    pub(super) fn flush_promoted(&mut self) {
        for &idx in &PROMOTED_REGS {
            self.flush_reg(idx);
            self.promoted.dirty[idx] = false;
        }
        for acc in [Accumulator::A, Accumulator::B] {
            let i = Self::acc_idx(acc);
            if self.promoted.acc_dirty[i] {
                self.flush_acc_to_memory(acc);
                self.promoted.acc_dirty[i] = false;
            }
        }
    }

    /// Flush the promoted registers read or written by the system-stack helper.
    /// SZ, SC, and EP are not promoted and therefore already live in memory.
    pub(super) fn flush_stack_promoted(&mut self) {
        for idx in [reg::SP, reg::SSH, reg::SSL, reg::OMR] {
            self.flush_reg(idx);
            self.promoted.dirty[idx] = false;
        }
    }

    /// Invalidate only registers the system-stack helper can modify. Keeping
    /// unrelated promoted values live is important inside conditional DO-loop
    /// emission, where invalidating the entire cache loses pending loop state.
    pub(super) fn invalidate_stack_promoted(&mut self) {
        for idx in [reg::SP, reg::SSH, reg::SSL, reg::OMR] {
            self.promoted.valid[idx] = false;
            self.promoted.dirty[idx] = false;
            if let Some(scope) = self.scope_stack.last_mut() {
                scope.entry_valid[idx] = true;
            }
        }
    }

    /// Mark all promoted registers as invalid and clear dirty flags.
    /// Used after external calls where we've already flushed everything.
    /// The next `load_reg`/`load_acc` will lazily reload from memory on demand.
    pub(super) fn invalidate_promoted(&mut self) {
        for &idx in &PROMOTED_REGS {
            self.promoted.valid[idx] = false;
        }
        self.promoted.acc_valid = [false; 2];
        self.promoted.dirty = [false; 64];
        self.promoted.acc_dirty = [false; 2];
        // Mark all registers as "valid at scope entry" so that subsequent
        // load_reg calls use inline memory reads (which see the extern call's
        // side effects) rather than deferred block-entry loads (which would
        // read stale pre-call values).
        if let Some(scope) = self.scope_stack.last_mut() {
            for &idx in &PROMOTED_REGS {
                scope.entry_valid[idx] = true;
            }
            scope.entry_acc_valid = [true; 2];
        }
    }

    /// Snapshot dirty/valid state before a conditional branch (brif).
    pub(super) fn begin_conditional(&mut self) -> ConditionalState {
        ConditionalState {
            saved_dirty: self.promoted.dirty,
            saved_acc_dirty: self.promoted.acc_dirty,
            saved_valid: self.promoted.valid,
            saved_acc_valid: self.promoted.acc_valid,
            modified: [false; 64],
            modified_acc: [false; 2],
        }
    }

    /// End a conditional arm: flush registers that became dirty in this arm
    /// to memory, record them in `modified`, then restore dirty/valid to the
    /// pre-branch snapshot so the next arm starts from the same state.
    ///
    /// Call this just before `jump(merge_blk)` in each arm.
    pub(super) fn end_conditional_arm(&mut self, state: &mut ConditionalState) {
        for &idx in &PROMOTED_REGS {
            if self.promoted.dirty[idx] && !state.saved_dirty[idx] {
                self.flush_reg(idx);
                state.modified[idx] = true;
            }
        }
        for i in 0..2 {
            if self.promoted.acc_dirty[i] && !state.saved_acc_dirty[i] {
                let acc = if i == 0 {
                    Accumulator::A
                } else {
                    Accumulator::B
                };
                self.flush_acc_to_memory(acc);
                state.modified_acc[i] = true;
            }
        }
        // Restore to pre-branch state for the next arm.
        self.promoted.dirty = state.saved_dirty;
        self.promoted.acc_dirty = state.saved_acc_dirty;
        self.promoted.valid = state.saved_valid;
        self.promoted.acc_valid = state.saved_acc_valid;
    }

    /// At the merge block after all arms: invalidate only the registers that
    /// were actually modified in any arm. Marks them as "valid at scope entry"
    /// so subsequent lazy loads use inline memory reads.
    pub(super) fn merge_conditional(&mut self, state: &ConditionalState) {
        let scope = self.scope_stack.last_mut().unwrap();
        for &idx in &PROMOTED_REGS {
            if state.modified[idx] {
                self.promoted.valid[idx] = false;
                scope.entry_valid[idx] = true;
            }
        }
        for i in 0..2 {
            if state.modified_acc[i] {
                self.promoted.acc_valid[i] = false;
                scope.entry_acc_valid[i] = true;
            }
        }
    }

    /// Flush only SR to memory (for jit_rnd56 which reads SR S0/S1 bits).
    pub(super) fn flush_sr(&mut self) {
        self.flush_reg(reg::SR);
        self.promoted.dirty[reg::SR] = false;
    }

    /// Store dirty promoted registers to memory (block exit / early return).
    /// Does NOT clear dirty flags - safe for side paths (early_ret) where
    /// the main path's dirty tracking must be preserved.
    pub(super) fn flush_all_to_memory(&mut self) {
        for &idx in &PROMOTED_REGS {
            self.flush_reg(idx);
        }
        for acc in [Accumulator::A, Accumulator::B] {
            if self.promoted.acc_dirty[Self::acc_idx(acc)] {
                self.flush_acc_to_memory(acc);
            }
        }
        // Store cur_inst_len for execute_one (postexecute_update_pc reads it).
        let il = self.builder.use_var(self.inst_len);
        self.store_u32(OFF_PC_ADVANCE, il);
    }

    // helpers: 56-bit accumulators

    pub(super) fn acc_regs(acc: Accumulator) -> (usize, usize, usize) {
        match acc {
            Accumulator::A => (reg::A2, reg::A1, reg::A0),
            Accumulator::B => (reg::B2, reg::B1, reg::B0),
        }
    }

    pub(super) fn acc_idx(acc: Accumulator) -> usize {
        match acc {
            Accumulator::A => 0,
            Accumulator::B => 1,
        }
    }

    /// Load accumulator as packed i64: (A2 << 48) | (A1 << 24) | A0
    pub(super) fn load_acc(&mut self, acc: Accumulator) -> Value {
        let i = Self::acc_idx(acc);
        if !self.promoted.acc_valid[i] {
            let scope = self.scope_stack.last_mut().unwrap();
            if !scope.entry_acc_valid[i] {
                // Was invalid before this scope started -> defer to pre-block.
                scope.acc_deferred[i] = true;
            } else {
                // Was valid at scope entry, invalidated during body -> inline reload.
                self.reload_acc(acc);
            }
            self.promoted.acc_valid[i] = true;
        }
        self.builder.use_var(self.promoted.acc[i])
    }

    /// Store packed i64 to accumulator Variable.
    pub(super) fn store_acc(&mut self, acc: Accumulator, val: Value) {
        let i = Self::acc_idx(acc);
        self.builder.def_var(self.promoted.acc[i], val);
        self.promoted.acc_valid[i] = true;
        self.promoted.acc_dirty[i] = true;
    }

    /// Pack three i32 sub-register values into a 56-bit i64 accumulator.
    pub(super) fn pack_acc(&mut self, a2: Value, a1: Value, a0: Value) -> Value {
        let v2 = self.builder.ins().uextend(types::I64, a2);
        let v1 = self.builder.ins().uextend(types::I64, a1);
        let v0 = self.builder.ins().uextend(types::I64, a0);
        let c48 = self.builder.ins().iconst(types::I32, 48);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let hi = self.builder.ins().ishl(v2, c48);
        let mid = self.builder.ins().ishl(v1, c24);
        let tmp = self.builder.ins().bor(hi, mid);
        self.builder.ins().bor(tmp, v0)
    }

    /// Extract bits 23:0 (A0/B0) from packed i64 accumulator as i32.
    pub(super) fn extract_acc_lo(&mut self, acc_val: Value) -> Value {
        let mask = self.builder.ins().iconst(types::I64, 0x00FFFFFF);
        let lo = self.builder.ins().band(acc_val, mask);
        self.builder.ins().ireduce(types::I32, lo)
    }

    /// Extract bits 47:24 (A1/B1) from packed i64 accumulator as i32.
    pub(super) fn extract_acc_mid(&mut self, acc_val: Value) -> Value {
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let shifted = self.builder.ins().ushr(acc_val, c24);
        let mask = self.builder.ins().iconst(types::I64, 0x00FFFFFF);
        let mid = self.builder.ins().band(shifted, mask);
        self.builder.ins().ireduce(types::I32, mid)
    }

    /// Extract bits 55:48 (A2/B2) from packed i64 accumulator as i32.
    pub(super) fn extract_acc_hi(&mut self, acc_val: Value) -> Value {
        let c48 = self.builder.ins().iconst(types::I32, 48);
        let shifted = self.builder.ins().ushr(acc_val, c48);
        let mask = self.builder.ins().iconst(types::I64, 0xFF);
        let hi = self.builder.ins().band(shifted, mask);
        self.builder.ins().ireduce(types::I32, hi)
    }

    /// Decompose packed i64 accumulator into three i32 memory stores.
    pub(super) fn flush_acc_to_memory(&mut self, acc: Accumulator) {
        let (r2, r1, r0) = Self::acc_regs(acc);
        let val = self.load_acc(acc);
        let lo = self.extract_acc_lo(val);
        let mid = self.extract_acc_mid(val);
        let hi = self.extract_acc_hi(val);
        self.store_u32(Self::reg_offset(r0), lo);
        self.store_u32(Self::reg_offset(r1), mid);
        self.store_u32(Self::reg_offset(r2), hi);
    }

    /// Load three i32 sub-registers from memory and pack into i64 Variable.
    pub(super) fn reload_acc(&mut self, acc: Accumulator) {
        let (r2, r1, r0) = Self::acc_regs(acc);
        let v0 = self.load_u32(Self::reg_offset(r0));
        let v1 = self.load_u32(Self::reg_offset(r1));
        let v2 = self.load_u32(Self::reg_offset(r2));
        let packed = self.pack_acc(v2, v1, v0);
        let i = Self::acc_idx(acc);
        self.builder.def_var(self.promoted.acc[i], packed);
    }

    /// Place a 24-bit i32 value at A1 position (bits 47:24), sign-extending
    /// bit 23 to A2 (bits 55:48). Used by add/sub immediates.
    pub(super) fn val24_to_acc56(&mut self, val32: Value) -> Value {
        let val64 = self.builder.ins().uextend(types::I64, val32);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let shifted = self.builder.ins().ishl(val64, c24);
        let c16 = self.builder.ins().iconst(types::I32, 16);
        let shl = self.builder.ins().ishl(shifted, c16);
        let sext = self.builder.ins().sshr(shl, c16);
        self.mask56(sext)
    }

    /// Place a 24-bit i32 value at A1 position (bits 47:24), without sign
    /// extension (A2 = 0). Used by cmp immediates.
    pub(super) fn val24_to_acc56_unsigned(&mut self, val32: Value) -> Value {
        let val64 = self.builder.ins().uextend(types::I64, val32);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let shifted = self.builder.ins().ishl(val64, c24);
        self.mask56(shifted)
    }

    pub(super) fn mask56(&mut self, val: Value) -> Value {
        let m = self
            .builder
            .ins()
            .iconst(types::I64, 0x00FFFFFFFFFFFFFF_u64 as i64);
        self.builder.ins().band(val, m)
    }

    /// Replace bits 47:0 (A1/A0) of an accumulator, preserving A2.
    pub(super) fn write_acc_lo_mid(&mut self, acc: Accumulator, mid_val: Value, lo_val: Value) {
        let cur = self.load_acc(acc);
        let a2_mask = self
            .builder
            .ins()
            .iconst(types::I64, 0x00FF_0000_0000_0000u64 as i64);
        let a2_preserved = self.builder.ins().band(cur, a2_mask);
        let lx64 = self.builder.ins().uextend(types::I64, mid_val);
        let ly64 = self.builder.ins().uextend(types::I64, lo_val);
        let mask24 = self.builder.ins().iconst(types::I64, 0x00FFFFFF);
        let lx_m = self.builder.ins().band(lx64, mask24);
        let ly_m = self.builder.ins().band(ly64, mask24);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let mid = self.builder.ins().ishl(lx_m, c24);
        let lo_mid = self.builder.ins().bor(mid, ly_m);
        let result = self.builder.ins().bor(a2_preserved, lo_mid);
        self.store_acc(acc, result);
    }

    // helpers: move register read/write

    /// Read register for move, handling A/B accumulator 24-bit read with
    /// scaling and limiting, and SSH stack pop semantics on SSH read.
    pub(super) fn read_reg_for_move(&mut self, idx: usize) -> Value {
        if idx == reg::A || idx == reg::B {
            self.read_accu24(idx)
        } else if idx == reg::SSH {
            // Reading SSH pops the stack.
            self.emit_call_extern_ret(jit_read_ssh as *const () as usize)
        } else {
            self.load_reg(idx)
        }
    }

    /// Read accumulator as 24-bit value with scaling and limiting.
    pub(super) fn read_accu24(&mut self, idx: usize) -> Value {
        let (val, _no_limit) = self.read_accu24_inner(idx);
        val
    }

    /// Like [`read_accu24`] but also returns the `no_limit` flag (nonzero = not clamped).
    /// Calls the `jit_read_accu24` extern helper which handles scaling, limiting,
    /// and S/L flag updates. Returns (24-bit value, no_limit flag).
    pub(super) fn read_accu24_inner(&mut self, idx: usize) -> (Value, Value) {
        let acc_idx = if idx == reg::A { 0u32 } else { 1u32 };
        let raw = self.emit_call_read_accu24(acc_idx);

        // Extract 24-bit result and no_limit flag from packed return value
        let result = self.mask24(raw);
        let no_limit = self.builder.ins().ushr_imm(raw, 24);

        (result, no_limit)
    }

    /// Write register for move, handling A/B accumulator write-through,
    /// SSH stack push, SSL stack update, and SP recompute.
    pub(super) fn write_reg_for_move(&mut self, idx: usize, val: Value) {
        if idx == reg::A || idx == reg::B {
            // Construct full 56-bit accumulator: A0=0, A1=val, A2=sign_ext(bit23).
            let acc = if idx == reg::A {
                Accumulator::A
            } else {
                Accumulator::B
            };
            let packed = self.val24_to_acc56(val);
            self.store_acc(acc, packed);
        } else if idx == reg::SSH {
            self.emit_call_extern_val(jit_write_ssh as *const () as usize, val);
        } else if idx == reg::SSL {
            self.emit_call_extern_val(jit_write_ssl as *const () as usize, val);
        } else if idx == reg::SP {
            self.emit_call_extern_val(jit_write_sp as *const () as usize, val);
        } else {
            self.store_reg(idx, val);
        }
    }

    /// Load a 24-bit register as 56-bit accumulator value.
    /// Places the 24-bit value in the A1 position (bits 47:24), sign-extends to A2.
    pub(super) fn load_reg24_as_acc56(&mut self, reg_idx: usize) -> Value {
        let val32 = self.load_reg(reg_idx);
        self.val24_to_acc56(val32)
    }

    /// Read an L-register pair (lx, ly) for pm_4x. The 3-bit `numreg` selects
    /// which pair: 0=A10, 1=B10, 2=X, 3=Y, 4=A(limited), 5=B(limited), 6=AB, 7=BA.
    pub(super) fn read_l_reg(&mut self, numreg: u32) -> (Value, Value) {
        match numreg {
            0 => {
                // A10: lx=A1, ly=A0
                (self.load_reg(reg::A1), self.load_reg(reg::A0))
            }
            1 => {
                // B10: lx=B1, ly=B0
                (self.load_reg(reg::B1), self.load_reg(reg::B0))
            }
            2 => {
                // X: lx=X1, ly=X0
                (self.load_reg(reg::X1), self.load_reg(reg::X0))
            }
            3 => {
                // Y: lx=Y1, ly=Y0
                (self.load_reg(reg::Y1), self.load_reg(reg::Y0))
            }
            4 => self.read_l_reg_limited(reg::A, reg::A0),
            5 => self.read_l_reg_limited(reg::B, reg::B0),
            6 => {
                let lx = self.read_accu24(reg::A);
                let ly = self.read_accu24(reg::B);
                (lx, ly)
            }
            7 => {
                let lx = self.read_accu24(reg::B);
                let ly = self.read_accu24(reg::A);
                (lx, ly)
            }
            _ => unreachable!(),
        }
    }

    /// Read a limited accumulator L-register pair. `acc` is reg::A or reg::B,
    /// `lo_reg` is the corresponding A0 or B0 sub-register.
    fn read_l_reg_limited(&mut self, acc: usize, lo_reg: usize) -> (Value, Value) {
        let (save_lx, no_limit) = self.read_accu24_inner(acc);
        // If limited: ly = bit23(lx) ? 0 : 0xFFFFFF
        // If not limited: ly = A0/B0
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let bit23 = self.builder.ins().ushr(save_lx, c23);
        let one = self.builder.ins().iconst(types::I32, 1);
        let bit23 = self.builder.ins().band(bit23, one);
        let is_neg = self.builder.ins().icmp_imm(IntCC::NotEqual, bit23, 0);
        let zero = self.builder.ins().iconst(types::I32, 0);
        let all_ones = self.builder.ins().iconst(types::I32, 0x00FF_FFFF);
        let limited_ly = self.builder.ins().select(is_neg, zero, all_ones);
        let lo = self.load_reg(lo_reg);
        let save_ly = self.builder.ins().select(no_limit, lo, limited_ly);
        (save_lx, save_ly)
    }

    /// Write an L-register pair (lx, ly) for pm_4x. The 3-bit `numreg` selects
    /// the destination pair.
    pub(super) fn write_l_reg(&mut self, numreg: u32, lx: Value, ly: Value) {
        match numreg {
            0 => {
                // A10: A1=lx, A0=ly (A2 preserved)
                self.write_acc_lo_mid(Accumulator::A, lx, ly);
            }
            1 => {
                // B10: B1=lx, B0=ly (B2 preserved)
                self.write_acc_lo_mid(Accumulator::B, lx, ly);
            }
            2 => {
                // X: X1=lx, X0=ly
                self.store_reg(reg::X1, lx);
                self.store_reg(reg::X0, ly);
            }
            3 => {
                // Y: Y1=lx, Y0=ly
                self.store_reg(reg::Y1, lx);
                self.store_reg(reg::Y0, ly);
            }
            4 | 5 => {
                let acc = if numreg == 4 {
                    Accumulator::A
                } else {
                    Accumulator::B
                };
                let ext = self.sign_ext_bit23(lx);
                let packed = self.pack_acc(ext, lx, ly);
                self.store_acc(acc, packed);
            }
            6 | 7 => {
                // 6=AB, 7=BA: first acc gets (sign_ext(lx), lx, 0), second gets (sign_ext(ly), ly, 0)
                let (first, second) = if numreg == 6 {
                    (Accumulator::A, Accumulator::B)
                } else {
                    (Accumulator::B, Accumulator::A)
                };
                let first_packed = self.val24_to_acc56(lx);
                self.store_acc(first, first_packed);
                let second_packed = self.val24_to_acc56(ly);
                self.store_acc(second, second_packed);
            }
            _ => unreachable!(),
        }
    }

    /// Sign-extend bit 23 of a 24-bit value to produce the 8-bit A2/B2 extension.
    /// Returns 0xFF if bit 23 is set, 0x00 otherwise.
    pub(super) fn sign_ext_bit23(&mut self, val: Value) -> Value {
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let sign = self.builder.ins().ushr(val, c23);
        let one = self.builder.ins().iconst(types::I32, 1);
        let sign = self.builder.ins().band(sign, one);
        let zero = self.builder.ins().iconst(types::I32, 0);
        let neg = self.builder.ins().isub(zero, sign);
        let mask = self.builder.ins().iconst(types::I32, 0xFF);
        self.builder.ins().band(neg, mask)
    }

    /// Sign-extend a 24-bit i32 value to full i64 (for multiplication).
    pub(super) fn sext24_to_i64(&mut self, val: Value) -> Value {
        let c8 = self.builder.ins().iconst(types::I32, 8);
        let shifted = self.builder.ins().ishl(val, c8);
        let sext32 = self.builder.ins().sshr(shifted, c8);
        self.builder.ins().sextend(types::I64, sext32)
    }

    pub(super) fn zext24_to_i64(&mut self, val: Value) -> Value {
        let mask = self.builder.ins().iconst(types::I32, 0x00FF_FFFF);
        let masked = self.builder.ins().band(val, mask);
        self.builder.ins().uextend(types::I64, masked)
    }

    /// Load X or Y 48-bit register pair as 56-bit accumulator value.
    /// hi_reg is X1/Y1 (bits 47:24), lo_reg is X0/Y0 (bits 23:0).
    pub(super) fn load_xy_as_acc56(&mut self, hi_reg: usize, lo_reg: usize) -> Value {
        let hi32 = self.load_reg(hi_reg);
        let lo32 = self.load_reg(lo_reg);
        let hi64 = self.builder.ins().uextend(types::I64, hi32);
        let lo64 = self.builder.ins().uextend(types::I64, lo32);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let mid = self.builder.ins().ishl(hi64, c24);
        let lo_mid = self.builder.ins().bor(mid, lo64);
        // Sign extend from bit 47 to bit 55
        let c16 = self.builder.ins().iconst(types::I32, 16);
        let shl = self.builder.ins().ishl(lo_mid, c16);
        let sext = self.builder.ins().sshr(shl, c16);
        self.mask56(sext)
    }
}
