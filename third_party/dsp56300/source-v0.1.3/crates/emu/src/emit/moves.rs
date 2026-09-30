use super::{inst_len_for_ea, *};

impl<'a> Emitter<'a> {
    pub(super) fn emit_movep_0(&mut self, space: MemSpace, pp_offset: u8, reg_idx: u8, w: bool) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let pp_addr = 0xFFFFC0 + pp_offset as u32;
        let reg_idx = reg_idx as usize;

        if w {
            // Write to peripheral: reg -> pp
            let val = self.read_reg_for_move(reg_idx);
            self.write_mem(space, pp_addr, val);
        } else {
            // Read from peripheral: pp -> reg
            let val = self.read_mem(space, pp_addr);
            self.write_reg_for_move(reg_idx, val);
        }
    }

    pub(super) fn emit_movep_1(
        &mut self,
        space: MemSpace,
        pp_offset: u8,
        ea_mode: u8,
        w: bool,
        next_word: u32,
    ) {
        // C: emu_movep_1 - transfer between P:ea and X/Y peripheral
        // Template: 0000100sW1MMMRRR01pppppp
        self.set_inst_len(1);
        self.set_cycles(6);
        let pp_addr = 0xFFFFC0 + pp_offset as u32;

        let (p_addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        // Mode 6 (absolute) makes this a 2-word instruction
        self.set_inst_len(inst_len_for_ea(ea_mode));

        if w {
            // Write to peripheral: P:ea -> X/Y:pp
            let val = self.read_mem_dyn(MemSpace::P, p_addr);
            self.write_mem(space, pp_addr, val);
        } else {
            // Read from peripheral: X/Y:pp -> P:ea
            let val = self.read_mem(space, pp_addr);
            self.write_mem_dyn(MemSpace::P, p_addr, val);
        }
    }

    pub(super) fn emit_movep_23(
        &mut self,
        pp_offset: u8,
        ea_mode: u8,
        w: bool,
        perspace: MemSpace,
        easpace: MemSpace,
        next_word: u32,
    ) {
        self.set_inst_len(1);
        self.set_cycles(2);
        let pp_addr = 0xFFFFC0 + pp_offset as u32;

        // This form uses MMMRRR-based EA
        let (addr, is_imm) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        // Mode 6 makes this a 2-word instruction
        self.set_inst_len(inst_len_for_ea(ea_mode));

        if w {
            // Write pp: EA/imm -> peripheral
            let val = if is_imm {
                addr // Mode 6 immediate: value IS the address
            } else {
                self.read_mem_dyn(easpace, addr)
            };
            self.write_mem(perspace, pp_addr, val);
        } else {
            // Read pp: peripheral -> EA
            let val = self.read_mem(perspace, pp_addr);
            self.write_mem_dyn(easpace, addr, val);
        }
    }

    pub(super) fn emit_movep_qq(
        &mut self,
        qq_offset: u8,
        ea_mode: u8,
        w: bool,
        qqspace: MemSpace,
        easpace: MemSpace,
        next_word: u32,
    ) {
        self.set_cycles(2);

        let qq_addr = 0xFFFF80 + qq_offset as u32;
        self.set_inst_len(inst_len_for_ea(ea_mode));

        let (ea_addr, is_immediate) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        if w {
            // Write to qqspace:qq peripheral
            if is_immediate {
                self.write_mem(qqspace, qq_addr, ea_addr);
            } else {
                let val = self.read_mem_dyn(easpace, ea_addr);
                self.write_mem(qqspace, qq_addr, val);
            }
        } else {
            // Read from qqspace:qq -> write to easpace:ea_addr
            let val = self.read_mem(qqspace, qq_addr);
            self.write_mem_dyn(easpace, ea_addr, val);
        }
    }

    pub(super) fn emit_movep_qq_pea(
        &mut self,
        qq_offset: u8,
        ea_mode: u8,
        w: bool,
        space: MemSpace,
        next_word: u32,
    ) {
        self.set_cycles(6);

        let qq_addr = 0xFFFF80 + qq_offset as u32;
        self.set_inst_len(inst_len_for_ea(ea_mode));

        let (ea_addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        if w {
            // Write to peripheral: P:ea -> qq
            let val = self.read_mem_dyn(MemSpace::P, ea_addr);
            self.write_mem(space, qq_addr, val);
        } else {
            // Read from peripheral: qq -> P:ea
            let val = self.read_mem(space, qq_addr);
            self.write_mem_dyn(MemSpace::P, ea_addr, val);
        }
    }

    pub(super) fn emit_movep_qq_r(&mut self, qq_offset: u8, reg_idx: u8, w: bool, space: MemSpace) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let qq_addr = 0xFFFF80 + qq_offset as u32;
        let reg_idx = reg_idx as usize;

        if w {
            // Write to peripheral: reg -> qq
            let val = self.read_reg_for_move(reg_idx);
            self.write_mem(space, qq_addr, val);
        } else {
            // Read from peripheral: qq -> reg
            let val = self.read_mem(space, qq_addr);
            self.write_reg_for_move(reg_idx, val);
        }
    }

    pub(super) fn emit_movec_imm(&mut self, imm: u8, dest: u8) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let val = self.builder.ins().iconst(types::I32, imm as i64);
        self.write_reg_for_move(dest as usize, val);
    }

    pub(super) fn emit_movec_reg(&mut self, src_reg: u8, dst_reg: u8, w: bool) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let src_reg = src_reg as usize;
        let dst_reg = dst_reg as usize;
        if w {
            // Write: D1 <- S1
            let val = self.read_reg_for_move(src_reg);
            self.write_reg_for_move(dst_reg, val);
        } else {
            // Read: S1 -> D1
            let val = self.read_reg_for_move(dst_reg);
            self.write_reg_for_move(src_reg, val);
        }
    }

    pub(super) fn emit_movec_ea(
        &mut self,
        ea_mode: u8,
        numreg: u8,
        w: bool,
        space: MemSpace,
        next_word: u32,
    ) {
        self.set_cycles(1);
        self.set_inst_len(inst_len_for_ea(ea_mode));
        let dst_reg = numreg as usize;

        let (addr, is_imm) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        if w {
            // Read from memory/immediate -> control register
            if is_imm {
                // Immediate mode: addr is the immediate value
                self.write_reg_for_move(dst_reg, addr);
            } else {
                let val = self.read_mem_dyn(space, addr);
                self.write_reg_for_move(dst_reg, val);
            }
        } else {
            // Write control register -> memory
            let val = self.read_reg_for_move(dst_reg);
            self.write_mem_dyn(space, addr, val);
        }
    }

    pub(super) fn emit_movec_aa(&mut self, addr: u8, numreg: u8, w: bool, space: MemSpace) {
        self.set_inst_len(1);
        self.set_cycles(1);
        let addr = addr as u32;
        let dst_reg = numreg as usize;

        if w {
            // Read from absolute address -> control register
            let val = self.read_mem(space, addr);
            self.write_reg_for_move(dst_reg, val);
        } else {
            // Write control register -> absolute address
            let val = self.read_reg_for_move(dst_reg);
            self.write_mem(space, addr, val);
        }
    }

    pub(super) fn emit_movem_ea(&mut self, ea_mode: u8, numreg: u8, w: bool, next_word: u32) {
        self.set_cycles(6);
        self.set_inst_len(inst_len_for_ea(ea_mode));
        let numreg = numreg as usize;

        let (addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        if w {
            // Read from P memory -> register
            let val = self.read_mem_dyn(MemSpace::P, addr);
            self.write_reg_for_move(numreg, val);
        } else {
            // Write register -> P memory
            let val = self.read_reg_for_move(numreg);
            self.write_mem_dyn(MemSpace::P, addr, val);
        }
    }

    pub(super) fn emit_movem_aa(&mut self, addr: u8, numreg: u8, w: bool) {
        self.set_inst_len(1);
        self.set_cycles(6);
        let numreg = numreg as usize;

        let addr_val = self.builder.ins().iconst(types::I32, addr as i64);
        if w {
            // Read P:addr -> register
            let val = self.read_mem_dyn(MemSpace::P, addr_val);
            self.write_reg_for_move(numreg, val);
        } else {
            // Write register -> P:addr
            let val = self.read_reg_for_move(numreg);
            self.write_mem_dyn(MemSpace::P, addr_val, val);
        }
    }

    pub(super) fn emit_parallel(
        &mut self,
        opcode: u32,
        move_type: ParallelMoveType,
        alu: &ParallelAlu,
        next_word: u32,
    ) {
        self.set_inst_len(1);
        self.set_cycles(1);

        match move_type {
            ParallelMoveType::Pm0 => self.emit_pm_0(opcode, alu, next_word),
            ParallelMoveType::Pm1 => self.emit_pm_1(opcode, alu, next_word),
            ParallelMoveType::Pm2 => self.emit_pm_2(opcode, alu),
            ParallelMoveType::Pm3 => self.emit_pm_3(opcode, alu),
            ParallelMoveType::Pm4 => self.emit_pm_4(opcode, alu, next_word),
            ParallelMoveType::Pm5 => self.emit_pm_5(opcode, alu, next_word),
            ParallelMoveType::Pm8 => self.emit_pm_8(opcode, alu),
        }
    }

    pub(super) fn emit_pm_0(&mut self, opcode: u32, alu: &ParallelAlu, next_word: u32) {
        let d_bit = (opcode >> 16) & 1;
        let memspace = MemSpace::xy((opcode >> 15) & 1);
        let ea_field = (opcode >> 8) & 0x3F;

        // Compute EA (may update address registers)
        let (ea_addr, _) = self.emit_calc_ea_ext(ea_field, next_word);

        // Mode 6 makes this a 2-word instruction
        if (ea_field >> 3) & 0x7 == 6 {
            self.set_inst_len(2);
        }

        let acc = reg::A + d_bit as usize;
        let acc_enum = if d_bit == 0 {
            Accumulator::A
        } else {
            Accumulator::B
        };
        let xy0_reg = if memspace == MemSpace::X {
            reg::X0
        } else {
            reg::Y0
        };

        // Save pre-ALU values
        let save_acc = self.read_reg_for_move(acc); // 24-bit limited
        let save_xy0 = self.load_reg(xy0_reg);
        let pre = self.load_acc(acc_enum);

        // Execute ALU
        self.emit_parallel_alu(alu);

        // Capture post-ALU accumulator
        let post = self.load_acc(acc_enum);

        // Write acc -> memory (pre-ALU value)
        self.write_mem_dyn(memspace, ea_addr, save_acc);

        // Write X0/Y0 -> acc (move's register write)
        self.write_reg_for_move(acc, save_xy0);

        // If ALU modified the accumulator, restore ALU result (ALU wins)
        let alu_mod = self.builder.ins().icmp(IntCC::NotEqual, pre, post);
        let cur = self.load_acc(acc_enum);
        let final_acc = self.builder.ins().select(alu_mod, post, cur);
        self.store_acc(acc_enum, final_acc);
    }

    pub(super) fn emit_pm_1(&mut self, opcode: u32, alu: &ParallelAlu, next_word: u32) {
        // C: emu_pm_1
        // Format:
        //   0001 ffdf w0mm mrrr - X: memory transfer
        //   0001 deff w1mm mrrr - Y: memory transfer
        let ea_field = (opcode >> 8) & 0x3F;
        let (ea_addr, is_imm) = self.emit_calc_ea_ext(ea_field, next_word);

        // Mode 6 makes this a 2-word instruction
        if (ea_field >> 3) & 0x7 == 6 {
            self.set_inst_len(2);
        }
        let memspace = MemSpace::xy((opcode >> 14) & 1);

        // Decode numreg1 (the register participating in memory transfer)
        let numreg1 = if memspace == MemSpace::Y {
            // Y space: bits 17:16
            match (opcode >> 16) & 3 {
                0 => reg::Y0,
                1 => reg::Y1,
                2 => reg::A,
                3 => reg::B,
                _ => unreachable!(),
            }
        } else {
            // X space: bits 19:18
            match (opcode >> 18) & 3 {
                0 => reg::X0,
                1 => reg::X1,
                2 => reg::A,
                3 => reg::B,
                _ => unreachable!(),
            }
        };

        let w = (opcode >> 15) & 1;

        // Read values BEFORE ALU
        let save_1 = if w == 1 {
            // Write D1: read from memory (or immediate if mode 6 + RRR!=0)
            if is_imm {
                ea_addr
            } else {
                self.read_mem_dyn(memspace, ea_addr)
            }
        } else {
            // Read S1: read from register
            self.read_reg_for_move(numreg1)
        };

        // S2: the other accumulator -> opposite register space
        let numreg2_src = if memspace == MemSpace::Y {
            // Y space: S2 is A or B -> bit 19
            reg::A + ((opcode >> 19) & 1) as usize
        } else {
            // X space: S2 is A or B -> bit 17
            reg::A + ((opcode >> 17) & 1) as usize
        };
        let save_2 = self.read_reg_for_move(numreg2_src);

        // Execute ALU
        self.emit_parallel_alu(alu);

        // Write D1
        if w == 1 {
            // Write memory value to register D1
            self.write_reg_for_move(numreg1, save_1);
        } else {
            // Write register S1 to memory
            self.write_mem_dyn(memspace, ea_addr, save_1);
        }

        // S2 -> D2: accumulator value -> opposite space register
        let numreg2_dst = if memspace == MemSpace::Y {
            // Y space: D2 is X0 or X1 -> bit 18
            reg::X0 + ((opcode >> 18) & 1) as usize
        } else {
            // X space: D2 is Y0 or Y1 -> bit 16
            reg::Y0 + ((opcode >> 16) & 1) as usize
        };
        self.store_reg(numreg2_dst, save_2);
    }

    pub(super) fn emit_pm_2(&mut self, opcode: u32, alu: &ParallelAlu) {
        if (opcode & 0xFFFF00) == 0x200000 {
            // NOP parallel move - just run ALU
            self.emit_parallel_alu(alu);
            return;
        }
        if (opcode & 0xFFE000) == 0x204000 {
            // R update via EA calc: 0010 0000 010m mrrr
            let ea_mode = (opcode >> 8) & 0x1F;
            let (_addr, _) = self.emit_calc_ea(ea_mode);
            self.emit_parallel_alu(alu);
            return;
        }
        if (opcode & 0xFFF000) == 0x202000 {
            // IFcc: 0010 0000 0010 CCCC alu
            // Execute ALU conditionally, never update CCR
            self.emit_ifcc(opcode, alu, false);
            return;
        }
        if (opcode & 0xFFF000) == 0x203000 {
            // IFcc.U: 0010 0000 0011 CCCC alu
            // Execute ALU conditionally, update CCR
            self.emit_ifcc(opcode, alu, true);
            return;
        }
        if (opcode & 0xFC0000) == 0x200000 {
            // Register-to-register: 0010 00ee eeed dddd
            self.emit_pm_2_2(opcode, alu);
            return;
        }
        // Fallthrough to pm_3 (immediate)
        self.emit_pm_3(opcode, alu);
    }

    pub(super) fn emit_pm_2_2(&mut self, opcode: u32, alu: &ParallelAlu) {
        let src_reg = ((opcode >> 13) & 0x1F) as usize;
        let dst_reg = ((opcode >> 8) & 0x1F) as usize;

        // Read source BEFORE ALU op
        let save_val = self.read_reg_for_move(src_reg);

        // Execute ALU op
        self.emit_parallel_alu(alu);

        // Write destination AFTER ALU op
        self.write_reg_for_move(dst_reg, save_val);
    }

    pub(super) fn emit_pm_3(&mut self, opcode: u32, alu: &ParallelAlu) {
        let dst_reg = ((opcode >> 16) & 0x1F) as usize;
        let mut imm_val = (opcode >> 8) & 0xFF;

        // For data registers and accumulators, shift immediate to bits 23:16
        match dst_reg {
            reg::X0 | reg::X1 | reg::Y0 | reg::Y1 | reg::A | reg::B => {
                imm_val <<= 16;
            }
            _ => {}
        }

        // Execute ALU first (pm_3 runs ALU before writing move result)
        self.emit_parallel_alu(alu);

        // Write immediate to register
        let v = self.builder.ins().iconst(types::I32, imm_val as i64);
        self.write_reg_for_move(dst_reg, v);
    }

    pub(super) fn emit_pm_4(&mut self, opcode: u32, alu: &ParallelAlu, next_word: u32) {
        // C: emu_pm_4 - if pattern is 0100_x0xx it's an L-memory move (pm_4x),
        // otherwise fall through to the standard X/Y move (pm_5).
        if (opcode & 0xF40000) == 0x400000 {
            self.emit_pm_4x(opcode, alu, next_word);
        } else {
            self.emit_pm_5(opcode, alu, next_word);
        }
    }

    pub(super) fn emit_pm_4x(&mut self, opcode: u32, alu: &ParallelAlu, next_word: u32) {
        // C: emu_pm_4x
        // Format:
        //   0100 l0ll w0aa aaaa   l:aa,D / S,l:aa
        //   0100 l0ll w1mm mrrr   l:ea,D / S,l:ea
        let ea_field = (opcode >> 8) & 0x3F;
        let numreg = ((opcode >> 16) & 0x3) | (((opcode >> 19) & 1) << 2);
        let w = (opcode >> 15) & 1;

        if (opcode >> 14) & 1 != 0 {
            // EA-based addressing (supports mode 6 absolute address)
            let (addr, _) = self.emit_calc_ea_ext(ea_field, next_word);
            // Mode 6 makes this a 2-word instruction
            if (ea_field >> 3) & 0x7 == 6 {
                self.set_inst_len(2);
            }

            if w == 1 {
                // Write D: mem -> register pair
                let save_lx = self.read_mem_dyn(MemSpace::X, addr);
                let save_ly = self.read_mem_dyn(MemSpace::Y, addr);
                self.emit_parallel_alu(alu);
                self.write_l_reg(numreg, save_lx, save_ly);
            } else {
                // Read S: register pair -> mem
                let (save_lx, save_ly) = self.read_l_reg(numreg);
                self.emit_parallel_alu(alu);
                self.write_mem_dyn(MemSpace::X, addr, save_lx);
                self.write_mem_dyn(MemSpace::Y, addr, save_ly);
            }
        } else {
            // Absolute 6-bit address
            let addr = ea_field;

            if w == 1 {
                let save_lx = self.read_mem(MemSpace::X, addr);
                let save_ly = self.read_mem(MemSpace::Y, addr);
                self.emit_parallel_alu(alu);
                self.write_l_reg(numreg, save_lx, save_ly);
            } else {
                let (save_lx, save_ly) = self.read_l_reg(numreg);
                self.emit_parallel_alu(alu);
                self.write_mem(MemSpace::X, addr, save_lx);
                self.write_mem(MemSpace::Y, addr, save_ly);
            }
        }
    }

    pub(super) fn emit_pm_5(&mut self, opcode: u32, alu: &ParallelAlu, next_word: u32) {
        let memspace = MemSpace::xy((opcode >> 19) & 1);
        let numreg_raw = ((opcode >> 16) & 0x7) | ((opcode >> 17) & (0x3 << 3));
        let numreg = numreg_raw as usize;
        let w = (opcode >> 15) & 1;
        let ea_field = (opcode >> 8) & 0x3F;

        if (opcode >> 14) & 1 == 0 {
            // Absolute address: ea_field is the address directly
            let addr = ea_field;

            if w == 1 {
                // Write D: mem -> reg
                let save_val = self.read_mem(memspace, addr);
                self.emit_parallel_alu(alu);
                self.write_reg_for_move(numreg, save_val);
            } else {
                // Read S: reg -> mem
                let save_val = self.read_reg_for_move(numreg);
                self.emit_parallel_alu(alu);
                self.write_mem(memspace, addr, save_val);
            }
        } else {
            // EA-based addressing
            let (addr, is_imm) = self.emit_calc_ea_ext(ea_field, next_word);

            // Mode 6 makes this a 2-word instruction
            if (ea_field >> 3) & 0x7 == 6 {
                self.set_inst_len(2);
            }

            if w == 1 {
                // Write D: mem -> reg (or immediate -> reg if is_imm)
                let save_val = if is_imm {
                    // Mode 6 with RRR!=0: addr IS the immediate value
                    addr
                } else {
                    self.read_mem_dyn(memspace, addr)
                };
                self.emit_parallel_alu(alu);
                self.write_reg_for_move(numreg, save_val);
            } else {
                // Read S: reg -> mem
                let save_val = self.read_reg_for_move(numreg);
                self.emit_parallel_alu(alu);
                self.write_mem_dyn(memspace, addr, save_val);
            }
        }
    }

    pub(super) fn emit_pm_8(&mut self, opcode: u32, alu: &ParallelAlu) {
        // Compute EA fields from the opcode encoding.
        let mut ea1 = (opcode >> 8) & 0x1F;
        if (ea1 >> 3) == 0 {
            ea1 |= 1 << 5;
        }
        let mut ea2 = ((opcode >> 13) & 0x3) | ((opcode >> 17) & (0x3 << 3));
        if (ea1 & (1 << 2)) == 0 {
            ea2 |= 1 << 2;
        }
        if (ea2 >> 3) == 0 {
            ea2 |= 1 << 5;
        }

        let (x_addr, _) = self.emit_calc_ea(ea1);
        let (y_addr, _) = self.emit_calc_ea(ea2);

        // Determine registers
        let numreg1 = match (opcode >> 18) & 0x3 {
            0 => reg::X0,
            1 => reg::X1,
            2 => reg::A,
            3 => reg::B,
            _ => unreachable!(),
        };
        let numreg2 = match (opcode >> 16) & 0x3 {
            0 => reg::Y0,
            1 => reg::Y1,
            2 => reg::A,
            3 => reg::B,
            _ => unreachable!(),
        };

        // Read sources BEFORE ALU
        let save_reg1 = if (opcode >> 15) & 1 == 1 {
            self.read_mem_dyn(MemSpace::X, x_addr)
        } else {
            self.read_reg_for_move(numreg1)
        };
        let save_reg2 = if (opcode >> 22) & 1 == 1 {
            self.read_mem_dyn(MemSpace::Y, y_addr)
        } else {
            self.read_reg_for_move(numreg2)
        };

        // Execute ALU
        self.emit_parallel_alu(alu);

        // Write results AFTER ALU
        if (opcode >> 15) & 1 == 1 {
            self.write_reg_for_move(numreg1, save_reg1);
        } else {
            self.write_mem_dyn(MemSpace::X, x_addr, save_reg1);
        }
        if (opcode >> 22) & 1 == 1 {
            self.write_reg_for_move(numreg2, save_reg2);
        } else {
            self.write_mem_dyn(MemSpace::Y, y_addr, save_reg2);
        }
    }

    pub(super) fn emit_move_long_disp(
        &mut self,
        space: MemSpace,
        w: bool,
        offreg_idx: u8,
        numreg: u8,
        next_word: u32,
    ) {
        self.set_inst_len(2);
        self.set_cycles(3);
        let offreg = reg::R0 + offreg_idx as usize;
        let numreg = numreg as usize;

        // Address = Rn + offset
        let rn = self.load_reg(offreg);
        let offset = self.builder.ins().iconst(types::I32, next_word as i64);
        let addr = self.builder.ins().iadd(rn, offset);
        let addr = self.mask24(addr);

        if w {
            let val = self.read_mem_dyn(space, addr);
            self.write_reg_for_move(numreg, val);
        } else {
            let val = self.read_reg_for_move(numreg);
            self.write_mem_dyn(space, addr, val);
        }
    }

    pub(super) fn emit_move_short_disp(
        &mut self,
        offset: u8,
        w: bool,
        offreg_idx: u8,
        numreg: u8,
        space: MemSpace,
    ) {
        self.set_inst_len(1);
        self.set_cycles(2);

        let offreg = reg::R0 + offreg_idx as usize;
        let numreg = numreg as usize;

        // Sign-extend 7-bit offset
        let sext_xxx = ((offset as i32) << 25) >> 25;
        let rn = self.load_reg(offreg);
        let off_val = self.builder.ins().iconst(types::I32, sext_xxx as i64);
        let addr = self.builder.ins().iadd(rn, off_val);
        let addr = self.mask24(addr);

        if w {
            // Read: mem -> register
            let val = self.read_mem_dyn(space, addr);
            self.write_reg_for_move(numreg, val);
        } else {
            // Write: register -> mem
            let val = self.read_reg_for_move(numreg);
            self.write_mem_dyn(space, addr, val);
        }
    }

    pub(super) fn emit_vsl(&mut self, s: Accumulator, ea_mode: u8, i_bit: u8, next_word: u32) {
        let mode = (ea_mode >> 3) & 0x7;
        self.set_inst_len(if mode == 6 { 2 } else { 1 });
        self.set_cycles(1);

        // Calculate EA (same address for both X and Y writes)
        let (addr, _) = self.emit_calc_ea_ext(ea_mode as u32, next_word);

        let acc = self.load_acc(s);

        // S[47:24] = A1/B1 -> write to X:ea
        let upper = self.extract_acc_mid(acc);
        self.write_mem_dyn(MemSpace::X, addr, upper);

        // (S[23:0] << 1) | i -> write to Y:ea
        let lower = self.extract_acc_lo(acc);
        let c1 = self.builder.ins().iconst(types::I32, 1);
        let shifted = self.builder.ins().ishl(lower, c1);
        let i_val = self.builder.ins().iconst(types::I32, i_bit as i64);
        let result = self.builder.ins().bor(shifted, i_val);
        let result = self.mask24(result);
        self.write_mem_dyn(MemSpace::Y, addr, result);
        // CCR: unchanged
    }

    pub(super) fn emit_lua(&mut self, ea_mode: u8, dst_reg: u8) {
        self.set_inst_len(1);
        self.set_cycles(3);
        let srcreg = (ea_mode & 0x7) as usize;
        let dstreg = (dst_reg & 0x7) as usize;

        // Save Rn, calc EA (which updates Rn), capture new Rn, restore old
        let saved_rn = self.load_reg(reg::R0 + srcreg);
        let (_addr, _) = self.emit_calc_ea(ea_mode as u32);
        let new_rn = self.load_reg(reg::R0 + srcreg);
        self.store_reg(reg::R0 + srcreg, saved_rn);

        if dst_reg & 8 != 0 {
            self.store_reg(reg::N0 + dstreg, new_rn);
        } else {
            self.store_reg(reg::R0 + dstreg, new_rn);
        }
    }

    pub(super) fn emit_lua_rel(&mut self, aa: u8, addr_reg: u8, dst_reg: u8, dest_is_n: bool) {
        self.set_inst_len(1);
        self.set_cycles(3);

        // Sign-extend 7-bit offset
        let aa_signed = if aa & 0x40 != 0 {
            aa as i32 | !0x7F
        } else {
            aa as i32
        };

        let rn = self.load_reg(reg::R0 + addr_reg as usize);
        let off = self.builder.ins().iconst(types::I32, aa_signed as i64);
        let result = self.builder.ins().iadd(rn, off);
        let result = self.mask24(result);

        if dest_is_n {
            self.store_reg(reg::N0 + dst_reg as usize, result);
        } else {
            self.store_reg(reg::R0 + dst_reg as usize, result);
        }
    }

    pub(super) fn emit_lra_rn(&mut self, addr_reg: u8, dst_reg: u8, pc: u32) {
        self.set_inst_len(1);
        self.set_cycles(3);
        let rn = self.load_reg(reg::R0 + addr_reg as usize);
        let pc_val = self.builder.ins().iconst(types::I32, pc as i64);
        let result = self.builder.ins().iadd(rn, pc_val);
        let result = self.mask24(result);
        self.write_reg_for_move(dst_reg as usize, result);
    }

    pub(super) fn emit_lra_disp(&mut self, dst_reg: u8, pc: u32, next_word: u32) {
        self.set_inst_len(2);
        self.set_cycles(3);
        let target = mask_pc(pc.wrapping_add(next_word));
        let v = self.builder.ins().iconst(types::I32, target as i64);
        self.write_reg_for_move(dst_reg as usize, v);
    }
}
