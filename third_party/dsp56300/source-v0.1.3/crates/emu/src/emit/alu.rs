use super::*;

impl<'a> Emitter<'a> {
    pub(super) fn emit_add_imm(&mut self, imm: u8, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let imm_val = self.builder.ins().iconst(types::I32, imm as i64);
        let src = self.val24_to_acc56(imm_val);
        self.emit_acc_addsub(src, d, false, true);
    }

    pub(super) fn emit_sub_imm(&mut self, imm: u8, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let imm_val = self.builder.ins().iconst(types::I32, imm as i64);
        let src = self.val24_to_acc56(imm_val);
        self.emit_acc_addsub(src, d, true, true);
    }

    pub(super) fn emit_inc(&mut self, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let one = self.builder.ins().iconst(types::I64, 1);
        self.emit_acc_addsub(one, d, false, true);
    }

    pub(super) fn emit_dec(&mut self, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let one = self.builder.ins().iconst(types::I64, 1);
        self.emit_acc_addsub(one, d, true, true);
    }

    pub(super) fn emit_add_long(&mut self, d: Accumulator, next_word: u32) {
        self.set_inst_len(2);
        self.set_cycles(2);
        let imm = self.builder.ins().iconst(types::I32, next_word as i64);
        let src = self.val24_to_acc56(imm);
        self.emit_acc_addsub(src, d, false, true);
    }

    pub(super) fn emit_sub_long(&mut self, d: Accumulator, next_word: u32) {
        self.set_inst_len(2);
        self.set_cycles(2);
        let imm = self.builder.ins().iconst(types::I32, next_word as i64);
        let src = self.val24_to_acc56(imm);
        self.emit_acc_addsub(src, d, true, true);
    }

    pub(super) fn emit_cmp_long(&mut self, d: Accumulator, next_word: u32) {
        self.set_inst_len(2);
        self.set_cycles(2);
        let imm = self.builder.ins().iconst(types::I32, next_word as i64);
        let src = self.val24_to_acc56(imm);
        self.emit_acc_addsub(src, d, true, false);
    }

    pub(super) fn emit_cmp_imm(&mut self, imm: u8, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let imm_val = self.builder.ins().iconst(types::I32, imm as i64);
        let src = self.val24_to_acc56(imm_val);
        self.emit_acc_addsub(src, d, true, false);
    }

    pub(super) fn emit_parallel_alu(&mut self, alu: &ParallelAlu) {
        use ParallelAlu::*;
        match *alu {
            Move | Undefined => {}

            TfrAcc { src, d } => self.emit_alu_tfr_acc(src, d),
            Addr { src, d } => self.emit_alu_addr(src, d),
            Tst { d } => self.emit_alu_tst(d),
            CmpAcc { src, d } => self.emit_alu_cmp_acc(src, d),
            Subr { src, d } => self.emit_alu_subr(src, d),
            CmpmAcc { src, d } => self.emit_alu_cmpm_acc(src, d),

            AddAcc { src, d } => self.emit_alu_add_acc(src, d),
            Rnd { d } => self.emit_alu_rnd(d),
            Addl { src, d } => self.emit_alu_addl(src, d),
            Clr { d } => self.emit_alu_clr(d),
            SubAcc { src, d } => self.emit_alu_sub_acc(src, d),
            Max => self.emit_alu_max(),
            Maxm => self.emit_alu_maxm(),
            Subl { src, d } => self.emit_alu_subl(src, d),
            Not { d } => self.emit_alu_not(d),

            AddXY { hi, lo, d } => self.emit_alu_add_xy(hi, lo, d),
            Adc { hi, lo, d } => self.emit_alu_adc(hi, lo, d),
            SubXY { hi, lo, d } => self.emit_alu_sub_xy(hi, lo, d),
            Sbc { hi, lo, d } => self.emit_alu_sbc(hi, lo, d),
            Asr { d } => self.emit_alu_asr(d),
            Lsr { d } => self.emit_alu_lsr(d),
            Abs { d } => self.emit_alu_abs(d),
            Ror { d } => self.emit_alu_ror(d),
            Asl { d } => self.emit_alu_asl(d),
            Lsl { d } => self.emit_alu_lsl(d),
            Neg { d } => self.emit_alu_neg(d),
            Rol { d } => self.emit_alu_rol(d),

            AddReg { src, d } => self.emit_alu_add_reg24(src, d),
            TfrReg { src, d } => self.emit_alu_tfr_reg24(src, d),
            Or { src, d } => self.emit_alu_or_reg(src, d),
            Eor { src, d } => self.emit_alu_eor_reg(src, d),
            SubReg { src, d } => self.emit_alu_sub_reg24(src, d),
            CmpReg { src, d } => self.emit_alu_cmp_reg24(src, d),
            And { src, d } => self.emit_alu_and_reg(src, d),
            CmpmReg { src, d } => self.emit_alu_cmpm_reg24(src, d),

            Mpy { negate, s1, s2, d } => self.emit_alu_mpy(s1, s2, d, negate),
            Mpyr { negate, s1, s2, d } => self.emit_alu_mpyr(s1, s2, d, negate),
            Mac { negate, s1, s2, d } => self.emit_alu_mac(s1, s2, d, negate),
            Macr { negate, s1, s2, d } => self.emit_alu_macr(s1, s2, d, negate),
        }
    }

    pub(super) fn emit_alu_clr(&mut self, d: Accumulator) {
        let zero = self.builder.ins().iconst(types::I64, 0);
        self.store_acc(d, zero);
        // Set Z=1, U=1; clear E, N, V
        let sr_c = self.clear_sr_flags((1u32 << sr::E) | (1u32 << sr::N) | (1u32 << sr::V));
        let set = self
            .builder
            .ins()
            .iconst(types::I32, ((1u32 << sr::U) | (1u32 << sr::Z)) as i64);
        let sr_new = self.builder.ins().bor(sr_c, set);
        self.store_reg(reg::SR, sr_new);
    }

    pub(super) fn emit_alu_tst(&mut self, d: Accumulator) {
        let acc = self.load_acc(d);
        let acc = self.mask56(acc);
        self.set_flags_nz_clear_v(acc);
    }

    pub(super) fn emit_alu_tfr_acc(&mut self, src: Accumulator, dst: Accumulator) {
        let acc = self.load_acc(src);
        self.store_acc(dst, acc);
    }

    pub(super) fn emit_alu_tfr_reg24(&mut self, src_reg: usize, d: Accumulator) {
        let val = self.load_reg(src_reg);
        let packed = self.val24_to_acc56(val);
        self.store_acc(d, packed);
    }

    pub(super) fn emit_alu_add_acc(&mut self, src: Accumulator, dst: Accumulator) {
        let s = self.load_acc(src);
        self.emit_acc_addsub(s, dst, false, true);
    }

    pub(super) fn emit_alu_sub_acc(&mut self, src: Accumulator, dst: Accumulator) {
        let s = self.load_acc(src);
        self.emit_acc_addsub(s, dst, true, true);
    }

    pub(super) fn emit_alu_cmp_acc(&mut self, src: Accumulator, dst: Accumulator) {
        let s = self.load_acc(src);
        self.emit_acc_addsub(s, dst, true, false);
    }

    pub(super) fn emit_alu_add_reg24(&mut self, src_reg: usize, d: Accumulator) {
        let src56 = self.load_reg24_as_acc56(src_reg);
        self.emit_acc_addsub(src56, d, false, true);
    }

    pub(super) fn emit_alu_sub_reg24(&mut self, src_reg: usize, d: Accumulator) {
        let src56 = self.load_reg24_as_acc56(src_reg);
        self.emit_acc_addsub(src56, d, true, true);
    }

    pub(super) fn emit_alu_cmp_reg24(&mut self, src_reg: usize, d: Accumulator) {
        let src56 = self.load_reg24_as_acc56(src_reg);
        self.emit_acc_addsub(src56, d, true, false);
    }

    pub(super) fn emit_alu_add_xy(&mut self, hi_reg: usize, lo_reg: usize, d: Accumulator) {
        let src56 = self.load_xy_as_acc56(hi_reg, lo_reg);
        self.emit_acc_addsub(src56, d, false, true);
    }

    pub(super) fn emit_alu_sub_xy(&mut self, hi_reg: usize, lo_reg: usize, d: Accumulator) {
        let src56 = self.load_xy_as_acc56(hi_reg, lo_reg);
        self.emit_acc_addsub(src56, d, true, true);
    }

