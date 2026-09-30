use super::*;

#[test]
fn test_do_imm() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // do #3,$0010 -> template 00000110iiiiiiii1000hhhh
    // count = ((opcode>>8) & 0xFF) + ((opcode & 0xF) << 8)
    // For count=3: bits[15:8]=3, bits[3:0]=0
    // opcode = 00000110_00000011_1000_0000 = 0x060380
    pram[0] = 0x060380;
    pram[1] = 0x0010; // loop end address
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 3);
    assert_eq!(s.registers[reg::LA], 0x0010);
    assert_ne!(s.registers[reg::SR] & (1 << 15), 0); // LF set
    assert_eq!(s.pc, 2); // advance past 2-word instruction
}

#[test]
fn test_do_reg() {
    // do X0,expr: encoding 0000011011DDDDDD00000000
    // X0 is register 0x04, so DDDDDD=000100
    // 0x06C400 (bits: 0000 0110 11 000100 00000000)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 10; // loop count = 10
    pram[0] = 0x06C400; // do X0,$0020
    pram[1] = 0x0020; // loop end address
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 10);
    assert_eq!(s.registers[reg::LA], 0x0020);
    assert_ne!(s.registers[reg::SR] & (1 << 15), 0); // LF set
    assert_eq!(s.pc, 2);
}

#[test]
fn test_rep_reg() {
    // rep X0: encoding 0000011011dddddd00100000
    // X0=0x04: 0x06C420
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 8;
    pram[0] = 0x06C420; // rep X0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 8);
    assert!(s.loop_rep);
}

#[test]
fn test_do_aa() {
    // do X:$10,$20: template 0000011000aaaaaa0S000000
    // aaaaaa=0x10, S=0 (X)
    // 0000_0110_0001_0000_0000_0000 = 0x061000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 5; // loop count
    pram[0] = 0x061000;
    pram[1] = 0x000020; // loop end addr
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 5);
    assert_eq!(s.registers[reg::LA], 0x20);
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF set
    assert_eq!(s.pc, 2);
}

#[test]
fn test_dor_imm() {
    // dor #10,$08: template 00000110iiiiiiii1001hhhh
    // Immediate loop count: bits 15:8 (low byte) | bits 3:0 (high nibble) << 8
    // count = 10 = 0x00A -> low=0x0A, high=0x0 -> iiiiiiii=0x0A, hhhh=0x0
    // 0000_0110_0000_1010_1001_0000 = 0x060A90
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060A90;
    pram[1] = 0x000008; // relative offset for loop end
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 10);
    // LA = PC(0) + relative_offset(8) = 8
    assert_eq!(s.registers[reg::LA], 8);
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_dor_reg() {
    // dor X0,$08: template 0000011011DDDDDD00010000
    // DDDDDD=0x04 (X0)
    // 0000_0110_1100_0100_0001_0000 = 0x06C410
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 7;
    pram[0] = 0x06C410;
    pram[1] = 0x000010; // relative offset
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 7);
    assert_eq!(s.registers[reg::LA], 0x10); // PC(0) + 0x10
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_rep_aa() {
    // rep X:$10: template 0000011000aaaaaa0S100000
    // aaaaaa=0x10, S=0 (X)
    // 0000_0110_0001_0000_0010_0000 = 0x061020
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 4; // repeat count
    s.registers[reg::LC] = 0x99; // old LC saved
    pram[0] = 0x061020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LC], 4);
    assert_eq!(s.registers[reg::TEMP], 0x99);
    assert!(s.loop_rep);
}

#[test]
fn test_do_loop_inc_execute_one() {
    // DO #3,$0003 (loop body: inc A, nop)
    // Expected: 3 iterations, A = 3
    //
    // Step-by-step:
    // Step 1: DO #3,$0003 -> push stack, LF=1, LA=3, LC=3, PC=2
    // Step 2: inc A -> A=1, postexecute: PC=3
    // Step 3: nop -> postexecute: PC=4=LA+1 -> LC=2, PC=SSH=2
    // Step 4: inc A -> A=2, postexecute: PC=3
    // Step 5: nop -> postexecute: PC=4=LA+1 -> LC=1, PC=SSH=2
    // Step 6: inc A -> A=3, postexecute: PC=3
    // Step 7: nop -> postexecute: PC=4=LA+1 -> LC=0, pop stack, LF=0, PC=4
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060380; // do #3,$addr
    pram[1] = 0x000003; // loop end = $0003
    pram[2] = 0x000008; // inc A
    pram[3] = 0x000000; // nop

    // Step 1: execute DO
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::LC], 3);
    assert_eq!(s.registers[reg::LA], 3);
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0);

    // Steps 2-7: 3 iterations of (inc A, nop)
    for i in 1..=3 {
        run_one(&mut s, &mut jit); // inc A
        assert_eq!(s.registers[reg::A0], i, "A0 after inc #{i}");
        run_one(&mut s, &mut jit); // nop
    }

    // After loop: LF cleared, PC past loop
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
    assert_eq!(s.pc, 4);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_do_nested() {
    // outer DO #2,$0005, inner DO #3,$0004
    // body: inc A
    // Expected: 2 * 3 = 6 iterations, A = 6
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2,$addr (outer)
    pram[1] = 0x000005; // outer end = $0005
    pram[2] = 0x060380; // do #3,$addr (inner)
    pram[3] = 0x000004; // inner end = $0004
    pram[4] = 0x000008; // inc A
    pram[5] = 0x000000; // nop (outer loop body padding)

    // Step 1: outer DO
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);

    // Outer loop: 2 iterations
    for _outer in 0..2 {
        // Step: inner DO
        run_one(&mut s, &mut jit); // DO #3
        // Inner loop: 3 iterations of inc A
        for _inner in 0..3 {
            run_one(&mut s, &mut jit); // inc A
        }
        // After inner loop: nop (outer body)
        run_one(&mut s, &mut jit); // nop
    }

    assert_eq!(s.registers[reg::A0], 6);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
    assert_eq!(s.pc, 6);
}

#[test]
fn test_enddo_mid_loop() {
    // DO #5,$0003, body: enddo, nop
    // enddo should exit loop immediately after first instruction
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060580; // do #5,$0003
    pram[1] = 0x000003; // loop end = 3
    pram[2] = 0x00008C; // enddo
    pram[3] = 0x000000; // nop (shouldn't reach as loop body end)

    run_one(&mut s, &mut jit); // DO
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0);
    run_one(&mut s, &mut jit); // enddo
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF cleared
    assert_eq!(s.pc, 3); // enddo sets PC past DO body
}

#[test]
fn test_run_do_loop() {
    // DO #3,$0003 + inc A + nop, then jmp-to-self
    // Tests DO loop iteration in run() mode
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060380; // do #3,$0003
    pram[1] = 0x000003; // loop end = $0003
    pram[2] = 0x000008; // inc A
    pram[3] = 0x000000; // nop
    pram[4] = 0x0C0004; // jmp $0004 (loop to stop)

    s.run(&mut jit, 1000);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_run_do_loop_inline_verifies_state() {
    // DO #4,$0004 with body: inc A, inc A, nop
    // Body is simple (no terminators/peripheral writes) -> inlined.
    // After loop: A = 4*2 = 8, PC = 5, LF cleared, LA/LC restored.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::LA] = 0x1234; // should be restored after loop
    s.registers[reg::LC] = 0x5678;
    pram[0] = 0x060480; // do #4,$0004
    pram[1] = 0x000004; // la = $0004
    pram[2] = 0x000008; // inc A
    pram[3] = 0x000008; // inc A
    pram[4] = 0x000000; // nop
    pram[5] = 0x0C0005; // jmp $0005 (halt)

    s.run(&mut jit, 1000);
    assert_eq!(s.registers[reg::A0], 8);
    assert_eq!(s.registers[reg::LA], 0x1234); // restored
    assert_eq!(s.registers[reg::LC], 0x5678); // restored
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF cleared
}

#[test]
fn test_run_do_loop_inline_lc1() {
    // DO #1: single iteration edge case.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060180; // do #1,$0002
    pram[1] = 0x000002; // la = $0002
    pram[2] = 0x000008; // inc A
    pram[3] = 0x0C0003; // jmp $0003 (halt)

    s.run(&mut jit, 1000);
    assert_eq!(s.registers[reg::A0], 1);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_run_do_loop_inline_with_rep() {
    // DO #2 with body containing REP #3 inc A.
    // Each DO iteration: REP executes inc A 3 times.
    // Total: 2 * 3 = 6 increments.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2,$0003
    pram[1] = 0x000003; // la = $0003
    pram[2] = 0x0603A0; // rep #3
    pram[3] = 0x000008; // inc A (repeated)
    pram[4] = 0x0C0004; // jmp $0004 (halt)

    s.run(&mut jit, 1000);
    assert_eq!(s.registers[reg::A0], 6);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_run_do_loop_dor_inline() {
    // DOR #3 with relative loop end.
    // Encoding: 00000110_iiiiiiii_1001_hhhh
    // count=3: bits[15:8]=0x03, bits[3:0]=0x0 -> 0x060390
    // next_word=2 -> la = pc + next_word = 0 + 2 = 2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060390; // dor #3,offset
    pram[1] = 0x000002; // offset=2 -> la = 0+2 = 2
    pram[2] = 0x000008; // inc A
    pram[3] = 0x0C0003; // jmp $0003 (halt)

    s.run(&mut jit, 1000);
    assert_eq!(s.registers[reg::A0], 3);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_rep_3_inc_execute_one() {
    // REP #3 + inc A via step-by-step
    //
    // Step 1: REP #3 -> loop_rep=true, pc_on_rep=true, LC=3
    //         postexecute: pc_on_rep->false, PC->1
    // Step 2: inc A -> A=1
    //         postexecute: LC=2, pc_advance=0, PC stays at 1
    // Step 3: inc A -> A=2
    //         postexecute: LC=1, pc_advance=0, PC stays at 1
    // Step 4: inc A -> A=3
    //         postexecute: LC=0, loop_rep=false, LC=TEMP, PC->2
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::LC] = 0x99; // will be saved to TEMP
    pram[0] = 0x0603A0; // rep #3
    pram[1] = 0x000008; // inc A

    // Step 1: REP
    run_one(&mut s, &mut jit);
    assert!(s.loop_rep);
    assert_eq!(s.registers[reg::LC], 3);
    assert_eq!(s.registers[reg::TEMP], 0x99);
    assert_eq!(s.pc, 1);

    // Steps 2-4: inc A x 3
    run_one(&mut s, &mut jit); // inc A (iter 1)
    assert_eq!(s.registers[reg::A0], 1);
    assert_eq!(s.pc, 1); // stays on inc A
    assert!(s.loop_rep);

    run_one(&mut s, &mut jit); // inc A (iter 2)
    assert_eq!(s.registers[reg::A0], 2);
    assert_eq!(s.pc, 1);
    assert!(s.loop_rep);

    run_one(&mut s, &mut jit); // inc A (iter 3, last)
    assert_eq!(s.registers[reg::A0], 3);
    assert_eq!(s.pc, 2); // past repeated instruction
    assert!(!s.loop_rep);
    assert_eq!(s.registers[reg::LC], 0x99); // TEMP restored
}

#[test]
fn test_run_rep() {
    // REP #3 + inc A via run() - Cranelift inline loop
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::LC] = 0x42;
    pram[0] = 0x0603A0; // rep #3
    pram[1] = 0x000008; // inc A
    pram[2] = 0x0C0002; // jmp $0002 (loop to stop)

    s.run(&mut jit, 1000);
    assert_eq!(s.registers[reg::A0], 3);
    assert_eq!(s.registers[reg::LC], 0x42); // TEMP restored
}

#[test]
fn test_rep_lc_zero() {
    // REP #0 repeats 65,536 times (page 13-160)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0600A0; // rep #0
    pram[1] = 0x000000; // nop (repeated instruction)

    run_one(&mut s, &mut jit); // REP
    assert!(s.loop_rep);
    assert_eq!(s.registers[reg::LC], 0x10000); // 65536 iterations
}

#[test]
fn test_do_non_inlineable_body() {
    // DO loop with body containing MOVEP (triggers needs_exit_check),
    // which prevents the DO body from being inlined. The loop still
    // works correctly via the block-boundary path in run().
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

    // DO #2, $03 with body = [INC A, MOVEP X0,x:$FFFFC0]
    // The MOVEP makes the body non-inlineable.
    pram[0] = 0x060280; // DO #2
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x000008; // INC A
    pram[3] = 0x08C400; // MOVEP X0,x:$FFFFC0 (LA instruction)
    pram[4] = 0x000000; // NOP (after loop)

    s.run(&mut jit, 100);

    // Body = [2, 3], runs 2 times.
    // Each iteration: INC A + MOVEP. INC adds 1 to the full 56-bit
    // accumulator, so A0 (low 24 bits) gets incremented.
    // After 2 iterations: A0 = 2.
    assert_eq!(s.registers[reg::A0], 2);
    // Loop should have exited (LF clear)
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_do_ea() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // do x:(r0),p:$0020 -- loop count from memory
    pram[0] = 0x066000;
    pram[1] = 0x0020;
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 5;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::LC], 5);
    assert_eq!(s.registers[reg::LA], 0x000020);
    assert_eq!(s.registers[reg::SP], 2);
}

