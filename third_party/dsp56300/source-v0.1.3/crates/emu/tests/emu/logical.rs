use super::*;

#[test]
fn test_parallel_and_x0_a() {
    // and X0,A: ALU byte 0x46 (JJ=0->X0, d=0->A, op=6->and)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xFF00FF;
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x200046; // nop + and X0,A
    run_one(&mut s, &mut jit);
    // AND operates on A1 only
    assert_eq!(s.registers[reg::A1], 0x120056);
}

#[test]
fn test_parallel_or_x0_a() {
    // or X0,A: ALU byte 0x42 (JJ=0->X0, d=0->A, op=2->or)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x00FF00;
    s.registers[reg::A1] = 0x120034;
    pram[0] = 0x200042; // nop + or X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x12FF34);
}

#[test]
fn test_parallel_eor_x0_a() {
    // eor X0,A: ALU byte 0x43 (JJ=0->X0, d=0->A, op=3->eor)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xFFFFFF;
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x200043; // nop + eor X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0xEDCBA9);
}

#[test]
fn test_parallel_and_x0_b() {
    // AND X0,B (alu_byte 0x4E)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xFF00FF;
    s.registers[reg::B1] = 0x123456;
    pram[0] = 0x20004E;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x120056);
}

#[test]
fn test_parallel_or_x0_b() {
    // OR X0,B (alu_byte 0x4A): JJ=0->X0, d=1->B, op=2->or
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x00FF00;
    s.registers[reg::B1] = 0x120034;
    pram[0] = 0x20004A;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x12FF34);
}

#[test]
fn test_parallel_eor_x0_b() {
    // EOR X0,B (alu_byte 0x4B)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0xFFFFFF;
    s.registers[reg::B1] = 0x123456;
    pram[0] = 0x20004B;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0xEDCBA9);
}

#[test]
fn test_parallel_not_a() {
    // not A: ALU byte 0x17
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x00FF00;
    pram[0] = 0x200017; // nop + not A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0xFF00FF);
}

#[test]
fn test_parallel_not_b() {
    // NOT B (alu_byte 0x1F): operates on B1 only
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B1] = 0x00FF00;
    pram[0] = 0x20001F;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0xFF00FF);
}

#[test]
fn test_parallel_lsl_b() {
    // alu_byte 0x3B: lsl B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x3B;
    s.registers[reg::B1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x400000);
}

#[test]
fn test_parallel_lsr_b() {
    // alu_byte 0x2B: lsr B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x2B;
    s.registers[reg::B1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_parallel_rol_b() {
    // alu_byte 0x3F: rol B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x3F;
    s.registers[reg::B1] = 0x200000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x400000);
}

#[test]
fn test_parallel_ror_b() {
    // alu_byte 0x2F: ror B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200000 | 0x2F;
    s.registers[reg::B1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x200000);
}

#[test]
fn test_lsl() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // lsl A: A1=0x800000 -> 0x000000, C=1 (old bit 23)
    pram[0] = 0x200033;
    s.registers[reg::A1] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 1);

    // lsl A: A1=0x400000 -> 0x800000, C=0
    pram[1] = 0x200033;
    s.registers[reg::A1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_lsr() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // lsr A: A1=0x800001 -> 0x400000, C=1 (old bit 0)
    pram[0] = 0x200023;
    s.registers[reg::A1] = 0x800001;
    s.registers[reg::A0] = 0x123456;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x400000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 1);

    // lsr A: A1=0x000000 -> 0, C=0
    pram[1] = 0x200023;
    s.registers[reg::A1] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_ror() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // ror A: A1=0x000001, C=0 -> old C(0) into bit23, bit0(1) into C
    // result: A1=0x000000, C=1
    pram[0] = 0x200027;
    s.registers[reg::A1] = 0x000001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 1);

    // ror A: A1=0x800001, C=1 (from above) -> old C(1) into bit23
    // shifted=0x400000, result=0xC00000, C=1
    pram[1] = 0x200027;
    s.registers[reg::A1] = 0x800001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0xC00000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 1);

    // ror A: A1=0x800000, C=1 (from above) -> old C(1) into bit23
    // shifted=0x400000, result=0xC00000, bit0 was 0 -> C=0
    pram[2] = 0x200027;
    s.registers[reg::A1] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0xC00000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_rol() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // rol A: A1=0x800000, C=0 -> old C(0) into bit0, bit23(1) into C
    // result: A1=0x000000, C=1
    pram[0] = 0x200037;
    s.registers[reg::A1] = 0x800000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 1);

    // rol A: A1=0x400000, C=1 (from above) -> old C(1) into bit0
    // shifted=0x800000, result=0x800001, C=0
    pram[1] = 0x200037;
    s.registers[reg::A1] = 0x400000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800001);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_and_imm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // and #$0F,A
    pram[0] = 0x014086 | (0x0F << 8);
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0xFF00FF;
    s.registers[reg::A0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::A1], 0x0F);
}

#[test]
fn test_or_imm() {
    // or #$1F,A: template 0000000101iiiiii1000d010
    // imm=$1F (6-bit), d=0 (A)
    // 0000_0001_0101_1111_1000_0010 = 0x015F82
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x015F82; // or #$1F,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x10001F);
}

#[test]
fn test_eor_imm() {
    // eor #$3F,A: template 0000000101iiiiii1000d011
    // imm=$3F, d=0 (A)
    // 0000_0001_0111_1111_1000_0011 = 0x017F83
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x00003F;
    pram[0] = 0x017F83; // eor #$3F,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000); // XOR with same value = 0
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z set
}

#[test]
fn test_and_long() {
    // and #xxxx,A: 1100d110, d=0 -> 0xC6 -> 0x0140C6
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0xFF00FF;
    pram[0] = 0x0140C6; // and #xxxx,A
    pram[1] = 0x0F0F0F;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x0F000F);
}

#[test]
fn test_or_long() {
    // or #xxxx,A: 1100d010, d=0 -> 0xC2 -> 0x0140C2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x100000;
    pram[0] = 0x0140C2; // or #xxxx,A
    pram[1] = 0x000001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x100001);
}

#[test]
fn test_eor_long() {
    // eor #xxxx,A: template 00000001010000001100d011
    // d=0 (A): 0000_0001_0100_0000_1100_0011 = 0x0140C3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0xFF00FF;
    pram[0] = 0x0140C3; // eor #xxxx,A
    pram[1] = 0xFFFFFF; // immediate
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x00FF00);
}

