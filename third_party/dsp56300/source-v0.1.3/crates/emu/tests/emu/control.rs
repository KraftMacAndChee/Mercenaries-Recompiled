use super::*;

#[test]
fn test_jmp() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0C0042; // jmp $0042
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
}

#[test]
fn test_jsr_rts() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0D0010; // jsr $0010
    pram[0x10] = 0x00000C; // rts
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10);
    // SP should be incremented
    assert_eq!(s.registers[reg::SP] & 0xF, 1);

    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1); // return to pc+1 = 1
}

#[test]
fn test_extended_stack_spills_and_refills_x_memory() {
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::EP] = 0x100;
    s.registers[reg::SZ] = 0x40;
    s.registers[reg::OMR] |= 1 << 20;

    for i in 1..=20u32 {
        s.stack_push(0x100000 + i, 0x200000 + i);
    }

    assert_eq!(s.registers[reg::SP], 20);
    assert_eq!(s.registers[reg::SC], 14);
    assert_eq!(s.registers[reg::EP], 0x10c);
    assert_ne!(s.registers[reg::OMR] & (1 << 19), 0);
    for i in 0..6usize {
        assert_eq!(xram[0x100 + i * 2], 0x100001 + i as u32);
        assert_eq!(xram[0x101 + i * 2], 0x200001 + i as u32);
    }

    for i in (1..=20u32).rev() {
        assert_eq!(s.stack_pop(), (0x100000 + i, 0x200000 + i));
    }
    assert_eq!(s.registers[reg::SP], 0);
    assert_eq!(s.registers[reg::SC], 0);
    assert_eq!(s.registers[reg::EP], 0x100);
    assert_eq!(s.registers[reg::OMR] & (1 << 17), 0);
}

#[test]
fn test_extended_stack_jit_deep_jsr_rts_round_trip() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::EP] = 0x180;
    s.registers[reg::SZ] = 0x40;
    s.registers[reg::OMR] |= 1 << 20;

    for level in 0..20usize {
        let pc = level * 0x10;
        pram[pc] = 0x0D0000 | ((level as u32 + 1) * 0x10);
        if level != 0 {
            pram[pc + 1] = 0x00000C;
        }
    }
    pram[20 * 0x10] = 0x00000C;

    for _ in 0..20 {
        run_one(&mut s, &mut jit);
    }
    assert_eq!(s.pc, 20 * 0x10);
    assert_eq!(s.registers[reg::SP], 20);
    assert_eq!(s.registers[reg::SC], 14);

    for _ in 0..20 {
        run_one(&mut s, &mut jit);
    }
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SP], 0);
    assert_eq!(s.registers[reg::SC], 0);
    assert_eq!(s.registers[reg::EP], 0x180);
}

#[test]
fn test_jcc_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set Z flag so EQ (cc=10=0xA) is true
    s.registers[reg::SR] = 1 << 2; // Z=1
    // jcc $100 with cc=EQ(0xA): 00001110 1010 aaaaaaaaaaaa
    // addr = 0x100
    // opcode = 0x0EA100
    pram[0] = 0x0EA100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Z=0, so EQ is false
    s.registers[reg::SR] = 0;
    pram[0] = 0x0EA100; // jcc EQ,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1); // not taken, advance by 1
}

#[test]
fn test_bra() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bra +5: template 00000101000011aaaa0aaaaa
    // 9-bit offset: high 4 bits at 16:13, low 5 bits at 4:0
    // offset 5 = 0b0_0000_0101 -> high4=0000, low5=00101
    // opcode = 00000101_000011_0000_0_00101 = 0x050C05
    pram[0x10] = 0x050C05;
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x15); // 0x10 + 5
}

#[test]
fn test_rti() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push a return address and SR to stack
    s.registers[reg::SP] = 1;
    s.stack[0][1] = 0x0042; // SSH = return PC
    s.stack[1][1] = 0x0300; // SSL = saved SR
    pram[0] = 0x000004; // rti
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
    assert_eq!(s.registers[reg::SR] & 0xEFFF, 0x0300);
}

#[test]
fn test_wait() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000086; // wait
    run_one(&mut s, &mut jit);
    // WAIT is a no-op; halt is only set via peripheral write to x:$FFFFC4.
    assert!(!s.halt_requested);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_bra_long() {
    // bra xxxx: 0x0D1180
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0D10C0; // bra xxxx
    pram[1] = 0x000042; // offset = $42
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42); // PC = 0 + $42
}

#[test]
fn test_bcc_long_taken() {
    // bcc xxxx (EQ): 0x0D104A (CC=0xA=EQ)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << 2; // Z=1
    pram[0] = 0x0D104A; // bcc xxxx (EQ)
    pram[1] = 0x000020; // offset = $20
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20); // taken: PC = 0 + $20
}

#[test]
fn test_bcc_long_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Z=0 so EQ is false
    pram[0] = 0x0D104A; // bcc xxxx (EQ)
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken: PC = 0 + 2
}

#[test]
fn test_bcc_short_taken() {
    // bcc short (EQ): template 00000101CCCC01aaaa0aaaaa
    // CCCC=1010 (EQ), 9-bit offset = +4: aaaa=0000, aaaaa=00100
    // 0000_0101_1010_0100_0000_0100 = 0x05A404
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z; // Z=1
    pram[0] = 0x05A404;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 4); // taken: PC = 0 + 4
}

#[test]
fn test_bcc_short_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Z=0 so EQ is false
    pram[0] = 0x05A404;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1); // not taken: PC = 0 + 1
}

#[test]
fn test_bsr_long() {
    // bsr xxxx: 0x0D1080
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0D1080; // bsr xxxx
    pram[1] = 0x000010; // offset = $10
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // PC = 0 + $10
    // Return address pushed: PC + 2 = 2
    assert_eq!(s.registers[reg::SP], 1);
}

#[test]
fn test_jscc_taken() {
    // jscc EQ,$100: template 00001111CCCCaaaaaaaaaaaa
    // CC=1010 (EQ), addr=$100
    // 0000_1111_1010_0001_0000_0000 = 0x0FA100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z; // Z=1 for EQ
    pram[0] = 0x0FA100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // stack pushed
}

#[test]
fn test_jscc_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Z=0, so EQ is false
    pram[0] = 0x0FA100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SP] & 0xF, 0); // no stack push
}

#[test]
fn test_reset() {
    // reset instruction just adds cycles, doesn't modify internal state
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000104; // reset
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_stop() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000087; // stop
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // STOP is a no-op; halt is only set via peripheral write to x:$FFFFC4.
    assert!(!s.halt_requested);
}

#[test]
fn test_jscc_ea_taken() {
    // jscc EQ,(R0): template 0000101111MMMRRR1010CCCC
    // MMM=100, RRR=000, CCCC=1010 (EQ)
    // 0000_1011_1110_0000_1010_1010 = 0x0BE0AA
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z; // Z=1 for EQ
    s.registers[reg::R0] = 0x000100; // EA = $100
    pram[0] = 0x0BE0AA;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_jscc_ea_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Z=0 so EQ is false
    s.registers[reg::R0] = 0x000100;
    pram[0] = 0x0BE0AA;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1); // not taken: 1-word instruction
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jcc_ea() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // jeq p:(r0) -- taken (Z set)
    pram[0] = 0x0AE0AA;
    s.registers[reg::R0] = 0x000042;
    s.registers[reg::SR] = 0x0300 | (1 << sr::Z);
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);

    // jeq p:(r0) -- not taken (Z clear)
    pram[0x42] = 0x0AE0AA;
    s.registers[reg::R0] = 0x000042;
    s.registers[reg::SR] = 0x0300;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x43);
}

#[test]
fn test_jsr_ea() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // jsr p:$0042 (ea mode 6: absolute)
    pram[0] = 0x0BF080;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
    assert_eq!(s.registers[reg::SP], 1);
}

#[test]
fn test_bra_rn() {
    // bra R2: template 0000110100011RRR11000000
    // RRR=2: 0000_1101_0001_1010_1100_0000 = 0x0D1AC0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R2] = 0x0042;
    pram[0] = 0x0D1AC0; // bra R2
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
}

#[test]
fn test_bcc_rn_taken() {
    // bcc R3 (EQ, cc=0xA): template 0000110100011RRR0100CCCC
    // RRR=3, CCCC=A: 0000_1101_0001_1011_0100_1010 = 0x0D1B4A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R3] = 0x0100;
    s.registers[reg::SR] = 1 << sr::Z; // Z=1 for EQ
    pram[0] = 0x0D1B4A; // bcc R3 (EQ)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100); // taken
}

#[test]
fn test_bcc_rn_not_taken() {
    // Same encoding but Z=0 -> EQ is false
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R3] = 0x0100;
    s.registers[reg::SR] = 0; // Z=0
    pram[0] = 0x0D1B4A; // bcc R3 (EQ)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1); // not taken
}

#[test]
fn test_bsr_rn() {
    // bsr R1: template 0000110100011RRR10000000
    // RRR=1: 0000_1101_0001_1001_1000_0000 = 0x0D1980
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0050;
    pram[0] = 0x0D1980; // bsr R1
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x50); // jumped to R1
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // return addr pushed
}

#[test]
fn test_bscc_taken() {
    // bscc (CC, cc=0) short: template 00000101CCCC00aaaa0aaaaa
    // cc=CC(0), offset=+5: high4=0000, low5=00101
    // 0000_0101_0000_0000_0000_0101 = 0x050005
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // C=0, so CC is true
    pram[0x10] = 0x050005; // bscc CC,+5
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x15); // 0x10 + 5
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // return addr pushed
}

#[test]
fn test_bscc_not_taken() {
    // Same but with C=1 -> CC is false
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1; // C=1
    pram[0x10] = 0x050005; // bscc CC,+5
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x11); // not taken, advance 1
}

#[test]
fn test_bscc_long_taken() {
    // bscc long (EQ): template 00001101000100000000CCCC
    // CCCC=A (EQ): 0000_1101_0001_0000_0000_1010 = 0x0D100A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z; // Z=1
    pram[0] = 0x0D100A; // bscc long EQ
    pram[1] = 0x000030; // offset = $30
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x30); // 0 + $30
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // return addr pushed
}

