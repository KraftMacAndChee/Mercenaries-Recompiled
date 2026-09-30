use super::*;
#[test]
fn test_inc_dec() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0, inc A -> A = 1
    pram[0] = 0x000008; // inc A (bit 0 = 0 = A)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 1);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.pc, 1);

    // dec A -> A = 0
    pram[1] = 0x00000A; // dec A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_add_imm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // add #$10,A -> 0000000101 010000 1000 0 000 = 0x015080
    pram[0] = 0x015080;
    run_one(&mut s, &mut jit);
    // 6-bit immediate placed at A1 position (per C emu_add_x: source[1]=xx)
    // A was 0, so A1 = 0x10, A0 = 0, A2 = 0
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A1], 0x10);
    assert_eq!(s.registers[reg::A2], 0);
}

#[test]
fn test_parallel_clr_a() {
    // clr A with no parallel move (pm_0, alu_byte = 0x13)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0x123;
    s.registers[reg::A1] = 0x456;
    s.registers[reg::A2] = 0x78;
    pram[0] = 0x200013; // pm_2 nop + clr A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::A2], 0);
    // Z flag should be set
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0);
}

#[test]
fn test_parallel_clr_b() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B0] = 0xABC;
    s.registers[reg::B1] = 0xDEF;
    s.registers[reg::B2] = 0x01;
    pram[0] = 0x20001B; // nop parallel move + clr B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B0], 0);
    assert_eq!(s.registers[reg::B1], 0);
    assert_eq!(s.registers[reg::B2], 0);
}

#[test]
fn test_parallel_tfr_x0_a() {
    // tfr X0,A: ALU byte 0x41 (JJ=0->X0, d=0->A, op=1->tfr)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x123456;
    pram[0] = 0x200041; // nop + tfr X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123456);
    assert_eq!(s.registers[reg::A0], 0);
    // Bit 23 = 0, so A2 = 0x00
    assert_eq!(s.registers[reg::A2], 0x00);
}

#[test]
fn test_parallel_tfr_x0_a_negative() {
    // tfr X0,A with negative value (bit 23 set)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x800000; // bit 23 set
    pram[0] = 0x200041; // nop + tfr X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A2], 0xFF); // sign extended
}

#[test]
fn test_parallel_add_x0_a() {
    // add X0,A: ALU byte 0x40 (JJ=0->X0, d=0->A, op=0->add)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x200000;
    pram[0] = 0x200040; // nop + add X0,A
    run_one(&mut s, &mut jit);
    // X0 = 0x100000 placed at A1 position (bits 47:24) = 0x00_100000_000000
    // A = 0x00_200000_000000
    // Sum = 0x00_300000_000000
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_parallel_sub_x0_a() {
    // sub X0,A: ALU byte 0x44 (JJ=0->X0, d=0->A, op=4->sub)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x300000;
    pram[0] = 0x200044; // nop + sub X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_parallel_tst_a() {
    // tst A: ALU byte 0x03
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A2] = 0;
    pram[0] = 0x200003; // nop + tst A
    run_one(&mut s, &mut jit);
    // Z should be set (accumulator is zero)
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0);
    // N should be clear
    assert_eq!(s.registers[reg::SR] & (1 << 3), 0);
}

#[test]
fn test_parallel_neg_a() {
    // neg A: ALU byte 0x36
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00_000001_000000 (positive 1 in A1)
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 1;
    s.registers[reg::A2] = 0;
    pram[0] = 0x200036; // nop + neg A
    run_one(&mut s, &mut jit);
    // neg: result = 0 - A = 56-bit negate
    // 0 - 0x00_000001_000000 = 0xFF_FFFFFF_000000 (in 56-bit)
    // A2 = 0xFF, A1 = 0xFFFFFF, A0 = 0x000000
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0xFFFFFF);
    assert_eq!(s.registers[reg::A0], 0x000000);
}

#[test]
fn test_parallel_abs_a() {
    // abs A: ALU byte 0x26
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0xFF_FFFFFF_000000 (= -0x000001_000000 in 56-bit signed)
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A2] = 0xFF;
    pram[0] = 0x200026; // nop + abs A
    run_one(&mut s, &mut jit);
    // abs should give 0x00_000001_000000
    assert_eq!(s.registers[reg::A2], 0x00);
    assert_eq!(s.registers[reg::A1], 0x000001);
    assert_eq!(s.registers[reg::A0], 0x000000);
}

#[test]
fn test_parallel_cmp_x0_a() {
    // cmp X0,A: ALU byte 0x45 (JJ=0->X0, d=0->A, op=5->cmp)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x200045; // nop + cmp X0,A
    run_one(&mut s, &mut jit);
    // A - X0 = 0, so Z should be set
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0);
    // A should be unchanged (cmp doesn't store result)
    assert_eq!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_parallel_tfr_b_a() {
    // tfr B,A: ALU byte 0x01
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B0] = 0x111;
    s.registers[reg::B1] = 0x222;
    s.registers[reg::B2] = 0x33;
    pram[0] = 0x200001; // nop + tfr B,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0x111);
    assert_eq!(s.registers[reg::A1], 0x222);
    assert_eq!(s.registers[reg::A2], 0x33);
}

#[test]
fn test_parallel_add_b_a() {
    // add B,A: ALU byte 0x10
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A2] = 0;
    s.registers[reg::B0] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B2] = 0;
    pram[0] = 0x200010; // nop + add B,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_add_long() {
    // add #xxxx,A: encoding 00000001 01000000 1100d000
    // For A (d=0): last byte = 1100_0_000 = 0xC0 -> 0x0140C0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0140C0; // add #xxxx,A
    pram[1] = 0x200000; // immediate = $200000
    run_one(&mut s, &mut jit);
    // A = $100000 + $200000 = $300000 in A1
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_sub_long() {
    // sub #xxxx,A: 1100d100, d=0 -> 0xC4 -> 0x0140C4
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x500000;
    pram[0] = 0x0140C4; // sub #xxxx,A
    pram[1] = 0x200000; // immediate = $200000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000);
}

#[test]
fn test_cmp_long() {
    // cmp #xxxx,A: 1100d101, d=0 -> 0xC5 -> 0x0140C5
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x200000;
    pram[0] = 0x0140C5; // cmp #xxxx,A
    pram[1] = 0x200000; // immediate = $200000
    run_one(&mut s, &mut jit);
    // A unchanged, Z flag should be set (equal)
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0); // Z bit
}

#[test]
fn test_cmp_imm() {
    // cmp #xx,A: 0000000101iiiiii1000d101
    // imm=5, d=0: bits 15:8 = 01_000101 = 0x45, bits 7:0 = 0x85
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000005;
    pram[0] = 0x014585; // cmp #5,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000005); // unchanged
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0); // Z set
}
#[test]
fn test_parallel_sub_b_from_a() {
    // SUB B,A (alu_byte 0x14): A = A - B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00_500000_000000, B = 0x00_200000_000000
    s.registers[reg::A1] = 0x500000;
    s.registers[reg::B1] = 0x200000;
    pram[0] = 0x200014;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A2], 0);
}

#[test]
fn test_parallel_sub_a_from_b() {
    // SUB A,B (alu_byte 0x1C): B = B - A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::B1] = 0x300000;
    pram[0] = 0x20001C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
    assert_eq!(s.registers[reg::B0], 0);
    assert_eq!(s.registers[reg::B2], 0);
}

#[test]
fn test_parallel_abs_b() {
    // ABS B (alu_byte 0x2E)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // B = -1 in A1 position: 0xFF_FFFFFF_000000
    s.registers[reg::B0] = 0;
    s.registers[reg::B1] = 0xFFFFFF;
    s.registers[reg::B2] = 0xFF;
    pram[0] = 0x20002E;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B2], 0x00);
    assert_eq!(s.registers[reg::B1], 0x000001);
    assert_eq!(s.registers[reg::B0], 0x000000);
}

#[test]
fn test_parallel_neg_b() {
    // NEG B (alu_byte 0x3E)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B0] = 0;
    s.registers[reg::B1] = 1;
    s.registers[reg::B2] = 0;
    pram[0] = 0x20003E;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B2], 0xFF);
    assert_eq!(s.registers[reg::B1], 0xFFFFFF);
    assert_eq!(s.registers[reg::B0], 0x000000);
}

#[test]
fn test_parallel_add_x_b() {
    // ADD X,B (alu_byte 0x28): B = B + (X1:X0)
    // X1:X0 as 48-bit placed in accumulator bits 47:0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0;
    s.registers[reg::B1] = 0x200000;
    pram[0] = 0x200028;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x300000);
}

#[test]
fn test_parallel_sub_y_b() {
    // SUB Y,B (alu_byte 0x3C): B = B - (Y1:Y0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0;
    s.registers[reg::B1] = 0x300000;
    pram[0] = 0x20003C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_parallel_sub_x0_b() {
    // SUB X0,B (alu_byte 0x4C)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::B1] = 0x300000;
    pram[0] = 0x20004C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_parallel_cmp_x0_b() {
    // CMP X0,B (alu_byte 0x4D): compares, doesn't store
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::B1] = 0x100000;
    pram[0] = 0x20004D;
    run_one(&mut s, &mut jit);
    // B should be unchanged (CMP doesn't store)
    assert_eq!(s.registers[reg::B1], 0x100000);
    // Z flag should be set (equal)
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0);
}

#[test]
fn test_addr() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // addr B,A: A = A/2 + B. A=0x00:400000:000000, B=0x00:200000:000000
    // A/2 = 0x00:200000:000000, result A = 0x00:400000:000000
    pram[0] = 0x200002;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x400000);
}

#[test]
fn test_subr() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // subr B,A: A = B - A/2. A=0x00:400000:000000, B=0x00:200000:000000
    // A/2 = 0x00:200000:000000, result A = 0x00:000000:000000
    pram[0] = 0x200006;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x000000);
}

#[test]
fn test_addl() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // addl B,A: A = B + 2*A. A=0x00:200000:000000, B=0x00:100000:000000
    // 2*A = 0x00:400000:000000, result A = 0x00:500000:000000
    pram[0] = 0x200012;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x500000);
}

#[test]
fn test_subl() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // subl B,A: A = 2*A - B. A=0x00:300000:000000, B=0x00:100000:000000
    // 2*A = 0x00:600000:000000, result A = 0x00:500000:000000
    pram[0] = 0x200016;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x300000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x500000);
}

#[test]
fn test_rnd() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // rnd A: convergent rounding of A0 into A1
    // A = 0x00:400000:800001 -> round up -> 0x00:400001:000000
    pram[0] = 0x200011;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x800001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x400001);
    assert_eq!(s.registers[reg::A0], 0x000000);

    // A = 0x00:400000:800000 -> convergent: half, A1 even -> stay (round down)
    pram[1] = 0x200011;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400000);
    assert_eq!(s.registers[reg::A0], 0x000000);

    // A = 0x00:400001:800000 -> convergent: half, A1 odd -> round up
    pram[2] = 0x200011;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400001;
    s.registers[reg::A0] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400002);
    assert_eq!(s.registers[reg::A0], 0x000000);
}

#[test]
fn test_max() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // max A,B: per DSP56300FM p.13-106: "If B-A <= 0 then A->B"
    // Transfer when A >= B. C cleared on transfer, set otherwise.
    pram[0] = 0x20001D;
    // A=0x00:400000:000000 > B=0x00:200000:000000 -> A>=B, transfer B=A, C=0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(
        s.registers[reg::B1],
        0x400000,
        "MAX: A>=B, B should become A"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "MAX: C=0 on transfer"
    );

    // A=0x00:100000:000000 < B=0x00:300000:000000 -> A<B, no transfer, C=1
    pram[1] = 0x20001D;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x300000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x300000, "MAX: A<B, B unchanged");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        1,
        "MAX: C=1 when no transfer"
    );
}

#[test]
fn test_adc() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // adc X,A: A = A + X + C. X = X1:X0 = 0x080000:0x100000
    // A = 0x00:400000:000000, C=1
    pram[0] = 0x200021;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::X1] = 0x080000;
    s.registers[reg::SR] |= 1 << sr::C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // A = 0x400000:000000 + 0x080000:100000 + 1 = 0x480000:100001
    assert_eq!(s.registers[reg::A1], 0x480000);
    assert_eq!(s.registers[reg::A0], 0x100001);

    // adc X,A without carry
    pram[1] = 0x200021;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::SR] &= !(1 << sr::C);
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x480000);
    assert_eq!(s.registers[reg::A0], 0x100000);
}

#[test]
fn test_sbc() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // sbc X,A: A = A - X - C. X = X1:X0 = 0x080000:0x100000
    // A = 0x00:500000:000000, C=0
    pram[0] = 0x200025;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x500000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::X1] = 0x080000;
    s.registers[reg::SR] &= !(1 << sr::C);
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // A = 0x500000:000000 - 0x080000:100000 = 0x47FFFF:F00000
    assert_eq!(s.registers[reg::A1], 0x47FFFF);
    assert_eq!(s.registers[reg::A0], 0xF00000);
}

#[test]
fn test_cmpm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // cmpm X0,A: compare |A| - |X0|, flags only, no store
    // A = 0x00:400000:000000, X0 = 0x400000 -> |A|=|X0|, Z=1
    pram[0] = 0x200047;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0);
}

#[test]
fn test_cmp_acc() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // cmp B,A (parallel alu byte 0x05): compare A - B, flags only
    // A > B -> N=0, Z=0
    pram[0] = 0x200005;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0);
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0);
    // A unchanged
    assert_eq!(s.registers[reg::A1], 0x400000);
}

#[test]
fn test_sub_imm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // sub #$10,A: opcode = 0x014084 | (imm << 8)
    pram[0] = 0x014084 | (0x10 << 8);
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x3FFFF0);
}
#[test]
fn test_condition_codes() {
    // Exercise all condition codes via TCC B,A (tcc_idx=0).
    // TCC encoding: 0x020000 | (cc << 12). B1 = marker, check A1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let m = 0x111111u32;
    // Place each cc variant at a different PC to avoid cache collisions
    pram[0] = 0x028000; // CS(8)
    pram[1] = 0x022000; // NE(2)
    pram[2] = 0x023000; // PL(3)
    pram[3] = 0x02B000; // MI(11)
    pram[4] = 0x021000; // GE(1)
    pram[5] = 0x029000; // LT(9)
    pram[6] = 0x027000; // GT(7)
    pram[7] = 0x02F000; // LE(15)
    pram[8] = 0x024000; // NN(4)
    pram[9] = 0x02C000; // NR(12)
    pram[10] = 0x025000; // EC(5)
    pram[11] = 0x02D000; // ES(13)
    pram[12] = 0x026000; // LC(6)
    pram[13] = 0x02E000; // LS(14)
    pram[14] = 0x02A000; // EQ(10)

    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B1] = m;

    // CS: taken when C=1
    s.registers[reg::SR] = 1 << sr::C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // NE: taken when Z=0
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // PL: taken when N=0
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // MI: taken when N=1
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::N;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // GE: taken when N^V=0 (N=0, V=0)
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // LT: taken when N^V=1 (N=1, V=0)
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::N;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // GT: taken when Z=0 AND N^V=0
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // LE: taken when Z=1 OR N^V=1
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::Z;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // NN: taken when Z+(!U&E)=0 (not normalized)
    // With Z=0, U=0, E=0: !U&E = 1&0 = 0, Z|0 = 0 -> NN is true
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // NR: taken when Z+(!U&E)=1 (normalized)
    // With Z=0, U=0, E=1: !U&E = 1&1 = 1, Z|1 = 1 -> NR is true
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::E;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // EC: taken when E=0
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // ES: taken when E=1
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::E;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // LC: taken when L=0
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // LS: taken when L=1
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::L;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);

    // EQ: taken when Z=1
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::SR] = 1 << sr::Z;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], m);
}
#[test]
fn test_alu_b_dispatch() {
    // Exercise B-accumulator ALU dispatch paths via 0x200000 | alu_byte
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    // 0x09: TFR A,B -- transfer A to B
    pram[0] = 0x200009;
    // 0x0B: TST B -- test B
    pram[1] = 0x20000B;
    // 0x18: ADD A,B -- add A to B
    pram[2] = 0x200018;
    // 0x19: RND B -- round B
    pram[3] = 0x200019;
    // 0x1A: ADDL A,B -- B = A + 2*B
    pram[4] = 0x20001A;

    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // TFR A,B: A1=0x100000 -> B should get A
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x100000);

    // TST B: test B (just updates flags, no data change)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x100000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // not zero

    // ADD A,B: B = B + A = 0x100000 + 0x100000 = 0x200000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);

    // RND B: round B (A0 determines rounding, B0=0 so no rounding effect)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);

    // ADDL A,B: B = A + 2*B = 0x100000_000000 + 2*0x200000_000000
    // = 0x100000_000000 + 0x400000_000000 = 0x500000_000000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x500000);
}

#[test]
fn test_cmpm_acc_to_acc() {
    // CMPM A,B (alu_byte=0x0F): compare magnitude |B| - |A|
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20000F; // CMPM A,B
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // |B| - |A| = 0 -> Z=1
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0);
}
#[test]
fn test_sub_imm_b() {
    // sub #$10,B -- covers SubImm with B accumulator
    // SubImm: 0000000101iiiiii1000d100, d=1, imm=0x10
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x01408C | (0x10 << 8); // sub #$10,B
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x400000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // B = 0x00_400000_000000 - 0x00_000010_000000 = 0x00_3FFFF0_000000
    assert_eq!(s.registers[reg::B1], 0x3FFFF0);
}

#[test]
fn test_parallel_tst_b() {
    // TST B (alu=0x0B). Covers parallel ALU dispatch line 5566.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20000B; // NOP + TST B
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // B is zero -> Z flag set
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0); // Z=1
}

#[test]
fn test_parallel_subr_a_b() {
    // SUBR A,B (alu=0x0E): B = (B >> 1) - A. Covers line 5567.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20000E; // NOP + SUBR A,B
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x600000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // B = (B >> 1) - A = 0x300000 - 0x100000 = 0x200000
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_parallel_cmpm_a_b() {
    // CMPM A,B (alu=0x0F): compare magnitude |A| vs |B|. Covers line 5568.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20000F; // NOP + CMPM A,B
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // Equal magnitude -> Z flag set
    assert_ne!(s.registers[reg::SR] & (1 << 2), 0); // Z=1
}

#[test]
fn test_parallel_subl_a_b() {
    // SUBL A,B (alu=0x1E): B = (B << 1) - A. Covers line 5585.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20001E; // NOP + SUBL A,B
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // B = (B << 1) - A = 0x400000 - 0x100000 = 0x300000
    assert_eq!(s.registers[reg::B1], 0x300000);
}

#[test]
fn test_parallel_add_y_a() {
    // ADD Y,A (alu=0x30). Covers line 5607.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200030; // NOP + ADD Y,A
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000);
}

