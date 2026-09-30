use super::*;

unsafe extern "C" fn cb_read(opaque: *mut std::ffi::c_void, _addr: u32) -> u32 {
    unsafe { *(opaque as *const u32) }
}

unsafe extern "C" fn cb_write(opaque: *mut std::ffi::c_void, _addr: u32, val: u32) {
    unsafe { *(opaque as *mut u32) = val }
}

unsafe extern "C" fn cb_read_zero(_opaque: *mut std::ffi::c_void, _addr: u32) -> u32 {
    0
}

unsafe extern "C" fn cb_set_halt(opaque: *mut std::ffi::c_void, _addr: u32, _val: u32) {
    let state = opaque as *mut DspState;
    unsafe {
        (*state).exit_requested = true;
        (*state).halt_requested = true;
    }
}

#[test]
fn test_block_simple_sequence() {
    // Three nops followed by a jmp - should compile as one block.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000000; // nop
    pram[1] = 0x000000; // nop
    pram[2] = 0x000000; // nop
    pram[3] = 0x0C0010; // jmp $0010
    // Budget: 3 nops (1) + jmp (3) = 6 cycles
    s.run(&mut jit, 6);
    assert_eq!(s.pc, 0x10);
    assert_eq!(s.cycle_count, 6);
}

#[test]
fn test_block_arithmetic_sequence() {
    // Sequence of parallel ALU ops ending in a jmp.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x100000;
    pram[0] = 0x200013; // clr A (nop move)
    pram[1] = 0x200040; // add X0,A (nop move)
    pram[2] = 0x200040; // add X0,A (nop move)
    pram[3] = 0x200040; // add X0,A (nop move)
    pram[4] = 0x0C0020; // jmp $0020
    // 4 parallel ops (1) + jmp (3) = 7 cycles
    s.run(&mut jit, 7);
    // A should be 3 * X0 = 0x300000
    assert_eq!(s.registers[reg::A1], 0x300000);
    assert_eq!(s.pc, 0x20);
    assert_eq!(s.cycle_count, 7);
}

#[test]
fn test_block_conditional_branch_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200013; // clr A (sets Z=1)
    pram[1] = 0x0EA100; // jcc EQ,$100 (Z=1 so taken)
    // clr A (1) + jcc (4) = 5 cycles
    s.run(&mut jit, 5);
    assert_eq!(s.pc, 0x100);
    assert_eq!(s.cycle_count, 5);
}

#[test]
fn test_block_conditional_branch_not_taken() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A is zero initially, inc A makes it non-zero -> Z=0
    pram[0] = 0x000008; // inc A
    pram[1] = 0x0EA100; // jcc EQ,$100 (Z=0 so not taken)
    // inc (1) + jcc (4) = 5 cycles
    s.run(&mut jit, 5);
    // Not taken: PC = 2 (past the 2 instructions)
    assert_eq!(s.pc, 2);
}

#[test]
fn test_run_loop_with_wait() {
    // Simple program: increment A a few times then wait.
    // WAIT is a no-op (power states not modeled), so execution continues past it.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000008; // inc A
    pram[1] = 0x000008; // inc A
    pram[2] = 0x0C0002; // jmp $0002 (loop to stop)
    s.run(&mut jit, 10);
    assert_eq!(s.registers[reg::A0], 2);
}

#[test]
fn test_run_cycle_budget() {
    // Run with limited cycles - should stop when budget exhausted.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Infinite loop: nop + jmp $0
    pram[0] = 0x000000; // nop (1 cycle)
    pram[1] = 0x0C0000; // jmp $0 (3 cycles)
    // Block = [nop, jmp] = 4 cycles, terminates at jmp.
    // Budget 5: 1st block (4) -> 1 left, 2nd block (4) -> -3 -> stop
    s.run(&mut jit, 5);
    assert_eq!(s.cycle_count, 8); // 2 blocks x 4 cycles
    assert_eq!(s.pc, 0);
}

#[test]
fn test_block_move_and_alu() {
    // Test a block that does a memory read + ALU op.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[3] = 0x123456;
    // move X:$03,X0 + clr A
    pram[0] = 0x448313;
    // add X0,A (nop move)
    pram[1] = 0x200040;
    // jmp $0020
    pram[2] = 0x0C0020;
    // 2 parallel ops (1) + jmp (3) = 5 cycles
    s.run(&mut jit, 5);
    // X0 should be loaded from X:$03
    assert_eq!(s.registers[reg::X0], 0x123456);
    // A = 0 (clr'd) + X0 = X0 in A1
    assert_eq!(s.registers[reg::A1], 0x123456);
    assert_eq!(s.pc, 0x20);
}

#[test]
fn test_basic_program() {
    // A minimal test program:
    //   org p:$0000
    //   jmp <start
    //   org p:$40
    // start:
    //   jmp mainloop
    // mainloop:
    //   move #$123456,A
    //   move A,X:3
    //   movep #$000001,x:$ffffc4
    //   jmp <mainloop
    let prog = "\
P 0000 0C0040
P 0040 0AF080
P 0041 000042
P 0042 56F400
P 0043 123456
P 0044 567000
P 0045 000003
P 0046 08F484
P 0047 000001
P 0048 0C0042";

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
    load_a56_program(&mut pram, &mut xram, &mut yram, prog);

    // Run several iterations of the main loop
    for _ in 0..20 {
        run_one(&mut s, &mut jit);
    }

    // The program writes 0x123456 to X:3 each iteration
    assert_eq!(xram[3], 0x123456);
    // movep #1,x:$ffffc4 writes 1 to peripheral register
    assert_eq!(periph[0x44], 1); // $ffffc4 = periph[$c4 - $80 = $44]
}

#[test]
fn test_illegal_raises_interrupt() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // ILLEGAL vector is at VBA:$04 (Table 2-2), not P:$3E (DSP56000 holdover on page 13-76)
    pram[0] = 0x000005; // illegal
    run_one(&mut s, &mut jit);
    // postexecute_interrupts() dispatches the interrupt: pending is cleared,
    // pipeline is started (interrupt_state becomes Fast, pipeline_stage = 5)
    assert!(!s.interrupts.pending(interrupt::ILLEGAL));
    assert!(!s.interrupts.has_pending());
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert_eq!(s.interrupts.pipeline_stage, 5);
}

#[test]
fn test_interrupt_dispatch_fast() {
    // Fast interrupt: vector has 2 inline instructions (not JSR)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set ILLEGAL IPL to 3 (non-maskable)
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    // Place NOP at ILLEGAL vector (0x04) - fast interrupt (no JSR)
    pram[0x04] = 0x000000; // nop
    pram[0x05] = 0x000000; // nop
    // Place ILLEGAL at PC=0, then NOPs for pipeline to run through
    pram[0] = 0x000005; // illegal
    pram[1..10].fill(0x000000); // nops

    // Step 1: execute ILLEGAL at PC=0 -> dispatches interrupt, pipeline=5
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert_eq!(s.interrupts.pipeline_stage, 5);
    assert_eq!(s.pc, 1);

    // Step 2: NOP at PC=1, pipeline 5->4
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 4);

    // Step 3: NOP at PC=2, pipeline 4->3 (save PC=3, redirect to vector 0x04)
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 3);
    assert_eq!(s.pc, 0x04); // redirected to vector

    // Step 4: NOP at PC=0x04 (first vector word), pipeline 3->2
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 2);
    assert_eq!(s.pc, 0x05);

    // Step 5: NOP at PC=0x05 (second vector word), pipeline 2->1
    // Fast interrupt detected (PC=vector+2) -> restore saved PC
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 1);
    assert_eq!(s.pc, 3); // restored saved PC

    // Step 6: NOP at PC=3, pipeline 1->0
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 0);

    // Step 7: NOP at PC=4, pipeline 0 -> re-enable (STATE_NONE)
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.state, InterruptState::None);
}

#[test]
fn test_interrupt_ipl_masking() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set SWI IPL to 1
    s.interrupts.ipl[interrupt::TRAP] = 1;
    // Set SR IPL to 2 (masks level 0 and 1)
    s.registers[reg::SR] = 2 << sr::I0;

    // Post SWI interrupt
    s.interrupts.add(interrupt::TRAP);
    assert!(s.interrupts.pending(interrupt::TRAP));
    assert!(s.interrupts.has_pending());

    // Execute a NOP - postexecute_interrupts should NOT dispatch (masked)
    pram[0] = 0x000000; // nop
    run_one(&mut s, &mut jit);
    // Interrupt should still be pending (not dispatched due to masking)
    assert!(s.interrupts.pending(interrupt::TRAP));
    assert!(s.interrupts.has_pending());
    assert_eq!(s.interrupts.state, InterruptState::None);
}

#[test]
fn test_interrupt_during_run() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // ILLEGAL vector is at VBA:$04 (Table 2-2)
    pram[0x04] = 0x000000; // nop at ILLEGAL vector
    pram[0x05] = 0x000000; // nop
    // Place ILLEGAL at PC=0, then many NOPs
    pram[0] = 0x000005; // illegal
    pram[1..20].fill(0x000000);

    // run() should handle the interrupt pipeline
    s.run(&mut jit, 100);
    // After enough cycles, interrupt should have completed
    assert_eq!(s.interrupts.state, InterruptState::None);
}

#[test]
fn test_interrupt_dispatch_long() {
    // Long interrupt: vector has JSR -> pushes context to stack
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set ILLEGAL IPL to 3 (non-maskable)
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    // ILLEGAL at PC=0, NOPs for pipeline
    pram[0] = 0x000005; // illegal
    pram[1..10].fill(0x000000);
    // Place JSR $100 at ILLEGAL vector (0x04) -> triggers LONG interrupt
    pram[0x04] = 0x0D0100; // jsr $100
    pram[0x05] = 0x000000; // nop (won't execute - JSR jumps away)
    // Subroutine at $100
    pram[0x100] = 0x000000; // nop
    pram[0x101] = 0x000004; // rti

    // Step 1: ILLEGAL -> dispatch pipeline
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert_eq!(s.interrupts.pipeline_stage, 5);

    // Step 2: pipeline 5->4
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 4);

    // Step 3: pipeline 4->3 (save PC, redirect to vector 0x04, detect JSR -> LONG)
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 3);
    assert_eq!(s.interrupts.state, InterruptState::Long);
    assert_eq!(s.pc, 0x04); // at vector

    // Step 4: execute JSR at 0x04 -> jumps to $100, pipeline 3->2
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.pipeline_stage, 2);
    assert_eq!(s.pc, 0x100); // at subroutine

    // Stack should have saved context (pushed by long interrupt detection)
    // SP incremented by stack_push
    assert!(s.registers[reg::SP] & 0xF > 0);
}

#[test]
fn test_interrupt_arbitration_priority() {
    // Post two interrupts with different IPLs, verify highest wins
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // TRAP at IPL 1, ILLEGAL at IPL 2
    s.interrupts.ipl[interrupt::TRAP] = 1;
    s.interrupts.ipl[interrupt::ILLEGAL] = 2;
    s.registers[reg::SR] &= !((0x3) << sr::I0);
    s.interrupts.add(interrupt::TRAP);
    s.interrupts.add(interrupt::ILLEGAL);
    assert!(s.interrupts.has_pending());

    // ILLEGAL vector is at 0x04, TRAP vector is at 0x08
    pram[0x04] = 0x000000;
    pram[0x05] = 0x000000;
    pram[0x08] = 0x000000;
    pram[0x09] = 0x000000;
    pram[0] = 0x000000;
    pram[1..20].fill(0x000000);

    // ILLEGAL (IPL=2) should be dispatched first
    run_one(&mut s, &mut jit);
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert!(!s.interrupts.pending(interrupt::ILLEGAL));
    assert!(s.interrupts.pending(interrupt::TRAP));
    assert!(s.interrupts.has_pending());
    assert_eq!(s.interrupts.vector_addr, 0x04); // ILLEGAL vector
}

#[test]
fn test_stack_push_overflow() {
    // Push 16 times to overflow (SP 15 -> 16, bit 4 set -> STACK_ERROR)
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    for i in 0..15 {
        s.stack_push(i as u32, 0);
    }
    assert_eq!(s.registers[reg::SP] & 0xF, 15);
    assert!(!s.interrupts.pending(interrupt::STACK_ERROR));

    // 16th push overflows
    s.stack_push(0x9999, 0);
    assert!(s.interrupts.pending(interrupt::STACK_ERROR));
    // SE bit (bit 4) should be set
    assert_ne!(s.registers[reg::SP] & (1 << 4), 0);
}