    pub(super) fn emit_alu_neg(&mut self, d: Accumulator) {
        let acc = self.load_acc(d);
        let zero = self.builder.ins().iconst(types::I64, 0);
        let result = self.builder.ins().isub(zero, acc);
        let result56 = self.mask56(result);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(d, result56);
        // NEG: V set only if value is most negative (0x80_000000_000000)
        let most_neg = self
            .builder
            .ins()
            .iconst(types::I64, 0x80000000000000u64 as i64);
        let is_most_neg = self.builder.ins().icmp(IntCC::Equal, acc, most_neg);
        let overflow = self.builder.ins().uextend(types::I32, is_most_neg);
        self.set_flags_nz_vl_sm(result56, overflow);
    }

    pub(super) fn emit_alu_abs(&mut self, d: Accumulator) {
        let acc = self.load_acc(d);
        // Negate
        let zero64 = self.builder.ins().iconst(types::I64, 0);
        let negated = self.builder.ins().isub(zero64, acc);
        let negated56 = self.mask56(negated);
        // Check if original was negative (bit 55)
        let c55 = self.builder.ins().iconst(types::I32, 55);
        let sign = self.builder.ins().ushr(acc, c55);
        let sign32 = self.builder.ins().ireduce(types::I32, sign);
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let is_neg = self.builder.ins().icmp(IntCC::NotEqual, sign32, zero32);
        // Select: if negative, use negated; else use original
        let result = self.builder.ins().select(is_neg, negated56, acc);
        let result = self.emit_saturate_sm(result);
        self.store_acc(d, result);
        // ABS: V/L from subtraction overflow (when negative), C is NOT modified.
        let sign_r = self.extract_bit_i64(negated56, 55);
        let one = self.builder.ins().iconst(types::I32, 1);
        let sign_d = self.builder.ins().band(sign32, one);
        let raw_overflow = self.builder.ins().band(sign_d, sign_r);
        let overflow = self.builder.ins().select(is_neg, raw_overflow, zero32);
        self.set_flags_nz_vl_sm(result, overflow);
    }

    /// Sign-extend a 56-bit value packed in i64 to a proper 64-bit signed value.
    fn sext56_to_i64(&mut self, val: Value) -> Value {
        let c8 = self.builder.ins().iconst(types::I32, 8);
        let shifted = self.builder.ins().ishl(val, c8);
        self.builder.ins().sshr(shifted, c8)
    }

    /// Arithmetic shift right by 1 for a 56-bit accumulator, preserving bit 55 (sign).
    fn asr_one_56bit(&mut self, val: Value) -> Value {
        let c1 = self.builder.ins().iconst(types::I32, 1);
        let shifted = self.builder.ins().ushr(val, c1);
        let sign_mask = self.builder.ins().iconst(types::I64, 1i64 << 55);
        let sign_bit = self.builder.ins().band(val, sign_mask);
        self.builder.ins().bor(shifted, sign_bit)
    }

    pub(super) fn emit_alu_asr(&mut self, d: Accumulator) {
        let acc = self.load_acc(d);
        // Carry = bit 0
        let carry = self.extract_bit_i64(acc, 0);
        self.set_carry(carry);
        self.clear_v_flag();
        // Arithmetic right shift by 1, keeping sign in bit 55
        let result = self.asr_one_56bit(acc);
        let result56 = self.mask56(result);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(d, result56);
        self.set_flags_nz_sm(result56);
    }

    pub(super) fn emit_alu_asl(&mut self, d: Accumulator) {
        let acc = self.load_acc(d);
        let c1 = self.builder.ins().iconst(types::I32, 1);
        // C = bit 55 of source (shifted out)
        let carry = self.extract_bit_i64(acc, 55);
        // V = bit 55 XOR bit 54 (overflow if sign changes)
        let bit54 = self.extract_bit_i64(acc, 54);
        let overflow = self.builder.ins().bxor(carry, bit54);
        // Update SR: C, V, L
        let sr_new = self.clear_sr_flags((1u32 << sr::V) | (1u32 << sr::C));
        let sr_new = self.builder.ins().bor(sr_new, carry);
        let sr_new = self.or_vl(sr_new, overflow);
        self.store_reg(reg::SR, sr_new);
        // Shift and store result
        let shifted = self.builder.ins().ishl(acc, c1);
        let result56 = self.mask56(shifted);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(d, result56);
        self.set_flags_nz_sm(result56);
    }

    /// ADDR: Arithmetic shift right dest by 1, then add source accumulator.
    pub(super) fn emit_alu_addr(&mut self, src: Accumulator, dst: Accumulator) {
        self.emit_alu_addr_subr(src, dst, false);
    }

    /// SUBR: arithmetic shift-right dest by 1, then subtract source from dest.
    pub(super) fn emit_alu_subr(&mut self, src: Accumulator, dst: Accumulator) {
        self.emit_alu_addr_subr(src, dst, true);
    }

    /// Shared ADDR/SUBR implementation: ASR dest by 1, then add or subtract source.
    fn emit_alu_addr_subr(&mut self, src: Accumulator, dst: Accumulator, is_sub: bool) {
        let s = self.load_acc(src);
        let d = self.load_acc(dst);
        // Arithmetic shift right by 1: preserve sign bit (bit 55)
        let d_shifted = self.asr_one_56bit(d);
        let result = if is_sub {
            self.builder.ins().isub(d_shifted, s)
        } else {
            self.builder.ins().iadd(d_shifted, s)
        };
        let result56 = self.mask56(result);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(dst, result56);
        self.set_flags_addsub(result56, s, d_shifted, result, is_sub);
    }

    /// ADDL: logical shift-left dest by 1, then add source to dest.
    pub(super) fn emit_alu_addl(&mut self, src: Accumulator, dst: Accumulator) {
        self.emit_alu_addl_subl(src, dst, false);
    }

    /// SUBL: logical shift-left dest by 1, then subtract source from dest.
    pub(super) fn emit_alu_subl(&mut self, src: Accumulator, dst: Accumulator) {
        self.emit_alu_addl_subl(src, dst, true);
    }

    /// Shared ADDL/SUBL implementation: LSL dest by 1, then add or subtract source.
    fn emit_alu_addl_subl(&mut self, src: Accumulator, dst: Accumulator, is_sub: bool) {
        let s = self.load_acc(src);
        let d = self.load_acc(dst);
        let c1 = self.builder.ins().iconst(types::I32, 1);
        // ASL flags: carry = bit 55, v = (bit55 before != bit55 after)
        let asl_carry = self.extract_bit_i64(d, 55);
        let d_shifted = self.builder.ins().ishl(d, c1);
        let d_shifted = self.mask56(d_shifted);
        let bit55_after32 = self.extract_bit_i64(d_shifted, 55);
        let asl_v = self.builder.ins().bxor(asl_carry, bit55_after32);
        let result = if is_sub {
            self.builder.ins().isub(d_shifted, s)
        } else {
            self.builder.ins().iadd(d_shifted, s)
        };
        let result56 = self.mask56(result);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(dst, result56);
        self.set_flags_addl_subl(result56, s, d_shifted, result, is_sub, asl_carry, asl_v);
    }

    /// XOR `carry` into SR.C, OR `overflow` into SR.V and SR.L.
    /// Used by ADDL/SUBL where the shift carry combines with the add/sub carry via XOR.
    pub(super) fn xor_c_or_vl(&mut self, carry: Value, overflow: Value) {
        let sr_val = self.load_reg(reg::SR);
        // XOR carry into C (bit 0)
        let sr_new = self.builder.ins().bxor(sr_val, carry);
        // OR overflow into V and L
        let v_bit = self.shift_to_bit(overflow, sr::V);
        let sr_new = self.builder.ins().bor(sr_new, v_bit);
        let l_bit = self.shift_to_bit(overflow, sr::L);
        let sr_new = self.builder.ins().bor(sr_new, l_bit);
        self.store_reg(reg::SR, sr_new);
    }

    /// ADC: Add X/Y (sign-extended to 56-bit) + carry flag to accumulator.
    pub(super) fn emit_alu_adc(&mut self, hi_reg: usize, lo_reg: usize, d: Accumulator) {
        self.emit_alu_adc_sbc(hi_reg, lo_reg, d, false);
    }