#[test]
fn test_rep_ea() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // rep x:(r0) -- repeat count from memory
    pram[0] = 0x066020;
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 8;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::LC], 8);
}

#[test]
fn test_enddo() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set up a DO loop: do #3,p:$0005
    pram[0] = 0x060380;
    pram[1] = 0x000005;
    pram[2] = 0x00008C; // enddo
    pram[3] = 0x000000; // nop (should execute after enddo)
    run_one(&mut s, &mut jit); // do #3
    assert_eq!(s.pc, 2);
    assert_eq!(s.registers[reg::SP], 2);

    run_one(&mut s, &mut jit); // enddo
    assert_eq!(s.pc, 3);
    assert_eq!(s.registers[reg::SP], 0); // loop stack popped
}

#[test]
fn test_do_inlined_body() {
    // DO #3 with inlineable body [INC A, NOP], compiled via run().
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060380; // DO #3
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x000008; // INC A
    pram[3] = 0x000000; // NOP (at LA)
    pram[4] = 0x0C0004; // JMP $4
    s.run(&mut jit, 50);
    assert_eq!(s.registers[reg::A0], 3);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_do_nested_inlined() {
    // Nested DO: outer DO #2 LA=5, inner DO #2 LA=4 with INC A body.
    // Outer body: [DO#2(inner), INC A, NOP]. Total INC = 2*2 = 4.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // DO #2 (outer)
    pram[1] = 0x0005; // outer LA = 5
    pram[2] = 0x060280; // DO #2 (inner)
    pram[3] = 0x0004; // inner LA = 4
    pram[4] = 0x000008; // INC A (inner body)
    pram[5] = 0x000000; // NOP (outer LA)
    pram[6] = 0x0C0006; // JMP $6
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 4);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_do_rep_in_body_inlined() {
    // DO #2 with REP #2 inside body. Body = [REP #2, INC A].
    // Each DO iter: REP 2 x INC = 2 increments. DO 2 iters = 4.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // DO #2
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x0602A0; // REP #2
    pram[3] = 0x000008; // INC A (repeated, at LA)
    pram[4] = 0x0C0004; // JMP $4
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 4);
}

#[test]
fn test_rep_standalone_in_block() {
    // REP #2 at block top level (not inside DO). Verifies REP inline works.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0602A0; // REP #2
    pram[1] = 0x000008; // INC A
    pram[2] = 0x0C0002; // JMP $2
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 2);
}

#[test]
fn test_do_rep1_in_body_inlined() {
    // DO #3 with REP #1 inside. Body = [REP #1, INC A].
    // Each DO iter: REP 1 x INC = 1 increment. DO 3 iters = 3.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060380; // DO #3
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x0601A0; // REP #1
    pram[3] = 0x000008; // INC A (repeated, at LA)
    pram[4] = 0x0C0004; // JMP $4
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_do_reg_inlined() {
    // DO X0 with inlineable body via run() -- covers emit_do_lc_value DoReg.
    // DO S encoding: 0000011011DDDDDD00000000, X0=reg 4.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x06C400; // DO X0
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x000008; // INC A
    pram[3] = 0x000000; // NOP (at LA)
    pram[4] = 0x0C0004; // JMP $4
    s.registers[reg::X0] = 3;
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_do_aa_inlined() {
    // DO X:$10 with inlineable body -- covers emit_do_lc_value DoAa.
    // DoAa encoding: 0000011000aaaaaa0S000000, addr=0x10, S=0 (X).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x061000; // DO X:$10
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x000008; // INC A
    pram[3] = 0x000000; // NOP (at LA)
    pram[4] = 0x0C0004; // JMP $4
    xram[0x10] = 3;
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_do_ea_inlined() {
    // DO X:(R0) with inlineable body -- covers emit_do_lc_value DoEa.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DoEa X:(R0): 0000011001MMMRRR0S000000
    // mode=100 (no update), RRR=000 (R0), S=0 (X space)
    // bits 15:8 = 01_100_000 = 0x60
    pram[0] = 0x066000; // DO X:(R0)
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x000008; // INC A
    pram[3] = 0x000000; // NOP (at LA)
    pram[4] = 0x0C0004; // JMP $4
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 3;
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_rep_reg_in_do_inlined() {
    // DO #2 with REP X0 inside body -- covers emit_rep_lc_value RepReg.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // DO #2
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x06C420; // REP X0
    pram[3] = 0x000008; // INC A (repeated, at LA)
    pram[4] = 0x0C0004; // JMP $4
    s.registers[reg::X0] = 3;
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 6); // 2 * 3
}

#[test]
fn test_rep_aa_in_do_inlined() {
    // DO #2 with REP X:$10 inside body -- covers emit_rep_lc_value RepAa.
    // RepAa encoding: 0000011000aaaaaa0S100000, addr=0x10, S=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // DO #2
    pram[1] = 0x0003; // LA = 3
    pram[2] = 0x061020; // REP X:$10
    pram[3] = 0x000008; // INC A (at LA)
    pram[4] = 0x0C0004; // JMP $4
    xram[0x10] = 3;
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 6); // 2 * 3
}

#[test]
fn test_rep_ea_standalone() {
    // REP X:(R1) -- covers emit_rep_lc_value RepEa path via emit_block.
    // RepEa: 0000011001MMMRRR0S100000, mode 4 R1, S=0 (X space)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x066120; // REP X:(R1)
    pram[1] = 0x000008; // INC A (repeated instruction)
    pram[2] = 0x0C0002; // JMP $2 (halt)
    s.registers[reg::R1] = 0x05;
    xram[0x05] = 3; // REP count = 3
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3);
}

#[test]
fn test_do_empty_body_not_inlined() {
    // DO with body_start > la. This falls through to non-inlined path.
    // DO #2,$0001 where LA=1, body_start=2, so body_start > la -> not inlineable
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // DO #2 (DoImm)
    pram[1] = 0x000001; // LA = 1, but body starts at PC=2 which is > 1
    pram[2] = 0x000000; // NOP
    pram[3] = 0x000000; // NOP
    s.run(&mut jit, 20);
    assert!(s.pc > 0);
}

#[test]
fn test_do_body_with_block_terminator() {
    // DO #1 with Jcc in body -> is_block_terminator returns true -> not inlineable
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DO #1,$0003 -- 1 iteration, LA=3, body=PC 2..3
    pram[0] = 0x060180;
    pram[1] = 0x0003;
    pram[2] = 0x000008; // INC A
    // Jcc CS,$0 (CCCC=1000, carry set -> false with default SR, block terminator)
    pram[3] = 0x0E8000;
    pram[4] = 0x0C0004; // JMP $4 (halt)
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 1);
}

#[test]
fn test_do_body_with_p_write() {
    // DO #1 with movem write in body -> writes_p_memory -> not inlineable
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DO #1,$0002
    pram[0] = 0x060180;
    pram[1] = 0x0002;
    pram[2] = 0x071004; // movem X0,P:$10 (writes P memory, at LA)
    pram[3] = 0x0C0003; // JMP $3 (halt)
    s.registers[reg::X0] = 0x123456;
    s.run(&mut jit, 100);
    assert_eq!(pram[0x10], 0x123456);
}