#[test]
fn test_andi_ccr() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0xFF;
    // andi #$F0,CCR -> 00000000 11110000 101110 01
    // imm = 0xF0, dest = 01 (CCR)
    // opcode = 0x00F0B9
    pram[0] = 0x00F0B9;
    run_one(&mut s, &mut jit);
    // CCR (low byte) should be ANDed with 0xF0
    assert_eq!(s.registers[reg::SR] & 0xFF, 0xF0);
}

#[test]
fn test_andi_mr() {
    // andi #$0F,MR: opcode = 0x000FB8 (dest=0, MR = SR bits 15:8)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000FB8;
    s.registers[reg::SR] = 0xFF00; // MR = 0xFF
    run_one(&mut s, &mut jit);
    // MR ANDed with 0x0F -> SR bits 15:8 = 0x0F
    assert_eq!(s.registers[reg::SR] & 0xFF00, 0x0F00);
}

#[test]
fn test_andi_omr() {
    // andi #$0F,OMR: opcode = 0x000FBA (dest=2)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000FBA;
    s.registers[reg::OMR] = 0xFF;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::OMR] & 0xFF, 0x0F);
}

#[test]
fn test_andi_invalid_dest() {
    // ANDI #$FF,?? with dest=3 (undefined) -> no-op (covers line 2225)
    // Pattern: 00000000_iiiiiiii_101110EE with EE=11
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0x0300; // set some SR bits
    pram[0] = 0x00FFBB; // andi #$FF, dest=3
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SR], 0x0300); // SR unchanged
}

#[test]
fn test_andi_omr_preserves_upper() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::OMR] = 0x0300FF; // upper bits set
    // ANDI #$0F,OMR: encoding = 00000000_0iiiiiiii_10111010
    // imm=0x0F, dest=10 (OMR) = 0x000FBA
    pram[0] = 0x000FBA;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::OMR],
        0x03000F,
        "ANDI should preserve upper OMR bits"
    );
}

#[test]
fn test_andi_eom_destination() {
    // ANDI with EOM destination (EE=3) was rejected as invalid by decoder.
    // ANDI #$0F,EOM should AND bits 15:8 of OMR with $0F (clearing bits 11:8).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set OMR with bits in both COM (7:0) and EOM (15:8)
    s.registers[reg::OMR] = 0x00FF55; // COM=$55, EOM=$FF

    // ANDI #$0F,EOM: opcode 00000000_00001111_10111011 = 0x000FBB
    pram[0] = 0x000FBB;
    run_one(&mut s, &mut jit);

    // EOM should be $0F (AND $FF with $0F), COM should remain $55
    assert_eq!(
        s.registers[reg::OMR],
        0x000F55,
        "ANDI #$0F,EOM: EOM=$0F, COM=$55"
    );
    assert_eq!(s.pc, 1);
}

#[test]
fn test_andi_mr_preserves_sr_upper() {
    // ANDI #xx,MR clobbers SR bits 23:16 (LF, FV, SA, CE, SM, RM).
    // Set SR with bits in upper byte (23:16), MR (15:8), and CCR (7:0).
    // ANDI #$0F,MR should only AND bits 15:8, preserving 23:16 and 7:0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // SR = 0x2AFF55: bits 23:16 = 0x2A (RM, SM set), MR = 0xFF, CCR = 0x55
    s.registers[reg::SR] = 0x2AFF55;

    // ANDI #$0F,MR: opcode = 0x000FB8 (dest=0)
    pram[0] = 0x000FB8;
    run_one(&mut s, &mut jit);

    // MR should be $0F (AND $FF with $0F), CCR should remain $55, upper byte $2A preserved
    assert_eq!(
        s.registers[reg::SR],
        0x2A0F55,
        "ANDI #$0F,MR must preserve SR bits 23:16; got {:#08X}",
        s.registers[reg::SR]
    );
}

#[test]
fn test_andi_ccr_preserves_sr_upper() {
    // ANDI #xx,CCR clobbers SR bits 23:16.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // SR = 0x2A00FF: bits 23:16 = 0x2A, MR = 0x00, CCR = 0xFF
    s.registers[reg::SR] = 0x2A00FF;

    // ANDI #$F0,CCR: opcode = 0x00F0B9 (dest=1)
    pram[0] = 0x00F0B9;
    run_one(&mut s, &mut jit);

    // CCR should be $F0, MR should remain $00, upper byte $2A preserved
    assert_eq!(
        s.registers[reg::SR],
        0x2A00F0,
        "ANDI #$F0,CCR must preserve SR bits 23:16; got {:#08X}",
        s.registers[reg::SR]
    );
}

#[test]
fn test_ori_ccr() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0;
    // ori #$04,CCR -> 00000000 00000100 111110 01
    // imm = 0x04, dest = 01 (CCR)
    // opcode = 0x0004F9
    pram[0] = 0x0004F9;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & 0xFF, 0x04);
}

#[test]
fn test_ori_mr() {
    // ori #$0F,MR: opcode = 0x000FF8 (dest=0)
    // Use #$0F to avoid SR bit 12 which is masked by REG_MASKS[SR]=0xEFFF.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000FF8;
    s.registers[reg::SR] = 0x0000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & 0xFF00, 0x0F00);
}

#[test]
fn test_ori_omr() {
    // ori #$04,OMR: opcode = 0x0004FA (dest=2)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0004FA;
    s.registers[reg::OMR] = 0x02;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::OMR] & 0xFF, 0x06);
}

#[test]
fn test_ori_invalid_dest() {
    // ORI #$FF,?? with dest=3 (undefined) -> no-op (covers line 2254)
    // Pattern: 00000000_iiiiiiii_111110EE with EE=11
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0x0300;
    pram[0] = 0x00FFFB; // ori #$FF, dest=3
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SR], 0x0300); // SR unchanged
}

#[test]
fn test_clb_positive() {
    // clb A,A: template 0000110000011110000000SD
    // S=0 (A), D=0 (A): 0000_1100_0001_1110_0000_0000 = 0x0C1E00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A = 0x00_010000_000000 -> 56-bit has many leading zeros
    // Bits: 0000_0000 0000_0001 0000_0000_0000_0000 0000_0000_0000_0000_0000_0000
    // Leading zeros in 56-bit: 15 (from bit 55 down to bit 40)
    // CLB returns 1 - count of leading sign bits
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0x010000;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1E00; // clb A,A
    run_one(&mut s, &mut jit);
    // 56-bit value = 0x00_010000_000000
    // Shift left 8: 0x0001_0000_0000_0000, clz = 15
    // CLB result = 9 - 15 = -6 -> 24-bit: 0xFFFFFA
    assert_eq!(s.registers[reg::A1], 0xFFFFFA);
}