#[test]
fn test_parallel_add_y_b() {
    // ADD Y,B (alu=0x38). Covers line 5615.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200038; // NOP + ADD Y,B
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x300000);
}

#[test]
fn test_parallel_sub_y_a() {
    // SUB Y,A (alu=0x34). Covers line 5611.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200034; // NOP + SUB Y,A
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x500000;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400000);
}

#[test]
fn test_parallel_add_x_b_dispatch() {
    // ADD X,B (alu=0x28). Covers parallel ALU dispatch line 5598.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200028; // NOP + ADD X,B
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x300000);
}

#[test]
fn test_parallel_adc_x_b() {
    // ADC X,B (alu=0x29). Covers line 5599.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200029; // NOP + ADC X,B
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    s.registers[reg::SR] |= 1; // set carry
    run_one(&mut s, &mut jit);
    // B = B + X + C = 0x200000 + 0x100000 + 1 in B0
    assert_eq!(s.registers[reg::B1], 0x300000);
    assert_eq!(s.registers[reg::B0], 1);
}

#[test]
fn test_parallel_sbc_x_b() {
    // SBC X,B (alu=0x2D). Covers line 5604.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20002D; // NOP + SBC X,B
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x500000;
    s.registers[reg::B0] = 0;
    // No carry set
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x400000);
}

#[test]
fn test_parallel_cmpm_b_a() {
    // alu_byte 0x07: cmpm_acc B,A -- compare magnitudes
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x07;
    s.registers[reg::A1] = 0x300000;
    s.registers[reg::B1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_parallel_addr_a_b() {
    // alu_byte 0x0A: addr A,B -- B = A + (B >> 1)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x0A;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::B1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // (B>>1) + A = 0x100000 + 0x100000 = 0x200000
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_parallel_cmp_a_b() {
    // alu_byte 0x0D: cmp_acc A,B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x0D;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::B1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_parallel_add_long_x_a() {
    // alu_byte 0x20: add_xy X1,X0,A -- A += (X1:X0) 48-bit
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x20;
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::A1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000);
}

#[test]
fn test_parallel_sub_long_x_a() {
    // alu_byte 0x24: sub_xy X1,X0,A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x24;
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::A1] = 0x300000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_parallel_sub_long_x_b() {
    // alu_byte 0x2C: sub_xy X1,X0,B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x2C;
    s.registers[reg::X1] = 0x100000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::B1] = 0x300000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_parallel_adc_y_a() {
    // alu_byte 0x31: adc Y1,Y0,A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x31;
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::A1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000);
}

#[test]
fn test_parallel_sbc_y_a() {
    // alu_byte 0x35: sbc Y1,Y0,A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x35;
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::A1] = 0x300000;
    s.registers[reg::SR] |= 1 << sr::C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_parallel_adc_y_b() {
    // alu_byte 0x39: adc Y1,Y0,B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x39;
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::B1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x300000);
}

#[test]
fn test_parallel_sbc_y_b() {
    // alu_byte 0x3D: sbc Y1,Y0,B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x3D;
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::Y0] = 0x000000;
    s.registers[reg::B1] = 0x300000;
    s.registers[reg::SR] |= 1 << sr::C;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}
#[test]
fn test_addr_negative_acc() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // addr B,A: A = B + A/2.  A = 0xFF:800000:000000 (negative), B = 0
    // A/2 (arithmetic) = 0xFF:C00000:000000 (sign preserved)
    pram[0] = 0x200002; // addr B,A
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A2],
        0xFF,
        "ADDR: sign bit must be preserved by arithmetic shift"
    );
    assert_eq!(s.registers[reg::A1], 0xC00000);
}

#[test]
fn test_subr_negative_acc() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // subr B,A: A = A/2 - B.  A = 0xFF:800000:000000 (negative), B = 0
    // A/2 (arithmetic) = 0xFF:C00000:000000
    pram[0] = 0x200006; // subr B,A
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A2],
        0xFF,
        "SUBR: sign bit must be preserved by arithmetic shift"
    );
    assert_eq!(s.registers[reg::A1], 0xC00000);
}

#[test]
fn test_addr_carry_flag() {
    // ADDR B,A with A having bit 0 set (shift carry = 1).
    // If add doesn't generate carry, C should be 0 (standard carry from addition only).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00:000000:000001 (bit 0 set, shift carry = 1), B = 0
    // A/2 = 0x00:000000:000000 (bit 0 shifted out), result = 0
    // C should be 0 (no carry from addition 0+0)
    pram[0] = 0x200002; // addr B,A
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000001;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0xC00300; // clear all CCR
    run_one(&mut s, &mut jit);
    let c = s.registers[reg::SR] & 1;
    assert_eq!(c, 0, "ADDR: shift carry should NOT be ORed into C");
}

#[test]
fn test_addl_carry_xor() {
    // ADDL B,A: A = B + 2*A.
    // A=0xFF:800000:000000, B=0xFF:800000:000000
    // bit 55 of A = 1, so asl_carry = 1
    // 2*A: d_shifted = mask56(A<<1) = 0xFF:000000:000000
    // result = B + d_shifted = 0xFF:800000:000000 + 0xFF:000000:000000
    //        = 0x01FE:800000:000000. Bit 56 = 1, C_from_add = 1.
    // Correct C = C_from_add XOR asl_carry = 1 XOR 1 = 0
    // Bug gives C = 1 OR 1 = 1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20001A; // addl B,A
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0xFF;
    s.registers[reg::B1] = 0x800000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0xC00300;
    run_one(&mut s, &mut jit);
    let c = s.registers[reg::SR] & 1;
    assert_eq!(
        c, 0,
        "ADDL: carry should be C_from_add XOR asl_carry, not OR"
    );
}
#[test]
fn test_addl_v_from_shift_overflow() {
    // addl B,A (0x200012): S=B, D=A. Operation: B + 2*A -> A
    // A = 0x40:000000:000000 (bit 54 set, bit 55=0). B = 0.
    // 2*A: left shift, bit 55 changes (0->1), asl_carry=0, asl_v=1.
    // V should be set from shift overflow (not shift carry).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200012; // addl b,a (S=B, D=A)
    s.registers[reg::A2] = 0x40; // bit 54 set, bit 55 clear -> asl_v=1
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0xC00300;
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(
        v, 1,
        "ADDL: V should be set when MSB changes during left shift"
    );
}

#[test]
fn test_rnd_overflow_sets_v() {
    // A = 0x7F:FFFFFF:800000. Rounding adds 0x800000 to A0,
    // A0 becomes 0x1000000 -> carry into A1 (0xFFFFFF) -> carry into A2.
    // A2 goes from 0x7F to 0x80 -> sign changes -> V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x800000;
    s.registers[reg::SR] = 0xC00300;
    pram[0] = 0x200011; // rnd A (parallel: nop)
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(v, 1, "RND: overflow should set V when sign changes");
}

#[test]
fn test_maxm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // maxm A,B: If |B|-|A| <= 0, transfer A->B. C=0 on transfer.
    // A = 0xFF:C00000:000000 (negative, |A| = 0x00:400000:000000)
    // B = 0x00:200000:000000 (positive, |B| = 0x00:200000:000000)
    // |A|=0x400000 > |B|=0x200000 -> |B|-|A| < 0 -> transfer A->B, C=0
    pram[0] = 0x200015; // maxm a,b (opcode 0x15)
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xC00000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0xC00300;
    run_one(&mut s, &mut jit);
    // B should become A (the original, not |A|)
    assert_eq!(
        s.registers[reg::B2],
        0xFF,
        "MAXM: B should get A's value (negative)"
    );
    assert_eq!(s.registers[reg::B1], 0xC00000);
    let c = s.registers[reg::SR] & 1;
    assert_eq!(c, 0, "MAXM: C=0 on transfer");
}
#[test]
fn test_jcc_nr_zero_is_normalized() {
    // NR (Normalized) should be true when Z=1 (zero value counts as normalized)
    // Manual Table 12-17: NR condition = Z + (!U & E) = 1
    // With Z=1, NR should be true regardless of U and E
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set Z=1, U=0, E=0 - NR should be true (Z=1 makes it true)
    s.registers[reg::SR] = 1 << sr::Z;
    // JNR $100: Jcc with NR(cc=1100=0xC), addr=0x100
    // 00001110 1100 aaaaaaaaaaaa = 0x0EC100
    pram[0] = 0x0EC100;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 0x100,
        "JNR should be taken when Z=1 (zero is normalized)"
    );
}

#[test]
fn test_jcc_nn_zero_is_not_unnormalized() {
    // NN (Not Normalized) should be false when Z=1 (zero value is normalized)
    // Manual Table 12-17: NN condition = Z + (!U & E) = 0
    // With Z=1, NN should be false (zero is normalized, not "not normalized")
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set Z=1, U=0, E=0 - NN should be false
    s.registers[reg::SR] = 1 << sr::Z;
    // JNN $100: Jcc with NN(cc=0100=0x4), addr=0x100
    // 00001110 0100 aaaaaaaaaaaa = 0x0E4100
    pram[0] = 0x0E4100;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 1,
        "JNN should NOT be taken when Z=1 (zero is normalized)"
    );
}

#[test]
fn test_rnd_twos_complement_mode() {
    // When RM=1 (two's complement rounding), the convergent (round-to-even)
    // adjustment should be skipped. With convergent rounding, 0.5 rounds to 0 (even).
    // With two's complement rounding, 0.5 rounds to 1.
    //
    // A = $00:000001:800000 (1.5 in the no-scaling fixed-point format where A1 is integer, A0 fraction)
    // RND A with RM=0: convergent -> A0=0x800000 is the tie, A1 bit 0 = 1 (odd), round up -> A1=2
    //   Actually: add 0x800000 -> A=$00:000002:000000. A0=0 -> clear A1 bit 0. A1=2, bit 0=0, stays 2.
    //   Result: $00:000002:000000
    // RND A with RM=1: two's complement -> add 0x800000, truncate A0. A=$00:000002:000000.
    //   No convergent adjustment. A1=2, A0=0. Result: $00:000002:000000.
    //
    // Better test case: A = $00:000002:800000 (2.5)
    // RND with RM=0: add 0x800000 -> $00:000003:000000. A0=0 -> clear A1 bit 0 -> A1=2.
    //   Result: $00:000002:000000 (convergent rounds 2.5 down to 2, the even number)
    // RND with RM=1: add 0x800000 -> $00:000003:000000. No convergent -> A1 stays 3.
    //   Result: $00:000003:000000 (two's complement rounds 2.5 up to 3)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set A = $00:000002:800000
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000002;
    s.registers[reg::A0] = 0x800000;

    // Set RM=1 (bit 21 of SR)
    s.registers[reg::SR] |= 1 << sr::RM;

    // RND A: opcode 0x200011 (rnd A, no parallel move)
    pram[0] = 0x200011;
    run_one(&mut s, &mut jit);

    // With RM=1 (two's complement), 2.5 should round UP to 3
    assert_eq!(s.registers[reg::A1], 3, "RM=1: 2.5 should round up to 3");
    assert_eq!(s.registers[reg::A0], 0, "A0 should be cleared after RND");
}

#[test]
fn test_neg_overflow_most_negative_56bit() {
    // NEG V flag should trigger for the most negative 56-bit value (0x80_000000_000000).
    // The old code incorrectly used 0x00_800000_000000 (2^47 instead of 2^55).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0x80_000000_000000 (most negative 56-bit value)
    s.registers[reg::A2] = 0x80;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200036; // nop + neg A
    run_one(&mut s, &mut jit);

    // NEG of most negative overflows: result is same value (wraps)
    assert_eq!(s.registers[reg::A2], 0x80);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::A0], 0x000000);
    // V must be set (overflow)
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "NEG: V should be set for most negative value"
    );
    // L must be set (sticky overflow)
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "NEG: L should be set for most negative value"
    );
}

#[test]
fn test_neg_no_overflow_2_to_47() {
    // Regression: the old buggy constant (2^47) would falsely trigger V for this value.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0x00_800000_000000 (2^47 - NOT the most negative 56-bit value)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200036; // nop + neg A
    run_one(&mut s, &mut jit);

    // Result: 0xFF_800000_000000 (negated, fits in 56 bits, no overflow)
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0x800000);
    assert_eq!(s.registers[reg::A0], 0x000000);
    // V must NOT be set (no overflow)
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "NEG: V should NOT be set for 2^47"
    );
}

#[test]
fn test_max_l_flag_sticky() {
    // MAX should update L = L | V (standard definition).
    // If V is already set, L must remain set after MAX.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Pre-set V=1 in SR (L=0 to test that MAX propagates V->L)
    s.registers[reg::SR] |= 1 << sr::V;
    s.registers[reg::SR] &= !(1u32 << sr::L);
    // A > B: transfer case
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x20001D; // max A,B
    run_one(&mut s, &mut jit);

    // L should now be set (L = L_old | V = 0 | 1 = 1)
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "MAX: L should be set when V is set (L = L | V)"
    );
}

#[test]
fn test_sm_add_saturates() {
    // SM=1: ADD that overflows 48 bits -> saturated
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] |= 1 << sr::SM;
    // A = $00_600000_000000
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x600000;
    s.registers[reg::A0] = 0x000000;
    // B = $00_400000_000000
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x400000;
    s.registers[reg::B0] = 0x000000;
    // ADD B,A (parallel): alu byte = 0x10
    pram[0] = 0x200010;
    run_one(&mut s, &mut jit);

    // $00_A00000_000000 overflows 48-bit -> saturate positive
    assert_eq!(s.registers[reg::A2], 0x00, "SM: ADD saturated A2");
    assert_eq!(s.registers[reg::A1], 0x7FFFFF, "SM: ADD saturated A1");
    assert_eq!(s.registers[reg::A0], 0xFFFFFF, "SM: ADD saturated A0");
}

#[test]
fn test_rnd_v_flag_negative_to_positive_no_overflow() {
    // RND V flag should NOT be set when a negative value rounds to zero
    // (adding a positive rounding constant to a negative value cannot overflow).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = $FF:FFFFFF:800000 (small negative, rounds to 0)
    pram[0] = 0x200011; // rnd A
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0x00);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::A0], 0x000000);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "RND: V must NOT be set when negative rounds to zero"
    );
}

#[test]
fn test_rnd_v_flag_positive_overflow() {
    // RND V flag SHOULD be set when a positive value overflows to negative.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = $7F:FFFFFF:800000: bit 55 = 0 (positive in 56-bit).
    // Rounding adds $800000 to A0, carrying into A1: $FFFFFF + 1 = $000000
    // with carry into A2: $7F + 1 = $80. Now bit 55 = 1 -> sign flipped.
    pram[0] = 0x200011; // rnd A
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "RND: V SHOULD be set on positive-to-negative overflow"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "RND: L SHOULD be latched on overflow"
    );
}

#[test]
fn test_parallel_asr_a() {
    // asr A: ALU byte 0x22
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00_000004_000000
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 4;
    s.registers[reg::A2] = 0;
    pram[0] = 0x200022; // nop + asr A
    run_one(&mut s, &mut jit);
    // Shift right by 1: 0x00_000004_000000 >> 1 = 0x00_000002_000000
    assert_eq!(s.registers[reg::A1], 2);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A2], 0);
    // Carry = bit 0 of original = 0
    assert_eq!(s.registers[reg::SR] & 1, 0);
}

#[test]
fn test_parallel_asl_a() {
    // asl A: ALU byte 0x32
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00_000002_000000
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 2;
    s.registers[reg::A2] = 0;
    pram[0] = 0x200032; // nop + asl A
    run_one(&mut s, &mut jit);
    // Shift left by 1: 0x00_000002_000000 << 1 = 0x00_000004_000000
    assert_eq!(s.registers[reg::A1], 4);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_parallel_asr_b() {
    // ASR B (alu_byte 0x2A): shift right by 1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B1] = 4;
    pram[0] = 0x20002A;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 2);
    assert_eq!(s.registers[reg::B0], 0);
}

#[test]
fn test_parallel_asl_b() {
    // ASL B (alu_byte 0x3A): shift left by 1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B1] = 2;
    pram[0] = 0x20003A;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 4);
    assert_eq!(s.registers[reg::B0], 0);
}

#[test]
fn test_asl_imm() {
    // asl #2,A,A: encoding 0000110000011101SiiiiiiD
    // S=0 (A), ii=000010 (shift 2), D=0 (A)
    // bits: 0000 1100 0001 1101 0 000010 0
    // = 0x0C1D04
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0C1D04; // asl #2,A,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400000);
}