#[test]
fn test_do_body_misaligned() {
    // DO with 2-word instruction that doesn't align with LA boundary
    // body_pc != la + 1 -> return false (line 878)
    // MPYI is always 2 words: body_start=2, body_pc=2+2=4 != la+1=3
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060180; // DO #1
    pram[1] = 0x0002; // LA=2
    pram[2] = 0x0141C0; // MPYI +X0,#imm,A (2 words, misaligns with LA)
    pram[3] = 0x000003; // immediate = 3
    pram[4] = 0x0C0004; // JMP $4 (halt)
    s.registers[reg::X0] = 4;
    s.run(&mut jit, 100);
    // MPYI executes: A0 = (4 * 3) << 1 = 24
    assert_eq!(s.registers[reg::A0], 24);
}

#[test]
fn test_do_body_nested_inner_overflows() {
    // Nested DO where inner LA > outer LA -> not inlineable
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Outer: DO #1,$0005 (LA=5, body = PC 2..5)
    // Inner: DO #1,$0006 at PC=2 (inner LA=6 > outer LA=5 -> overflow)
    pram[0] = 0x060180; // DO #1
    pram[1] = 0x0005; // outer LA=5
    pram[2] = 0x060180; // inner DO #1
    pram[3] = 0x0006; // inner LA=6 (> outer LA=5!)
    pram[4] = 0x000008; // INC A
    pram[5] = 0x000000; // NOP (outer LA)
    pram[6] = 0x000000; // NOP (inner LA)
    pram[7] = 0x0C0007; // JMP $7 (halt)
    s.run(&mut jit, 200);
    assert_eq!(s.registers[reg::A0], 1);
}

#[test]
fn test_do_body_rep_unsafe_repeated() {
    // DO body contains REP + movem (writes P memory) as repeated instruction
    // is_do_body_inlineable detects unsafe REP target -> returns false (line 862)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DO #1,$0004 -- LA=4, body: [2,4]
    pram[0] = 0x060180;
    pram[1] = 0x0004;
    pram[2] = 0x000008; // INC A
    pram[3] = 0x060120; // REP #1
    pram[4] = 0x076084; // movem X0,P:(R0) -- writes P memory (repeated inst)
    pram[5] = 0x0C0005; // JMP $5 (halt)
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 1);
}

#[test]
fn test_do_body_rep_past_la() {
    // REP at the end of DO body where repeated inst extends past LA
    // is_do_body_inlineable: rep_next + rep_len > la + 1 -> false (line 866)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DO #1,$0003 -- LA=3, body: [2,3]
    pram[0] = 0x060180;
    pram[1] = 0x0003;
    pram[2] = 0x000008; // INC A
    pram[3] = 0x060120; // REP #1 (at la=3, rep_next=4, past la)
    pram[4] = 0x000008; // INC A (repeated)
    pram[5] = 0x0C0005; // JMP $5 (halt)
    s.run(&mut jit, 100);
    // Just verify it completes without panic
    assert!(s.registers[reg::A0] >= 1);
}

#[test]
fn test_do_body_count_gt_64() {
    // DO body with 65 NOP instructions -> count > MAX_INLINE_LEN(64) -> not inlined
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DO #1,$0042 -- LA=66, body: [2, 66] = 65 instructions
    pram[0] = 0x060180;
    pram[1] = 0x0042;
    // Fill body with 64 NOPs + 1 INC A at the end
    for slot in &mut pram[2..66] {
        *slot = 0x000000; // NOP
    }
    pram[66] = 0x000008; // INC A at LA
    pram[67] = 0x0C0043; // JMP $67 (halt)
    s.run(&mut jit, 200);
    assert_eq!(s.registers[reg::A0], 1);
}

#[test]
fn test_do_body_nested_inner_not_inlineable() {
    // Outer DO contains inner DO whose body has a block terminator
    // Recursive is_do_body_inlineable returns false -> outer not inlined (line 830)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Outer DO #1,$0005 -- LA=5, body: [2, 5]
    pram[0] = 0x060180;
    pram[1] = 0x0005;
    // Inner DO #1,$0004 -- LA=4, body: [4, 4]
    pram[2] = 0x060180;
    pram[3] = 0x0004;
    pram[4] = 0x0E8000; // Jcc CS,$0 (block terminator, not taken)
    pram[5] = 0x000008; // INC A (at outer LA)
    pram[6] = 0x0C0006; // JMP $6 (halt)
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 1);
}

#[test]
fn test_do_body_movep1_writes_p() {
    // DO body containing Movep1 with W=0 (write to P memory)
    // writes_p_memory check for Movep1 -> not inlineable (line 575)
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
    // DO #1,$0003 -- LA=3, body: [2, 3]
    pram[0] = 0x060180;
    pram[1] = 0x0003;
    pram[2] = 0x000008; // INC A
    // Movep1: 0000100sW1MMMRRR01pppppp
    // s=0, W=0 (write X:pp -> P:ea), MMMRRR=100000, pp=0
    pram[3] = 0x086040;
    pram[4] = 0x0C0004; // JMP $4 (halt)
    periph[0x40] = 0x000000; // X:$FFFFC0 source
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 1);
}

#[test]
fn test_brkcc_taken() {
    // brkcc (EQ): template 00000000000000100001CCCC
    // CCCC=A (EQ): 0000_0000_0000_0010_0001_1010 = 0x00021A
    // Need to be in a DO loop for BRKcc to work
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set up a DO loop: do #10,$0005
    pram[0] = 0x060A80; // do #10
    pram[1] = 0x000005; // LA=5
    pram[2] = 0x200013; // clr A (sets Z=1)
    pram[3] = 0x00021A; // brkcc EQ (should break, Z=1)
    pram[4] = 0x000000; // nop
    pram[5] = 0x000000; // nop (loop end)
    pram[6] = 0x0C0006; // jmp $6 (halt)
    s.run(&mut jit, 50);
    assert_eq!(s.pc, 6); // broke out of loop, reached halt
    // LC should have been modified (loop exited early)
}

#[test]
fn test_brkcc_not_taken() {
    // brkcc (NE, cc=8): CCCC=8
    // 0000_0000_0000_0010_0001_1000 = 0x000218
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2,$0003
    pram[1] = 0x000003;
    pram[2] = 0x200013; // clr A (sets Z=1)
    pram[3] = 0x000218; // brkcc NE (Z=1, NE is false -> not taken)
    pram[4] = 0x0C0004; // jmp $4
    s.run(&mut jit, 50);
    assert_eq!(s.pc, 4); // loop completed normally
}

#[test]
fn test_do_forever() {
    // do forever,$0003: opcode 000000000000001000000011 = 0x000203
    // Verify it sets up the loop stack (LF, FV, LA; LC preserved)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000203; // do forever
    pram[1] = 0x000010; // LA=0x10
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::LA], 0x10);
    assert_eq!(s.registers[reg::LC], 0); // DO FOREVER preserves LC
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF set
    assert_ne!(s.registers[reg::SR] & (1 << sr::FV), 0); // FV set
    assert_eq!(s.pc, 2);
}

#[test]
fn test_do_lc_zero_annulled() {
    // DO #0: loop body is not executed (DOR p.13-61)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060080; // do #0,$0003
    pram[1] = 0x000003; // LA=3
    pram[2] = 0x000008; // inc A (body - should be skipped)
    pram[3] = 0x000008; // inc A (body end at LA - should be skipped)
    pram[4] = 0x000000; // nop (after loop)

    run_one(&mut s, &mut jit); // DO #0 -> annulled (execute_one calls advance_pc)
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF cleared (annulled)
    assert_eq!(s.registers[reg::A0], 0); // body not executed
    assert_eq!(s.pc, 4); // skipped body entirely (PC = LA+1)
}

#[test]
fn test_run_do_lc_zero_annulled() {
    // Same as test_do_lc_zero_annulled but via run() (inline DO path)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060080; // do #0,$0003
    pram[1] = 0x000003; // LA=3
    pram[2] = 0x000008; // inc A (body - should be skipped)
    pram[3] = 0x000008; // inc A (body end at LA - should be skipped)
    pram[4] = 0x0C0004; // jmp $4 (halt)

    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 0); // body not executed
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF cleared
}

#[test]
fn test_dor_forever() {
    // dor forever,$xxxx: opcode 000000000000001000000010 = 0x000202
    // Like DO FOREVER but uses relative offset
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000202; // dor forever
    pram[1] = 0x000003; // relative offset -> LA = PC + offset = 0 + 3 = 3
    pram[2] = 0x000008; // inc A (loop body)
    pram[3] = 0x000213; // brkcc PL (inside loop at LA=3)

    // Execute DOR FOREVER
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2);
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF set
    assert_ne!(s.registers[reg::SR] & (1 << sr::FV), 0); // FV set
    assert_eq!(s.registers[reg::LA], 3);

    // Execute INC A (loop body)
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 3);

    // Execute BRKcc PL (PL=true since A=1 is positive)
    run_one(&mut s, &mut jit);
    // BRKcc breaks: pops loop stack, jumps to LA+1 = 4
    assert_eq!(s.pc, 4);
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0); // LF cleared
}

#[test]
fn test_dor_forever_block_path() {
    // Same as test_dor_forever but via s.run() (block compiler path).
    // Verifies BRKcc inside a DO FOREVER loop works with block compilation.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000202; // dor forever
    pram[1] = 0x000003; // relative offset -> LA = PC + offset = 0 + 3 = 3
    pram[2] = 0x000008; // inc A (loop body)
    pram[3] = 0x000213; // brkcc PL (inside loop at LA=3, PL=true when N=0)
    pram[4] = 0x0C0004; // jmp $4 (halt)
    s.run(&mut jit, 100);
    assert_eq!(
        s.pc, 4,
        "BRKcc should break out of DO FOREVER to LA+1, then JMP halts at $4"
    );
}

