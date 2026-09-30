use super::*;

#[test]
fn test_movec_imm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // movec #$42,M0 -> template 00000101iiiiiiii101ddddd
    // M0 = reg 0x20. Bits 5:0 = 100000. Bit 5 is the fixed '1'
    // in "101ddddd", so ddddd = 00000.
    // imm = 0x42, opcode = 00000101_01000010_101_00000 = 0x0542A0
    pram[0] = 0x0542A0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::M0], 0x42);
}

#[test]
fn test_movep_0_write() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::A1] = 0x000001;
    // movep A,x:$ffffc4 -> template 0000100sW1dddddd00pppppp
    // bits: 23:17=0000100, 16=s, 15=W, 14=1(fixed), 13:8=d, 7:6=00, 5:0=pp
    // s=0, W=1, d=0x0E(A), pp=4
    // = 0000100_0_1_1_001110_00_000100 = 0x08CE04
    pram[0] = 0x08CE04;
    run_one(&mut s, &mut jit);
    // x:$FFFFC4 -> periph[0xFFFFC4 - 0xFFFF80] = periph[68]
    assert_eq!(periph[68], 0x000001);
}

#[test]
fn test_parallel_move_reg_to_reg() {
    // pm_2_2: move X0,A + clr B
    // template 001000eeeeedddddaaaaaaaa, src=X0(0x04), dst=A(0x0E), alu=0x1B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x123456;
    s.registers[reg::B0] = 0xFF;
    s.registers[reg::B1] = 0xFF;
    s.registers[reg::B2] = 0xFF;
    pram[0] = 0x208E1B; // move X0,A + clr B
    run_one(&mut s, &mut jit);
    // A should have X0's value (via write_reg_for_move)
    assert_eq!(s.registers[reg::A1], 0x123456);
    assert_eq!(s.registers[reg::A0], 0);
    // B should be cleared by ALU op
    assert_eq!(s.registers[reg::B0], 0);
    assert_eq!(s.registers[reg::B1], 0);
    assert_eq!(s.registers[reg::B2], 0);
}

#[test]
fn test_parallel_move_imm() {
    // pm_3: move #$42,X0 (nop ALU)
    // template 001dddddiiiiiiiiaaaaaaaa, dst=X0(0x04), imm=0x42
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x244200;
    run_one(&mut s, &mut jit);
    // For data registers, immediate is shifted to bits 23:16
    // So X0 = 0x42 << 16 = 0x420000
    assert_eq!(s.registers[reg::X0], 0x420000);
}

#[test]
fn test_parallel_pm5_x_mem_read() {
    // pm_5 absolute: move X:$03,X0 + clr A
    // Format: 01dd 0ddd w0aa aaaa aaaa aaaa
    // X0 = 0x04 = 0b00_100: dd=00(bits 21:20), ddd=100(bits 18:16)
    // memspace=0(bit 19), W=1(bit 15), absolute(bit 14=0), addr=$03(bits 13:8)
    // ALU = 0x13 (clr A)
    // opcode = 0100_0100_1000_0011_0001_0011 = 0x448313
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[3] = 0xABCDEF;
    pram[0] = 0x448313;
    run_one(&mut s, &mut jit);
    // X0 should get value from X:$03
    assert_eq!(s.registers[reg::X0], 0xABCDEF);
    // A should be cleared
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::A2], 0);
}

#[test]
fn test_parallel_pm5_x_mem_write() {
    // pm_5 absolute: move A,X:$05 + clr B
    // A = 0x0E = 0b01_110: dd=01(bits 21:20), ddd=110(bits 18:16)
    // memspace=0(bit 19), W=0(bit 15=write), absolute(bit 14=0), addr=$05(bits 13:8)
    // ALU = 0x1B (clr B)
    // opcode = 0101_0110_0000_0101_0001_1011 = 0x56051B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x56051B;
    run_one(&mut s, &mut jit);
    // X:$05 should get A1 value (read_reg_for_move reads A1 for accumulator A)
    assert_eq!(xram[5], 0x123456);
}

#[test]
fn test_parallel_pm5_y_mem_read() {
    // pm_5 absolute: move Y:$02,A + nop
    // A = 0x0E: dd=01, ddd=110, memspace=1(Y, bit 19), W=1, addr=$02
    // opcode = 0101_1110_1000_0010_0000_0000 = 0x5E8200
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[2] = 0x654321;
    pram[0] = 0x5E8200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x654321);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_move_x_long_read() {
    // move X:(R1+xxxx),X0  W=1
    // encoding: 0000101001110RRR1WDDDDDD
    // R1(RRR=001), W=1, D=X0(0x04): 0x0A71C4
    // Note: must use R1+ (not R0) to avoid decode collision with jclr_ea
    // (MMM=110, RRR=0 is "absolute short" addressing in jclr_ea)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0000;
    xram[5] = 0xABCDEF;
    pram[0] = 0x0A71C4; // move X:(R1+xxxx),X0
    pram[1] = 0x000005; // offset = 5
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xABCDEF);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_move_x_long_write() {
    // move X0,X:(R1+xxxx)  W=0
    // R1(RRR=001), W=0, D=X0(0x04): 0x0A7184
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0000;
    s.registers[reg::X0] = 0x123456;
    pram[0] = 0x0A7184; // move X0,X:(R1+xxxx)
    pram[1] = 0x000003; // offset = 3
    run_one(&mut s, &mut jit);
    assert_eq!(xram[3], 0x123456);
}

#[test]
fn test_move_x_imm_read() {
    // move X:(R0+1),X0: template 0000001aaaaaaRRR1a0WDDDD
    // offset=1, RRR=0 (R0), W=1 (read), DDDD=0x4 (X0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000005;
    xram[0x06] = 0xABCDEF; // R0+1 = 6
    pram[0] = 0x0200D4;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xABCDEF);
}

#[test]
fn test_move_x_imm_write() {
    // move X0,X:(R0+2): same template, W=0 (write), offset=2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000003;
    s.registers[reg::X0] = 0x123456;
    pram[0] = 0x020884;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x05], 0x123456); // R0+2 = 5
}

#[test]
fn test_move_y_imm_read() {
    // move Y:(R0+1),X0: template 0000001aaaaaaRRR1a1WDDDD
    // offset=1, RRR=0 (R0), W=1 (read), DDDD=0x4 (X0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000002;
    yram[0x03] = 0x654321; // R0+1 = 3
    pram[0] = 0x0200F4;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x654321);
}

#[test]
fn test_movem_aa_read() {
    // movem P:$10,X0: template 00000111W0aaaaaa00dddddd
    // W=1 (read), aaaaaa=0x10, dddddd=0x04 (X0)
    // 0000_0111_1001_0000_0000_0100 = 0x079004
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0x10] = 0xDEADBE;
    pram[0] = 0x079004;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xDEADBE);
}

#[test]
fn test_movem_aa_write() {
    // movem X0,P:$10: W=0 (write)
    // 0000_0111_0001_0000_0000_0100 = 0x071004
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xCAFE00;
    pram[0] = 0x071004;
    run_one(&mut s, &mut jit);
    assert_eq!(pram[0x10], 0xCAFE00);
}

#[test]
fn test_movep_x_qq_write() {
    // movep X:(R0),X:$FFFF85: template 00000111W1MMMRRR0Sqqqqqq
    // W=1 (write to qq), MMM=100, RRR=000, S=0, qqqqqq=0x05 (-> $FFFF85)
    // 0000_0111_1110_0000_0000_0101 = 0x07E005
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0xABCDEF; // value at X:(R0)
    pram[0] = 0x07E005;
    run_one(&mut s, &mut jit);
    // X:$FFFF85 = periph[$FFFF85 - $FFFF80] = periph[5]
    assert_eq!(periph[5], 0xABCDEF);
}

#[test]
fn test_movep_x_qq_read() {
    // movep X:$FFFF85,X:(R0): W=0 (read from qq)
    // 0000_0111_0110_0000_0000_0101 = 0x076005
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = 0x000010;
    periph[5] = 0x123456; // periph value at $FFFF85
    pram[0] = 0x076005;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x123456);
}

#[test]
fn test_movec_write_ssh() {
    // movec X0,SSH - should push X0 onto stack
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x1234;
    // movec_reg pattern: 00000100W1eeeeee101ddddd
    // W=1 (write to ctrl reg), eeeeee=X0(0x04), ddddd=SSH[4:0]=0x1C
    // bits[7:0] = 101_11100 = 0xBC, bits[15:8] = 11_000100 = 0xC4
    // = 0x04C4BC
    pram[0] = 0x04C4BC;
    run_one(&mut s, &mut jit);
    // SP should have incremented
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
    // stack[0][1] should have X0 value
    assert_eq!(s.stack[0][1], 0x1234);
    // SSH register should reflect the pushed value
    assert_eq!(s.registers[reg::SSH], 0x1234);
}

#[test]
fn test_movec_read_ssh() {
    // movec SSH,X0 - should pop from stack
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push a value onto stack first
    s.registers[reg::SP] = 1;
    s.stack[0][1] = 0xABCD;
    s.stack[1][1] = 0x0000;
    s.registers[reg::SSH] = 0xABCD;
    s.registers[reg::SSL] = 0x0000;
    // movec_reg pattern: 00000100W1eeeeee101ddddd
    // W=0 (read from ctrl reg), eeeeee=X0(0x04), ddddd=SSH[4:0]=0x1C
    // bits[7:0] = 101_11100 = 0xBC, bits[15:8] = 01_000100 = 0x44
    // = 0x0444BC
    pram[0] = 0x0444BC;
    run_one(&mut s, &mut jit);
    // X0 should have the popped value
    assert_eq!(s.registers[reg::X0], 0xABCD);
    // SP should have decremented
    assert_eq!(s.registers[reg::SP] & 0xF, 0);
}

#[test]
fn test_movec_write_sp() {
    // movec X0,SP - should update SP and recompute SSH/SSL
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Pre-populate stack so SSH/SSL get recomputed
    s.stack[0][2] = 0x1111;
    s.stack[1][2] = 0x2222;
    s.registers[reg::X0] = 2; // SP value to write
    // movec_reg: W=1, eeeeee=X0(0x04), ddddd=SP[4:0]=0x1B
    // SP = reg::SP = 0x3B, ddddd = 0x1B, bits[7:0] = 101_11011 = 0xBB
    // bits[15:8] = 11_000100 = 0xC4 -> 0x04C4BB
    pram[0] = 0x04C4BB;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SP] & 0xF, 2);
    assert_eq!(s.registers[reg::SSH], 0x1111);
    assert_eq!(s.registers[reg::SSL], 0x2222);
}

#[test]
fn test_pm3_imm_to_r0() {
    // PM3: #$42,R0 with NOP ALU
    // bits 23:20 = 0011 (PM3), bits 19:16 = R0 low nibble = 0
    // bits 15:8 = 0x42, bits 7:0 = 0x00 (NOP)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x304200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::R0], 0x42);
}

#[test]
fn test_pm3_imm_to_n0_with_alu() {
    // PM3: #$10,N0 with CLR A
    // N0 = reg 0x18, bits 19:16 = 0x18 & 0xF = 8
    // bits 23:20 = 0011, bits 19:16 = 1000 -> opcode prefix = 0x38
    // bits 15:8 = 0x10 (imm), bits 7:0 = 0x13 (CLR A)
    // opcode = 0x381013
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x381013;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::N0], 0x10);
    // CLR A should have cleared A
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A2], 0);
}

#[test]
fn test_movec_write_a0() {
    // MOVEC M0,A0: W=0, eeeeee=A0=0x08, ddddd=M0[4:0]=0x00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::M0] = 0x123456;
    pram[0] = 0x0448A0;
    run_one(&mut s, &mut jit);
    // A0 should have the value (masked to 24 bits)
    assert_eq!(s.registers[reg::A0], 0x123456);
}

#[test]
fn test_movec_write_a2() {
    // MOVEC M0,A2: W=0, eeeeee=A2=0x0A, ddddd=0x00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::M0] = 0x42;
    pram[0] = 0x044AA0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0x42);
}

#[test]
fn test_movec_write_b0() {
    // MOVEC M0,B0: W=0, eeeeee=B0=0x09, ddddd=0x00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::M0] = 0xABCDEF;
    pram[0] = 0x0449A0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B0], 0xABCDEF);
}

#[test]
fn test_movec_write_b2() {
    // MOVEC M0,B2: W=0, eeeeee=B2=0x0B, ddddd=0x00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::M0] = 0xFF;
    pram[0] = 0x044BA0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B2], 0xFF);
}