#[test]
fn test_clb_negative() {
    // A = 0xFF_F00000_000000 -> negative, leading 1s
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xF00000;
    s.registers[reg::A2] = 0xFF;
    pram[0] = 0x0C1E00; // clb A,A
    run_one(&mut s, &mut jit);
    // 56-bit: 0xFF_F00000_000000, negative -> invert with 56-bit mask
    // Inverted: 0x00_0FFFFF_FFFFFF
    // Shift left 8: 0x000F_FFFF_FFFF_FF00
    // clz = 12 (first set bit at position 51)
    // result = 9 - 12 = -3 -> 24-bit: 0xFFFFFD
    assert_eq!(s.registers[reg::A1], 0xFFFFFD);
}

#[test]
fn test_clb_all_zeros_returns_zero() {
    // CLB: manual Note 1 says all-zeros accumulator should produce result 0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // CLB A,B: opcode 0x0C1E20
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::B2] = 0;
    s.registers[reg::B1] = 0;
    s.registers[reg::B0] = 0;
    s.registers[reg::SR] = 0;
    pram[0] = 0x0C1E20;
    run_one(&mut s, &mut jit);
    // B1 should be 0 (result stored in B accumulator A1 position)
    assert_eq!(s.registers[reg::B1], 0, "CLB of all-zeros should return 0");
}

#[test]
fn test_lsl_imm() {
    // lsl #3,A: template 000011000001111010iiiiiD
    // iiiii=3 (00011), D=0 (A)
    // 0000_1100_0001_1110_1000_0110 = 0x0C1E86
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000001;
    pram[0] = 0x0C1E86;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000008); // 1 << 3
}

#[test]
fn test_lsl_imm_carry() {
    // lsl #1,A: shift A1 left by 1, carry = bit 23
    // iiiii=1 (00001), D=0 (A)
    // 0000_1100_0001_1110_1000_0010 = 0x0C1E82
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x800000; // bit 23 set
    pram[0] = 0x0C1E82;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000); // shifted out
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry set
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // zero
}

#[test]
fn test_lsl_imm_b() {
    // LSL #5,B. Covers B accumulator destination in lsl_imm.
    // Pattern: 000011000001111010iiiiiD, iiiii=00101, D=1 -> 0x0C1E8B
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1E8B; // LSL #5,B
    s.registers[reg::B1] = 0x000001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x000020); // 1 << 5 = 32
}

#[test]
fn test_lsl_imm_zero() {
    // LSL #0,A. Covers early return when shift=0.
    // iiiii=00000, D=0 -> 0x0C1E80
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1E80; // LSL #0,A
    s.registers[reg::A1] = 0x123456;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123456); // unchanged
}

#[test]
fn test_lsr_imm() {
    // lsr #5,A: template 000011000001111011iiiiiD
    // iiiii=5 (bits 5:1 = 00101), D=0 (A)
    // 0000_1100_0001_1110_1100_1010 = 0x0C1ECA
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000020; // bit 5 set (= 32)
    pram[0] = 0x0C1ECA; // lsr #5,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000001); // 32 >> 5 = 1
}

#[test]
fn test_lsr_imm_flags() {
    // lsr #1,A -> shift out bit 0, check carry
    // iiiii=1, D=0: 0000_1100_0001_1110_1100_0010 = 0x0C1EC2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000003; // bits 0 and 1 set
    pram[0] = 0x0C1EC2; // lsr #1,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000001); // 3 >> 1 = 1
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = shifted-out bit
}

#[test]
fn test_lsl_reg() {
    // lsl X0,A: template 00001100000111100001sssD
    // sss=100 (X0), D=0 (A)
    // 0000_1100_0001_1110_0001_1000 = 0x0C1E18
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 4; // shift by 4
    s.registers[reg::A1] = 0x001000;
    pram[0] = 0x0C1E18; // lsl X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x010000); // 0x001000 << 4
}

#[test]
fn test_lsr_reg() {
    // lsr X0,A: template 00001100000111100011sssD
    // sss=100 (X0), D=0
    // 0000_1100_0001_1110_0011_1000 = 0x0C1E38
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 4; // shift by 4
    s.registers[reg::A1] = 0x010000;
    pram[0] = 0x0C1E38; // lsr X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x001000); // 0x010000 >> 4
}

#[test]
fn test_ror_n_flag() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // C=1 before ROR, A1=0x000002 (bit 0=0, bit 1=1)
    // ROR: old C(=1) -> bit 23 of result, old bit 0 (=0) -> new C
    // N = bit 23 of result = old C = 1
    s.registers[reg::SR] = 0xC00301; // C=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000002;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200027; // ror A (parallel: nop)
    run_one(&mut s, &mut jit);
    let n = (s.registers[reg::SR] >> 3) & 1;
    assert_eq!(n, 1, "ROR: N should be bit 23 of result (= old carry)");
    let c = s.registers[reg::SR] & 1;
    assert_eq!(c, 0, "ROR: new C should be old bit 0 (= 0)");
}

#[test]
fn test_lsl_imm_zero_clears_c() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0xC00301; // C=1 before
    s.registers[reg::A1] = 0x123456;
    // LSL #0,A encoding: 000011000001111010iiiiiD
    // iiiii=00000, D=0 (A) = 0x0C1E80
    pram[0] = 0x0C1E80;
    run_one(&mut s, &mut jit);
    let c = s.registers[reg::SR] & 1;
    assert_eq!(c, 0, "LSL #0 should clear C");
    assert_eq!(
        s.registers[reg::A1],
        0x123456,
        "LSL #0 should not change value"
    );
}

#[test]
fn test_lsr_imm_zero_clears_c() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0xC00301; // C=1
    s.registers[reg::A1] = 0x123456;
    // LSR #0,A encoding: 000011000001111011iiiiiD
    // iiiii=00000, D=0 (A) = 0x0C1EC0
    pram[0] = 0x0C1EC0;
    run_one(&mut s, &mut jit);
    let c = s.registers[reg::SR] & 1;
    assert_eq!(c, 0, "LSR #0 should clear C");
}