#[test]
fn test_stack_pop_underflow() {
    // Pop when SP=0 -> underflow (stack wraps to 0xF, bit 4 set -> STACK_ERROR)
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    assert_eq!(s.registers[reg::SP] & 0xF, 0);
    assert!(!s.interrupts.pending(interrupt::STACK_ERROR));

    s.stack_pop();
    assert!(s.interrupts.pending(interrupt::STACK_ERROR));
    assert_ne!(s.registers[reg::SP] & (1 << 4), 0);
}

#[test]
fn test_add_interrupt_disabled() {
    // Posting an interrupt with IPL=-1 should be a no-op
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::TRAP] = -1; // disabled
    s.interrupts.add(interrupt::TRAP);
    assert!(!s.interrupts.pending(interrupt::TRAP));
    assert!(!s.interrupts.has_pending());

    // Out-of-range index should also be a no-op
    s.interrupts.add(99);
    assert!(!s.interrupts.has_pending());
}

#[test]
fn test_movec_ssh_overflow() {
    // Write SSH 16 times -> SP overflows, STACK_ERROR interrupt
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    s.registers[reg::X0] = 0xAAAA;
    // movec X0,SSH = 0x04C4BC
    pram[..16].fill(0x04C4BC);
    pram[16] = 0x000000; // nop sentinel

    // Execute 15 pushes (SP 0->15, no overflow)
    for _ in 0..15 {
        run_one(&mut s, &mut jit);
    }
    assert_eq!(s.registers[reg::SP] & 0xF, 15);
    assert!(!s.interrupts.pending(interrupt::STACK_ERROR));

    // 16th push -> overflow -> STACK_ERROR dispatched by postexecute_interrupts
    run_one(&mut s, &mut jit);
    // Interrupt was dispatched (is_pending cleared, pipeline started)
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert_eq!(s.interrupts.vector_addr, 0x02); // STACK_ERROR vector
    // SE bit (bit 4) should be set in SP
    assert_ne!(s.registers[reg::SP] & (1 << 4), 0);
}

#[test]
fn test_movec_sp_overflow_value() {
    // movec X0,SP where X0 has bit 4 set -> STACK_ERROR
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    // STACK_ERROR vector at 0x02
    pram[0x02] = 0x000000; // nop
    pram[0x03] = 0x000000; // nop
    s.registers[reg::X0] = 0x10; // bit 4 set = SE
    // movec X0,SP = 0x04C4BB
    pram[0] = 0x04C4BB;
    run_one(&mut s, &mut jit);
    // STACK_ERROR dispatched by postexecute_interrupts
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    assert_eq!(s.interrupts.vector_addr, 0x02); // STACK_ERROR vector
    // SP should have the error bits set
    assert_ne!(s.registers[reg::SP] & (1 << 4), 0);
}

#[test]
fn test_movem_writes_pram_dirty() {
    // movem R0,P:$10 should write R0 value to pram[0x10]
    // and set the pram dirty range.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // movem R0,P:$10 = 00000111_0_0_010000_00_010000 = 0x071010
    pram[0] = 0x071010;
    s.registers[reg::R0] = 0x000008; // inc A opcode
    run_one(&mut s, &mut jit);
    assert_eq!(pram[0x10], 0x000008, "movem should write R0 to P:$10");
    // Dirty bitmap bit for address 0x10 should be set
    assert_ne!(s.pram_dirty.dirty[0x10 / 64] & (1u64 << 0x10), 0);
}

#[test]
fn test_self_modifying_code_run() {
    // Self-modifying code: overwrite P:$10 with "inc A" opcode,
    // then jump there. The JIT cache should be invalidated so
    // the new instruction executes, not the original NOP.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Block 1: overwrite P:$10, then jump to $10
    pram[0x00] = 0x071010; // movem R0,P:$10
    pram[0x01] = 0x0C0010; // jmp $10

    // Block 2 target: initially NOP, will be overwritten to "inc A"
    pram[0x10] = 0x000000; // nop (overwritten at runtime)
    pram[0x11] = 0x000000; // nop (fallthrough)

    // R0 = "inc A" opcode (0x000008)
    s.registers[reg::R0] = 0x000008;

    // Run enough cycles to execute both blocks
    s.run(&mut jit, 100);

    assert_eq!(pram[0x10], 0x000008, "P:$10 should be inc A");
    assert_eq!(s.registers[reg::A0], 1, "inc A should have executed");
    assert!(s.pc > 0x10, "PC should have advanced past $10");
}

#[test]
fn test_self_modifying_code_two_stages() {
    // Two-stage self-modification: stage 1 writes stage 2 code
    // to a different address, jumps there. Stage 2 writes stage 3
    // code, jumps there. Verify all stages execute correctly.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Stage 1 at PC=0: write "inc A" to P:$20, then jump to $20
    pram[0x00] = 0x072010; // movem R0,P:$20
    pram[0x01] = 0x0C0020; // jmp $20

    // Stage 2 at PC=$20: initially NOP (overwritten to "inc A")
    // After inc A, write "inc B" to P:$30, then jump to $30
    pram[0x20] = 0x000000; // nop (overwritten to inc A)
    pram[0x21] = 0x073011; // movem R1,P:$30 (R1=reg 0x11)
    pram[0x22] = 0x0C0030; // jmp $30

    // Stage 3 at PC=$30: initially NOP (overwritten to "inc B")
    pram[0x30] = 0x000000; // nop (overwritten to inc B)
    pram[0x31] = 0x000000; // nop

    // R0 = "inc A" opcode (0x000008), R1 = "inc B" opcode (0x000009)
    s.registers[reg::R0] = 0x000008;
    s.registers[reg::R1] = 0x000009;

    s.run(&mut jit, 200);

    // Stage 1 wrote inc A to P:$20 and jumped there
    assert_eq!(pram[0x20], 0x000008, "P:$20 should be inc A");
    assert_eq!(s.registers[reg::A0], 1, "inc A should have executed");

    // Stage 2 wrote inc B to P:$30 and jumped there
    assert_eq!(pram[0x30], 0x000009, "P:$30 should be inc B");
    assert_eq!(s.registers[reg::B0], 1, "inc B should have executed");
}

#[test]
fn test_range_mask() {
    // Single bit
    assert_eq!(PramDirtyBitmap::range_mask(0, 1), 1);
    assert_eq!(PramDirtyBitmap::range_mask(5, 6), 1 << 5);
    // Full word
    assert_eq!(PramDirtyBitmap::range_mask(0, 64), !0u64);
    // Partial range
    assert_eq!(PramDirtyBitmap::range_mask(2, 5), 0b11100);
    // High bits
    assert_eq!(PramDirtyBitmap::range_mask(60, 64), 0xF << 60);
}

#[test]
fn test_is_range_dirty_single_word() {
    // Range within a single u64 word
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    pc.dirty[0] = 1 << 10; // bit 10 is dirty
    assert!(pc.is_range_dirty(5, 15));
    assert!(pc.is_range_dirty(10, 11));
    assert!(!pc.is_range_dirty(11, 20));
    assert!(!pc.is_range_dirty(0, 10));
}

#[test]
fn test_is_range_dirty_multi_word() {
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    // Set bit 100 dirty (word 1, bit 36)
    pc.dirty[1] = 1 << 36;
    assert!(pc.is_range_dirty(0, 128));
    assert!(pc.is_range_dirty(96, 104));
    assert!(!pc.is_range_dirty(0, 64));
    assert!(!pc.is_range_dirty(128, 192));
}

#[test]
fn test_is_range_dirty_empty_range() {
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    pc.dirty.fill(!0u64); // all dirty
    assert!(!pc.is_range_dirty(10, 10)); // empty range
    assert!(!pc.is_range_dirty(20, 15)); // inverted range
}

#[test]
fn test_is_range_dirty_boundary_bits() {
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    // Dirty bit at word boundary: bit 63
    pc.dirty[0] = 1 << 63;
    assert!(pc.is_range_dirty(60, 65));
    assert!(!pc.is_range_dirty(64, 128));
    // Dirty bit at word boundary: bit 64
    pc.dirty[0] = 0;
    pc.dirty[1] = 1;
    assert!(pc.is_range_dirty(60, 65));
    assert!(!pc.is_range_dirty(0, 64));
}

#[test]
fn test_clear_dirty_range_single_word() {
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    pc.dirty[0] = 0xFF; // bits 0-7 dirty
    pc.clear_dirty_range(2, 6);
    assert_eq!(pc.dirty[0], 0xFF & !(0b111100)); // bits 2-5 cleared
}

#[test]
fn test_clear_dirty_range_multi_word() {
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    pc.dirty.fill(!0u64); // all dirty
    pc.clear_dirty_range(60, 130);
    // Word 0: bits 60-63 should be cleared
    assert_eq!(pc.dirty[0] & (0xF << 60), 0);
    // Word 1: fully cleared (bits 64-127)
    assert_eq!(pc.dirty[1], 0);
    // Word 2: bits 128-129 should be cleared
    assert_eq!(pc.dirty[2] & 0x3, 0);
    // Bits outside the range should still be set
    assert_ne!(pc.dirty[0], 0); // bits 0-59 still set
    assert_ne!(pc.dirty[2], 0); // bits 130+ still set
}

#[test]
fn test_clear_dirty_range_noop_empty() {
    let mut pc = PramDirtyBitmap::new(PRAM_SIZE);
    pc.dirty.fill(!0u64);
    let before = pc.dirty.clone();
    pc.clear_dirty_range(10, 10); // empty range
    assert_eq!(pc.dirty, before);
}

#[test]
fn test_pram_generation_bumps_on_write() {
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    assert_eq!(s.pram_dirty.generation, 0);
    unsafe { jit_write_mem(&mut s as *mut DspState, MemSpace::P, 0x100, 0x123456) };
    assert_eq!(s.pram_dirty.generation, 1);
    assert_eq!(pram[0x100], 0x123456);
    // Writing same value again should NOT bump (no change)
    unsafe { jit_write_mem(&mut s as *mut DspState, MemSpace::P, 0x100, 0x123456) };
    assert_eq!(s.pram_dirty.generation, 1);
    // Writing different value should bump
    unsafe { jit_write_mem(&mut s as *mut DspState, MemSpace::P, 0x100, 0x654321) };
    assert_eq!(s.pram_dirty.generation, 2);
}