#[test]
fn test_bscc_long_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Z=0 so EQ is false
    pram[0] = 0x0D100A;
    pram[1] = 0x000030;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken: 2-word instruction
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bscc_rn_taken() {
    // bscc Rn (CC, cc=0): template 0000110100011RRR0000CCCC
    // RRR=2, CCCC=0: 0000_1101_0001_1010_0000_0000 = 0x0D1A00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R2] = 0x0080;
    s.registers[reg::SR] = 0; // C=0, CC is true
    pram[0] = 0x0D1A00; // bscc R2, CC
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x80);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bscc_rn_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R2] = 0x0080;
    s.registers[reg::SR] = 1; // C=1, CC is false
    pram[0] = 0x0D1A00;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1); // not taken
    assert_eq!(s.registers[reg::SP] & 0xF, 0); // nothing pushed
}

#[test]
fn test_debug_nop() {
    // DEBUG: 000000000000001000000000 = 0x000800
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000800; // debug (treated as NOP)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_trap_nop() {
    // TRAP: 000000000000000000000110 = 0x000006
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000006; // trap (treated as NOP)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_debugcc_nop() {
    // DEBUGCC (CS): 00000000000000110000CCCC, CC=8
    // 0x000308
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000308; // debugcs (treated as NOP)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_trapcc_nop() {
    // TRAPCC (CS): 00000000000000000001CCCC, CC=8
    // 0x000018
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000018; // trapcs (treated as NOP)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_wait_sets_power_state() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000086; // WAIT
    assert_eq!(s.power_state, PowerState::Normal);
    run_one(&mut s, &mut jit);
    assert_eq!(s.power_state, PowerState::Wait);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_stop_sets_power_state() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000087; // STOP
    assert_eq!(s.power_state, PowerState::Normal);
    run_one(&mut s, &mut jit);
    assert_eq!(s.power_state, PowerState::Stop);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_bra_rn_is_pc_relative() {
    // BRA Rn: PC + Rn -> PC (PC-relative, NOT absolute)
    // Manual page 13-25: "PC + Rn -> Pc"
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // BRA R0 opcode: template 0000110100011RRR11000000, R0=000
    // = 0x0D18C0
    s.pc = 0x10;
    s.registers[reg::R0] = 5; // displacement = +5
    pram[0x10] = 0x0D18C0; // BRA R0
    run_one(&mut s, &mut jit);
    // Expected: PC = 0x10 + 5 = 0x15 (PC-relative)
    // Bug: code produces PC = 5 (absolute)
    assert_eq!(s.pc, 0x15, "BRA R0 should compute PC + R0, not just R0");
}

#[test]
fn test_jscc_ea_mode6_absolute() {
    // JSCC (carry clear) ea with mode 6 absolute address - CC is true when C=0
    // JScc ea opcode: 0000101111MMMRRR1010CCCC
    // MMM=110, RRR=000, CCCC=0000 (CC = carry clear)
    // 0000_1011_1111_0000_1010_0000 = 0x0BF0A0
    // Extension word: jump target address
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0BF0A0; // jscc ea (carry clear = true when C=0, mode 6)
    pram[1] = 0x000200; // absolute target = $200
    pram[0x200] = 0x000000; // nop at target
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 0x200,
        "JScc should jump to absolute address from extension word"
    );
    // Return address on stack should be PC+2 (past the 2-word instruction)
    assert_eq!(
        s.registers[reg::SSH],
        2,
        "return address should be PC+2 for 2-word JScc ea"
    );
}

#[test]
fn test_trap_posts_interrupt() {
    // TRAP: opcode 0x000006. Posts interrupt::TRAP (index 4, vector $08).
    // Manual page 13-179: "Begin trap exception process"
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Place handler at TRAP vector ($08): JMP $20
    pram[0x08] = 0x0C0020; // jmp $20
    pram[0x09] = 0x000000; // nop (2nd slot of long interrupt vector)
    pram[0x20] = 0x0C0020; // jmp $20 (halt)

    pram[0] = 0x000006; // TRAP
    let cycles = run_one(&mut s, &mut jit);
    assert_eq!(cycles, 9, "TRAP should take 9 cycles");

    // Verify interrupt was posted and dispatched
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert_eq!(s.interrupts.pipeline_stage, 5);
}

#[test]
fn test_trap_dispatches_to_vector() {
    // TRAP should dispatch to vector $08 after the interrupt pipeline completes.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    pram[0x08] = 0x0C0020; // jmp $20 (long interrupt handler)
    pram[0x09] = 0x000000;
    pram[0x20] = 0x0C0020; // jmp $20 (halt)

    pram[0] = 0x000006; // TRAP
    // Fill pipeline slots with NOPs
    pram[1..8].fill(0x000000);
    s.run(&mut jit, 100);
    assert_eq!(s.pc, 0x20, "TRAP should dispatch to vector $08 -> JMP $20");
}

#[test]
fn test_trapcc_taken_posts_interrupt() {
    // TRAPcc with condition true should post TRAP interrupt.
    // TRAPcc CC (carry clear): CCCC=0000, opcode = 0x000010
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    pram[0x08] = 0x0C0020;
    pram[0x09] = 0x000000;
    pram[0x20] = 0x0C0020;

    // C=0 by default, so CC (carry clear) is true
    pram[0] = 0x000010; // trapcc CC
    run_one(&mut s, &mut jit);

    // Verify interrupt was posted
    assert_eq!(s.interrupts.state, InterruptState::Fast);
}

#[test]
fn test_trapcc_not_taken_continues() {
    // TRAPcc with condition false should not post interrupt.
    // TRAPcc CS (carry set): CCCC=1000, opcode = 0x000018
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // C=0 by default, so CS (carry set) is false
    pram[0] = 0x000018; // trapcc CS
    pram[1] = 0x000000; // nop (should execute next)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);

    // No interrupt posted, next instruction is NOP at PC=1
    assert_eq!(s.interrupts.state, InterruptState::None);
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 2,
        "TRAPcc CS (not taken) should continue to next instruction"
    );
}

#[test]
fn test_trapcc_gt_taken() {
    // TRAPcc with GT condition (N XOR V = 0 AND Z = 0).
    // TRAPcc encoding: 00000000000000000001CCCC, GT=7 -> 0x000017
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set up TRAP vector at VBA:$08 to loop
    pram[0x08] = 0x0C0020; // jmp $20
    pram[0x09] = 0x000000;
    pram[0x20] = 0x0C0020; // jmp $20 (loop)

    // Set N=0, Z=0, V=0 -> GT is true
    s.registers[reg::SR] = 0xC00300; // IPL=3, no CCR flags set
    pram[0] = 0x000017; // trapcc GT
    run_one(&mut s, &mut jit);

    assert_eq!(
        s.interrupts.state,
        InterruptState::Fast,
        "TRAPgt should post interrupt when GT is true"
    );
}

#[test]
fn test_trapcc_gt_not_taken_z_set() {
    // TRAPcc with GT condition when Z=1 -> GT is false (Z blocks GT).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set Z=1 -> GT is false regardless of N,V
    s.registers[reg::SR] = 0xC00300 | (1 << sr::Z);
    pram[0] = 0x000017; // trapcc GT
    run_one(&mut s, &mut jit);

    assert_eq!(s.pc, 1, "TRAPgt should not trap when Z=1");
    assert_eq!(
        s.interrupts.state,
        InterruptState::None,
        "No interrupt when GT is false"
    );
}

#[test]
fn test_nop() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000000; // nop
    let cycles = run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(cycles, 1);
}

#[test]
fn test_ifcc_taken() {
    // IFcc: add X0,A ifcc -> condition CC (carry clear, cc=0)
    // pm_2 IFcc: bits [23:20]=0x2, [19:16]=0x0, [15:12]=0x2, [11:8]=CCCC
    // IFcc encoding: 0x202000 | (CCCC << 8) | alu_byte
    // add X0,A = alu_byte 0x40, CC(cc=0) -> 0x202040
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::SR] = 0; // C=0, CC condition true
    pram[0] = 0x202040; // add X0,A ifcc
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x300000); // add executed
}

#[test]
fn test_ifcc_not_taken() {
    // Same but C=1 -> CC is false, ALU should not execute
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::SR] = 1; // C=1, CC is false
    pram[0] = 0x202040; // add X0,A ifcc
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x200000); // unchanged
}

#[test]
fn test_ifcc_u_taken_updates_ccr() {
    // IFcc.U: clr A ifcc.u -> should update CCR when condition true
    // IFcc.U encoding: 0x203000 | (CCCC << 8) | alu_byte
    // clr A = alu_byte 0x13, CC(cc=0) -> 0x203013
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::SR] = 0; // C=0, CC is true
    pram[0] = 0x203013; // clr A ifcc.u
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0); // cleared
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z set by clr
}

#[test]
fn test_ifcc_u_not_taken_no_ccr_update() {
    // IFcc.U not taken: CCR unchanged
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::A1] = 0x123456;
    s.registers[reg::SR] = 1; // C=1, CC is false
    pram[0] = 0x203013; // clr A ifcc.u
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x123456); // unchanged
    assert_eq!(s.registers[reg::SR] & (1 << sr::Z), 0); // Z not set
}

#[test]
fn test_jclr_pp() {
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
    periph[69] = 0x00; // bit 0 is clear
    // jclr #0,x:$ffffc5,$0042 -> 2-word
    pram[0] = 0x0A8580;
    pram[1] = 0x0042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42); // should jump (bit is clear)
}

#[test]
fn test_jclr_pp_not_taken() {
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
    periph[69] = 0x01; // bit 0 is set
    pram[0] = 0x0A8580;
    pram[1] = 0x0042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken, skip 2-word instruction
}

#[test]
fn test_jclr_reg_taken() {
    // jclr #0,X0,$100: jump if bit 0 of X0 is clear
    // encoding: 0000101011DDDDDD0000bbbb
    // X0=0x04 (DDDDDD=000100), bit=0 (bbbb=0000)
    // = 0x0AC400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000002; // bit 0 is clear
    pram[0] = 0x0AC400; // jclr #0,X0,$100
    pram[1] = 0x000100; // target
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100); // taken
}