#[test]
fn test_movec_read_a0() {
    // MOVEC A0,M0: W=1, eeeeee=A0=0x08, ddddd=0x00
    // M0 is 24-bit, so use a value that fits.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0x1234;
    pram[0] = 0x04C8A0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::M0], 0x1234);
}

#[test]
fn test_pm0_x_read_write() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM0: move a,x:(r0)+ x0,a (ALU=NOP)
    // d=0(A), S=0(X), ea=(r0)+ = mode 011_000
    pram[0] = 0x080000 | (0b011_000 << 8);
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x334455;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x222222;
    s.registers[reg::R0] = 0x000010;
    s.registers[reg::N0] = 1;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x222222);
    assert_eq!(xram[0x10], 0x334455);
    assert_eq!(s.registers[reg::R0], 0x000011);
}

#[test]
fn test_pm1_x_read() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM1: X read + reg-to-reg transfer
    // 0x100000 | (1<<15) | (0x19<<8): read X:(r1)+n1 -> X0, move A -> Y0
    pram[0] = 0x100000 | (1 << 15) | (0x19 << 8);
    s.registers[reg::R1] = 0x000010;
    s.registers[reg::N1] = 0x000001;
    xram[0x10] = 0x123456;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x654321;
    s.registers[reg::A0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::X0], 0x123456);
    assert_eq!(s.registers[reg::Y0], 0x654321);
    assert_eq!(s.registers[reg::R1], 0x000011);
}

#[test]
fn test_pm1_y_read() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM1 Y read: read Y:(r1)+n1 -> Y0, move A1 -> X0
    pram[0] = 0x100000 | (1 << 15) | (1 << 14) | (0x19 << 8);
    s.registers[reg::R1] = 0x000010;
    s.registers[reg::N1] = 0x000001;
    yram[0x10] = 0xABCDEF;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x112233;
    s.registers[reg::A0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::Y0], 0xABCDEF);
    assert_eq!(s.registers[reg::X0], 0x112233);
    assert_eq!(s.registers[reg::R1], 0x000011);
}

#[test]
fn test_pm4x() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM4x: L-memory write (mem->reg) at abs $0010, X regs, tst A
    // Opcode from pm4x_opcode(L_X=2, true, None, ALU_TST_A=0x03)
    pram[0] = 0x429003;
    xram[0x10] = 0xAAAAAA;
    yram[0x10] = 0xBBBBBB;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::X1], 0xAAAAAA);
    assert_eq!(s.registers[reg::X0], 0xBBBBBB);
}

#[test]
fn test_pm8() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM8: move x:(r0)+n0,x0 y:(r4),y0
    pram[0] = 0xC08800;
    s.registers[reg::R0] = 0x000010;
    s.registers[reg::N0] = 0x000001;
    s.registers[reg::R4] = 0x000020;
    s.registers[reg::N4] = 0x000001;
    xram[0x10] = 0x111111;
    yram[0x20] = 0x222222;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::X0], 0x111111);
    assert_eq!(s.registers[reg::Y0], 0x222222);
    assert_eq!(s.registers[reg::R0], 0x000011);
}

#[test]
fn test_movep_1() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    // movep x:$ffffc0,p:(r0) -- periph to P-space
    // pp=0 maps to $ffffc0 = PERIPH_BASE+64
    pram[0] = 0x086040;
    s.registers[reg::R0] = 0x000042;
    periph[64] = 0xABCDEF;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(pram[0x42], 0xABCDEF);
}

#[test]
fn test_movec_aa() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // movec x:$0010,m0 -- read from abs addr to control reg
    pram[0] = 0x059020;
    xram[0x10] = 0x000007;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::M0], 0x000007);

    // movec m0,x:$0010 -- write control reg to abs addr
    pram[1] = 0x051020;
    s.registers[reg::M0] = 0x000003;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(xram[0x10], 0x000003);
}

#[test]
fn test_pm0_b_y_nop() {
    // PM0 with d=1 (B acc), Y space, NOP ALU -- covers Accumulator::B and Y0 paths
    // Format: 0000_100d_xSmm_mrrr_aaaa_aaaa
    // d=1, x=1(Y), S=0, ea=0x20(R0 no-update), ALU=NOP
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x09A000; // PM0: Y:(R0),B NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::B1] = 0x123456;
    s.registers[reg::B0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::Y0] = 0xABCDEF;
    run_one(&mut s, &mut jit);
    // PM0: B_limited -> Y:(R0), Y0 -> B
    assert_eq!(yram[0x10], 0x123456);
    assert_eq!(s.registers[reg::B1], 0xABCDEF);
}

#[test]
fn test_pm0_b_y_tfr() {
    // PM0 with d=1, Y space, TFR A,B -- covers ALU modification detection path
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x09A009; // PM0: Y:(R0)+,B  TFR A,B (alu=0x09)
    s.registers[reg::R0] = 0x10;
    s.registers[reg::Y0] = 0x999999;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x654321;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // TFR A,B wins over the move: B = A = 0x00_123456_000000
    assert_eq!(s.registers[reg::B1], 0x123456);
    assert_eq!(s.registers[reg::B2], 0);
    // Memory gets pre-ALU B value (limited)
    assert_eq!(yram[0x10], 0x654321);
}

#[test]
fn test_pm1_y_space_y1() {
    // PM1 Y space: Y1 -> Y:(R0), A -> X0. Covers Y1 numreg1 + A numreg2_src.
    // bits 23:20=0001, bit19=0(A src), bit18=0(X0 dst), bit17:16=01(Y1), bit15=0(w), bit14=1(Y)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x116000; // PM1 Y: Y1->Y:(R0), A->X0, NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::Y1] = 0x111111;
    s.registers[reg::A1] = 0x222222;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(yram[0x10], 0x111111); // Y1 written to Y memory
    assert_eq!(s.registers[reg::X0], 0x222222); // A limited -> X0
}

#[test]
fn test_pm1_x_space_x1_b() {
    // PM1 X space: X1 -> X:(R0), B -> Y0. Covers X1 numreg1 + B numreg2_src.
    // bits 19:18=01(X1), bit17=1(B src), bit16=0(Y0 dst), bit15=0(w), bit14=0(X)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x162000; // PM1 X: X1->X:(R0), B->Y0, NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::X1] = 0x333333;
    s.registers[reg::B1] = 0x444444;
    s.registers[reg::B0] = 0;
    s.registers[reg::B2] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x333333); // X1 written to X memory
    assert_eq!(s.registers[reg::Y0], 0x444444); // B limited -> Y0
}

#[test]
fn test_pm1_y_read_a_b() {
    // PM1 Y space read: Y:(R0)->A, B->X1. Covers A/B numreg1 + B numreg2_src.
    // bit19=1(B src), bit18=1(X1 dst), bit17:16=10(A numreg1), bit15=1(w read), bit14=1(Y)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x1EE000; // PM1 Y: Y:(R0)->A, B->X1, NOP
    s.registers[reg::R0] = 0x10;
    yram[0x10] = 0x555555;
    s.registers[reg::B1] = 0x666666;
    s.registers[reg::B0] = 0;
    s.registers[reg::B2] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x555555); // Y mem -> A
    assert_eq!(s.registers[reg::X1], 0x666666); // B limited -> X1
}

#[test]
fn test_pm4x_b10_read() {
    // PM4X read L:$10 -> B10 (numreg=1). Covers write_l_reg case 1 (B10).
    // numreg=1: bits 17:16=01, bit19=0. w=1, absolute (bit14=0).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x419000; // PM4X: L:$10->B10, NOP
    xram[0x10] = 0xAAAA00;
    yram[0x10] = 0xBBBB00;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0xAAAA00);
    assert_eq!(s.registers[reg::B0], 0xBBBB00);
}

#[test]
fn test_pm4x_y_read() {
    // PM4X read L:$10 -> Y (numreg=3). Covers read/write Y pair.
    // numreg=3: bits 17:16=11, bit19=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x439000; // PM4X: L:$10->Y, NOP
    xram[0x10] = 0xCC0000;
    yram[0x10] = 0xDD0000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::Y1], 0xCC0000);
    assert_eq!(s.registers[reg::Y0], 0xDD0000);
}

#[test]
fn test_pm4x_x_write() {
    // PM4X write X pair -> L:$10 (numreg=2, w=0). Covers read_l_reg X + write path.
    // numreg=2: bits 17:16=10, bit19=0. w=0, absolute.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x421000; // PM4X: X->L:$10, NOP
    s.registers[reg::X1] = 0x112233;
    s.registers[reg::X0] = 0x445566;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x112233);
    assert_eq!(yram[0x10], 0x445566);
}

#[test]
fn test_pm4x_a_limited_ea() {
    // PM4X read L:(R0) -> A limited (numreg=4, EA-based). Covers read_l_reg case 4 + EA path.
    // numreg=4: bits 17:16=00, bit19=1. w=1, EA (bit14=1).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // 0x400000 | (1<<19) | (1<<15) | (1<<14) | (0x20<<8) = 0x48E000
    pram[0] = 0x48E000; // PM4X: L:(R0)->A(limited), NOP
    s.registers[reg::R0] = 0x10;
    xram[0x10] = 0x112233;
    yram[0x10] = 0x445566;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x112233);
    assert_eq!(s.registers[reg::A0], 0x445566);
}

#[test]
fn test_pm4x_b_limited_ea() {
    // PM4X read L:(R0) -> B limited (numreg=5). Covers read_l_reg case 5 (B limited).
    // numreg=5: bits 17:16=01, bit19=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x49E000; // PM4X: L:(R0)->B(limited), NOP
    s.registers[reg::R0] = 0x10;
    xram[0x10] = 0x778899;
    yram[0x10] = 0xAABBCC;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x778899);
    assert_eq!(s.registers[reg::B0], 0xAABBCC);
}

#[test]
fn test_pm4x_ab_read() {
    // PM4X read L:$10 -> AB (numreg=6). Covers read_l_reg case 6 (AB).
    // numreg=6: bits 17:16=10, bit19=1. w=1, absolute.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x4A9000; // PM4X: L:$10->AB, NOP
    xram[0x10] = 0x100000;
    yram[0x10] = 0x200000;
    run_one(&mut s, &mut jit);
    // AB: A1=lx(X mem), B1=ly(Y mem). Both sign-extended to 56-bit.
    assert_eq!(s.registers[reg::A1], 0x100000);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_pm4x_ba_read() {
    // PM4X read L:$10 -> BA (numreg=7). Covers read_l_reg case 7 (BA).
    // numreg=7: bits 17:16=11, bit19=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x4B9000; // PM4X: L:$10->BA, NOP
    xram[0x10] = 0x300000;
    yram[0x10] = 0x400000;
    run_one(&mut s, &mut jit);
    // BA: B1=lx(X mem), A1=ly(Y mem)
    assert_eq!(s.registers[reg::B1], 0x300000);
    assert_eq!(s.registers[reg::A1], 0x400000);
}

#[test]
fn test_pm4x_a_write() {
    // PM4X write A -> L:$10 (numreg=4, w=0). Covers write_l_reg case 4 (A full).
    // numreg=4: bits 17:16=00, bit19=1. w=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x481000; // PM4X: A->L:$10, NOP
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::A0] = 0x789ABC;
    run_one(&mut s, &mut jit);
    // A (limited): lx=A1 limited, ly depends on limiting
    assert_eq!(xram[0x10], 0x123456); // A1 not limited (A2=0, value positive)
}

#[test]
fn test_pm4x_b_write() {
    // PM4X write B -> L:$10 (numreg=5, w=0). Covers write_l_reg case 5 (B full).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x491000; // PM4X: B->L:$10, NOP
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x654321;
    s.registers[reg::B0] = 0x111111;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x654321); // B1 limited
}

#[test]
fn test_pm4x_ab_write() {
    // PM4X write AB -> L:$10 (numreg=6, w=0). Covers read_l_reg case 6 (AB pair).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x4A1000; // PM4X: AB->L:$10, NOP
    // Use values where bit 23=0 so read_accu24 doesn't limit
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x3A0000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x5B0000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // AB write: lx=read_accu24(A), ly=read_accu24(B)
    assert_eq!(xram[0x10], 0x3A0000);
    assert_eq!(yram[0x10], 0x5B0000);
}

