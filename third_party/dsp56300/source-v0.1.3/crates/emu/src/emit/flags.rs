use super::*;

impl<'a> Emitter<'a> {
    /// Flush any pending lazy flag computation into SR. Called automatically
    /// when SR is read via `load_reg(SR)`. The `take()` ensures re-entrant
    /// calls (from the flag computation code itself loading SR) are safe.
    fn set_pending(&mut self, flags: PendingFlags) {
        self.pending_flags = Some(flags);
    }

    pub(super) fn flush_pending_flags(&mut self) {
        let Some(flags) = self.pending_flags.take() else {
            return;
        };
        match flags {
            PendingFlags::AluAddSub {
                result56,
                source,
                dest,
                result_raw,
                is_sub,
            } => {
                self.update_nz_now(result56);
                self.update_vcl(source, dest, result_raw, is_sub);
                self.emit_sm_vl_deferred();
            }
            PendingFlags::NzClearV { result56 } => {
                self.update_nz_now(result56);
                self.clear_v_flag();
            }
            PendingFlags::NzOnly { result56 } => {
                self.update_nz_now(result56);
            }
            PendingFlags::NzClearVSm { result56 } => {
                self.update_nz_now(result56);
                self.clear_v_flag();
                self.emit_sm_vl_deferred();
            }
            PendingFlags::MacVlSm {
                result56,
                product,
                acc,
            } => {
                self.update_nz_now(result56);
                self.mac_set_vl(product, acc, result56);
                self.emit_sm_vl_deferred();
            }
            PendingFlags::NzVlSm { result56, overflow } => {
                self.update_nz_now(result56);
                self.set_vl_overflow(overflow);
                self.emit_sm_vl_deferred();
            }
            PendingFlags::NzSm { result56 } => {
                self.update_nz_now(result56);
                self.emit_sm_vl_deferred();
            }
            PendingFlags::NzVclSub {
                result56,
                source,
                dest,
                result_raw,
            } => {
                self.update_nz_now(result56);
                self.update_vcl_sub(source, dest, result_raw);
            }
            PendingFlags::AddlSubl {
                result56,
                source,
                dest_shifted,
                result_raw,
                is_sub,
                asl_carry,
                asl_v,
            } => {
                self.update_nz_now(result56);
                self.update_vcl(source, dest_shifted, result_raw, is_sub);
                self.xor_c_or_vl(asl_carry, asl_v);
                self.emit_sm_vl_deferred();
            }
            PendingFlags::DmacVl {
                result56,
                product,
                acc,
            } => {
                self.update_nz_now(result56);
                self.mac_set_vl(product, acc, result56);
            }
            PendingFlags::Shift24 {
                carry,
                n_val,
                result,
            } => {
                self.update_shift24_flags(carry, n_val, result);
            }
            PendingFlags::Logical { result24 } => {
                self.update_nzv_logical(result24);
            }
        }
    }

    /// Set pending EUNZ + VCL flags for a standard add/sub ALU operation.
    /// Replaces any previously pending flags.
    pub(super) fn set_flags_addsub(
        &mut self,
        result56: Value,
        source: Value,
        dest: Value,
        result_raw: Value,
        is_sub: bool,
    ) {
        self.set_pending(PendingFlags::AluAddSub {
            result56,
            source,
            dest,
            result_raw,
            is_sub,
        });
    }

    /// Set pending EUNZ flags with V cleared (TST pattern).
    pub(super) fn set_flags_nz_clear_v(&mut self, result56: Value) {
        self.set_pending(PendingFlags::NzClearV { result56 });
    }

    pub(super) fn set_flags_nz(&mut self, result56: Value) {
        self.set_pending(PendingFlags::NzOnly { result56 });
    }

    pub(super) fn set_flags_nz_clear_v_sm(&mut self, result56: Value) {
        self.set_pending(PendingFlags::NzClearVSm { result56 });
    }

    pub(super) fn set_flags_mac_vl_sm(&mut self, result56: Value, product: Value, acc: Value) {
        self.set_pending(PendingFlags::MacVlSm {
            result56,
            product,
            acc,
        });
    }

    pub(super) fn set_flags_nz_vl_sm(&mut self, result56: Value, overflow: Value) {
        self.set_pending(PendingFlags::NzVlSm { result56, overflow });
    }

    pub(super) fn set_flags_nz_sm(&mut self, result56: Value) {
        self.set_pending(PendingFlags::NzSm { result56 });
    }