#[test]
fn test_lsl_imm_zero_updates_nzv() {
    // LSL #0,A -> opcode 0x0C1E80
    // Set A1 = 0x800000 (bit 23 set) and V=1 from a prior instruction.
    // After LSL #0: C=0, N=1 (bit 23 set), Z=0, V=0.
    // Bug: only clears C, leaves N/Z/V stale.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1E80; // LSL #0,A
    s.registers[reg::A1] = 0x800000;
    // Set V=1, N=0 in SR to see if they get updated
    s.registers[reg::SR] = 1 << sr::V;
    run_one(&mut s, &mut jit);
    let n = (s.registers[reg::SR] >> sr::N) & 1;
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "LSL #0: C should be cleared");
    assert_eq!(v, 0, "LSL #0: V should be cleared");
    assert_eq!(n, 1, "LSL #0: N should be set (bit 23 of A1 is 1)");
    assert_eq!(z, 0, "LSL #0: Z should be clear (A1 is nonzero)");
}

#[test]
fn test_lsr_imm_zero_updates_nzv() {
    // LSR #0,A -> opcode 0x0C1EC0
    // Pattern: 000011000001111011iiiiiD, iiiii=00000, D=0
    // Set A1 = 0 and V=1 from prior instruction.
    // After LSR #0: C=0, N=0, Z=1, V=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C1EC0; // LSR #0,A
    s.registers[reg::A1] = 0;
    s.registers[reg::SR] = (1 << sr::V) | (1 << sr::N); // V=1, N=1 (stale)
    run_one(&mut s, &mut jit);
    let n = (s.registers[reg::SR] >> sr::N) & 1;
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "LSR #0: C should be cleared");
    assert_eq!(v, 0, "LSR #0: V should be cleared");
    assert_eq!(n, 0, "LSR #0: N should be clear (A1 is 0)");
    assert_eq!(z, 1, "LSR #0: Z should be set (A1 is 0)");
}

#[test]
fn test_extract_imm() {
    // extract #CO,A,A (0000110000011000000s000D, s=0 D=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1800; // extract #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    // Extracts A1[23:16]=0xAB, sign-extended to 56 bits
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0xFFFFFF);
    assert_eq!(s.registers[reg::A0], 0xFFFFAB);
}

#[test]
fn test_extractu_imm() {
    // extractu #CO,A,A (0000110000011000100s000D, s=0 D=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1880; // extractu #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    // Extracts A1[23:16]=0xAB, zero-extended
    assert_eq!(s.registers[reg::A2], 0);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::A0], 0xAB);
}

#[test]
fn test_insert_imm() {
    // insert #CO,X0,A (00001100000110010qqq000D, qqq=4 D=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x0000FF;
    s.registers[reg::A1] = 0x000000;
    pram[0] = 0x0C1940; // insert #CO,X0,A
    pram[1] = 0x008018; // width=8, offset=24
    run_one(&mut s, &mut jit);
    // 0xFF inserted at A1[7:0]
    assert_eq!(s.registers[reg::A1], 0x0000FF);
    assert_eq!(s.registers[reg::A0], 0x000000);
}

#[test]
fn test_merge() {
    // merge X0,A: template 00001100000110111000sssD
    // sss=100 (X0), D=0 (A)
    // 0000_1100_0001_1011_1000_1000 = 0x0C1B88
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000ABC; // lower 12 bits = 0xABC
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x0C1B88; // merge X0,A
    run_one(&mut s, &mut jit);
    // new A1 = (X0[11:0] << 12) | old_A1[11:0] = (0xABC << 12) | 0x456
    // Result: 0xABC456
    assert_eq!(s.registers[reg::A1], 0xABC456);
}

#[test]
fn test_extract_reg() {
    // extract X0,A,A (0000110000011010000sSSSD, s=0 SSS=100 (X0) D=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0;
    s.registers[reg::X0] = 0x008028; // width=8, offset=40
    pram[0] = 0x0C1A08; // extract X0,A,A
    run_one(&mut s, &mut jit);
    // Extracts A1[23:16]=0xAB, sign-extended to 56 bits
    assert_eq!(s.registers[reg::A2], 0xFF);
    assert_eq!(s.registers[reg::A1], 0xFFFFFF);
    assert_eq!(s.registers[reg::A0], 0xFFFFAB);
}

#[test]
fn test_extractu_reg() {
    // extractu X0,A,A (0000110000011010100sSSSD, s=0 SSS=100 (X0) D=0)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0;
    s.registers[reg::X0] = 0x008028; // width=8, offset=40
    pram[0] = 0x0C1A88; // extractu X0,A,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A2], 0);
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(s.registers[reg::A0], 0xAB);
}

#[test]
fn test_insert_reg() {
    // insert X0,X0,A (00001100000110110qqqSSSD, qqq=100 (X0) SSS=100 (X0) D=0)
    // X0 = source data AND control word (width=8, offset=24)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A2] = 0;
    s.registers[reg::X0] = 0x008018; // width=8, offset=24
    pram[0] = 0x0C1B48; // insert X0,X0,A
    run_one(&mut s, &mut jit);
    // Low 8 bits of X0 (0x18) inserted at offset 24 -> A1[7:0]
    assert_eq!(s.registers[reg::A1], 0x000018);
}

#[test]
fn test_extract_from_b() {
    // extract X0,B,B: source accumulator B, dest B
    // Template: 0000110000011010000sSSSD
    // s=1 (B), SSS=100 (X0), D=1 (B): 0x0C1A19
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // B = 0x00_ABCDEF_000000 -- extract 8 bits at offset 40 (B1[23:16] = 0xAB)
    s.registers[reg::B0] = 0;
    s.registers[reg::B1] = 0xABCDEF;
    s.registers[reg::B2] = 0;
    s.registers[reg::X0] = 0x008028; // width=8, offset=40
    pram[0] = 0x0C1A19; // extract X0,B,B
    run_one(&mut s, &mut jit);
    // Extracted value 0xAB sign-extended (bit 7 set -> negative)
    assert_eq!(s.registers[reg::B2], 0xFF);
    assert_eq!(s.registers[reg::B1], 0xFFFFFF);
    assert_eq!(s.registers[reg::B0], 0xFFFFAB);
}

#[test]
fn test_and_reg_ccr_flags() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Sub-test 1: A1=0x800000 AND X0=0x800000 -> A1=0x800000, N=1, Z=0, V=0
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::X0] = 0x800000;
    pram[0] = 0x200046; // and X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000);
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0); // N=1
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V=0

    // Sub-test 2: A1=0x800000 AND X0=0x000000 -> A1=0, Z=1, N=0, V=0
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::X0] = 0x000000;
    s.pc = 0;
    pram[0] = 0x200046; // and X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0); // N=0
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=1
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V=0
}