#[test]
fn test_dor_imm_loop() {
    // dor #3,$xxxx: template 00000110iiiiiiii1001hhhh
    // count=3 (i=3, h=0): 0000_0110_0000_0011_1001_0000 = 0x060390
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060390; // dor #3
    pram[1] = 0x000004; // LA = mask_pc(pc + next_word) = 0 + 4 = 4
    pram[2] = 0x000008; // inc A
    pram[3] = 0x000000; // nop
    pram[4] = 0x000000; // nop (loop end at LA)
    pram[5] = 0x0C0005; // jmp $5
    s.run(&mut jit, 100);
    assert_eq!(s.pc, 5);
    assert_eq!(s.registers[reg::A0], 3); // incremented 3 times
}

#[test]
fn test_dor_aa() {
    // DOR X:aa,expr - loop with count from X memory, PC-relative LA.
    // Set X:$00 = 3 (loop 3 times). Body increments A.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0] = 3; // loop count
    // DOR X:$00,$+3: loop body = next instruction (inc A), LA = PC + displacement
    // DorAa encoding: 0000011000aaaaaa0S010000 + next_word (displacement)
    // space=X (S=0), addr=$00 (aaaaaa=000000):
    // 0000_0110_0000_0000_0001_0000 = 0x060010
    // next_word = displacement so that LA = pc + disp
    // pc=0, body at pc=2, LA = 2 (last instr of body). disp = 2.
    pram[0] = 0x060010; // dor X:$00,$+2
    pram[1] = 0x000002; // displacement = 2 -> LA = 0 + 2 = 2
    pram[2] = 0x000008; // inc A: 00000000_00000000_00001000
    s.registers[reg::A2] = 0;
    s.registers[reg::A1] = 0;
    s.registers[reg::A0] = 0;
    s.registers[reg::SR] = 0xC00300;
    for _ in 0..20 {
        run_one(&mut s, &mut jit);
        if s.pc > 3 {
            break;
        }
    }
    // After 3 iterations of inc A, A should be 3
    assert_eq!(
        s.registers[reg::A0],
        3,
        "DOR X:aa should loop the correct number of times"
    );
}

#[test]
fn test_do_forever_does_not_exit_on_lc_zero() {
    // DO FOREVER with LC initially set to 2. After 2 iterations LC wraps to 0.
    // With FV set, the loop should NOT exit - it should keep looping.
    // Program: DO FOREVER, _end / INC A / NOP (_end) / NOP (after loop)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // DO FOREVER opcode=0x000203, LA extension word=end_of_loop_addr
    // Loop body: INC A (0x000008), NOP (0x000000)
    // DO FOREVER at PC=0, extension word at PC=1 = LA value.
    // Loop body: PC=2 (INC A) and PC=3 (NOP). LA=3, end-of-loop at PC=LA+1=4.
    pram[0] = 0x000203; // DO FOREVER
    pram[1] = 0x000003; // LA = 3 (last instruction of loop body)
    pram[2] = 0x000008; // INC A (at loop start, PC=2)
    pram[3] = 0x000000; // NOP (last instruction of loop, PC=3)
    pram[4] = 0x000000; // NOP (after loop, should not reach)
    s.registers[reg::LC] = 2; // LC will be saved and restored; loop uses its own counter
    // Execute DO FOREVER (sets up loop, pushes LA/LC, sets LF+FV)
    run_one(&mut s, &mut jit); // DO FOREVER
    assert_eq!(s.pc, 2);
    assert_ne!(s.registers[reg::SR] & (1 << sr::FV), 0, "FV should be set");
    // Now LC was pushed to stack. DO FOREVER doesn't set LC from the instruction,
    // it preserves existing LC. So LC = 2 initially.
    // Run loop body: INC A + NOP = 2 instructions, then end-of-loop check
    // Iteration 1: LC decrements from 2 to 1, loops back
    run_one(&mut s, &mut jit); // INC A (PC=2)
    run_one(&mut s, &mut jit); // NOP (PC=3) -> end of loop, LC: 2->1, loop back
    assert_eq!(s.pc, 2, "should loop back after first iteration");
    // Iteration 2: LC decrements from 1 to 0, but FV is set -> should NOT exit
    run_one(&mut s, &mut jit); // INC A (PC=2)
    run_one(&mut s, &mut jit); // NOP (PC=3) -> end of loop, LC: 1->0, FV set -> continue
    assert_eq!(
        s.pc, 2,
        "DO FOREVER should not exit when LC reaches 0 with FV set"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should still be set"
    );
}

#[test]
fn test_do_sp_loads_lc_with_sp_plus_one() {
    // DO SP,expr: Manual page 13-56 says LC = SP_before + 1
    // With SP=3 before DO, LC should be loaded with 4.
    // DO SP,expr opcode: 0x06FB00 (DDDDDD=111011=SP)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SP] = 3;
    pram[0] = 0x06FB00; // DO SP,expr
    pram[1] = 0x000005; // LA = 5
    pram[2] = 0x000000; // NOP (loop body start)
    pram[3] = 0x000000; // NOP
    pram[4] = 0x000000; // NOP
    pram[5] = 0x000000; // NOP (last instruction of loop)
    run_one(&mut s, &mut jit); // execute DO SP,expr
    // LC should be SP_before + 1 = 3 + 1 = 4
    assert_eq!(
        s.registers[reg::LC],
        4,
        "DO SP should load LC with SP+1 (3+1=4)"
    );
}

#[test]
fn test_nested_do_inside_do_forever_terminates() {
    // Regular DO nested inside DO FOREVER should still exit via LC=0.
    // The FV bit from DO FOREVER must be cleared when entering the inner DO.
    //
    // Program:
    //   DO FOREVER,$0009      ; outer loop (FV=1)
    //     INC A               ; increment A each outer iteration
    //     DO #2,$0007          ; inner loop (should clear FV, run 2 times)
    //       INC B             ; increment B each inner iteration
    //     [end inner loop at LA=$0007]
    //     NOP
    //   [end outer loop at LA=$0009]
    //
    // After 1 full outer iteration: A incremented once, B incremented twice.
    // We run enough steps for ~2 outer iterations and check B > 2 to verify
    // the inner loop terminates and the outer loop continues.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // DO FOREVER,$0009
    pram[0] = 0x000203; // do forever
    pram[1] = 0x000009; // LA=9
    // INC A
    pram[2] = 0x000008; // inc A
    // DO #2,$0007
    pram[3] = 0x060280; // do #2
    pram[4] = 0x000007; // LA=7
    // INC B
    pram[5] = 0x000009; // inc B
    // NOP (padding inside inner loop body to reach LA=7)
    pram[6] = 0x000000; // nop
    pram[7] = 0x000000; // nop (inner loop end, LA=7)
    // NOP (after inner loop, still inside outer)
    pram[8] = 0x000000; // nop
    pram[9] = 0x000000; // nop (outer loop end, LA=9)

    // Run enough instructions for 2+ outer iterations.
    // 1 outer iteration = DO FOREVER(2) + INC A(1) + DO #2(2) + inner_body*2(6) + NOP(1) + NOP(1)
    // ~13 instructions per outer iteration; run 30 to get through ~2 iterations.
    for _ in 0..30 {
        run_one(&mut s, &mut jit);
    }

    let a_val = (s.registers[reg::A0] as u64)
        | ((s.registers[reg::A1] as u64) << 24)
        | ((s.registers[reg::A2] as u64) << 48);
    let b_val = (s.registers[reg::B0] as u64)
        | ((s.registers[reg::B1] as u64) << 24)
        | ((s.registers[reg::B2] as u64) << 48);

    // If inner DO terminates correctly: A >= 2, B >= 4 (2 inner iterations per outer)
    // If inner DO never terminates (bug): B would be huge or A would be 0/1
    assert!(
        a_val >= 2,
        "outer loop should have iterated at least twice, A={a_val}"
    );
    assert!(
        b_val >= 4,
        "inner loop should have completed at least 4 total iterations, B={b_val}"
    );
    // B should be roughly 2x A (2 inner iterations per outer iteration)
    assert_eq!(
        b_val,
        a_val * 2,
        "inner loop should run exactly 2 iterations per outer iteration"
    );
}

#[test]
fn test_do_sp_inline_loads_lc_with_sp_plus_one() {
    // The inline DO path (emit_do_lc_value) was missing the SP+1
    // adjustment that the non-inline path (emit_do_reg) correctly applies.
    // Manual page 13-56: "For the DO SP, expr instruction, the actual value
    // that is loaded into the LC is the value of SP before the DO instruction
    // executes, incremented by one."
    //
    // Use s.run() to trigger block compilation which uses the inline path.
    // DO SP with SP=2: LC should be 3. Loop body increments X0 each iteration.
    // If LC=3, X0 should be 3 at end. If buggy (LC=2), X0 would be 2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SP] = 2;
    // DO SP,$003 - loop body is [PC+2, LA] = [2, 3]
    pram[0] = 0x06FB00; // DO SP,expr (DDDDDD=111011=SP)
    pram[1] = 0x000003; // LA = 3
    // Loop body: add #1,A (places 1 at A1 position) + nop
    pram[2] = 0x014180; // add #1,A
    pram[3] = 0x000000; // nop (last instruction = LA)
    pram[4] = 0x000000; // nop (after loop)
    // Give enough cycles: DO=6, body=2*3iterations=6, post-loop=1 => ~13+
    s.run(&mut jit, 50);
    // With SP=2, LC should be 3 (SP+1). Each iteration does add #1,A => A1=3.
    // If buggy (LC=2), A1 would be 2.
    assert_eq!(
        s.registers[reg::A1],
        3,
        "DO SP with SP=2 should loop 3 times (LC=SP+1=3), got A1={}",
        s.registers[reg::A1]
    );
}

#[test]
fn test_do_reg_lc_zero_annul_block_jit() {
    // DO with register LC=0 must skip loop body in block JIT (run()) path.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0; // LC = 0 -> annul
    pram[0] = 0x06C400; // do X0,$0003
    pram[1] = 0x000003; // LA=3
    pram[2] = 0x000008; // inc A (body - should be skipped)
    pram[3] = 0x000008; // inc A (body end at LA - should be skipped)
    pram[4] = 0x0C0004; // jmp $4 (halt)

    s.run(&mut jit, 100);
    assert_eq!(
        s.registers[reg::A0],
        0,
        "DO X0 with LC=0: loop body must not execute in block JIT path"
    );
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_do_reg_lc_zero_annul_step_one() {
    // Same via execute_one path.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0; // LC = 0 -> annul
    pram[0] = 0x06C400; // do X0,$0003
    pram[1] = 0x000003; // LA=3
    pram[2] = 0x000008; // inc A (body - should be skipped)
    pram[3] = 0x000008; // inc A (body end at LA - should be skipped)
    pram[4] = 0x000000; // nop (after loop)

    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::A0],
        0,
        "DO X0 with LC=0: body must not execute"
    );
    assert_eq!(s.pc, 4, "PC should be at LA+1 after annul");
    assert_eq!(s.registers[reg::SR] & (1 << sr::LF), 0);
}