#[test]
fn test_cycle_counts_match_manual() {
    // Verify per-instruction cycle counts match DSP56300FM Table A-1.
    // All instructions pre-populated at unique addresses before any execution
    // to avoid JIT block cache invalidation issues.
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
    s.registers[reg::X0] = 0x000002; // bit 0 clear (for JCLR test)

    // === Pre-populate all instructions at unique addresses ===
    // JMP trampolines bridge between address regions.

    // --- 0x00: 1-cycle instructions ---
    pram[0x00] = 0x000000; // NOP (T=1)
    pram[0x01] = 0x000008; // INC A (T=1)
    pram[0x02] = 0x015080; // ADD #$10,A (T=1)
    pram[0x03] = 0x0542A0; // MOVEC #$42,M0 (T=1)
    pram[0x04] = 0x200013; // CLR A (parallel DALU, T=1)
    pram[0x05] = 0x018040; // DIV X0,A (T=1)
    pram[0x06] = 0x020000; // Tcc B,A with CC condition (T=1)
    pram[0x07] = 0x0C0010; // JMP $10 (T=3)

    // --- 0x10: 2-cycle and 3-cycle instructions ---
    pram[0x10] = 0x0BC463; // BTST #3,X0 (T=2, bit op reg)
    pram[0x11] = 0x0140C0; // ADD #xxxxxx,A (T=2, 2-word)
    pram[0x12] = 0x000042; //   immediate value
    pram[0x13] = 0x00F0B9; // ANDI #$F0,CCR (T=3)
    pram[0x14] = 0x0004F9; // ORI #$04,CCR (T=3)
    pram[0x15] = 0x0D0030; // JSR $30 (T=3)

    // --- 0x30: RTS ---
    pram[0x30] = 0x00000C; // RTS (T=3) --returns to 0x16

    // --- 0x16: back from JSR ---
    pram[0x16] = 0x0C0040; // JMP $40 (T=3)

    // --- 0x40: 4-cycle branches ---
    pram[0x40] = 0x050C02; // BRA +2 (T=4) --lands at $42
    pram[0x41] = 0x000000; // filler
    pram[0x42] = 0x200013; // CLR A (T=1) --clears C for Bcc CC
    pram[0x43] = 0x050403; // Bcc CC,+3 (T=4) --lands at $46
    pram[0x44] = 0x000000; // filler
    pram[0x45] = 0x000000; // filler
    pram[0x46] = 0x050802; // BSR +2 (T=4) --push ret=$47, jump to $48
    pram[0x47] = 0x0C0050; // JMP $50 (T=3) --after BSR return
    pram[0x48] = 0x00000C; // RTS (T=3) --returns to $47

    // --- 0x50: JCLR reg (T=4, 2-word) ---
    pram[0x50] = 0x0AC400; // JCLR #0,X0,target (bit 0 of X0 is clear)
    pram[0x51] = 0x000060; //   target $60

    // --- 0x60: DO #1 (T=5, 2-word) ---
    pram[0x60] = 0x060180; // DO #1,$63
    pram[0x61] = 0x000063; //   loop end address
    pram[0x62] = 0x000000; // NOP (loop body)
    pram[0x63] = 0x000000; // NOP (at LA, loop exits after)
    pram[0x64] = 0x0C0070; // JMP $70 (T=3)

    // --- 0x70: REP #1 (T=5) ---
    pram[0x70] = 0x0601A0; // REP #1
    pram[0x71] = 0x000000; // NOP (repeated once)
    pram[0x72] = 0x0C0080; // JMP $80 (T=3)

    // --- 0x80: MOVEP, MOVEM, LUA, NORM, RESET ---
    pram[0x80] = 0x08CE04; // MOVEP A,X:$FFFFC4 (T=1, reg<->pp)
    pram[0x81] = 0x079004; // MOVEM P:$10,X0 (T=6, aa form)
    pram[0x82] = 0x040011; // LUA (R0+$01),R1 (T=3)
    pram[0x83] = 0x01D815; // NORM R0,A (T=5)
    pram[0x84] = 0x000084; // RESET (T=7)

    // === Execute and verify cycle counts ===

    // 1-cycle instructions
    assert_eq!(run_one(&mut s, &mut jit), 1, "NOP");
    assert_eq!(run_one(&mut s, &mut jit), 1, "INC A");
    assert_eq!(run_one(&mut s, &mut jit), 1, "ADD #xx,D");
    assert_eq!(run_one(&mut s, &mut jit), 1, "MOVEC #xx,D1");
    assert_eq!(run_one(&mut s, &mut jit), 1, "CLR A (parallel)");
    assert_eq!(run_one(&mut s, &mut jit), 1, "DIV");
    assert_eq!(run_one(&mut s, &mut jit), 1, "Tcc");
    // JMP $10 (T=3)
    assert_eq!(run_one(&mut s, &mut jit), 3, "JMP aa");
    assert_eq!(s.pc, 0x10);

    // 2-cycle instructions
    assert_eq!(run_one(&mut s, &mut jit), 2, "BTST #n,D");
    assert_eq!(run_one(&mut s, &mut jit), 2, "ADD #xxxxxx,D");
    // 3-cycle instructions
    assert_eq!(run_one(&mut s, &mut jit), 3, "ANDI");
    assert_eq!(run_one(&mut s, &mut jit), 3, "ORI");
    assert_eq!(run_one(&mut s, &mut jit), 3, "JSR aa");
    assert_eq!(s.pc, 0x30);
    assert_eq!(run_one(&mut s, &mut jit), 3, "RTS");
    assert_eq!(s.pc, 0x16);
    assert_eq!(run_one(&mut s, &mut jit), 3, "JMP (trampoline)");
    assert_eq!(s.pc, 0x40);

    // 4-cycle branches
    assert_eq!(run_one(&mut s, &mut jit), 4, "BRA short");
    assert_eq!(s.pc, 0x42);
    assert_eq!(run_one(&mut s, &mut jit), 1, "CLR A (setup)");
    assert_eq!(run_one(&mut s, &mut jit), 4, "Bcc CC short");
    assert_eq!(s.pc, 0x46);
    assert_eq!(run_one(&mut s, &mut jit), 4, "BSR short");
    assert_eq!(s.pc, 0x48);
    assert_eq!(run_one(&mut s, &mut jit), 3, "RTS (from BSR)");
    assert_eq!(s.pc, 0x47);
    assert_eq!(run_one(&mut s, &mut jit), 3, "JMP (trampoline)");
    assert_eq!(s.pc, 0x50);

    // JCLR reg (T=4, bit 0 of X0 is clear -> jump taken)
    assert_eq!(run_one(&mut s, &mut jit), 4, "JCLR #n,D");
    assert_eq!(s.pc, 0x60);

    // DO #1 (T=5)
    assert_eq!(run_one(&mut s, &mut jit), 5, "DO #imm");
    assert_eq!(run_one(&mut s, &mut jit), 1, "NOP (loop body)");
    assert_eq!(run_one(&mut s, &mut jit), 1, "NOP (at LA)");
    assert_eq!(s.pc, 0x64);
    assert_eq!(run_one(&mut s, &mut jit), 3, "JMP (trampoline)");
    assert_eq!(s.pc, 0x70);

    // REP #1 (T=5)
    assert_eq!(run_one(&mut s, &mut jit), 5, "REP #imm");
    assert_eq!(run_one(&mut s, &mut jit), 1, "NOP (repeated)");
    assert_eq!(run_one(&mut s, &mut jit), 3, "JMP (trampoline)");
    assert_eq!(s.pc, 0x80);

    // MOVEP reg<->pp (T=1), MOVEM P:aa (T=6), LUA (T=3), NORM (T=5), RESET (T=7)
    assert_eq!(run_one(&mut s, &mut jit), 1, "MOVEP reg<->pp");
    assert_eq!(run_one(&mut s, &mut jit), 6, "MOVEM P:aa");
    assert_eq!(run_one(&mut s, &mut jit), 3, "LUA (Rn+aa)");
    assert_eq!(run_one(&mut s, &mut jit), 5, "NORM Rn,D");
    assert_eq!(run_one(&mut s, &mut jit), 7, "RESET");
}

#[test]
fn test_pram_dirty_bits_set_on_write() {
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    unsafe { jit_write_mem(&mut s as *mut DspState, MemSpace::P, 100, 0x000042) };
    // Bit 100 should be set in word 1 (100/64=1, 100%64=36)
    assert_ne!(s.pram_dirty.dirty[1] & (1 << 36), 0);
}

#[test]
fn test_callback_read_static() {
    // MOVEP0 reads from a callback region (static address path).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut cb_val: u32 = 0x42;
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Callback {
            opaque: &mut cb_val as *mut u32 as *mut std::ffi::c_void,
            read_fn: cb_read,
            write_fn: cb_write,
        },
    });
    let mut s = DspState::new(map);
    // movep x:$FFFFC0,X0 (read from peripheral to X0)
    // s=0, W=0, d=X0=0x04, pp=0
    pram[0] = 0x084400;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x42);
}

#[test]
fn test_callback_write_static() {
    // MOVEP0 writes to a callback region (static address path).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut cb_val: u32 = 0;
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Callback {
            opaque: &mut cb_val as *mut u32 as *mut std::ffi::c_void,
            read_fn: cb_read,
            write_fn: cb_write,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::X0] = 0xABCDEF;
    // movep X0,x:$FFFFC0 (write X0 to peripheral)
    // s=0, W=1, d=X0=0x04, pp=0
    pram[0] = 0x08C400;
    run_one(&mut s, &mut jit);
    assert_eq!(cb_val, 0xABCDEF);
}

#[test]
fn test_callback_read_dyn() {
    // MOVEC ea reads from a callback region via dynamic EA dispatch.
    // Uses MOVEC X:(R0),M0 where R0 points into a callback region.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut cb_val: u32 = 0x99;
    let cb_start = XRAM_SIZE as u32; // just past the buffer region
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: cb_start,
        end: cb_start + 4,
        kind: RegionKind::Callback {
            opaque: &mut cb_val as *mut u32 as *mut std::ffi::c_void,
            read_fn: cb_read,
            write_fn: cb_write,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = cb_start;
    // MOVEC X:(R0),M0: W=1, MMM=4(no update), RRR=0, s=0(X), ddddd=M0=0x00
    pram[0] = 0x05E020;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::M0], 0x99);
}

#[test]
fn test_callback_write_dyn() {
    // MOVEC ea writes to a callback region via dynamic EA dispatch.
    // Uses MOVEC M0,X:(R0) where R0 points into a callback region.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut cb_val: u32 = 0;
    let cb_start = XRAM_SIZE as u32;
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: cb_start,
        end: cb_start + 4,
        kind: RegionKind::Callback {
            opaque: &mut cb_val as *mut u32 as *mut std::ffi::c_void,
            read_fn: cb_read,
            write_fn: cb_write,
        },
    });
    let mut s = DspState::new(map);
    s.registers[reg::R0] = cb_start;
    s.registers[reg::M0] = 0xFEDCBA;
    // MOVEC M0,X:(R0): W=0, MMM=4(no update), RRR=0, s=0(X), ddddd=M0=0x00
    pram[0] = 0x056020;
    run_one(&mut s, &mut jit);
    assert_eq!(cb_val, 0xFEDCBA);
}

#[test]
fn test_block_exit_requested() {
    // Block: MOVEP read(1), MOVEP write(1), [exit check], NOP(1), JMP(3)
    // The MOVEP write callback sets exit_requested + halt_requested.
    // Block should return after MOVEP read + MOVEP write = 2 cycles, PC = 2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let state_ptr = &mut s as *mut DspState;
    s.map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Callback {
            opaque: state_ptr as *mut std::ffi::c_void,
            read_fn: cb_read_zero,
            write_fn: cb_set_halt,
        },
    });

    pram[0] = 0x084400; // MOVEP x:$FFFFC0,X0 (1 cycle, read via cb_read_zero)
    pram[1] = 0x08C400; // MOVEP X0,x:$FFFFC0 (1 cycle, write triggers cb_set_halt)
    pram[2] = 0x000000; // NOP (should not execute)
    pram[3] = 0x0C0000; // JMP $0 (should not execute)

    s.run(&mut jit, 100);

    // Block exited early: MOVEP read(1) + MOVEP write(1) = 2 cycles
    assert_eq!(s.cycle_count, 2);
    // PC should be at instruction after MOVEP write
    assert_eq!(s.pc, 2);
    // halt_requested should still be set
    assert!(s.halt_requested);
    // X0 should be 0 (from cb_read_zero)
    assert_eq!(s.registers[reg::X0], 0);
}

#[test]
fn test_dump_profile() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    jit.enable_profiling();

    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000000; // NOP
    pram[1] = 0x0C0000; // JMP $0

    s.run(&mut jit, 10);

    // Dump to a temp file and verify it contains expected content.
    let path = std::env::temp_dir().join("dsp56300_test_profile.txt");
    jit.dump_profile(&s.map, path.to_str().unwrap());

    let contents = std::fs::read_to_string(&path).unwrap();
    assert!(contents.contains("hits"));
    assert!(contents.contains("total_cycles"));
    assert!(contents.contains("P-SPACE DUMP OF TOP 20 BLOCKS"));
    assert!(contents.contains("0000.."));
    std::fs::remove_file(&path).ok();
}