#[test]
fn test_or_reg_ccr_n_flag() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::X0] = 0x800000;
    pram[0] = 0x200042; // or X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0xC00000);
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0); // N=1
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V=0
}

#[test]
fn test_eor_reg_ccr_v_cleared() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Pre-set V=1 in SR
    s.registers[reg::SR] |= 1 << sr::V;
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::X0] = 0x000000;
    pram[0] = 0x200043; // eor X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123456); // XOR with 0 = unchanged
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V always cleared
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0); // N=0 (bit 23 = 0)
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=0 (nonzero result)
}

#[test]
fn test_not_ccr_flags() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000000;
    pram[0] = 0x200017; // not A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0xFFFFFF);
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0); // N=1 (bit 47 set)
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V always cleared
}

#[test]
fn test_not_all_ones() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0xFFFFFF;
    pram[0] = 0x200017; // not A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0); // N=0
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=1
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V always cleared
}

#[test]
fn test_clb_all_ones() {
    // A = all-ones = -1 (0xFF:FFFFFF:FFFFFF).
    // Negative, so count leading 1s after sign bit = 55 redundant sign bits.
    // Result is nonzero (input not normalized), N flag set (negative shift count).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    // Result is nonzero
    assert_ne!(s.registers[reg::B1], 0x000000);
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V=0
}

#[test]
fn test_clb_ccr_flags() {
    // Test that CLB sets meaningful CCR flags and V=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Pre-set V=1 to verify it gets cleared
    s.registers[reg::SR] |= 1 << sr::V;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x010000;
    s.registers[reg::A0] = 0;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V always cleared
    // Result is nonzero so Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0);
}

#[test]
fn test_extract_ccr_vc_cleared() {
    // Pre-set V=1, C=1, run extract, verify V=0, C=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= (1 << sr::V) | (1 << sr::C);
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1800; // extract #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V cleared
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // C cleared
}

#[test]
fn test_extractu_ccr_vc_cleared() {
    // Pre-set V=1, C=1, run extractu, verify V=0, C=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= (1 << sr::V) | (1 << sr::C);
    s.registers[reg::A0] = 0;
    s.registers[reg::A1] = 0xABCDEF;
    s.registers[reg::A2] = 0;
    pram[0] = 0x0C1880; // extractu #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V cleared
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // C cleared
}

#[test]
fn test_insert_preserves_outside_bits() {
    // Insert a small field and verify bits outside the field are preserved
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x0000AB; // source data low 8 bits = 0xAB
    s.registers[reg::A1] = 0xFFFF00; // pre-fill A1 with bits set outside field
    pram[0] = 0x0C1940; // insert #CO,X0,A
    pram[1] = 0x008000; // width=8, offset=0 -> insert into A0[7:0]
    run_one(&mut s, &mut jit);
    // Bits outside the 8-bit field at offset 0 should be preserved
    assert_eq!(s.registers[reg::A1], 0xFFFF00); // A1 untouched
    assert_eq!(s.registers[reg::A0] & 0xFF, 0xAB); // inserted field
}

#[test]
fn test_insert_ccr_vc_cleared() {
    // Pre-set V=1, C=1, run insert, verify V=0, C=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= (1 << sr::V) | (1 << sr::C);
    s.registers[reg::X0] = 0x0000FF;
    s.registers[reg::A1] = 0x000000;
    pram[0] = 0x0C1940; // insert #CO,X0,A
    pram[1] = 0x008018; // width=8, offset=24
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V cleared
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // C cleared
}

#[test]
fn test_merge_ccr_flags() {
    // merge X0,A with X0=0 and A1=0 -> result=0, Z=1, N=0, V=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::A1] = 0x000000;
    pram[0] = 0x0C1B88; // merge X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=1
    assert_eq!(s.registers[reg::SR] & (1 << sr::N), 0); // N=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V=0
}

#[test]
fn test_merge_n_flag() {
    // merge X0,A: X0[11:0]=0x800, A1[11:0]=0x000
    // Result = (0x800 << 12) | 0x000 = 0x800000 -> bit 23 set -> N=1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000800;
    s.registers[reg::A1] = 0x000000;
    pram[0] = 0x0C1B88; // merge X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000);
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0); // N=1
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z=0
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V=0
}

#[test]
fn test_clb_v_always_cleared() {
    // Pre-set V=1, run CLB, verify V=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::V;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::V), 0); // V always cleared
}

#[test]
fn test_extract_positive_sign_extend() {
    // DSP56300FM p.13-70: EXTRACT extracts a bit field and sign-extends.
    // A = 0x00:7BCDEF:000000. Control word: width=8, offset=40.
    // Extracts bits 47..40 of the 56-bit accumulator = 0x7B.
    // 0x7B has bit 7 = 0, so sign extension fills with zeros.
    // Expected result: 0x00:000000:00007B (positive sign-extended).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x7BCDEF;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1800; // extract #CO,A,A (s=0, D=0)
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A2],
        0x00,
        "A2 should be 0 (positive sign-extend)"
    );
    assert_eq!(s.registers[reg::A1], 0x000000, "A1 should be 0");
    assert_eq!(
        s.registers[reg::A0],
        0x00007B,
        "A0 should contain extracted field 0x7B"
    );
    let n = (s.registers[reg::SR] >> sr::N) & 1;
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    assert_eq!(n, 0, "N should be clear (positive result)");
    assert_eq!(z, 0, "Z should be clear (non-zero result)");
}

#[test]
fn test_and_reg_y1_b() {
    // DSP56300FM p.13-28: AND S,D - logical AND of source with destination accumulator.
    // and Y1,B: ALU byte 0x7E (JJ=11->Y1, d=1->B, op=6->and)
    // B1=0xABCDEF, Y1=0xF0F0F0. Result: B1 = 0xABCDEF & 0xF0F0F0 = 0xA0C0E0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0xABCDEF;
    s.registers[reg::B0] = 0x000000;
    s.registers[reg::Y1] = 0xF0F0F0;
    pram[0] = 0x20007E; // and y1,b
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0xA0C0E0,
        "B1 should be AND of 0xABCDEF & 0xF0F0F0"
    );
}