#[test]
fn test_pm4x_ba_write() {
    // PM4X write BA -> L:$10 (numreg=7, w=0). Covers write_l_reg case 7 (BA pair).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x4B1000; // PM4X: BA->L:$10, NOP
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x110000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x220000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // BA write: lx=read_accu24(B), ly=read_accu24(A)
    assert_eq!(xram[0x10], 0x220000); // B -> X
    assert_eq!(yram[0x10], 0x110000); // A -> Y
}

#[test]
fn test_pm8_x1_y1() {
    // PM8 with X1 and Y1 registers (read from memory).
    // bits 19:18=01(X1), 17:16=01(Y1), bit22=1(Y mem->reg), bit15=1(X mem->reg)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0xC58800;
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 1;
    s.registers[reg::R4] = 0x20;
    s.registers[reg::N4] = 1;
    xram[0x10] = 0xA1A1A1;
    yram[0x20] = 0xB2B2B2;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X1], 0xA1A1A1);
    assert_eq!(s.registers[reg::Y1], 0xB2B2B2);
}

#[test]
fn test_pm8_write_to_mem() {
    // PM8 write registers to memory (both w bits = 0).
    // bits 19:18=00(X0), 17:16=00(Y0), bit22=0(Y reg->mem), bit15=0(X reg->mem)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x800800; // PM8 write X0->X:(ea), Y0->Y:(ea)
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 1;
    s.registers[reg::R4] = 0x20;
    s.registers[reg::N4] = 1;
    s.registers[reg::X0] = 0x111111;
    s.registers[reg::Y0] = 0x222222;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x111111);
    assert_eq!(yram[0x20], 0x222222);
}

#[test]
fn test_pm8_a_b_regs() {
    // PM8 with A and B accumulators (read from memory).
    // bits 19:18=10(A), 17:16=11(B), bit22=1, bit15=1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0xCB8800; // PM8: X:(ea)->A, Y:(ea)->B
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 1;
    s.registers[reg::R4] = 0x20;
    s.registers[reg::N4] = 1;
    xram[0x10] = 0x100000;
    yram[0x20] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x100000);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_pm2_r_update() {
    // PM2 R-update: 0010 0000 010m mrrr + ALU NOP
    // ea mode 3 (R0+): bits 12:8 = 11000 = 0x18
    // Covers emit_pm_2 R-update EA path
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x205800; // PM2 R-update: (R0)+, NOP
    s.registers[reg::R0] = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::R0], 0x11); // R0 incremented
}

#[test]
fn test_pm4x_a10_abs_write() {
    // PM4X write A10 -> L:$10 (numreg=0, w=0, abs). Covers read_l_reg case 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x401000; // PM4X: A10->L:$10, NOP (numreg=0, w=0, abs)
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x111111;
    s.registers[reg::A0] = 0x222222;
    run_one(&mut s, &mut jit);
    // A10 write: lx=A1, ly=A0
    assert_eq!(xram[0x10], 0x111111);
    assert_eq!(yram[0x10], 0x222222);
}

#[test]
fn test_pm4x_a10_abs_read() {
    // PM4X read L:$10 -> A10 (numreg=0, w=1, abs). Covers write_l_reg case 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x409000; // PM4X: L:$10->A10, NOP (numreg=0, w=1, abs)
    s.registers[reg::A2] = 0x42; // should be preserved
    xram[0x10] = 0x333333; // lx -> A1
    yram[0x10] = 0x444444; // ly -> A0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x333333);
    assert_eq!(s.registers[reg::A0], 0x444444);
    assert_eq!(s.registers[reg::A2], 0x42); // A2 preserved
}

#[test]
fn test_pm4x_y_abs_write() {
    // PM4X write Y -> L:$10 (numreg=3, w=0, abs). Covers read_l_reg case 3.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x431000; // PM4X: Y->L:$10, NOP (numreg=3, w=0, abs)
    s.registers[reg::Y1] = 0x555555;
    s.registers[reg::Y0] = 0x666666;
    run_one(&mut s, &mut jit);
    // Y write: lx=Y1, ly=Y0
    assert_eq!(xram[0x10], 0x555555);
    assert_eq!(yram[0x10], 0x666666);
}

#[test]
fn test_pm4x_b10_abs_write() {
    // PM4X write B10 -> L:$10 (numreg=1, w=0, abs). Covers read_l_reg case 1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x411000; // PM4X: B10->L:$10, NOP (numreg=1, w=0, abs)
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x777777;
    s.registers[reg::B0] = 0x888888;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x777777);
    assert_eq!(yram[0x10], 0x888888);
}

#[test]
fn test_pm4x_ea_read_b10() {
    // PM4X read L:(R0)+ -> B10 (numreg=1, w=1, EA). Covers PM4X EA read path.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // numreg=1 (B10): bits 19=0, 17:16=01. w=1: bit15=1. EA: bit14=1.
    // ea_field = 011_000 = 0x18 ((R0)+). bits 13:8 = 0x18.
    pram[0] = 0x41D800; // PM4X: L:(R0)+->B10, NOP
    s.registers[reg::R0] = 0x08;
    s.registers[reg::B2] = 0x55;
    xram[0x08] = 0xAAAAAA;
    yram[0x08] = 0xBBBBBB;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0xAAAAAA);
    assert_eq!(s.registers[reg::B0], 0xBBBBBB);
    assert_eq!(s.registers[reg::B2], 0x55); // preserved
    assert_eq!(s.registers[reg::R0], 0x09); // post-increment
}

#[test]
fn test_pm4x_ea_write_a10() {
    // PM4X write A10 -> L:(R0)+ (numreg=0, w=0, EA). Covers PM4X EA write path.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // numreg=0 (A10): bits 19=0, 17:16=00. w=0: bit15=0. EA: bit14=1.
    // ea_field = 011_000 = 0x18 ((R0)+). bits 13:8 = 0x18.
    pram[0] = 0x405800; // PM4X: A10->L:(R0)+, NOP
    s.registers[reg::R0] = 0x08;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0xCCCCCC;
    s.registers[reg::A0] = 0xDDDDDD;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x08], 0xCCCCCC);
    assert_eq!(yram[0x08], 0xDDDDDD);
    assert_eq!(s.registers[reg::R0], 0x09);
}

#[test]
fn test_pm1_y_space_a_reg() {
    // PM1 Y space with numreg1=A (value 2). Covers line 4948-4949.
    // Y space (bit 14=1), numreg1=2->A (bits 17:16=10), w=0 (write A to Y mem),
    // bit 19=1 (B->numreg2_src), bit 18=0 (X0 dest for numreg2_dst)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x1A6000; // PM1 Y: A->Y:(R0), B->X0, NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x1A2B3C;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x4D5E6F;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(yram[0x10], 0x1A2B3C); // A limited -> Y mem
    assert_eq!(s.registers[reg::X0], 0x4D5E6F); // B limited -> X0
}

#[test]
fn test_pm1_x_space_a_reg() {
    // PM1 X space with numreg1=A (value 2). Covers lines 4957.
    // X space (bit 14=0), bits 19:18 select numreg1: 10=A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bits 19:18=10 -> numreg1=A, bit14=0 (X space), bit17=0 (A->Y0), bit15=0 (write)
    pram[0] = 0x1A2000; // PM1 X: A->X:(R0), B->Y0, NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x2A3B4C;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x5D6E7F;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x2A3B4C); // A limited -> X mem
    assert_eq!(s.registers[reg::Y0], 0x5D6E7F); // B limited -> Y0
}

#[test]
fn test_pm1_x_space_b_reg() {
    // PM1 X space with numreg1=B (value 3). Covers lines 4958-4959.
    // X space, bits 19:18 = 11 = B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bits 19:18 = 11 -> B, bit17=0(A->Y0), bit16=0(Y0), bit15=0(write), bit14=0(X)
    pram[0] = 0x1E2000; // PM1 X: B->X:(R0), A->Y0, NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x3B4C5D;
    s.registers[reg::B0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x6E7F80;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x3B4C5D);
}

#[test]
fn test_pm8_b_x_a_y() {
    // PM8 with numreg1=B(3), numreg2=A(2). Covers lines 5354, 5360.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // numreg1 = (opcode >> 18) & 3 = 3 -> B
    // numreg2 = (opcode >> 16) & 3 = 2 -> A
    // bits 19:16 = 1110 -> byte = 0xE
    // Both directions read: bit15=1 (X read), bit22=1 (Y read)
    pram[0] = 0xCE8800; // PM8: X:(R0),B Y:(R4),A NOP
    s.registers[reg::R0] = 0x10;
    s.registers[reg::R4] = 0x20;
    xram[0x10] = 0x123456;
    yram[0x20] = 0x789ABC;
    run_one(&mut s, &mut jit);
    // B gets X mem value (sign-extended), A gets Y mem value
    assert_eq!(s.registers[reg::B1], 0x123456);
    assert_eq!(s.registers[reg::A1], 0x789ABC);
}

#[test]
fn test_pm8_ea_mode_low() {
    // PM8 where ea1 has mode bits = 0, triggering ea1 |= 1<<5 (line 5336)
    // ea1 = (opcode >> 8) & 0x1F. Need ea1 bits 4:3 = 00, e.g. ea1 = 0x01 (mode 0, R1)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // ea1=0x01 -> (opcode >> 8) & 0x1F = 0x01 -> mode 0, R1
    // After ea1 |= 1<<5 -> ea1 = 0x21 -> mode 4, R1 -> (R1) no update
    // Need move_bits >= 8 and valid PM8 format
    // bits 23:20 = 0xC (PM8), bits 19:18 = 00 (X0), bits 17:16 = 00 (Y0)
    // bit 15 = 1 (X read), bit 22 = 1 (Y read)
    // ea1 bits at opcode bits 12:8 = 00001 -> low nibble of byte 1 = 0x01
    pram[0] = 0xC08100; // PM8 X0/Y0 with ea1 mode=0 R1
    s.registers[reg::R1] = 0x10;
    s.registers[reg::R4] = 0x20;
    xram[0x10] = 0xAABBCC;
    yram[0x20] = 0xDDEEFF;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xAABBCC);
}

#[test]
fn test_movem_ea_write() {
    // movem X0,P:(R0) -- W=0 write register to P memory via EA
    // Pattern: 00000111W1MMMRRR10dddddd
    // W=0, MMMRRR=100000 (mode 4, R0), dddddd=000100 (X0)
    // Bits 23:16=0x07, 15:8=0_1_100000=0x60, 7:0=10_000100=0x84
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x076084;
    s.registers[reg::X0] = 0xABCDEF;
    s.registers[reg::R0] = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(pram[0x10], 0xABCDEF);
}

#[test]
fn test_movec_ea_imm() {
    // movec #imm,M0 -- mode 6 with RRR=4, w=1 reads immediate value
    // Pattern: 00000101W1MMMRRR0s1ddddd
    // W=1, MMMRRR=110100 (mode 6, RRR=4 -> immediate), s=0, ddddd=00000 (M0)
    // M0=reg 0x20. Bits 5:0 = 1_00000. Bit 5 is the fixed '1'.
    // Byte0: 0_0_1_00000=0x20, Byte1: 1_1_110100=0xF4, Byte2: 0x05
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x05F420;
    pram[1] = 0x001234; // immediate value (M0 is 16-bit)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::M0], 0x1234);
}

#[test]
fn test_pm1_y_space_b_write() {
    // PM1 Y space write with numreg1=3 -> reg::B
    // Base: 0x1A6009 has bits 17:16=10 (numreg1=A). Change to 11 (numreg1=B).
    // 0x1A6009 | (1 << 16) = 0x1B6009
    // w=0 (write B->Y mem), EA mode 4 R0, alu_byte=0x09 (TFR A,B)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x1B6000; // PM1 Y write with numreg1=B, NOP ALU
    s.registers[reg::R0] = 0x05;
    s.registers[reg::B1] = 0x3ABBCC; // bit 23=0, B2=0 -> no limiting
    run_one(&mut s, &mut jit);
    assert_eq!(yram[0x05], 0x3ABBCC);
}

#[test]
fn test_pm1_mode6_imm_x0() {
    // move #$055556,x0 b,y0 - 2-word Pm1 with mode 6 immediate
    // Opcode: 0x12B400, extension: 0x055556
    // Verifies: PC advances by 2, immediate value is loaded (not zero)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x12B400;
    pram[1] = 0x055556;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x123456;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2, "mode 6 Pm1 must advance PC by 2");
    assert_eq!(
        s.registers[reg::X0],
        0x055556,
        "immediate from extension word"
    );
    assert_eq!(s.registers[reg::Y0], 0x123456, "b,y0 parallel move");
}