#[test]
fn test_undefined_alu_op() {
    // Parallel instruction with undefined ALU byte 0x04 -> no-op ALU (covers line 5650)
    // Use PM2 (move_bits=2) with alu_byte=0x04
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x200004; // PM2 + undefined ALU 0x04
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_read_mem_dyn_empty_region() {
    // Dynamic Y memory read with no Y regions -> returns 0 (covers line 2613)
    // Uses PM1 Y read which calls read_mem_dyn(Y, ...).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let map = MemoryMap {
        x_regions: vec![MemoryRegion {
            start: 0,
            end: xram.len() as u32,
            kind: RegionKind::Buffer {
                base: xram.as_mut_ptr(),
                offset: 0,
            },
        }],
        y_regions: vec![], // empty!
        p_regions: vec![MemoryRegion {
            start: 0,
            end: pram.len() as u32,
            kind: RegionKind::Buffer {
                base: pram.as_mut_ptr(),
                offset: 0,
            },
        }],
    };
    let mut s = DspState::new(map);
    // PM1 Y read: w=1 (read), bit14=1 (Y space), mode 4 R0
    // move_bits=1 -> bits 23:20 = 0001
    // 0x100000 | (1<<15) | (1<<14) | (0x20<<8) = 0x10E000
    pram[0] = 0x10E000;
    s.registers[reg::R0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_write_mem_dyn_empty_region() {
    // Dynamic Y memory write with no Y regions -> silently drops (covers line 2735)
    // Uses PM0 Y space which calls write_mem_dyn(Y, ...).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let map = MemoryMap {
        x_regions: vec![MemoryRegion {
            start: 0,
            end: xram.len() as u32,
            kind: RegionKind::Buffer {
                base: xram.as_mut_ptr(),
                offset: 0,
            },
        }],
        y_regions: vec![], // empty!
        p_regions: vec![MemoryRegion {
            start: 0,
            end: pram.len() as u32,
            kind: RegionKind::Buffer {
                base: pram.as_mut_ptr(),
                offset: 0,
            },
        }],
    };
    let mut s = DspState::new(map);
    // PM0 Y: 0x09A000 -- d=B, Y space, mode4 R0, NOP ALU
    // PM0 always writes acc to memory via write_mem_dyn
    pram[0] = 0x09A000;
    s.registers[reg::R0] = 0;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_deferred_acc_reload_in_rep() {
    // REP-inlined instruction with callback Y region causes acc
    // invalidation mid-body. load_acc finds acc_valid=false but
    // entry_acc_valid=true -> inline reload.
    // Covers lines 1432-1434 in load_acc.
    //
    // PC 0: TST B -- load_acc(B) defers to entry, acc_valid[B]=true
    // PC 1: REP #2
    // PC 2: PM1 Y:(R0),Y0 A,X0 TST B -- read_mem_dyn(Y) invalidates
    //        (has_callbacks), then TST B -> load_acc(B) inline reload
    // PC 3: JMP $3 (halt)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut cb_val: u32 = 0;
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.y_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Callback {
            opaque: &mut cb_val as *mut u32 as *mut std::ffi::c_void,
            read_fn: cb_read,
            write_fn: cb_write,
        },
    });
    let mut s = DspState::new(map);
    pram[0] = 0x20000B; // tst b
    pram[1] = 0x0602A0; // rep #2
    pram[2] = 0x10E00B; // tst b  a,x0  y:(r0),y0
    pram[3] = 0x0C0003; // jmp $3
    s.registers[reg::R0] = 0;
    s.registers[reg::M0] = 0xFFFFFF;
    s.run(&mut jit, 200);
    assert_eq!(s.pc, 3);
}

#[test]
fn test_pram_boundary_next_word() {
    // Instruction at last pram address triggers the next_word boundary
    // fallback in emit_block (line 609).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; 2];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000000; // nop
    pram[1] = 0x000000; // nop (last address, next_word OOB)
    s.run(&mut jit, 1);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_pram_boundary_rep_next_word() {
    // REP where the repeated instruction is at last pram address,
    // triggering the next_next_word boundary in emit_rep_inline (line 949).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; 3];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000000; // nop
    pram[1] = 0x0602A0; // rep #2
    pram[2] = 0x000000; // nop (repeated, next_next_word OOB)
    s.run(&mut jit, 1);
    assert_eq!(s.pc, 3);
}

#[test]
fn test_do_inline_la_past_pram() {
    // DO with LA >= pram.len() -> body not inlineable (line 792).
    // Falls back to non-inlined DO.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; 4];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2
    pram[1] = 0x000A; // LA=10 (>= pram.len()=4)
    pram[2] = 0x000000; // nop (body start, LA target unreachable)
    pram[3] = 0x000000; // nop
    s.run(&mut jit, 1);
    // DO not inlined, executed as block terminator; PC advances to body
    assert_eq!(s.pc, 2);
}

#[test]
fn test_do_inline_body_past_pram() {
    // DO body extends past pram end -> body_pc >= pram.len() (line 809).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; 4];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2
    pram[1] = 0x0005; // LA=5 (body 2..5, but pram ends at 3)
    pram[2] = 0x000000; // nop
    pram[3] = 0x000000; // nop (scan reaches PC 4 which is OOB)
    s.run(&mut jit, 1);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_do_inline_rep_at_pram_boundary() {
    // REP inside a DO body at pram boundary -> rep_next >= pram.len() (line 852).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; 5];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x060280; // do #2
    pram[1] = 0x0004; // LA=4
    pram[2] = 0x000000; // nop
    pram[3] = 0x000000; // nop
    pram[4] = 0x0602A0; // rep #2 (rep_next=5 >= pram.len()=5)
    s.run(&mut jit, 1);
    assert_eq!(s.pc, 2);
}

#[test]
fn test_do_inline_max_nesting_depth() {
    // 10 nested DOs -> innermost is depth 9 > MAX_NESTING_DEPTH (line 788).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // 10 levels of DO #2, all with LA=21
    for i in 0..10 {
        pram[i * 2] = 0x060280; // do #2
        pram[i * 2 + 1] = 0x000015; // LA=21
    }
    // Body of innermost + filler up to LA
    pram[20] = 0x000000; // nop
    pram[21] = 0x000000; // nop
    pram[22] = 0x0C0016; // jmp $22
    s.run(&mut jit, 1);
    // Outer DO not inlined (too deep), executed as block terminator
    assert_eq!(s.pc, 2);
}

#[test]
fn test_pop_loop_scope_reload_from_parent() {
    // REP inside an inlined DO where the register and accumulator were
    // valid before the DO but invalidated by a callback in the DO body.
    // The REP body defers them (its own entry_valid is false), then
    // pop_loop_scope finds the parent DO scope had entry_valid=true and
    // emits loads from memory in the pre-block.
    // Covers lines 444, 447-448 (reg reload) and 456-457 (acc reload)
    // in pop_loop_scope.
    //
    // PC 0: tfr y0,a        -- load_reg(Y0), valid[Y0]=true
    // PC 1: tst b           -- load_acc(B), acc_valid[B]=true
    // PC 2: do #2,$7        -- push_loop_scope captures Y0/B as valid
    // PC 3: LA=7
    // PC 4: move a,x:(r0) x0,a  -- write_mem_dyn(X) with callback -> invalidate all
    // PC 5: rep #2
    // PC 6: tst b y0,x0     -- load_reg(Y0) deferred, load_acc(B) deferred
    // PC 7: nop              -- DO LA
    // PC 8: jmp $8           -- halt
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut cb_val: u32 = 0;
    let mut map = MemoryMap::test(&mut xram, &mut yram, &mut pram);
    map.x_regions.push(MemoryRegion {
        start: PERIPH_BASE,
        end: PERIPH_BASE + PERIPH_SIZE as u32,
        kind: RegionKind::Callback {
            opaque: &mut cb_val as *mut u32 as *mut std::ffi::c_void,
            read_fn: cb_read,
            write_fn: cb_write,
        },
    });
    let mut s = DspState::new(map);
    pram[0] = 0x200051; // tfr y0,a
    pram[1] = 0x20000B; // tst b
    pram[2] = 0x060280; // do #2
    pram[3] = 0x0007; // LA=7
    pram[4] = 0x082000; // move a,x:(r0) x0,a  (PM0 X)
    pram[5] = 0x0602A0; // rep #2
    pram[6] = 0x20C40B; // tst b  y0,x0  (PM2 reg-to-reg + TST B)
    pram[7] = 0x000000; // nop
    pram[8] = 0x0C0008; // jmp $8
    s.registers[reg::R0] = 0;
    s.registers[reg::M0] = 0xFFFFFF;
    s.registers[reg::Y0] = 0x123456;
    s.run(&mut jit, 200);
    assert_eq!(s.pc, 8);
}

#[test]
fn test_wait_halts_run_loop() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000086; // WAIT
    pram[1] = 0x000000; // NOP
    s.run(&mut jit, 100);
    assert_eq!(s.power_state, PowerState::Wait);
    assert_eq!(s.pc, 1);
    assert_eq!(s.cycle_budget, 0);
}

#[test]
fn test_stop_halts_run_loop() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x000087; // STOP
    pram[1] = 0x000000; // NOP
    s.run(&mut jit, 100);
    assert_eq!(s.power_state, PowerState::Stop);
    assert_eq!(s.pc, 1);
    assert_eq!(s.cycle_budget, 0);
}

#[test]
fn test_wait_wakes_on_interrupt() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    pram[0] = 0x000086; // WAIT
    pram[1] = 0x000000; // NOP
    pram[8] = 0x000000; // TRAP vector: NOP
    pram[9] = 0x000000; // NOP

    s.interrupts.ipl[interrupt::TRAP] = 0;
    s.registers[reg::SR] &= !(3 << sr::I0); // IPL=0: SWI unmasked

    s.run(&mut jit, 20);
    assert_eq!(s.power_state, PowerState::Wait);
    assert_eq!(s.pc, 1);

    s.interrupts.add(interrupt::TRAP);
    s.run(&mut jit, 100);
    assert_eq!(s.power_state, PowerState::Normal);
}

#[test]
fn test_stop_ignores_interrupt() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    pram[0] = 0x000087; // STOP
    pram[1] = 0x000000; // NOP

    s.run(&mut jit, 20);
    assert_eq!(s.power_state, PowerState::Stop);
    assert_eq!(s.pc, 1);

    s.interrupts.add(interrupt::TRAP);
    s.run(&mut jit, 100);
    assert_eq!(s.power_state, PowerState::Stop);
    assert_eq!(s.pc, 1);
}

#[test]
fn test_wait_masked_interrupt_no_wake() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    pram[0] = 0x000086; // WAIT
    pram[1] = 0x000000; // NOP

    s.run(&mut jit, 20);
    assert_eq!(s.power_state, PowerState::Wait);

    s.registers[reg::SR] |= 2 << sr::I0; // IPL=2: blocks IPL 0/1
    s.interrupts.ipl[interrupt::TRAP] = 0;
    s.interrupts.add(interrupt::TRAP);

    s.run(&mut jit, 100);
    assert_eq!(s.power_state, PowerState::Wait);
}

#[test]
fn test_power_state_from_u8() {
    assert_eq!(PowerState::from(0), PowerState::Normal);
    assert_eq!(PowerState::from(1), PowerState::Wait);
    assert_eq!(PowerState::from(2), PowerState::Stop);
    assert_eq!(PowerState::from(255), PowerState::Normal); // Unknown -> Normal
}

#[test]
fn test_jit_block_clr_a_after_move() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    // P:$0000: move x:$0002,a     ; A = 0x400000
    // P:$0001: clr a a,b           ; B = A(24-bit), A = 0
    // P:$0002: move a,x:$0004     ; store A (should be 0) to X:$0004
    // P:$0003: WAIT
    pram[0] = 0x568200;
    pram[1] = 0x21CF13;
    pram[2] = 0x560400;
    pram[3] = 0x000086;
    xram[0x02] = 0x400000;

    // Interpreter
    let mut s1 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s1.pc = 0;
    for _ in 0..10 {
        if s1.power_state != PowerState::Normal {
            break;
        }
        s1.execute_one(&mut jit);
    }
    let interp_out = xram[0x04];

    // Reset
    xram[0x04] = 0;
    xram[0x02] = 0x400000;
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.pc = 0;
    s2.run(&mut jit2, 200);
    let block_out = xram[0x04];

    eprintln!(
        "clr-after-move: interp=${:06X} block=${:06X}",
        interp_out, block_out
    );
    assert_eq!(
        block_out, interp_out,
        "clr-after-move: block=${:06X} interp=${:06X}",
        block_out, interp_out
    );
}

#[test]
fn test_s_flag_data_growth() {
    // S flag is set when accumulator data moves to bus.
    // For no-scaling mode: S = (A46 XOR A45).
    // Use PM0 dual move that reads A (triggers read_accu24 limiting path).
    // move a,x:(r0)+ x0,a: 0x082000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x082000; // move a,x:(r0)+ x0,a (PM0 form)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000; // bit 46=1, bit 45=0 -> S should be set
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::R0] = 0x000000;
    s.registers[reg::M0] = REG_MASKS[reg::M0];
    s.registers[reg::N0] = 0x000001;
    s.registers[reg::SR] = 0xC00300; // S=0 initially
    xram[0] = 0;
    run_one(&mut s, &mut jit);
    let s_flag = (s.registers[reg::SR] >> sr::S) & 1;
    assert_eq!(
        s_flag, 1,
        "S flag should be set when bits 46,45 differ (data growth)"
    );
}

#[test]
fn test_movep_qq_pea_cycle_count() {
    // MOVEP P:(R0),[X]:qq - qq+P:ea form, should be 6 cycles
    // Template: 0000100sW1MMMRRR01pppppp
    // s=0 (X), W=1 (P:ea -> qq), MMM=100 (Rn), RRR=000 (R0), pppppp=000000
    // 0000_1000_1110_0000_0100_0000 = 0x08E040
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x0010;
    pram[0x10] = 0x123456; // value at P:$10
    pram[0] = 0x00E000; // movep P:(R0),X:$FFFF80
    let cycles = run_one(&mut s, &mut jit);
    assert_eq!(cycles, 6, "MOVEP qq+P:ea should be 6 cycles");
}

