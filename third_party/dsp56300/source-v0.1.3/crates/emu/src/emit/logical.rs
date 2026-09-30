use super::*;

impl<'a> Emitter<'a> {
    /// ANDI/ORI #xx,D - dest selects target per EE field:
    /// 0=MR (SR bits 15:8), 1=CCR (SR bits 7:0), 2=COM (OMR bits 7:0), 3=EOM (OMR bits 15:8).
    pub(super) fn emit_andi(&mut self, imm: u8, dest: u8) {
        self.set_inst_len(1);
        self.set_cycles(3);
        match dest {
            0 => {
                // AND with MR (SR bits 15:8)
                let sr_val = self.load_reg(reg::SR);
                let m = self
                    .builder
                    .ins()
                    .iconst(types::I32, ((imm as u32) << 8 | 0xFF00FF) as i64);
                let r = self.builder.ins().band(sr_val, m);
                self.store_reg(reg::SR, r);
            }
            1 => {
                // AND with CCR (SR bits 7:0)
                let sr_val = self.load_reg(reg::SR);
                let m = self
                    .builder
                    .ins()
                    .iconst(types::I32, (imm as i64) | 0xFFFF00);
                let r = self.builder.ins().band(sr_val, m);
                self.store_reg(reg::SR, r);
            }
            2 => {
                // AND with COM (low 8 bits of OMR), preserve upper bits
                let omr = self.load_reg(reg::OMR);
                let m = self
                    .builder
                    .ins()
                    .iconst(types::I32, (imm as u32 | 0xFFFF00) as i64);
                let r = self.builder.ins().band(omr, m);
                self.store_reg(reg::OMR, r);
            }
            3 => {
                // AND with EOM (OMR bits 15:8), preserve other bits
                let omr = self.load_reg(reg::OMR);
                let m = self
                    .builder
                    .ins()
                    .iconst(types::I32, ((imm as u32) << 8 | 0xFF00FF) as i64);
                let r = self.builder.ins().band(omr, m);
                self.store_reg(reg::OMR, r);
            }
            _ => {}
        }
    }

    /// See [`emit_andi`] for EE field documentation.
    pub(super) fn emit_ori(&mut self, imm: u8, dest: u8) {
        self.set_inst_len(1);
        self.set_cycles(3);
        match dest {
            0 => {
                // OR with MR (SR bits 15:8)
                let sr_val = self.load_reg(reg::SR);
                let bits = self
                    .builder
                    .ins()
                    .iconst(types::I32, ((imm as u32) << 8) as i64);
                let r = self.builder.ins().bor(sr_val, bits);
                self.store_reg(reg::SR, r);
            }
            1 => {
                // OR with CCR (SR bits 7:0)
                let sr_val = self.load_reg(reg::SR);
                let bits = self.builder.ins().iconst(types::I32, imm as i64);
                let r = self.builder.ins().bor(sr_val, bits);
                self.store_reg(reg::SR, r);
            }
            2 => {
                // OR with COM (OMR bits 7:0)
                let omr = self.load_reg(reg::OMR);
                let bits = self.builder.ins().iconst(types::I32, imm as i64);
                let r = self.builder.ins().bor(omr, bits);
                self.store_reg(reg::OMR, r);
            }
            3 => {
                // OR with EOM (OMR bits 15:8)
                let omr = self.load_reg(reg::OMR);
                let bits = self
                    .builder
                    .ins()
                    .iconst(types::I32, ((imm as u32) << 8) as i64);
                let r = self.builder.ins().bor(omr, bits);
                self.store_reg(reg::OMR, r);
            }
            _ => {}
        }
    }

    pub(super) fn emit_logical_op_acc(
        &mut self,
        d: Accumulator,
        src_val: i64,
        inst_len: u32,
        op: LogicalOp,
    ) {
        self.set_inst_len(inst_len);
        self.set_cycles(inst_len as i64);
        let (_, r1, _) = Self::acc_regs(d);
        let dst = self.load_reg(r1);
        let src = self.builder.ins().iconst(types::I32, src_val);
        let result = match op {
            LogicalOp::And => self.builder.ins().band(dst, src),
            LogicalOp::Or => self.builder.ins().bor(dst, src),
            LogicalOp::Eor => self.builder.ins().bxor(dst, src),
        };
        self.store_reg(r1, result);
        self.set_flags_logical(result);
    }