    /// SBC: Subtract X/Y (sign-extended to 56-bit) + carry flag from accumulator.
    pub(super) fn emit_alu_sbc(&mut self, hi_reg: usize, lo_reg: usize, d: Accumulator) {
        self.emit_alu_adc_sbc(hi_reg, lo_reg, d, true);
    }

    /// Shared ADC/SBC implementation: add or subtract X/Y + carry to/from accumulator.
    fn emit_alu_adc_sbc(&mut self, hi_reg: usize, lo_reg: usize, d: Accumulator, is_sub: bool) {
        let src56 = self.load_xy_as_acc56(hi_reg, lo_reg);
        let dst = self.load_acc(d);
        // Read current carry
        let sr_val = self.load_reg(reg::SR);
        let one32 = self.builder.ins().iconst(types::I32, 1);
        let cur_carry = self.builder.ins().band(sr_val, one32); // C is bit 0
        let cur_carry64 = self.builder.ins().uextend(types::I64, cur_carry);
        let result = if is_sub {
            let result = self.builder.ins().isub(dst, src56);
            self.builder.ins().isub(result, cur_carry64)
        } else {
            let result = self.builder.ins().iadd(dst, src56);
            self.builder.ins().iadd(result, cur_carry64)
        };
        let result56 = self.mask56(result);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(d, result56);
        self.set_flags_addsub(result56, src56, dst, result, is_sub);
    }

    /// Helper: compute absolute value of a 56-bit accumulator value.
    pub(super) fn abs56(&mut self, val: Value) -> Value {
        let c55 = self.builder.ins().iconst(types::I32, 55);
        let sign = self.builder.ins().ushr(val, c55);
        let sign32 = self.builder.ins().ireduce(types::I32, sign);
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let is_neg = self.builder.ins().icmp(IntCC::NotEqual, sign32, zero32);
        let zero64 = self.builder.ins().iconst(types::I64, 0);
        let negated = self.builder.ins().isub(zero64, val);
        let negated = self.mask56(negated);
        self.builder.ins().select(is_neg, negated, val)
    }

    /// CMPM between two accumulators: |D| - |S|, update flags, don't store.
    pub(super) fn emit_alu_cmpm_acc(&mut self, src: Accumulator, dst: Accumulator) {
        let s = self.load_acc(src);
        let d = self.load_acc(dst);
        let abs_s = self.abs56(s);
        let abs_d = self.abs56(d);
        let result = self.builder.ins().isub(abs_d, abs_s);
        let result56 = self.mask56(result);
        self.set_flags_nz_vcl_sub(result56, abs_s, abs_d, result);
    }

    /// CMPM with 24-bit register source: |reg| - |D|, update flags, don't store.
    pub(super) fn emit_alu_cmpm_reg24(&mut self, src_reg: usize, d: Accumulator) {
        let src56 = self.load_reg24_as_acc56(src_reg);
        let dst = self.load_acc(d);
        let abs_s = self.abs56(src56);
        let abs_d = self.abs56(dst);
        let result = self.builder.ins().isub(abs_d, abs_s);
        let result56 = self.mask56(result);
        self.set_flags_nz_vcl_sub(result56, abs_s, abs_d, result);
    }

    /// RND: Convergent round accumulator based on SR scaling mode.
    pub(super) fn emit_alu_rnd(&mut self, d: Accumulator) {
        // Add rounding constant, then apply convergent rounding, then truncate
        // lower bits.
        let acc = self.load_acc(d);
        let sr_val = self.load_reg(reg::SR);
        let s0_mask = self.builder.ins().iconst(types::I32, 1i64 << sr::S0);
        let s1_mask = self.builder.ins().iconst(types::I32, 1i64 << sr::S1);
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let zero64 = self.builder.ins().iconst(types::I64, 0);
        let has_s0 = self.builder.ins().band(sr_val, s0_mask);
        let is_s0 = self.builder.ins().icmp(IntCC::NotEqual, has_s0, zero32);
        let has_s1 = self.builder.ins().band(sr_val, s1_mask);
        let is_s1 = self.builder.ins().icmp(IntCC::NotEqual, has_s1, zero32);
        // RM=0: convergent (round-to-even), RM=1: two's complement (just truncate)
        let rm_mask = self.builder.ins().iconst(types::I32, 1i64 << sr::RM);
        let has_rm = self.builder.ins().band(sr_val, rm_mask);
        let is_convergent = self.builder.ins().icmp(IntCC::Equal, has_rm, zero32);

        let mask24 = self.builder.ins().iconst(types::I64, 0xFFFFFF);
        let c24 = self.builder.ins().iconst(types::I32, 24);

        // Default mode (no scaling)
        // Add 0x800000 to acc, if a0==0 after add clear bit 0 of a1, zero a0
        let rnd_def = self.builder.ins().iconst(types::I64, 0x800000);
        let rd = self.builder.ins().iadd(acc, rnd_def);
        let rd = self.mask56(rd);
        let rd_a0 = self.builder.ins().band(rd, mask24);
        let rd_a0_zero = self.builder.ins().icmp(IntCC::Equal, rd_a0, zero64);
        // Clear bit 0 of a1 (convergent: round to even) when a0 == 0 AND RM=0
        let rd_conv_cond = self.builder.ins().band(rd_a0_zero, is_convergent);
        let bit0_a1_mask = self.builder.ins().iconst(types::I64, 1i64 << 24);
        let rd_cleared = self.builder.ins().band_not(rd, bit0_a1_mask);
        let rd = self.builder.ins().select(rd_conv_cond, rd_cleared, rd);
        // Zero out a0
        let mask_a0 = self.builder.ins().iconst(types::I64, !0xFFFFFFi64);
        let mask_a0 = self.mask56(mask_a0);
        let result_def = self.builder.ins().band(rd, mask_a0);

        // S0 mode
        // rnd_const[1]=1, [2]=0 -> add (1 << 24) to acc
        let rnd_s0 = self.builder.ins().iconst(types::I64, 1i64 << 24);
        let rs0 = self.builder.ins().iadd(acc, rnd_s0);
        let rs0 = self.mask56(rs0);
        let rs0_a0 = self.builder.ins().band(rs0, mask24);
        let rs0_a1 = {
            let shifted = self.builder.ins().ushr(rs0, c24);
            self.builder.ins().band(shifted, mask24)
        };
        // Convergent rounding: if A0==0 and A1 bit 0 is even AND RM=0, clear A1 bits 1:0
        let one64 = self.builder.ins().iconst(types::I64, 1);
        let rs0_a0_zero = self.builder.ins().icmp(IntCC::Equal, rs0_a0, zero64);
        let rs0_a1_bit0 = self.builder.ins().band(rs0_a1, one64);
        let rs0_a1_even = self.builder.ins().icmp(IntCC::Equal, rs0_a1_bit0, zero64);
        let rs0_conv = self.builder.ins().band(rs0_a0_zero, rs0_a1_even);
        let rs0_conv = self.builder.ins().band(rs0_conv, is_convergent);
        let bits10_mask = self.builder.ins().iconst(types::I64, 0x3i64 << 24);
        let rs0_conv_cleared = self.builder.ins().band_not(rs0, bits10_mask);
        let rs0 = self.builder.ins().select(rs0_conv, rs0_conv_cleared, rs0);
        // Clear A1 bit 0, then clear A0
        let rs0 = self.builder.ins().band_not(rs0, bit0_a1_mask);
        let result_s0 = self.builder.ins().band(rs0, mask_a0);

        // S1 mode: rounding constant is 0x400000 (bit 22 of A0)
        let rnd_s1 = self.builder.ins().iconst(types::I64, 0x400000);
        let rs1 = self.builder.ins().iadd(acc, rnd_s1);
        let rs1 = self.mask56(rs1);
        let rs1_a0 = self.builder.ins().band(rs1, mask24);
        // If A0 bits 22:0 are all zero AND RM=0, clear all of A0
        let mask_7f = self.builder.ins().iconst(types::I64, 0x7FFFFF);
        let rs1_a0_low23 = self.builder.ins().band(rs1_a0, mask_7f);
        let rs1_low23_zero = self.builder.ins().icmp(IntCC::Equal, rs1_a0_low23, zero64);
        let rs1_conv_cond = self.builder.ins().band(rs1_low23_zero, is_convergent);
        let rs1_a0_cleared = self.builder.ins().band_not(rs1, mask24);
        let rs1 = self
            .builder
            .ins()
            .select(rs1_conv_cond, rs1_a0_cleared, rs1);
        // Keep only bit 23 of A0 (sign extension bit)
        let mask_bit23 = self.builder.ins().iconst(types::I64, 0x800000);
        let a0_kept = self.builder.ins().band(rs1, mask_bit23);
        let result_s1 = self.builder.ins().band_not(rs1, mask24);
        let result_s1 = self.builder.ins().bor(result_s1, a0_kept);

        // Select based on scaling mode
        let result = self.builder.ins().select(is_s0, result_s0, result_def);
        let result = self.builder.ins().select(is_s1, result_s1, result);
        let result = self.emit_saturate_sm(result);

        self.store_acc(d, result);
        // V = overflow from rounding addition.  The rounding constant is always
        // positive, so only positive-to-negative overflow is possible (adding a
        // positive value to a negative value cannot overflow).  Standard formula
        // with sign_B=0: V = (sign_A XOR sign_R) AND sign_R.
        let orig_sign = self.extract_bit_i64(acc, 55);
        let result_sign = self.extract_bit_i64(result, 55);
        let sign_changed = self.builder.ins().bxor(orig_sign, result_sign);
        let v_overflow = self.builder.ins().band(sign_changed, result_sign);
        self.set_flags_nz_vl_sm(result, v_overflow);
    }