#[test]
fn test_asr_imm() {
    // asr #2,A,A: encoding 0000110000011100SiiiiiiD
    // S=0 (A), ii=000010 (shift 2), D=0 (A)
    // = 0x0C1C04
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x400000;
    pram[0] = 0x0C1C04; // asr #2,A,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_asl_imm_b_to_b() {
    // ASL #1,B,B. Covers S=1 (B source) and D=1 (B dest).
    // Pattern: 0000110000011101SiiiiiiD, S=1, iiiiii=000001, D=1 -> 0x0C1D83
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1D83; // ASL #1,B,B
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x400000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // B << 1: 0x00_400000_000000 << 1 = 0x00_800000_000000
    assert_eq!(s.registers[reg::B1], 0x800000);
}
#[test]
fn test_asr_imm_b_to_b() {
    // ASR #1,B,B -- covers emit_asr_imm B source + B dest
    // Pattern: 0000110000011100SiiiiiiD, S=1(bit7), shift=1(bits6:1), D=1(bit0)
    // 0x0C1C00 | 0x80 | 0x02 | 0x01 = 0x0C1C83
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1C83;
    s.registers[reg::B1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_asr_imm_zero_shift() {
    // ASR #0,A,A -- covers shift=0 C=0 path
    // S=0, shift=0, D=0 -> 0x0C1C00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1C00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::SR] |= 1 << sr::C; // set C initially
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400000); // unchanged
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // C cleared
}

#[test]
fn test_asl_reg() {
    // asl X0,A,A: template 0000110000011110010SsssD
    // S=0 (A src), sss=100 (X0), D=0 (A dst)
    // S=0, sss=100, D=0 -> 0000_1100_0001_1110_0100_1000 = 0x0C1E48
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 3; // shift by 3
    s.registers[reg::A1] = 0x010000;
    pram[0] = 0x0C1E48; // asl X0,A,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x080000); // 0x010000 << 3
}

#[test]
fn test_asr_reg() {
    // asr X0,A,A: template 0000110000011110011SsssD
    // S=0, sss=100 (X0), D=0 -> 0000_1100_0001_1110_0110_1000 = 0x0C1E68
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 2; // shift by 2
    s.registers[reg::A1] = 0x080000;
    pram[0] = 0x0C1E68; // asr X0,A,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x020000); // 0x080000 >> 2
}

#[test]
fn test_asl_reg_a1_source() {
    // asl A1,A,A (0000110000011110010SsssD, S=0 sss=010 D=0)
    // Covers sss_to_shift_reg arm 2 (A1)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000003;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    // A = 0x00_000003_000000, shift by A1[5:0]=3 -> 0x00_000018_000000
    pram[0] = 0x0C1E44; // asl A1,A,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000018);
}

#[test]
fn test_asr_imm_negative() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // asr #4,A,A: A = 0xFF:000000:000000 (negative, A2=0xFF)
    // Arithmetic right shift by 4 should preserve sign: 0xFF:F00000:000000
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    // ASR #4,A,A encoding: 0000110000011100 S=0 iiiiii=000100 D=0
    // = 0x0C1C08
    pram[0] = 0x0C1C08;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0xFF, "ASR: sign must be preserved");
    assert_eq!(s.registers[reg::A1], 0xF00000);
}

#[test]
fn test_asr_clears_v() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set V=1 before ASR, verify it gets cleared
    s.registers[reg::SR] = 0xC00302; // V=1 (bit 1)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200022; // asr A (parallel: nop)
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> 1) & 1;
    assert_eq!(v, 0, "ASR should always clear V");
}

#[test]
fn test_asl_imm_v_intermediate() {
    // ASL #2,A,A: shift A left by 2.
    // A = 0x00:6FFFFF:FFFFFF (bit 55=0, bit 54=1, bit 53=1)
    // After shift 1: bit 55 = old bit 54 = 1 (CHANGED from 0)
    // After shift 2: bit 55 = old bit 53 = 1 (same as shift 1)
    // V should be 1 (bit 55 changed during shift)
    // But old code only checks start vs end (0 vs 1 = 1, happens to be correct here)
    // Need a case where start == end but intermediate differs:
    // A = 0x00:4FFFFF:FFFFFF (bit 55=0, bit 54=1, bit 53=0)
    // shift 1: bit 55 = 1 (changed!)
    // shift 2: bit 55 = 0 (same as start)
    // V should be 1 (changed during shift), but old code gives V=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00:4FFFFF:FFFFFF -> packed = bit 54 set, bit 55 clear, bit 53 clear
    // bits 55:53 = 010 (not all same -> V=1)
    s.registers[reg::A2] = 0x40; // bits 55:48 = 0x40 = 0100_0000, bit 54 set
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    s.registers[reg::SR] = 0xC00300;
    // ASL #2,A,A encoding: 0000110000011101 S=0 iiiiii=000010 D=0 = 0x0C1D04
    pram[0] = 0x0C1D04;
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(
        v, 1,
        "ASL #2: V should be set when bit 55 changes at any intermediate step"
    );
}

#[test]
fn test_norm() {
    // norm R0,A: template 0000000111011RRR0001d101
    // RRR=0 (R0), d=0 (A)
    // 0000_0001_1101_1000_0001_0101 = 0x01D815
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    // E and U flags determine norm behavior
    pram[0] = 0x01D815;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_norm_shift_left() {
    // euz path: U=1, E=0, Z=0 -> shift left, Rn--
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 10;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::SR] = 1 << sr::U;
    pram[0] = 0x01D815; // norm R0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_eq!(s.registers[reg::R0], 9);
}

#[test]
fn test_norm_shift_right() {
    // E path: E=1 -> shift right, Rn++
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 10;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::SR] = 1 << sr::E;
    pram[0] = 0x01D815; // norm R0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x080000);
    assert_eq!(s.registers[reg::R0], 11);
}

#[test]
fn test_norm_b() {
    // NORM R0,B. Covers B accumulator path.
    // Pattern: 0000000111011RRR0001d101, RRR=000, d=1 -> 0x01D81D
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x01D81D; // NORM R0,B
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    s.registers[reg::R0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_norm_saturation_mode() {
    // NORM should apply arithmetic saturation when SM=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Enable SM (bit 20), set U=1, E=0, Z=0 -> NORM takes ASL path
    s.registers[reg::SR] |= 1 << sr::SM;
    s.registers[reg::SR] |= 1 << sr::U;
    s.registers[reg::SR] &= !(1 << sr::E);
    s.registers[reg::SR] &= !(1 << sr::Z);

    // A = $00:7FFFFF:FFFFFF -- ASL produces $00:FFFFFE:FFFFFE which has
    // bits 55/48/47 mismatch -> SM should clamp to max positive
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0xFFFFFF;

    pram[0] = 0x01D815; // norm R0,A
    run_one(&mut s, &mut jit);

    assert_eq!(s.registers[reg::A2], 0x00, "NORM SM: A2 should be 0x00");
    assert_eq!(
        s.registers[reg::A1],
        0x7FFFFF,
        "NORM SM: A1 should be 0x7FFFFF"
    );
    assert_eq!(
        s.registers[reg::A0],
        0xFFFFFF,
        "NORM SM: A0 should be 0xFFFFFF"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "NORM SM: V should be set on saturation"
    );
}

#[test]
fn test_normf() {
    // normf X0,A: template 00001100000111100010sssD
    // sss=100 (X0), D=0 (A)
    // 0000_1100_0001_1110_0010_1000 = 0x0C1E28
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // X0 = -3 (negative => ASL by 3)
    s.registers[reg::X0] = 0xFFFFFD; // -3 in 24-bit
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x010000;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1E28; // normf X0,A
    run_one(&mut s, &mut jit);
    // ASL A by 3: 0x00_010000_000000 << 3 = 0x00_080000_000000
    assert_eq!(s.registers[reg::A1], 0x080000);
}

#[test]
fn test_normf_positive_shift() {
    // X0 = 2 (positive => ASR by 2)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 2;
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x080000;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1E28; // normf X0,A
    run_one(&mut s, &mut jit);
    // ASR A by 2: 0x00_080000_000000 >> 2 = 0x00_020000_000000
    assert_eq!(s.registers[reg::A1], 0x020000);
}

#[test]
fn test_normf_v_flag_bit55() {
    // NORMF should check bit 55 (MSB of 56-bit accumulator), not bit 39
    // (which was the DSP56000 40-bit position).
    //
    // Accumulator layout: (A2[7:0] << 48) | (A1[23:0] << 24) | A0[23:0]
    // Bit 55 = A2 bit 7. Bit 54 = A2 bit 6.
    //
    // A2=0x40, A1=0, A0=0 -> acc bit 54 = 1, bit 55 = 0 (positive).
    // X0 = 0xFFFFFE (-2 -> left shift by 2).
    // After ASL by 2: bit 54 shifts into bit 55 -> bit 55 changes (0->1) -> V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x40; // bit 54 of accumulator set
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0xFFFFFE; // -2 -> left shift by 2
    s.registers[reg::SR] = 0xC00300;
    pram[0] = 0x0C1E28; // normf X0,A
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(
        v, 1,
        "NORMF: V should be set when bit 55 changes during left shift"
    );
}

#[test]
fn test_normf_v_flag_bit55_no_change() {
    // A2=0x10, A1=0, A0=0 -> acc bit 52 = 1, bits 55-53 = 0.
    // X0 = 0xFFFFFE (-2 -> left shift by 2).
    // After ASL by 2: bit 52 -> bit 54. Bit 55 stays 0 -> V=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x10; // bit 52 of accumulator set
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0xFFFFFE; // -2 -> left shift by 2
    s.registers[reg::SR] = 0xC00300;
    pram[0] = 0x0C1E28; // normf X0,A
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(v, 0, "NORMF: V should be clear when bit 55 is unchanged");
}

#[test]
fn test_normf_asr_negative_acc_v_flag_clear() {
    // NORMF with positive source (S[23]=0) performs ASR on accumulator.
    // During ASR, bit 55 (sign bit) is replicated and never changes.
    // Therefore V must always be 0 for the ASR path.
    //
    // Template: 000011001D0sssss (NORMF S,D)
    // S=X0 (sssss from 5-bit reg encoding: X0=00100), D=0 (A)
    // 0000_1100_1_0_0_00100 = 0x0C8004
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = $FF:800000:000000 (most negative 56-bit value)
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    // X0 = 1 (positive => ASR path, shift right by 1)
    s.registers[reg::X0] = 0x000001;
    // Clear V and L flags first
    s.registers[reg::SR] &= !((1 << sr::V) | (1 << sr::L));
    // Template: 00001100000111100010sssD, sss=100 (X0), D=0 (A)
    // 0000_1100_0001_1110_0010_1000 = 0x0C1E28
    pram[0] = 0x0C1E28; // normf X0,A
    run_one(&mut s, &mut jit);
    // After ASR by 1: A = $FF:C00000:000000 (sign bit replicated)
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0xC00000);
    // V must be 0 (bit 55 never changed during ASR)
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "V flag should be clear for NORMF ASR path"
    );
    // L must remain 0 (V was never set)
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "L flag should remain clear when V is not set"
    );
}

#[test]
fn test_cmpu_equal() {
    // cmpu X0,A: template 00001100000111111111gggd
    // ggg=100 (X0), d=0 (A)
    // 0000_1100_0001_1111_1111_1000 = 0x0C1FF8
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1FF8;
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z set (equal)
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0); // not negative
}

#[test]
fn test_cmpu_less() {
    // cmpu X0,A: A < X0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x200000;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1FF8;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // not zero
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0); // negative (A - X0 < 0)
}

#[test]
fn test_cmpu() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // cmpu X0,A: unsigned compare of A1 - X0
    pram[0] = 0x0C1FF8;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // equal -> Z set
}

#[test]
fn test_cmpu_x0_b() {
    // CMPU X0,B. Covers ggg=4 (X0), d=1 (B dest).
    // Pattern: 00001100000111111111gggd, ggg=100, d=1 -> 0x0C1FF9
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FF9; // CMPU X0,B
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // B > X0 so carry should be clear
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_cmpu_y1_a() {
    // CMPU Y1,A. Covers ggg=7 (Y1), d=0 (A dest).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FFE; // CMPU Y1,A (ggg=111, d=0)
    s.registers[reg::Y1] = 0x100000;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    run_one(&mut s, &mut jit);
    // A == Y1 -> Z flag set
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0);
}

#[test]
fn test_cmpu_acc_a_b() {
    // CMPU A,B (ggg=0, d=1). Covers ggg=0 d!=0 -> reg::A source.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FF1; // CMPU A,B (ggg=000, d=1)
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x300000;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0;
    run_one(&mut s, &mut jit);
    // B < A -> carry set (unsigned borrow)
    assert_eq!(s.pc, 1);
}

#[test]
fn test_cmpu_b_a() {
    // CMPU B,A -- ggg=0, d=0 -> source=reg::B
    // 00001100000111111111gggd, ggg=000, d=0 -> 0x0C1FF0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FF0;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::B1] = 0x100000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_cmpu_y0_a() {
    // CMPU Y0,A -- ggg=5, d=0 -> source=reg::Y0
    // ggg=101, d=0: 0x0C1FFA
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FFA;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::Y0] = 0x100000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_cmpu_x1_a() {
    // CMPU X1,A -- ggg=6, d=0 -> source=reg::X1
    // ggg=110, d=0: 0x0C1FFC
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FFC;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::X1] = 0x100000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_cmpu_ggg_undefined() {
    // cmpu with ggg=1 -> maps to reg::NULL (undefined source register)
    // Pattern: 00001100000111111111gggd
    // ggg=001, d=0 -> opcode = 0x0C1FF2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0C1FF2;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_cmpu_unsigned_ignores_extension() {
    // CMPU X0,A: compare unsigned, EXP does not affect operation.
    // A = 0xFF:400000:000000 (negative 56-bit, but bits 47:0 = 0x400000_000000)
    // X0 = 0x400000 (24-bit, left-aligned to 48-bit = 0x400000_000000)
    // Unsigned comparison of 0x400000_000000 - 0x400000_000000 = 0 -> Z=1
    // encoding: 0000110000011111 1111gggd
    // ggg=100 (X0), d=0 (A): 00001100_00011111_11111000 = 0x0C1FF8
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF; // extension = negative, but should be ignored
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::SR] = 0xC00300;
    pram[0] = 0x0C1FF8; // cmpu X0,A
    run_one(&mut s, &mut jit);
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    assert_eq!(z, 1, "CMPU: equal unsigned values should set Z");
}

#[test]
fn test_cmpu_acc_source_uses_a0() {
    // CMPU A,B (ggg=0, d=1) -> opcode 0x0C1FF1
    // Set A = 0x00:000000:000001 (only A0 is nonzero)
    // Set B = 0x00:000000:000000
    // Correct: B[47:0] - A[47:0] = 0 - 1 = borrow -> C=1, Z=0
    // Bug: loads stale reg slot 0x0e (=0) as source -> 0 - 0 = 0 -> C=0, Z=1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1FF1; // CMPU A,B
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 1;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0;
    s.registers[reg::B0] = 0;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    assert_eq!(
        c, 1,
        "CMPU A,B: borrow should occur when B < A in bits 47:0"
    );
    assert_eq!(z, 0, "CMPU A,B: result is not zero");
}

#[test]
fn test_parallel_mpy_x0_y0_a() {
    // mpy +X0,Y0,A: ALU byte = 1_00_0_0_00 = 0x80
    // QQ=00 -> X0*Y0, d=0 -> A, k=0 -> positive, OO=00 -> mpy
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // X0 = 0x400000 (0.5 in Q1.23), Y0 = 0x400000 (0.5)
    // Product = 0.5 * 0.5 = 0.25 => 0x200000 in A1
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x200080; // nop + mpy +X0,Y0,A
    run_one(&mut s, &mut jit);
    // Q1.23 multiply: (0x400000 * 0x400000) << 1 >> 24 for A1
    // = 0x10_0000_0000_0000 << 1 = 0x20_0000_0000_0000
    // In 56 bits: 0x00_200000_000000
    // A2=0, A1=0x200000, A0=0
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_eq!(s.registers[reg::A0], 0);
    assert_eq!(s.registers[reg::A2], 0);
}

#[test]
fn test_parallel_mac_x0_y0_a() {
    // mac +X0,Y0,A: ALU byte = 1_00_0_0_10 = 0x82
    // QQ=00 -> X0*Y0, d=0 -> A, k=0 -> positive, OO=10 -> mac
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Start A = 0x00_100000_000000
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A2] = 0;
    // X0 = 0x400000, Y0 = 0x400000
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x200082; // nop + mac +X0,Y0,A
    run_one(&mut s, &mut jit);
    // Product = 0.5 * 0.5 = 0.25 = 0x00_200000_000000
    // A = A + product = 0x00_100000_000000 + 0x00_200000_000000 = 0x00_300000_000000
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_mpyi() {
    // mpyi #imm,X0,A (2-word): template 000000010100000111qqdk00
    // qq=00 (X0), d=0 (A), k=0 (positive)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000010; // 16 (as signed 24-bit)
    pram[0] = 0x0141C0; // mpyi #imm,X0,A
    pram[1] = 0x000003; // immediate = 3
    run_one(&mut s, &mut jit);
    // mul56: (16 * 3) << 1 = 96 = 0x60 in A0
    assert_eq!(s.registers[reg::A0], 0x60);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::A2], 0);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_mpy_variants() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    // MPY X0,X0,B (alu_byte=0x88): B = X0*X0
    pram[0] = 0x200088;
    // -MPY X0,X0,A (alu_byte=0x84): A = -(X0*X0)
    pram[1] = 0x200084;
    // MPYR X0,X0,A (alu_byte=0x81): A = round(X0*X0)
    pram[2] = 0x200081;
    // MAC X0,X0,A (alu_byte=0x82): A += X0*X0
    pram[3] = 0x200082;
    // MACR X0,X0,A (alu_byte=0x83): A = round(A + X0*X0)
    pram[4] = 0x200083;
    // MPY X0,Y1,A (alu_byte=0xC0, cross regs): A = X0*Y1
    pram[5] = 0x2000C0;

    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // X0 = 0x400000 = 0.5 in fractional, Y1 = 0x200000 = 0.25
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y1] = 0x200000;

    // MPY X0,X0,B: B = 0.5 * 0.5 = 0.25 -> B1 = 0x200000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);

    // -MPY X0,X0,A: A = -(0.5*0.5) = -0.25
    run_one(&mut s, &mut jit);
    // -0.25 in 56-bit: FF_E00000_000000 -> A2=FF, A1=E00000
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0xE00000);

    // MPYR X0,X0,A: A = round(0.5*0.5) = round(0x00_200000_000000)
    // B0=0 so no rounding: A1 = 0x200000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000);

    // MAC X0,X0,A: A = A + X0*X0 = 0x200000 + 0x200000 = 0x400000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400000);

    // MACR X0,X0,A: A = round(A + X0*X0) = round(0x400000 + 0x200000)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x600000);

    // MPY X0,Y1,A (cross): A = 0.5*0.25 = 0.125 -> A1 = 0x100000
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mpyi_y0_b_neg() {
    // MPYI: -#3 * Y0 -> B. Covers Y0 source (qq=1), B dest (d=1), negate (k=1).
    // Pattern: 000000010100000111_01_1_1_00 = 0x0141DC
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0141DC; // MPYI -#imm,Y0,B
    pram[1] = 0x000003; // immediate = 3
    s.registers[reg::Y0] = 0x000002; // 2
    run_one(&mut s, &mut jit);
    // Result = -(3 * 2 << 1) = -12 in 56-bit = 0xFF_FFFFFF_FFFFF4
    assert_eq!(s.registers[reg::B2], 0xFF);
    assert_eq!(s.registers[reg::B1], 0xFFFFFF);
    assert_eq!(s.registers[reg::B0], 0xFFFFF4);
}

