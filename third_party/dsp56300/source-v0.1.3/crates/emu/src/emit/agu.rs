use super::*;

impl<'a> Emitter<'a> {
    /// Calculate effective address from 6-bit EA mode field.
    /// Returns (address_value, is_immediate).
    /// Updates Rn registers as a side effect (for post-increment etc.)
    pub(super) fn emit_calc_ea(&mut self, ea_mode: u32) -> (Value, bool) {
        self.emit_calc_ea_ext(ea_mode, 0)
    }

    pub(super) fn emit_calc_ea_ext(&mut self, ea_mode: u32, next_word: u32) -> (Value, bool) {
        let mode = (ea_mode >> 3) & 0x7;
        let numreg = (ea_mode & 0x7) as usize;

        match mode {
            0 => {
                // (Rn)-Nn : return Rn, then update Rn with -Nn modifier
                let rn = self.load_reg(reg::R0 + numreg);
                let nn = self.load_nn_signed(numreg);
                let neg_nn = self.builder.ins().ineg(nn);
                self.emit_update_rn(numreg, neg_nn);
                (rn, false)
            }
            1 => {
                // (Rn)+Nn : return Rn, then update Rn with +Nn modifier
                let rn = self.load_reg(reg::R0 + numreg);
                let nn = self.load_nn_signed(numreg);
                self.emit_update_rn(numreg, nn);
                (rn, false)
            }
            2 => {
                // (Rn)- : return Rn, then update Rn with -1 modifier
                let rn = self.load_reg(reg::R0 + numreg);
                let neg_one = self.builder.ins().iconst(types::I32, -1i32 as i64);
                self.emit_update_rn(numreg, neg_one);
                (rn, false)
            }
            3 => {
                // (Rn)+ : return Rn, then update Rn with +1 modifier
                let rn = self.load_reg(reg::R0 + numreg);
                let one = self.builder.ins().iconst(types::I32, 1);
                self.emit_update_rn(numreg, one);
                (rn, false)
            }
            4 => {
                // (Rn) : return Rn, no update
                let rn = self.load_reg(reg::R0 + numreg);
                (rn, false)
            }
            5 => {
                // (Rn+Nn) : Compute effective address using update_rn logic,
                // but restore Rn afterwards (transient address, no side-effect).
                self.pending_cycles += 1;
                let rn = self.load_reg(reg::R0 + numreg);
                let nn = self.load_nn_signed(numreg);
                // Update Rn to compute the modulo-aware address
                self.emit_update_rn(numreg, nn);
                let addr = self.load_reg(reg::R0 + numreg);
                // Restore original Rn
                self.store_reg(reg::R0 + numreg, rn);
                (addr, false)
            }
            6 => {
                // Absolute (RRR=0) or immediate (RRR=4): value from next program word
                self.pending_cycles += 1;
                let val = self.builder.ins().iconst(types::I32, next_word as i64);
                (val, numreg != 0) // RRR!=0 means immediate value
            }
            7 => {
                // -(Rn) : update Rn with -1 modifier, then return Rn
                self.pending_cycles += 1;
                let neg_one = self.builder.ins().iconst(types::I32, -1i32 as i64);
                self.emit_update_rn(numreg, neg_one);
                let new_rn = self.load_reg(reg::R0 + numreg);
                (new_rn, false)
            }
            _ => unreachable!(),
        }
    }

    /// Load N register and sign-extend from the configured address-register
    /// width to i32.
    ///
    /// Generic DSP56300 address registers are 24-bit.  MCPX implements its
    /// R/N/M register bank at 16 bits, matching Xemu's MCPX interpreter and
    /// the hardware register mask table.  In either profile the stored u32
    /// value must be sign-extended at that profile's width before it is used
    /// as an address modifier.
    fn load_nn_signed(&mut self, numreg: usize) -> Value {
        let nn = self.load_reg(reg::N0 + numreg);
        let shift = 32 - self.address_register_mask.count_ones();
        let shift_value = self.builder.ins().iconst(types::I32, shift as i64);
        let shifted = self.builder.ins().ishl(nn, shift_value);
        self.builder.ins().sshr(shifted, shift_value)
    }

    /// Emit address register update with modulo/bit-reverse support.
    ///
    /// Fast path (M[numreg] == $FFFFFF): inline linear `Rn += modifier`.
    /// Slow path: call `jit_update_rn` helper for modulo/bit-reverse.
    pub(super) fn emit_update_rn(&mut self, numreg: usize, modifier: Value) {
        let mn = self.load_reg(reg::M0 + numreg);
        let linear_mode = self
            .builder
            .ins()
            .iconst(types::I32, self.address_register_mask as i64);
        let is_linear = self.builder.ins().icmp(IntCC::Equal, mn, linear_mode);

        let linear_blk = self.builder.create_block();
        let nonlinear_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        // Flush R/M/N[numreg] to memory before the branch so the nonlinear
        // C helper sees current values.
        let r_idx = reg::R0 + numreg;
        self.flush_reg(r_idx);
        self.flush_reg(reg::M0 + numreg);
        self.flush_reg(reg::N0 + numreg);
        // Clear Rn's dirty flag after flushing so the linear arm's store_reg
        // is detected as "newly dirty" by end_conditional_arm.  Without this,
        // Rn stays dirty from before the branch, neither arm marks it as
        // modified, merge_conditional never invalidates it, and subsequent
        // code keeps using the stale promoted value -- overwriting whatever
        // jit_update_rn wrote in the nonlinear path.
        self.promoted.dirty[r_idx] = false;

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(is_linear, linear_blk, &[], nonlinear_blk, &[]);

        // Fast path: linear addressing
        self.builder.switch_to_block(linear_blk);
        self.builder.seal_block(linear_blk);
        let rn = self.load_reg(reg::R0 + numreg);
        let new_rn = self.builder.ins().iadd(rn, modifier);
        let r_mask = self
            .builder
            .ins()
            .iconst(types::I32, self.address_register_mask as i64);
        let new_rn = self.builder.ins().band(new_rn, r_mask);
        self.store_reg(reg::R0 + numreg, new_rn);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        // Slow path: call jit_update_rn(state_ptr, numreg, modifier)
        // jit_update_rn reads R/M/N[numreg] from memory (flushed above)
        // and writes R[numreg] back.
        self.builder.switch_to_block(nonlinear_blk);
        self.builder.seal_block(nonlinear_blk);
        let fn_ptr = self
            .builder
            .ins()
            .iconst(self.ptr_ty, jit_update_rn as *const () as usize as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty)); // *mut DspState
        sig.params.push(AbiParam::new(types::I32)); // numreg
        sig.params.push(AbiParam::new(types::I32)); // profile-width modifier sign-extended to i32
        let sig_ref = self.builder.import_signature(sig);
        let numreg_val = self.builder.ins().iconst(types::I32, numreg as i64);
        self.builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, numreg_val, modifier]);
        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }
}