    /// MAX: If B - A <= 0 (i.e., A >= B), copy A -> B. C cleared on transfer, set otherwise.
    pub(super) fn emit_alu_max(&mut self) {
        let a = self.load_acc(Accumulator::A);
        let b = self.load_acc(Accumulator::B);
        // Compute B - A (per DSP56300FM p.13-106: "If B - A <= 0")
        let diff = self.builder.ins().isub(b, a);
        let diff56 = self.mask56(diff);
        self.emit_max_tail(a, b, diff56);
    }

    /// MAXM: If |B| - |A| <= 0 (i.e., |A| >= |B|), copy A -> B. C cleared on transfer.
    /// Per DSP56300FM p.13-107. E/U/N/Z/V unchanged.
    pub(super) fn emit_alu_maxm(&mut self) {
        let a = self.load_acc(Accumulator::A);
        let b = self.load_acc(Accumulator::B);
        let abs_a = self.abs56(a);
        let abs_b = self.abs56(b);
        // Compute |B| - |A|
        let diff = self.builder.ins().isub(abs_b, abs_a);
        let diff56 = self.mask56(diff);
        self.emit_max_tail(a, b, diff56);
    }

    /// Shared MAX/MAXM tail: given original A, B and a 56-bit diff,
    /// conditionally copy A->B when diff <= 0, set C flag, L = L | V.
    fn emit_max_tail(&mut self, a: Value, b: Value, diff56: Value) {
        // Check sign bit (bit 55): if set, diff < 0
        let is_negative = self.extract_bit_i64(diff56, 55);
        // Check if result is zero
        let zero64 = self.builder.ins().iconst(types::I64, 0);
        let is_zero = self.builder.ins().icmp(IntCC::Equal, diff56, zero64);
        let is_zero_i32 = self.builder.ins().uextend(types::I32, is_zero);
        // pass = negative OR zero
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let one32 = self.builder.ins().iconst(types::I32, 1);
        let pass = self.builder.ins().bor(is_negative, is_zero_i32);
        let pass_bool = self.builder.ins().icmp(IntCC::NotEqual, pass, zero32);
        // If pass, copy A to B
        let a0 = self.extract_acc_lo(a);
        let a1 = self.extract_acc_mid(a);
        let a2 = self.extract_acc_hi(a);
        let a_packed = self.pack_acc(a2, a1, a0);
        let new_b = self.builder.ins().select(pass_bool, a_packed, b);
        self.store_acc(Accumulator::B, new_b);
        // C = 0 on transfer (pass), C = 1 otherwise (NOT pass)
        let not_pass = self.builder.ins().bxor(pass, one32);
        let not_pass = self.builder.ins().band(not_pass, one32);
        let sr_new = self.clear_sr_flags(1u32 << sr::C);
        let sr_new = self.builder.ins().bor(sr_new, not_pass);
        // L = L | V (standard definition: sticky overflow)
        let v_mask = self.builder.ins().iconst(types::I32, 1i64 << sr::V);
        let v_val = self.builder.ins().band(sr_new, v_mask);
        let shift_amt = self
            .builder
            .ins()
            .iconst(types::I32, (sr::L - sr::V) as i64);
        let l_from_v = self.builder.ins().ishl(v_val, shift_amt);
        let sr_new = self.builder.ins().bor(sr_new, l_from_v);
        self.store_reg(reg::SR, sr_new);
    }

    /// Shared multiply core: load sources, multiply, shift, optionally negate.
    /// Returns the 56-bit product result.
    pub(super) fn emit_alu_mpy_core(&mut self, s1: usize, s2: usize, negate: bool) -> Value {
        let s1_32 = self.load_reg(s1);
        let s2_32 = self.load_reg(s2);

        let s1_64 = self.sext24_to_i64(s1_32);
        let s2_64 = self.sext24_to_i64(s2_32);

        let product = self.builder.ins().imul(s1_64, s2_64);

        // Shift left 1 (fractional format: remove extra sign bit)
        let c1 = self.builder.ins().iconst(types::I32, 1);
        let product_shifted = self.builder.ins().ishl(product, c1);

        let result = if negate {
            let zero = self.builder.ins().iconst(types::I64, 0);
            self.builder.ins().isub(zero, product_shifted)
        } else {
            product_shifted
        };

        self.mask56(result)
    }

    pub(super) fn emit_alu_mpy(&mut self, s1: usize, s2: usize, d: Accumulator, negate: bool) {
        let result56 = self.emit_alu_mpy_core(s1, s2, negate);
        let result56 = self.emit_saturate_sm(result56);
        self.store_acc(d, result56);
        self.set_flags_nz_clear_v_sm(result56);
    }

    pub(super) fn emit_alu_mpyr(&mut self, s1: usize, s2: usize, d: Accumulator, negate: bool) {
        let result56 = self.emit_alu_mpy_core(s1, s2, negate);
        let rounded = self.emit_rnd56(result56);
        let rounded = self.emit_saturate_sm(rounded);
        self.store_acc(d, rounded);
        self.set_flags_nz_clear_v_sm(rounded);
    }

    pub(super) fn emit_alu_mac(&mut self, s1: usize, s2: usize, d: Accumulator, negate: bool) {
        let result56 = self.emit_alu_mpy_core(s1, s2, negate);
        let acc = self.load_acc(d);
        let sum = self.builder.ins().iadd(acc, result56);
        let sum56 = self.mask56(sum);
        let saturated = self.emit_saturate_sm(sum56);
        self.store_acc(d, saturated);
        self.set_flags_mac_vl_sm(sum56, result56, acc);
    }

    pub(super) fn emit_alu_macr(&mut self, s1: usize, s2: usize, d: Accumulator, negate: bool) {
        let product = self.emit_alu_mpy_core(s1, s2, negate);
        let acc = self.load_acc(d);
        let sum = self.builder.ins().iadd(acc, product);
        let sum56 = self.mask56(sum);
        let rounded = self.emit_rnd56(sum56);
        let saturated = self.emit_saturate_sm(rounded);
        self.store_acc(d, saturated);
        // MACR "result" is the rounded value (pre-saturation) per manual:
        // operation is D +/- S1*S2 + r -> D
        self.set_flags_mac_vl_sm(rounded, product, acc);
    }

    /// Emit a call to jit_rnd56(state, val) -> rounded i64.
    /// Implements convergent rounding per DSP56300 spec.
    /// jit_rnd56 reads SR (S0/S1 bits) but is otherwise pure.
    pub(super) fn emit_rnd56(&mut self, val: Value) -> Value {
        self.flush_sr();
        let fn_ptr = self
            .builder
            .ins()
            .iconst(self.ptr_ty, jit_rnd56 as *const () as usize as i64);
        let mut sig = Signature::new(HOST_CALL_CONV);
        sig.params.push(AbiParam::new(self.ptr_ty)); // *mut DspState
        sig.params.push(AbiParam::new(types::I64)); // val
        sig.returns.push(AbiParam::new(types::I64)); // return i64
        let sig_ref = self.builder.import_signature(sig);
        let call = self
            .builder
            .ins()
            .call_indirect(sig_ref, fn_ptr, &[self.state_ptr, val]);
        self.builder.inst_results(call)[0]
    }