#[test]
fn test_pm1_mode6_imm_x1() {
    // move #$ABCDEF,x1 a,y1 - 2-word Pm1 with mode 6, X1 destination
    // Opcode: 0x15B400, extension: 0xABCDEF
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x15B400;
    pram[1] = 0xABCDEF;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x777777;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::X1], 0xABCDEF);
    assert_eq!(s.registers[reg::Y1], 0x777777);
}

#[test]
fn test_pm1_mode6_abs_read() {
    // move x:$0100,x0 a,y0 - 2-word Pm1 with mode 6 absolute address
    // Opcode: 0x10B000, extension: 0x000100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x10B000;
    pram[1] = 0x000100;
    xram[0x100] = 0xFEDCBA;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x333333;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::X0], 0xFEDCBA, "read from x:$0100");
    assert_eq!(s.registers[reg::Y0], 0x333333, "a,y0 parallel move");
}

#[test]
fn test_pm1_mode6_abs_write() {
    // move a,x:$0100 b,y0 - 2-word Pm1 with mode 6 absolute write
    // Opcode: 0x1A3000, extension: 0x000100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x1A3000;
    pram[1] = 0x000100;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x555555;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x333333;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(xram[0x100], 0x555555, "a written to x:$0100");
    assert_eq!(s.registers[reg::Y0], 0x333333, "b,y0 parallel move");
}

#[test]
fn test_pm1_mode6_sequence() {
    // Verify the instruction after a 2-word Pm1 mode 6 executes correctly
    // (i.e. PC=2 points to the right instruction, not the extension word)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Instruction 0: move #$055556,x0 b,y0 (2 words)
    pram[0] = 0x12B400;
    pram[1] = 0x055556;
    // Instruction 1: INC A (at PC=2)
    pram[2] = 0x000008;
    // Instruction 2: JMP $3 (halt)
    pram[3] = 0x0C0003;
    s.registers[reg::B1] = 0x111111;
    s.run(&mut jit, 10);
    assert_eq!(s.registers[reg::X0], 0x055556);
    assert_eq!(s.registers[reg::Y0], 0x111111);
    assert_eq!(s.registers[reg::A0], 1, "INC A at PC=2 must execute");
}

#[test]
fn test_pm0_mode6_abs_write() {
    // move a,x:$0100 x0,a - 2-word Pm0 with mode 6 absolute address
    // Opcode: 0x083000, extension: 0x000100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x083000;
    pram[1] = 0x000100;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x654321;
    s.registers[reg::A0] = 0;
    s.registers[reg::X0] = 0xABCDEF;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2, "mode 6 Pm0 must advance PC by 2");
    assert_eq!(xram[0x100], 0x654321, "a written to x:$0100");
    assert_eq!(s.registers[reg::A1], 0xABCDEF, "x0 loaded into a");
}

#[test]
fn test_movep_23_periph_read() {
    // movep_23 with w=0: read from peripheral -> write to EA
    // Pattern: 0000100sW1MMMRRR1Spppppp
    // s=0 (X periph), W=0 (read), MMMRRR=100000 (mode 4, R0),
    // S=0 (X ea space), pppppp=0x00 -> periph addr 0xFFFFC0
    // byte 2: 0x08, byte 1: 0_1_100000=0x60, byte 0: 10_000000=0x80
    // 0x086080
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    periph[0x40] = 0xBEEF00; // X:$FFFFC0 at periph[$FFFFC0 - $FFFF80] = periph[64]
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    pram[0] = 0x086080;
    s.registers[reg::R0] = 0x05;
    run_one(&mut s, &mut jit);
    // Read X:0xFFFFC0 (periph[0]) -> write to X:(R0)=X:5
    assert_eq!(xram[0x05], 0xBEEF00);
}

#[test]
fn test_movec_write_ssl() {
    // movec X0,SSL: write X0 to SSL via jit_write_ssl
    // movec_reg: 00000100W1eeeeee101ddddd
    // W=1, eeeeee=X0(0x04), ddddd=SSL[4:0]=0x1D
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SP] = 1; // need valid stack frame
    s.registers[reg::X0] = 0xABCD;
    pram[0] = 0x04C4BD;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SSL], 0xABCD);
    assert_eq!(s.stack[1][1], 0xABCD);
}

#[test]
fn test_pm5_ea_read() {
    // MOVE X:(R0)+,X0 NOP -- PM4 dispatching to PM5 (not PM4x pattern)
    // bits[23:20]=0100, bit14=1 (EA), w=1 (read), MMMRRR=100000 (mode 4 R0)
    // numreg_raw = bits[18:16] | (bits[21:20]<<3) = 100|00 = 4 = X0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x44E000;
    s.registers[reg::R0] = 5;
    xram[5] = 0x123456;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x123456);
}

#[test]
fn test_movep_x_qq_imm_write() {
    // movep #imm,X:qq -- mode 6 RRR=4, W=1 writes immediate to X:qq
    // Pattern: 00000111W1MMMRRR0Sqqqqqq
    // W=1, MMMRRR=110100, S=0, qq=0x20 -> x_addr=0xFFFFA0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    pram[0] = 0x07F420; // movep #imm,X:$FFFFA0
    pram[1] = 0xABCDE0; // immediate value
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    // periph[$FFFFA0 - $FFFF80] = periph[0x20]
    assert_eq!(periph[0x20], 0xABCDE0);
}

#[test]
fn test_movep_1_mode6_write() {
    // movep P:$10,X:$FFC0 -- mode 6 RRR=0, W=1, reads P mem, writes to periph
    // Pattern: 0000100sW1MMMRRR01pppppp
    // s=0, W=1, MMMRRR=110000 (mode 6, RRR=0), pp=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    pram[0] = 0x08F040; // movep P:ea,X:$FFFFC0
    pram[1] = 0x000010; // absolute P address
    pram[0x10] = 0x123456; // P memory content
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    // pp_addr = $FFFFC0, periph[$FFFFC0 - $FFFF80] = periph[0x40]
    assert_eq!(periph[0x40], 0x123456);
}

#[test]
fn test_movep_23_ea_write() {
    // movep X:(R0)+,X:$FFC0 -- EA read, write to periph (w=1, non-immediate)
    // Pattern: 0000100sW1MMMRRR1Spppppp
    // s=0, W=1, MMMRRR=100000 (mode 4, R0), S=0, pp=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    pram[0] = 0x08E080;
    s.registers[reg::R0] = 5;
    xram[5] = 0xABCDEF;
    run_one(&mut s, &mut jit);
    // periph[$FFFFC0 - $FFFF80] = periph[0x40]
    assert_eq!(periph[0x40], 0xABCDEF);
}

#[test]
fn test_movep_1_writes_p_block() {
    // Standalone Movep1 W=0 compiled via s.run() to trigger writes_p_memory
    // check in emit_block (line 575/690).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    periph[0x40] = 0xCAFE00; // X:$FFFFC0 source value
    s.registers[reg::R0] = 0x20;
    // Movep1 W=0: read X:$FFFFC0 -> write P:(R0)=P:$20
    pram[0] = 0x086040;
    pram[1] = 0x0C0001; // JMP $1 (halt)
    s.run(&mut jit, 100);
    assert_eq!(pram[0x20], 0xCAFE00);
}

#[test]
fn test_pm8_ea2_high_mode() {
    // PM8 where ea1 bit 2 = 1 and ea2 bits 4:3 != 0 (covers lines 5341, 5344)
    // ea1 = (opcode>>8)&0x1F = 0x1C -> mode 3 (Rn+), R4 -> ea1 bit 2 = 1
    // move_bits = 0xB (11) -> ea2 bits 4:3 from opcode bits 21:20 = 01 -> (ea2>>3) != 0
    // bit 15 = 1 (X read: mem->reg), bit 22 = 0 (Y write: reg->mem)
    // numreg1 = X0, numreg2 = Y0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0xB09C00; // PM8: X:(R4)+->X0, Y0->Y:(R0)+, NOP
    // ea1 = 0x1C -> mode 3 (Rn+), R4.  ea2 = 0x18 -> mode 3 (Rn+), R0.
    s.registers[reg::R4] = 5;
    s.registers[reg::M4] = 0xFFFFFF;
    s.registers[reg::R0] = 10;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[5] = 0xABCDEF;
    s.registers[reg::Y0] = 0x111111;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xABCDEF);
    assert_eq!(yram[10], 0x111111);
}

#[test]
fn test_move_y_long_read() {
    // move Y:(R1+xxxx),X0: template 0000101101110RRR1WDDDDDD
    // RRR=1, W=1 (read from mem), D=X0(0x04)
    // 0000_1011_0111_0001_1100_0100 = 0x0B71C4
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0000;
    yram[5] = 0xDEADBE;
    pram[0] = 0x0B71C4;
    pram[1] = 0x000005; // offset = 5
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xDEADBE);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_move_y_long_write() {
    // move X0,Y:(R1+xxxx): W=0 (write to mem)
    // 0000_1011_0111_0001_1000_0100 = 0x0B7184
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0000;
    s.registers[reg::X0] = 0xCAFE01;
    pram[0] = 0x0B7184;
    pram[1] = 0x000003;
    run_one(&mut s, &mut jit);
    assert_eq!(yram[3], 0xCAFE01);
}

#[test]
fn test_movep_qq_r_read() {
    // movep X:$FFFF80,X0: template 00000100W1dddddd1q0qqqqq
    // W=1 (read from periph), d=X0(0x04), qq=0
    // 0000_0100_1100_0100_1000_0000 = 0x04C480
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    periph[0] = 0xBEEF01; // $FFFF80 = periph[0]
    pram[0] = 0x044480; // W=0 (read from periph to reg)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xBEEF01);
}

#[test]
fn test_movep_qq_r_write() {
    // movep X0,X:$FFFF80: W=1 (write reg to periph)
    // 0000_0100_1100_0100_1000_0000 = 0x04C480
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::X0] = 0xFACE01;
    pram[0] = 0x04C480; // W=1 (write reg to periph)
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0], 0xFACE01);
}

#[test]
fn test_movep_y_qq_r_read() {
    // movep Y:$FFFF80,X0: template 00000100W1dddddd0q1qqqqq
    // W=0 (read from periph to reg), d=X0(0x04), qq=0
    // 0000_0100_0100_0100_0010_0000 = 0x044420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.y_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    periph[0] = 0xF00D01;
    pram[0] = 0x044420; // W=0 (read from periph to reg)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0xF00D01);
}

#[test]
fn test_movep_y_qq_read() {
    // MovepQq (Y): 00000111W0MMMRRR1Sqqqqqq
    // W=0 (read Y:qq -> easpace:ea), MMM=100, RRR=000, S=0 (X space), qq=0
    // 0000_0111_0010_0000_1000_0000 = 0x072080
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.y_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    periph[0] = 0xDEAD01; // Y:$FFFF80
    pram[0] = 0x072080; // movep y:$FFFF80,x:(R0)
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0xDEAD01);
}

#[test]
fn test_movep_y_qq_write() {
    // MovepQq (Y): W=1 (easpace:ea -> Y:qq), MMM=100, RRR=000, S=0 (X), qq=0
    // 0000_0111_1010_0000_1000_0000 = 0x07A080
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.y_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0xBEEF02; // value at X:$0010
    pram[0] = 0x07A080; // movep x:(R0),y:$FFFF80
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0], 0xBEEF02);
}

#[test]
fn test_movep_qq_pea_write() {
    // MovepQqPea: 000000001WMMMRRR0sqqqqqq
    // W=1 (P:ea -> qq), MMM=100, RRR=000, s=0 (X space), qq=0
    // 0000_0000_1110_0000_0000_0000 = 0x00E000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = 0x0005;
    s.registers[reg::M0] = 0xFFFFFF;
    pram[5] = 0xCAFE03; // P:$0005
    pram[0] = 0x00E000; // movep p:(R0),x:$FFFF80
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0], 0xCAFE03);
}

#[test]
fn test_movep_qq_pea_read() {
    // MovepQqPea: W=0 (qq -> P:ea), MMM=100, RRR=000, s=0 (X), qq=0
    // 000000001_0_100_000_00_000000 = 0x00A000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = 0x0005;
    s.registers[reg::M0] = 0xFFFFFF;
    periph[0] = 0xFACE04; // X:$FFFF80
    pram[0] = 0x00A000; // movep x:$FFFF80,p:(R0)
    run_one(&mut s, &mut jit);
    assert_eq!(pram[5], 0xFACE04);
}