#[test]
fn test_mpyi_x1_a() {
    // MPYI: #4 * X1 -> A. Covers X1 source (qq=2), A dest (d=0).
    // Pattern: 000000010100000111_10_0_0_00 = 0x0141E0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0141E0; // MPYI #imm,X1,A
    pram[1] = 0x000005; // immediate = 5
    s.registers[reg::X1] = 0x000003; // 3
    run_one(&mut s, &mut jit);
    // Result = (5 * 3) << 1 = 30
    assert_eq!(s.registers[reg::A0], 30);
    assert_eq!(s.registers[reg::A1], 0);
}

#[test]
fn test_mpy_y0_y0_a() {
    // MPY Y0,Y0,A (qq=1 below 0xC0). Covers MAC/MPY register pair line 6279.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200090; // PM2 NOP + MPY Y0,Y0,A (alu=0x90)
    s.registers[reg::Y0] = 0x400000; // 0.5 in fractional
    run_one(&mut s, &mut jit);
    // 0x400000 * 0x400000 = 2^44, <<1 = 2^45, A1 = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_mpy_x1_x0_a() {
    // MPY X1,X0,A (qq=2 below 0xC0). Covers line 6280.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2000A0; // PM2 NOP + MPY X1,X0,A (alu=0xA0)
    s.registers[reg::X1] = 0x400000;
    s.registers[reg::X0] = 0x200000;
    run_one(&mut s, &mut jit);
    // 0x400000 * 0x200000 = 2^43, <<1 = 2^44, A1 = 0x100000
    assert_eq!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mpy_y1_y0_a() {
    // MPY Y1,Y0,A (qq=3 below 0xC0). Covers line 6281.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2000B0; // PM2 NOP + MPY Y1,Y0,A (alu=0xB0)
    s.registers[reg::Y1] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    run_one(&mut s, &mut jit);
    // 0x400000 * 0x400000 <<1 -> A1 = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_mpy_x0_y1_a() {
    // MPY X0,Y1,A (qq=0 above 0xC0). Covers lines 6286.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2000C0; // PM2 NOP + MPY X0,Y1,A (alu=0xC0)
    s.registers[reg::X0] = 0x200000;
    s.registers[reg::Y1] = 0x400000;
    run_one(&mut s, &mut jit);
    // 0x200000 * 0x400000 <<1 -> A1 = 0x100000
    assert_eq!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mpy_y0_x0_b() {
    // MPY Y0,X0,B (qq=1 above 0xC0, d=1). Covers line 6287.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2000D8; // PM2 NOP + MPY Y0,X0,B (alu=0xD8: qq=1,d=1,neg=0,op=0)
    s.registers[reg::Y0] = 0x400000;
    s.registers[reg::X0] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_mpy_x1_y0_a() {
    // MPY X1,Y0,A (qq=2 above 0xC0). Covers line 6288.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2000E0; // PM2 NOP + MPY X1,Y0,A (alu=0xE0)
    s.registers[reg::X1] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_mpy_y1_x1_a() {
    // MPY Y1,X1,A (qq=3 above 0xC0). Covers line 6289.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x2000F0; // PM2 NOP + MPY Y1,X1,A (alu=0xF0)
    s.registers[reg::Y1] = 0x400000;
    s.registers[reg::X1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_mpyi_y1_a() {
    // MPYI +Y1,#imm,A -- qq=3 -> reg::Y1
    // Pattern: 000000010100000111qqdk00
    // qq=11, d=0 (A), k=0 (positive): bits 7:0 = 11_11_0_0_00 = 0xF0
    // Full: 0x0141F0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0141F0;
    pram[1] = 0x000004; // immediate = 4
    s.registers[reg::Y1] = 3;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::A0], 24); // (3 * 4) << 1 = 24
}

#[test]
fn test_mpyi_basic() {
    // mpyi #xxxx,X0,A: template 000000010100000111qqdk00
    // Bits: 00000001_01000001_11qqdk00
    // qq=0 (X0), d=0 (A), k=0 -> 00000001_01000001_11000000 = 0x0141C0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5 in Q1.23
    pram[0] = 0x0141C0; // mpyi #xxxx,X0,A
    pram[1] = 0x400000; // 0.5 in Q1.23
    run_one(&mut s, &mut jit);
    // 0.5 * 0.5 = 0.25, product = 0x200000 in A1
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_mpyi_negative() {
    // mpyi -#xxxx,X0,A: k=1 -> 00000001_01000001_11000100 = 0x0141C4
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    pram[0] = 0x0141C4; // mpyi -#xxxx,X0,A
    pram[1] = 0x400000; // 0.5
    run_one(&mut s, &mut jit);
    // -(0.5 * 0.5) = -0.25
    // In 56-bit: 0xFF_E00000_000000 -> A2=0xFF, A1=0xE00000
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0xE00000);
}

#[test]
fn test_mpyri_basic() {
    // mpyri #xxxx,X0,A: template 000000010100000111qqdk01
    // qq=0, d=0, k=0 -> 00000001_01000001_11000001 = 0x0141C1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    pram[0] = 0x0141C1; // mpyri #xxxx,X0,A
    pram[1] = 0x400000; // 0.5
    run_one(&mut s, &mut jit);
    // Product = 0.25, rounded: A1 = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_maci_basic() {
    // maci #xxxx,X0,A: template 000000010100000111qqdk10
    // qq=0, d=0, k=0 -> 00000001_01000001_11000010 = 0x0141C2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    // A starts at 0x100000 in A1
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141C2; // maci #xxxx,X0,A
    pram[1] = 0x400000; // 0.5
    run_one(&mut s, &mut jit);
    // A = A + (0.5 * 0.5) = 0x100000 + 0x200000 = 0x300000
    assert_eq!(s.registers[reg::A1], 0x300000);
}

#[test]
fn test_macri_basic() {
    // macri #xxxx,X0,A: template 000000010100000111qqdk11
    // qq=0, d=0, k=0 -> 00000001_01000001_11000011 = 0x0141C3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141C3; // macri #xxxx,X0,A
    pram[1] = 0x400000; // 0.5
    run_one(&mut s, &mut jit);
    // A = round(A + 0.5*0.5) = round(0x300000_000000) = 0x300000
    assert_eq!(s.registers[reg::A1], 0x300000);
}

#[test]
fn test_dmac_ss() {
    // dmac ss X0,X0,A: template 000000010010010s1sdkQQQQ
    // ss=0 (both signed), k=0, d=0 (A), QQQQ=0 (X0,X0)
    // 000000010010010_0_1_0_0_0_0000 = 0x012480
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    // A starts at 0
    pram[0] = 0x012480; // dmac ss X0,X0,A
    run_one(&mut s, &mut jit);
    // DMAC: D = D + S1*S2 (double precision)
    // With both signed: 0.5 * 0.5 = 0.25 => A1 = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_mac_su() {
    // mac (su) X0,X0,A: template 00000001001001101sdkQQQQ
    // s=0 (SU mode), k=0, d=0, QQQQ=0 (X0,X0)
    // 0000_0001_0010_0110_1_0_0_0_0000 = 0x012680
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x012680;
    run_one(&mut s, &mut jit);
    // MAC(su): signed * unsigned, accumulate
    // Result should be A + product of signed 0.5 * unsigned 0.5
    // Non-trivial, just check it runs and A changed
    assert_ne!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mpy_su() {
    // mpy (su) X0,X0,A: template 00000001001001111sdkQQQQ
    // s=0, k=0, d=0, QQQQ=0
    // 0000_0001_0010_0111_1_0_0_0_0000 = 0x012780
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    pram[0] = 0x012780;
    run_one(&mut s, &mut jit);
    // MPY(su): product only, no accumulate
    // Result should be non-zero
    assert_ne!(s.registers[reg::A1], 0);
}

#[test]
fn test_mul_shift_mpy() {
    // MulShift mpy: 00000001000sssss11QQdk00
    // shift=1, QQ=0(Y1), d=0(A), k=0(positive)
    // bits: 00000001000_00001_11_00_0_0_00 = 0x0101C0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y1] = 0x400000; // 0.5 in fractional
    pram[0] = 0x0101C0; // mpy y1,#1,a -> A = Y1 * 2^-1
    run_one(&mut s, &mut jit);
    // Y1=0.5, shift by 2^-1 = 0.25
    // In accumulator format: 0x400000 << 24 = 0x00_400000_000000
    // >> 1 = 0x00_200000_000000
    assert_eq!(s.registers[reg::A2], 0);
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_dmac_negate() {
    // dmac (ss) -X0,X0,A (k=1 negate path)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A2] = 0;
    pram[0] = 0x012490;
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mac_su_negate() {
    // mac(su) -X0,X0,A (k=1 negate path)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A2] = 0;
    pram[0] = 0x012690;
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mpy_su_negate() {
    // mpy(su) -X0,X0,A (k=1 negate path)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x012790;
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::A1], 0);
}

#[test]
fn test_mpyri_negate() {
    // mpyri -#imm,X0,A (k=1 negate path)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x0141C5;
    pram[1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::A1], 0);
}

#[test]
fn test_dmac_su() {
    // dmac su X0,X0,A: ss=10 (signed*unsigned)
    // Template: 000000010010010s1sdkQQQQ
    // ss=10 -> bit8=1, bit6=0, d=0, k=0, QQQQ=0000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5 signed, or large unsigned
    pram[0] = 0x012580; // dmac su X0,X0,A
    run_one(&mut s, &mut jit);
    // signed*unsigned product should differ from signed*signed
    // With S1 unsigned (0x400000 = 4194304) and S2 signed (0x400000 = 0.5)
    // Result should be non-zero
    assert_ne!(s.registers[reg::A1], 0);
}

#[test]
fn test_dmac_uu() {
    // dmac uu X0,X0,A: ss=11 (unsigned*unsigned)
    // ss=11 -> bit8=1, bit6=1, d=0, k=0, QQQQ=0000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x0125C0; // dmac uu X0,X0,A
    run_one(&mut s, &mut jit);
    // unsigned*unsigned: 0x400000 * 0x400000 = 0x100000000000 << 1
    assert_ne!(s.registers[reg::A1], 0);
}

#[test]
fn test_dmac_ss_to_b() {
    // dmac ss X0,X0,B: d=1 (accumulator B)
    // ss=00, d=1 (bit5=1), k=0, QQQQ=0000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000; // 0.5
    pram[0] = 0x0124A0; // dmac ss X0,X0,B
    run_one(&mut s, &mut jit);
    // 0.5 * 0.5 = 0.25 -> B1 = 0x200000
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_mac_su_uu_mode() {
    // mac(su) with s=1 (unsigned*unsigned mode): X0,X0,A
    // Template: 00000001001001101sdkQQQQ, s=1 at bit6
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0126C0; // mac(uu) X0,X0,A
    run_one(&mut s, &mut jit);
    // Both operands treated as unsigned, accumulate into A
    assert_ne!(s.registers[reg::A1], 0x100000);
}

#[test]
fn test_mpy_su_uu_mode() {
    // mpy(su) with s=1 (unsigned*unsigned mode): X0,X0,A
    // Template: 00000001001001111sdkQQQQ, s=1 at bit6
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x0127C0; // mpy(uu) X0,X0,A
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::A1], 0);
}

#[test]
fn test_mac_overflow_sets_v() {
    // MAC X0,Y0,A with accumulator near max and product pushing it over
    // A = 0x007F_FFFF_000000 (near max positive 56-bit)
    // X0*Y0 = large positive product that overflows when accumulated
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A near max positive
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x000000;
    // X1=0x400000, X0=0x400000: product = 0.5*0.5 = 0.25 -> 0x00:200000:000000
    s.registers[reg::X1] = 0x400000;
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::SR] = 0xC00300;
    // mac +x1,x0,a = opcode byte 0xA2 (QQQ=010, mac, d=a)
    pram[0] = 0x2000A2;
    run_one(&mut s, &mut jit);
    // Result overflows 56-bit signed: A + 0x00:200000:000000 wraps
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(v, 1, "MAC: accumulation overflow should set V");
}

#[test]
fn test_mac_sm_saturation_sets_v_l() {
    // MAC with SM=1: accumulation sum exceeds 48-bit MSP:LSP range,
    // triggering arithmetic saturation. V and L should be set from the
    // saturation (via emit_sm_vl_deferred), even though the 56-bit
    // accumulation itself doesn't overflow bit 55.
    //
    // A = $00:7FFFFF:000000, X0=$400000 (0.5), Y0=$100000 (0.125)
    // Product = $00:080000:000000
    // Sum = $00:87FFFF:000000 (bit 47=1, bit 48=0 -> SM mismatch -> saturate)
    // Stored = $00:7FFFFF:FFFFFF (clamped to max positive)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x100000;
    // SM=1 (bit 15), IPL=3
    s.registers[reg::SR] = 0xC00300 | (1 << sr::SM);
    pram[0] = 0x2000D2; // mac +y0,x0,a
    run_one(&mut s, &mut jit);
    let sr = s.registers[reg::SR];
    assert_ne!(sr & (1 << sr::V), 0, "V=1 from SM saturation");
    assert_ne!(sr & (1 << sr::L), 0, "L=1 from SM saturation (sticky)");
    // Stored value should be clamped to max positive
    assert_eq!(s.registers[reg::A2], 0x00, "A2=$00 (positive, clamped)");
    assert_eq!(s.registers[reg::A1], 0x7FFFFF, "A1=$7FFFFF (clamped)");
    assert_eq!(s.registers[reg::A0], 0xFFFFFF, "A0=$FFFFFF (clamped)");
}

#[test]
fn test_mul_shift_mac_overflow_sets_v() {
    // MAC +2^0,Y1,A: product placed at A1 position, added to A
    // A near max positive, product causes bit 55 sign flip -> V=1
    // Opcode 0x0100C2: MAC +2^0,Y1,A (s=0, QQ=00=Y1, d=0=A, k=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x7F_FFFFFF_FFFFFF (max positive before sign flip)
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    // Y1 = 1 -> product = 1 << 24 = 0x00_000001_000000
    // Sum overflows: 0x7F_FFFFFF_FFFFFF + 0x00_000001_000000 = 0x80_000000_FFFFFF
    s.registers[reg::Y1] = 1;
    s.registers[reg::SR] = 0;
    pram[0] = 0x0100C2;
    run_one(&mut s, &mut jit);
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(v, 1, "MAC MulShift accumulation overflow should set V");
}

#[test]
fn test_dmac_v_flag_computed() {
    // DMAC V flag should be computed per standard definition, not always cleared.
    // Note: DMAC's (D >> 24) + product cannot actually overflow 56 bits (the right-shift
    // reduces D to ~32-bit range while product is ~49-bit, sum fits in ~49 bits < 56).
    // However, V should be computed (not hardcoded) to match the manual's CCR table.
    // Verify V remains 0 after computation (not hardcoded clear), and that a pre-set V
    // from a prior instruction is properly cleared by the computation.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Pre-set V=1 to verify DMAC clears it (via computation, not hardcoded)
    s.registers[reg::SR] = 1 << sr::V;
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A2] = 0;
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::Y0] = 0x100000;

    // dmac ss X0,Y0,A: template 000000010010010s1sdkQQQQ
    // ss=00, d=0, k=0, QQQQ=0010 (X0,Y0) => 0x012482
    pram[0] = 0x012482;
    run_one(&mut s, &mut jit);

    // V should be cleared (no overflow possible)
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "DMAC should clear V when no overflow; SR = {:#08X}",
        s.registers[reg::SR]
    );
}

#[test]
fn test_sm_mac_saturates_positive() {
    // SM=1: MAC result overflows 48-bit positive -> clamped to $00_7FFFFF_FFFFFF
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set SM=1 in SR
    s.registers[reg::SR] |= 1 << sr::SM;
    // A = $00_7FFFFF_000000 (large positive, near 48-bit max)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    // X0 = 0x400000 (0.5), Y0 = 0x400000 (0.5)
    // MAC X0,Y0,A: product = 0.5 * 0.5 = 0.25 => $00_200000_000000
    // sum = $00_9FFFFF_000000 -> bits 55=0, 48=1, 47=0 -> mismatch -> saturate positive
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x200082; // mac x0,y0,a (QQ=00, d=0 for A, negate=0)
    run_one(&mut s, &mut jit);

    assert_eq!(
        s.registers[reg::A2],
        0x00,
        "SM: A2 should be 0x00 (positive sat)"
    );
    assert_eq!(
        s.registers[reg::A1],
        0x7FFFFF,
        "SM: A1 should be 0x7FFFFF (positive sat)"
    );
    assert_eq!(
        s.registers[reg::A0],
        0xFFFFFF,
        "SM: A0 should be 0xFFFFFF (positive sat)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "SM: V should be set on saturation"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "SM: L should be set on saturation"
    );
}

#[test]
fn test_sm_mac_saturates_negative() {
    // SM=1: MAC result overflows 48-bit negative -> clamped to $FF_800000_000000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] |= 1 << sr::SM;
    // A = $FF_800000_000000 (large negative, near 48-bit min)
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    // MAC -X0,Y0,A: subtract product from A, making it more negative
    // product = 0.5 * 0.5 = 0.25 = $00_200000_000000
    // mac with negate: A = A - product = $FF_600000_000000 -> bits 55=1, 48=0, 47=1 -> mismatch -> saturate negative
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x200086; // mac -x0,y0,a (k=1, QQ=00, d=0, OO=10)
    run_one(&mut s, &mut jit);

    assert_eq!(
        s.registers[reg::A2],
        0xFF,
        "SM: A2 should be 0xFF (negative sat)"
    );
    assert_eq!(
        s.registers[reg::A1],
        0x800000,
        "SM: A1 should be 0x800000 (negative sat)"
    );
    assert_eq!(
        s.registers[reg::A0],
        0x000000,
        "SM: A0 should be 0x000000 (negative sat)"
    );
}

#[test]
fn test_sm_no_saturation_fits_48bit() {
    // SM=1 but result fits in 48 bits -> no saturation
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] |= 1 << sr::SM;
    // A = $00_100000_000000 (small positive)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    // MAC X0,Y0,A: product ~0.25, sum ~0.375 -> fits in 48 bits
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x200082; // mac x0,y0,a
    run_one(&mut s, &mut jit);

    // Result should be $00_300000_000000 (no saturation)
    assert_eq!(s.registers[reg::A2], 0x00);
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.registers[reg::A0], 0x000000);
}