#[test]
fn test_ori_eom() {
    // DSP56300FM p.13-152: ORI #xx,D - OR immediate to control register.
    // EOM is the upper byte (bits 23:16) of OMR. ori #$AA,eom sets OMR[23:16] |= 0xAA.
    // Starting OMR=0x000000 (after clearing reset default). Expected: OMR = 0x00AA00.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::OMR] = 0x000000;
    pram[0] = 0x00AAFB; // ori #$AA,eom
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::OMR] & 0x00FF00,
        0x00AA00,
        "EOM byte should be 0xAA"
    );
}

#[test]
fn test_ori_mr_preserves_sr_upper() {
    // DSP56300FM p.13-152: ORI #xx,mr - OR immediate to MR (bits 15:8 of SR).
    // SR=0x102A00 (MR=0x2A, some upper bits set). ori #$0F,mr -> MR |= 0x0F.
    // MR is SR[15:8], so SR[15:8] = 0x2A | 0x0F = 0x2F. Upper SR bits preserved.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0x102A00; // MR (bits 15:8) = 0x2A, upper byte = 0x10
    pram[0] = 0x000FF8; // ori #$0F,mr
    run_one(&mut s, &mut jit);
    let mr = (s.registers[reg::SR] >> 8) & 0xFF;
    assert_eq!(mr, 0x2F, "MR should be old_MR(0x2A) | 0x0F = 0x2F");
    let upper = (s.registers[reg::SR] >> 16) & 0xFF;
    assert_eq!(upper, 0x10, "Upper SR byte should be preserved");
}

#[test]
fn test_lsl_imm_max_shift() {
    // DSP56300FM p.13-93: LSL #ii,D - logical shift left by immediate amount.
    // A1=0xFFFFFF. lsl #23,a shifts left 23 positions within 24-bit A1.
    // Bit 0 (=1) moves to bit 23. All other original bits shift out.
    // Result: A1=0x800000. C=1 (last bit shifted out was bit 23 of original).
    // N=1 (bit 23 of result is set), Z=0 (result nonzero).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1EAE; // lsl #23,a
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0x800000,
        "A1 should have only bit 23 set"
    );
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    let n = (s.registers[reg::SR] >> sr::N) & 1;
    assert_eq!(c, 1, "C should be 1 (last bit shifted out)");
    assert_eq!(z, 0, "Z should be 0 (result is nonzero)");
    assert_eq!(n, 1, "N should be 1 (bit 23 of result is set)");
}

#[test]
fn test_rol_ccr_z_flag() {
    // DSP56300FM p.13-165: ROL D - rotate left through carry.
    // A1=0x000000, C=0. Old C(0) enters bit 24, old bit 47(0) exits to C.
    // Result: A1=0x000000, C=0, Z=1, N=0, V=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::SR] &= !(1 << sr::C); // ensure C=0
    pram[0] = 0x200037; // rol a
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000, "A1 should remain 0");
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    let z = (s.registers[reg::SR] >> sr::Z) & 1;
    let n = (s.registers[reg::SR] >> sr::N) & 1;
    let v = (s.registers[reg::SR] >> sr::V) & 1;
    assert_eq!(c, 0, "C should be 0 (old bit 47 was 0)");
    assert_eq!(z, 1, "Z should be 1 (result is zero)");
    assert_eq!(n, 0, "N should be 0 (bit 47 clear)");
    assert_eq!(v, 0, "V should be 0");
}

#[test]
fn test_clb_normalized_value() {
    // DSP56300FM p.13-45: CLB S,D - count leading redundant sign bits.
    // A = 0x00:100000:000000 (bit 44 is highest set bit).
    // 56-bit value: 0x0000100000000000. After left-shift by 8: 0x0010000000000000.
    // CLZ = 11 (bits 63..53 are 0, bit 52 = 1). Result = 9 - 11 = -2.
    // In 24-bit two's complement: 0xFFFFFE. Stored in B1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1E01; // clb a,b
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0xFFFFFE,
        "B1 should be -2 (0xFFFFFE) for CLB"
    );
}

#[test]
fn test_extract_width1() {
    // DSP56300FM p.13-70: EXTRACT extracts a bit field and sign-extends.
    // Width=1, offset=47 -> extract bit 47 (= A1[23]).
    // A = 0x00:800000:000000 (bit 47 set). Extracted 1-bit field = 1.
    // Sign-extend 1 bit: 1 -> 0xFF:FFFFFF:FFFFFF (-1 in 56-bit).
    // Uses same opcode as test_extract_imm: extract #CO,A,A = 0x0C1800.
    // Control word: width=1 -> bits[17:12]=000001, offset=47 -> bits[5:0]=101111.
    // Control = (1 << 12) | 47 = 0x00102F.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1800; // extract #CO,A,A
    pram[1] = 0x00102F; // width=1, offset=47
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A2],
        0xFF,
        "A2 should be 0xFF (negative sign-extend)"
    );
    assert_eq!(s.registers[reg::A1], 0xFFFFFF, "A1 should be 0xFFFFFF");
    assert_eq!(s.registers[reg::A0], 0xFFFFFF, "A0 should be 0xFFFFFF (-1)");
}

#[test]
fn test_merge_b_dest() {
    // DSP56300FM p.13-108: MERGE S,D - merge upper/lower halves.
    // ARCHITECTURE-NOTES.md: MERGE uses bits 11:0 (12 bits), not 7:0.
    // merge Y0,B: sss=5 (Y0), D=1 (B).
    // Template: 00001100000110111000sssD -> 0x0C1B8B.
    // Y0=0x000ABC, B1=0x123456.
    // Result: B1 = (Y0[11:0] << 12) | B1[11:0] = (0xABC << 12) | 0x456 = 0xABC456.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0x000ABC;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x123456;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x0C1B8B; // merge Y0,B
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0xABC456,
        "B1 should be {{Y0[11:0], B1[11:0]}}"
    );
}

#[test]
fn test_and_c_flag_unchanged() {
    // DSP56300FM p.13-11: AND - C is unchanged.
    // Pre-set C=1, perform AND X0,A, verify C remains 1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C; // pre-set C=1
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::X0] = 0xFFFFFF;
    pram[0] = 0x200046; // and X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0x123456,
        "AND with all-ones is identity"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should remain 1 (unchanged by AND)"
    );
}

#[test]
fn test_or_z_flag_zero_result() {
    // DSP56300FM p.13-150: OR - Z=1 if bits 47-24 of result are zero.
    // OR with both zero operands: A1=0, X0=0 -> result=0, Z=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::X0] = 0x000000;
    pram[0] = 0x200042; // or X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z should be 1 (zero result)"
    );
}