    /// Add or subtract `src` from accumulator `dst_acc`, mask to 56 bits,
    /// optionally store, and update NZ + VCL flags.
    pub(super) fn emit_acc_addsub(
        &mut self,
        src: Value,
        dst_acc: Accumulator,
        is_sub: bool,
        store: bool,
    ) {
        let dst = self.load_acc(dst_acc);
        let result = if is_sub {
            self.builder.ins().isub(dst, src)
        } else {
            self.builder.ins().iadd(dst, src)
        };
        let result56 = self.mask56(result);
        let result56 = if store {
            let sat = self.emit_saturate_sm(result56);
            self.store_acc(dst_acc, sat);
            sat
        } else {
            result56
        };
        self.set_flags_addsub(result56, src, dst, result, is_sub);
    }

    pub(super) fn emit_normf(&mut self, src_reg: usize, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let sreg_val = self.load_reg(src_reg);
        let acc = self.load_acc(d);

        // Check bit 23 of source (sign)
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let sign_raw = self.builder.ins().ushr(sreg_val, c23);
        let one32 = self.builder.ins().iconst(types::I32, 1);
        let sign_bit = self.builder.ins().band(sign_raw, one32);

        // Compute absolute shift amount = |S| (bits 5:0)
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let is_neg = self.builder.ins().icmp(IntCC::NotEqual, sign_bit, zero32);
        // For negative 24-bit: negate = (~val + 1) & 0xFFFFFF
        let neg_val = self.builder.ins().ineg(sreg_val);
        let neg_val = self.mask24(neg_val);
        let abs_val = self.builder.ins().select(is_neg, neg_val, sreg_val);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let shift_amt = self.builder.ins().band(abs_val, mask6);

        // If negative (S[23]=1): ASL D by |S|, else ASR D by S
        // ASL: shift left, fill with zeros
        let asl_result = self.builder.ins().ishl(acc, shift_amt);
        let asl_result = self.mask56(asl_result);
        // ASR: arithmetic shift right (sign-extending)
        // Need to sign-extend 56-bit to 64-bit first, shift, then mask back
        let c8_32 = self.builder.ins().iconst(types::I32, 8);
        let sext = self.builder.ins().ishl(acc, c8_32);
        let sext = self.builder.ins().sshr(sext, c8_32); // sign-extend bit 55 to 63
        let asr_result = self.builder.ins().sshr(sext, shift_amt);
        let asr_result = self.mask56(asr_result);

        let result = self.builder.ins().select(is_neg, asl_result, asr_result);
        let result = self.emit_saturate_sm(result);
        self.store_acc(d, result);

        // V = "Set if bit 55 is changed any time during the shift operation"
        // (The manual text says "bit 39" but that's a DSP56000 40-bit holdover;
        // for the 56-bit DSP56300, the MSB is bit 55 - same as NORM/ASL.)
        //
        // For ASL (left shift): bit 55 can change if the bits being shifted into
        // position 55 differ from the original sign. Check uniformity of
        // bits [55:(55-n)] of the original value.
        //
        // For ASR (right shift): bit 55 is the sign bit and is always replicated
        // during arithmetic right shift - it can never change. V is always 0.
        let c55 = self.builder.ins().iconst(types::I32, 55);
        let shift_plus1 = self.builder.ins().iadd(shift_amt, one32);
        let field_start = self.builder.ins().isub(c55, shift_amt);
        let field = self.builder.ins().ushr(acc, field_start);
        // Build (shift+1)-bit mask
        let one_i64 = self.builder.ins().iconst(types::I64, 1);
        let field_mask = self.builder.ins().ishl(one_i64, shift_plus1);
        let field_mask = self.builder.ins().isub(field_mask, one_i64);
        let field = self.builder.ins().band(field, field_mask);
        // V=1 if field is not all-0 and not all-1
        let zero_i64 = self.builder.ins().iconst(types::I64, 0);
        let is_all_zero = self.builder.ins().icmp(IntCC::Equal, field, zero_i64);
        let is_all_ones = self.builder.ins().icmp(IntCC::Equal, field, field_mask);
        let is_uniform = self.builder.ins().bor(is_all_zero, is_all_ones);
        let uniform_i32 = self.builder.ins().uextend(types::I32, is_uniform);
        let asl_v_bit = self.builder.ins().isub(one32, uniform_i32);
        // V is only set for the ASL (left shift) path; ASR always has V=0
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let v_bit = self.builder.ins().select(is_neg, asl_v_bit, zero32);
        self.set_flags_nz_vl_sm(result, v_bit);
    }

    pub(super) fn emit_asl_imm(&mut self, shift: u8, s: Accumulator, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let shift = shift as u32;
        let acc = self.load_acc(s);

        // C = bit (55 - shift) of original (last bit shifted out of MSB)
        // For shift=0, (55-0)=55 -> bit 55 which is 0 in a 56-bit value -> C=0
        let carry_i32 = self.extract_bit_i64(acc, 56u32.wrapping_sub(shift));

        let cnt = self.builder.ins().iconst(types::I32, shift as i64);
        let result = self.builder.ins().ishl(acc, cnt);
        let result_m = self.mask56(result);
        let result_m = self.emit_saturate_sm(result_m);
        self.store_acc(d, result_m);

        // V = "set if bit 55 is changed any time during the shift operation"
        // This means: extract bits [55:(55-shift)] of original. If NOT all same, V=1.
        // Equivalent: shift right by (55-shift), check if result is all-0 or all-1
        // within the (shift+1)-bit field.
        let v_bit = if shift == 0 {
            self.builder.ins().iconst(types::I32, 0)
        } else {
            let field_shift = 55u32.saturating_sub(shift);
            let fs = self.builder.ins().iconst(types::I32, field_shift as i64);
            let field = self.builder.ins().ushr(acc, fs);
            // Mask to (shift+1) bits
            let field_mask = ((1u64 << (shift + 1)) - 1) as i64;
            let fm = self.builder.ins().iconst(types::I64, field_mask);
            let field = self.builder.ins().band(field, fm);
            // V=1 if field is not all-0 and not all-1 (within the mask)
            let zero_i64 = self.builder.ins().iconst(types::I64, 0);
            let all_ones = self.builder.ins().iconst(types::I64, field_mask);
            let is_all_zero = self.builder.ins().icmp(IntCC::Equal, field, zero_i64);
            let is_all_ones = self.builder.ins().icmp(IntCC::Equal, field, all_ones);
            let is_uniform = self.builder.ins().bor(is_all_zero, is_all_ones);
            let one = self.builder.ins().iconst(types::I32, 1);
            let uniform_i32 = self.builder.ins().uextend(types::I32, is_uniform);
            self.builder.ins().isub(one, uniform_i32)
        };

        // Update SR: clear C, V; set new C, V; L = L_old | V_new
        let sr = self.clear_sr_flags((1u32 << sr::C) | (1u32 << sr::V));
        let c_shifted = self.shift_to_bit(carry_i32, sr::C);
        let sr = self.builder.ins().bor(sr, c_shifted);
        let sr = self.or_vl(sr, v_bit);
        self.store_reg(reg::SR, sr);
        self.set_flags_nz_sm(result_m);
    }

    pub(super) fn emit_asr_imm(&mut self, shift: u8, s: Accumulator, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let shift = shift as u32;
        let acc = self.load_acc(s);

        // C = bit (shift-1) of original (last bit shifted out), C=0 if shift=0
        let carry_bit = if shift > 0 {
            self.extract_bit_i64(acc, shift - 1)
        } else {
            self.builder.ins().iconst(types::I32, 0)
        };

        // Arithmetic shift right: sign-extend from 56 to 64 bits, then sshr
        let sext = self.sext56_to_i64(acc);
        let cnt = self.builder.ins().iconst(types::I32, shift as i64);
        let result = self.builder.ins().sshr(sext, cnt);
        let result_m = self.mask56(result);
        let result_m = self.emit_saturate_sm(result_m);
        self.store_acc(d, result_m);

        // Update SR: clear C and V; set new C (ASR doesn't set V or L)
        let sr = self.clear_sr_flags((1u32 << sr::C) | (1u32 << sr::V));
        let c_shifted = self.shift_to_bit(carry_bit, sr::C);
        let sr = self.builder.ins().bor(sr, c_shifted);
        self.store_reg(reg::SR, sr);
        self.set_flags_nz_sm(result_m);
    }