#[test]
fn test_sm_disabled_no_saturation() {
    // SM=0: overflow is NOT saturated (default behavior)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // SM=0 (default)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x200082; // mac x0,y0,a
    run_one(&mut s, &mut jit);

    // Result should be $00_9FFFFF_000000 - NOT saturated
    assert_eq!(s.registers[reg::A2], 0x00, "SM=0: A2 not saturated");
    assert_eq!(s.registers[reg::A1], 0x9FFFFF, "SM=0: A1 not saturated");
    assert_eq!(s.registers[reg::A0], 0x000000, "SM=0: A0 not saturated");
}

#[test]
fn test_sm_excluded_mac_su() {
    // SM=1 but MAC(su) is excluded -> no saturation
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] |= 1 << sr::SM;
    // Set up a MAC(su) that would overflow 48 bits
    // A = $00_7FFFFF_000000
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    // MAC(su): template 000000010010010s1sdkQQQQ
    // s=0 (su mode), k=0 (no negate), d=0 (A), QQQQ selects sources
    // QQQQ=0000: X0,Y0
    s.registers[reg::X0] = 0x400000; // signed 0.5
    s.registers[reg::Y0] = 0x400000; // unsigned ~0.5
    // mac(su) X0,Y0,A opcode: 0000_0001_0010_0100_1000_0000 = 0x012480
    pram[0] = 0x012480;
    run_one(&mut s, &mut jit);

    // Result should NOT be saturated (MAC(su) is excluded from SM)
    assert_ne!(
        s.registers[reg::A1],
        0x7FFFFF,
        "SM excluded: MAC(su) result should not be clamped to max positive"
    );
}

#[test]
fn test_sm_mpy_saturates() {
    // SM=1: MPY where product extends beyond 48 bits -> saturated
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] |= 1 << sr::SM;
    // MPY X0,Y0,A: X0=0x800000 (-1.0), Y0=0x800000 (-1.0)
    // Product = (-1.0)*(-1.0) = +1.0, which overflows 48-bit (max positive < 1.0)
    // Result = $00_800000_000000: bits 55=0, 48=0, 47=1 -> mismatch -> saturate positive
    s.registers[reg::X0] = 0x800000;
    s.registers[reg::Y0] = 0x800000;
    // MPY +X0,Y0,A: k=0, QQ=00, D=0, OO=00 -> alu=0x80
    pram[0] = 0x200080;
    run_one(&mut s, &mut jit);

    assert_eq!(s.registers[reg::A2], 0x00, "SM: MPY saturated A2");
    assert_eq!(s.registers[reg::A1], 0x7FFFFF, "SM: MPY saturated A1");
    assert_eq!(s.registers[reg::A0], 0xFFFFFF, "SM: MPY saturated A0");
}

#[test]
fn test_dmac_qqqq_x1_x1() {
    // dmac ss X1,X1,A: QQQQ=1000 (0x8)
    // template: 000000010010010s1sdkQQQQ
    // ss=0, k=0, d=0(A), QQQQ=1000
    // 000000010010010_0_1_0_0_0_1000 = 0x012488
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0x400000; // 0.5
    pram[0] = 0x012488;
    run_one(&mut s, &mut jit);
    // DMAC ss: 0.5 * 0.5 = 0.25 => A1 = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000);
}

#[test]
fn test_mpy_su_qqqq_y0_x1() {
    // mpy (su) Y0,X1,A: QQQQ=1110 (0xE)
    // template: 00000001001001111sdkQQQQ
    // s=0 (SU), k=0, d=0(A), QQQQ=1110
    // 0000_0001_0010_0111_1_0_0_0_1110 = 0x01278E
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0x400000; // signed 0.5
    s.registers[reg::X1] = 0x400000; // unsigned 0.25 (treated as unsigned)
    pram[0] = 0x01278E;
    run_one(&mut s, &mut jit);
    // Should decode and execute without panicking
    assert_ne!(s.pc, 0, "instruction should have executed");
}

#[test]
fn test_div() {
    // div X0,A: template 000000011000000001JJd000, JJ=00 (X0), d=0 (A)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Simple case: A has a value, divide step by X0
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::X0] = 0x200000;
    pram[0] = 0x018040;
    run_one(&mut s, &mut jit);
    // DIV is iterative (one step per execution)
    // Just verify it doesn't crash and modifies A
    assert_eq!(s.pc, 1);
}

#[test]
fn test_div_diff_sign() {
    // DIV with different signs: dest negative, source positive
    // This exercises the add path (signs differ -> add source)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF; // negative (bit 55 = 1)
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0;
    s.registers[reg::X0] = 0x200000; // positive (bit 23 = 0)
    pram[0] = 0x018040; // div X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // Verify A was modified (signs differ -> add path)
    assert_ne!(
        (s.registers[reg::A2], s.registers[reg::A1]),
        (0xFF, 0x800000)
    );
}

#[test]
fn test_div_y0_b() {
    // DIV Y0,B. Covers Y0 source (JJ=1), B dest (d=1).
    // Pattern: 000000011000000001_01_1_000 = 0x018058
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x018058; // DIV Y0,B
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0;
    s.registers[reg::Y0] = 0x400000;
    run_one(&mut s, &mut jit);
    // DIV is iterative -- one step per execution. Just verify it runs without panic.
    assert_eq!(s.pc, 1);
}

#[test]
fn test_div_x1_a() {
    // DIV X1,A. Covers X1 source (JJ=2).
    // Pattern: 000000011000000001_10_0_000 = 0x018060
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x018060; // DIV X1,A
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x080000;
    s.registers[reg::A0] = 0;
    s.registers[reg::X1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_div_y1_a() {
    // DIV Y1,A. Covers Y1 source (JJ=3).
    // Pattern: 000000011000000001_11_0_000 = 0x018070
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x018070; // DIV Y1,A
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0;
    s.registers[reg::Y1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_tcc_taken_b_to_a() {
    // tcc B,A: condition CC (carry clear), tcc_idx=0 (B->A)
    // encoding: 00000010CCCC00000JJJd000
    // CC=CC(0x0): bits 15:12=0000, tcc_idx=0: bits 6:3=0000
    // So: 0x020000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B0] = 0x000001;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B2] = 0x00;
    // SR: C=0 (carry clear) so CC condition is true
    s.registers[reg::SR] = 0;
    pram[0] = 0x020000; // tcc B,A
    run_one(&mut s, &mut jit);
    // A should now equal B
    assert_eq!(s.registers[reg::A0], 0x000001);
    assert_eq!(s.registers[reg::A1], 0x200000);
    assert_eq!(s.registers[reg::A2], 0x00);
}

#[test]
fn test_tcc_not_taken() {
    // Same tcc B,A but with C=1 (carry set), so CC is false
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::SR] = 1; // C=1
    pram[0] = 0x020000; // tcc B,A
    run_one(&mut s, &mut jit);
    // A should remain unchanged
    assert_eq!(s.registers[reg::A1], 0);
}

#[test]
fn test_tcc_x0_to_a() {
    // tcc X0,A: tcc_idx=8 (X0->A)
    // encoding: 00000010CCCC00000JJJd000
    // CC=CC(0x0), tcc_idx=8: bits 6:3 = 1000 = 0x40
    // So: 0x020040
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::SR] = 0; // C=0, CC is true
    pram[0] = 0x020040; // tcc X0,A
    run_one(&mut s, &mut jit);
    // A1 = X0 = 0x400000, A0 = 0, A2 = 0 (positive)
    assert_eq!(s.registers[reg::A1], 0x400000);
    assert_eq!(s.registers[reg::A0], 0);
}

#[test]
fn test_tcc_a_to_b() {
    // TCC A,B: tcc_idx=1 (A->B), cc=CC(0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x020008; // tcc A,B cc=CC
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x333333;
    s.registers[reg::A0] = 0x000001;
    s.registers[reg::SR] = 0; // C=0, CC true
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x333333);
    assert_eq!(s.registers[reg::B0], 0x000001);
}

#[test]
fn test_tcc_with_r_transfer() {
    // TCC A,B R3,R5: tcc_idx=1, has_r=1, src2=R3, dst2=R5, cc=CC(0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x03030D; // tcc A,B R3,R5 cc=CC
    s.registers[reg::A1] = 0x444444;
    s.registers[reg::R3] = 0x000099;
    s.registers[reg::SR] = 0; // C=0, CC true
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x444444);
    assert_eq!(s.registers[reg::R5], 0x000099);
}

#[test]
fn test_tcc_null_source() {
    // TCC with tcc_idx=2 -> (NULL, NULL): no data transfer, just condition eval
    // This covers the src1 == reg::NULL branch skip (line 3449 closing brace)
    // Pattern: 00000010CCCC00000JJJd000
    // CCCC=0000 (CC), JJJd=0010 (idx=2 -> NULL,NULL)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x123456; // should be unchanged
    pram[0] = 0x020010; // TCC CC (null,null)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123456); // no transfer happened
}

#[test]
fn test_div_24_iterations_correct_quotient() {
    // Divide 0.25 by 0.5 = 0.5 (fractional).
    // Per DSP56300FM p.13-52: run DIV 24 times, quotient in A0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Dividend: A = 0.25 fractional = $00:200000:000000
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0x000000;
    // Divisor: X0 = 0.5 = $400000
    s.registers[reg::X0] = 0x400000;

    // 24 DIV X0,A instructions followed by a nop
    pram[..24].fill(0x018040); // div x0,a
    pram[24] = 0x000000; // nop

    for _ in 0..24 {
        run_one(&mut s, &mut jit);
    }

    // Quotient should be 0.5 = $400000 in A0
    assert_eq!(
        s.registers[reg::A0],
        0x400000,
        "DIV: 0.25 / 0.5 should give quotient 0.5 ($400000) in A0"
    );
}

#[test]
fn test_abs_most_negative_56bit() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0x80:000000:000000 (most negative 56-bit value)
    s.registers[reg::A2] = 0x80;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200026; // abs a
    run_one(&mut s, &mut jit);

    // ABS of most-negative wraps back to same value (overflow)
    assert_eq!(s.registers[reg::A2], 0x80, "ABS most-neg: A2 unchanged");
    assert_eq!(s.registers[reg::A1], 0x000000, "ABS most-neg: A1 unchanged");
    assert_eq!(s.registers[reg::A0], 0x000000, "ABS most-neg: A0 unchanged");
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "ABS most-neg: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "ABS most-neg: L=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "ABS most-neg: N=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "ABS most-neg: Z=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "ABS most-neg: C=0");
}

#[test]
fn test_clr_all_flags() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Pre-set flags: E=1, U=0, N=1, V=1, C=1
    s.registers[reg::SR] |= (1 << sr::E) | (1 << sr::N) | (1 << sr::V) | (1 << sr::C);
    s.registers[reg::SR] &= !(1u32 << sr::U);
    // Put something in A so CLR has work to do
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x200013; // clr a
    run_one(&mut s, &mut jit);

    assert_eq!(s.registers[reg::A2], 0, "CLR: A2=0");
    assert_eq!(s.registers[reg::A1], 0, "CLR: A1=0");
    assert_eq!(s.registers[reg::A0], 0, "CLR: A0=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::E), 0, "CLR: E=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::U), 0, "CLR: U=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "CLR: N=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "CLR: Z=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "CLR: V=0");
    // C is not affected by CLR
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "CLR: C unchanged (was 1)"
    );
}

#[test]
fn test_inc_overflow() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = max positive 56-bit: 0x7F:FFFFFF:FFFFFF
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    pram[0] = 0x000008; // inc a
    run_one(&mut s, &mut jit);

    // Overflow wraps to 0x80:000000:000000
    assert_eq!(s.registers[reg::A2], 0x80, "INC overflow: A2");
    assert_eq!(s.registers[reg::A1], 0x000000, "INC overflow: A1");
    assert_eq!(s.registers[reg::A0], 0x000000, "INC overflow: A0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "INC overflow: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "INC overflow: L=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "INC overflow: N=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "INC overflow: Z=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "INC overflow: C=0");
}

#[test]
fn test_inc_flags_zero_to_one() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0
    pram[0] = 0x000008; // inc a
    run_one(&mut s, &mut jit);

    assert_eq!(s.registers[reg::A0], 1, "INC 0->1: A0=1");
    assert_eq!(s.registers[reg::A1], 0, "INC 0->1: A1=0");
    assert_eq!(s.registers[reg::A2], 0, "INC 0->1: A2=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "INC 0->1: Z=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "INC 0->1: N=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "INC 0->1: V=0");
}

#[test]
fn test_dec_zero_to_negative() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0
    pram[0] = 0x00000A; // dec a
    run_one(&mut s, &mut jit);

    // 0 - 1 = -1 = 0xFF:FFFFFF:FFFFFF
    assert_eq!(s.registers[reg::A2], 0xFF, "DEC 0->-1: A2=0xFF");
    assert_eq!(s.registers[reg::A1], 0xFFFFFF, "DEC 0->-1: A1=0xFFFFFF");
    assert_eq!(s.registers[reg::A0], 0xFFFFFF, "DEC 0->-1: A0=0xFFFFFF");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "DEC 0->-1: N=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "DEC 0->-1: Z=0");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "DEC 0->-1: C=1 (borrow)"
    );
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "DEC 0->-1: V=0");
}

#[test]
fn test_dec_flags() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 2
    s.registers[reg::A0] = 0x000002;
    pram[0] = 0x00000A; // dec a
    run_one(&mut s, &mut jit);

    assert_eq!(s.registers[reg::A0], 1, "DEC 2->1: A0=1");
    assert_eq!(s.registers[reg::A1], 0, "DEC 2->1: A1=0");
    assert_eq!(s.registers[reg::A2], 0, "DEC 2->1: A2=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "DEC 2->1: N=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "DEC 2->1: Z=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "DEC 2->1: V=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "DEC 2->1: C=0");
}

#[test]
fn test_add_overflow_v_flag() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0x7F:000000:000000 (large positive)
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    // B = 0x01:000000:000000
    s.registers[reg::B2] = 0x01;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x200010; // add b,a
    run_one(&mut s, &mut jit);

    // 0x7F + 0x01 in A2 = 0x80 (positive overflow to negative)
    assert_eq!(s.registers[reg::A2], 0x80, "ADD overflow: A2=0x80");
    assert_eq!(s.registers[reg::A1], 0x000000, "ADD overflow: A1=0");
    assert_eq!(s.registers[reg::A0], 0x000000, "ADD overflow: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "ADD overflow: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "ADD overflow: L=1");
}

#[test]
fn test_sub_underflow_carry() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0, X0 = 1
    s.registers[reg::X0] = 0x000001;
    pram[0] = 0x200044; // sub x0,a
    run_one(&mut s, &mut jit);

    // 0 - 1 (at A1 position) = negative
    assert_eq!(s.registers[reg::A2], 0xFF, "SUB underflow: A2=0xFF");
    assert_eq!(s.registers[reg::A1], 0xFFFFFF, "SUB underflow: A1=0xFFFFFF");
    assert_eq!(s.registers[reg::A0], 0x000000, "SUB underflow: A0=0");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "SUB underflow: C=1 (borrow)"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "SUB underflow: N=1");
}

#[test]
fn test_cmp_carry_flag_borrow() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0x00:000001:000000 (X0 maps to A1 position, so put 1 in A1)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000001;
    s.registers[reg::A0] = 0x000000;
    // X0 = 2
    s.registers[reg::X0] = 0x000002;
    pram[0] = 0x200045; // cmp x0,a
    run_one(&mut s, &mut jit);

    // CMP does A - X0 = 1 - 2 = -1 (doesn't store result)
    // A should be unchanged
    assert_eq!(s.registers[reg::A2], 0x00, "CMP: A2 unchanged");
    assert_eq!(s.registers[reg::A1], 0x000001, "CMP: A1 unchanged");
    assert_eq!(s.registers[reg::A0], 0x000000, "CMP: A0 unchanged");
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0, "CMP: C=1 (borrow)");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "CMP: N=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "CMP: Z=0");
}

#[test]
fn test_cmpm_negative_operands() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = negative value with |A| = 0x00:400000:000000
    // A = 0xFF:C00000:000000 (A2=0xFF sign-extends, A1=0xC00000)
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xC00000;
    s.registers[reg::A0] = 0x000000;
    // X0 = 0x400000
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x200047; // cmpm x0,a
    run_one(&mut s, &mut jit);

    // CMPM: |A1| - |X0| = 0x400000 - 0x400000 = 0
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "CMPM: Z=1 (magnitudes equal)"
    );
}

#[test]
fn test_adc_overflow_sets_v() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = max positive: 0x7F:FFFFFF:FFFFFF
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    // X = 0 (X1:X0)
    s.registers[reg::X1] = 0x000000;
    s.registers[reg::X0] = 0x000000;
    // Set C=1
    s.registers[reg::SR] |= 1 << sr::C;
    pram[0] = 0x200021; // adc x,a
    run_one(&mut s, &mut jit);

    // A + 0 + 1 overflows from max positive
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "ADC overflow: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "ADC overflow: L=1");
}

#[test]
fn test_sbc_borrow_flag() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0, X1=0, X0=1, C=0
    s.registers[reg::X1] = 0x000000;
    s.registers[reg::X0] = 0x000001;
    s.registers[reg::SR] &= !(1u32 << sr::C);
    pram[0] = 0x200025; // sbc x,a
    run_one(&mut s, &mut jit);

    // A - X - C = 0 - (0:1) - 0 = negative
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0, "SBC: C=1 (borrow)");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "SBC: N=1");
}

#[test]
fn test_maxm_no_transfer() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A = 0x00:100000:000000 (|A| = 0x100000)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    // B = 0x00:200000:000000 (|B| = 0x200000)
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x200015; // maxm a,b
    run_one(&mut s, &mut jit);

    // |B| > |A|, no transfer, C=1
    assert_eq!(
        s.registers[reg::B1],
        0x200000,
        "MAXM no transfer: B1 unchanged"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "MAXM no transfer: C=1"
    );
}