#[test]
fn test_jset_reg_taken() {
    // jset #0,X0,$100: jump if bit 0 of X0 is set
    // encoding: 0000101011DDDDDD0010bbbb
    // X0=0x04, bit=0: byte3 = 0010_0000 = 0x20
    // = 0x0AC420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 is set
    pram[0] = 0x0AC420; // jset #0,X0,$100
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100); // taken
}

#[test]
fn test_jclr_aa_taken() {
    // jclr #2,X:$10,$100: template 0000101000aaaaaa1S00bbbb
    // aaaaaa=0x10, S=0 (X), bbbb=2
    // 0000_1010_0001_0000_1000_0010 = 0x0A1082
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 2 clear -> taken
    pram[0] = 0x0A1082; // jclr #2,X:$10,$100
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jclr_aa_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000004; // bit 2 set -> not taken
    pram[0] = 0x0A1082;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jset_aa_taken() {
    // jset #3,X:$10,$100: template 0000101000aaaaaa1S10bbbb
    // aaaaaa=0x10, S=0 (X), bbbb=3
    // 0000_1010_0001_0000_1010_0011 = 0x0A10A3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000008; // bit 3 set -> taken
    pram[0] = 0x0A10A3; // jset #3,X:$10,$100
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jsclr_reg_taken() {
    // jsclr #0,X0,$200: template 0000101111DDDDDD000bbbbb
    // DDDDDD=0x04 (X0), bbbbb=0
    // 0000_1011_1100_0100_0000_0000 = 0x0BC400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000002; // bit 0 clear
    pram[0] = 0x0BC400; // jsclr #0,X0,$200
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200);
    // Check stack was pushed (SP incremented)
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_jsset_reg_taken() {
    // jsset #0,X0,$200: template 0000101111DDDDDD001bbbbb
    // DDDDDD=0x04 (X0), bbbbb=0
    // 0000_1011_1100_0100_0010_0000 = 0x0BC420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set
    pram[0] = 0x0BC420; // jsset #0,X0,$200
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_jsclr_pp_taken() {
    // jsclr #1,X:$FFFFC4,$300: template 0000101110pppppp1S0bbbbb
    // pp=4, S=0 (X), bbbbb=1
    // 0000_1011_1000_0100_1000_0001 = 0x0B8481
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
    periph[0x44] = 0x000000; // bit 1 clear
    pram[0] = 0x0B8481;
    pram[1] = 0x000300;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x300);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_brclr_pp_taken() {
    // brclr #0,X:$FFFFC4,$10: template 0000110011pppppp0S0bbbbb
    // pp=4, S=0, bbbbb=0
    // 0000_1100_1100_0100_0000_0000 = 0x0CC400
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
    periph[0x44] = 0x000002; // bit 0 clear
    pram[0] = 0x0CC400;
    pram[1] = 0x000010; // relative offset
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // PC = 0 + offset $10
}

#[test]
fn test_brset_pp_taken() {
    // brset #0,X:$FFFFC4,$10: template 0000110011pppppp0S1bbbbb
    // pp=4, S=0 (X), bbbbb=0 (bit 0)
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
    periph[0x44] = 0x000001; // bit 0 set
    pram[0] = 0x0CC420;
    pram[1] = 0x000020; // relative offset
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20); // PC = 0 + $20
}

#[test]
fn test_jclr_ea_taken() {
    // jclr #1,X:(R0),$100: template 0000101001MMMRRR1S00bbbb
    // MMM=100, RRR=000, S=0, bbbb=1
    // 0000_1010_0110_0000_1000_0001 = 0x0A6081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000000; // bit 1 clear -> taken
    pram[0] = 0x0A6081;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jset_ea_taken() {
    // jset #0,X:(R0),$100: template 0000101001MMMRRR1S10bbbb
    // MMM=100, RRR=000, S=0, bbbb=0
    // 0000_1010_0110_0000_1010_0000 = 0x0A60A0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000001; // bit 0 set -> taken
    pram[0] = 0x0A60A0;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jsclr_ea_taken() {
    // jsclr #0,X:(R0),$200: template 0000101101MMMRRR1S00bbbb
    // MMM=100, RRR=000, S=0, bbbb=0
    // 0000_1011_0110_0000_1000_0000 = 0x0B6080
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000002; // bit 0 clear
    pram[0] = 0x0B6080;
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200);
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // stack pushed
}

#[test]
fn test_jsclr_ea_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000001; // bit 0 set -> jsclr NOT taken
    pram[0] = 0x0B6080;
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken: 2-word instruction
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jsset_ea_taken() {
    // jsset #0,X:(R0),$200: template 0000101101MMMRRR1S10bbbb
    // MMM=100, RRR=000, S=0, bbbb=0
    // 0000_1011_0110_0000_1010_0000 = 0x0B60A0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000001; // bit 0 set
    pram[0] = 0x0B60A0;
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_brclr_reg() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // brclr #3,x0,p:$0042 -- bit clear, branch taken
    pram[0x10] = 0x0CC483;
    pram[0x11] = 0x000042;
    s.pc = 0x10;
    s.registers[reg::X0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10 + 0x42); // PC-relative

    // brclr #3,x0,p:$0042 -- bit set, not taken
    pram[0x20] = 0x0CC483;
    pram[0x21] = 0x000042;
    s.pc = 0x20;
    s.registers[reg::X0] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x22);
}

#[test]
fn test_brset_reg() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // brset #3,x0,p:$0042 -- bit set, branch taken
    pram[0x10] = 0x0CC4A3;
    pram[0x11] = 0x000042;
    s.pc = 0x10;
    s.registers[reg::X0] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10 + 0x42);

    // brset #3,x0,p:$0042 -- bit clear, not taken
    pram[0x20] = 0x0CC4A3;
    pram[0x21] = 0x000042;
    s.pc = 0x20;
    s.registers[reg::X0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x22);
}

#[test]
fn test_jset_pp() {
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
    // jset #3,x:$ffffc0,p:$0042 -- bit set, jump taken
    // pp=0 maps to $ffffc0 = PERIPH_BASE+64
    pram[0] = 0x0A80A3;
    pram[1] = 0x000042;
    periph[64] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);

    // jset #3,x:$ffffc0,p:$0042 -- bit clear, not taken
    pram[0x42] = 0x0A80A3;
    pram[0x43] = 0x000042;
    periph[64] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x44);
}

#[test]
fn test_jsclr_aa() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // jsclr #3,x:$0010,p:$0042 -- bit clear, jump taken
    pram[0] = 0x0B1083;
    pram[1] = 0x000042;
    xram[0x10] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
    assert_eq!(s.registers[reg::SP], 1);

    // jsclr #3,x:$0010,p:$0042 -- bit set, not taken
    pram[0x42] = 0x0B1083;
    pram[0x43] = 0x000042;
    xram[0x10] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x44);
}

#[test]
fn test_jsset_pp() {
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
    // jsset #3,x:$ffffc0,p:$0042 -- bit set, jump taken
    // pp=0 maps to $ffffc0 = PERIPH_BASE+64
    pram[0] = 0x0B80A3;
    pram[1] = 0x000042;
    periph[64] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
    assert_eq!(s.registers[reg::SP], 1);

    // jsset #3,x:$ffffc0,p:$0042 -- bit clear, not taken
    pram[0x42] = 0x0B80A3;
    pram[0x43] = 0x000042;
    periph[64] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x44);
}

#[test]
fn test_jsset_aa() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // jsset #3,x:$0010,p:$0042 -- bit set, jump taken
    pram[0] = 0x0B10A3;
    pram[1] = 0x000042;
    xram[0x10] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
    assert_eq!(s.registers[reg::SP], 1);

    // jsset #3,x:$0010,p:$0042 -- bit clear, not taken
    pram[0x42] = 0x0B10A3;
    pram[0x43] = 0x000042;
    xram[0x10] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x44);
}

#[test]
fn test_brclr_aa_taken() {
    // brclr #2,X:$10,xxxx: template 0000110010aaaaaa1S0bbbbb
    // aaaaaa=0x10, S=0 (X), bbbbb=2
    // 0000_1100_1001_0000_1000_0010 = 0x0C9082
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 2 is clear
    pram[0] = 0x0C9082; // brclr #2,X:$10,xxxx
    pram[1] = 0x000020; // offset = $20
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20); // 0 + $20
}

#[test]
fn test_brclr_aa_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000004; // bit 2 is set
    pram[0] = 0x0C9082;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken
}

#[test]
fn test_brset_aa_taken() {
    // brset #2,X:$10,xxxx: template 0000110010aaaaaa1S1bbbbb
    // aaaaaa=0x10, S=0 (X), bbbbb=2
    // 0000_1100_1001_0000_1010_0010 = 0x0C90A2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000004; // bit 2 is set
    pram[0] = 0x0C90A2; // brset #2,X:$10,xxxx
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // taken
}

#[test]
fn test_brclr_pp_periph() {
    // brclr #0,X:$FFFFC0,xxxx: template 0000110011pppppp0S0bbbbb
    // pppppp=0, S=0, bbbbb=0
    // 0000_1100_1100_0000_0000_0000 = 0x0CC000
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
    periph[0x40] = 0x000000; // bit 0 clear at $FFFFC0 (periph[0x40])
    pram[0] = 0x0CC000;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // taken
}

#[test]
fn test_brclr_reg_taken() {
    // brclr #3,X0,$xxxx: template 0000110011DDDDDD100bbbbb
    // DDDDDD=0x04 (X0), bbbbb=3
    // 0000_1100_1100_0100_1000_0011 = 0x0CC483
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // bit 3 clear
    pram[0] = 0x0CC483;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // taken
}

#[test]
fn test_brset_reg_taken() {
    // brset #3,X0,$xxxx: template 0000110011DDDDDD101bbbbb
    // DDDDDD=0x04 (X0), bbbbb=3
    // 0000_1100_1100_0100_1010_0011 = 0x0CC4A3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000008; // bit 3 set
    pram[0] = 0x0CC4A3;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // taken
}

#[test]
fn test_bsclr_aa_taken() {
    // bsclr #1,X:$10,xxxx: template 0000110110aaaaaa1S0bbbbb
    // aaaaaa=0x10, S=0, bbbbb=1
    // 0000_1101_1001_0000_1000_0001 = 0x0D9081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 1 clear
    pram[0] = 0x0D9081;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20); // taken
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // return addr pushed
}