    pub(super) fn emit_asl_reg(&mut self, src_reg: usize, s: Accumulator, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let acc = self.load_acc(s);

        // Shift amount = bits 5-0 of control register S1, clamped to 0-63
        let sreg_val = self.load_reg(src_reg);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let shift_i32 = self.builder.ins().band(sreg_val, mask6);

        // C = bit (56 - shift) of original; C=0 if shift=0
        let c56 = self.builder.ins().iconst(types::I32, 56);
        let carry_pos = self.builder.ins().isub(c56, shift_i32);
        let carry_raw = self.builder.ins().ushr(acc, carry_pos);
        let one_i64 = self.builder.ins().iconst(types::I64, 1);
        let carry_bit = self.builder.ins().band(carry_raw, one_i64);
        let carry_i32 = self.builder.ins().ireduce(types::I32, carry_bit);

        let result = self.builder.ins().ishl(acc, shift_i32);
        let result_m = self.mask56(result);
        let result_m = self.emit_saturate_sm(result_m);
        self.store_acc(d, result_m);

        // V = "set if bit 55 is changed any time during the shift"
        // Extract bits [55:(55-shift)] by shifting right by (55-shift), then check
        // if the field (shift+1 bits wide) is all-0 or all-1.
        let c55 = self.builder.ins().iconst(types::I32, 55);
        let field_shift = self.builder.ins().isub(c55, shift_i32);
        let field = self.builder.ins().ushr(acc, field_shift);
        // Build (shift+1)-bit mask: (1 << (shift+1)) - 1
        let one = self.builder.ins().iconst(types::I32, 1);
        let shift_plus1 = self.builder.ins().iadd(shift_i32, one);
        let one_i64 = self.builder.ins().iconst(types::I64, 1);
        let field_mask = self.builder.ins().ishl(one_i64, shift_plus1);
        let field_mask = self.builder.ins().isub(field_mask, one_i64);
        let field = self.builder.ins().band(field, field_mask);
        // V=1 if field is not all-0 and not all-1
        let zero_i64 = self.builder.ins().iconst(types::I64, 0);
        let is_all_zero = self.builder.ins().icmp(IntCC::Equal, field, zero_i64);
        let is_all_ones = self.builder.ins().icmp(IntCC::Equal, field, field_mask);
        let is_uniform = self.builder.ins().bor(is_all_zero, is_all_ones);
        let uniform_i32 = self.builder.ins().uextend(types::I32, is_uniform);
        let v_bit = self.builder.ins().isub(one, uniform_i32);

        // Update SR: clear C, V; set new C, V; L = L_old | V_new
        let sr = self.clear_sr_flags((1u32 << sr::C) | (1u32 << sr::V));
        let c_shifted = self.shift_to_bit(carry_i32, sr::C);
        let sr = self.builder.ins().bor(sr, c_shifted);
        let sr = self.or_vl(sr, v_bit);
        self.store_reg(reg::SR, sr);
        self.set_flags_nz_sm(result_m);
    }

    pub(super) fn emit_asr_reg(&mut self, src_reg: usize, s: Accumulator, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let acc = self.load_acc(s);

        // Shift amount = bits 5-0 of control register S1
        let sreg_val = self.load_reg(src_reg);
        let mask6 = self.builder.ins().iconst(types::I32, 0x3F);
        let shift_i32 = self.builder.ins().band(sreg_val, mask6);

        // C = bit (shift-1) of original; C=0 if shift=0
        let one_i32 = self.builder.ins().iconst(types::I32, 1);
        let carry_pos = self.builder.ins().isub(shift_i32, one_i32);
        let carry_raw = self.builder.ins().ushr(acc, carry_pos);
        let one_i64 = self.builder.ins().iconst(types::I64, 1);
        let carry_bit = self.builder.ins().band(carry_raw, one_i64);
        let carry_i32 = self.builder.ins().ireduce(types::I32, carry_bit);
        // If shift=0, carry_pos wraps to large value, ushr gives 0, so C=0. Correct.

        // Arithmetic shift right: sign-extend from 56 to 64 bits, then sshr
        let sext = self.sext56_to_i64(acc);
        let result = self.builder.ins().sshr(sext, shift_i32);
        let result_m = self.mask56(result);
        let result_m = self.emit_saturate_sm(result_m);
        self.store_acc(d, result_m);

        // Update SR: clear C and V; set new C
        let sr = self.clear_sr_flags((1u32 << sr::C) | (1u32 << sr::V));
        let c_shifted = self.shift_to_bit(carry_i32, sr::C);
        let sr = self.builder.ins().bor(sr, c_shifted);
        self.store_reg(reg::SR, sr);
        self.set_flags_nz_sm(result_m);
    }

    pub(super) fn emit_cmpu(&mut self, src_reg: usize, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);

        // Per DSP56300FM p.13-48: unsigned 48-bit comparison, EXP does not affect.
        // Load destination bits 47:0 (ignore extension register)
        let dst_full = self.load_acc(d);
        let mask48 = self
            .builder
            .ins()
            .iconst(types::I64, 0x0000FFFFFFFFFFFF_u64 as i64);
        let dst = self.builder.ins().band(dst_full, mask48);

        // Load source: accumulator uses full 48 bits, register uses 24-bit zero-extended
        let src = if src_reg == reg::A || src_reg == reg::B {
            // Accumulator source: use bits 47:0, ignore extension (per manual)
            let acc = self.load_acc(if src_reg == reg::A {
                Accumulator::A
            } else {
                Accumulator::B
            });
            self.builder.ins().band(acc, mask48)
        } else {
            // 24-bit register: aligned left, zero-filled to 48 bits (unsigned)
            let src_val = self.load_reg(src_reg);
            self.val24_to_acc56_unsigned(src_val)
        };

        let result = self.builder.ins().isub(dst, src);

        // Update N, Z, C (V always cleared)
        let sr = self
            .clear_sr_flags((1u32 << sr::N) | (1u32 << sr::Z) | (1u32 << sr::V) | (1u32 << sr::C));

        // C flag = borrow from bit 48 (standard carry from unsigned subtraction)
        let carry = self.extract_bit_i64(result, 48);
        let c_flag = self.shift_to_bit(carry, sr::C);
        let sr = self.builder.ins().bor(sr, c_flag);

        // Z flag = bits 47-0 of result are 0
        let result48 = self.builder.ins().band(result, mask48);
        let zero64 = self.builder.ins().iconst(types::I64, 0);
        let is_zero = self.builder.ins().icmp(IntCC::Equal, result48, zero64);
        let one32 = self.builder.ins().iconst(types::I32, 1);
        let zero32 = self.builder.ins().iconst(types::I32, 0);
        let z_val = self.builder.ins().select(is_zero, one32, zero32);
        let z_flag = self.shift_to_bit(z_val, sr::Z);
        let sr = self.builder.ins().bor(sr, z_flag);