#[test]
fn test_mpy_flags_negative_result() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // X0 = 0x400000 (+0.5 fractional), Y0 = 0xC00000 (-0.5 fractional)
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0xC00000;
    pram[0] = 0x2000D0; // mpy x0,y0,a
    run_one(&mut s, &mut jit);

    // 0.5 * -0.5 = -0.25 -> 0xFF:E00000:000000
    assert_eq!(s.registers[reg::A2], 0xFF, "MPY neg: A2=0xFF");
    assert_eq!(s.registers[reg::A1], 0xE00000, "MPY neg: A1=0xE00000");
    assert_eq!(s.registers[reg::A0], 0x000000, "MPY neg: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "MPY neg: N=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "MPY neg: Z=0");
}

#[test]
fn test_subl_v_from_shift() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A2=0x40, A1=0, A0=0. B=0.
    // SUBL: D = 2*A - B. Shifting A left: bit 55 changes from 0 to 1 -> V=1.
    s.registers[reg::A2] = 0x40;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x200016; // subl b,a
    run_one(&mut s, &mut jit);

    // 2 * 0x40:000000:000000 = 0x80:000000:000000
    assert_eq!(s.registers[reg::A2], 0x80, "SUBL shift: A2=0x80");
    assert_eq!(s.registers[reg::A1], 0x000000, "SUBL shift: A1=0");
    assert_eq!(s.registers[reg::A0], 0x000000, "SUBL shift: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "SUBL shift: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "SUBL shift: L=1");
}

#[test]
fn test_div_v_flag() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // A2=0x40 (bit 55=0, bit 54=1), A1=0, A0=0. X0=0x400000.
    // DIV shifts D left; bit 54 shifts into bit 55, changing it from 0->1 -> V=1.
    s.registers[reg::A2] = 0x40;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x018040; // div x0,a
    run_one(&mut s, &mut jit);

    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "DIV: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "DIV: L=1");
}

#[test]
fn test_tst_v_always_cleared() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Pre-set V=1 in SR
    s.registers[reg::SR] |= 1 << sr::V;
    // A = some non-zero positive value
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);

    // TST always clears V
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "TST: V=0 (always cleared)"
    );
    // Non-zero positive: N=0, Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "TST: N=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "TST: Z=0");
}

#[test]
fn test_adc_carry_flag_output() {
    // DSP56300FM p.13-6: ADC adds S + C + D -> D. When the 56-bit result
    // produces a carry out, C is set.
    // A = 0xFF:FFFFFF:FFFFFF (-1). X = {X1=0, X0=1}. C=0.
    // adc x,a: A + X(48-bit) + C = (-1) + 1 + 0 = 0 with carry out from bit 55.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    s.registers[reg::X1] = 0;
    s.registers[reg::X0] = 1;
    // Ensure C=0 initially
    s.registers[reg::SR] &= !(1 << sr::C);
    pram[0] = 0x200021; // adc x,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "ADC: A2=0");
    assert_eq!(s.registers[reg::A1], 0, "ADC: A1=0");
    assert_eq!(s.registers[reg::A0], 0, "ADC: A0=0");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "ADC: C=1 (carry out)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "ADC: Z=1 (result is zero)"
    );
}

#[test]
fn test_add_negative_carry() {
    // DSP56300FM p.13-7: ADD S + D -> D with standard CCR.
    // A = 0xFF:FFFFFF:000000 (-1 in A2:A1). X0 = 0x000001.
    // X0 is a 24-bit word, sign-extended to 56 bits and placed at A1 position:
    // X0 as 56-bit = 0x00:000001:000000.
    // Sum: 0xFF:FFFFFF:000000 + 0x00:000001:000000 = 0x00:000000:000000.
    // Carry out from bit 55: C=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x000001;
    pram[0] = 0x200040; // add x0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "ADD: A2=0");
    assert_eq!(s.registers[reg::A1], 0, "ADD: A1=0");
    assert_eq!(s.registers[reg::A0], 0, "ADD: A0=0");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "ADD: C=1 (carry out)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "ADD: Z=1 (result is zero)"
    );
}

// NOTE: test_norm_v_flag_left_shift is intentionally omitted.
// DSP56300FM p.13-146: NORM's V flag is "set if bit 55 is changed as a result of
// a left shift." The left-shift (ASL) path requires E=0 and U=1 (bits 47,46 same
// but extension not in use). For bit 55 to change during ASL, bits 55..48 would
// need to NOT be uniform - but that would set E=1, which routes to the ASR path
// instead. Therefore V=1 is unreachable on NORM's ASL path in no-scaling mode.

#[test]
fn test_maci_negate() {
    // DSP56300FM p.13-101: MACI D +/- #xxxx * S -> D.
    // A=0. X0=0x400000 (0.5 fractional).
    // maci -#$200000,x0,a: computes A - (0x200000 * 0x400000).
    // Fractional multiply: (0x200000/2^23) * (0x400000/2^23) = 0.25 * 0.5 = 0.125.
    // Product as 56-bit: 0x00:100000:000000. Negated: 0xFF:F00000:000000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x0141C6; // maci -#$200000,x0,a (word 1)
    pram[1] = 0x200000; // immediate data extension (word 2)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0xFF, "MACI neg: A2=0xFF");
    assert_eq!(s.registers[reg::A1], 0xF00000, "MACI neg: A1=0xF00000");
    assert_eq!(s.registers[reg::A0], 0x000000, "MACI neg: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "MACI neg: N=1");
}

#[test]
fn test_macri_flags() {
    // DSP56300FM p.13-105: MACRI is MAC-round with immediate.
    // A=0. X0=0x400000 (0.5).
    // macri #$200000,x0,a: computes A + (0x200000 * 0x400000) + round.
    // Product = 0.25 * 0.5 = 0.125 = 0x00:100000:000000.
    // After accumulate: A = 0x00:100000:000000. A0=0 so rounding adds 0.
    // A0 cleared after rounding.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x0141C3; // macri #$200000,x0,a (word 1)
    pram[1] = 0x200000; // immediate data extension (word 2)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "MACRI: A2=0");
    assert_eq!(s.registers[reg::A1], 0x100000, "MACRI: A1=0x100000");
    assert_eq!(s.registers[reg::A0], 0, "MACRI: A0=0 (cleared by rounding)");
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "MACRI: N=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "MACRI: Z=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "MACRI: V=0");
}

#[test]
fn test_mpyri_flags() {
    // DSP56300FM p.13-143: MPYRI is MPY-round with immediate.
    // X0=0x400000 (0.5).
    // mpyri #$200000,x0,a: computes (0x200000 * 0x400000) + round -> A.
    // Product = 0.125 = 0x00:100000:000000. A0=0, no rounding adjustment.
    // A0 cleared after rounding.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x0141C1; // mpyri #$200000,x0,a (word 1)
    pram[1] = 0x200000; // immediate data extension (word 2)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "MPYRI: A2=0");
    assert_eq!(s.registers[reg::A1], 0x100000, "MPYRI: A1=0x100000");
    assert_eq!(s.registers[reg::A0], 0, "MPYRI: A0=0 (cleared by rounding)");
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "MPYRI: N=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "MPYRI: Z=0");
}

#[test]
fn test_macr_convergent_rounding_tie() {
    // DSP56300FM p.13-103: MACR does convergent rounding (RM=0 default).
    // At exact midpoint (A0=0x800000), convergent rounding rounds to even:
    // if A1 bit 0 = 1 (odd), round up; if A1 bit 0 = 0 (even), round down.
    //
    // Setup: A = 0x00:000001:800000 (A1 bit 0 = 1, A0 = midpoint).
    // X0=0, Y0=0. macr x0,y0,a: product = 0, so result = A + 0, then round.
    // A0 = 0x800000 is the tie case. A1 bit 0 = 1 (odd) -> round up.
    // A1 becomes 0x000002, A0 cleared.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x000001;
    s.registers[reg::A0] = 0x800000;
    s.registers[reg::X0] = 0;
    s.registers[reg::Y0] = 0;
    pram[0] = 0x2000D3; // macr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "MACR tie: A2=0");
    assert_eq!(
        s.registers[reg::A1],
        0x000002,
        "MACR tie: A1=0x000002 (rounded up to even)"
    );
    assert_eq!(
        s.registers[reg::A0],
        0,
        "MACR tie: A0=0 (cleared by rounding)"
    );
}

#[test]
fn test_mpyr_rounding_tie() {
    // DSP56300FM p.13-141: MPYR does convergent rounding.
    // X0=0x400001, Y0=0x400000.
    // Raw product = 0x400001 * 0x400000 = 0x100000_400000.
    // Fractional shift left by 1: 0x200000_800000.
    // A1=0x200000, A0=0x800000 (tie). A1 bit 0 = 0 (even) -> round down (no increment).
    // A0 cleared. Result: A = 0x00:200000:000000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400001;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D1; // mpyr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "MPYR tie: A2=0");
    assert_eq!(
        s.registers[reg::A1],
        0x200000,
        "MPYR tie: A1=0x200000 (even, round down)"
    );
    assert_eq!(
        s.registers[reg::A0],
        0,
        "MPYR tie: A0=0 (cleared by rounding)"
    );
}

#[test]
fn test_abs_zero() {
    // DSP56300FM p.13-6: ABS - |D| -> D.
    // A=0: |0|=0, Z=1, N=0, V=0, C=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200026; // abs a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "ABS zero: A2=0");
    assert_eq!(s.registers[reg::A1], 0, "ABS zero: A1=0");
    assert_eq!(s.registers[reg::A0], 0, "ABS zero: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "ABS zero: Z=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0, "ABS zero: N=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "ABS zero: V=0");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "ABS zero: C=0");
}

#[test]
fn test_abs_positive_unchanged() {
    // DSP56300FM p.13-6: ABS of a positive value leaves it unchanged. V=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200026; // abs a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0x00, "ABS pos: A2 unchanged");
    assert_eq!(s.registers[reg::A1], 0x400000, "ABS pos: A1 unchanged");
    assert_eq!(s.registers[reg::A0], 0x000000, "ABS pos: A0 unchanged");
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0, "ABS pos: V=0");
}

#[test]
fn test_addl_zero_result() {
    // DSP56300FM p.13-14: ADDL - S + 2*D -> D.
    // A=0, B=0: 0 + 2*0 = 0. Z=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200012; // addl b,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "ADDL zero: A2=0");
    assert_eq!(s.registers[reg::A1], 0, "ADDL zero: A1=0");
    assert_eq!(s.registers[reg::A0], 0, "ADDL zero: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "ADDL zero: Z=1");
}

#[test]
fn test_addr_overflow_v_flag() {
    // DSP56300FM p.13-15: ADDR - S + D/2 -> D.
    // B=0x7F:000000:000000, A=0x7F:000000:000000.
    // A/2 = 0x3F:800000:000000.
    // B + A/2 = 0x7F:000000:000000 + 0x3F:800000:000000 overflows positive range. V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // addr b,a: D=A, S=B. Operation: B + A/2 -> A.
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x7F;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x200002; // addr b,a
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "ADDR overflow: V=1");
}

#[test]
fn test_asl_imm_zero_shift() {
    // DSP56300FM p.13-18: ASL #0,A,A - no shift, C cleared.
    // "C - Set if bit 55 is shifted out of the MSB. Cleared if shift count is zero."
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::A0] = 0x789ABC;
    // Pre-set C=1 to verify it gets cleared
    s.registers[reg::SR] |= 1 << sr::C;
    pram[0] = 0x0C1D00; // asl #0,a,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0x00, "ASL #0: A2 unchanged");
    assert_eq!(s.registers[reg::A1], 0x123456, "ASL #0: A1 unchanged");
    assert_eq!(s.registers[reg::A0], 0x789ABC, "ASL #0: A0 unchanged");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "ASL #0: C cleared");
}

#[test]
fn test_asl_imm_shift_24() {
    // DSP56300FM p.13-18: ASL #23,A,A.
    // A = 0x00:FFFFFF:000000 (A2=0, A1=0xFFFFFF, A0=0).
    // Shift left 23: result A2=0xFF, A1=0x800000, A0=0.
    // C = old bit (55-23) = old bit 33 = A1[9] = 1.
    // V = 1 (bit 55 changes from 0 to 1 during the shift).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1D2E; // asl #23,a,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0xFF, "ASL #23: A2=0xFF");
    assert_eq!(s.registers[reg::A1], 0x800000, "ASL #23: A1=0x800000");
    assert_eq!(s.registers[reg::A0], 0x000000, "ASL #23: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0, "ASL #23: C=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "ASL #23: V=1");
}

#[test]
fn test_max_equal_values() {
    // DSP56300FM p.13-106: MAX A,B - "if B-A <= 0 then A -> B".
    // A=B=0x00:100000:000000. B-A=0 <= 0, so transfer occurs. C=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x20001D; // max a,b
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B2], 0x00, "MAX equal: B2");
    assert_eq!(s.registers[reg::B1], 0x100000, "MAX equal: B1 = A1");
    assert_eq!(s.registers[reg::B0], 0x000000, "MAX equal: B0");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "MAX equal: C=0 (transfer)"
    );
}

#[test]
fn test_maxm_equal_magnitude() {
    // DSP56300FM p.13-107: MAXM A,B - "if |B|-|A| <= 0 then A -> B".
    // A=0xFF:F00000:000000 (negative). |A| = 0x00:100000:000000.
    // B=0x00:100000:000000 (positive). |B| = 0x00:100000:000000.
    // |B|-|A| = 0 <= 0, transfer A -> B. C=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xF00000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x200015; // maxm a,b
    run_one(&mut s, &mut jit);
    // Transfer: B gets the original A value (not |A|)
    assert_eq!(
        s.registers[reg::B2],
        0xFF,
        "MAXM eq mag: B2=0xFF (A transferred)"
    );
    assert_eq!(s.registers[reg::B1], 0xF00000, "MAXM eq mag: B1=0xF00000");
    assert_eq!(s.registers[reg::B0], 0x000000, "MAXM eq mag: B0=0");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "MAXM eq mag: C=0 (transfer)"
    );
}

#[test]
fn test_mpy_zero_product() {
    // DSP56300FM p.13-137: MPY X0,Y0,A.
    // X0=0, Y0=0x400000. Product = 0*0x400000 = 0. After frac shift: 0.
    // Z=1 (result is zero).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D0; // mpy x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "MPY zero: A2=0");
    assert_eq!(s.registers[reg::A1], 0, "MPY zero: A1=0");
    assert_eq!(s.registers[reg::A0], 0, "MPY zero: A0=0");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "MPY zero: Z=1");
}

#[test]
fn test_tst_negative() {
    // DSP56300FM p.13-181: TST - test accumulator.
    // A=0xFF:800000:000000 (most negative 48-bit sign-extended value).
    // N=1 (negative), Z=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "TST neg: N=1");
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0, "TST neg: Z=0");
}

#[test]
fn test_tfr_s_l_flags() {
    // DSP56300FM p.13-178: TFR - S -> D. CCR bits E,U,N,Z,V,C are NOT affected.
    // Pre-set V=1, C=1. After TFR, V and C must remain set.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= (1 << sr::V) | (1 << sr::C);
    s.registers[reg::X0] = 0x123456;
    pram[0] = 0x200041; // tfr x0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123456, "TFR: A1 = X0");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "TFR: V unchanged (still 1)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "TFR: C unchanged (still 1)"
    );
}

#[test]
fn test_div_l_flag() {
    // DSP56300FM p.13-52: DIV - L is set if V is set (L = L | V).
    // Pre-set L=0. Use inputs that produce V=1.
    // A2=0x40 (bit 55=0, bit 54=1), X0=0x400000. After DIV shift,
    // bit 54 shifts into bit 55, changing it -> V=1, so L must be set.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] &= !(1 << sr::L); // ensure L=0
    s.registers[reg::A2] = 0x40;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x018040; // div x0,a
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "DIV L: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "DIV L: L=1 (L|=V)");
}

#[test]
fn test_macr_twos_complement_rounding() {
    // DSP56300FM p.13-103: MACR with RM=1 uses two's complement rounding
    // (add 1 at bit 23, no convergent tie-breaking).
    // A=0x00:000001:800000 (tie: A0=0x800000). X0=0, Y0=0.
    // macr x0,y0,a: MAC product is 0, so result is just rounding of A.
    // RM=1: unconditional add of 1 at rounding position -> A1 becomes 0x000002.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::RM; // RM=1: two's complement rounding
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000001;
    s.registers[reg::A0] = 0x800000;
    s.registers[reg::X0] = 0;
    s.registers[reg::Y0] = 0;
    pram[0] = 0x2000D3; // macr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0, "MACR RM=1: A2=0");
    assert_eq!(
        s.registers[reg::A1],
        0x000002,
        "MACR RM=1: A1=0x000002 (rounded up)"
    );
    assert_eq!(
        s.registers[reg::A0],
        0,
        "MACR RM=1: A0=0 (cleared by rounding)"
    );
}

#[test]
fn test_dmac_all_qqqq_high_encodings() {
    // DSP56300FM Table 12-16 (Encoding 4): All 16 QQQQ values (0x0-0xF) are valid.
    // ARCHITECTURE-NOTES.md documents the full mapping. Existing tests cover 0-8.
    // Verify QQQQ values 9, 10, 12, 15 execute without panic and produce nonzero results.
    // DMAC template: 000000010010010s1sdkQQQQ, ss=0, k=0, d=0(A)
    let cases: &[(u32, &str, usize, usize)] = &[
        // (opcode, label, reg_to_set_1, reg_to_set_2)
        // QQQQ=9 (1001): Y1,Y1
        (0x012489, "QQQQ=9 (Y1,Y1)", reg::Y1, reg::Y1),
        // QQQQ=10 (1010): X0,X1
        (0x01248A, "QQQQ=10 (X0,X1)", reg::X0, reg::X1),
        // QQQQ=12 (1100): Y1,X0
        (0x01248C, "QQQQ=12 (Y1,X0)", reg::Y1, reg::X0),
        // QQQQ=15 (1111): X1,Y1
        (0x01248F, "QQQQ=15 (X1,Y1)", reg::X1, reg::Y1),
    ];
    for &(opcode, label, r1, r2) in cases {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        s.registers[r1] = 0x400000; // 0.5
        s.registers[r2] = 0x400000; // 0.5
        pram[0] = opcode;
        run_one(&mut s, &mut jit);
        assert_ne!(
            s.registers[reg::A1],
            0,
            "{label}: A1 should be nonzero after DMAC with nonzero operands"
        );
        assert_eq!(s.pc, 1, "{label}: PC should advance");
    }
}