#[test]
fn test_plock_ea_nop() {
    // PlockEa: 0000101111MMMRRR10000001, MMM=100(Rn), RRR=0
    // 0x0BE081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0BE081; // plock (r0) (treated as NOP, 1-word)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_punlock_ea_nop() {
    // PunlockEa: 0000101011MMMRRR10000001, MMM=100(Rn), RRR=0
    // 0x0AE081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0AE081; // punlock (r0) (treated as NOP, 1-word)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_plock_ea_mode6_nop() {
    // PlockEa with absolute EA (mode 6): MMM=110, RRR=000
    // 0000101111110000_10000001 = 0x0BF081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0BF081; // plock $1234 (2-word, ea mode 6)
    pram[1] = 0x001234;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // 2-word instruction
}

#[test]
fn test_plockr() {
    // plockr xxxx: opcode 0x00000F, 2-word (next word = address)
    // Pipeline lock hint -- no-op in our implementation
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x00000F; // plockr
    pram[1] = 0x000100; // target address (ignored)
    let cycles = run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // 2-word instruction
    assert_eq!(cycles, 4); // 4 cycles per Table A-1
}

#[test]
fn test_punlockr() {
    // punlockr xxxx: opcode 0x00000E, 2-word
    // Pipeline unlock hint -- no-op in our implementation
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x00000E; // punlockr
    pram[1] = 0x000100; // target address (ignored)
    let cycles = run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(cycles, 4); // 4 cycles per Table A-1
}

#[test]
fn test_l_move_mode6_absolute_address() {
    // L: parallel move with mode 6 (absolute address) should use
    // the extension word as the memory address.
    //
    // Instruction: move l:$100,x (NOP ALU)
    // L: move format: 0100_l0ll w1mm_mrrr aaaa_aaaa
    //   numreg=2 (X: X1,X0): l=0,ll=10 -> bits [19]=0, [17:16]=10
    //   w=1 (read from memory)
    //   mmm=110 (mode 6, absolute), rrr=000
    //   Opcode: 0100 0010 1111 0000 = 0x42F000
    //   But we also need the ALU op in bits 7:0 = NOP = 0x00
    //   Full opcode: 0x42F000
    //   Extension word: 0x000100 (address $100)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    xram[0x100] = 0xAAAAAA; // X:$100 -> X1
    yram[0x100] = 0xBBBBBB; // Y:$100 -> X0

    pram[0] = 0x42F000; // move l:$100,x (mode 6, NOP ALU)
    pram[1] = 0x000100; // extension word: address $100

    run_one(&mut s, &mut jit);

    assert_eq!(
        s.registers[reg::X1],
        0xAAAAAA,
        "X1 should be loaded from X:$100"
    );
    assert_eq!(
        s.registers[reg::X0],
        0xBBBBBB,
        "X0 should be loaded from Y:$100"
    );
    assert_eq!(s.pc, 2, "mode 6 L: move is 2 words");
}

#[test]
fn test_plock_ea_updates_address_register() {
    // PLOCK ea should apply address register side effects.
    // PLOCK (R0)+  should increment R0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::R0] = 0x100;
    s.registers[reg::N0] = 1;
    s.registers[reg::M0] = 0xFFFFFF; // linear mode
    // PLOCK ea: template 0000101111MMMRRR10000001
    // (R0)+ = MMM=011, RRR=000 -> ea_mode = 0b011_000 = 0x18
    // Full opcode: 0000_1011_1101_1000_1000_0001 = 0x0BD881
    pram[0] = 0x0BD881;
    run_one(&mut s, &mut jit);

    assert_eq!(
        s.registers[reg::R0],
        0x101,
        "PLOCK (R0)+: R0 should be incremented by N0"
    );
}

#[test]
fn test_vsl() {
    // vsl A,(R0)+ (0000101S11MMMRRR110i0000, S=0 MMM=011 RRR=000 i=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    s.registers[reg::A0] = 0x123456;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0x00;
    pram[0] = 0x0AD8C0; // vsl A,(R0)+
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::R0], 0x0011);
}

#[test]
fn test_lua_rel() {
    // lua (R0+5),R1: template 0000010000aaaRRRaaaadddd
    // offset=5, RRR=0 (R0), dddd=0001 (R1)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0100;
    pram[0] = 0x040051; // lua (R0+$05),R1
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::R1], 0x0105);
    // R0 should be unchanged
    assert_eq!(s.registers[reg::R0], 0x0100);
}

#[test]
fn test_lua_ea() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // lua (r0)+,n0
    pram[0] = 0x045808;
    s.registers[reg::R0] = 0x000010;
    s.registers[reg::N0] = 0x000001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // (r0)+ computes r0+1 = 0x11, stores in N0; R0 unchanged
    assert_eq!(s.registers[reg::N0], 0x000011);
    assert_eq!(s.registers[reg::R0], 0x000010);
}

#[test]
fn test_lua_ea_n_dest() {
    // LUA (R0)+,N2. Covers N-register destination path in emit_lua.
    // Pattern: 00000100010MMRRR000ddddd, MM=01(post-inc), RRR=000(R0), ddddd=01010(N2)
    // bit3=1(N), bits2:0=010(N2)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // MM=01 -> bits 12:11 = 01. EA field for calc_ea = 01_000 = 8
    // Format: 00000100010MMRRR000ddddd
    // MM=01, RRR=000, ddddd=01010(N2)
    pram[0] = 0x04480A; // LUA (R0)+,N2
    s.registers[reg::R0] = 0x100;
    s.registers[reg::N0] = 1;
    run_one(&mut s, &mut jit);
    // LUA computes EA update, writes new R0 to N2, restores R0
    assert_eq!(s.registers[reg::N2], 0x101); // (R0)+ = R0+N0 = 0x101
    assert_eq!(s.registers[reg::R0], 0x100); // R0 unchanged
}

#[test]
fn test_lua_rel_n_dest_neg() {
    // LUA (R1 - 2),N0. Covers N-register dest + negative offset in emit_lua_rel.
    // Pattern: 0000010000aaaRRRaaaadddd
    // offset = -2 = 0x7E (7-bit: 1111110)
    // Upper 3 bits (bits 13:11) = 111, lower 4 bits (bits 7:4) = 1110
    // RRR=001 (R1), dddd=1000 (bit3=1 for N, bits2:0=000 for N0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // byte 2: 00000100 = 0x04
    // byte 1: 00_111_001 = 0x39 (bits15:14=00, bits13:11=111, bits10:8=001)
    // byte 0: 1110_1000 = 0xE8 (bits7:4=1110, bits3:0=1000)
    pram[0] = 0x0439E8; // LUA (R1-2),N0
    s.registers[reg::R1] = 0x100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::N0], 0xFE); // 0x100 - 2 = 0xFE
}

#[test]
fn test_lua_ea_r_dest() {
    // lua (R0)+,R2 -- bit 3 = 0 stores to R register instead of N register
    // Existing test_lua_ea uses N dest (bit 3=1). Change ddddd from 01000 to 00010.
    // Base opcode 0x045808 -> 0x045802 (ddddd=00010, bit3=0, dstreg=2 -> R2)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x045802;
    s.registers[reg::R0] = 0x000010;
    s.registers[reg::N0] = 0x000001;
    run_one(&mut s, &mut jit);
    // (R0)+ computes R0+1=0x11, stores in R2; R0 unchanged
    assert_eq!(s.registers[reg::R2], 0x000011);
    assert_eq!(s.registers[reg::R0], 0x000010);
}

#[test]
fn test_lra_rn() {
    // lra R0,N0: template 0000010011000RRR000ddddd
    // RRR=0 (R0), ddddd = N0 register index
    // N0 is reg 0x18 (24), so ddddd = 0x18 = 11000
    // 0000_0100_1100_0000_0001_1000 = 0x04C018
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0050;
    pram[0x10] = 0x04C018; // lra R0,N0
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    // N0 = (R0 + PC) & 0xFFFFFF = (0x50 + 0x10) & 0xFFFFFF = 0x60
    assert_eq!(s.registers[reg::N0], 0x60);
}

#[test]
fn test_lra_disp() {
    // lra xxxx,N0: template 0000010001000000010ddddd
    // ddddd = N0 (0x18) = 11000
    // 0000_0100_0100_0000_0101_1000 = 0x044058
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0x10] = 0x044058; // lra xxxx,N0
    pram[0x11] = 0x000020; // displacement
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    // N0 = (PC + displacement) & 0xFFFFFF = (0x10 + 0x20) & 0xFFFFFF = 0x30
    assert_eq!(s.registers[reg::N0], 0x30);
}

#[test]
fn test_movec_a_limiting_scale_up_positive() {
    // Scale-up limiting misses bit 47 (A1[23]) check.
    // A = $00:800000:000000: A2=0x00 (positive extension), A1=0x800000 (bit 23 set).
    // In scale-up mode, bits 55:46 = 00000000_10 (NOT uniform) -> must limit to $7FFFFF.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // MOVEC A,M0: template 00000100W1eeeeee101ddddd, W=1, e=A(0x0e), d=M0(0x00)
    pram[0] = 0x04CEA0;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    // Set scale-up mode: S1=1, S0=0 -> SR bit 11 set, bit 10 clear
    s.registers[reg::SR] |= 1 << 11; // S1
    s.registers[reg::SR] &= !(1 << 10); // S0=0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::M0],
        0x7FFFFF,
        "should limit to positive max"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_movec_a_limiting_scale_up_negative() {
    // Scale-up limiting, negative case.
    // B = $FF:7FFFFF:000000: B2=0xFF (negative extension), B1=0x7FFFFF (bit 23 clear).
    // In scale-up mode, bits 55:46 = 11111111_01 (NOT uniform) -> must limit to $800000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // MOVEC B,M0: template 00000100W1eeeeee101ddddd, W=1, e=B(0x0f), d=M0(0x00)
    pram[0] = 0x04CFA0;
    s.registers[reg::B2] = 0xFF;
    s.registers[reg::B1] = 0x7FFFFF;
    s.registers[reg::B0] = 0x000000;
    // Set scale-up mode: S1=1, S0=0
    s.registers[reg::SR] |= 1 << 11;
    s.registers[reg::SR] &= !(1 << 10);
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::M0],
        0x800000,
        "should limit to negative min"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_movec_a_no_limiting_scale_up_positive_ok() {
    // Verify no false positive: A = $00:200000:000000 in scale-up mode.
    // Bits 55:46 = 00000000_00 (uniform, A2=0, A1[23]=0, A1[22]=0) -> no limiting.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x04CEA0; // MOVEC A,M0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::SR] |= 1 << 11;
    s.registers[reg::SR] &= !(1 << 10);
    run_one(&mut s, &mut jit);
    // Scale-up: output = (0x00200000 << 1 | 0) & 0xFFFFFF = 0x400000
    assert_eq!(
        s.registers[reg::M0],
        0x400000,
        "no limiting, pass through scaled value"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "SR.L should NOT be set"
    );
}

#[test]
fn test_pm0_scale_up_limiting_positive() {
    // Parallel move A->X memory in scale-up mode should limit.
    // A = $00:800000:000000. In scale-up, bits 55:46 are NOT uniform -> limit to $7FFFFF.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // move a,x:(r0) - opcode 0x566000
    pram[0] = 0x566000;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0x000010;
    // Set scale-up mode: S1=1, S0=0
    s.registers[reg::SR] |= 1 << sr::S1;
    s.registers[reg::SR] &= !(1 << sr::S0);
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x7FFFFF, "should limit to positive max");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_pm0_scale_up_limiting_negative() {
    // Scale-up mode: B = $FF:7FFFFF:000000. Bits 55:46 NOT uniform -> limit to $800000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // move b,x0 - opcode 0x21E400
    pram[0] = 0x21E400;
    s.registers[reg::B2] = 0xFF;
    s.registers[reg::B1] = 0x7FFFFF;
    s.registers[reg::B0] = 0x000000;
    // Set scale-up mode: S1=1, S0=0
    s.registers[reg::SR] |= 1 << sr::S1;
    s.registers[reg::SR] &= !(1 << sr::S0);
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x800000,
        "should limit to negative min"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_pm0_scale_down_limiting_positive() {
    // Scale-down mode (S1=0, S0=1). A = $01:000000:000000.
    // Bits 55:48 = 00000001 - NOT uniform -> must limit to $7FFFFF.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // move a,x0 - opcode 0x21C400
    pram[0] = 0x21C400;
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    // Set scale-down mode: S1=0, S0=1
    s.registers[reg::SR] &= !(1 << sr::S1);
    s.registers[reg::SR] |= 1 << sr::S0;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x7FFFFF,
        "should limit to positive max"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_parallel_move_reads_before_alu() {
    // Parallel move source reads happen BEFORE ALU, writes happen AFTER.
    // clr a  a,x:(r0)+ - opcode 0x565813
    // Should read A (0x123456) for the move, THEN clear A.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x565813;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0x000010;
    s.registers[reg::N0] = 0x000001;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x123456, "pre-CLR value written to memory");
    assert_eq!(s.registers[reg::A1], 0x000000, "CLR executed");
    assert_eq!(s.registers[reg::A0], 0x000000, "CLR zeroed A0");
    assert_eq!(s.registers[reg::A2], 0x000000, "CLR zeroed A2");
    assert_eq!(s.registers[reg::R0], 0x000011, "R0 post-incremented");
}

#[test]
fn test_movec_x0_to_sr_sets_ccr() {
    // MOVEC X0,SR: writing X0 to SR should overwrite all CCR bits.
    // movec_reg: 00000100W1eeeeee101ddddd
    // W=1, eeeeee=000100 (X0), ddddd=11001 (SR[4:0]=0x19)
    // 0000_0100_1100_0100_1011_1001 = 0x04C4B9
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let ccr_mask = (1 << sr::C)
        | (1 << sr::V)
        | (1 << sr::Z)
        | (1 << sr::N)
        | (1 << sr::U)
        | (1 << sr::E)
        | (1 << sr::L)
        | (1 << sr::S);
    // Start with all CCR bits clear
    s.registers[reg::SR] &= !ccr_mask;
    // Write 0xFF to SR -> all CCR bits should be set
    s.registers[reg::X0] = 0x0000FF;
    pram[0] = 0x04C4B9; // movec x0,sr
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & ccr_mask,
        ccr_mask,
        "MOVEC X0,SR with X0=$FF should set all 8 CCR bits"
    );

    // Now clear all CCR bits
    s.registers[reg::X0] = 0x000000;
    pram[1] = 0x04C4B9; // movec x0,sr
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & ccr_mask,
        0,
        "MOVEC X0,SR with X0=$00 should clear all 8 CCR bits"
    );
}