#[test]
fn test_or_c_flag_unchanged() {
    // DSP56300FM p.13-150: OR - C is unchanged.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::X0] = 0x200000;
    pram[0] = 0x200042; // or X0,A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should remain 1 (unchanged by OR)"
    );
}

#[test]
fn test_eor_n_flag_negative_result() {
    // DSP56300FM p.13-68: EOR - N = bit 47 of result.
    // A1=0, X0=0x800000 -> XOR -> A1=0x800000, bit 23 set -> N=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::X0] = 0x800000;
    pram[0] = 0x200043; // eor X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N should be 1 (bit 23 set)"
    );
}

#[test]
fn test_eor_c_flag_unchanged() {
    // DSP56300FM p.13-68: EOR - C is unchanged.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C;
    s.registers[reg::A1] = 0xFF00FF;
    s.registers[reg::X0] = 0xFF00FF;
    pram[0] = 0x200043; // eor X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000, "self-XOR produces zero");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should remain 1 (unchanged by EOR)"
    );
}

#[test]
fn test_not_c_unchanged_a0_a2_preserved() {
    // DSP56300FM p.13-149: NOT - C unchanged, only D[47:24] affected.
    // A0 and A2 should be preserved.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C;
    s.registers[reg::A2] = 0x42;
    s.registers[reg::A1] = 0xF0F0F0;
    s.registers[reg::A0] = 0xABCDEF;
    pram[0] = 0x200017; // not A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x0F0F0F, "NOT should complement A1");
    assert_eq!(
        s.registers[reg::A2],
        0x42,
        "A2 should be preserved through NOT"
    );
    assert_eq!(
        s.registers[reg::A0],
        0xABCDEF,
        "A0 should be preserved through NOT"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should remain 1 (unchanged by NOT)"
    );
}

#[test]
fn test_lsl_reg_carry_flag() {
    // DSP56300FM p.13-94: LSL S,D register variant - C = last bit shifted out.
    // A1=0xC00000, shift by 1 -> bit 23 (=1) shifted out -> C=1, result=0x800000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 1; // shift by 1
    s.registers[reg::A1] = 0xC00000; // bits 23 and 22 set
    pram[0] = 0x0C1E18; // lsl X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000, "0xC00000 << 1 = 0x800000");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (bit 23 shifted out)"
    );
}

#[test]
fn test_lsr_n_always_zero_single_bit() {
    // DSP56300FM p.13-95: LSR single-bit - N = bit 47 of result = always 0.
    // After LSR, bit 23 of A1 is always 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0xFFFFFF; // all bits set
    pram[0] = 0x200023; // lsr A (single-bit)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x7FFFFF, "LSR shifts 0 into bit 23");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N should be 0 after single-bit LSR"
    );
}

#[test]
fn test_lsr_imm_max_shift_23() {
    // DSP56300FM p.13-96: LSR #ii,D - shift right by 23 positions.
    // A1=0xFFFFFF, LSR #23 -> A1 = 0x000001, C=1 (last shifted-out bit).
    // iiiii=23=10111, D=0. Encoding: 000011000001111011iiiiiD
    // 0000_1100_0001_1110_1110_1110 = 0x0C1EEE
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0xFFFFFF;
    pram[0] = 0x0C1EEE; // lsr #23,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000001, "0xFFFFFF >> 23 = 1");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (last bit shifted out was 1)"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N should be 0 after LSR"
    );
}

#[test]
fn test_lsr_reg_carry_flag() {
    // DSP56300FM p.13-96: LSR S,D register variant - C = last bit shifted out.
    // A1=0x000003, shift by 1 -> bit 0 (=1) shifted out -> C=1, result=0x000001.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 1;
    s.registers[reg::A1] = 0x000003;
    pram[0] = 0x0C1E38; // lsr X0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000001);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (bit 0 shifted out)"
    );
}

#[test]
fn test_rol_n_flag_set() {
    // DSP56300FM p.13-165: ROL - N = bit 47 of result (= bit 23 of A1).
    // A1=0x400000, C=1 -> shifted: 0x800000, old C(1) into bit 0 -> 0x800001.
    // Bit 23 = 1 -> N=1. New C = old bit 23 (0) = 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C; // C=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200037; // rol A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0x800001,
        "ROL: 0x400000 <<1 | C=1 = 0x800001"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N=1 (bit 23 of result is set)"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=0 (old bit 23 was 0)"
    );
}

#[test]
fn test_ror_z_flag_set() {
    // DSP56300FM p.13-166: ROR - Z=1 when result bits 47-24 are zero.
    // A1=0x000001, C=0 -> shifted=0x000000, old C(0) into bit 23 -> 0x000000.
    // Old bit 0 (=1) -> C=1. Result is zero -> Z=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] &= !(1 << sr::C); // C=0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000001;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200027; // ror A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000, "ROR: result should be zero");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "Z=1 (zero result)");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (old bit 0 was 1)"
    );
}

#[test]
fn test_ror_v_always_cleared() {
    // DSP56300FM p.13-166: ROR - V is always cleared.
    // Pre-set V=1, run ROR, verify V=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::V; // pre-set V=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200027; // ror A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "V should be cleared by ROR"
    );
}

#[test]
fn test_clb_z_flag_zero_input() {
    // DSP56300FM p.13-42: CLB - when input is zero, result is 0, Z=1.
    // A = 0x00:000000:000000. CLB A,B -> B1=0, Z=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0x000000,
        "CLB of zero should produce 0"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z=1 for zero result"
    );
}

#[test]
fn test_clb_b_source() {
    // DSP56300FM p.13-42: CLB B,A - B as source accumulator (S=1, D=0).
    // Encoding: 0000110000011110000000SD, S=1, D=0 -> 0x0C1E02.
    // B = 0x00:040000:000000. CLB result = -4 (0xFFFFFC).
    // Same logic as test_clb_positive: pack into i64, clz, subtract 9.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x040000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x0C1E02; // clb B,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0xFFFFFC,
        "CLB B,A: B=0x00:040000:000000 should give -4"
    );
}

#[test]
fn test_clb_n_flag_explicit() {
    // DSP56300FM p.13-42: CLB - N = bit 23 of result.
    // Use input that produces negative result (which is typical for non-normalized inputs).
    // A = 0x00:010000:000000, CLB A,B -> B1 = 0xFFFFFA = -6.
    // Bit 23 of 0xFFFFFA = 1 -> N=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x010000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0xFFFFFA, "CLB result should be -6");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N=1 (bit 23 of result is set)"
    );
}