        // N flag = bit 55 of result (standard definition for arithmetic instructions)
        let result_m = self.mask56(result);
        let n_val = self.extract_bit_i64(result_m, 55);
        let n_flag = self.shift_to_bit(n_val, sr::N);
        let sr = self.builder.ins().bor(sr, n_flag);
        self.store_reg(reg::SR, sr);
    }

    pub(super) fn emit_norm(&mut self, rreg_idx: u8, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(5);

        let acc = d;
        let rreg = reg::R0 + rreg_idx as usize;

        // Read E, U, Z flags from SR
        let sr = self.load_reg(reg::SR);
        let e_val = self.extract_bit(sr, sr::E);
        let u_val = self.extract_bit(sr, sr::U);
        let z_val = self.extract_bit(sr, sr::Z);

        // cur_euz = !E & U & !Z
        let zero = self.builder.ins().iconst(types::I32, 0);
        let one = self.builder.ins().iconst(types::I32, 1);
        let not_e = self.builder.ins().isub(one, e_val);
        let not_z = self.builder.ins().isub(one, z_val);
        let euz = self.builder.ins().band(not_e, u_val);
        let euz = self.builder.ins().band(euz, not_z);

        let dst = self.load_acc(acc);

        // If euz: shift left 1, Rn--
        let shifted_left = {
            let c1 = self.builder.ins().iconst(types::I32, 1);
            let sl = self.builder.ins().ishl(dst, c1);
            self.mask56(sl)
        };
        // If E: arithmetic shift right 1 (preserve sign bit), Rn++
        let shifted_right = self.asr_one_56bit(dst);

        // Select result based on conditions
        let is_euz = self.builder.ins().icmp(IntCC::NotEqual, euz, zero);
        let is_e = self.builder.ins().icmp(IntCC::NotEqual, e_val, zero);

        let result1 = self.builder.ins().select(is_euz, shifted_left, dst);
        let result = self.builder.ins().select(is_e, shifted_right, result1);
        let result = self.emit_saturate_sm(result);

        self.store_acc(acc, result);

        let rn = self.load_reg(rreg);
        let rn_dec = self.builder.ins().isub(rn, one);
        let rn_inc = self.builder.ins().iadd(rn, one);
        let rn1 = self.builder.ins().select(is_euz, rn_dec, rn);
        let rn_final = self.builder.ins().select(is_e, rn_inc, rn1);
        let r_mask = self
            .builder
            .ins()
            .iconst(types::I32, self.address_register_mask as i64);
        let rn_final = self.builder.ins().band(rn_final, r_mask);
        self.store_reg(rreg, rn_final);

        // Update flags: V = set if bit 55 changed due to left shift (euz case only)
        let old55 = self.extract_bit_i64(dst, 55);
        let new55 = self.extract_bit_i64(shifted_left, 55);
        let asl_v_raw = self.builder.ins().bxor(old55, new55);
        // V only set when euz path was taken (left shift)
        let euz_i32 = self.builder.ins().uextend(types::I32, is_euz);
        let norm_v = self.builder.ins().band(asl_v_raw, euz_i32);
        self.set_flags_nz_vl_sm(result, norm_v);
    }

    pub(super) fn emit_mul_shift(
        &mut self,
        op: MulShiftOp,
        shift: u8,
        src_reg: usize,
        d: Accumulator,
        k: bool,
    ) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let s_val = self.load_reg(src_reg);
        let s_sext = self.sext24_to_i64(s_val);

        // Compute S * 2^-n: in accumulator format, S occupies bits [47:24],
        // so S_acc = S << 24. Then S_acc >> n = S << (24-n).
        let shift_amount = 24i32 - shift as i32;
        let result = if shift_amount >= 0 {
            let c = self.builder.ins().iconst(types::I32, shift_amount as i64);
            self.builder.ins().ishl(s_sext, c)
        } else {
            let c = self
                .builder
                .ins()
                .iconst(types::I32, (-shift_amount) as i64);
            self.builder.ins().sshr(s_sext, c)
        };

        // Negate if k=1
        let result = if k {
            let zero = self.builder.ins().iconst(types::I64, 0);
            self.builder.ins().isub(zero, result)
        } else {
            result
        };

        let result_m = self.mask56(result);

        // MAC/MACR: accumulate; MPY/MPYR: store directly
        let is_mac = matches!(op, MulShiftOp::Mac | MulShiftOp::Macr);
        let (final_val, mac_acc, mac_flag_result) = if is_mac {
            let acc = self.load_acc(d);
            let sum = self.builder.ins().iadd(acc, result_m);
            let sum56 = self.mask56(sum);
            // MACR: round, then use rounded value for flags (operation is D+/-S1*S2+r)
            // MAC: no rounding, use sum56 for flags
            if op == MulShiftOp::Macr {
                let rounded = self.emit_rnd56(sum56);
                (rounded, Some(acc), Some(rounded))
            } else {
                (sum56, Some(acc), Some(sum56))
            }
        } else {
            // MPYR: round; MPY: no round
            let val = match op {
                MulShiftOp::Mpyr => self.emit_rnd56(result_m),
                _ => result_m,
            };
            (val, None, None)
        };

        let final_val = self.emit_saturate_sm(final_val);
        self.store_acc(d, final_val);
        if let (Some(acc), Some(flag_result)) = (mac_acc, mac_flag_result) {
            self.set_flags_mac_vl_sm(flag_result, result_m, acc);
        } else {
            self.set_flags_nz_clear_v_sm(final_val);
        }
    }

    pub(super) fn emit_mpyi(&mut self, k: bool, d: Accumulator, src_reg: usize, next_word: u32) {
        self.emit_mpyi_family(k, d, src_reg, next_word, false, false);
    }

    /// Helper: compute signed multiply product for immediate forms.
    /// Returns the 56-bit masked product (already shifted left 1 and optionally negated).
    pub(super) fn emit_imm_multiply(&mut self, k: bool, src_reg: usize, next_word: u32) -> Value {
        let s1 = next_word as i64;
        let s2_val = self.load_reg(src_reg);

        let s1_const = self.builder.ins().iconst(types::I64, s1);
        let s1_sext = {
            let c40 = self.builder.ins().iconst(types::I32, 40);
            let sh = self.builder.ins().ishl(s1_const, c40);
            self.builder.ins().sshr(sh, c40)
        };
        let s2_sext = self.sext24_to_i64(s2_val);

        let product = self.builder.ins().imul(s1_sext, s2_sext);
        let one64 = self.builder.ins().iconst(types::I32, 1);
        let product = self.builder.ins().ishl(product, one64);

        if k {
            let zero = self.builder.ins().iconst(types::I64, 0);
            self.builder.ins().isub(zero, product)
        } else {
            product
        }
    }

    pub(super) fn emit_mpyri(&mut self, k: bool, d: Accumulator, src_reg: usize, next_word: u32) {
        self.emit_mpyi_family(k, d, src_reg, next_word, false, true);
    }

    pub(super) fn emit_maci(&mut self, k: bool, d: Accumulator, src_reg: usize, next_word: u32) {
        self.emit_mpyi_family(k, d, src_reg, next_word, true, false);
    }

    pub(super) fn emit_macri(&mut self, k: bool, d: Accumulator, src_reg: usize, next_word: u32) {
        self.emit_mpyi_family(k, d, src_reg, next_word, true, true);
    }

    /// Shared MPYI/MPYRI/MACI/MACRI implementation.
    /// - `accumulate`: if true, add product to existing accumulator (MAC variants)
    /// - `round`: if true, apply rounding (MPYRI/MACRI variants)
    fn emit_mpyi_family(
        &mut self,
        k: bool,
        d: Accumulator,
        src_reg: usize,
        next_word: u32,
        accumulate: bool,
        round: bool,
    ) {
        self.set_inst_len(2);
        self.set_cycles(2);
        let product = self.emit_imm_multiply(k, src_reg, next_word);
        let product_m = self.mask56(product);

        let (final_val, vl_info) = if accumulate {
            let acc = self.load_acc(d);
            let sum = self.builder.ins().iadd(acc, product_m);
            let sum_m = self.mask56(sum);
            if round {
                // MACRI: flags use rounded value (operation is D +/- #xxxx*S + r)
                let rounded = self.emit_rnd56(sum_m);
                let saturated = self.emit_saturate_sm(rounded);
                (saturated, Some((product_m, acc, rounded)))
            } else {
                let result_m = self.emit_saturate_sm(sum_m);
                (result_m, Some((product_m, acc, sum_m)))
            }
        } else if round {
            let rounded = self.emit_rnd56(product_m);
            let rounded = self.emit_saturate_sm(rounded);
            (rounded, None)
        } else {
            let result_m = self.emit_saturate_sm(product_m);
            (result_m, None)
        };

        self.store_acc(d, final_val);
        if let Some((pm, acc, sum)) = vl_info {
            self.set_flags_mac_vl_sm(sum, pm, acc);
        } else {
            self.set_flags_nz_clear_v_sm(final_val);
        }
    }

    pub(super) fn emit_dmac(
        &mut self,
        ss: u8,
        k: bool,
        d: Accumulator,
        s1_reg: usize,
        s2_reg: usize,
    ) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let s1_val = self.load_reg(s1_reg);
        let s2_val = self.load_reg(s2_reg);

        // Sign mode: bit8 (ss bit 1) controls S2, bit6 (ss bit 0) controls S1
        // ss=00 -> signed*signed, ss=01 -> unsigned*signed,
        // ss=10 -> signed*unsigned, ss=11 -> unsigned*unsigned
        let s1_64 = if ss & 1 != 0 {
            self.zext24_to_i64(s1_val)
        } else {
            self.sext24_to_i64(s1_val)
        };
        let s2_64 = if ss & 2 != 0 {
            self.zext24_to_i64(s2_val)
        } else {
            self.sext24_to_i64(s2_val)
        };

        let product = self.builder.ins().imul(s1_64, s2_64);
        let one64 = self.builder.ins().iconst(types::I32, 1);
        let product = self.builder.ins().ishl(product, one64);

        let product = if k {
            let zero = self.builder.ins().iconst(types::I64, 0);
            self.builder.ins().isub(zero, product)
        } else {
            product
        };

        // DMAC: D = (D >> 24) + S1*S2
        // Sign-extend acc from 56 to 64 bits before arithmetic shift
        let acc = self.load_acc(d);
        let acc_sext = self.sext56_to_i64(acc);
        let c24 = self.builder.ins().iconst(types::I32, 24);
        let acc_shifted = self.builder.ins().sshr(acc_sext, c24);
        let sum = self.builder.ins().iadd(acc_shifted, product);
        let result_m = self.mask56(sum);
        // SM only applies to DMAC ss (signed*signed), not su/uu
        let result_m = if ss == 0 {
            self.emit_saturate_sm(result_m)
        } else {
            result_m
        };
        self.store_acc(d, result_m);
        // V is always 0: the product fits in ~49 bits and D>>24 in ~32 bits,
        // so their sum cannot overflow 56 bits (2^49 + 2^31 << 2^55).
        if ss == 0 {
            self.set_flags_nz_clear_v_sm(result_m);
        } else {
            self.set_flags_nz_clear_v(result_m);
        }
    }

    pub(super) fn emit_mac_su(
        &mut self,
        s: u8,
        k: bool,
        d: Accumulator,
        s1_reg: usize,
        s2_reg: usize,
    ) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let s1_val = self.load_reg(s1_reg);
        let s2_val = self.load_reg(s2_reg);

        // s field: 0 = su (signed*unsigned), 1 = uu (unsigned*unsigned)
        let s1_64 = if s & 1 != 0 {
            self.zext24_to_i64(s1_val)
        } else {
            self.sext24_to_i64(s1_val)
        };
        let s2_64 = self.zext24_to_i64(s2_val);

        let product = self.builder.ins().imul(s1_64, s2_64);
        let one64 = self.builder.ins().iconst(types::I32, 1);
        let product = self.builder.ins().ishl(product, one64);

        let product = if k {
            let zero = self.builder.ins().iconst(types::I64, 0);
            self.builder.ins().isub(zero, product)
        } else {
            product
        };

        let product_m = self.mask56(product);
        let acc = self.load_acc(d);
        let sum = self.builder.ins().iadd(acc, product_m);
        let result_m = self.mask56(sum);
        self.store_acc(d, result_m);
        self.set_flags_dmac_vl(result_m, product_m, acc);
    }

    pub(super) fn emit_mpy_su(
        &mut self,
        s: u8,
        k: bool,
        d: Accumulator,
        s1_reg: usize,
        s2_reg: usize,
    ) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let s1_val = self.load_reg(s1_reg);
        let s2_val = self.load_reg(s2_reg);

        // s field: 0 = su (signed*unsigned), 1 = uu (unsigned*unsigned)
        let s1_64 = if s & 1 != 0 {
            self.zext24_to_i64(s1_val)
        } else {
            self.sext24_to_i64(s1_val)
        };
        let s2_64 = self.zext24_to_i64(s2_val);

        let product = self.builder.ins().imul(s1_64, s2_64);
        let one64 = self.builder.ins().iconst(types::I32, 1);
        let product = self.builder.ins().ishl(product, one64);

        let product = if k {
            let zero = self.builder.ins().iconst(types::I64, 0);
            self.builder.ins().isub(zero, product)
        } else {
            product
        };

        let result_m = self.mask56(product);
        self.store_acc(d, result_m);
        self.set_flags_nz_clear_v(result_m);
    }

    pub(super) fn emit_div(&mut self, src_reg: usize, d: Accumulator) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let acc = d;

        let src_val = self.load_reg(src_reg);
        let dst = self.load_acc(acc);

        // Sign of dest (bit 55) XOR sign of source (bit 23)
        let dest_sign = self.extract_bit_i64(dst, 55);
        let one = self.builder.ins().iconst(types::I32, 1);
        let c23 = self.builder.ins().iconst(types::I32, 23);
        let src_sign = self.builder.ins().ushr(src_val, c23);
        let src_sign = self.builder.ins().band(src_sign, one);
        let xor_signs = self.builder.ins().bxor(dest_sign, src_sign);

        // ASL dest by 1
        let old_bit55 = self.extract_bit_i64(dst, 55);
        let one64 = self.builder.ins().iconst(types::I32, 1);
        let shifted = self.builder.ins().ishl(dst, one64);
        let shifted_m = self.mask56(shifted);
        let new_bit55 = self.extract_bit_i64(shifted_m, 55);

        // Source placed at A1 position with sign extension
        let src_acc = self.val24_to_acc56(src_val);

        // If signs differ: add source, else: subtract source
        let add_result = self.builder.ins().iadd(shifted_m, src_acc);
        let sub_result = self.builder.ins().isub(shifted_m, src_acc);

        let zero = self.builder.ins().iconst(types::I32, 0);
        let signs_differ = self.builder.ins().icmp(IntCC::NotEqual, xor_signs, zero);
        let result = self
            .builder
            .ins()
            .select(signs_differ, add_result, sub_result);
        let result_m = self.mask56(result);

        // Set A0 bit 0 = old carry
        let sr = self.load_reg(reg::SR);
        let old_carry = self.extract_bit(sr, sr::C);
        let old_carry64 = self.builder.ins().uextend(types::I64, old_carry);
        let result_m = self.builder.ins().bor(result_m, old_carry64);

        self.store_acc(acc, result_m);

        // New carry = 1 - bit 55 of result
        let res_sign = self.extract_bit_i64(result_m, 55);
        let oneb = self.builder.ins().iconst(types::I32, 1);
        let new_carry = self.builder.ins().isub(oneb, res_sign);

        // V = set if MSB changed during left shift; L = L_old | V_new
        let div_v = self.builder.ins().bxor(old_bit55, new_bit55);
        let sr2 = self.clear_sr_flags((1u32 << sr::C) | (1u32 << sr::V));
        let sr2 = self.builder.ins().bor(sr2, new_carry);
        let sr2 = self.or_vl(sr2, div_v);
        self.store_reg(reg::SR, sr2);
    }

    pub(super) fn emit_tcc(
        &mut self,
        cc: CondCode,
        acc: Option<(usize, usize)>,
        r: Option<(u8, u8)>,
    ) {
        self.set_inst_len(1);
        self.set_cycles(1);

        let cond = self.eval_cc(cc);
        let zero = self.builder.ins().iconst(types::I32, 0);
        let taken = self.builder.ins().icmp(IntCC::NotEqual, cond, zero);

        let taken_blk = self.builder.create_block();
        let merge_blk = self.builder.create_block();

        let mut cond_state = self.begin_conditional();
        self.builder
            .ins()
            .brif(taken, taken_blk, &[], merge_blk, &[]);

        self.builder.switch_to_block(taken_blk);
        self.builder.seal_block(taken_blk);

        // S1,D1 accumulator transfer (templates 1 and 2)
        if let Some((src1, dst1)) = acc {
            let dst_acc = if dst1 == reg::A {
                Accumulator::A
            } else {
                Accumulator::B
            };
            if src1 == reg::A || src1 == reg::B {
                let src_acc = if src1 == reg::A {
                    Accumulator::A
                } else {
                    Accumulator::B
                };
                let val = self.load_acc(src_acc);
                self.store_acc(dst_acc, val);
            } else {
                let val = self.load_reg(src1);
                let packed = self.val24_to_acc56(val);
                self.store_acc(dst_acc, packed);
            }
        }

        // S2,D2 address register transfer (templates 2 and 3)
        if let Some((r_src, r_dst)) = r {
            let src2 = reg::R0 + r_src as usize;
            let dst2 = reg::R0 + r_dst as usize;
            let v = self.load_reg(src2);
            self.store_reg(dst2, v);
        }

        self.end_conditional_arm(&mut cond_state);
        self.builder.ins().jump(merge_blk, &[]);

        self.builder.switch_to_block(merge_blk);
        self.builder.seal_block(merge_blk);
        self.merge_conditional(&cond_state);
    }
}