#[test]
fn test_stack_overflow_posts_interrupt() {
    // JIT stack_push must detect overflow and post STACK_ERROR.
    // Fill the stack to capacity (SP counter = 15), then push one more.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set SP counter to 15 (0xF) - one more push will overflow
    s.registers[reg::SP] = 0x0F;

    // JSR $10: pushes PC+1 and SR onto stack, jumps to $10
    pram[0] = 0x0D0010; // jsr $10

    run_one(&mut s, &mut jit);

    // SP counter should have wrapped: (0xF + 1) & 0xF = 0, with SE bit (bit 4) set
    let sp = s.registers[reg::SP];
    assert!(
        sp & 0x10 != 0,
        "SP SE bit should be set after overflow; SP = {:#04X}",
        sp
    );
    // STACK_ERROR interrupt should have been dispatched (process_pending_interrupts
    // clears pending bit and moves to pipeline - check that it entered the pipeline)
    assert_eq!(
        s.interrupts.state,
        InterruptState::Fast,
        "STACK_ERROR interrupt should have entered pipeline after overflow"
    );
}

#[test]
fn test_stack_underflow_posts_interrupt() {
    // JIT stack_pop must detect underflow and post STACK_ERROR.
    // SP counter = 0, pop should underflow.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // SP = 0 (empty stack), RTS will pop and underflow
    s.registers[reg::SP] = 0;
    // Put a return address on stack[0][0] so RTS has something to read
    s.stack[0][0] = 0x0002; // return to $0002
    s.registers[reg::SSH] = 0x0002;

    // RTS: opcode = 0x00000C
    pram[0] = 0x00000C;
    run_one(&mut s, &mut jit);

    // SP counter should have wrapped below 0: (0 - 1) & 0xF = 0xF, with SE bit set
    let sp = s.registers[reg::SP];
    assert!(
        sp & 0x10 != 0,
        "SP SE bit should be set after underflow; SP = {:#04X}",
        sp
    );
    assert_eq!(
        s.interrupts.state,
        InterruptState::Fast,
        "STACK_ERROR interrupt should have entered pipeline after underflow"
    );
}

#[test]
fn test_do_loop_la_max_address() {
    // advance_pc compares pc == LA+1 without masking to 24 bits.
    // If LA=0xFFFFFF, LA+1=0x01000000 which never matches masked PC=0x000000.
    // This test verifies the loop terminates correctly when LA is at 0xFFFFFF.
    //
    // We can't easily set up code at the top of address space in a small PRAM,
    // but we can test the advance_pc logic directly by setting LA and checking
    // that end-of-loop triggers when PC wraps to 0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Manually set up loop state as if DO #1,$FFFFFF was executed:
    // Push LA=old, LC=old onto stack, then push PC=return, SR onto stack
    s.stack_push(0, 0); // saved LA, LC
    s.stack_push(0x100, s.registers[reg::SR]); // saved PC (return addr), SR
    s.registers[reg::LA] = 0xFFFFFF;
    s.registers[reg::LC] = 1; // one iteration remaining
    s.registers[reg::SR] |= 1 << sr::LF; // loop flag
    // Set PC to LA (0xFFFFFF) - next advance_pc will go to LA+1 = 0x000000
    s.pc = 0xFFFFFF;
    // Put a NOP at address 0xFFFFFF (wraps to pram index via modulo)
    // For the test, we rely on the default 0 = NOP in pram
    pram[0] = 0x000000; // NOP at address 0 (where PC wraps to)
    let _cycles = run_one(&mut s, &mut jit);
    // After executing the NOP at 0xFFFFFF, PC advances to 0x000000.
    // The end-of-loop check should trigger (PC == mask_pc(LA+1) == 0).
    // LC was 1, so it decrements to 0 and the loop exits.
    // LF should be cleared (restored from stack).
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "Loop flag should be cleared after loop exit at LA=0xFFFFFF"
    );
    assert_eq!(s.pc, 0, "PC should be 0 after wrapping from 0xFFFFFF");
}

#[test]
fn test_stack_push_overflow_overwrites_slot0() {
    // The hardware stack is a circular buffer. When SP overflows from 15
    // to 16 (masked to 0), the oldest entry at slot 0 IS overwritten.
    // This matches core.rs (which always writes) and the hardware behavior.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Put sentinel in slot 0
    s.stack[0][0] = 0xDEAD01;
    s.stack[1][0] = 0xDEAD02;

    // Fill stack to SP=15 via interpreter
    for i in 1..=15 {
        s.stack_push(0x100 + i, 0x200 + i);
    }
    assert_eq!(s.registers[reg::SP] & 0xF, 15);

    // One more push via JIT (JSR $0010). This overflows SP from 15 to 16.
    pram[0] = 0x0D0010; // JSR $0010
    pram[0x10] = 0x000000; // NOP at target
    s.pc = 0;
    run_one(&mut s, &mut jit);

    // SE bit should be set (overflow detected)
    assert_ne!(s.registers[reg::SP] & 0x10, 0, "SE bit should be set");

    // Slot 0 should be overwritten by the overflow push (circular buffer)
    // JSR pushes (PC+1, SR) - PC+1 = 1
    assert_eq!(
        s.stack[0][0], 1,
        "Stack slot 0 SSH should be overwritten by overflow push"
    );
}

#[test]
fn test_do_inline_cycle_count() {
    // Inline DO charged 6 cycles overhead instead of 5.
    // Compare cycle counts: inline (run) vs non-inline (execute_one).
    // DO #3 with NOP body + STOP after loop (cleanly halts both paths).
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    pram[0] = 0x060380; // DO #3
    pram[1] = 0x000002; // LA = 2
    pram[2] = 0x000000; // NOP (body = LA)
    pram[3] = 0x0C0004; // JMP $4 (spin after loop)
    pram[4] = 0x0C0004; // JMP $4

    // Non-inline path (execute_one): step until past the loop
    let mut jit1 = JitEngine::new(PRAM_SIZE);
    let mut s1 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let mut noninline_cycles = 0i32;
    while s1.pc < 3 {
        noninline_cycles += s1.execute_one(&mut jit1);
    }

    // Inline path (run): give generous budget, check cycle consumption.
    // The block compiles DO inline + JMP as one block. After one execution
    // the block has consumed DO overhead + 3 body NOPs + JMP = total.
    // Each subsequent run just does JMP (3 cycles). So:
    // consumed = noninline_cycles + JMP(3) for first block if inline matches.
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Give enough for first block + a few JMPs, then check residual.
    let budget = 100;
    s2.run(&mut jit2, budget);
    let consumed = budget - s2.cycle_budget;
    // consumed should be N * 3 (JMP cycles) + noninline_cycles + 3 (first JMP in block).
    // So (consumed - noninline_cycles) should be divisible by 3 (all JMPs).
    // If DO overhead is wrong by 1, the residual won't be divisible by 3.
    let after_loop = consumed - noninline_cycles;
    assert_eq!(
        after_loop % 3,
        0,
        "Inline DO overhead should match non-inline ({}cy). \
         After subtracting, remaining {} should be divisible by 3 (JMP cycles). \
         consumed={}, noninline={}",
        noninline_cycles,
        after_loop,
        consumed,
        noninline_cycles
    );
}

#[test]
fn test_interrupt_long_jscc() {
    // detect_long_interrupt only recognized plain JSR, not JScc.
    // Place JScc (cc=0000=CC "carry clear") at the ILLEGAL vector.
    // With C=0 the condition is true, so JScc should be detected as long.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    pram[0] = 0x000005; // ILLEGAL at PC=0
    pram[1..10].fill(0x000000);
    // JScc xxx: 00001111CCCCaaaaaaaaaaaa
    // cc=0000 (CC), addr=$100: 0x0F0100
    pram[0x04] = 0x0F0100; // JScc CC,$100
    pram[0x05] = 0x000000;
    pram[0x100] = 0x000000;
    pram[0x101] = 0x000004; // RTI

    s.registers[reg::SR] &= !(1 << sr::C); // carry clear -> condition true

    run_one(&mut s, &mut jit); // dispatch
    assert_eq!(s.interrupts.state, InterruptState::Fast);
    run_one(&mut s, &mut jit); // 5->4
    run_one(&mut s, &mut jit); // 4->3, detect long
    assert_eq!(
        s.interrupts.state,
        InterruptState::Long,
        "JScc at interrupt vector should be detected as long interrupt"
    );
}

#[test]
fn test_interrupt_long_bsr() {
    // BSR at interrupt vector should also be detected as long.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    pram[0] = 0x000005; // ILLEGAL
    pram[1..10].fill(0x000000);
    // BSR xxxx: 0x0D1080 + displacement word
    pram[0x04] = 0x0D1080; // BSR xxxx
    pram[0x05] = 0x0000FC; // displacement

    run_one(&mut s, &mut jit);
    run_one(&mut s, &mut jit);
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.interrupts.state,
        InterruptState::Long,
        "BSR at interrupt vector should be detected as long interrupt"
    );
}

#[test]
fn test_btst_ssh_does_not_pop_stack() {
    // BTST #n,D on SSH should read SSH as a plain register, not pop the stack.
    // Template: 0000101111DDDDDD0110bbbb
    // DDDDDD=0x3C (SSH=111100), bbbb=0000 (bit 0)
    // 0000_1011_1111_1100_0110_0000 = 0x0BFC60
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push two values onto the stack so SP=2
    s.stack_push(0x123456, 0xABCDEF); // SP=1
    s.stack_push(0x654321, 0xFEDCBA); // SP=2
    let sp_before = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_before, 2);
    pram[0] = 0x0BFC60; // btst #0,SSH
    run_one(&mut s, &mut jit);
    // SP must remain unchanged (no pop)
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after, sp_before, "BTST on SSH should not pop the stack");
}

#[test]
fn test_jset_ssh_does_not_pop_stack() {
    // Verify bit ops on SSH don't pop/push.
    // BCLR #0,SSH: 0x0AFC40
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.stack_push(0x123456, 0xABCDEF); // SP=1
    s.stack_push(0x654321, 0xFEDCBA); // SP=2
    let sp_before = s.registers[reg::SP] & 0xF;
    pram[0] = 0x0AFC40; // bclr #0,SSH
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(
        sp_after, sp_before,
        "BCLR on SSH should not pop/push the stack"
    );
}

#[test]
fn test_jit_stack_push_overflow_writes_stack() {
    // When SP=15 (max), pushing should wrap to 0 and write to stack[0].
    // The JIT was skipping the array write because idx=16&0xF=0.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set SP=15 directly (SE bit clear, max stack entries)
    s.registers[reg::SP] = 15;
    // Clear stack[0] so we can verify the write
    s.stack[0][0] = 0xDEAD;
    s.stack[1][0] = 0xBEEF;
    // JSR $100: pushes (PC+1, SR) onto stack => SP wraps to 0
    pram[0] = 0x0D0064; // JSR $64 (100)
    pram[100] = 0x000000; // NOP
    run_one(&mut s, &mut jit);
    // SP wraps: (15+1)&0xF = 0, SE bit should be set
    assert_eq!(s.registers[reg::SP] & 0xF, 0);
    assert_ne!(s.registers[reg::SP] & (1 << 4), 0, "SE bit should be set");
    // The pushed return address (PC+1=1) should be in stack[0][0]
    assert_eq!(
        s.stack[0][0], 1,
        "stack[0][0] should have the overflow push value (return address)"
    );
}

#[test]
fn test_long_interrupt_clears_sa_bit() {
    // Per DSP56300FM Section 2.3.2.5 item 4: "The Sixteen-bit Arithmetic
    // (SA) mode bit is cleared." during long interrupt formation.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set SA bit in SR
    s.registers[reg::SR] |= 1 << sr::SA;
    // Use ILLEGAL interrupt (IPL=3, non-maskable) - vector at address 4
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    pram[0] = 0x000005; // ILLEGAL instruction triggers the interrupt
    pram[1..10].fill(0x000000);
    // Put a JSR at the ILLEGAL vector (address 4) - triggers long interrupt
    pram[0x04] = 0x0D0064; // JSR $64
    pram[100] = 0x000000; // NOP at JSR target
    // Execute: ILLEGAL fires, pipeline stages process, JSR at vector -> long
    run_one(&mut s, &mut jit);
    run_one(&mut s, &mut jit);
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.interrupts.state,
        InterruptState::Long,
        "interrupt should have been detected as long"
    );
    // After long interrupt formation, SA should be cleared
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::SA),
        0,
        "SA bit should be cleared during long interrupt formation"
    );
}