#[test]
fn test_dor_ea_basic() {
    // DOR x:(r0),end - loop with count from X memory via EA, PC-relative LA.
    // DOR ea encoding: 0x066010 (x:(r0), no update) + extension word (displacement).
    // LA = PC_of_DOR + displacement. DOR at PC=0, displacement=4 -> LA=4.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 3; // loop count
    s.registers[reg::R0] = 0x000010;
    pram[0] = 0x066010; // dor x:(r0),end
    pram[1] = 0x000004; // displacement = 4 -> LA = 0 + 4 = 4
    pram[2] = 0x000008; // inc a (body)
    pram[3] = 0x000008; // inc a (body)
    pram[4] = 0x000008; // inc a (body, last instruction at LA)
    pram[5] = 0x000000; // nop (after loop)
    pram[6] = 0x0C0006; // jmp $6 (halt)
    s.run(&mut jit, 100);
    // 3 iterations * 3 increments = 9
    assert_eq!(
        s.registers[reg::A0],
        9,
        "DOR x:(r0) should loop 3 times over 3 inc instructions"
    );
    assert_eq!(s.pc, 6);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be clear after loop completes"
    );
}

#[test]
fn test_enddo_restores_fv_from_do_forever() {
    // ENDDO inside DO FOREVER must restore FV=0 from SSL.
    // DO FOREVER sets LF=1, FV=1. ENDDO pops stack, restoring pre-DO SR (LF=0, FV=0).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000203; // do forever
    pram[1] = 0x000003; // LA = 3
    pram[2] = 0x00008C; // enddo (exit immediately)
    pram[3] = 0x000000; // nop (at LA, never reached)
    pram[4] = 0x000000; // nop (after loop)

    run_one(&mut s, &mut jit); // do forever
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be set after DO FOREVER"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::FV),
        0,
        "FV must be set after DO FOREVER"
    );

    run_one(&mut s, &mut jit); // enddo
    assert_eq!(s.pc, 3, "PC should advance to instruction after ENDDO");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be restored to 0 after ENDDO"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::FV),
        0,
        "FV must be restored to 0 after ENDDO"
    );
}

#[test]
fn test_brkcc_restores_la_lc_lf_fv() {
    // BRKcc taken must restore all loop state from stack and jump to LA+1.
    // Use brkeq with Z pre-set via clr A.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060580; // do #5,end
    pram[1] = 0x000004; // LA = 4
    pram[2] = 0x200013; // clr a (sets Z=1, condition EQ true)
    pram[3] = 0x00021A; // brkeq (taken: Z=1)
    pram[4] = 0x000008; // inc a (at LA, should not execute)
    pram[5] = 0x000000; // nop (after loop, LA+1)
    pram[6] = 0x0C0006; // jmp $6 (halt)

    run_one(&mut s, &mut jit); // do #5 setup
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be set after DO"
    );
    assert_eq!(s.registers[reg::SP] & 0xF, 2, "SP must be 2 after DO setup");

    run_one(&mut s, &mut jit); // clr a
    run_one(&mut s, &mut jit); // brkeq (taken)
    assert_eq!(s.pc, 5, "BRKcc taken must jump to LA+1");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be restored to 0 after BRKcc"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::FV),
        0,
        "FV must be restored to 0 after BRKcc"
    );
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        0,
        "SP must be 0 after BRKcc pops loop stack"
    );
    assert_eq!(
        s.registers[reg::A0],
        0,
        "A must be 0 (only clr executed, no inc)"
    );
}

#[test]
fn test_rep_reg_lc_zero_65536() {
    // REP with register source = 0 must repeat 65536 times (per DSP56300FM).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0; // LC = 0 -> repeat 65536 times
    pram[0] = 0x06C420; // rep x0
    pram[1] = 0x000008; // inc a (repeated instruction)
    pram[2] = 0x000000; // nop (after rep)
    pram[3] = 0x0C0003; // jmp $3 (halt)
    s.run(&mut jit, 70000);
    // After 65536 increments: A = 0x010000
    assert_eq!(
        s.registers[reg::A0],
        0x010000,
        "REP with LC=0 must repeat 65536 times"
    );
    assert_eq!(
        s.registers[reg::A1],
        0,
        "A1 must be 0 (no overflow from A0)"
    );
    assert_eq!(s.pc, 3, "PC should be at halt after REP completes");
}

#[test]
fn test_do_nested_3_deep() {
    // Three nested DO loops: outer=2, middle=3, inner=4.
    // Inner body has 2 inc A instructions. Total = 2 * 3 * 4 * 2 = 48 increments.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2,_e1 (outer)
    pram[1] = 0x00000C; // LA1 = 12
    pram[2] = 0x060380; // do #3,_e2 (middle)
    pram[3] = 0x00000A; // LA2 = 10
    pram[4] = 0x060480; // do #4,_e3 (inner)
    pram[5] = 0x000008; // LA3 = 8
    pram[6] = 0x000008; // inc a (inner body)
    pram[7] = 0x000008; // inc a (inner body)
    pram[8] = 0x000000; // nop (at LA3, inner loop end)
    pram[9] = 0x000000; // nop (middle body after inner)
    pram[10] = 0x000000; // nop (at LA2, middle loop end)
    pram[11] = 0x000000; // nop (outer body after middle)
    pram[12] = 0x000000; // nop (at LA1, outer loop end)
    pram[13] = 0x0C000D; // jmp $13 (halt)
    s.run(&mut jit, 500);
    // 2 * 3 * 4 * 2 = 48 increments
    assert_eq!(
        s.registers[reg::A0],
        48,
        "Triple-nested DO loops: 2*3*4*2 = 48 increments"
    );
    assert_eq!(s.registers[reg::A1], 0);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be clear after all loops complete"
    );
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        0,
        "SP must be 0 after all loops complete"
    );
    assert_eq!(s.pc, 13, "PC should be at halt after all loops");
}

#[test]
fn test_do_imm_ccr_unchanged() {
    // DO instruction should not modify CCR bits (C, V, Z, N, U, E, L, S).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set all 8 CCR bits: C, V, Z, N, U, E, L, S
    let ccr_mask = (1 << sr::C)
        | (1 << sr::V)
        | (1 << sr::Z)
        | (1 << sr::N)
        | (1 << sr::U)
        | (1 << sr::E)
        | (1 << sr::L)
        | (1 << sr::S);
    s.registers[reg::SR] |= ccr_mask;
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x060380; // do #3,$0010
    pram[1] = 0x000010;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & ccr_mask,
        sr_before & ccr_mask,
        "DO should not modify any CCR bits"
    );
}

#[test]
fn test_enddo_ccr_unchanged() {
    // ENDDO should not modify CCR bits.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set up a DO loop
    pram[0] = 0x060380; // do #3,$0005
    pram[1] = 0x000005;
    run_one(&mut s, &mut jit); // execute DO
    // Now set all CCR bits
    let ccr_mask = (1 << sr::C)
        | (1 << sr::V)
        | (1 << sr::Z)
        | (1 << sr::N)
        | (1 << sr::U)
        | (1 << sr::E)
        | (1 << sr::L)
        | (1 << sr::S);
    s.registers[reg::SR] |= ccr_mask;
    let ccr_before = s.registers[reg::SR] & ccr_mask;
    pram[2] = 0x00008C; // enddo
    run_one(&mut s, &mut jit); // execute ENDDO
    assert_eq!(
        s.registers[reg::SR] & ccr_mask,
        ccr_before,
        "ENDDO should not modify any CCR bits"
    );
}

#[test]
fn test_rep_imm_ccr_unchanged() {
    // REP should not modify CCR bits.
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
    s.registers[reg::SR] |= ccr_mask;
    let ccr_before = s.registers[reg::SR] & ccr_mask;
    pram[0] = 0x0601A0; // rep #1
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & ccr_mask,
        ccr_before,
        "REP should not modify any CCR bits"
    );
}

#[test]
fn test_dor_reg_lc_zero_annul() {
    // DOR with LC=0 should annul the loop body (skip to LA+1).
    // dor X0,$offset: 0x06C410 + extension word
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0; // loop count = 0
    pram[0] = 0x06C410; // dor x0,_end
    pram[1] = 0x000004; // displacement = 4 -> LA = 0 + 4 = 4
    pram[2] = 0x000008; // inc a (body, should be skipped)
    pram[3] = 0x000008; // inc a (body, should be skipped)
    pram[4] = 0x000008; // inc a (body at LA, should be skipped)
    pram[5] = 0x000000; // nop (after loop)

    run_one(&mut s, &mut jit); // DOR with LC=0 -> annulled
    assert_eq!(
        s.registers[reg::A0],
        0,
        "loop body should not execute when LC=0"
    );
    assert_eq!(s.pc, 5, "PC should jump to LA+1 when LC=0");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should not be set for annulled loop"
    );
}

#[test]
fn test_dor_reg_full_loop() {
    // DOR with register operand, full loop execution via s.run().
    // X0 = 3 (loop count). 3 iterations x 2 inc = 6.
    // dor x0,_end: 0x06C410 + displacement word.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 3;
    pram[0] = 0x06C410; // dor x0,_end
    pram[1] = 0x000004; // displacement: LA = 0 + 4 = 4
    pram[2] = 0x000008; // inc a (body)
    pram[3] = 0x000008; // inc a (body)
    pram[4] = 0x000000; // nop (at LA, end of loop)
    pram[5] = 0x0C0005; // jmp $5 (halt after loop)
    s.run(&mut jit, 50);
    assert_eq!(s.registers[reg::A0], 6, "3 iterations x 2 inc = 6");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should be clear after loop completes"
    );
    assert_eq!(s.pc, 5, "PC should be at halt instruction after loop");
}