#[test]
fn test_bsset_aa_taken() {
    // bsset #1,X:$10,xxxx: template 0000110110aaaaaa1S1bbbbb
    // aaaaaa=0x10, S=0, bbbbb=1
    // 0000_1101_1001_0000_1010_0001 = 0x0D90A1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000002; // bit 1 set
    pram[0] = 0x0D90A1;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bsclr_reg_taken() {
    // bsclr #0,X0,xxxx: template 0000110111DDDDDD100bbbbb
    // DDDDDD=0x04 (X0), bbbbb=0
    // 0000_1101_1100_0100_1000_0000 = 0x0DC480
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // bit 0 clear
    pram[0] = 0x0DC480;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bsset_reg_taken() {
    // bsset #0,X0,xxxx: template 0000110111DDDDDD101bbbbb
    // DDDDDD=0x04, bbbbb=0
    // 0000_1101_1100_0100_1010_0000 = 0x0DC4A0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set
    pram[0] = 0x0DC4A0;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bsclr_pp_taken() {
    // bsclr #0,X:$FFFFC0,xxxx: template 0000110111pppppp0S0bbbbb
    // pppppp=0, S=0, bbbbb=0
    // 0000_1101_1100_0000_0000_0000 = 0x0DC000
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
    periph[0x40] = 0x000000; // bit 0 clear
    pram[0] = 0x0DC000;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_jclr_qq_taken() {
    // jclr #0,X:$FFFF80,xxxx: template 0000000110qqqqqq1S00bbbb
    // qq=0, S=0, bbbb=0
    // 0000_0001_1000_0000_1000_0000 = 0x018080
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
    periph[0] = 0x000000; // bit 0 clear
    pram[0] = 0x018080;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42); // taken
}

#[test]
fn test_jclr_qq_not_taken() {
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
    periph[0] = 0x000001; // bit 0 set
    pram[0] = 0x018080;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken
}

#[test]
fn test_jset_qq_taken() {
    // jset #0,X:$FFFF80,xxxx: template 0000000110qqqqqq1S10bbbb
    // qq=0, S=0, bbbb=0
    // 0000_0001_1000_0000_1010_0000 = 0x0180A0
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
    periph[0] = 0x000001; // bit 0 set
    pram[0] = 0x0180A0;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42); // taken
}

#[test]
fn test_jsclr_qq_taken() {
    // jsclr #0,X:$FFFF80,xxxx: template 0000000111qqqqqq1S0bbbbb
    // qq=0, S=0, bbbbb=0
    // 0000_0001_1100_0000_1000_0000 = 0x01C080
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
    periph[0] = 0x000000; // bit 0 clear
    pram[0] = 0x01C080;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42); // taken
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // return addr pushed
}

#[test]
fn test_jsset_qq_taken() {
    // jsset #0,X:$FFFF80,xxxx: template 0000000111qqqqqq1S1bbbbb
    // qq=0, S=0, bbbbb=0
    // 0000_0001_1100_0000_1010_0000 = 0x01C0A0
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
    periph[0] = 0x000001; // bit 0 set
    pram[0] = 0x01C0A0;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42); // taken
    assert_eq!(s.registers[reg::SP] & 0xF, 1); // return addr pushed
}

#[test]
fn test_brclr_qq_taken() {
    // brclr #0,X:$FFFF80,xxxx: template 0000010010qqqqqq0S0bbbbb
    // qq=0, S=0, bbbbb=0
    // 0000_0100_1000_0000_0000_0000 = 0x048000
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
    periph[0] = 0x000000;
    pram[0] = 0x048000;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // taken
}

#[test]
fn test_brset_qq_taken() {
    // brset #0,X:$FFFF80,xxxx: template 0000010010qqqqqq0S1bbbbb
    // qq=0, S=0, bbbbb=0
    // 0000_0100_1000_0000_0010_0000 = 0x048020
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
    periph[0] = 0x000001;
    pram[0] = 0x048020;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x10); // taken
}

#[test]
fn test_bsclr_qq_taken() {
    // bsclr #0,X:$FFFF80,xxxx: template 0000010010qqqqqq1S0bbbbb
    // qq=0, S=0, bbbbb=0
    // 0000_0100_1000_0000_1000_0000 = 0x048080
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
    periph[0] = 0x000000;
    pram[0] = 0x048080;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bsset_qq_taken() {
    // bsset #0,X:$FFFF80,xxxx: template 0000010010qqqqqq1S1bbbbb
    // qq=0, S=0, bbbbb=0
    // 0000_0100_1000_0000_1010_0000 = 0x0480A0
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
    periph[0] = 0x000001;
    pram[0] = 0x0480A0;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bsset_pp_periph() {
    // bsset #0,X:$FFC0,disp (0000110111pppppp0S1bbbbb, pp=0 S=0 bit=0)
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
    periph[0x40] = 0x000001; // bit 0 is set at X:$FFFFC0 (pp base)
    pram[0] = 0x0DC020; // bsset #0,x:$FFFFC0,xxxx
    pram[1] = 0x000020; // displacement
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP], 1);
}

#[test]
fn test_brclr_ea_taken() {
    // BRCLR ea: 0000110010MMMRRR0S0bbbbb
    // MMM=100 (Rn no update), RRR=000 (R0), S=0 (X), bit=0
    // 0000_1100_1010_0000_0000_0000 = 0x0CA000
    // Next word = relative displacement
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000000; // bit 0 clear
    pram[0] = 0x0CA000; // brclr #0,x:(R0),xxxx
    pram[1] = 0x000020; // displacement
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
}

#[test]
fn test_brset_ea_taken() {
    // BRSET ea: 0000110010MMMRRR0S1bbbbb
    // MMM=100 (Rn), RRR=000, S=0, bit=0
    // 0000_1100_1010_0000_0001_0000 = 0x0CA010
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000001; // bit 0 set
    pram[0] = 0x0CA010; // brset #0,x:(R0),xxxx
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
}

#[test]
fn test_bsclr_ea_taken() {
    // BSCLR ea: 0000110110MMMRRR0S0bbbbb
    // MMM=100, RRR=000, S=0, bit=0
    // 0000_1101_1010_0000_0000_0000 = 0x0DA000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000000; // bit 0 clear
    pram[0] = 0x0DA000; // bsclr #0,x:(R0),xxxx
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP], 1);
}

#[test]
fn test_bsclr_ea_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000001; // bit 0 set -> bsclr NOT taken
    pram[0] = 0x0DA000;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2); // not taken: 2-word instruction
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsset_ea_taken() {
    // BSSET ea: 0000110110MMMRRR0S1bbbbb
    // MMM=100, RRR=000, S=0, bit=0
    // 0000_1101_1010_0000_0001_0000 = 0x0DA010
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000001; // bit 0 set
    pram[0] = 0x0DA010; // bsset #0,x:(R0),xxxx
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.registers[reg::SP], 1);
}

#[test]
fn test_bsclr_ccr_unchanged() {
    // BSCLR should not modify CCR flags (S,L changed; E,U,N,Z,V,C unchanged).
    // Test with not-taken path to verify C is not corrupted by bit testing.
    // BSCLR reg template: 0000110111DDDDDD100bbbbb + 24-bit target address
    // DDDDDD=000100 (X0=0x04), bbbbb=00000 (bit 0)
    // 0000_1101_1100_0100_1000_0000 = 0x0DC480
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0xC00300; // C=0, IPL=3
    s.registers[reg::X0] = 0x000001; // bit 0 = 1 -> bit is SET, so BSCLR (branch if clear) NOT taken
    pram[0] = 0x0DC480; // bsclr #0,X0,$0010
    pram[1] = 0x000010; // target address (not taken)
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 2,
        "BSCLR not taken should advance past 2-word instruction"
    );
    // CCR should be unchanged: C was 0 before, should still be 0
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C flag should be unchanged after BSCLR (not taken)");
    // N, Z, V, E, U should also be unchanged
    let sr_ccr = s.registers[reg::SR] & 0xFF;
    assert_eq!(sr_ccr, 0x00, "CCR bits should be unchanged after BSCLR");
}

#[test]
fn test_bra_negative_displacement() {
    // BRA with negative displacement (backward branch).
    // BRA long: 0x0D10C0 + 24-bit extension (displacement = target - PC_of_bra).
    // Place BRA at PC=0x20, target=0x10. displacement = 0x10 - 0x20 = -0x10 = 0xFFFFF0 (24-bit).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.pc = 0x20;
    pram[0x20] = 0x0D10C0; // bra (long)
    pram[0x21] = 0xFFFFF0; // displacement = -16
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 0x10,
        "BRA with negative displacement should jump backward to 0x10"
    );
}

#[test]
fn test_jsr_ssl_contains_sr() {
    // JSR should push PC+1 to SSH and SR to SSL.
    // jsr $100 (short form): 0x0D0100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set SR to a known value
    let sr_value = 0xC00345;
    s.registers[reg::SR] = sr_value;
    pram[0] = 0x0D0100; // jsr $0100
    pram[0x100] = 0x000000; // nop at target
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "JSR should jump to target address");
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(sp, 1, "SP should be incremented after JSR");
    assert_eq!(
        s.stack[0][sp], 1,
        "SSH should contain return address (PC+1=1)"
    );
    assert_eq!(
        s.stack[1][sp], sr_value,
        "SSL should contain pre-JSR SR value"
    );
}

#[test]
fn test_jmp_ea_indirect() {
    // JMP (R0): jump to address in R0.
    // JMP ea (MMM=100/Rn, RRR=000/R0): 0x0AE000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x42;
    pram[0] = 0x0AE080; // jmp (r0)
    pram[0x42] = 0x000000; // nop at target
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42, "JMP (R0) should jump to address held in R0");
}