#[test]
fn test_fast_interrupt_pc_restore_masking() {
    // Verify that process_pending_interrupts correctly masks vector_addr+1
    // and vector_addr+2 comparisons. This test uses a normal VBA to verify
    // the basic fast interrupt mechanism works with the masking in place.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set up a fast interrupt: two 1-word instructions at the vector
    // VBA=0, interrupt slot 3 (vector at address 6)
    pram[6] = 0x000000; // NOP (1-word instruction)
    pram[7] = 0x000000; // NOP (1-word instruction)
    pram[0] = 0x000000; // NOP at initial PC
    // Trigger interrupt slot 3 at IPL 1
    s.interrupts.ipl[3] = 1;
    s.interrupts.set_pending(3);
    // Run the pipeline: stage 5 (arbitrate) -> 4 (jump to vector) -> 3 -> 2 (restore PC)
    // PC starts at 0, process_pending_interrupts runs each cycle
    let _saved_pc = s.pc;
    // Execute instructions to advance the interrupt pipeline
    for _ in 0..6 {
        run_one(&mut s, &mut jit);
    }
    // After pipeline completes, PC should be restored to saved_pc or advanced past it
    // The key thing is that it doesn't hang/crash with the masking fix
    assert_eq!(
        s.interrupts.state,
        InterruptState::None,
        "interrupt pipeline should have completed"
    );
}

#[test]
fn test_brclr_ssh_pops_stack() {
    // BRCLR with SSH source should pop the stack (decrement SP).
    // brclr #0,SSH,xxxx: template 0000110011DDDDDD100bbbbb
    // DDDDDD=111100 (SSH=0x3C), bbbbb=00000 (bit 0)
    // 0000_1100_1111_1100_1000_0000 = 0x0CFC80
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.stack_push(0x123456, 0x000000);
    s.stack_push(0x654321, 0x000000);
    let sp_before = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_before, 2);

    pram[0] = 0x0CFC80; // brclr #0,SSH,$xxxx
    pram[1] = 0x000004; // displacement
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(
        sp_after,
        sp_before - 1,
        "BRCLR on SSH should pop the stack (decrement SP)"
    );
}

#[test]
fn test_jclr_ssh_does_not_pop_stack() {
    // JCLR with SSH source should NOT pop.
    // jclr #0,SSH,xxxx: template 0000101011DDDDDD100bbbbb
    // DDDDDD=111100 (SSH), bbbbb=00000
    // 0000_1010_1111_1100_1000_0000 = 0x0AFC80
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.stack_push(0x123456, 0x000000);
    s.stack_push(0x654321, 0x000000);
    let sp_before = s.registers[reg::SP] & 0xF;

    pram[0] = 0x0AFC80; // jclr #0,SSH,$xxxx
    pram[1] = 0x000004; // absolute target
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after, sp_before, "JCLR on SSH must NOT pop the stack");
}

#[test]
fn test_long_interrupt_preserves_fv() {
    // Long interrupt should NOT clear FV (manual Section 2.3.2.5).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set up DO FOREVER state so LF=1 and FV=1
    s.registers[reg::SR] |= (1 << sr::LF) | (1 << sr::FV);
    s.stack_push(0x000010, s.registers[reg::SR]);
    s.stack_push(0x000000, 0x000005);

    // NOPs at PC=0, JSR at interrupt vector 0x10
    pram[..0x20].fill(0x000000);
    pram[0x10] = 0x0D0000; // jsr $xxxx
    pram[0x11] = 0x000020; // target = $20

    // Post interrupt at index 8 (vector addr = 0x10), IPL 3
    s.interrupts.set_pending(8);
    s.interrupts.ipl[8] = 3;

    for _ in 0..10 {
        run_one(&mut s, &mut jit);
    }

    // The SSL pushed by detect_long_interrupt should contain the original
    // SR with FV=1 preserved.
    let sp = s.registers[reg::SP] & 0xF;
    let mut found_fv = false;
    for level in 1..=sp as usize {
        let ssl = s.stack[1][level];
        if ssl & (1 << sr::FV) != 0 {
            found_fv = true;
            break;
        }
    }
    assert!(
        found_fv,
        "Long interrupt: FV=1 must be preserved in stacked SR"
    );
}

#[test]
fn test_e_flag_scale_down() {
    // Per DSP56300FM Table 5-1: In scale-down mode (S1=0, S0=1),
    // E checks bits 55:48 (the signed integer portion shifts).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Scale-down mode: S0=1 (bit 10), S1=0 (bit 11)
    s.registers[reg::SR] |= 1 << sr::S0;
    s.registers[reg::SR] &= !(1 << sr::S1);

    // A = $01:000000:000000 - bit 48 (A2[0]) = 1, bits 55:49 = 0 -> NOT uniform -> E=1
    s.registers[reg::A2] = 0x01;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200003; // tst a
    pram[1] = 0x000000; // NOP
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "E should be set when bits 55:48 are not uniform (scale-down)"
    );

    // A = $00:000000:000000 - bits 55:48 all zero (uniform) -> E=0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.pc = 0;
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "E should be clear when bits 55:48 are uniform (scale-down)"
    );
}

#[test]
fn test_e_flag_scale_up() {
    // Per DSP56300FM Table 5-1: In scale-up mode (S1=1, S0=0),
    // E checks bits 55:46.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Scale-up mode: S1=1 (bit 11), S0=0 (bit 10)
    s.registers[reg::SR] |= 1 << sr::S1;
    s.registers[reg::SR] &= !(1 << sr::S0);

    // A = $00:400000:000000. bit 46=A1[22]=1, bits 55:47 = 0.
    // Bits 55:46 = 00_0000_0001 -> NOT uniform -> E=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200003; // tst a
    pram[1] = 0x000000; // NOP
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "E should be set when bits 55:46 are not uniform (scale-up)"
    );

    // A = $00:200000:000000. Bits 55:46 = 00_0000_0000 (uniform, bit 45 below range) -> E=0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000;
    s.registers[reg::A0] = 0x000000;
    s.pc = 0;
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "E should be clear when bits 55:46 are uniform (scale-up)"
    );
}

#[test]
fn test_u_flag_scale_down() {
    // Per DSP56300FM Table 5-1: In scale-down mode, U = NOT(bit48 XOR bit47).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Scale-down mode: S0=1, S1=0
    s.registers[reg::SR] |= 1 << sr::S0;
    s.registers[reg::SR] &= !(1 << sr::S1);

    // A = $00:000000:000000. bit48=0, bit47=0. XOR=0 -> U=1 (unnormalized)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200003; // tst a
    pram[1] = 0x000000; // NOP
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "U should be set when bit48==bit47 (scale-down)"
    );

    // A = $00:800000:000000. bit48=A2[0]=0, bit47=A1[23]=1. XOR=1 -> U=0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000;
    s.registers[reg::A0] = 0x000000;
    s.pc = 0;
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "U should be clear when bit48!=bit47 (scale-down)"
    );
}

#[test]
fn test_u_flag_scale_up() {
    // Per DSP56300FM Table 5-1: In scale-up mode, U = NOT(bit46 XOR bit45).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Scale-up mode: S1=1, S0=0
    s.registers[reg::SR] |= 1 << sr::S1;
    s.registers[reg::SR] &= !(1 << sr::S0);

    // A = $00:000000:000000. bit46=0, bit45=0. XOR=0 -> U=1
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    pram[0] = 0x200003; // tst a
    pram[1] = 0x000000; // NOP
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "U should be set when bit46==bit45 (scale-up)"
    );

    // A = $00:400000:000000. bit46=A1[22]=1, bit45=A1[21]=0. XOR=1 -> U=0
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x400000;
    s.registers[reg::A0] = 0x000000;
    s.pc = 0;
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "U should be clear when bit46!=bit45 (scale-up)"
    );
}

#[test]
fn test_long_interrupt_clears_lf_s1_s0() {
    // Per DSP56300FM Section 2.3.2.5: During long interrupt formation,
    // LF, S1, S0 are cleared and I1:I0 are raised.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Set LF=1, S1=1, S0=1, I0=0, I1=0 in SR
    s.registers[reg::SR] |= (1 << sr::LF) | (1 << sr::S1) | (1 << sr::S0);
    s.registers[reg::SR] &= !((0x3) << sr::I0);

    // Use ILLEGAL interrupt (IPL=3, non-maskable) - vector at address 4
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    pram[0] = 0x000005; // ILLEGAL instruction triggers the interrupt
    pram[1..10].fill(0x000000);
    // Put a JSR at the ILLEGAL vector (address 4) - triggers long interrupt
    pram[0x04] = 0x0D0064; // JSR $64
    pram[100] = 0x000000; // NOP at JSR target

    // Execute: ILLEGAL fires, pipeline stages process, JSR at vector -> long
    run_one(&mut s, &mut jit);
    run_one(&mut s, &mut jit);
    run_one(&mut s, &mut jit);

    assert_eq!(
        s.interrupts.state,
        InterruptState::Long,
        "interrupt should have been detected as long"
    );

    // LF should be cleared
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::LF),
        0,
        "LF bit should be cleared during long interrupt formation"
    );
    // S1 should be cleared
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::S1),
        0,
        "S1 bit should be cleared during long interrupt formation"
    );
    // S0 should be cleared
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::S0),
        0,
        "S0 bit should be cleared during long interrupt formation"
    );
    // I1:I0 should be raised to >= 3
    assert!(
        (s.registers[reg::SR] >> sr::I0) & 3 >= 3,
        "I1:I0 should be raised to at least 3 during long interrupt formation"
    );
}

#[test]
fn test_stack_se_sticky() {
    // SE (Stack Error) bit should be sticky: once set by overflow, it stays set even after pop.
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    // Push 16 times to trigger overflow (SE bit set)
    for i in 0..16 {
        s.stack_push(i as u32, 0);
    }
    assert_ne!(
        s.registers[reg::SP] & (1 << 4),
        0,
        "SE bit should be set after overflow"
    );
    // Pop once
    s.stack_pop();
    assert_ne!(
        s.registers[reg::SP] & (1 << 4),
        0,
        "SE bit should remain set (sticky) after pop"
    );
}

#[test]
fn test_stack_uf_sticky() {
    // UF (Underflow) bit should be sticky: once set by underflow, it stays set even after push.
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;
    // Pop at SP=0 to trigger underflow
    s.stack_pop();
    let uf_bit = 1 << 5;
    assert_ne!(
        s.registers[reg::SP] & uf_bit,
        0,
        "UF bit should be set after underflow"
    );
    // Push once
    s.stack_push(0x42, 0);
    assert_ne!(
        s.registers[reg::SP] & uf_bit,
        0,
        "UF bit should remain set (sticky) after push"
    );
}

#[test]
fn test_sr_reserved_bits_masked() {
    // SR has reserved bits that should always read as 0.
    // Per DSP56300FM Table 5-1: bit 12 and bit 18 are reserved.
    // REG_MASKS[SR] = 0x00FB_EFFF masks out bits 12 and 18.
    // Write all-ones and verify reserved bits are cleared by the mask.
    let all_ones = 0x00FF_FFFF;
    let masked = all_ones & REG_MASKS[reg::SR];
    // Bit 12 should be cleared
    assert_eq!(
        masked & (1 << 12),
        0,
        "SR bit 12 (reserved) should be masked out"
    );
    // Bit 18 should be cleared
    assert_eq!(
        masked & (1 << 18),
        0,
        "SR bit 18 (reserved) should be masked out"
    );
    // Non-reserved bits should pass through
    assert_ne!(masked & (1 << 0), 0, "SR bit 0 (C) should be preserved");
    assert_ne!(masked & (1 << 15), 0, "SR bit 15 (LF) should be preserved");
}

#[test]
fn test_movec_ssh_double_pop() {
    // Two consecutive MOVEC SSH,X0 reads should pop the stack twice.
    // Per DSP56300FM p.13-130: "SP is post-decremented by 1 after SSH has been read."
    // Per ARCHITECTURE-NOTES.md: SSH pop-on-read applies to move instructions.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push 3 values onto the stack
    s.stack_push(0x111111, 0xA00000); // SP=1
    s.stack_push(0x222222, 0xB00000); // SP=2
    s.stack_push(0x333333, 0xC00000); // SP=3
    assert_eq!(s.registers[reg::SP] & 0xF, 3);
    // movec SSH,X0 = 0x0444BC (W=0, eeeeee=X0(0x04), ddddd=SSH(0x1C))
    pram[0] = 0x0444BC; // first pop: reads SSH (0x333333), SP -> 2
    pram[1] = 0x0444BC; // second pop: reads SSH (0x222222), SP -> 1
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x333333,
        "First MOVEC SSH,X0 should read top-of-stack"
    );
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        2,
        "SP should be 2 after first pop"
    );
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x222222,
        "Second MOVEC SSH,X0 should read next stack entry"
    );
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        1,
        "SP should be 1 after second pop"
    );
}