#[test]
fn test_dor_aa_lc_zero_annul() {
    // DOR aa with LC=0 from memory should annul (skip body).
    // DorAa encoding: 0000011000aaaaaa0S010000
    // X:$10 (S=0, aaaaaa=010000): 0x061010
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0; // loop count = 0
    pram[0] = 0x061010; // dor X:$10,_end
    pram[1] = 0x000004; // displacement: LA = 0 + 4 = 4
    pram[2] = 0x000008; // inc a (body - should be skipped)
    pram[3] = 0x000008; // inc a (body - skipped)
    pram[4] = 0x000000; // nop (at LA - skipped)
    pram[5] = 0x000000; // nop (after loop)
    run_one(&mut s, &mut jit); // DOR with LC=0 -> annulled
    assert_eq!(s.registers[reg::A0], 0, "body should not execute when LC=0");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should not be set for annulled loop"
    );
    assert_eq!(s.pc, 5, "PC should jump to LA+1 when LC=0");
}

#[test]
fn test_do_stack_contents_verify() {
    // DSP56300FM p.13-56: DO pushes (old_LA, old_LC) then (PC, SR) onto the system stack.
    // After DO: SP=2.
    //   stack[0][1] = old LA (saved before DO overwrites LA)
    //   stack[1][1] = old LC (saved before DO overwrites LC)
    //   stack[0][2] = PC of first loop body instruction
    //   stack[1][2] = SR before DO modified it
    // do #3,$0003: opcode 0x060380, extension word LA=3.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set distinguishable old LA/LC values so we can verify they're saved
    s.registers[reg::LA] = 0x42;
    s.registers[reg::LC] = 0x99;
    let sr_before = s.registers[reg::SR];
    pram[0] = 0x060380; // do #3,$3
    pram[1] = 0x000003; // LA = 3
    pram[2] = 0x000000; // nop (body)
    pram[3] = 0x000000; // nop (at LA)
    run_one(&mut s, &mut jit); // execute DO setup
    // SP should be 2 (two pushes)
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        2,
        "SP should be 2 after DO setup"
    );
    // First push saves old LA and old LC at stack level 1
    assert_eq!(s.stack[0][1], 0x42, "SSH[1] should contain old LA (0x42)");
    assert_eq!(s.stack[1][1], 0x99, "SSL[1] should contain old LC (0x99)");
    // Second push saves PC and SR at stack level 2
    // PC pushed = first instruction of loop body = 2 (after 2-word DO instruction)
    assert_eq!(
        s.stack[0][2], 2,
        "SSH[2] should contain loop body start PC (2)"
    );
    // SSL[2] should contain original SR value (before DO set LF)
    assert_eq!(
        s.stack[1][2], sr_before,
        "SSL[2] should contain SR value from before DO"
    );
    // Verify DO set the new LA and LC correctly
    assert_eq!(s.registers[reg::LA], 3, "LA should be set to 3 by DO");
    assert_eq!(s.registers[reg::LC], 3, "LC should be set to 3 by DO");
}

#[test]
fn test_do_aa_y_space() {
    // DO Y:$10,_end - loop count from Y-space absolute address (p.13-56).
    // Template: 0000011000aaaaaa0S000000, aaaaaa=0x10, S=1 (Y space).
    // 0000_0110_0001_0000_0100_0000 = 0x061040
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x10] = 3; // loop count
    // DO at PC=0, ext at PC=1, body at PC=2, LA=2 (last instruction of loop body).
    pram[0] = 0x061040; // do y:$10,_end
    pram[1] = 0x000002; // LA = 2 (extension word: absolute address of last loop instr)
    pram[2] = 0x000008; // inc A (loop body = single instruction at LA)
    pram[3] = 0x0C0003; // jmp $3 (halt after loop)
    s.run(&mut jit, 50);
    // Loop body (inc A) executes 3 times.
    assert_eq!(
        s.registers[reg::A0],
        3,
        "A0 should be 3 after 3 iterations of inc A"
    );
}

#[test]
fn test_do_ea_post_increment() {
    // DO X:(R0)+,_end - loop count from memory with post-increment EA (p.13-56).
    // Template: 0000011001MMMRRR0S000000, MMM=011 ((R0)+), RRR=000 (R0), S=0 (X).
    // 0000_0110_0101_1000_0000_0000 = 0x065800
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x20] = 2; // loop count
    s.registers[reg::R0] = 0x20;
    s.registers[reg::M0] = 0xFFFFFF; // linear modifier
    pram[0] = 0x065800; // do x:(r0)+,_end
    pram[1] = 0x000002; // LA = 2
    pram[2] = 0x000008; // inc A (loop body)
    pram[3] = 0x0C0003; // jmp $3 (halt)
    s.run(&mut jit, 50);
    assert_eq!(
        s.registers[reg::A0],
        2,
        "A0 should be 2 after 2 iterations of inc A"
    );
    assert_eq!(
        s.registers[reg::R0],
        0x21,
        "R0 should be post-incremented from 0x20 to 0x21"
    );
}

#[test]
fn test_do_reg_accumulator_source() {
    // DO A,_end - loop count from accumulator A register (p.13-56).
    // Template: 0000011011DDDDDD00000000, DDDDDD = A = 0x0E = 001110.
    // 0000_0110_1100_1110_0000_0000 = 0x06CE00
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A1 holds the loop count for DO reg; A2 and A0 don't matter for the count.
    s.registers[reg::A1] = 4;
    s.registers[reg::A0] = 0;
    s.registers[reg::A2] = 0;
    pram[0] = 0x06CE00; // do A,_end
    pram[1] = 0x000002; // LA = 2
    run_one(&mut s, &mut jit); // execute DO setup
    assert_eq!(
        s.registers[reg::LC],
        4,
        "LC should be set to 4 from accumulator A"
    );
    assert_eq!(s.registers[reg::LA], 2, "LA should be 2");
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0, "LF should be set");
    assert_eq!(s.pc, 2);
}

#[test]
fn test_brkcc_multiple_conditions() {
    // BRKcc with GE and LT conditions inside a DO loop.
    // brkge: CCCC=0001, opcode = 0x000211 (GE = !(N XOR V))
    // brklt: CCCC=1001, opcode = 0x000219 (LT = N XOR V)
    //
    // Sub-test 1: BRKGE taken (N=0, V=0 -> GE=1).
    {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        pram[0] = 0x060A80; // do #10,$0004
        pram[1] = 0x000004; // LA=4
        pram[2] = 0x000008; // inc A
        pram[3] = 0x000211; // brkge (N=0, V=0 -> taken on first iteration)
        pram[4] = 0x000000; // nop (loop end)
        pram[5] = 0x0C0005; // jmp $5 (halt)
        s.run(&mut jit, 50);
        assert_eq!(s.pc, 5, "BRKGE should break out of loop");
        // Only one inc A executes before brkge breaks
        assert_eq!(s.registers[reg::A0], 1, "only 1 inc A before brkge");
    }

    // Sub-test 2: BRKLT taken (N=1, V=0 -> LT=1).
    {
        let mut jit = JitEngine::new(PRAM_SIZE);
        let mut xram = [0u32; XRAM_SIZE];
        let mut yram = [0u32; YRAM_SIZE];
        let mut pram = [0u32; PRAM_SIZE];
        let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
        pram[0] = 0x060A80; // do #10,$0004
        pram[1] = 0x000004; // LA=4
        // Use "dec A" to set N=1 (A goes from 0 to -1, negative).
        // dec A = 0x00000A
        pram[2] = 0x00000A; // dec A -> A=-1, N=1
        pram[3] = 0x000219; // brklt (N=1, V=0 -> LT=1, taken)
        pram[4] = 0x000000; // nop (loop end)
        pram[5] = 0x0C0005; // jmp $5 (halt)
        s.run(&mut jit, 50);
        assert_eq!(s.pc, 5, "BRKLT should break out of loop");
    }
}

#[test]
fn test_do_nested_7_deep() {
    // DSP56300FM Section 5.4.3: hardware stack is 15 entries.
    // Each DO pushes 2 entries (SSH=PC, SSL=SR then SSH=LA, SSL=LC).
    // 7 nested DO #2 loops = 14 stack entries, which fits within the 15-entry limit.
    // Total inc A executions = 2^7 = 128.
    //
    // Layout: 7 nested DO #2 with decreasing LA, innermost body is inc a.
    //   PC=0:  do #2, LA=26   (level 1, body = 2..26)
    //   PC=2:  do #2, LA=24   (level 2, body = 4..24)
    //   PC=4:  do #2, LA=22   (level 3, body = 6..22)
    //   PC=6:  do #2, LA=20   (level 4, body = 8..20)
    //   PC=8:  do #2, LA=18   (level 5, body = 10..18)
    //   PC=10: do #2, LA=16   (level 6, body = 12..16)
    //   PC=12: do #2, LA=14   (level 7, body = 14..14)
    //   PC=14: inc a           (innermost body, also LA7)
    //   PC=15: nop
    //   PC=16: nop             (LA6)
    //   PC=17: nop
    //   PC=18: nop             (LA5)
    //   PC=19: nop
    //   PC=20: nop             (LA4)
    //   PC=21: nop
    //   PC=22: nop             (LA3)
    //   PC=23: nop
    //   PC=24: nop             (LA2)
    //   PC=25: nop
    //   PC=26: nop             (LA1)
    //   PC=27: jmp $27         (halt)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // do #2 opcode = 0x060280
    pram[0] = 0x060280;
    pram[1] = 26; // L1: do #2, LA=26
    pram[2] = 0x060280;
    pram[3] = 24; // L2: do #2, LA=24
    pram[4] = 0x060280;
    pram[5] = 22; // L3: do #2, LA=22
    pram[6] = 0x060280;
    pram[7] = 20; // L4: do #2, LA=20
    pram[8] = 0x060280;
    pram[9] = 18; // L5: do #2, LA=18
    pram[10] = 0x060280;
    pram[11] = 16; // L6: do #2, LA=16
    pram[12] = 0x060280;
    pram[13] = 14; // L7: do #2, LA=14
    pram[14] = 0x000008; // inc a (innermost body)
    // nops from 15..26
    pram[15..=26].fill(0x000000);
    pram[27] = 0x0C001B; // jmp $27 (halt)

    s.run(&mut jit, 5000);

    assert_eq!(
        s.registers[reg::A0],
        128,
        "7 nested DO #2 loops: 2^7 = 128 inc a"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be clear after all loops"
    );
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        0,
        "SP must be 0 after all loops complete"
    );
    assert_eq!(s.pc, 27, "PC should be at halt");
}