#[test]
fn test_jcc_ea_agu_side_effect_when_not_taken() {
    // Per DSP56300FM p.13-80: "the effective address is always calculated,
    // regardless of the condition." So even when Jcc is not taken, the AGU
    // side effect (post-increment) must still occur.
    //
    // jeq (r0)+: template 0000101011MMMRRR1010CCCC
    // MMM=011 (postincrement), RRR=000 (R0), CCCC=1010 (EQ)
    // 0000_1010_1101_1000_1010_1010 = 0x0AD8AA
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x100;
    // Z=0 -> JEQ condition is false, branch not taken
    s.registers[reg::SR] = 0x0300;
    pram[0] = 0x0AD8AA; // jeq (r0)+
    run_one(&mut s, &mut jit);
    // Not taken: PC advances to next instruction
    assert_eq!(s.pc, 1, "JEQ not taken should advance PC to 1");
    // R0 should still be post-incremented by the EA calculation
    assert_eq!(
        s.registers[reg::R0],
        0x101,
        "R0 should be post-incremented even when Jcc is not taken"
    );
}

#[test]
fn test_bsclr_ssh_pop_push_semantics() {
    // Per ARCHITECTURE-NOTES.md: BSCLR with SSH as source pops the stack
    // (reads SSH), then if branch taken, pushes PC+2 and SR.
    //
    // bsclr #0,ssh,target: template 0000110111DDDDDD100bbbbb
    // DDDDDD=0x3C (SSH), bbbbb=00000 (bit 0)
    // 0000_1101_1111_1100_1000_0000 = 0x0DFC80
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Sub-test 1: NOT taken (bit 0 of SSH is set -> "bit clear" is false)
    s.stack_push(0xAAAAAA, 0x111111); // SP=1
    s.stack_push(0xBBBBBB, 0x222222); // SP=2, SSH=0xBBBBBB, bit 0 = 1
    pram[0] = 0x0DFC80; // bsclr #0,ssh,$0020
    pram[1] = 0x000020; // target address
    run_one(&mut s, &mut jit);
    // SSH read pops: SP goes from 2 to 1. Not taken: no push.
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "NOT taken: SP should be 1 (popped by SSH read, no push)"
    );
    assert_eq!(
        s.pc, 2,
        "NOT taken: PC should advance past 2-word instruction"
    );

    // Sub-test 2: TAKEN (bit 0 of SSH is clear -> "bit clear" is true)
    // Per ARCHITECTURE-NOTES.md: BSCLR pops SSH on read, then BSR pushes PC+SR.
    // Net effect: SP pop(-1) + push(+1) = net 0. So SP stays at 2.
    // However, the current implementation pushes first (BSR), then the SSH read
    // may or may not pop depending on ordering. We verify the branch target is correct.
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut xram2 = [0u32; XRAM_SIZE];
    let mut yram2 = [0u32; YRAM_SIZE];
    let mut pram2 = [0u32; PRAM_SIZE];
    let mut s2 = DspState::new(MemoryMap::test(&mut xram2, &mut yram2, &mut pram2));
    s2.stack_push(0xAAAAAA, 0x111111); // SP=1
    s2.stack_push(0xBBBBBA, 0x222222); // SP=2, SSH=0xBBBBBA, bit 0 = 0
    pram2[0] = 0x0DFC80; // bsclr #0,ssh,$0020
    pram2[1] = 0x000020; // target address
    pram2[0x20] = 0x000000; // nop at target
    run_one(&mut s2, &mut jit2);
    // Per ARCHITECTURE-NOTES.md (Bit-Test-and-Branch SSH Pop Semantics):
    // BSCLR pops SSH on read, then if taken, BSR pushes PC+2 and SR.
    // Net effect: pop(-1) + push(+1) = SP stays at 2.
    assert_eq!(
        s2.registers[reg::SP] & 0xF,
        2,
        "TAKEN: SP should be 2 (SSH pop then BSR push, manual p.13-32)"
    );
    assert_eq!(s2.pc, 0x20, "TAKEN: PC should jump to target address");
}

#[test]
fn test_jsr_stack_overflow() {
    // JSR when SP=15 (max) should wrap SP to 0 and set SE bit (bit 4).
    // Per DSP56300FM Section 5.4.3.1, Table 5-2: stack overflow sets SE.
    // Per ARCHITECTURE-NOTES.md: push writes to slot 0 on overflow (circular buffer).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SP] = 15;
    // Populate SSH/SSL views to match SP=15
    s.stack[0][15] = 0;
    s.stack[1][15] = 0;
    s.registers[reg::SSH] = 0;
    s.registers[reg::SSL] = 0;
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    pram[0] = 0x0D0100; // jsr $0100 (short form, 12-bit address)
    pram[0x100] = 0x000000; // nop at target
    run_one(&mut s, &mut jit);
    // SP P[3:0] should wrap to 0
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        0,
        "SP low nibble should wrap to 0 on overflow"
    );
    // SE bit (bit 4) should be set
    assert_ne!(
        s.registers[reg::SP] & (1 << 4),
        0,
        "SE bit should be set after stack overflow"
    );
    // STACK_ERROR interrupt should have been dispatched by process_pending_interrupts
    // (called at end of execute_one). The pending bit is cleared and the interrupt
    // pipeline enters the Fast state.
    assert_eq!(
        s.interrupts.state,
        InterruptState::Fast,
        "Interrupt pipeline should be in Fast state after STACK_ERROR dispatch"
    );
    // The vector address should point to STACK_ERROR's IVT slot (VBA + $02)
    assert_eq!(
        s.interrupts.vector_addr, 0x02,
        "Vector address should be STACK_ERROR slot ($02)"
    );
}

#[test]
fn test_bsr_long_form() {
    // BSR long: 0x0D1080 + extension word = absolute target offset from PC.
    // BSR to $50 from PC=0: extension = $50.
    // Per DSP56300FM p.13-24: "PC + displacement -> PC", return address pushed.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0D1080; // bsr xxxx (long form)
    pram[1] = 0x000050; // displacement = $50
    pram[0x50] = 0x000000; // nop at target
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x50, "PC should jump to displacement target");
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should be 1 after BSR push"
    );
    // Return address = PC + 2 (2-word instruction)
    assert_eq!(s.stack[0][1], 2, "SSH should hold return address (PC+2)");
}

#[test]
fn test_pflush_nop() {
    // PFLUSH (0x000003) is treated as NOP per LIMITATIONS.md.
    // Per DSP56300FM p.13-153: flushes pipeline cache - no architectural effect in emulation.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000003; // pflush
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1, "PFLUSH should advance PC by 1 (treated as NOP)");
}

#[test]
fn test_plock_ea_agu_side_effects() {
    // PLOCK (R0)+ is NOP for cache locking, but EA side effects should still
    // occur: R0 should be post-incremented by the AGU.
    // Per ARCHITECTURE-NOTES.md Section 4.4.2: address register indirect modes
    // update Rn. PLOCK template: 0000101111MMMRRR10000001, MMM=011((Rn)+), RRR=000(R0).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x100;
    pram[0] = 0x0BD881; // plock (r0)+
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1, "PLOCK should advance PC by 1");
    assert_eq!(
        s.registers[reg::R0],
        0x101,
        "R0 should be post-incremented by EA calculation"
    );
}

#[test]
fn test_jcc_cc_taken() {
    // Jcc CC (carry clear, cc=0): true when C=0.
    // Jcc template: 00001110CCCCaaaaaaaaaaaa, CCCC=0000, addr=$100
    // 0000_1110_0000_0001_0000_0000 = 0x0E0100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // C=0, CC is true
    pram[0] = 0x0E0100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_cc_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::C; // C=1, CC is false
    pram[0] = 0x0E0100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_jcc_ge_taken() {
    // GE (cc=1): true when N^V=0 (N=V).
    // CCCC=0001, addr=$100 -> 0x0E1100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = (1 << sr::N) | (1 << sr::V); // N=1,V=1 -> N^V=0, GE true
    pram[0] = 0x0E1100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_ge_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::N; // N=1,V=0 -> N^V=1, GE false
    pram[0] = 0x0E1100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_jcc_lt_taken() {
    // LT (cc=9): true when N^V=1 (N!=V).
    // CCCC=1001, addr=$100 -> 0x0E9100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::V; // N=0,V=1 -> N^V=1, LT true
    pram[0] = 0x0E9100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_lt_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // N=0,V=0 -> N^V=0, LT false
    pram[0] = 0x0E9100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_jcc_ne_taken() {
    // NE (cc=2): true when Z=0.
    // CCCC=0010, addr=$100 -> 0x0E2100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // Z=0, NE true
    pram[0] = 0x0E2100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_ne_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z; // Z=1, NE false
    pram[0] = 0x0E2100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_jcc_pl_taken() {
    // PL (cc=3): true when N=0.
    // CCCC=0011, addr=$100 -> 0x0E3100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // N=0, PL true
    pram[0] = 0x0E3100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_pl_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::N; // N=1, PL false
    pram[0] = 0x0E3100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_jcc_mi_taken() {
    // MI (cc=0xB): true when N=1.
    // CCCC=1011, addr=$100 -> 0x0EB100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::N; // N=1, MI true
    pram[0] = 0x0EB100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
}

#[test]
fn test_jcc_mi_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // N=0, MI false
    pram[0] = 0x0EB100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_bcc_short_ge_taken() {
    // Bcc short GE: template 00000101CCCC01aaaa0aaaaa
    // CCCC=0001 (GE), offset +4: aaaa=0000, aaaaa=00100
    // 0000_0101_0001_0100_0000_0100 = 0x051404
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // N=0,V=0 -> N^V=0, GE true
    pram[0] = 0x051404;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 4);
}

#[test]
fn test_bcc_short_ge_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::N; // N=1,V=0 -> N^V=1, GE false
    pram[0] = 0x051404;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_bcc_short_ne_taken() {
    // Bcc short NE: CCCC=0010, offset +4
    // 0000_0101_0010_0100_0000_0100 = 0x052404
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // Z=0, NE true
    pram[0] = 0x052404;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 4);
}

#[test]
fn test_bcc_long_ge_taken() {
    // Bcc long GE: 00001101000100000100CCCC, CCCC=0001
    // 0x0D1041
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // GE true (N=V=0)
    pram[0] = 0x0D1041; // bcc GE xxxx
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x20);
}

#[test]
fn test_bcc_long_ge_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::N; // GE false
    pram[0] = 0x0D1041;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_bscc_short_ge_taken() {
    // BScc short GE: template 00000101CCCC00aaaa0aaaaa
    // CCCC=0001, offset +5: 0000_0101_0001_0000_0000_0101 = 0x051005
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // GE true
    pram[0x10] = 0x051005;
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x15); // 0x10 + 5
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_bscc_short_ge_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::N; // GE false
    pram[0x10] = 0x051005;
    s.pc = 0x10;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x11);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jscc_ge_taken() {
    // JScc GE: 00001111CCCCaaaaaaaaaaaa, CCCC=0001, addr=$100
    // 0x0F1100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // GE true
    pram[0] = 0x0F1100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
}

#[test]
fn test_jscc_ge_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::V; // GE false (N=0,V=1 -> N^V=1)
    pram[0] = 0x0F1100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bcc_negative_displacement() {
    // Bcc long with negative displacement (backward branch when taken).
    // Bcc EQ long at PC=0x20, displacement = -0x10 = 0xFFFFF0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.pc = 0x20;
    s.registers[reg::SR] = 1 << sr::Z; // EQ true
    pram[0x20] = 0x0D104A; // bcc EQ long
    pram[0x21] = 0xFFFFF0; // displacement = -16
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 0x10,
        "Bcc with negative displacement should branch backward"
    );
}