    pub(super) fn emit_alu_not(&mut self, d: Accumulator) {
        // NOT operates on A1/B1 only
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);
        let mask = self.builder.ins().iconst(types::I32, 0x00FFFFFF);
        let notted = self.builder.ins().bxor(val, mask);
        self.store_reg(r1, notted);
        self.set_flags_logical(notted);
    }

    pub(super) fn emit_alu_and_reg(&mut self, src_reg: usize, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let src = self.load_reg(src_reg);
        let dst = self.load_reg(r1);
        let result = self.builder.ins().band(src, dst);
        let result = self.mask24(result);
        self.store_reg(r1, result);
        self.set_flags_logical(result);
    }

    pub(super) fn emit_alu_or_reg(&mut self, src_reg: usize, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let src = self.load_reg(src_reg);
        let dst = self.load_reg(r1);
        let result = self.builder.ins().bor(src, dst);
        let result = self.mask24(result);
        self.store_reg(r1, result);
        self.set_flags_logical(result);
    }

    pub(super) fn emit_alu_eor_reg(&mut self, src_reg: usize, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let src = self.load_reg(src_reg);
        let dst = self.load_reg(r1);
        let result = self.builder.ins().bxor(src, dst);
        let result = self.mask24(result);
        self.store_reg(r1, result);
        self.set_flags_logical(result);
    }

    /// LSR: Logical shift right A1/B1 by 1. C=old bit 0, N=0, Z=(==0), V=0.
    pub(super) fn emit_alu_lsr(&mut self, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);
        let one = self.builder.ins().iconst(types::I32, 1);
        let carry = self.builder.ins().band(val, one);
        let result = self.builder.ins().ushr(val, one);
        self.store_reg(r1, result);
        self.set_flags_shift24(carry, None, result);
    }

    /// LSL: Logical shift left A1/B1 by 1. C=old bit 23, N=new bit 23, Z=(==0), V=0.
    pub(super) fn emit_alu_lsl(&mut self, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let one = self.builder.ins().iconst(types::I32, 1);
        let carry = self.builder.ins().ushr(val, c23);
        let carry = self.builder.ins().band(carry, one);
        let shifted = self.builder.ins().ishl(val, one);
        let result = self.mask24(shifted);
        self.store_reg(r1, result);
        let n_raw = self.builder.ins().ushr(result, c23);
        let n_raw = self.builder.ins().band(n_raw, one);
        self.set_flags_shift24(carry, Some(n_raw), result);
    }

    /// ROR: Rotate right A1/B1 by 1. Old C -> bit 23, old bit 0 -> C.
    /// N = bit 23 of result (= old carry). V=0.
    pub(super) fn emit_alu_ror(&mut self, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);
        let one = self.builder.ins().iconst(types::I32, 1);
        // New carry = old bit 0 (bit shifted out right)
        let new_carry = self.builder.ins().band(val, one);
        // Old carry from SR (bit 0)
        let sr_val = self.load_reg(reg::SR);
        let old_carry = self.builder.ins().band(sr_val, one);
        // Shift right by 1, put old carry into bit 23
        let shifted = self.builder.ins().ushr(val, one);
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let high_bit = self.builder.ins().ishl(old_carry, c23);
        let result = self.builder.ins().bor(shifted, high_bit);
        self.store_reg(r1, result);
        // N = bit 23 of result = old_carry (not new_carry)
        self.set_flags_shift24(new_carry, Some(old_carry), result);
    }

    /// ROL: Rotate left A1/B1 by 1 through carry. Old C -> bit 0, old bit 23 -> C.
    pub(super) fn emit_alu_rol(&mut self, d: Accumulator) {
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);
        let one = self.builder.ins().iconst(types::I32, 1);
        let c23 = self.builder.ins().iconst(types::I32, 23);
        // New carry = old bit 23
        let new_carry = self.builder.ins().ushr(val, c23);
        let new_carry = self.builder.ins().band(new_carry, one);
        // Old carry from SR (bit 0)
        let sr_val = self.load_reg(reg::SR);
        let old_carry = self.builder.ins().band(sr_val, one);
        // Shift left by 1, put old carry into bit 0
        let shifted = self.builder.ins().ishl(val, one);
        let result = self.builder.ins().bor(shifted, old_carry);
        let result = self.mask24(result);
        self.store_reg(r1, result);
        let n_raw = self.builder.ins().ushr(result, c23);
        let n_raw = self.builder.ins().band(n_raw, one);
        self.set_flags_shift24(new_carry, Some(n_raw), result);
    }

    pub(super) fn emit_lsl_imm(&mut self, shift: u8, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let ii = shift as u32;
        let (_, r1, _) = Self::acc_regs(d);

        if ii == 0 {
            // Per DSP56300FM p.13-93: C cleared, N/Z/V still updated per their definitions
            let sr = self.clear_sr_flags(1u32 << sr::C);
            self.store_reg(reg::SR, sr);
            let val = self.load_reg(r1);
            self.set_flags_logical(val);
            return;
        }

        let val = self.load_reg(r1);
        // Carry = bit (24 - ii) before shift (last bit shifted out)
        let carry_bit = 24u32.saturating_sub(ii);
        self.update_carry_from_bit(val, carry_bit);

        let shift = self.builder.ins().iconst(types::I32, ii as i64);
        let shifted = self.builder.ins().ishl(val, shift);
        let result = self.mask24(shifted);
        self.store_reg(r1, result);

        self.set_flags_logical(result);
    }

    pub(super) fn emit_lsr_imm(&mut self, shift: u8, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let ii = shift as u32;
        let (_, r1, _) = Self::acc_regs(d);

        if ii == 0 {
            // Per DSP56300FM p.13-96: C cleared, N/Z/V still updated per their definitions
            let sr = self.clear_sr_flags(1u32 << sr::C);
            self.store_reg(reg::SR, sr);
            let val = self.load_reg(r1);
            self.set_flags_logical(val);
            return;
        }

        let val = self.load_reg(r1);
        // Carry = bit (ii - 1) before shift (last bit shifted out)
        let carry_bit = ii.saturating_sub(1);
        self.update_carry_from_bit(val, carry_bit);

        let shift = self.builder.ins().iconst(types::I32, ii as i64);
        let result = self.builder.ins().ushr(val, shift);
        self.store_reg(r1, result);

        self.set_flags_logical(result);
    }

    pub(super) fn emit_lsl_reg(&mut self, src_reg: usize, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);

        // Shift amount = bits 5-0 of control register S1
        let sreg_val = self.load_reg(src_reg);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let shift_i32 = self.builder.ins().band(sreg_val, mask6);

        // C = bit (24 - shift) before shift
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let carry_pos = self.builder.ins().isub(c24, shift_i32);
        let carry_raw = self.builder.ins().ushr(val, carry_pos);
        let one = self.builder.ins().iconst(types::I32, 1);
        let carry_bit = self.builder.ins().band(carry_raw, one);

        let shifted = self.builder.ins().ishl(val, shift_i32);
        let result = self.mask24(shifted);
        self.store_reg(r1, result);

        // Update C
        let sr = self.clear_sr_flags(1u32 << sr::C);
        let c_shifted = self.shift_to_bit(carry_bit, sr::C);
        let sr = self.builder.ins().bor(sr, c_shifted);
        self.store_reg(reg::SR, sr);

        self.set_flags_logical(result);
    }

    pub(super) fn emit_lsr_reg(&mut self, src_reg: usize, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let (_, r1, _) = Self::acc_regs(d);
        let val = self.load_reg(r1);

        // Shift amount = bits 5-0 of control register S1
        let sreg_val = self.load_reg(src_reg);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let shift_i32 = self.builder.ins().band(sreg_val, mask6);

        // C = bit (shift - 1) before shift
        let one = self.builder.ins().iconst(types::I32, 1);
        let carry_pos = self.builder.ins().isub(shift_i32, one);
        let carry_raw = self.builder.ins().ushr(val, carry_pos);
        let carry_bit = self.builder.ins().band(carry_raw, one);

        let result = self.builder.ins().ushr(val, shift_i32);
        self.store_reg(r1, result);

        // Update C
        let sr = self.clear_sr_flags(1u32 << sr::C);
        let c_shifted = self.shift_to_bit(carry_bit, sr::C);
        let sr = self.builder.ins().bor(sr, c_shifted);
        self.store_reg(reg::SR, sr);

        self.set_flags_logical(result);
    }

    pub(super) fn emit_clb(&mut self, s: Accumulator, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let acc = self.load_acc(s);

        // Check sign bit (bit 55)
        let c55 = self.builder.ins().iconst(types::I32, 55);
        let sign = self.builder.ins().ushr(acc, c55);
        let one64 = self.builder.ins().iconst(types::I64, 1);
        let sign_bit = self.builder.ins().band(sign, one64);

        // If negative, invert all 56 bits so we count leading 0s in both cases
        let mask56 = self
            .builder
            .ins()
            .iconst(types::I64, 0x00FFFFFFFFFFFFFF_u64 as i64);
        let inverted = self.builder.ins().bxor(acc, mask56);
        let zero64 = self.builder.ins().iconst(types::I64, 0);
        let is_neg = self.builder.ins().icmp(IntCC::NotEqual, sign_bit, zero64);
        let val = self.builder.ins().select(is_neg, inverted, acc);

        // Shift left by 8 to align 56-bit value to 64-bit boundary
        let c8 = self.builder.ins().iconst(types::I32, 8);
        let shifted = self.builder.ins().ishl(val, c8);

        // Count leading zeros (= count of leading sign-matching bits in 56-bit value)
        let lz = self.builder.ins().clz(shifted);
        let lz32 = self.builder.ins().ireduce(types::I32, lz);

        // Clamp to 56 max (the shifted-in 8 zeros can inflate CLZ to 64)
        let max_lz = self.builder.ins().iconst(types::I32, 56);
        let clamped = self.builder.ins().umin(lz32, max_lz);

        // Per manual Note 1: if source is all zeros, result = 0
        let is_all_zero = self.builder.ins().icmp(IntCC::Equal, acc, zero64);

        // Result = 9 - count (after 8-bit left-shift alignment, 9 corresponds
        // to the original bit 55 boundary in the 56-bit value)
        let nine = self.builder.ins().iconst(types::I32, 9);
        let computed = self.builder.ins().isub(nine, clamped);
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let result = self.builder.ins().select(is_all_zero, zero32, computed);

        // Mask to 24 bits and store in destination accumulator
        let result24 = self.mask24(result);
        let packed = self.val24_to_acc56(result24);
        self.store_acc(d, packed);

        // Update N and Z flags based on A1 portion (bits 47:24)
        let a1 = self.extract_acc_mid(packed);
        self.set_flags_logical(a1);
    }

    /// EXTRACT / EXTRACTU: extract bit-field from accumulator.
    /// `is_reg`: true if S1 comes from a register (1-word), false if from next_word (2-word).
    /// `unsigned`: true for EXTRACTU (zero-fill), false for EXTRACT (sign-extend).
    pub(super) fn emit_extract(
        &mut self,
        s1_reg: Option<usize>,
        s2: Accumulator,
        d: Accumulator,
        next_word: u32,
        unsigned: bool,
    ) {
        self.set_inst_len(if s1_reg.is_some() { 1 } else { 2 });
        self.set_cycles(if s1_reg.is_some() { 1 } else { 2 });

        // Get control word: either from register S1 or from immediate next_word
        let control = if let Some(reg) = s1_reg {
            self.load_reg(reg)
        } else {
            self.builder.ins().iconst(types::I32, next_word as i64)
        };

        // Width = control[17:12] (6 bits), Offset = control[5:0] (6 bits)
        let c12 = self.builder.ins().iconst(types::I32, 12);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let width_raw = self.builder.ins().ushr(control, c12);
        let width = self.builder.ins().band(width_raw, mask6);
        let offset = self.builder.ins().band(control, mask6);

        // Load source accumulator S2 (56-bit packed i64)
        let src_acc = self.load_acc(s2);

        // Shift right by offset
        let shifted = self.builder.ins().ushr(src_acc, offset);

        // Create mask of width bits: (1 << width) - 1
        let one64 = self.builder.ins().iconst(types::I64, 1);
        let mask = self.builder.ins().ishl(one64, width);
        let mask = self.builder.ins().isub(mask, one64);

        let field = self.builder.ins().band(shifted, mask);

        let result = if unsigned {
            // EXTRACTU: zero-fill -- field is already right-aligned with zeros above
            field
        } else {
            // EXTRACT: sign-extend from bit (width-1)
            // Get sign bit of extracted field: bit (width-1)
            let one_i32 = self.builder.ins().iconst(types::I32, 1);
            let width_m1 = self.builder.ins().isub(width, one_i32);
            let sign_raw = self.builder.ins().ushr(field, width_m1);
            let one_i64 = self.builder.ins().iconst(types::I64, 1);
            let sign_bit = self.builder.ins().band(sign_raw, one_i64);

            // If sign bit is 1, fill bits above field with 1s
            let zero64 = self.builder.ins().iconst(types::I64, 0);
            let is_neg = self.builder.ins().icmp(IntCC::NotEqual, sign_bit, zero64);
            // Sign extension mask: ~((1 << width) - 1) & 0x00FFFFFFFFFFFFFF
            let mask56 = self
                .builder
                .ins()
                .iconst(types::I64, 0x00FFFFFFFFFFFFFF_u64 as i64);
            let ext_mask_raw = self.builder.ins().bxor(mask, mask56);
            let ext_masked = self.builder.ins().bor(field, ext_mask_raw);
            self.builder.ins().select(is_neg, ext_masked, field)
        };

        let result = self.mask56(result);
        self.store_acc(d, result);

        // Clear V and C, update E/U/N/Z per standard definition
        let sr_new = self.clear_sr_flags((1u32 << sr::V) | (1u32 << sr::C));
        self.store_reg(reg::SR, sr_new);
        self.update_nz(result);
    }

    /// INSERT: insert bit-field into accumulator.
    pub(super) fn emit_insert(
        &mut self,
        s1_reg: Option<usize>,
        s2_reg: usize,
        d: Accumulator,
        next_word: u32,
    ) {
        self.set_inst_len(if s1_reg.is_some() { 1 } else { 2 });
        self.set_cycles(if s1_reg.is_some() { 1 } else { 2 });

        // Get control word
        let control = if let Some(reg) = s1_reg {
            self.load_reg(reg)
        } else {
            self.builder.ins().iconst(types::I32, next_word as i64)
        };

        // Width = control[17:12], Offset = control[5:0]
        let c12 = self.builder.ins().iconst(types::I32, 12);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let width_raw = self.builder.ins().ushr(control, c12);
        let width = self.builder.ins().band(width_raw, mask6);
        let offset = self.builder.ins().band(control, mask6);

        let s2_val = self.load_reg(s2_reg);
        let s2_64 = self.builder.ins().uextend(types::I64, s2_val);

        let acc = self.load_acc(d);

        // Create mask of width bits: (1 << width) - 1
        let one64 = self.builder.ins().iconst(types::I64, 1);
        let field_mask = self.builder.ins().ishl(one64, width);
        let field_mask = self.builder.ins().isub(field_mask, one64);

        let field = self.builder.ins().band(s2_64, field_mask);

        // Shift field to offset position
        let field_shifted = self.builder.ins().ishl(field, offset);

        // Create positioned mask and clear that range in D
        let mask_shifted = self.builder.ins().ishl(field_mask, offset);
        let mask_inv = self.builder.ins().bnot(mask_shifted);
        let acc_cleared = self.builder.ins().band(acc, mask_inv);

        let result = self.builder.ins().bor(acc_cleared, field_shifted);
        let result = self.mask56(result);
        self.store_acc(d, result);

        // Clear V and C, update E/U/N/Z per standard definition
        let sr_new = self.clear_sr_flags((1u32 << sr::V) | (1u32 << sr::C));
        self.store_reg(reg::SR, sr_new);
        self.update_nz(result);
    }

    pub(super) fn emit_merge(&mut self, src_reg: usize, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let src = self.load_reg(src_reg);
        let mask12 = self.builder.ins().iconst(types::I32, 0xFFF);
        let src_lo12 = self.builder.ins().band(src, mask12);

        let acc = self.load_acc(d);

        // Extract old A1 = bits 47:24
        let old_a1 = self.extract_acc_mid(acc);

        // D[35:24] = A1[11:0]
        let old_a1_lo12 = self.builder.ins().band(old_a1, mask12);

        // new A1 = (S[11:0] << 12) | D_A1[11:0]
        let c12 = self.builder.ins().iconst(types::I32, 12);
        let src_shifted = self.builder.ins().ishl(src_lo12, c12);
        let new_a1 = self.builder.ins().bor(src_shifted, old_a1_lo12);

        // Replace A1 in packed accumulator: clear bits 47:24, set new A1
        let clear_mask = self
            .builder
            .ins()
            .iconst(types::I64, 0xFF_000000_FFFFFFu64 as i64);
        let acc_cleared = self.builder.ins().band(acc, clear_mask);
        let new_a1_64 = self.builder.ins().uextend(types::I64, new_a1);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let new_a1_shifted = self.builder.ins().ishl(new_a1_64, c24);
        let result = self.builder.ins().bor(acc_cleared, new_a1_shifted);
        self.store_acc(d, result);

        // Update flags: N = bit 23 of new_a1, Z = (new_a1 == 0), V = 0
        self.set_flags_logical(new_a1);
    }
}