#[test]
fn test_do_nested_8_deep_stack_overflow() {
    // DSP56300FM Section 5.4.3.1: stack overflow occurs when SP reaches 16 (wraps to 0).
    // 8 nested DO #2 = 16 stack entries needed, exceeding the 15-entry hardware stack.
    // ARCHITECTURE-NOTES.md: "SP wraps to 0 (P[3:0] = 0000) with SE bit set."
    // SE is bit 4 of SP register.
    // We verify the emulator doesn't panic and SE is set.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // 8 nested DO #2 with decreasing LA
    pram[0] = 0x060280;
    pram[1] = 30; // L1: do #2, LA=30
    pram[2] = 0x060280;
    pram[3] = 28; // L2: do #2, LA=28
    pram[4] = 0x060280;
    pram[5] = 26; // L3: do #2, LA=26
    pram[6] = 0x060280;
    pram[7] = 24; // L4: do #2, LA=24
    pram[8] = 0x060280;
    pram[9] = 22; // L5: do #2, LA=22
    pram[10] = 0x060280;
    pram[11] = 20; // L6: do #2, LA=20
    pram[12] = 0x060280;
    pram[13] = 18; // L7: do #2, LA=18
    pram[14] = 0x060280;
    pram[15] = 16; // L8: do #2, LA=16
    pram[16] = 0x000008; // inc a (innermost body)
    pram[17..=30].fill(0x000000); // nops
    pram[31] = 0x0C001F; // jmp $31 (halt)

    // Run enough cycles to push all 8 DOs. The 8th DO overflows the stack.
    // Use run_one to step through the 8 DO instructions so we can check SE.
    for _ in 0..8 {
        run_one(&mut s, &mut jit);
    }
    // After 8 DO pushes (16 stack entries), SE bit must be set.
    // SE is bit 4 of SP register (Section 5.4.3.1, Table 5-2).
    assert_ne!(
        s.registers[reg::SP] & (1 << 4),
        0,
        "SE bit must be set after 8 nested DOs overflow the 15-entry stack"
    );
}

#[test]
fn test_enddo_outside_loop() {
    // ENDDO (0x00008C) executed when LF=0 (no active hardware loop).
    // The manual does not specify behavior for ENDDO outside a loop.
    // DSP56300FM Section 13.3 (ENDDO): precondition is LF=1.
    // Verify the emulator doesn't panic and PC advances past the instruction.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be clear initially"
    );
    pram[0] = 0x00008C; // enddo
    pram[1] = 0x000000; // nop

    run_one(&mut s, &mut jit);
    // Must not panic. PC should advance.
    assert_eq!(s.pc, 1, "ENDDO outside loop should advance PC to 1");
}

#[test]
fn test_brkcc_outside_loop() {
    // BRKcc (0x000210 = BRKPL, always-true variant) executed when LF=0.
    // The manual does not specify behavior for BRKcc outside a loop.
    // DSP56300FM Section 13.3 (BRKcc): operation pops loop stack if condition true and LF=1.
    // Verify the emulator doesn't panic and PC advances past the instruction.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be clear initially"
    );
    pram[0] = 0x000210; // brkcc (BRKPL: always true when N=0)
    pram[1] = 0x000000; // nop

    run_one(&mut s, &mut jit);
    // Must not panic. PC should advance.
    assert_eq!(s.pc, 1, "BRKcc outside loop should advance PC to 1");
}

#[test]
fn test_do_ssl_source_reads_current_ssl() {
    // DSP56300FM p.13-56: DO with a register source reads that register
    // to determine the loop count. When SSL is the source, the current
    // SSL value (visible at the current SP) is used as the count.
    //
    // Setup: pre-load the stack so SSL=7 (a known value), then execute
    // DO SSL,LA which should iterate 7 times.
    //
    // do SSL register encoding: 0000011011DDDDDD00000000
    // SSL = reg 0x3D = 111101, DDDDDD=111101
    // 00000110_11111101_00000000 = 0x06FD00
    //
    // Layout:
    //   PC=0: do SSL, LA=2   (loop count = current SSL = 7)
    //   PC=2: inc a           (body, also LA)
    //   PC=3: jmp $3          (halt)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Pre-load stack so SSL=7. stack_push sets SP=1, SSH=ssh_val, SSL=ssl_val.
    s.stack_push(0, 7); // SSH=0 (don't care), SSL=7
    assert_eq!(s.registers[reg::SSL], 7, "SSL should be 7 after stack_push");

    pram[0] = 0x06FD00; // do SSL, LA=2
    pram[1] = 0x000002; // LA = 2
    pram[2] = 0x000008; // inc a (body)
    pram[3] = 0x0C0003; // jmp $3 (halt)

    s.run(&mut jit, 200);

    assert_eq!(
        s.registers[reg::A0],
        7,
        "DO SSL should iterate 7 times (SSL was 7)"
    );
    // After loop completes, SP should have the pre-push entry plus DO's own pops resolved.
    // DO pushes 2 entries (SP goes from 1 to 3), loop completion pops 2 (SP back to 1).
    // The original stack_push entry remains.
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should be 1 (original push remains)"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF must be clear after loop"
    );
    assert_eq!(s.pc, 3, "PC should be at halt");
}

#[test]
fn test_brkcc_ccr_unchanged() {
    // DSP56300FM p.13-27: BRKcc does not modify CCR.
    // Set up a DO loop with known CCR flags (C=1, V=1, N=1, Z=0).
    // Place brkcc (CC condition = carry clear). Since C=1, CC is false -> NOT taken.
    // Verify all CCR bits remain unchanged after execution.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Pre-set CCR flags: C=1, V=1, N=1, Z=0
    s.registers[reg::SR] |= (1 << sr::C) | (1 << sr::V) | (1 << sr::N);
    // Ensure Z is clear
    s.registers[reg::SR] &= !(1 << sr::Z);

    // DO #2 loop containing brkcc + nop
    pram[0] = 0x060280; // do #2
    pram[1] = 0x000003; // LA=3
    pram[2] = 0x000210; // brkcc (CC = carry clear; C=1 so condition is false -> not taken)
    pram[3] = 0x000000; // nop (at LA)
    pram[4] = 0x0C0004; // jmp $4 (halt)

    s.run(&mut jit, 200);

    // After loop completes (2 iterations, brkcc never taken), check CCR flags
    let sr_val = s.registers[reg::SR];
    assert_ne!(sr_val & (1 << sr::C), 0, "C should still be 1");
    assert_ne!(sr_val & (1 << sr::V), 0, "V should still be 1");
    assert_ne!(sr_val & (1 << sr::N), 0, "N should still be 1");
    assert_eq!(sr_val & (1 << sr::Z), 0, "Z should still be 0");
    assert_eq!(s.pc, 4, "PC should be at halt");
}

#[test]
fn test_dor_aa_block_jit() {
    // DSP56300FM p.13-61: DOR aa via s.run() (block JIT path).
    // DorAa encoding: 0000011000aaaaaa0S010000 + displacement.
    // X:$00 = 4 (loop 4 times). Body increments A. LA = PC + disp.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0] = 4; // loop count
    pram[0] = 0x060010; // dor X:$00,end
    pram[1] = 0x000002; // displacement = 2 -> LA = 0 + 2 = 2
    pram[2] = 0x000008; // inc A (body at LA)
    pram[3] = 0x0C0003; // jmp $3 (halt)
    s.run(&mut jit, 100);
    assert_eq!(
        s.registers[reg::A0],
        4,
        "DOR X:aa should loop 4 times via block JIT"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should be clear after loop"
    );
    assert_eq!(s.pc, 3, "PC should be at halt");
}

#[test]
fn test_dor_ea_execute_one() {
    // DSP56300FM p.13-61: DOR ea via run_one (step-by-step path).
    // DOR x:(r0),end. Encoding: 0x066010 + displacement.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x20] = 2; // loop count
    s.registers[reg::R0] = 0x20;
    pram[0] = 0x066010; // dor x:(r0),end (mode 4, no increment)
    pram[1] = 0x000002; // displacement = 2 -> LA = 0 + 2 = 2
    pram[2] = 0x000008; // inc A (body at LA)
    pram[3] = 0x000000; // nop (after loop)

    // Step through: DOR setup, then loop body iterations
    run_one(&mut s, &mut jit); // DOR: setup loop
    assert_eq!(s.registers[reg::LC], 2, "LC should be 2");
    assert_eq!(s.registers[reg::LA], 2, "LA should be 2 (PC + disp)");
    assert_ne!(s.registers[reg::SR] & (1 << sr::LF), 0, "LF should be set");
    assert_eq!(s.pc, 2, "PC should advance to body start");

    // Iteration 1: INC A at LA, loop back
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 1);

    // Iteration 2: INC A at LA, LC->0, exit loop
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 2);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should be clear after loop exit"
    );
    assert_eq!(s.pc, 3, "PC should be at instruction after loop");
}

#[test]
fn test_do_aa_lc_zero_annul() {
    // DSP56300FM p.13-56: DO aa - when LC from memory is 0, annul (skip body).
    // Template: 0000011000aaaaaa0S000000, X:$10 (aaaaaa=0x10, S=0).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0; // loop count = 0
    pram[0] = 0x061000; // do X:$10,_end
    pram[1] = 0x000004; // LA = 4
    pram[2] = 0x000008; // inc A (body - should be skipped)
    pram[3] = 0x000008; // inc A (body - skipped)
    pram[4] = 0x000000; // nop (at LA - skipped)
    pram[5] = 0x000000; // nop (after loop)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0, "body should not execute when LC=0");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should not be set for annulled loop"
    );
    assert_eq!(s.pc, 5, "PC should jump to LA+1 when LC=0");
}