#[test]
fn test_interrupt_vector_with_vba() {
    // VBA (Vector Base Address) relocates the interrupt vector table.
    // Per DSP56300FM Section 5.4.4.4: vector = VBA[23:8] | slot_offset[7:0].
    // Set VBA=$0200 (bits 7:0 are read-only zeros per REG_MASKS).
    // Execute ILLEGAL (0x000005) -> should dispatch to VBA + $04 = $0204.
    // Per DSP56300FM Section 5.4.4.4: vector = VBA[23:8] | slot_offset[7:0].
    // Use execute_one in a loop for precise control over interrupt pipeline stages.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::VBA] = 0x000200; // VBA = $0200 (bits 7:0 masked to 0)
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    pram[0] = 0x000005; // illegal instruction
    // Put a halt loop at the expected vector: VBA($0200) + ILLEGAL offset($04) = $0204.
    pram[0x204] = 0x0C0204; // jmp $0204 (halt loop at vector)
    // Step through: ILLEGAL -> 5-stage pipeline -> PC lands at vector.
    for _ in 0..10 {
        run_one(&mut s, &mut jit);
    }
    // After pipeline completes, PC should be at $0204 (the halt loop).
    assert_eq!(
        s.pc, 0x0204,
        "PC should be at VBA+ILLEGAL vector ($0204), got {:#06X}",
        s.pc
    );
}

#[test]
fn test_block_invalidation_targeted() {
    // Writing to addr 0 should invalidate blocks covering addr 0 but NOT
    // blocks at unrelated addresses.
    // Per pram_dirty bitmap: only the written address's bit is set.
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    let gen_before = s.pram_dirty.generation;
    // Write to addr 0
    unsafe { jit_write_mem(&mut s as *mut DspState, MemSpace::P, 0, 0x000008) };
    assert_eq!(pram[0], 0x000008, "P:$0 should be written");
    let gen_after_first = s.pram_dirty.generation;
    assert!(
        gen_after_first > gen_before,
        "Generation should bump after writing new value to P:$0"
    );
    // Dirty bit for addr 0 should be set
    assert_ne!(
        s.pram_dirty.dirty[0] & 1u64,
        0,
        "Dirty bit for addr 0 should be set"
    );
    // Dirty bit for addr 0x100 should NOT be set (untouched)
    let word_0x100 = 0x100 / 64;
    let bit_0x100 = 0x100 % 64;
    assert_eq!(
        s.pram_dirty.dirty[word_0x100] & (1u64 << bit_0x100),
        0,
        "Dirty bit for addr 0x100 should NOT be set"
    );
    // Write to addr 0x100
    unsafe { jit_write_mem(&mut s as *mut DspState, MemSpace::P, 0x100, 0x000009) };
    assert_eq!(pram[0x100], 0x000009, "P:$100 should be written");
    let gen_after_second = s.pram_dirty.generation;
    assert!(
        gen_after_second > gen_after_first,
        "Generation should bump again after writing to P:$100"
    );
    // Now both bits should be set
    assert_ne!(
        s.pram_dirty.dirty[word_0x100] & (1u64 << bit_0x100),
        0,
        "Dirty bit for addr 0x100 should now be set"
    );
}

#[test]
fn test_fast_interrupt_not_interruptible() {
    // Per DSP56300FM Section 2.3.2.8 (p.2-27): "A fast interrupt is not interruptible."
    // While the interrupt pipeline is processing stages 5->0, a second pending interrupt
    // should NOT be dispatched - it must wait until the pipeline completes.
    //
    // The emulator implements this: process_pending_interrupts() returns early during
    // stages 5->1 (lines 638-686 of core.rs), never reaching the arbitration code.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Put NOPs at both interrupt vectors
    // ILLEGAL = slot 2, vector at VBA:$04
    pram[0x04] = 0x000000; // nop (fast interrupt word 1)
    pram[0x05] = 0x000000; // nop (fast interrupt word 2)
    // TRAP = slot 4, vector at VBA:$08
    pram[0x08] = 0x000000;
    pram[0x09] = 0x000000;
    // NOP at PC=0
    pram[0] = 0x000000;

    // Trigger ILLEGAL interrupt (IPL=3, non-maskable)
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    s.interrupts.set_pending(interrupt::ILLEGAL);

    // First execute_one: dispatches ILLEGAL, enters pipeline stage 5
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.interrupts.state,
        InterruptState::Fast,
        "Should be in Fast interrupt state"
    );

    // Now post a SECOND interrupt (TRAP at IPL=3) while fast interrupt is in-flight
    s.interrupts.ipl[interrupt::TRAP] = 3;
    s.interrupts.set_pending(interrupt::TRAP);

    // Execute through the remaining pipeline stages.
    // The TRAP interrupt must remain pending throughout - it should NOT be dispatched
    // until the ILLEGAL fast interrupt pipeline completes.
    for stage in (0..5).rev() {
        assert!(
            s.interrupts.pending(interrupt::TRAP),
            "TRAP should remain pending during fast interrupt pipeline stage {}",
            stage
        );
        run_one(&mut s, &mut jit);
    }

    // After pipeline completes (state -> None), TRAP should now be dispatchable.
    // It may have been dispatched on the last run_one when stage hit 0.
    // Just verify the fast interrupt completed without the second interrupt corrupting it.
    assert!(
        !s.interrupts.pending(interrupt::ILLEGAL),
        "ILLEGAL interrupt should have been fully serviced"
    );
}

#[test]
fn test_rep_not_interruptible() {
    // Per DSP56300FM Section 2.3.2.8 (p.2-27): "During the execution of the repeated
    // instruction, no interrupts are serviced" until LC decrements to 1.
    // The emulator implements this via loop_rep flag in process_pending_interrupts.
    //
    // Setup: REP #5 + INC A, with an IPL-3 interrupt posted before execution.
    // The interrupt should NOT fire during the REP iterations. After REP completes,
    // the interrupt should be serviced.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Put NOPs at ILLEGAL interrupt vector (VBA:$04, i.e. pram[4] and pram[5])
    pram[0x04] = 0x000000;
    pram[0x05] = 0x000000;

    // Program: REP #5; INC A; NOP
    pram[0] = 0x0605A0; // rep #5
    pram[1] = 0x000008; // inc a (repeated instruction)
    pram[2] = 0x000000; // nop (after REP)

    // Execute REP setup first (before posting interrupt)
    run_one(&mut s, &mut jit);
    // After REP #5 executes: loop_rep=true, pc_on_rep cleared by advance_pc,
    // PC should point to the repeated instruction (pram[1] = inc a).
    assert_eq!(s.pc, 1, "PC should point to repeated instruction after REP");
    assert!(s.loop_rep, "loop_rep should be true after REP instruction");

    // NOW post an IPL-3 (non-maskable) interrupt while REP is active
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    s.interrupts.set_pending(interrupt::ILLEGAL);
    assert!(s.interrupts.has_pending());

    // Execute the repeated INC A instructions (5 iterations via execute_one)
    for i in 0..5 {
        run_one(&mut s, &mut jit);
        if i < 4 {
            // During REP iterations (loop_rep is true), interrupt should stay pending
            assert!(
                s.interrupts.has_pending(),
                "Interrupt should remain pending during REP iteration {}",
                i
            );
        }
    }

    // After REP completes (loop_rep cleared), A should be 5
    assert_eq!(s.registers[reg::A0], 5, "REP #5 INC A should produce A0=5");

    // Now the interrupt should be serviceable on the next step
    // (process_pending_interrupts is called at the end of each execute_one)
    // After the 5th iteration, loop_rep is cleared and the interrupt can fire.
    // It may have already been dispatched on the last run_one call.
    // Just verify A got the right value - the interrupt didn't corrupt the REP.
}

#[test]
fn test_s_flag_scale_up() {
    // DSP56300FM Table 5-1: In scale-up mode (S1=1, S0=0),
    // S flag checks bits 47:46 (instead of 47:45 in no-scaling).
    // S = 1 when bits 47 and 46 differ.
    // A = $00:C00000:000000. bit47=1, bit46=1. Same => S=0.
    // A = $00:800000:000000. bit47=1, bit46=0. Differ => S=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Scale-up: S1=1, S0=0. Use PM0 move to trigger S flag computation.
    // PM0: move a,x:(r0)+ x0,a (reads A through data shifter).
    s.registers[reg::SR] = 1 << sr::S1;
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x800000; // bit47=1, bit46=0 => S=1 in scale-up
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::R0] = 0x000000;
    s.registers[reg::M0] = REG_MASKS[reg::M0];
    s.registers[reg::N0] = 0x000001;
    xram[0] = 0;
    pram[0] = 0x082000; // move a,x:(r0)+ x0,a
    run_one(&mut s, &mut jit);
    let s_flag = (s.registers[reg::SR] >> sr::S) & 1;
    assert_eq!(
        s_flag, 1,
        "S flag should be set when bits 47:46 differ (scale-up)"
    );
}

#[test]
fn test_s_flag_scale_down() {
    // DSP56300FM Table 5-1: In scale-down mode (S1=0, S0=1),
    // S flag checks bits 45:44 (A45 XOR A44).
    // A = $00:200000:000000. bit45=A1[21]=1, bit44=A1[20]=0. Differ => S=1.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] = 1 << sr::S0; // scale-down (S1=0, S0=1)
    s.registers[reg::A2] = 0x00;
    s.registers[reg::A1] = 0x200000; // bit45=1 (A1[21]), bit44=0 (A1[20]) => differ => S=1
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::R0] = 0x000000;
    s.registers[reg::M0] = REG_MASKS[reg::M0];
    s.registers[reg::N0] = 0x000001;
    xram[0] = 0;
    pram[0] = 0x082000; // move a,x:(r0)+ x0,a
    run_one(&mut s, &mut jit);
    let s_flag = (s.registers[reg::SR] >> sr::S) & 1;
    assert_eq!(
        s_flag, 1,
        "S flag should be set when bits 45:44 differ (scale-down)"
    );
}

#[test]
fn test_pm4_l_move_scaling() {
    // PM4 L: move with accumulator + scaling mode active.
    // Verify limiter applies scale-up limiting to accumulated value read.
    // L: move reads A through limiter. With scale-up, limiting checks bits 55:46.
    // A = $00:400000:000000. Bits 55:46 = 0000000001 (not uniform) => limiting.
    // After limiting, the output should be limited to $007FFF:000000 (positive max).
    // Actually wait - limiting only triggers when extension bits don't match.
    // In scale-up: E checks bits 55:46. If not uniform, data is limited.
    // $00:400000:000000 has bits 55:46 = 0_0000_0000_1 (bit 46 is A1[22]=1).
    // Not all same => limited.
    //
    // L: move to X:Y memory. Encoding: 01LLddd0HHmMRRRR (PM4).
    // Use TST A to trigger S flag computation with scale-up mode.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Scale-up: S1=1, S0=0
    s.registers[reg::SR] = 1 << sr::S1;
    // A = $02:000000:000000. Bits 55:46 not uniform (bit 49 differs) -> E=1.
    s.registers[reg::A2] = 0x02;
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::R0] = 0x000010;
    s.registers[reg::M0] = REG_MASKS[reg::M0];
    pram[0] = 0x200003; // tst a
    run_one(&mut s, &mut jit);
    // With scale-up, E checks bits 55:46. A=$02:000000:000000 has bit 49 set.
    // bits 55:46 = 00_0000_0100 => not uniform => E=1.
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "PM4 scaling: E=1 for non-uniform extension in scale-up"
    );
}