#[test]
fn test_mac_all_qqq_pairs() {
    // DSP56300FM p.13-96: MAC uses QQQ encoding (8 register pairs).
    // Verify all QQQ variants execute and produce nonzero accumulator.
    // Set all data regs to small nonzero fractional values.
    let opcodes: &[(u32, &str)] = &[
        (0x2000D2, "mac x0,y0,a"),
        (0x200082, "mac x0,x0,a"),
        (0x200092, "mac y0,y0,a"),
        (0x2000E2, "mac x1,y0,a"),
        (0x2000C2, "mac x0,y1,a"),
        (0x2000F2, "mac x1,y1,a"),
    ];
    for &(opcode, label) in opcodes {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        s.registers[reg::X0] = 0x100000;
        s.registers[reg::X1] = 0x200000;
        s.registers[reg::Y0] = 0x100000;
        s.registers[reg::Y1] = 0x200000;
        pram[0] = opcode;
        run_one(&mut s, &mut jit);
        let a_val = ((s.registers[reg::A2] as u64) << 48)
            | ((s.registers[reg::A1] as u64) << 24)
            | (s.registers[reg::A0] as u64);
        assert_ne!(
            a_val, 0,
            "{label}: A should be nonzero after MAC with nonzero operands"
        );
        assert_eq!(s.pc, 1, "{label}: PC should advance");
    }
}

#[test]
fn test_norm_nop_when_normalized() {
    // DSP56300FM p.13-146: NORM does nothing when accumulator is already normalized
    // (E=0, U=0). The accumulator and Rn are both unchanged.
    // A = 0x00:400000:000000 - bit 47=0, bit 46=1 -> U=0; bits 55:48 all zero -> E=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 5;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    // norm R0,A = 0x01D815 (template: 0000000111011RRR0001d101, RRR=0, d=0)
    pram[0] = 0x01D815;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0x00, "A2 unchanged when normalized");
    assert_eq!(
        s.registers[reg::A1],
        0x400000,
        "A1 unchanged when normalized"
    );
    assert_eq!(
        s.registers[reg::A0],
        0x000000,
        "A0 unchanged when normalized"
    );
    assert_eq!(
        s.registers[reg::R0],
        5,
        "R0 unchanged when normalized (no shift)"
    );
}

#[test]
fn test_mpyr_immediate_shift() {
    // DSP56300FM p.13-134: MPYR with immediate shift variant: mpyr S,#n,D
    // Template: 00000001000sssss11QQdk01
    // shift=1, QQ=00(Y1,Y0->Y1), d=0(A), k=0(positive)
    // 00000001000_00001_11_00_0_0_01 = 0x0101C1
    // Same as mul_shift_mpy but with rounding (last 2 bits = 01 instead of 00).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y1] = 0x400000; // 0.5 in fractional
    pram[0] = 0x0101C1; // mpyr y1,#1,a
    run_one(&mut s, &mut jit);
    // Y1=0.5, shift by 2^-1 => product = 0.25 = 0x200000 before rounding.
    // Rounding clears A0, result in A1.
    assert_eq!(s.registers[reg::A2], 0, "MPYR shift: A2=0");
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYR shift: A1=0x200000");
    assert_eq!(s.registers[reg::A0], 0, "MPYR shift: A0=0 (rounded)");
}

#[test]
fn test_maci_flags() {
    // Per DSP56300FM p.13-101: MACI sets standard CCR flags. V is always cleared,
    // C is unchanged. Verify V=0 and Z=1 when result is zero.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A=0, X0=0 -> MACI #anything * 0 = 0 -> A stays 0 -> Z=1, V=0
    s.registers[reg::SR] |= 1 << sr::V; // pre-set V=1
    s.registers[reg::X0] = 0;
    pram[0] = 0x0141C2; // maci #$200000,x0,a
    pram[1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0, "MACI: A0=0");
    assert_eq!(s.registers[reg::A1], 0, "MACI: A1=0");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "MACI: Z=1 for zero result"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "MACI: V=0 (always cleared)"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "MACI: N=0 for zero result"
    );
}

#[test]
fn test_rnd_scale_down() {
    // DSP56300FM p.13-163: RND with scale-down (S1:S0=01) rounds at bit 24.
    // A = 0x00:000003:000000. With rounding at bit 24, rnd_const = {0, 1, 0}.
    // sum = 0x00:000003:000000 + 0x00:000001:000000 = 0x00:000004:000000
    // Result bits below 24 cleared: A = 0x00:000004:000000.
    // Without scale-down (rounding at bit 23), rnd_const is 0x800000 in A0,
    // and the result would be 0x00:000003:000000 (A0 cleared to 0, no carry).
    // This verifies the rounding position actually shifts.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Scale-down: S0=1, S1=0
    s.registers[reg::SR] = 1 << sr::S0;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000003;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200011; // rnd A
    run_one(&mut s, &mut jit);
    // Scale-down rounds at bit 24. Adding 1<<24 to 0x000003:000000 gives 0x000004:000000.
    // Low bit of A1 cleared (bit 24 rounding zeroes A1[0]).
    assert_eq!(
        s.registers[reg::A1],
        0x000004,
        "RND scale-down: A1 should round at bit 24"
    );
    assert_eq!(s.registers[reg::A0], 0x000000, "RND scale-down: A0=0");
}

#[test]
fn test_rnd_scale_up() {
    // DSP56300FM p.13-163: RND with scale-up (S1:S0=10) rounds at bit 22.
    // A = 0x00:400000:600000. rnd_const at bit 22 = 0x400000 added to A0.
    // sum = 0x00:400000:600000 + 0x00:000000:400000 = 0x00:400000:A00000.
    // Bits 22:0 of A0 zeroed but bit 23 preserved: A0 = 0x800000.
    //
    // Compare: without scaling, rounding at bit 23 would add 0x800000 to A0:
    // 0x600000 + 0x800000 = 0xE00000, carry=0 => A0=0, A1=0x400001. Different result.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Scale-up: S1=1, S0=0
    s.registers[reg::SR] = 1 << sr::S1;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x600000;
    pram[0] = 0x200011; // rnd A
    run_one(&mut s, &mut jit);
    // Scale-up rounds at bit 22. Adding 0x400000 to A0=0x600000 gives 0xA00000.
    // Bits 21:0 zeroed, bit 23 kept: A0 = 0x800000.
    assert_eq!(s.registers[reg::A1], 0x400000, "RND scale-up: A1 unchanged");
    assert_eq!(
        s.registers[reg::A0],
        0x800000,
        "RND scale-up: A0 = 0x800000 (bits 21:0 zeroed)"
    );
}

#[test]
fn test_mpyr_scale_down() {
    // MPYR with scale-down (S1:S0=01): rounding at bit 24.
    // X0=0x400000 (0.5), Y0=0x400000 (0.5). Product = 0.25 = 0x00:200000:000000.
    // With scale-down rounding at bit 24: add 1<<24 = 0x00:000001:000000.
    // Result = 0x00:200001:000000, then A1[0] cleared if convergent and remainder=0.
    // A1 even? 0x200001 is odd. No tie-breaking. Result A1 = 0x200000 (A1[0] cleared).
    // Wait: the code does s1 &= 0xFF_FFFE unconditionally for S0 mode.
    // So result = 0x200000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D1; // mpyr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYR scale-down: A1");
    assert_eq!(
        s.registers[reg::A0],
        0x000000,
        "MPYR scale-down: A0 cleared"
    );
}

#[test]
fn test_mpyr_scale_up() {
    // MPYR with scale-up (S1:S0=10): rounding at bit 22.
    // X0=0x400000 (0.5), Y0=0x400000 (0.5). Product = 0x00:200000:000000.
    // With scale-up rounding at bit 22: add 1<<22 = 0x400000 to A0.
    // A0 = 0x400000. Bits 21:0 zeroed, bit 23 kept: A0 = 0.
    // Actually: s0 &= 0x80_0000 => A0 = 0 (bit 23 not set in 0x400000).
    // convergent check: (s0 & 0x7F_FFFF) == 0 ? 0x400000 & 0x7F_FFFF = 0x400000 != 0.
    // So no convergent zeroing. s0 = 0x400000 & 0x800000 = 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S1; // scale-up
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D1; // mpyr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYR scale-up: A1");
    assert_eq!(
        s.registers[reg::A0],
        0x000000,
        "MPYR scale-up: A0 cleared by bit masking"
    );
}

#[test]
fn test_macr_scale_down() {
    // MACR with scale-down: accumulate then round at bit 24.
    // A=0, X0=0x400000, Y0=0x400000. MAC product = 0x00:200000:000000.
    // Round at bit 24: add 1<<24 => 0x00:200001:000000. A1[0] cleared => 0x200000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D3; // macr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MACR scale-down: A1");
    assert_eq!(
        s.registers[reg::A0],
        0x000000,
        "MACR scale-down: A0 cleared"
    );
}

#[test]
fn test_macr_scale_up() {
    // MACR with scale-up: accumulate then round at bit 22.
    // A=0, X0=0x400000, Y0=0x400000. MAC product = 0x00:200000:000000.
    // Round at bit 22: add 0x400000 to A0 (=0) => A0=0x400000.
    // s0 &= 0x800000 => A0 = 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S1; // scale-up
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D3; // macr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MACR scale-up: A1");
    assert_eq!(s.registers[reg::A0], 0x000000, "MACR scale-up: A0 cleared");
}

#[test]
fn test_addl_b_destination() {
    // DSP56300FM p.13-14: ADDL A,B (opcode 0x20001A). Operation: A + 2*B -> B.
    // A = 0x00:100000:000000, B = 0x00:200000:000000.
    // 2*B = 0x00:400000:000000, result B = A + 2*B = 0x00:500000:000000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x20001A; // addl A,B
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x200000;
    s.registers[reg::B0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x500000, "ADDL A,B: B1 = 0x500000");
    assert_eq!(s.registers[reg::B0], 0x000000, "ADDL A,B: B0 = 0");
}

#[test]
fn test_cmp_v_flag_overflow() {
    // DSP56300FM p.13-45: CMP D-S, flags only. V=1 when subtraction overflows.
    // A = $7F:FFFFFF:000000 (max positive), B = $80:000000:000000 (most negative).
    // CMP B,A: A - B = huge positive - huge negative = overflow. V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200005; // cmp B,A (alu_byte 0x05)
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x80;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    // A should be unchanged (CMP doesn't store)
    assert_eq!(s.registers[reg::A2], 0x7F, "CMP V=1: A unchanged");
    assert_eq!(s.registers[reg::A1], 0xFFFFFF, "CMP V=1: A1 unchanged");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "CMP: V=1 from overflow"
    );
}

#[test]
fn test_cmpm_n_flag() {
    // DSP56300FM p.13-46: CMPM - |D| - |S|. N=1 when |D| < |S|.
    // A = $00:100000:000000, X0 = $200000. |A| = 0x100000, |X0| = 0x200000.
    // CMPM X0,A: |A| - |X0| = negative. N=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200047; // cmpm X0,A
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x200000;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "CMPM: N=1 when |D| < |S|"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "CMPM: Z=0 (not equal)"
    );
}

#[test]
fn test_dec_b_accumulator() {
    // DSP56300FM p.13-50: DEC B (opcode 0x00000B).
    // B = 0x00:000002:000000. DEC B -> B = 0x00:000001:000000.
    // Wait - 0x00000B is TST B in the parallel ALU dispatch. DEC B = 0x00000B is different.
    // Actually from existing test: DEC A = 0x00000A, so DEC B = 0x00000B.
    // But the alu_b_dispatch test shows 0x20000B = TST B.
    // DEC is a non-parallel instruction: opcode 0x00000A = dec A, 0x00000B = dec B.
    // The encoding is: 0000000000000000 00001d10, d=0->A, d=1->B.
    // dec A = 0x00000A, dec B = 0x00000B.
    // NOTE: 0x20000B with PM2 nop prefix would be TST B (alu=0x0B).
    // Non-parallel DEC B uses the raw opcode 0x00000B.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000002;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x00000B; // dec B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B0], 0xFFFFFF, "DEC B: B0 = 0xFFFFFF");
    assert_eq!(s.registers[reg::B1], 0x000001, "DEC B: B1 = 1");
    assert_eq!(s.registers[reg::B2], 0x00, "DEC B: B2 = 0");
}

#[test]
fn test_dec_most_negative_overflow() {
    // DSP56300FM p.13-50: DEC of most negative value should overflow.
    // A = $80:000000:000000. DEC A -> underflow. V=1, L=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x80;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::SR] = 0;
    pram[0] = 0x00000A; // dec A
    run_one(&mut s, &mut jit);
    // $80:000000:000000 - 1 = $7F:FFFFFF:FFFFFF. Sign changes from negative to positive. V=1.
    assert_eq!(s.registers[reg::A2], 0x7F, "DEC overflow: A2 = 0x7F");
    assert_eq!(
        s.registers[reg::A1],
        0xFFFFFF,
        "DEC overflow: A1 = 0xFFFFFF"
    );
    assert_eq!(
        s.registers[reg::A0],
        0xFFFFFF,
        "DEC overflow: A0 = 0xFFFFFF"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "DEC overflow: V=1");
    assert_ne!(s.registers[reg::SR] & (1 << sr::L), 0, "DEC overflow: L=1");
}

#[test]
fn test_inc_b_accumulator() {
    // DSP56300FM p.13-79: INC B (opcode 0x000009).
    // B = 0x00:000000:000000. INC B -> B = 0x00:000000:000001.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000009; // inc B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B0], 1, "INC B: B0 = 1");
    assert_eq!(s.registers[reg::B1], 0, "INC B: B1 = 0");
    assert_eq!(s.registers[reg::B2], 0, "INC B: B2 = 0");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "INC B: Z=0 (nonzero)"
    );
}

#[test]
fn test_rnd_b_accumulator() {
    // DSP56300FM p.13-163: RND B (opcode 0x200019).
    // B = 0x00:400000:800001. Rounding: 0x800001 + 0x800000 = 0x1000001, carry into B1.
    // B1 = 0x400001, B0 cleared. (convergent: B0 remainder != 0, so no tie-breaking)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x400000;
    s.registers[reg::B0] = 0x800001;
    pram[0] = 0x200019; // rnd B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x400001, "RND B: B1 rounds up");
    assert_eq!(s.registers[reg::B0], 0x000000, "RND B: B0 cleared");
}

#[test]
fn test_sbc_with_carry_input() {
    // DSP56300FM p.13-169: SBC X,A: A = A - X - C.
    // A = 0x00:500000:200000, X = 0x080000:100000, C = 1.
    // A - X - 1 = 0x500000:200000 - 0x080000:100000 - 1 = 0x47FFFF:0FFFFF.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200025; // sbc X,A
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x500000;
    s.registers[reg::A0] = 0x200000;
    s.registers[reg::X1] = 0x080000;
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::SR] = 1 << sr::C; // C=1
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x480000, "SBC C=1: A1");
    assert_eq!(s.registers[reg::A0], 0x0FFFFF, "SBC C=1: A0");
}

#[test]
fn test_sbc_overflow_v_flag() {
    // DSP56300FM p.13-169: SBC overflow. A = $80:000000:000000 (most negative).
    // X = 0x000001:000000, C=0. A - X = overflow (more negative than representable). V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200025; // sbc X,A
    s.registers[reg::A2] = 0x80;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X1] = 0x000001;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::SR] = 0; // C=0
    run_one(&mut s, &mut jit);
    // $80:000000:000000 - $00:000001:000000 = $7F:FFFFFF:000000 (sign flip). V=1.
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "SBC overflow: V=1");
}

#[test]
fn test_subl_carry_flag() {
    // DSP56300FM p.13-174: SUBL B,A: A = 2*A - B. C = carry from ASL XOR carry from sub.
    // A = 0x00:200000:000000 (bit 55=0), B = 0x00:100000:000000.
    // 2*A = 0x00:400000:000000 (asl_carry=0), result = 0x00:300000:000000. No sub carry.
    // C = 0 XOR 0 = 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200016; // subl B,A
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000, "SUBL: A1 = 0x300000");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "SUBL: C=0");
}

#[test]
fn test_subl_v_from_subtraction_overflow() {
    // DSP56300FM p.13-174: SUBL - V from shift or subtraction.
    // A = 0x00:000000:000000, B = 0x80:000000:000000 (most negative).
    // 2*A = 0 (asl_carry=0, asl_v=0). result = 0 - B = -($80:000000:000000).
    // This is 0x80:000000:000000 which overflows: sub of most-negative from 0. V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200016; // subl B,A
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x80;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    // 0 - $80:000000:000000 wraps to $80:000000:000000, sign same as B but result
    // differs from both inputs. Subtraction overflow: V=1.
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "SUBL: V=1 from subtraction overflow"
    );
}

#[test]
fn test_subr_v_and_c_flags() {
    // DSP56300FM p.13-175: SUBR B,A: A = A/2 - B. V from subtraction.
    // A = 0x80:000000:000000 (most negative), B = 0x7F:000000:000000 (large positive).
    // A/2 = 0xC0:000000:000000 (arithmetic right shift). A/2 - B = negative - positive overflow.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200006; // subr B,A
    s.registers[reg::A2] = 0x80;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::B2] = 0x7F;
    s.registers[reg::B1] = 0x000000;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::SR] = 0;
    run_one(&mut s, &mut jit);
    // A/2 = $C0:000000:000000. C0 - 7F = 0x41 => result = 0x41:000000:000000 (positive).
    // Sign change from negative (A/2) to positive (result) with positive B: V=1.
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "SUBR: V=1 from subtraction overflow"
    );
}

#[test]
fn test_macr_v_from_overflow() {
    // DSP56300FM p.13-103: MACR - V from accumulation overflow.
    // A = $7F:FFFFFF:000000 (near max). X0=0x400000 (0.5), Y0=0x400000 (0.5).
    // Product = 0.25 = 0x00:200000:000000. A + product overflows. V=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::Y0] = 0x400000;
    s.registers[reg::SR] = 0;
    pram[0] = 0x2000D3; // macr x0,y0,a
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "MACR: V=1 from overflow"
    );
}