#[test]
fn test_do_ea_lc_zero_annul() {
    // DSP56300FM p.13-56: DO ea - when LC from memory is 0, annul.
    // DO x:(r0),_end with memory value 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0;
    s.registers[reg::R0] = 0x10;
    pram[0] = 0x066000; // do x:(r0),_end
    pram[1] = 0x000003; // LA = 3
    pram[2] = 0x000008; // inc A (skipped)
    pram[3] = 0x000000; // nop (at LA, skipped)
    pram[4] = 0x000000; // nop (after loop)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0, "body should not execute when LC=0");
    assert_eq!(s.pc, 4, "PC should jump to LA+1 when LC=0");
}

#[test]
fn test_do_ea_y_space() {
    // DSP56300FM p.13-56: DO y:(r0),_end - Y-space EA form.
    // Template: 0000011001MMMRRR0S000000, MMM=100 (mode 4: (R0)), RRR=000, S=1.
    // 0000_0110_0110_0000_0100_0000 = 0x066040
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x10] = 3;
    s.registers[reg::R0] = 0x10;
    pram[0] = 0x066040; // do y:(r0),_end
    pram[1] = 0x000003; // LA = 3
    pram[2] = 0x000008; // inc A
    pram[3] = 0x000000; // nop (at LA)
    pram[4] = 0x0C0004; // jmp $4 (halt)
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3, "DO y:(r0) should loop 3 times");
}

#[test]
fn test_do_forever_run_path() {
    // DSP56300FM p.13-59: DO FOREVER via run() with BRKcc to exit.
    // DO FOREVER, inc A x 3 iterations, then BRKcc (BRKPL, always true when N=0).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set a register to count iterations, break after 3
    s.registers[reg::X0] = 3; // threshold
    pram[0] = 0x000203; // do forever
    pram[1] = 0x000004; // LA = 4
    pram[2] = 0x014180; // add #1,a (adds 1 to A1)
    // cmp x0,a  at PC=3. opcode: 0x200045 (cmp x0,a)
    pram[3] = 0x200045; // cmp x0,a
    pram[4] = 0x00021A; // brkeq (exit when A==X0, i.e., after 3 increments)
    pram[5] = 0x0C0005; // jmp $5 (halt)
    s.run(&mut jit, 200);
    assert_eq!(
        s.registers[reg::A1],
        3,
        "DO FOREVER should execute 3 iterations before BRKcc"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should be clear after loop exit"
    );
    assert_eq!(s.pc, 5, "PC should be at halt");
}

#[test]
fn test_dor_imm_lc_zero_annul() {
    // DSP56300FM p.13-61: DOR imm - when LC=0, annul.
    // DOR #0,$+4. Encoding: 00000110iiiiiiii1001hhhh.
    // count=0: iiiiiiii=0, hhhh=0 -> 0x060090.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060090; // dor #0,end
    pram[1] = 0x000004; // displacement = 4 -> LA = 0 + 4 = 4
    pram[2] = 0x000008; // inc A (skipped)
    pram[3] = 0x000008; // inc A (skipped)
    pram[4] = 0x000000; // nop (at LA, skipped)
    pram[5] = 0x000000; // nop (after loop)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0, "body should not execute when LC=0");
    assert_eq!(s.pc, 5, "PC should jump to LA+1 when LC=0");
}

#[test]
fn test_dor_sp_loads_lc_with_sp_plus_one() {
    // DSP56300FM p.13-61: DOR SP,expr - LC = SP_before + 1.
    // DOR reg encoding: 0000011011DDDDDD00010000, DDDDDD=111011 (SP).
    // 0x06FB10
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SP] = 2;
    pram[0] = 0x06FB10; // dor SP,end
    pram[1] = 0x000002; // displacement = 2 -> LA = 0 + 2 = 2
    pram[2] = 0x000008; // inc A (body at LA)
    pram[3] = 0x0C0003; // jmp $3 (halt)
    s.run(&mut jit, 200);
    // LC should have been SP+1 = 3, so 3 iterations
    assert_eq!(
        s.registers[reg::A0],
        3,
        "DOR SP should iterate SP+1 = 3 times"
    );
}

#[test]
fn test_dor_reg_lc_zero_annul_run() {
    // DSP56300FM p.13-61: DOR reg - LC=0 annul via run().
    // Use X0=0 as loop count.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0; // LC = 0
    pram[0] = 0x06C410; // dor X0,end
    pram[1] = 0x000004; // displacement = 4 -> LA = 0 + 4 = 4
    pram[2] = 0x000008; // inc A (skipped)
    pram[3] = 0x000008; // inc A (skipped)
    pram[4] = 0x000000; // nop (at LA, skipped)
    pram[5] = 0x0C0005; // jmp $5 (halt)
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 0, "body should not execute when LC=0");
    assert_eq!(s.pc, 5, "PC should be at halt (after annulled loop)");
}

#[test]
fn test_dor_ea_lc_zero_annul() {
    // DSP56300FM p.13-61: DOR ea - LC=0 annul.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0;
    s.registers[reg::R0] = 0x10;
    pram[0] = 0x066010; // dor x:(r0),end
    pram[1] = 0x000003; // displacement = 3 -> LA = 0 + 3 = 3
    pram[2] = 0x000008; // inc A (skipped)
    pram[3] = 0x000000; // nop (at LA, skipped)
    pram[4] = 0x000000; // nop (after loop)
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A0], 0, "body should not execute when LC=0");
    assert_eq!(s.pc, 4, "PC should jump to LA+1 when LC=0");
}

#[test]
fn test_rep_aa_y_space() {
    // DSP56300FM p.13-160: REP Y:aa - repeat count from Y-space.
    // Template: 0000011000aaaaaa0S100000, aaaaaa=0x10, S=1 (Y).
    // 0000_0110_0001_0000_0110_0000 = 0x061060
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x10] = 3;
    pram[0] = 0x061060; // rep Y:$10
    pram[1] = 0x000008; // inc A (repeated instruction)
    pram[2] = 0x0C0002; // jmp $2 (halt)
    s.run(&mut jit, 100);
    assert_eq!(s.registers[reg::A0], 3, "REP Y:aa should repeat 3 times");
}

#[test]
fn test_rep_ea_y_space_post_increment() {
    // DSP56300FM p.13-160: REP y:(r0)+ - Y-space EA with post-increment.
    // Template: 0000011001MMMRRR0S100000, MMM=011 ((R0)+), RRR=000, S=1 (Y).
    // 0000_0110_0101_1000_0110_0000 = 0x065860
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x20] = 5;
    s.registers[reg::R0] = 0x20;
    pram[0] = 0x065860; // rep y:(r0)+
    pram[1] = 0x000008; // inc A (repeated instruction)
    pram[2] = 0x0C0002; // jmp $2 (halt)
    s.run(&mut jit, 200);
    assert_eq!(s.registers[reg::A0], 5, "REP y:(r0)+ should repeat 5 times");
    assert_eq!(s.registers[reg::R0], 0x21, "R0 should be post-incremented");
}

#[test]
fn test_enddo_direct_run_path() {
    // ENDDO inside a DO loop via run() - verifies the block JIT handles ENDDO correctly.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060580; // do #5,_end (LA=5)
    pram[1] = 0x000005;
    pram[2] = 0x000008; // inc A
    pram[3] = 0x00008C; // enddo (exit immediately on first iteration)
    pram[4] = 0x000000; // nop
    pram[5] = 0x000000; // nop (at LA)
    pram[6] = 0x0C0006; // jmp $6 (halt)
    s.run(&mut jit, 100);
    // ENDDO exits after first INC, so A = 1
    assert_eq!(s.registers[reg::A0], 1, "ENDDO should exit after first INC");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF should be clear after ENDDO"
    );
}

#[test]
fn test_rep_ssh_source_pop() {
    // DSP56300FM p.13-160: REP with SSH source should pop stack (SP decremented).
    // rep SSH: template 0000011011dddddd00100000, d=SSH(0x3C)
    // 0000_0110_1111_1100_0010_0000 = 0x06FC20
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push two values onto stack so SP=2
    s.stack_push(0x000003, 0); // SP=1, SSH[1]=3
    s.stack_push(0x000005, 0); // SP=2, SSH[2]=5
    let sp_before = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_before, 2);
    pram[0] = 0x06FC20; // rep SSH (reads SSH, which pops -> LC=5, SP=1)
    pram[1] = 0x000008; // inc A (repeated instruction)
    run_one(&mut s, &mut jit);
    // SSH read should pop: SP decrements from 2 to 1
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should decrement (SSH pop on REP source read)"
    );
    // LC should have been loaded with the popped SSH value (5)
    assert_eq!(
        s.registers[reg::LC],
        5,
        "LC should be loaded from SSH value"
    );
}

#[test]
fn test_enddo_does_not_restore_ccr_or_ipl() {
    // ENDDO should only restore LF+FV from stacked SR, NOT I1:I0 or CCR bits.
    // Per DSP56300FM p.13-67: "Note that LF is the only bit in the SR that is restored"
    // (plus FV per ARCHITECTURE-NOTES.md errata).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // DO #2,$0004: loop from pc=2 to pc=4
    pram[0] = 0x060280; // do #2,$0004
    pram[1] = 0x000004; // loop end address
    pram[2] = 0x000000; // nop (loop body)
    pram[3] = 0x000000; // nop
    pram[4] = 0x000000; // nop (LA)

    run_one(&mut s, &mut jit); // DO: pushes SR to SSL, sets LF=1

    // After DO, modify I1:I0 and C flag in current SR
    // The stacked SR has the pre-DO values. We change the live SR so that
    // when ENDDO restores LF+FV, it should NOT touch I1:I0 or C.
    s.registers[reg::SR] |= (1 << sr::I0) | (1 << sr::C);

    // Now execute ENDDO
    pram[5] = 0x00008C; // enddo
    s.pc = 5;
    run_one(&mut s, &mut jit); // ENDDO

    // LF should be restored from stacked SR (was 0 before DO)
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "ENDDO: LF should be restored from stack"
    );
    // But I0 and C should remain as we set them (not restored from stack)
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::I0),
        0,
        "ENDDO: I0 should NOT be restored from stack"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "ENDDO: C should NOT be restored from stack"
    );
}