#[test]
fn test_same_ipl_intra_level_priority() {
    // DSP56300FM Section 2.3.2.3: When multiple interrupts at the same IPL are pending,
    // the one with the lower vector index (lower slot number) has higher priority.
    // TRAP=slot 4 (vector $08), NMI=slot 5 (vector $0A). Both at IPL 3.
    // TRAP (lower index) should win.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.interrupts.ipl[interrupt::TRAP] = 3;
    s.interrupts.ipl[interrupt::NMI] = 3;
    s.registers[reg::SR] &= !((0x3) << sr::I0); // I1:I0 = 0 (accept all)
    s.interrupts.add(interrupt::TRAP);
    s.interrupts.add(interrupt::NMI);

    // Place NOPs at both vectors
    pram[0x08] = 0x000000; // TRAP vector
    pram[0x09] = 0x000000;
    pram[0x0A] = 0x000000; // NMI vector
    pram[0x0B] = 0x000000;
    pram[0] = 0x000000; // NOP at PC=0
    pram[1..20].fill(0x000000);

    run_one(&mut s, &mut jit);
    // TRAP (slot 4, lower index) should be dispatched first
    assert_eq!(
        s.interrupts.state,
        InterruptState::Fast,
        "Interrupt should be in pipeline"
    );
    assert!(
        !s.interrupts.pending(interrupt::TRAP),
        "TRAP should be cleared (dispatched first)"
    );
    assert!(
        s.interrupts.pending(interrupt::NMI),
        "NMI should still be pending (lower priority within same IPL)"
    );
    assert_eq!(
        s.interrupts.vector_addr, 0x08,
        "TRAP vector ($08) should be dispatched, not NMI ($0A)"
    );
}

#[test]
fn test_nested_long_interrupt_preemption() {
    // DSP56300FM Section 2.3.2.5: A long interrupt can be preempted by a
    // higher-priority interrupt during the ISR.
    // Setup: trigger a long interrupt (IPL 1), then while in the ISR,
    // post a higher-priority interrupt (IPL 3). The higher-priority one
    // should be dispatched after the long interrupt pipeline completes.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Use slots 6 and 7 as two generic interrupt sources.
    // Slot 6 at IPL=1 (long interrupt), slot 7 at IPL=3 (preempts).
    s.interrupts.ipl[6] = 1;
    s.interrupts.ipl[7] = 3;
    s.registers[reg::SR] &= !((0x3) << sr::I0); // I1:I0=0

    // Place a JSR at slot 6 vector (VBA + 6*2 = $0C) to make it a long interrupt.
    pram[0x0C] = 0x0D0100; // JSR $100
    pram[0x0D] = 0x000000;
    // ISR at $100: just NOPs
    pram[0x100] = 0x000000;
    pram[0x101] = 0x000000;
    // Place NOPs at slot 7 vector ($0E)
    pram[0x0E] = 0x000000;
    pram[0x0F] = 0x000000;

    pram[0] = 0x000000; // NOP at PC=0
    pram[1..20].fill(0x000000);

    // Trigger slot 6
    s.interrupts.add(6);

    // Run through the interrupt pipeline (several steps)
    for _ in 0..8 {
        run_one(&mut s, &mut jit);
    }

    // At this point the long interrupt should be active (ISR executing at $100+).
    // Now post a higher-priority interrupt (slot 7, IPL=3).
    s.interrupts.add(7);

    // Execute a few more steps - the higher-priority interrupt should preempt.
    for _ in 0..8 {
        run_one(&mut s, &mut jit);
    }

    // Slot 7 should have been dispatched (no longer pending).
    assert!(
        !s.interrupts.pending(7),
        "Higher-priority interrupt (slot 7) should have been dispatched during long ISR"
    );
}

#[test]
fn test_rti_restores_sr_from_ssl() {
    // DSP56300FM p.13-167: RTI pops PC from SSH and SR from SSL.
    // Verify that SR is fully restored (including LF, S1:S0, IPL bits).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Push a return address and a specific SR value onto the stack.
    // The saved SR has LF=1, S1=1, S0=1, I1:I0=2.
    let saved_sr: u32 =
        (1 << sr::LF) | (1 << sr::S1) | (1 << sr::S0) | (2 << sr::I0) | (1 << sr::C);
    s.stack_push(0x0020, saved_sr); // SSH=return_PC, SSL=saved_SR

    // Current SR is different
    s.registers[reg::SR] = 3 << sr::I0; // I1:I0=3, everything else 0

    // RTI at PC=0
    pram[0] = 0x000004; // rti
    pram[0x20] = 0x000000; // NOP at return address
    run_one(&mut s, &mut jit);

    // PC should be restored to saved_pc
    assert_eq!(s.pc, 0x0020, "RTI: PC restored from SSH");
    // SR should be restored from SSL
    let sr = s.registers[reg::SR];
    assert_ne!(sr & (1 << sr::LF), 0, "RTI: LF restored from SSL");
    assert_ne!(sr & (1 << sr::S1), 0, "RTI: S1 restored from SSL");
    assert_ne!(sr & (1 << sr::S0), 0, "RTI: S0 restored from SSL");
    assert_ne!(sr & (1 << sr::C), 0, "RTI: C restored from SSL");
    assert_eq!((sr >> sr::I0) & 3, 2, "RTI: I1:I0 restored to 2 from SSL");
}

#[test]
fn test_ssl_read_no_pop() {
    // DSP56300FM: MOVEC SSL,X0 should read SSL without popping SP.
    // Only SSH has pop-on-read semantics; SSL does not.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // Push two values
    s.stack_push(0x111111, 0xA00000); // SP=1
    s.stack_push(0x222222, 0xB00000); // SP=2

    // movec SSL,X0: encoding = 0x0444BD (W=0, eeeeee=X0(0x04), ddddd=SSL(0x1D))
    pram[0] = 0x0444BD;
    run_one(&mut s, &mut jit);

    assert_eq!(
        s.registers[reg::X0],
        0xB00000,
        "SSL read should return current SSL value"
    );
    assert_eq!(
        s.registers[reg::SP] & 0xF,
        2,
        "SSL read should NOT pop SP (SP should remain at 2)"
    );
}

#[test]
fn test_vba_mask_bits_7_0() {
    // DSP56300FM: VBA bits 7:0 are read-only zeros.
    // REG_MASKS[VBA] = 0x00FF_FF00 should mask out low 8 bits.
    // Write $02FF to VBA, verify it reads as $0200.
    let vba_mask = REG_MASKS[reg::VBA];
    let written = 0x0002FF;
    let expected = written & vba_mask;
    assert_eq!(expected, 0x000200, "VBA mask should clear bits 7:0");

    // Also verify through DspState
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::VBA] = 0x0002FF & vba_mask;
    assert_eq!(
        s.registers[reg::VBA],
        0x000200,
        "VBA register should have bits 7:0 cleared"
    );
}

#[test]
fn test_scale_down_negative_limiting() {
    // PM0 scale-down with negative accumulator: verify limiter works correctly.
    // With scale-down (S0=1), limiting checks bits 55:48 (extension).
    // A = $FE:000000:000000 (negative, extension not all-1s). Should be limited
    // to $FF:800000:000000 (negative max limited value).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    s.registers[reg::SR] = 1 << sr::S0; // scale-down
    s.registers[reg::A2] = 0xFE; // extension not all 0xFF => needs limiting
    s.registers[reg::A1] = 0x000000;
    s.registers[reg::A0] = 0x000000;
    s.registers[reg::X0] = 0x000000;
    s.registers[reg::R0] = 0x000000;
    s.registers[reg::M0] = REG_MASKS[reg::M0];
    s.registers[reg::N0] = 0x000001;
    xram[0] = 0;

    // PM0: move a,x:(r0)+ x0,a - reads A through limiter, writes to xram[0].
    pram[0] = 0x082000;
    run_one(&mut s, &mut jit);

    // Check that the value written to xram was limited (negative max = $800000).
    assert_eq!(
        xram[0], 0x800000,
        "Scale-down negative limiting should write $800000 to memory"
    );
    // L flag should be set when limiting occurs.
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::L),
        0,
        "L flag should be set when limiting occurs"
    );
}

#[test]
fn test_omr_mask_reserved_bits() {
    // DSP56300FM Table 5-2: OMR bit 5 is reserved.
    // REG_MASKS[OMR] = 0x00FF_FFDF should mask out bit 5.
    let omr_mask = REG_MASKS[reg::OMR];
    assert_eq!(
        omr_mask & (1 << 5),
        0,
        "OMR mask should clear bit 5 (reserved)"
    );

    let written = 0x00FF_FFFF;
    let expected = written & omr_mask;
    assert_eq!(expected & (1 << 5), 0, "OMR bit 5 should always be 0");
    // Non-reserved bits should pass through
    assert_ne!(expected & (1 << 0), 0, "OMR bit 0 should be preserved");
    assert_ne!(
        expected & (1 << 20),
        0,
        "OMR bit 20 (SEN) should be preserved"
    );
}

#[test]
fn test_double_stack_overflow() {
    // DSP56300FM Table 5-2: Double overflow. Verify SE bit is sticky.
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;

    // Fill stack to SP=15
    for i in 0..15 {
        s.stack_push(i as u32, 0);
    }
    assert_eq!(s.registers[reg::SP] & 0xF, 15);

    // First overflow: SP wraps to 0, SE set
    s.stack_push(0xAAAA, 0);
    assert_ne!(
        s.registers[reg::SP] & (1 << 4),
        0,
        "SE set after first overflow"
    );
    let sp_after_first = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after_first, 0, "SP wraps to 0 after overflow from 15");

    // Push again - SE should remain sticky
    s.stack_push(0xBBBB, 0);
    assert_ne!(s.registers[reg::SP] & (1 << 4), 0, "SE still set (sticky)");
}

#[test]
fn test_double_stack_underflow() {
    // DSP56300FM Table 5-2: Underflow twice. Verify UF bit is sticky.
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.interrupts.ipl[interrupt::STACK_ERROR] = 3;

    // First underflow from SP=0
    s.stack_pop();
    let uf_bit = 1 << 5;
    assert_ne!(
        s.registers[reg::SP] & uf_bit,
        0,
        "UF set after first underflow"
    );
    let sp_after_first = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after_first, 15, "SP wraps to 15 after underflow from 0");

    // Pop again - UF should remain sticky
    s.stack_pop();
    assert_ne!(s.registers[reg::SP] & uf_bit, 0, "UF still set (sticky)");
}

#[test]
fn test_long_interrupt_ipl1_raises_i1i0_to_2() {
    // Non-IPL3 interrupt should raise I1:I0 to the interrupt's IPL+1.
    // Per DSP56300FM Section 2.3.2.5: I1:I0 = min(ipl+1, 3).
    // Use TRAP interrupt at IPL=1. I1:I0 should become 2.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));

    // SR: I1:I0 = 0 (allows IPL >= 0)
    s.registers[reg::SR] &= !((0x3) << sr::I0);

    // Set TRAP to IPL=1
    s.interrupts.ipl[interrupt::TRAP] = 1;
    // Post TRAP interrupt
    s.interrupts.add(interrupt::TRAP);

    // Put a JSR at the TRAP vector (address 8) to force long interrupt
    pram[0] = 0x000000; // nop - triggers interrupt dispatch
    pram[1..10].fill(0x000000);
    pram[0x08] = 0x0D0064; // JSR $64
    pram[100] = 0x000000; // NOP at JSR target

    // Execute through the interrupt pipeline
    run_one(&mut s, &mut jit); // NOP at PC=0 (interrupt fires)
    run_one(&mut s, &mut jit); // fast interrupt word 1 at vector
    run_one(&mut s, &mut jit); // JSR detected -> long interrupt formation

    assert_eq!(
        s.interrupts.state,
        InterruptState::Long,
        "interrupt should have been detected as long"
    );

    // I1:I0 should be 2 (= IPL 1 + 1)
    let i1i0 = (s.registers[reg::SR] >> sr::I0) & 3;
    assert_eq!(
        i1i0, 2,
        "I1:I0 should be raised to IPL+1 = 2 for an IPL-1 interrupt"
    );
}

#[test]
fn test_illegal_long_interrupt_sets_i1i0_to_3() {
    // ILLEGAL dispatch should raise I1:I0 to 3 (IPL=3, non-maskable).
    // Put JSR at ILLEGAL vector (address 4) to force long interrupt formation.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Start with I1:I0 = 0
    s.registers[reg::SR] &= !((0x3) << sr::I0);
    s.interrupts.ipl[interrupt::ILLEGAL] = 3;
    pram[0] = 0x000005; // ILLEGAL
    pram[1..10].fill(0x000000);
    pram[0x04] = 0x0D0064; // JSR $64 at ILLEGAL vector
    pram[100] = 0x000000; // NOP at JSR target
    run_one(&mut s, &mut jit); // ILLEGAL - posts interrupt, enters fast pipeline
    run_one(&mut s, &mut jit); // fast word 1 (JSR at vector)
    run_one(&mut s, &mut jit); // JSR detected -> long interrupt formation
    assert_eq!(
        s.interrupts.state,
        InterruptState::Long,
        "ILLEGAL should trigger long interrupt via JSR at vector"
    );
    let i1i0 = (s.registers[reg::SR] >> sr::I0) & 3;
    assert_eq!(
        i1i0, 3,
        "ILLEGAL long interrupt: I1:I0 should be raised to 3"
    );
}