#[test]
fn test_movec_sr_to_x0_reads_sr() {
    // MOVEC SR,X0: reading SR into X0 should capture current SR value.
    // movec_reg: 00000100W1eeeeee101ddddd
    // W=0, eeeeee=000100 (X0), ddddd=11001 (SR[4:0]=0x19)
    // 0000_0100_0100_0100_1011_1001 = 0x0444B9
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set some known SR value
    s.registers[reg::SR] = 0xC00300 | (1 << sr::C) | (1 << sr::Z) | (1 << sr::L);
    let expected_sr = s.registers[reg::SR];
    s.registers[reg::X0] = 0;
    pram[0] = 0x0444B9; // movec sr,x0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        expected_sr,
        "MOVEC SR,X0 should copy SR value to X0"
    );
}

#[test]
fn test_movem_aa_write_updates_pram() {
    // MOVEM X0,P:$20: write X0 to program memory at address $20.
    // movem aa write: 00000111W0aaaaaa00dddddd
    // W=0 (write to P memory), aaaaaa=0x20, dddddd=0x04 (X0)
    // 0000_0111_0010_0000_0000_0100 = 0x072004
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xABCDEF;
    pram[0] = 0x072004; // movem x0,p:$20
    run_one(&mut s, &mut jit);
    assert_eq!(pram[0x20], 0xABCDEF, "MOVEM should write X0 value to P:$20");
}

#[test]
fn test_pm1_scale_up_accumulator_read() {
    // Per DSP56300FM p.13-111: PM1 parallel move reads accumulator through the
    // data shifter/limiter. In scale-up mode (S1=1, S0=0), B2=0x00, B1=0x800000,
    // B0=0 triggers limiting: the scaled value overflows the 24-bit window, so
    // the limiter clamps to positive max (0x7FFFFF) and sets SR.L.
    //
    // Instruction: tfr b,a  b,y0 (0x21E601) - PM2 reg-to-reg move B->Y0 + TFR B,A.
    // The move reads B through read_reg_for_move (data shifter/limiter path).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Scale-up mode: S1=1, S0=0
    s.registers[reg::SR] = 0x0300 | (1 << sr::S1);
    // B = 0x00:800000:000000 - bit 23 of B1 is set, positive extension
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x800000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x21E601; // tfr b,a  b,y0
    run_one(&mut s, &mut jit);
    // Y0 should be limited to 0x7FFFFF (positive max in scale-up mode)
    assert_eq!(
        s.registers[reg::Y0],
        0x7FFFFF,
        "Scale-up accumulator read should limit to 0x7FFFFF"
    );
    // SR.L should be set by the limiter
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "SR.L should be set when limiting occurs"
    );
}

#[test]
fn test_movem_ssh_pop() {
    // Per DSP56300FM p.13-132: MOVEM with SSH as source should pop the stack
    // (decrement SP after reading SSH).
    // movem ssh,p:$20 = 0x07203C (from assembler listing)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push two values onto stack
    s.stack_push(0xAAAAAA, 0x111111); // SP=1
    s.stack_push(0xBBBBBB, 0x222222); // SP=2
    pram[0] = 0x07203C; // movem ssh,p:$20
    run_one(&mut s, &mut jit);
    // SP should decrement from 2 to 1 (pop from SSH read)
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should decrement after SSH read in MOVEM"
    );
}

#[test]
fn test_movep_ssh_pop() {
    // Per DSP56300FM p.13-134: MOVEP with SSH as source should pop the stack.
    // movep ssh,x:$ffffc0: template 0000100sW1dddddd00pppppp
    // s=0, W=1 (write to periph), dddddd=0x3C (SSH), pppppp=0x00
    // 0000_1000_1111_1100_0000_0000 = 0x08FC00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    // Push two values onto stack
    s.stack_push(0xAAAAAA, 0x111111); // SP=1
    s.stack_push(0xBBBBBB, 0x222222); // SP=2
    pram[0] = 0x08FC00; // movep ssh,x:$ffffc0
    run_one(&mut s, &mut jit);
    // SP should decrement from 2 to 1 (pop)
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should decrement after SSH read in MOVEP"
    );
    // Verify the value was written to the peripheral address
    // x:$FFFFC0 = periph[$FFFFC0 - $FFFF80] = periph[64]
    assert_eq!(
        periph[64], 0xBBBBBB,
        "MOVEP should write SSH value to peripheral"
    );
}

#[test]
fn test_vsl_accumulator_shift() {
    // VSL A,(R0)+ with i=1: writes A1 to X:ea, (A0 << 1 | i) to Y:ea.
    // Template: 0000101S11MMMRRR110i0000, S=0, MMM=011, RRR=000, i=1.
    // 0000_1010_1101_1000_1101_0000 = 0x0AD8D0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF; // linear modifier
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0AD8D0; // vsl A,(R0)+ with i=1
    run_one(&mut s, &mut jit);
    // X:$10 = A1 = 0x400000
    assert_eq!(xram[0x10], 0x400000, "VSL should write A1 to X:ea");
    // Y:$10 = (A0 << 1) | 1 = (0 << 1) | 1 = 0x000001
    assert_eq!(
        yram[0x10], 0x000001,
        "VSL should write (A0 << 1 | i) to Y:ea"
    );
    // R0 post-incremented
    assert_eq!(
        s.registers[reg::R0],
        0x0011,
        "R0 should be post-incremented"
    );
}

#[test]
fn test_move_short_disp_negative_offset() {
    // X:(R0 + xxx) with negative 7-bit offset (-3).
    // Template: 0000001aaaaaaRRR1a0WDDDD (p.13-115).
    // offset = -3 -> 7-bit two's complement = 0x7D = 0b1111101.
    // pack_xy_imm_offset: xxx_hi = (0x7D >> 1) & 0x3F = 0x3E, xxx_lo = 1.
    // RRR=0 (R0), W=1 (read), DDDD=0x4 (X0), s=0 (X space).
    // opcode = 0x020080 | (0x3E << 11) | (0 << 8) | (1 << 6) | 0 | 0x10 | 4
    //        = 0x020080 | 0x1F000 | 0x40 | 0x10 | 4 = 0x03F0D4
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x10;
    xram[0x0D] = 0x123456; // R0 + (-3) = 0x10 - 3 = 0x0D
    pram[0] = 0x03F0D4; // move X:(R0-3),X0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x123456,
        "X0 should read from X:(R0-3)"
    );
}

#[test]
fn test_movec_imm_ssh_push() {
    // MOVEC #imm,SSH - writing to SSH uses push semantics (p.13-130):
    // SP is pre-incremented, then value is written to stack[SP].
    // Template: 00000101iiiiiiii101ddddd, ddddd = SSH[4:0] = 0x1C.
    // imm = 0x30: 00000101_00110000_10111100 = 0x0530BC.
    // Verify two successive pushes stack correctly.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // First push: movec #$30,SSH
    pram[0] = 0x0530BC;
    // Second push: movec #$42,SSH = 00000101_01000010_10111100 = 0x0542BC
    pram[1] = 0x0542BC;
    run_one(&mut s, &mut jit); // first push
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should be 1 after first MOVEC #imm,SSH"
    );
    assert_eq!(
        s.stack[0][1], 0x30,
        "stack[0][1] should hold first pushed value (0x30)"
    );
    run_one(&mut s, &mut jit); // second push
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        2,
        "SP should be 2 after second MOVEC #imm,SSH"
    );
    assert_eq!(
        s.stack[0][2], 0x42,
        "stack[0][2] should hold second pushed value (0x42)"
    );
    // First value should still be at level 1.
    assert_eq!(
        s.stack[0][1], 0x30,
        "stack[0][1] should still hold first value after second push"
    );
}

#[test]
fn test_lra_negative_displacement() {
    // LRA xxxx,N0 with negative displacement.
    // Template: 0000010001000000010ddddd, ddddd = N0 (0x18 & 0x1F) = 0x18 = 11000.
    // 0000_0100_0100_0000_0101_1000 = 0x044058
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0x100] = 0x044058; // lra xxxx,N0
    pram[0x101] = 0xFFFFF0; // displacement = -16 (24-bit two's complement)
    s.pc = 0x100;
    run_one(&mut s, &mut jit);
    // N0 = (PC + displacement) & 0xFFFFFF = (0x100 + 0xFFFFF0) & 0xFFFFFF = 0xF0
    assert_eq!(
        s.registers[reg::N0],
        0xF0,
        "N0 should be 0xF0 from PC(0x100) + displacement(-16)"
    );
}

#[test]
fn test_pm3_immediate_to_accumulator() {
    // PM3: #$80,A with NOP ALU - 8-bit immediate to accumulator A (p.13-113).
    // For A/B destinations, immediate is shifted to bits 23:16, then sign-extended
    // to full 56-bit accumulator via write_reg_for_move.
    // Template: 001dddddiiiiiiiiaaaaaaaa, ddddd = A (0x0E & 0x1F) = 01110.
    // 0010_1110_1000_0000_0000_0000 = 0x2E8000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2E8000; // move #$80,A (NOP ALU)
    run_one(&mut s, &mut jit);
    // #$80 << 16 = 0x800000 placed at A1. Bit 23 = 1 -> sign-extended: A2=0xFF.
    assert_eq!(
        s.registers[reg::A1],
        0x800000,
        "A1 should be 0x800000 from #$80 << 16"
    );
    assert_eq!(
        s.registers[reg::A2],
        0xFF,
        "A2 should be 0xFF (sign-extended from bit 23 of A1)"
    );
    assert_eq!(s.registers[reg::A0], 0, "A0 should be 0");
}

#[test]
fn test_move_long_disp_accumulator_source() {
    // Move A,X:(R1+xxxx) - accumulator source with positive extension triggers limiting.
    // Template: 0000101001110RRR1WDDDDDD, RRR=001 (R1), W=0 (write to mem), D=A(0x0E).
    // 0000_1010_0111_0001_1000_1110 = 0x0A718E
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0000;
    // A2=0x01 (positive extension), A1=0, A0=0 -> positive overflow -> limit to 0x7FFFFF.
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0A718E; // move A,X:(R1+xxxx)
    pram[1] = 0x000005; // offset = 5
    run_one(&mut s, &mut jit);
    // Accumulator read with limiting: A2=0x01 overflows -> clamped to 0x7FFFFF.
    assert_eq!(
        xram[5], 0x7FFFFF,
        "limited positive overflow should write 0x7FFFFF"
    );
    // SR.L should be set after limiting.
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "SR.L should be set after accumulator limiting"
    );
}