#[test]
fn test_bsr_short() {
    // BSR short: template 00000101000010aaaa0aaaaa
    // offset +5: aaaa=0000, aaaaa=00101
    // 0000_0101_0000_1000_0000_0101 = 0x050805
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let sr_before = 0xC00300u32;
    s.registers[reg::SR] = sr_before;
    s.pc = 0x10;
    pram[0x10] = 0x050805; // bsr +5
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.pc, 0x15,
        "BSR short should branch to PC+displacement (0x10+5)"
    );
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(sp, 1, "SP should be 1 after push");
    assert_eq!(
        s.stack[0][sp], 0x11,
        "SSH should contain return address (PC+1 for 1-word BSR)"
    );
    assert_eq!(
        s.stack[1][sp], sr_before,
        "SSL should contain pre-BSR SR value"
    );
}

#[test]
fn test_jclr_reg_not_taken() {
    // jclr #0,X0,$100: bit 0 set -> "clear" is false -> not taken.
    // Uses same encoding as test_jclr_reg_taken: 0x0AC400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set
    pram[0] = 0x0AC400;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jclr_ea_not_taken() {
    // jclr #1,X:(R0),$100: bit 1 set -> not taken.
    // Same encoding as test_jclr_ea_taken: 0x0A6081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000002; // bit 1 set -> jclr NOT taken
    pram[0] = 0x0A6081;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jset_reg_not_taken() {
    // jset #0,X0,$100: bit 0 clear -> "set" is false -> not taken.
    // Same encoding as test_jset_reg_taken: 0x0AC420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // bit 0 clear
    pram[0] = 0x0AC420;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jset_qq_not_taken() {
    // jset #0,X:$FFFF80,$42: bit 0 clear -> not taken.
    // Same encoding as test_jset_qq_taken: 0x0180A0
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
    periph[0] = 0x000000; // bit 0 clear
    pram[0] = 0x0180A0;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jset_aa_not_taken() {
    // jset #3,X:$10,$100: bit 3 clear -> not taken.
    // Same encoding as test_jset_aa_taken: 0x0A10A3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 3 clear -> jset not taken
    pram[0] = 0x0A10A3;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jset_ea_not_taken() {
    // jset #0,X:(R0),$100: bit 0 clear -> not taken.
    // Same encoding as test_jset_ea_taken: 0x0A60A0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000000; // bit 0 clear
    pram[0] = 0x0A60A0;
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_jsclr_reg_not_taken() {
    // jsclr #0,X0,$200: bit 0 set -> "clear" is false -> not taken.
    // Same encoding as test_jsclr_reg_taken: 0x0BC400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set
    pram[0] = 0x0BC400;
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jsclr_pp_not_taken() {
    // jsclr #1,X:$FFFFC4,$300: bit 1 set -> not taken.
    // Same encoding as test_jsclr_pp_taken: 0x0B8481
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
    periph[0x44] = 0x000002; // bit 1 set
    pram[0] = 0x0B8481;
    pram[1] = 0x000300;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jsclr_qq_not_taken() {
    // jsclr #0,X:$FFFF80,$42: bit 0 set -> not taken.
    // Same encoding as test_jsclr_qq_taken: 0x01C080
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
    periph[0] = 0x000001; // bit 0 set
    pram[0] = 0x01C080;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jsset_reg_not_taken() {
    // jsset #0,X0,$200: bit 0 clear -> "set" is false -> not taken.
    // Same encoding as test_jsset_reg_taken: 0x0BC420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // bit 0 clear
    pram[0] = 0x0BC420;
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jsset_qq_not_taken() {
    // jsset #0,X:$FFFF80,$42: bit 0 clear -> not taken.
    // Same encoding as test_jsset_qq_taken: 0x01C0A0
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
    periph[0] = 0x000000; // bit 0 clear
    pram[0] = 0x01C0A0;
    pram[1] = 0x000042;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_jsset_ea_not_taken() {
    // jsset #0,X:(R0),$200: bit 0 clear -> not taken.
    // Same encoding as test_jsset_ea_taken: 0x0B60A0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000000; // bit 0 clear
    pram[0] = 0x0B60A0;
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_brclr_pp_not_taken() {
    // brclr #0,X:$FFFFC4,$10: bit 0 set -> not taken.
    // Same encoding as test_brclr_pp_taken: 0x0CC400
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
    periph[0x44] = 0x000001; // bit 0 set at $FFFFC4 (PERIPH_BASE + 0x44)
    pram[0] = 0x0CC400;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brclr_qq_not_taken() {
    // brclr #0,X:$FFFF80,$10: bit 0 set -> not taken.
    // Same encoding as test_brclr_qq_taken: 0x048000
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
    periph[0] = 0x000001; // bit 0 set
    pram[0] = 0x048000;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brclr_ea_not_taken() {
    // brclr #0,X:(R0),$20: bit 0 set -> not taken.
    // Same encoding as test_brclr_ea_taken: 0x0CA000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000001; // bit 0 set
    pram[0] = 0x0CA000;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brset_pp_not_taken() {
    // brset #0,X:$FFFFC4,$10: bit 0 clear -> not taken.
    // Same encoding as test_brset_pp_taken: 0x0CC420
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
    periph[69] = 0x000000; // bit 0 clear
    pram[0] = 0x0CC420;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brset_qq_not_taken() {
    // brset #0,X:$FFFF80,$10: bit 0 clear -> not taken.
    // Same encoding as test_brset_qq_taken: 0x048020
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
    periph[0] = 0x000000; // bit 0 clear
    pram[0] = 0x048020;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brset_aa_not_taken() {
    // brset #2,X:$10,$10: bit 2 clear -> not taken.
    // Same encoding as test_brset_aa_taken: 0x0C90A2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 2 clear
    pram[0] = 0x0C90A2;
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brset_ea_not_taken() {
    // brset #0,X:(R0),$20: bit 0 clear -> not taken.
    // BRSET ea: 0000110010MMMRRR0S1bbbbb (bit 5 = 1 for BRSET)
    // MMM=100 ((R0)), RRR=000, S=0, bbbbb=00000
    // 0000_1100_1010_0000_0010_0000 = 0x0CA020
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000000; // bit 0 clear -> BRSET not taken
    pram[0] = 0x0CA020;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_bsclr_reg_not_taken() {
    // bsclr #0,X0,$20: bit 0 set -> "clear" false -> not taken.
    // Same encoding as test_bsclr_reg_taken: 0x0DC480
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set
    pram[0] = 0x0DC480;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsclr_pp_not_taken() {
    // bsclr #0,X:$FFFFC0,$20: bit 0 set -> not taken.
    // Same encoding as test_bsclr_pp_taken: 0x0DC000
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
    periph[0x40] = 0x000001; // bit 0 set
    pram[0] = 0x0DC000;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsclr_qq_not_taken() {
    // bsclr #0,X:$FFFF80,$20: bit 0 set -> not taken.
    // Same encoding as test_bsclr_qq_taken: 0x048080
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
    periph[0] = 0x000001; // bit 0 set
    pram[0] = 0x048080;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsclr_aa_not_taken() {
    // bsclr #1,X:$10,$20: bit 1 set -> not taken.
    // Same encoding as test_bsclr_aa_taken: 0x0D9081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000002; // bit 1 set
    pram[0] = 0x0D9081;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsset_reg_not_taken() {
    // bsset #0,X0,$20: bit 0 clear -> "set" false -> not taken.
    // Same encoding as test_bsset_reg_taken: 0x0DC4A0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // bit 0 clear
    pram[0] = 0x0DC4A0;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsset_pp_not_taken() {
    // bsset #0,X:$FFFFC0,$20: bit 0 clear -> not taken.
    // Same encoding as test_bsset_pp_periph: 0x0DC020
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
    periph[0x40] = 0x000000; // bit 0 clear
    pram[0] = 0x0DC020;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsset_qq_not_taken() {
    // bsset #0,X:$FFFF80,$20: bit 0 clear -> not taken.
    // Same encoding as test_bsset_qq_taken: 0x0480A0
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
    periph[0] = 0x000000; // bit 0 clear
    pram[0] = 0x0480A0;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsset_aa_not_taken() {
    // bsset #1,X:$10,$20: bit 1 clear -> not taken.
    // Same encoding as test_bsset_aa_taken: 0x0D90A1
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 1 clear
    pram[0] = 0x0D90A1;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_bsset_ea_not_taken() {
    // bsset #0,X:(R0),$20: bit 0 clear -> not taken.
    // BSSET ea: 0000110110MMMRRR0S1bbbbb (bit 5 = 1 for BSSET)
    // MMM=100 ((R0)), RRR=000, S=0, bbbbb=00000
    // 0000_1101_1010_0000_0010_0000 = 0x0DA020
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    s.registers[reg::M0] = 0xFFFFFF;
    xram[0x10] = 0x000000; // bit 0 clear -> BSSET not taken
    pram[0] = 0x0DA020;
    pram[1] = 0x000020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 0);
}

#[test]
fn test_pflushun_nop() {
    // PFLUSHUN: 0x000001. Treated as NOP per LIMITATIONS.md.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000001; // pflushun
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_pfree_nop() {
    // PFREE: 0x000002. Treated as NOP per LIMITATIONS.md.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000002; // pfree
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_plockr_nop() {
    // PLOCKR xxxx: 0x00000F + extension word. PC should advance by 2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x00000F; // plockr xxxx
    pram[1] = 0x000042; // displacement (ignored)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_punlockr_nop() {
    // PUNLOCKR xxxx: 0x00000E + extension word. PC should advance by 2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x00000E; // punlockr xxxx
    pram[1] = 0x000042; // displacement (ignored)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_punlock_ea_nop() {
    // PUNLOCK (R0): 0000101011MMMRRR10000001
    // MMM=100 (no update), RRR=000 (R0)
    // 0000_1010_1110_0000_1000_0001 = 0x0AE081
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x100;
    pram[0] = 0x0AE081; // punlock (r0)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_rts_standalone_sr_unchanged() {
    // RTS pops PC from SSH but does NOT restore SR (unlike RTI).
    // Per DSP56300FM p.13-168: "SSH -> PC; SP-1 -> SP"
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push a return address and SR to stack (as JSR would)
    s.registers[reg::SP] = 1;
    s.stack[0][1] = 0x0042; // SSH = return PC
    s.stack[1][1] = 0x000345; // SSL = some saved SR (should NOT be restored by RTS)
    // Set current SR to a different value
    let sr_before = 0xC00100u32;
    s.registers[reg::SR] = sr_before;
    pram[0] = 0x00000C; // rts
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42, "RTS should pop PC from SSH");
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        0,
        "SP should decrement after RTS"
    );
    // SR should remain unchanged (RTS does NOT restore SR)
    assert_eq!(
        s.registers[reg::SR],
        sr_before,
        "RTS should NOT modify SR (unlike RTI)"
    );
}

#[test]
fn test_rti_full_sr_restore() {
    // RTI should restore ALL SR bits (both MR and CCR) from SSL.
    // Per DSP56300FM p.13-167: "SSH -> PC; SSL -> SR; SP-1 -> SP"
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Saved SR has MR bits (I1:I0=11, LF=1) and CCR bits (Z=1, C=1, N=1)
    let saved_sr =
        (1 << sr::I1) | (1 << sr::I0) | (1 << sr::LF) | (1 << sr::Z) | (1 << sr::C) | (1 << sr::N);
    s.registers[reg::SP] = 1;
    s.stack[0][1] = 0x0042; // SSH = return PC
    s.stack[1][1] = saved_sr; // SSL = saved SR
    s.registers[reg::SR] = 0; // current SR is all zeros
    pram[0] = 0x000004; // rti
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42);
    assert_eq!(s.registers[reg::SP] & 0xF, 0);
    // Check MR bits restored
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::I1),
        0,
        "I1 should be restored"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::I0),
        0,
        "I0 should be restored"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should be restored"
    );
    // Check CCR bits restored
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z should be restored"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should be restored"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N should be restored"
    );
}

#[test]
fn test_rts_vs_rti_sr_difference() {
    // Same stack state for both: prove RTS does not restore SR, RTI does.
    // RTS path
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let saved_sr = (1 << sr::Z) | (1 << sr::N) | (1 << sr::I0);
    s.registers[reg::SP] = 1;
    s.stack[0][1] = 0x0042;
    s.stack[1][1] = saved_sr;
    s.registers[reg::SR] = 0xC00000; // current SR different from saved
    pram[0] = 0x00000C; // rts
    run_one(&mut s, &mut jit);
    let sr_after_rts = s.registers[reg::SR];

    // RTI path
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut xram2 = [0u32; XRAM_SIZE];
    let mut yram2 = [0u32; YRAM_SIZE];
    let mut pram2 = [0u32; PRAM_SIZE];
    let mut s2 = DspState::new(MemoryMap::test(&mut xram2, &mut yram2, &mut pram2));
    s2.registers[reg::SP] = 1;
    s2.stack[0][1] = 0x0042;
    s2.stack[1][1] = saved_sr;
    s2.registers[reg::SR] = 0xC00000;
    pram2[0] = 0x000004; // rti
    run_one(&mut s2, &mut jit2);
    let sr_after_rti = s2.registers[reg::SR];

    // RTS should NOT have restored saved_sr bits
    assert_eq!(
        sr_after_rts & (1 << sr::Z),
        0,
        "RTS should not restore Z from stack"
    );
    // RTI SHOULD have restored saved_sr bits
    assert_ne!(
        sr_after_rti & (1 << sr::Z),
        0,
        "RTI should restore Z from stack"
    );
    assert_ne!(
        sr_after_rti & (1 << sr::N),
        0,
        "RTI should restore N from stack"
    );
    assert_ne!(
        sr_after_rti & (1 << sr::I0),
        0,
        "RTI should restore I0 from stack"
    );
}

#[test]
fn test_ifcc_ccr_not_updated_when_taken() {
    // IFcc (without .U) should NOT update CCR even when the ALU op executes.
    // Per DSP56300FM p.13-74: "Condition codes are NOT updated."
    // Use ADD X0,A which would normally set N=1 for a large result.
    // IFcc encoding: 0x202000 | (CCCC << 8) | alu_byte
    // ADD X0,A = 0x40, CC(cc=0): 0x202040
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x400000;
    s.registers[reg::A1] = 0x400000;
    // Result A1 = 0x800000 which is the sign bit position - would set N=1 in normal ADD.
    s.registers[reg::SR] = 0; // C=0 (CC true), N=0, Z=0
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x202040; // add X0,A ifcc
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x800000, "ADD should execute");
    // CCR bits should NOT change (IFcc without .U)
    assert_eq!(
        s.registers[reg::SR] & 0xFF,
        sr_before & 0xFF,
        "IFcc (not .U) should not update CCR"
    );
}

#[test]
fn test_jmp_ea_mode6_absolute() {
    // JMP ea with mode 6 (absolute address from extension word).
    // JMP ea: 0000101011MMMRRR00000000
    // MMM=110, RRR=000: 0000_1010_1111_0000_0000_0000 = 0x0AF000
    // But need to check the exact encoding. Looking at JSR ea mode 6:
    // test_jsr_ea uses 0x0BF080 for JSR ea mode 6. JSR ea: 0000101111MMMRRR10000000
    // So JMP ea mode 6: 0000101011MMMRRR00000000, MMM=110, RRR=000
    // 0000_1010_1111_0000_0000_0000 = 0x0AF000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0AF000; // jmp p:$xxxx (mode 6 absolute)
    pram[1] = 0x000200; // target = $200
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200);
}

#[test]
fn test_jmp_ea_postincrement() {
    // JMP (R0)+ should jump to address in R0 AND post-increment R0.
    // JMP ea: 0000101011MMMRRR10000000
    // MMM=011 ((Rn)+), RRR=000 (R0): 0000_1010_1101_1000_1000_0000 = 0x0AD880
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x42;
    s.registers[reg::M0] = 0xFFFFFF;
    pram[0] = 0x0AD880; // jmp (r0)+
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42, "JMP should jump to address in R0");
    assert_eq!(s.registers[reg::R0], 0x43, "R0 should be post-incremented");
}

#[test]
fn test_jsr_ea_indirect_rn() {
    // JSR (R0): indirect jump to subroutine at address held in R0.
    // JSR ea: 0000101111MMMRRR10000000
    // MMM=100 (no update), RRR=000 (R0)
    // 0000_1011_1110_0000_1000_0000 = 0x0BE080
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x100;
    pram[0] = 0x0BE080; // jsr (r0)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100);
    assert_eq!(s.registers[reg::SP] & 0xF, 1);
    // Return address should be PC+1 (1-word instruction)
    assert_eq!(s.stack[0][1], 1, "SSH should hold return address");
}

#[test]
fn test_bsr_long_ssl_contains_sr() {
    // BSR long should push SR to SSL, just like JSR.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let sr_before = 0xC00345u32;
    s.registers[reg::SR] = sr_before;
    pram[0] = 0x0D1080; // bsr xxxx
    pram[1] = 0x000050;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x50);
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(
        s.stack[1][sp], sr_before,
        "SSL should contain pre-BSR SR value"
    );
}

#[test]
fn test_bsr_rn_ssh_return_address() {
    // BSR Rn is a 1-word instruction: return address should be PC+1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R1] = 0x0050;
    s.pc = 0x10;
    pram[0x10] = 0x0D1980; // bsr R1
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x60, "BSR Rn should jump to PC + Rn");
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(
        s.stack[0][sp], 0x11,
        "SSH should hold return address (PC+1 for 1-word BSR Rn)"
    );
}

#[test]
fn test_bscc_long_ssl_contains_sr() {
    // BScc long taken should push SR to SSL.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let sr_before = 0x000034u32; // Z=1 (bit 2 set) so EQ is true, plus some other bits
    s.registers[reg::SR] = sr_before;
    pram[0] = 0x0D100A; // bscc EQ long
    pram[1] = 0x000030;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x30);
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(
        s.stack[1][sp], sr_before,
        "SSL should contain pre-BScc SR value"
    );
}

#[test]
fn test_jcc_gt_taken() {
    // GT condition (CondCode::GT=7). GT = !(Z | (N ^ V)).
    // Setup: Z=0, N=0, V=0 -> GT = !(0 | (0^0)) = !0 = 1 -> taken.
    // Jcc template: 00001110CCCC_aaaaaaaaaaaa. CCCC=0111 (GT=7), addr=$100.
    // 0000_1110_0111_0001_0000_0000 = 0x0E7100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // Z=0, N=0, V=0
    pram[0] = 0x0E7100; // jcc GT,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "GT: should be taken when Z=0, N=V");
}

#[test]
fn test_ep_firmware_cmp_parallel_move_then_bgt() {
    // Exact EP sequence at P:$05BA-$05BC. Parallel moves read the old X1 for
    // the CMP and commit -2 for the following instruction. A=0x7fffff is
    // greater than old X1=2, so BGT must take the firmware's relative branch.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.pc = 0x05BA;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0;
    s.registers[reg::X1] = 2;
    pram[0x05BA] = 0x45F465; // cmp x1,a; move #$fffffe,x1
    pram[0x05BB] = 0xFFFFFE;
    pram[0x05BC] = 0x057417; // bgt $05d3

    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x05BC);
    assert_eq!(s.registers[reg::X1], 0xFFFFFE);
    assert_eq!(
        s.registers[reg::SR] & ((1 << sr::N) | (1 << sr::V) | (1 << sr::Z)),
        0
    );

    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x05D3, "EP firmware BGT must take for 0x7fffff > 2");
}

#[test]
fn test_ep_firmware_cmp_negative_two_then_bgt() {
    // Exact steady-state EP sequence at P:$05BA-$05BC after X1 already holds -2.
    // The accumulator is positive and therefore greater than -2; BGT must take.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.pc = 0x05BA;
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0x7FFFFF;
    s.registers[reg::A0] = 0;
    s.registers[reg::X1] = 0xFFFFFE;
    pram[0x05BA] = 0x45F465;
    pram[0x05BB] = 0xFFFFFE;
    pram[0x05BC] = 0x057417;

    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x05BC);
    assert_eq!(s.registers[reg::X1], 0xFFFFFE);
    assert_eq!(
        s.registers[reg::SR] & ((1 << sr::N) | (1 << sr::V) | (1 << sr::Z)),
        0
    );

    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x05D3, "EP firmware BGT must take for 0x7fffff > -2");
}
#[test]
fn test_jcc_gt_not_taken_z_set() {
    // GT = !(Z | (N^V)). Z=1 -> GT=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z;
    pram[0] = 0x0E7100; // jcc GT,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1, "GT: should not be taken when Z=1");
}

#[test]
fn test_jcc_le_taken() {
    // LE condition (CondCode::LE=15). LE = Z | (N ^ V).
    // Z=1 -> LE=1 -> taken.
    // CCCC=1111 (LE=15), addr=$100.
    // 0000_1110_1111_0001_0000_0000 = 0x0EF100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::Z;
    pram[0] = 0x0EF100; // jcc LE,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "LE: should be taken when Z=1");
}

#[test]
fn test_jcc_cs_taken() {
    // CS (carry set, CondCode::CS=8). CS = C.
    // C=1 -> CS=1 -> taken.
    // CCCC=1000 (CS=8), addr=$100.
    // 0000_1110_1000_0001_0000_0000 = 0x0E8100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::C;
    pram[0] = 0x0E8100; // jcc CS,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "CS: should be taken when C=1");
}

#[test]
fn test_jcc_nn_taken() {
    // NN (not normalized, CondCode::NN=4). NN = Z | !(U | E).
    // Z=0, U=0, E=0 -> NN = 0 | !(0|0) = 1 -> taken.
    // CCCC=0100 (NN=4), addr=$100.
    // 0000_1110_0100_0001_0000_0000 = 0x0E4100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0;
    pram[0] = 0x0E4100; // jcc NN,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "NN: should be taken when Z=0, U=0, E=0");
}

#[test]
fn test_jcc_ec_taken() {
    // EC (extension clear, CondCode::EC=5). EC = !E.
    // E=0 -> EC=1 -> taken.
    // CCCC=0101 (EC=5), addr=$100.
    // 0000_1110_0101_0001_0000_0000 = 0x0E5100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0;
    pram[0] = 0x0E5100; // jcc EC,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "EC: should be taken when E=0");
}

#[test]
fn test_jcc_lc_taken() {
    // LC (limit clear, CondCode::LC=6). LC = !L.
    // L=0 -> LC=1 -> taken.
    // CCCC=0110 (LC=6), addr=$100.
    // 0000_1110_0110_0001_0000_0000 = 0x0E6100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0;
    pram[0] = 0x0E6100; // jcc LC,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "LC: should be taken when L=0");
}

#[test]
fn test_punlock_ea_agu_side_effects() {
    // PUNLOCK (R0)+ should post-increment R0.
    // PUNLOCK template: 0000101011MMMRRR10000001, MMM=011((R0)+), RRR=000
    // 0000_1010_1101_1000_1000_0001 = 0x0AD881
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x100;
    pram[0] = 0x0AD881; // punlock (R0)+
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::R0],
        0x101,
        "PUNLOCK (R0)+: R0 should be post-incremented"
    );
}

#[test]
fn test_jsclr_ssl_contains_sr() {
    // JSCLR taken should push SSL with pre-instruction SR.
    // jsclr #0,X0,$200: 0x0BC400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000002; // bit 0 clear -> JSCLR taken
    s.registers[reg::SR] = (1 << sr::N) | (1 << sr::C); // pre-set some flags
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x0BC400; // jsclr #0,X0,$200
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200, "JSCLR: should jump when bit 0 is clear");
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(sp, 1, "JSCLR: SP should be 1 after push");
    assert_eq!(
        s.stack[1][sp], sr_before,
        "JSCLR: SSL should contain the pre-instruction SR value"
    );
}

#[test]
fn test_jsset_ssl_contains_sr() {
    // JSSET taken should push SSL with pre-instruction SR.
    // jsset #0,X0,$200: 0x0BC420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set -> JSSET taken
    s.registers[reg::SR] = (1 << sr::Z) | (1 << sr::V); // pre-set flags
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x0BC420; // jsset #0,X0,$200
    pram[1] = 0x000200;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x200, "JSSET: should jump when bit 0 is set");
    let sp = (s.registers[reg::SP] & 0xF) as usize;
    assert_eq!(sp, 1, "JSSET: SP should be 1 after push");
    assert_eq!(
        s.stack[1][sp], sr_before,
        "JSSET: SSL should contain the pre-instruction SR value"
    );
}

#[test]
fn test_jclr_ccr_unchanged() {
    // JCLR should not modify CCR (S, L, or any flag).
    // jclr #0,X0,$100: template 0000101011DDDDDD000bbbbb
    // DDDDDD=0x04 (X0), bbbbb=0
    // 0000_1010_1100_0100_0000_0000 = 0x0AC400
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000002; // bit 0 clear -> taken
    s.registers[reg::SR] = (1 << sr::N) | (1 << sr::V) | (1 << sr::L) | (1 << sr::S);
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x0AC400; // jclr #0,X0,$100
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "JCLR: should jump");
    assert_eq!(
        s.registers[reg::SR],
        sr_before,
        "JCLR: SR/CCR should be completely unchanged"
    );
}

#[test]
fn test_jset_ccr_unchanged() {
    // JSET should not modify CCR.
    // jset #0,X0,$100: 0x0AC420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000001; // bit 0 set -> taken
    s.registers[reg::SR] = (1 << sr::N) | (1 << sr::V) | (1 << sr::L) | (1 << sr::S);
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x0AC420; // jset #0,X0,$100
    pram[1] = 0x000100;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "JSET: should jump");
    assert_eq!(
        s.registers[reg::SR],
        sr_before,
        "JSET: SR/CCR should be completely unchanged"
    );
}

#[test]
fn test_jcc_nr_taken() {
    // (remaining): NR (normalized, CondCode::NR=12). NR = Z | (!U & E).
    // U=0, E=1 -> NR = 0 | (!0 & 1) = 1 -> taken.
    // CCCC=1100 (NR=12), addr=$100.
    // 0000_1110_1100_0001_0000_0000 = 0x0EC100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::E; // E=1, U=0
    pram[0] = 0x0EC100; // jcc NR,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "NR: should be taken when E=1 and U=0");
}

#[test]
fn test_jcc_es_taken() {
    // (remaining): ES (extension set, CondCode::ES=13). ES = E.
    // E=1 -> ES=1 -> taken.
    // CCCC=1101 (ES=13), addr=$100.
    // 0000_1110_1101_0001_0000_0000 = 0x0ED100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::E;
    pram[0] = 0x0ED100; // jcc ES,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "ES: should be taken when E=1");
}

#[test]
fn test_jcc_ls_taken() {
    // (remaining): LS (limit set, CondCode::LS=14). LS = L.
    // L=1 -> LS=1 -> taken.
    // CCCC=1110 (LS=14), addr=$100.
    // 0000_1110_1110_0001_0000_0000 = 0x0EE100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 1 << sr::L;
    pram[0] = 0x0EE100; // jcc LS,$100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "LS: should be taken when L=1");
}

#[test]
fn test_jmp_short_ccr_unchanged() {
    // JMP short should not modify any CCR/SR bits.
    // Manual p.13-83: all CCR bits marked "---" (unchanged).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set all CCR bits plus some MR bits
    s.registers[reg::SR] = (1 << sr::N)
        | (1 << sr::Z)
        | (1 << sr::V)
        | (1 << sr::C)
        | (1 << sr::L)
        | (1 << sr::S)
        | (1 << sr::E)
        | (1 << sr::U);
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x0C0042; // jmp $0042
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x42, "JMP: PC should be $42");
    assert_eq!(
        s.registers[reg::SR],
        sr_before,
        "JMP: SR should be completely unchanged"
    );
}

#[test]
fn test_jsclr_pc_dependent_return_addr() {
    // Verify JSCLR pushes the correct return address when the same opcode
    // appears at different PCs. The PC-independent instruction cache must
    // NOT share compiled JSCLR across PCs (it embeds pc+2 as the return
    // address constant).
    //
    // jsclr #0,X0,$100: opcode=0x0BC400, next_word=0x000100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000002; // bit 0 clear -> taken

    // Place same JSCLR at PC=$0000 and PC=$0010
    pram[0x00] = 0x0BC400;
    pram[0x01] = 0x000100; // target $100
    pram[0x10] = 0x0BC400;
    pram[0x11] = 0x000100; // target $100
    // RTS at target to pop the return address
    pram[0x100] = 0x00000C; // rts

    // Execute JSCLR at PC=$0000 -> should push return addr $0002
    s.pc = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "JSCLR at $0000 should jump to $100");
    let ssh1 = s.stack[0][(s.registers[reg::SP] & 0xF) as usize];
    assert_eq!(ssh1, 0x0002, "Return address from PC=$0000 should be $0002");

    // Pop via RTS
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x0002, "RTS should return to $0002");

    // Execute JSCLR at PC=$0010 -> should push return addr $0012
    s.pc = 0x10;
    s.registers[reg::X0] = 0x000002; // bit 0 still clear
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x100, "JSCLR at $0010 should jump to $100");
    let ssh2 = s.stack[0][(s.registers[reg::SP] & 0xF) as usize];
    assert_eq!(ssh2, 0x0012, "Return address from PC=$0010 should be $0012");

    // Pop via RTS
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 0x0012, "RTS should return to $0012");
}