#[test]
fn test_clb_positive_result() {
    // DSP56300FM p.13-42: CLB returns positive value (+8) for already-normalized input.
    // A = 0x00:400000:000000. This is normalized (bit 54 differs from bit 55).
    // 56-bit = 0x00_400000_000000. Left-shift 8 = 0x0040_0000_0000_0000.
    // clz = 9. Result = 9 - 9 = 0. Actually let me check: if fully normalized,
    // A=0x00:400000:000000 (already normalized). CLB result = 0, N=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    // Normalized value: CLB should return 0 (no shift needed)
    assert_eq!(
        s.registers[reg::B1],
        0x000000,
        "CLB of normalized value should be 0"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N=0 (positive result)"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z=1 (result is zero)"
    );
}

#[test]
fn test_extractu_b_source() {
    // DSP56300FM p.13-72: EXTRACTU with B as source (s=1).
    // extractu #CO,B,B: template 0000110000011000100s000D, s=1, D=1.
    // 0x0C1881 + extension word.
    // B = 0x00:ABCDEF:000000. Control: width=8, offset=40 -> extract B1[23:16]=0xAB.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0xABCDEF;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x0C1891; // extractu #CO,B,B (s=1, D=1)
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B2], 0x00, "B2 should be 0 (zero-fill)");
    assert_eq!(
        s.registers[reg::B1],
        0x000000,
        "B1 should be 0 (result in B0)"
    );
    assert_eq!(
        s.registers[reg::B0],
        0xAB,
        "B0 should contain extracted field 0xAB"
    );
}

#[test]
fn test_extractu_n_flag_always_zero() {
    // DSP56300FM p.13-72: EXTRACTU (unsigned) - N should always be 0.
    // Extract a field with MSB=1. Zero-fill means result MSB cannot be 1 at bit 55.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0xFF;
    s.registers[reg::A1] = 0xFFFFFF;
    s.registers[reg::A0] = 0xFFFFFF;
    pram[0] = 0x0C1880; // extractu #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N should always be 0 for EXTRACTU"
    );
    assert_eq!(s.registers[reg::A0], 0xFF, "extracted field should be 0xFF");
}

#[test]
fn test_extractu_z_flag_zero_field() {
    // DSP56300FM p.13-72: EXTRACTU - Z=1 when extracted field is zero.
    // A = 0x00:000000:000000. Extract width=8, offset=40 -> field is 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1880; // extractu #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z=1 for zero extracted field"
    );
    assert_eq!(s.registers[reg::A0], 0, "extracted field should be 0");
}

#[test]
fn test_extract_z_flag_zero_field() {
    // DSP56300FM p.13-70: EXTRACT - Z=1 when extracted field is zero.
    // A = 0x00:000000:000000. Extract width=8, offset=40 -> field is 0.
    // Sign-extend of 0 is still 0 -> Z=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1800; // extract #CO,A,A
    pram[1] = 0x008028; // width=8, offset=40
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z=1 for zero extracted field"
    );
}

#[test]
fn test_insert_b_dest() {
    // DSP56300FM p.13-78: INSERT #CO,S,B - insert into B accumulator (D=1).
    // insert #CO,X0,B: template 00001100000110010qqq000D, qqq=4 (X0), D=1.
    // 0x0C1941. Extension: width=8, offset=24 -> insert into B1[7:0].
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x0000AB;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0xFF0000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x0C1941; // insert #CO,X0,B
    pram[1] = 0x008018; // width=8, offset=24 -> B1[7:0]
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0xFF00AB,
        "B1 should have 0xAB inserted at bits 7:0"
    );
}

#[test]
fn test_insert_nz_flags() {
    // DSP56300FM p.13-78: INSERT - N=bit 55 of result, Z=1 if all-zero result.
    // Insert 0 into zero destination -> Z=1, N=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1940; // insert #CO,X0,A
    pram[1] = 0x008018; // width=8, offset=24
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z=1 for zero result"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N=0 for zero result"
    );
}

#[test]
fn test_merge_c_unchanged() {
    // DSP56300FM p.13-108: MERGE - C is unchanged.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C; // pre-set C=1
    s.registers[reg::X0] = 0x000123;
    s.registers[reg::A1] = 0x456789;
    pram[0] = 0x0C1B88; // merge X0,A
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should remain 1 (unchanged by MERGE)"
    );
}

#[test]
fn test_clb_c_flag_unchanged() {
    // CLB should leave C unchanged (manual p.13-42: C = "-").
    // CLB A,B: 0x0C1E01 (S=0 A, D=1 B)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |= 1 << sr::C; // pre-set C=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x100000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x0C1E01; // clb A,B
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "CLB: C should remain 1 (unchanged)"
    );
}

#[test]
fn test_and_x1_source() {
    // AND with X1 source (JJ=2). Tests untested JJ variant.
    // AND X1,A: alu_byte = 0x46 | (2<<4) | (0<<3) = 0x66 -> parallel: 0x200066
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0xFF0000;
    s.registers[reg::A1] = 0x123456;
    pram[0] = 0x200066; // nop + and X1,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0x120000,
        "AND X1,A: A1 & 0xFF0000 = 0x120000"
    );
}

#[test]
fn test_and_y0_source() {
    // AND with Y0 source (JJ=1).
    // AND Y0,A: alu_byte = 0x46 | (1<<4) | (0<<3) = 0x56 -> parallel: 0x200056
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0x00FF00;
    s.registers[reg::A1] = 0xABCDEF;
    pram[0] = 0x200056; // nop + and Y0,A
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A1],
        0x00CD00,
        "AND Y0,A: A1 & 0x00FF00 = 0x00CD00"
    );
}

#[test]
fn test_or_x1_source() {
    // OR with X1 source (JJ=2).
    // OR X1,A: alu_byte = 0x42 | (2<<4) | (0<<3) = 0x62 -> parallel: 0x200062
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X1] = 0x000F00;
    s.registers[reg::A1] = 0x123000;
    pram[0] = 0x200062; // nop + or X1,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123F00, "OR X1,A: A1 | 0x000F00");
}

#[test]
fn test_eor_y0_source() {
    // EOR with Y0 source (JJ=1).
    // EOR Y0,A: alu_byte = 0x43 | (1<<4) | (0<<3) = 0x53 -> parallel: 0x200053
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::Y0] = 0xFFFFFF;
    s.registers[reg::A1] = 0xABCDEF;
    pram[0] = 0x200053; // nop + eor Y0,A
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x543210, "EOR Y0,A: A1 ^ 0xFFFFFF");
}