    pub(super) fn set_flags_nz_vcl_sub(
        &mut self,
        result56: Value,
        source: Value,
        dest: Value,
        result_raw: Value,
    ) {
        self.set_pending(PendingFlags::NzVclSub {
            result56,
            source,
            dest,
            result_raw,
        });
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn set_flags_addl_subl(
        &mut self,
        result56: Value,
        source: Value,
        dest_shifted: Value,
        result_raw: Value,
        is_sub: bool,
        asl_carry: Value,
        asl_v: Value,
    ) {
        self.set_pending(PendingFlags::AddlSubl {
            result56,
            source,
            dest_shifted,
            result_raw,
            is_sub,
            asl_carry,
            asl_v,
        });
    }

    pub(super) fn set_flags_dmac_vl(&mut self, result56: Value, product: Value, acc: Value) {
        self.set_pending(PendingFlags::DmacVl {
            result56,
            product,
            acc,
        });
    }

    pub(super) fn set_flags_shift24(&mut self, carry: Value, n_val: Option<Value>, result: Value) {
        self.set_pending(PendingFlags::Shift24 {
            carry,
            n_val,
            result,
        });
    }

    pub(super) fn set_flags_logical(&mut self, result24: Value) {
        self.set_pending(PendingFlags::Logical { result24 });
    }

    /// Load SR and clear the given flag bits. Returns the cleared SR value.
    /// `flags` is a bitmask, e.g. `(1u32 << sr::N) | (1u32 << sr::Z)`.
    pub(super) fn clear_sr_flags(&mut self, flags: u32) -> Value {
        let sr = self.load_reg(reg::SR);
        let mask = self.builder.ins().iconst(types::I32, !flags as i64);
        self.builder.ins().band(sr, mask)
    }

    /// Lazy EUNZ update: stores result for deferred computation.
    pub(super) fn update_nz(&mut self, acc_val: Value) {
        self.set_flags_nz(acc_val);
    }

    /// Immediate EUNZ update via extern helper. Replaces 77 IR instructions
    /// with a single native function call, dramatically reducing Cranelift
    /// compilation time while keeping the same runtime semantics.
    fn update_nz_now(&mut self, acc_val: Value) {
        use crate::core::jit_update_nz;
        self.emit_call_sr_helper_i64(jit_update_nz as *const () as usize, acc_val);
    }

    /// Update CCR flags for logical operations (AND, OR, EOR, NOT).
    ///
    /// Logical ops only clear N/Z/V and set N from bit 23 of the 24-bit
    /// result, Z if result == 0. E, U, C, L are left untouched.
    pub(super) fn update_nzv_logical(&mut self, result24: Value) {
        // Clear N, Z, V
        let sr_new = self.clear_sr_flags((1u32 << sr::N) | (1u32 << sr::Z) | (1u32 << sr::V));

        // N = (result >> 23) & 1
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let n_raw = self.builder.ins().ushr(result24, c23);
        let one = self.builder.ins().iconst(types::I32, 1);
        let n_raw = self.builder.ins().band(n_raw, one);
        let n_bit = self.shift_to_bit(n_raw, sr::N);
        let sr_new = self.builder.ins().bor(sr_new, n_bit);

        // Z = (result == 0)
        let zero = self.builder.ins().iconst(types::I32, 0);
        let is_zero = self.builder.ins().icmp(IntCC::Equal, result24, zero);
        let z_val = self.builder.ins().uextend(types::I32, is_zero);
        let z_bit = self.shift_to_bit(z_val, sr::Z);
        let sr_new = self.builder.ins().bor(sr_new, z_bit);

        self.store_reg(reg::SR, sr_new);
    }

    /// Shift `overflow` (0/1 i32) to V and L bit positions and OR into `sr`.
    pub(super) fn or_vl(&mut self, sr: Value, overflow: Value) -> Value {
        let v_bit = self.shift_to_bit(overflow, sr::V);
        let l_bit = self.shift_to_bit(overflow, sr::L);
        let sr = self.builder.ins().bor(sr, v_bit);
        self.builder.ins().bor(sr, l_bit)
    }

    /// Update V and L from an overflow bit. Clears V, sets V and L (sticky).
    /// Does NOT modify C.
    pub(super) fn set_vl_overflow(&mut self, overflow: Value) {
        let sr_new = self.clear_sr_flags(1u32 << sr::V);
        let sr_new = self.or_vl(sr_new, overflow);
        self.store_reg(reg::SR, sr_new);
    }

    /// Arithmetic Saturation Mode via extern helper. Returns bits 55:0 as the
    /// (possibly clamped) value. Bit 56 of the helper's return encodes needs_sat.
    pub(super) fn emit_saturate_sm(&mut self, result: Value) -> Value {
        use crate::core::jit_saturate_sm;

        // Flush SR so the helper can read SM. Don't invalidate — SM doesn't change.
        self.flush_reg(reg::SR);
        self.promoted.dirty[reg::SR] = false;

        let fn_ptr = self
            .builder
            .ins()
            .iconst(self.ptr_ty, jit_saturate_sm as *const () as usize as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty));
        sig.params.push(AbiParam::new(types::I64));
        sig.returns.push(AbiParam::new(types::I64));
        let sig_ref = self.builder.import_signature(sig);
        let call = self
            .builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, result]);
        let packed = self.builder.inst_results(call)[0];

        // Extract needs_sat (bit 56) for deferred V/L update
        let needs_sat = self.extract_bit_i64(packed, 56);
        self.builder.def_var(self.sm_needs_sat_var, needs_sat);

        // Mask to 56 bits
        self.mask56(packed)
    }

    /// Apply deferred V/L flags from a previous `emit_saturate_sm` call.
    /// Call AFTER the instruction's normal flag computation (update_nz, update_vcl, etc.).
    pub(super) fn emit_sm_vl_deferred(&mut self) {
        let needs_sat = self.builder.use_var(self.sm_needs_sat_var);
        let sr = self.load_reg(reg::SR);
        let sr = self.or_vl(sr, needs_sat);
        self.store_reg(reg::SR, sr);
    }

    /// Compute V from MAC accumulation overflow (C unchanged).
    /// Uses standard signed overflow formula: both input signs agree but differ from result.
    pub(super) fn mac_set_vl(&mut self, product: Value, acc: Value, result: Value) {
        let sign_s = self.extract_bit_i64(product, 55);
        let sign_d = self.extract_bit_i64(acc, 55);
        let sign_r = self.extract_bit_i64(result, 55);
        let sr_xr = self.builder.ins().bxor(sign_s, sign_r);
        let dr_xr = self.builder.ins().bxor(sign_d, sign_r);
        let overflow = self.builder.ins().band(sr_xr, dr_xr);
        self.set_vl_overflow(overflow);
    }

    /// Update SR flags for 24-bit shift/rotate: clear C|N|Z|V, then set C from
    /// `carry` (bit-0 value), optionally set N from `n_val` (bit-0 value), set Z
    /// from `result == 0`. V is always cleared.
    pub(super) fn update_shift24_flags(
        &mut self,
        carry: Value,
        n_val: Option<Value>,
        result: Value,
    ) {
        let sr_new = self
            .clear_sr_flags((1u32 << sr::C) | (1u32 << sr::N) | (1u32 << sr::Z) | (1u32 << sr::V));
        // C = carry (already in bit 0 position, same as sr::C)
        let sr_new = self.builder.ins().bor(sr_new, carry);
        // N (if provided)
        let sr_new = if let Some(n) = n_val {
            let n_bit = self.shift_to_bit(n, sr::N);
            self.builder.ins().bor(sr_new, n_bit)
        } else {
            sr_new
        };
        // Z = (result == 0)
        let zero = self.builder.ins().iconst(types::I32, 0);
        let is_zero = self.builder.ins().icmp(IntCC::Equal, result, zero);
        let z_val = self.builder.ins().uextend(types::I32, is_zero);
        let z_bit = self.shift_to_bit(z_val, sr::Z);
        let sr_new = self.builder.ins().bor(sr_new, z_bit);
        self.store_reg(reg::SR, sr_new);
    }

    /// Update V, C, L flags for a 56-bit add or subtract.
    ///
    /// `source` and `dest` are the original 56-bit values.
    /// `result` is the unmasked i64 result. `is_sub` selects the overflow formula.
    pub(super) fn update_vcl(&mut self, source: Value, dest: Value, result: Value, is_sub: bool) {
        // Carry = bit 56 of the unmasked result
        let carry = self.extract_bit_i64(result, 56);

        // Extract sign bits (bit 55)
        let sign_s = self.extract_bit_i64(source, 55);
        let sign_d = self.extract_bit_i64(dest, 55);
        let result56 = self.mask56(result);
        let sign_r = self.extract_bit_i64(result56, 55);

        // Overflow formula differs for add vs sub
        let overflow = if is_sub {
            // Sub: (sign_s ^ sign_d) & (sign_r ^ sign_d)
            let sd = self.builder.ins().bxor(sign_s, sign_d);
            let rd = self.builder.ins().bxor(sign_r, sign_d);
            self.builder.ins().band(sd, rd)
        } else {
            // Add: (sign_s ^ sign_r) & (sign_d ^ sign_r)
            let sr = self.builder.ins().bxor(sign_s, sign_r);
            let dr = self.builder.ins().bxor(sign_d, sign_r);
            self.builder.ins().band(sr, dr)
        };

        // Update SR: clear V|C, set C (bit 0), V and L (sticky)
        let sr_new = self.clear_sr_flags((1u32 << sr::V) | (1u32 << sr::C));
        let sr_new = self.builder.ins().bor(sr_new, carry); // C is bit 0
        let sr_new = self.or_vl(sr_new, overflow);
        self.store_reg(reg::SR, sr_new);
    }

    /// Set the carry flag in SR.
    pub(super) fn set_carry(&mut self, c_bit: Value) {
        let sr_c = self.clear_sr_flags(1u32 << sr::C);
        // c_bit should be 0 or 1; C is bit 0 so mask only
        let one = self.builder.ins().iconst(types::I32, 1);
        let c_masked = self.builder.ins().band(c_bit, one);
        let sr_new = self.builder.ins().bor(sr_c, c_masked); // C is bit 0
        self.store_reg(reg::SR, sr_new);
    }

    /// Clear the V (overflow) flag in SR.
    pub(super) fn clear_v_flag(&mut self) {
        let sr = self.clear_sr_flags(1u32 << sr::V);
        self.store_reg(reg::SR, sr);
    }

    pub(super) fn update_vcl_sub(&mut self, source: Value, dest: Value, result: Value) {
        self.update_vcl(source, dest, result, true);
    }
}