#[test]
fn test_parallel_move_alu_reads_old_value() {
    // Per DSP56300FM Section 2.3.1 (p.2-14): In a parallel instruction, the ALU
    // reads its source registers before the move writes its destination. If the move
    // overwrites a register the ALU also reads, the ALU should see the OLD value.
    //
    // Instruction: add x0,a  x:(r0)+,x0  (PM5 read X:(R0)+ -> X0, ALU = ADD X0,A)
    // PM5 encoding: bits[23:20]=0100, bit19=0 (X), bits[18:16]=100 (X0),
    //   bit15=1 (w=read), bit14=1 (EA), bits[13:8]=011_000 (mode 3 = (R0)+),
    //   bits[7:0]=0x40 (add x0,a)
    // Opcode = 0x44D840.
    //
    // The move reads X:(R0)+ into X0 (new value = 0x100000).
    // ADD reads the OLD X0 (0x400000) and adds to A (0).
    // Result: A = 0x00_400000_000000 (old X0 sign-extended), X0 = 0x100000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // old X0 = 0.5 fractional
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 0x000001;
    s.registers[reg::M0] = 0xFFFFFF; // linear
    xram[0x10] = 0x100000; // new value that will overwrite X0 via the move
    pram[0] = 0x44D840; // add x0,a  x:(r0)+,x0
    run_one(&mut s, &mut jit);
    // ALU should have used old X0 (0x400000): A = 0 + 0x400000 (sign-ext) = 0x00_400000_000000
    assert_eq!(
        s.registers[reg::A1],
        0x400000,
        "A1 should reflect old X0 (0x400000), not new X0 from memory"
    );
    assert_eq!(
        s.registers[reg::A2],
        0x00,
        "A2 should be 0 (positive result)"
    );
    assert_eq!(s.registers[reg::A0], 0x000000, "A0 should be 0");
    // X0 should now hold the value read from memory (the move result)
    assert_eq!(
        s.registers[reg::X0],
        0x100000,
        "X0 should be overwritten by parallel move with mem value"
    );
    // R0 should have post-incremented
    assert_eq!(
        s.registers[reg::R0],
        0x11,
        "R0 should post-increment from 0x10 to 0x11"
    );
}

// NOTE: test_lra_ssh_pre_increment removed. DSP56300FM p.13-92 says "If D is SSH,
// the SP is pre-incremented by one", but the LRA encoding uses a 5-bit ddddd field
// (registers 0-31 only). SSH is register index 60, which cannot be encoded in 5 bits.
// The assembler accepts `lra $100,ssh` but produces an opcode targeting register 28
// (60 & 0x1F), not SSH. LRA to SSH is not a valid instruction on DSP56300.

#[test]
fn test_movec_a_limiting_scale_down_negative() {
    // DSP56300FM p.13-130: MOVEC A,reg - accumulator source through data shifter/limiter.
    // Scale-down mode (S1=0, S0=1). A = $FE:000000:000000 (negative extension).
    // Bits 55:48 = 0xFE (NOT all-1s) -> must limit to $800000 (negative max).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x04CEA0; // movec A,M0
    s.registers[reg::A2] = 0xFE;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    // Set scale-down mode: S1=0, S0=1
    s.registers[reg::SR] &= !(1 << sr::S1);
    s.registers[reg::SR] |= 1 << sr::S0;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::M0],
        0x800000,
        "should limit to negative max"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_movec_y_space_aa() {
    // DSP56300FM p.13-130: MOVEC Y:aa,M0 - read from Y-space absolute address.
    // movec_aa: 00000101W0aaaaaa0s1ddddd, W=1, aaaaaa=0x10, s=1 (Y), ddddd=00000 (M0).
    // 0000_0101_1001_0000_0110_0000 = 0x059060
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x10] = 0x654321;
    pram[0] = 0x059060; // movec y:$10,M0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::M0],
        0x654321,
        "MOVEC Y:aa should read from Y-space"
    );
}

#[test]
fn test_movec_ea_post_increment() {
    // DSP56300FM p.13-130: MOVEC X:(R0)+,M0 - ea read with post-increment.
    // movec_ea: 00000101W1MMMRRR0S1ddddd, W=1, MMM=011 ((R0)+), RRR=000, S=0, ddddd=00000 (M0).
    // 0000_0101_1101_1000_0010_0000 = 0x05D820
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000007;
    s.registers[reg::R0] = 0x10;
    pram[0] = 0x05D820; // movec x:(r0)+,M0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::M0],
        0x000007,
        "M0 should be loaded from X:(R0)"
    );
    assert_eq!(s.registers[reg::R0], 0x11, "R0 should be post-incremented");
}

#[test]
fn test_movem_ea_read_post_increment() {
    // DSP56300FM p.13-132: MOVEM P:(R0)+,X0 - read from program memory via EA.
    // Template: 00000111W1MMMRRR10dddddd, W=1, MMM=011 ((R0)+), RRR=000, dddddd=000100 (X0).
    // 0000_0111_1101_1000_1000_0100 = 0x07D884
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0x20] = 0xBEEF42;
    s.registers[reg::R0] = 0x20;
    pram[0] = 0x07D884; // movem p:(r0)+,x0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0xBEEF42,
        "X0 should be loaded from P:(R0)"
    );
    assert_eq!(s.registers[reg::R0], 0x21, "R0 should be post-incremented");
}

#[test]
fn test_movem_ea_mode6_absolute() {
    // DSP56300FM p.13-132: MOVEM with mode 6 absolute address (2-word instruction).
    // movem p:$100,x0: MMM=110, RRR=000 (mode 6 absolute).
    // Template: 00000111W1MMMRRR10dddddd, W=1, MMM=110, RRR=000, dddddd=000100 (X0).
    // 0000_0111_1111_0000_1000_0100 = 0x07F084
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0x100] = 0xCAFE42;
    pram[0] = 0x07F084; // movem p:xxxx,x0 (mode 6 absolute)
    pram[1] = 0x000100; // absolute address = 0x100
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0xCAFE42,
        "X0 should be loaded from P:$100"
    );
    assert_eq!(s.pc, 2, "PC should advance past 2-word instruction");
}

#[test]
fn test_pm0_scale_down_limiting_negative() {
    // DSP56300FM p.13-120: PM0 accumulator read - scale-down negative limiting.
    // A = $FE:000000:000000 (negative extension overflow).
    // Scale-down (S1=0, S0=1): bits 55:48 = 0xFE (not all-1s) -> limit to $800000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // move a,x0 - opcode 0x21C400
    pram[0] = 0x21C400;
    s.registers[reg::A2] = 0xFE;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::SR] &= !(1 << sr::S1);
    s.registers[reg::SR] |= 1 << sr::S0;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x800000,
        "should limit to negative max"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SR.L should be set");
}

#[test]
fn test_pm0_no_scaling_no_limit() {
    // DSP56300FM p.13-120: A = $00:7FFFFF:000000 in no-scaling mode.
    // A2=0x00, A1=0x7FFFFF - extension matches (all zeros), no limiting needed.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x21C400; // move a,x0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    // No scaling (S1=0, S0=0 - default)
    s.registers[reg::SR] &= !((1 << sr::S1) | (1 << sr::S0));
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x7FFFFF,
        "should NOT limit when A2=0 and A1=$7FFFFF"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "SR.L should NOT be set"
    );
}

#[test]
fn test_pm1_scale_down_accumulator_read() {
    // DSP56300FM p.13-125: PM1 accumulator read in scale-down mode.
    // move x:(r0)+,x0  a,y0 - PM1 with accumulator A -> Y0.
    // A = $01:000000:000000 (positive overflow). Scale-down -> limit to $7FFFFF.
    // PM1 encoding from existing test_pm1_scale_up_accumulator_read modified for scale-down.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM1: X:(R0)+,X0 A,Y0. From test_pm1_x_read: base opcode 0xC49800.
    // That reads x:(r0)+ -> x0, a -> y0 with nop ALU.
    pram[0] = 0x109800; // x:(r0)+,x0  a,y0  (nop ALU)
    s.registers[reg::R0] = 0x10;
    xram[0x10] = 0x123456;
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    // Set scale-down mode: S1=0, S0=1
    s.registers[reg::SR] &= !(1 << sr::S1);
    s.registers[reg::SR] |= 1 << sr::S0;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::Y0],
        0x7FFFFF,
        "scale-down should limit A to $7FFFFF"
    );
    assert_eq!(
        s.registers[reg::X0],
        0x123456,
        "X0 should be loaded from memory"
    );
}

#[test]
fn test_pm2_subreg_a0_to_x0() {
    // DSP56300FM p.13-115: PM2 register-to-register move. MOVE A0,X0 + NOP.
    // Template: 001000eeeeedddddaaaaaaaa.
    // src=A0(reg 0x08)=001000, dst=X0(reg 0x04)=000100, ALU=0x00 (NOP).
    // 0010_0010_0000_0100_0000_0000 = 0x220400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0x123456;
    pram[0] = 0x210400; // move A0,X0 + NOP
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x123456,
        "PM2: A0 value should be copied to X0"
    );
}

#[test]
fn test_pm5_accumulator_source_scaling() {
    // DSP56300FM p.13-118: PM5 accumulator source with scaling.
    // clr b  a,x:$10 - write A to X:$10 with NOP+CLR.
    // A = $01:000000:000000 (positive overflow). No scaling -> limit to $7FFFFF.
    // PM5 absolute write: bits[23:20]=0101, bit19=0(X), bits[18:16]=A(110),
    //   bit15=0(w=write), bit14=0(abs), bits[13:8]=addr, bits[7:0]=ALU.
    // opcode for 'clr b  a,x:$10' = 0x561013 (derived from test_parallel_move_reads_before_alu)
    // move a,x:$10 + nop -> 0x561000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // PM5 X write: 01dd_0ddd_w0aa_aaaa_aaaa_aaaa
    // D=A(0x0E): dd=01, ddd=110 -> bits[21:20]=01, bit19=0(X), bits[18:16]=110
    // w=0(write), abs addr=$10 -> bits[14:8]=0010000, ALU=0x00(nop).
    // 0101_0110_0001_0000_0000_0000 = 0x561000
    pram[0] = 0x561000; // move a,x:$10 + nop
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    // No scaling mode
    s.registers[reg::SR] &= !((1 << sr::S1) | (1 << sr::S0));
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x7FFFFF,
        "A with overflow should be limited to $7FFFFF"
    );
}

#[test]
fn test_pm5_y_space_write() {
    // DSP56300FM p.13-118: PM5 Y-space write.
    // move x0,y:$10 + nop.
    // PM5: bits[23:20]=01dd, bit19=1(Y), bits[18:16]=ddd.
    // X0=0x04: dd=00, ddd=100. w=0(write), bit19=1(Y-space), addr=$10.
    // 0100_1100_0001_0000_0000_0000 = 0x4C1000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xABCDEF;
    pram[0] = 0x4C1000; // move x0,y:$10 + nop
    run_one(&mut s, &mut jit);
    assert_eq!(yram[0x10], 0xABCDEF, "PM5: X0 should be written to Y:$10");
}

#[test]
fn test_pm8_accumulator_scaling() {
    // DSP56300FM p.13-128: PM8 accumulator source with scale-up mode.
    // A = $00:800000:000000. Scale-up: bits 55:46 not uniform -> limit to $7FFFFF.
    // Same encoding as test_pm8_accumulator_scaling_real but with scale-up active.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S1; // scale-up
    pram[0] = 0xC80800; // PM8: A->X:(R0)+N0, Y:(R4)->Y0, NOP ALU
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 1;
    s.registers[reg::R4] = 0x20;
    s.registers[reg::N4] = 1;
    yram[0x20] = 0xABCDEF;
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x7FFFFF,
        "PM8 scale-up: positive overflow should limit A to $7FFFFF"
    );
    assert_eq!(
        s.registers[reg::Y0],
        0xABCDEF,
        "PM8 scale-up: Y-side read should load Y0"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "PM8 scale-up: L flag should be set when limiting occurs"
    );
}