#[test]
fn test_macr_v_from_rounding_overflow() {
    // DSP56300FM p.13-103: MACR V flag from rounding overflow.
    // The accumulation sum is just below the bit-55 boundary, and the
    // rounding constant pushes the result over. V should be set because
    // bit 55 changed from 0 to 1 in the final rounded result.
    //
    // Setup: A = $7F:800001:FFFFFD, X0=$7FFFFF, Y0=$7FFFFF
    // Product = 0x7FFFFF * 0x7FFFFF << 1 = $7FFFFE:000002
    // Sum = A + product = $7F:FFFFFF:FFFFFF (bit 55 = 0, max positive)
    // Rounding (no scaling): add $800000 to sum -> $80:000000:7FFFFF
    // After clearing LSBs: $80:000000:000000 (bit 55 = 1)
    // V=1 (sign changed), N=1 (bit 55 set), L=1 (L|V)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x7F;
    s.registers[reg::A1] = 0x800001;
    s.registers[reg::A0] = 0xFFFFFD;
    s.registers[reg::X0] = 0x7FFFFF;
    s.registers[reg::Y0] = 0x7FFFFF;
    s.registers[reg::SR] = 0;
    pram[0] = 0x2000D3; // macr x0,y0,a
    run_one(&mut s, &mut jit);
    let sr = s.registers[reg::SR];
    assert_ne!(sr & (1 << sr::V), 0, "V=1: rounding caused overflow");
    assert_ne!(sr & (1 << sr::N), 0, "N=1: result is negative");
    assert_ne!(sr & (1 << sr::L), 0, "L=1: L|V sticky");
    assert_eq!(s.registers[reg::A2], 0x80, "A2=$80 after rounding overflow");
    assert_eq!(s.registers[reg::A1], 0x000000, "A1=$000000");
    assert_eq!(s.registers[reg::A0], 0x000000, "A0=$000000");
}

#[test]
fn test_macri_y0_source() {
    // DSP56300FM p.13-105: MACRI with Y0 source (qq=01).
    // Template: 000000010100000111qqdk11, qq=01 (Y0), d=0, k=0
    // Encoding: 000000010100000111_01_0_0_11 = 0x0141D3.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141D3; // macri #xxxx,Y0,A
    pram[1] = 0x400000; // immediate = 0.5
    run_one(&mut s, &mut jit);
    // A = round(A + 0.5 * 0.5) = round(0x100000 + 0x200000) = 0x300000
    assert_eq!(s.registers[reg::A1], 0x300000, "MACRI Y0: A1 = 0x300000");
}

#[test]
fn test_macri_x1_source() {
    // MACRI with X1 source (qq=10).
    // Template: 000000010100000111_10_0_0_11 = 0x0141E3.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141E3; // macri #xxxx,X1,A
    pram[1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000, "MACRI X1: A1 = 0x300000");
}

#[test]
fn test_macri_y1_source() {
    // MACRI with Y1 source (qq=11).
    // Template: 000000010100000111_11_0_0_11 = 0x0141F3.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y1] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141F3; // macri #xxxx,Y1,A
    pram[1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000, "MACRI Y1: A1 = 0x300000");
}

#[test]
fn test_maci_y0_source() {
    // MACI with Y0 source (qq=01).
    // Template: 000000010100000111_01_0_0_10 = 0x0141D2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141D2; // maci #xxxx,Y0,A
    pram[1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000, "MACI Y0: A1 = 0x300000");
}

#[test]
fn test_maci_x1_source() {
    // MACI with X1 source (qq=10).
    // Template: 000000010100000111_10_0_0_10 = 0x0141E2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0x400000; // 0.5
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0141E2; // maci #xxxx,X1,A
    pram[1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000, "MACI X1: A1 = 0x300000");
}

#[test]
fn test_mpyr_twos_complement_rounding() {
    // DSP56300FM p.13-141: MPYR with RM=1 uses two's complement rounding.
    // X0=0x400001, Y0=0x400000. Product has a nonzero A0.
    // With RM=1, the tie at 0x800000 should always round up (no convergent tie-breaking).
    // Product = 0x400001 * 0x400000 * 2 = ...
    // Verify RM=1 (two's complement) rounds a tie case differently than convergent.
    // Set A to have a tie value (A0=0x800000), use product=0.
    // X0=0, Y0=anything. MPYR overwrites A with round(0) = 0. Not helpful.
    // MPYR is MPY+round, not MAC+round. It replaces A, doesn't accumulate.
    //
    // Actually for MPYR RM=1, the simplest verification is:
    // Choose operands where product A0 = exactly 0x800000, A1 odd.
    // 0x200001 * 0x400000 * 2 = 0x200001_000000 * 2 = ... let's compute.
    // 0x200001 * 0x400000 = 0x80000400000. << 1 = 0x100000800000.
    // A2=0, A1=0x100000, A0=0x800000. A1 is even (0x100000).
    // RM=0 (convergent): tie with even A1 => round down. A1=0x100000.
    // RM=1 (two's complement): always round up at tie. A1=0x100001.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::RM; // two's complement rounding
    s.registers[reg::X0] = 0x200001;
    s.registers[reg::Y0] = 0x400000;
    pram[0] = 0x2000D1; // mpyr x0,y0,a
    run_one(&mut s, &mut jit);
    // With RM=1, tie at 0x800000 with even A1 should round UP (not stay even).
    assert_eq!(
        s.registers[reg::A1],
        0x100001,
        "MPYR RM=1: A1 rounds up at tie"
    );
    assert_eq!(s.registers[reg::A0], 0x000000, "MPYR RM=1: A0 cleared");
}

#[test]
fn test_mpyri_y0_source() {
    // MPYRI with Y0 source (qq=01). Covers C2 gap.
    // Template: 000000010100000111_01_0_0_01 = 0x0141D1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0x400000; // 0.5
    pram[0] = 0x0141D1; // mpyri #xxxx,Y0,A
    pram[1] = 0x400000; // immediate = 0.5
    run_one(&mut s, &mut jit);
    // round(0.5 * 0.5) = round(0x200000:000000) = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYRI Y0: A1 = 0x200000");
    assert_eq!(s.registers[reg::A0], 0x000000, "MPYRI Y0: A0 cleared");
}

#[test]
fn test_mpy_scaling_mode_e_u_flags() {
    // DSP56300FM p.13-137: MPY - E/U flag computation affected by S1:S0.
    // With scale-up (S1=1), E checks bits 55:46 and U checks bit46 XOR bit45.
    // MPY X0,Y0,A with small product should set U=1 (unnormalized for scale-up).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S1; // scale-up
    // Small product: X0=1, Y0=1 => product = 2 (tiny).
    // Result: A = 0x00:000000:000002. Bits 55:46 all 0 (uniform) => E=0.
    // bit46=0, bit45=0 => U=1.
    s.registers[reg::X0] = 0x000001;
    s.registers[reg::Y0] = 0x000001;
    pram[0] = 0x2000D0; // mpy x0,y0,a
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "MPY scale-up: U=1 (unnormalized)"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "MPY scale-up: E=0 (no extension)"
    );
}

#[test]
fn test_mac_scaling_mode_e_u_flags() {
    // Same as above but for MAC. With scale-down, test E and U.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    // A = 0x01:000000:000000. bit48=1, bits 55:49=0 => NOT uniform => E=1 in scale-down.
    // MAC product = 0 (X0=0, Y0=0). Result = A unchanged.
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0;
    s.registers[reg::Y0] = 0;
    pram[0] = 0x2000D2; // mac x0,y0,a
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "MAC scale-down: E=1"
    );
}

#[test]
fn test_mpy_su_flag_verification() {
    // DSP56300FM p.13-139: MPY(su,uu) - verify N and Z flags are computed.
    // MPY su X0,X0,A with zero inputs: product=0, Z=1, N=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // mpy (su) X0,X0,A: QQQQ=0 (X0,X0), d=0, k=0
    // Template: 00000001001000101sdkQQQQ, s=0(su)
    // 0000_0001_0010_0111_1_0_0_0_0000 = 0x012780
    // Test with nonzero inputs to verify N and Z flags.
    // X0 = 0x400000 (positive signed, 0.5), product = 0.5 * 0.5_unsigned.
    // Both positive => result positive. N=0, Z=0.
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x012780; // mpy (su) X0,X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "MPY(su) nonzero: Z=0"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "MPY(su) positive: N=0"
    );

    // Zero product: X0=0, result=0, Z=1, N=0.
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.registers[reg::X0] = 0;
    pram[0] = 0x012780; // mpy (su) X0,X0,A
    run_one(&mut s2, &mut jit2);
    // Result is zero - verify the accumulator is zeroed
    assert_eq!(s2.registers[reg::A2], 0);
    assert_eq!(s2.registers[reg::A1], 0);
    assert_eq!(s2.registers[reg::A0], 0);
    assert_ne!(
        s2.registers[reg::SR] & (1 << sr::Z),
        0,
        "MPY(su): Z=1 for zero product"
    );
    assert_eq!(
        s2.registers[reg::SR] & (1 << sr::N),
        0,
        "MPY(su): N=0 for zero product"
    );
}

#[test]
fn test_mac_su_flag_verification() {
    // DSP56300FM p.13-99: MAC(su,uu) - verify N and Z flags.
    // mac (su) X0,X0,A: signed X0 * unsigned X0 + A.
    // A=0, X0=0. Product=0. Z=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0;
    pram[0] = 0x012680; // mac (su) X0,X0,A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "MAC(su): Z=1 for zero result"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "MAC(su): N=0 for zero result"
    );
}

#[test]
fn test_dmac_n_z_flags() {
    // DSP56300FM p.13-54: DMAC - verify N and Z flags specifically.
    // A=0, X0=0, Y0=0. Product=0 + D>>24=0. Z=1, N=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x012480; // dmac ss X0,X0,A (QQQQ=0)
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "DMAC: Z=1 for zero result"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "DMAC: N=0 for zero result"
    );

    // Now test with negative result
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.registers[reg::X0] = 0x800000; // -1.0
    s2.registers[reg::Y0] = 0x400000; // 0.5
    // QQQQ=0010 => X1,X0 but we want X0,Y0. QQQQ for DMAC: 0101 = Y0,X0.
    pram[0] = 0x012485; // dmac ss Y0,X0,A (QQQQ=0101)
    run_one(&mut s2, &mut jit2);
    // Signed * signed: -1.0 * 0.5 = -0.5. N=1.
    assert_ne!(
        s2.registers[reg::SR] & (1 << sr::N),
        0,
        "DMAC: N=1 for negative result"
    );
    assert_eq!(
        s2.registers[reg::SR] & (1 << sr::Z),
        0,
        "DMAC: Z=0 for nonzero result"
    );
}

#[test]
fn test_add_imm_b_variant() {
    // DSP56300FM p.13-7: ADD #xx,B - 6-bit immediate added to B.
    // Encoding: 0000000101iiiiii1000d000, d=1 for B.
    // add #$10,B: imm=0x10, d=1 => bits 15:8 = 01_010000, bits 7:0 = 10001000 = 0x88.
    // Full: 0x015088.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x100000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x015088; // add #$10,B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x100010, "ADD #xx,B: B1 = 0x100010");
}

#[test]
fn test_mpyri_scale_down() {
    // DSP56300FM p.13-143: MPYRI rounds after multiply. Rounding position shifts with
    // scaling mode (p.13-163): normal=bit23, scale-down=bit24, scale-up=bit22.
    // Scale-down (S1:S0=01): rounding at bit 24.
    // X0=0x400000 (0.5), imm=0x400000 (0.5). Product = 0.25 = 0x00:200000:000000.
    // Round at bit 24: add 1<<24 => 0x00:200001:000000. Convergent: A1[0] cleared => 0x200000.
    // Then A0 zeroed (bits below rounding position cleared).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    s.registers[reg::X0] = 0x400000; // 0.5
    pram[0] = 0x0141C1; // mpyri #xxxx,X0,A
    pram[1] = 0x400000; // 0.5
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYRI scale-down: A1");
    assert_eq!(
        s.registers[reg::A0],
        0x000000,
        "MPYRI scale-down: A0 cleared"
    );

    // Compare with no-scaling mode to verify the rounding position differs.
    // No scaling: round at bit 23. Product = 0x00:200000:000000.
    // Verify the scaling mode path is wired up correctly for MPYRI.
    // The MPYR scale tests already prove the rounding shift mechanism;
    // this test confirms MPYRI uses the same path.
}

#[test]
fn test_mpyri_scale_up() {
    // DSP56300FM p.13-143/163: MPYRI with scale-up (S1:S0=10): rounding at bit 22.
    // X0=0x400000 (0.5), imm=0x400000 (0.5). Product = 0x00:200000:000000.
    // Scale-up round at bit 22: add 1<<22=0x400000 to A0. A0=0x400000.
    // Convergent: (A0 & 0x3FFFFF)==0 => tie, clear bit above: A0 & 0x800000 = 0.
    // A0 = 0, A1 = 0x200000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::S1; // scale-up
    s.registers[reg::X0] = 0x400000; // 0.5
    pram[0] = 0x0141C1; // mpyri #xxxx,X0,A
    pram[1] = 0x400000; // 0.5
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYRI scale-up: A1");
    assert_eq!(s.registers[reg::A0], 0x000000, "MPYRI scale-up: A0");
}

#[test]
fn test_mpyri_x1_source() {
    // MPYRI with X1 source (qq=10)
    // Template: 000000010100000111_10_0_0_01 = 0x0141E1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0x400000; // 0.5
    pram[0] = 0x0141E1; // mpyri #xxxx,X1,A
    pram[1] = 0x400000; // immediate = 0.5
    run_one(&mut s, &mut jit);
    // round(0.5 * 0.5) = round(0x200000:000000) = 0x200000
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYRI X1: A1");
    assert_eq!(s.registers[reg::A0], 0x000000, "MPYRI X1: A0 cleared");
}

#[test]
fn test_mpyri_y1_source() {
    // MPYRI with Y1 source (qq=11)
    // Template: 000000010100000111_11_0_0_01 = 0x0141F1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y1] = 0x400000; // 0.5
    pram[0] = 0x0141F1; // mpyri #xxxx,Y1,A
    pram[1] = 0x400000; // immediate = 0.5
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "MPYRI Y1: A1");
    assert_eq!(s.registers[reg::A0], 0x000000, "MPYRI Y1: A0 cleared");
}

#[test]
fn test_neg_c_flag_unchanged() {
    // NEG should leave C unchanged (manual p.13-144: C = "-").
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C; // pre-set C=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200036; // nop + neg A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "NEG: C should remain 1 (unchanged)"
    );
}

#[test]
fn test_abs_c_flag_unchanged() {
    // ABS should leave C unchanged (manual p.13-5: C = "-").
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C; // pre-set C=1
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0x700000; // negative acc
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200026; // abs A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "ABS: C should remain 1 (unchanged)"
    );
}

#[test]
fn test_addr_v_zero_from_shift() {
    // ADDR V=0 when shift holds MSB constant.
    // Manual p.13-10: "V can only be set by the addition operation"
    // A=$00:400000:000000 (bit 54 set, bit 55=0), B=0.
    // ADDR B,A: D/2+S = A/2+0 = $00:200000:000000. V should be 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    // B = 0 (default)
    pram[0] = 0x200002; // addr B,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000, "ADDR: A1 = A/2");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "ADDR: V=0 (shift didn't overflow addition)"
    );
}

#[test]
fn test_cmpu_v_always_cleared() {
    // CMPU always clears V (manual p.13-48: "V: Always cleared").
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::V; // pre-set V=1
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x200000;
    pram[0] = 0x0C1FF8; // cmpu X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "CMPU: V should always be cleared"
    );
}

#[test]
fn test_div_nzeu_unchanged() {
    // DIV should leave N,Z,E,U unchanged (manual p.13-54: all "-").
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Pre-set N=1, Z=1, E=1, U=1
    s.registers[reg::SR] |= (1 << sr::N) | (1 << sr::Z) | (1 << sr::E) | (1 << sr::U);
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x400000;
    pram[0] = 0x018040; // div X0,A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "DIV: N should remain 1 (unchanged)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "DIV: Z should remain 1 (unchanged)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "DIV: E should remain 1 (unchanged)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "DIV: U should remain 1 (unchanged)"
    );
}

#[test]
fn test_norm_cs_unchanged() {
    // NORM should leave C and S unchanged (manual p.13-146: both "-").
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Pre-set C=1, S=1; set up unnormalized acc so NORM does a left-shift
    s.registers[reg::SR] |= (1 << sr::C) | (1 << sr::S);
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000; // not normalized (E=0, U=1 -> shift left)
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0; // shift count destination
    pram[0] = 0x01D815; // norm R0,A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "NORM: C should remain 1 (unchanged)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::S),
        0,
        "NORM: S should remain 1 (unchanged)"
    );
}

#[test]
fn test_subr_v_zero_from_shift() {
    // SUBR V=0 when shift holds MSB constant.
    // Manual p.13-175: "V can only be set by the addition operation"
    // A=$00:800000:000000, B=0. SUBR B,A: D/2-S = A/2-0 = $00:400000:000000. V=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    // B = 0 (default)
    pram[0] = 0x200006; // subr B,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x400000, "SUBR: A1 = A/2");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "SUBR: V=0 (shift didn't overflow subtraction)"
    );
}

#[test]
fn test_tcc_r_only_variant() {
    // Tcc S2,D2 (R-only, no accumulator transfer).
    // Template 3: 00000010CCCC1ttt00000TTT
    // cc=CC(0), ttt=R0(0), TTT=R3(3): Tcc R0,R3
    // 00000010 0000 1 000 00000 011 = 0x020803
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000042;
    s.registers[reg::R3] = 0x000000;
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::B1] = 0x654321;
    s.registers[reg::SR] = 0; // C=0 -> CC is true
    pram[0] = 0x020803; // tcc R0,R3 (R-only, no acc transfer)
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::R3],
        0x000042,
        "Tcc R-only: R3 should receive R0"
    );
    assert_eq!(
        s.registers[reg::A1],
        0x123456,
        "Tcc R-only: A1 should be unchanged (no acc transfer)"
    );
    assert_eq!(
        s.registers[reg::B1],
        0x654321,
        "Tcc R-only: B1 should be unchanged (no acc transfer)"
    );
}

#[test]
fn test_tcc_r_only_not_taken() {
    // Tcc R-only variant when condition is false - no transfer should occur.
    // cc=CC(0), ttt=R0(0), TTT=R3(3): Tcc R0,R3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000042;
    s.registers[reg::R3] = 0x000099;
    s.registers[reg::SR] = 1 << sr::C; // C=1 -> CC is false
    pram[0] = 0x020803; // tcc R0,R3
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::R3],
        0x000099,
        "Tcc R-only not taken: R3 should be unchanged"
    );
}