#[test]
fn test_vsl_b_accumulator() {
    // DSP56300FM p.13-182: VSL B,(R0)+ - B accumulator source (S=1).
    // Template: 0000101S11MMMRRR110i0000, S=1, MMM=011, RRR=000, i=0.
    // 0000_1011_1101_1000_1100_0000 = 0x0BD8C0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x123456;
    s.registers[reg::B0] = 0x789ABC;
    pram[0] = 0x0BD8C0; // vsl B,(R0)+
    run_one(&mut s, &mut jit);
    // X:$10 = B1, Y:$10 = (B0 << 1) | i = (0x789ABC << 1) | 0 = 0xF13578
    assert_eq!(xram[0x10], 0x123456, "VSL B: X:ea should be B1");
    assert_eq!(yram[0x10], 0xF13578, "VSL B: Y:ea should be (B0 << 1) | i");
    assert_eq!(
        s.registers[reg::R0],
        0x0011,
        "R0 should be post-incremented"
    );
}

#[test]
fn test_vsl_nonzero_a0_shift() {
    // DSP56300FM p.13-182: VSL A,(R0)+ with i=1 and non-zero A0.
    // A0 = 0x400000, i=1. Y:ea = (0x400000 << 1) | 1 = 0x800001.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0020;
    s.registers[reg::M0] = 0xFFFFFF;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0xAAAAAA;
    s.registers[reg::A0] = 0x400000;
    pram[0] = 0x0AD8D0; // vsl A,(R0)+ with i=1
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x20], 0xAAAAAA, "VSL: X:ea should be A1");
    assert_eq!(
        yram[0x20], 0x800001,
        "VSL: Y:ea should be (A0<<1)|1 = (0x400000<<1)|1"
    );
}

#[test]
fn test_lra_rn_r_dest() {
    // DSP56300FM p.13-92: LRA Rn,D - with R register as destination (e.g., R2).
    // Template: 0000010011000RRR000ddddd, RRR=0 (R0), ddddd=R2 (0x02).
    // 0000_0100_1100_0000_0000_0010 = 0x04C002
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0050;
    pram[0x10] = 0x04C012; // lra R0,R2
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    // R2 = (R0 + PC) & 0xFFFFFF = (0x50 + 0x10) = 0x60
    assert_eq!(
        s.registers[reg::R2],
        0x60,
        "LRA R0,R2: R2 should be R0 + PC"
    );
}

#[test]
fn test_move_long_disp_negative() {
    // DSP56300FM p.13-118: X:(Rn+xxxx) with negative 24-bit displacement.
    // R1=0x100, displacement = -5 (0xFFFFFB). EA = 0x100 + 0xFFFFFB = 0xFB.
    // Template: 0000101001110RRR1WDDDDDD, RRR=001 (R1), W=1 (read), D=X0(0x04).
    // 0000_1010_0111_0001_1100_0100 = 0x0A71C4
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0100;
    xram[0xFB] = 0xBEEF42;
    pram[0] = 0x0A71C4; // move x:(r1+xxxx),x0
    pram[1] = 0xFFFFFB; // displacement = -5
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0xBEEF42,
        "should read from X:(R1-5)=X:$FB"
    );
}

#[test]
fn test_move_short_disp_accumulator_dest() {
    // DSP56300FM p.13-118: X:(R0+1),A - accumulator as destination.
    // Template: 0000001aaaaaaRRR1a0WDDDD, offset=1, RRR=0, W=1, DDDD=A(0x0E).
    // From test_move_x_imm_read (X0=0x4, offset=1): opcode=0x0200D4.
    // Change DDDD from 0x4 (X0) to 0xE (A): 0x0200DE
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x10;
    xram[0x11] = 0x800000; // R0+1 = 0x11
    pram[0] = 0x0200DE; // move x:(r0+1),A
    run_one(&mut s, &mut jit);
    // 0x800000 loaded into A1, sign-extended: A2=0xFF, A0=0
    assert_eq!(s.registers[reg::A1], 0x800000, "A1 should be loaded");
    assert_eq!(
        s.registers[reg::A2],
        0xFF,
        "A2 should be sign-extended from bit 23"
    );
}

#[test]
fn test_move_short_disp_max_positive_offset() {
    // DSP56300FM p.13-118: Maximum positive 7-bit signed offset = +63.
    // Template: 0000001aaaaaaRRR1a0WDDDD.
    // offset=63=0x3F. pack_xy_imm_offset: xxx_hi=(0x3F>>1)&0x3F=0x1F, xxx_lo=0x3F&1=1.
    // RRR=0 (R0), W=1 (read), DDDD=0x4 (X0), s=0 (X space).
    // opcode = 0x020080 | (0x1F << 11) | (0 << 8) | (1 << 6) | 0 | 0x10 | 4 = 0x02F8D4.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x10;
    xram[0x4F] = 0xCAFE00; // R0 + 63 = 0x10 + 0x3F = 0x4F
    pram[0] = 0x02F8D4; // move X:(R0+63),X0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0xCAFE00,
        "X0 should read from X:(R0+63)"
    );
}

#[test]
fn test_movep_y_space_pp_read() {
    // DSP56300FM p.13-134: MOVEP Y:pp -> register (Y-space peripheral read).
    // Movep0: 0000100sW1dddddd00pppppp, s=1 (Y), W=0 (read), d=A(0x0E), pp=4
    // 0000100_1_0_1_001110_00_000100 = 0x094E04
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut periph = [0u32; PERIPH_SIZE];
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.y_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Buffer {
            base: periph.as_mut_ptr(),
            offset: 0,
        },
    });
    let mut s = DspState::new(map);
    // Y:$FFFFC4 -> periph[0xFFFFC4 - 0xFFFF80] = periph[68]
    periph[68] = 0xABCDEF;
    pram[0] = 0x094E04; // movep y:$ffffc4,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0xABCDEF,
        "MOVEP Y:pp should read from Y-space peripheral"
    );
}

#[test]
fn test_pm2_move_a_b_limiting() {
    // DSP56300FM p.13-115: MOVE A,B - accumulator A is read through the data
    // shifter/limiter. When extension bits are in use (A2 != sign-extend of A1),
    // the value is limited to $7FFFFF (positive) or $800000 (negative).
    // PM2 template: 001000eeeeedddddaaaaaaaa
    // src=A(0x0E), dst=B(0x0F), alu=NOP(0x00)
    // 001000_01110_01111_00000000 = 0x21CF00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Positive overflow: A = $01:000000:000000 (extension in use, positive)
    // Should limit to B1 = $7FFFFF
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x21CF00; // move A,B (PM2, NOP ALU)
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0x7FFFFF,
        "Positive overflow should limit to $7FFFFF"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "L flag should be set when limiting occurs"
    );

    // Negative overflow: A = $FE:FFFFFF:000000 (extension in use, negative)
    // Should limit to B1 = $800000
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.registers[reg::A2] = 0xFE;
    s2.registers[reg::A1] = 0xFFFFFF;
    s2.registers[reg::A0] = 0x000000;
    pram[0] = 0x21CF00;
    run_one(&mut s2, &mut jit2);
    assert_eq!(
        s2.registers[reg::B1],
        0x800000,
        "Negative overflow should limit to $800000"
    );
}

#[test]
fn test_pm4_l_move_scale_down_limiting() {
    // DSP56300FM p.13-126: L: move reads accumulator through data shifter/limiter.
    // Scale-down (S1:S0=01): limiting checks bits 55:48. If not uniform, limit.
    // A = $02:000000:000000 (positive, extension not uniform -> limit to $7FFFFF).
    // Opcode: 0x481000 = move A10,L:$10 (PM4X write A to L:$10, NOP ALU).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    s.registers[reg::A2] = 0x02;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x481000; // move A10,L:$10 (PM4X)
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x7FFFFF,
        "PM4 L: move scale-down positive overflow should limit A1 to $7FFFFF"
    );
    assert_eq!(
        yram[0x10], 0xFFFFFF,
        "PM4 L: move scale-down positive overflow should limit A0 to $FFFFFF"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "PM4 L: move scale-down: L flag should be set"
    );
}

#[test]
fn test_pm4_l_move_scale_up_limiting_negative() {
    // Scale-up (S1:S0=10): limiting checks bits 55:46.
    // A = $FF:7FFFFF:000000 (negative, bits 55:46 not uniform -> limit to $800000).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S1; // scale-up
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x481000; // move A10,L:$10 (PM4X)
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x800000,
        "PM4 L: move scale-up negative overflow should limit A1 to $800000"
    );
    assert_eq!(
        yram[0x10], 0x000000,
        "PM4 L: move scale-up negative overflow should limit A0 to $000000"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "PM4 L: move scale-up: L flag should be set"
    );
}

#[test]
fn test_pm8_accumulator_scaling_real() {
    // DSP56300FM p.13-128: PM8 accumulator source uses read_reg_for_move which applies
    // scaling/limiting. Test A write to X memory with positive overflow (no scaling).
    // A = $01:000000:000000 (positive extension overflow -> limit to $7FFFFF).
    // PM8 encoding: A write to X:(R0)+N0, Y0 read from Y:(R4).
    // bit23=1, bit22=1(Y reads mem), bits19:18=10(A), bits17:16=00(Y0),
    // bit15=0(X writes reg->mem), ea1=01000((R0)+N0), ea2 bits computed.
    // Opcode = 0xC80800.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0xC80800; // PM8: A->X:(R0)+N0, Y:(R4)->Y0, NOP ALU
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 1;
    s.registers[reg::R4] = 0x20;
    s.registers[reg::N4] = 1;
    yram[0x20] = 0x123456;
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x7FFFFF,
        "PM8 accumulator positive overflow should limit to $7FFFFF"
    );
    assert_eq!(
        s.registers[reg::Y0],
        0x123456,
        "PM8 Y-side read should load Y0"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "PM8: L flag should be set when limiting occurs"
    );
}

#[test]
fn test_movem_ssh_push() {
    // MOVEM writing to SSH should push the stack (increment SP).
    // movem p:$20,SSH: W=1, DDDDDD=111100 (SSH=0x3C), addr=0x20
    // Template: 0000_0111_W0aa_aaaa_00DD_DDDD
    // 0000_0111_1010_0000_0011_1100 = 0x07A03C
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let sp_before = s.registers[reg::SP] & 0xF;
    pram[0x20] = 0xABCDEF; // value in P-space
    pram[0] = 0x07A03C; // movem p:$20,SSH
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(
        sp_after,
        sp_before + 1,
        "MOVEM to SSH should push stack (increment SP)"
    );
}

#[test]
fn test_movem_accumulator_source_scale_up() {
    // MOVEM with accumulator source and scaling mode should limit.
    // movem A,p:$20: W=0, DDDDDD=001110 (A=0x0E), addr=0x20
    // 0000_0111_0010_0000_0000_1110 = 0x07200E
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::S1; // scale-up mode
    // A with positive extension overflow - should be limited
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x07200E; // movem A,p:$20
    run_one(&mut s, &mut jit);
    assert_eq!(
        pram[0x20], 0x7FFFFF,
        "MOVEM accumulator source with scale-up should limit to $7FFFFF"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "MOVEM: L flag should be set when limiting occurs"
    );
}

#[test]
fn test_move_long_disp_accumulator_source_scale_up() {
    // move_long_disp with accumulator source + scaling mode.
    // move A,X:(R1+xxxx) with scale-up: positive overflow -> limit to $7FFFFF.
    // Same encoding as test_move_long_disp_accumulator_source but with S1 set.
    // 0x0A718E = move A,X:(R1+xxxx)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::S1; // scale-up mode
    s.registers[reg::R1] = 0x0000;
    s.registers[reg::A2] = 0x01; // positive extension overflow
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0A718E; // move A,X:(R1+xxxx)
    pram[1] = 0x000005; // offset = 5
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[5], 0x7FFFFF,
        "move_long_disp with scale-up should limit to $7FFFFF"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "move_long_disp: L flag should be set when limiting occurs"
    );
}

#[test]
fn test_pm4_l_move_scale_down_negative_limiting() {
    // PM4 L: move scale-down negative overflow verification.
    // Scale-down (S1:S0=01): limiting checks bits 55:48. If not uniform, limit.
    // A = $FD:000000:000000 (negative, extension bits not all 1s -> limit to $800000:000000).
    // Opcode: 0x481000 = move A10,L:$10 (PM4X write A to L:$10).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    s.registers[reg::A2] = 0xFD; // negative, extension not uniform
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::A0] = 0x789ABC;
    pram[0] = 0x481000; // move A10,L:$10
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x800000,
        "PM4 L: scale-down negative overflow should limit A1 to $800000"
    );
    assert_eq!(
        yram[0x10], 0x000000,
        "PM4 L: scale-down negative overflow should limit A0 to $000000"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "PM4 L: L flag should be set on limiting"
    );
}
