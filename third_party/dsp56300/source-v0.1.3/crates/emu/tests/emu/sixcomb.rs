use super::*;

const SIXCOMB_LOD: &str = include_str!("../data/sixcomb.lod");

/// Load the full sixcomb program from the LOD file.
fn load_sixcomb(pram: &mut [u32], xram: &mut [u32], yram: &mut [u32]) {
    load_a56_program(pram, xram, yram, SIXCOMB_LOD);
}

#[test]
fn test_jit_block_mac_xy_moves() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    // Signal vector
    xram[0x28] = 0x400000; // Lin = 0.5
    xram[0x29] = 0x400000; // Rin = 0.5
    xram[0x2A] = 0x000000;
    xram[0x2B] = 0x000000;

    // Gain vectors
    yram[0x52] = 0x400000; // Reverb_gain: [0.5, 0.5, 0, 0]
    yram[0x53] = 0x400000;
    yram[0x54] = 0x000000;
    yram[0x55] = 0x000000;
    yram[0x56] = 0x000000; // Lout_gain: [0, 0, 1.0, 0]
    yram[0x57] = 0x000000;
    yram[0x58] = 0x7FFFFF;
    yram[0x59] = 0x000000;
    xram[0x21] = 0x7FFFFF; // L_overall

    // Mini matrix-multiply program (extracted from sixcomb hf_comp)
    pram[0] = 0x310800; // move #$28,r0
    pram[1] = 0x05F420; // movec #imm,m0
    pram[2] = 0x000003; // immediate = 3 (modulo 4)
    pram[3] = 0x34D200; // move #$52,r4
    pram[4] = 0x05F424; // movec #imm,m4
    pram[5] = 0xFFFFFF; // M4 = linear addressing
    pram[6] = 0x47A100; // move x:<$21,y1
    pram[7] = 0xF09813; // clr a x:(r0)+,x0 y:(r4)+,y0
    pram[8] = 0xF098D2; // mac y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[9] = 0xF098D2; // mac y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[10] = 0xF098D2; // mac y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[11] = 0xF098D3; // macr y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[12] = 0x21CF13; // clr a a,b
    pram[13] = 0xF098D2; // mac y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[14] = 0xF098D2; // mac y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[15] = 0xF098D2; // mac y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[16] = 0xF098D3; // macr y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[17] = 0x21C513; // clr a a,x1
    pram[18] = 0x2000F0; // mpy y1,x1,a
    pram[19] = 0x560413; // clr a a,x:$0004
    pram[20] = 0x000086; // wait

    // Run interpreter
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    for _ in 0..21 {
        if s.power_state != PowerState::Normal {
            break;
        }
        s.execute_one(&mut jit);
    }
    let interpret_out_l = xram[0x04];

    // Reset and run block JIT
    xram[0x04] = 0xDEAD;
    xram[0x28] = 0x400000;
    xram[0x29] = 0x400000;
    xram[0x2A] = 0x000000;
    xram[0x2B] = 0x000000;
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.run(&mut jit, 100);
    let block_out_l = xram[0x04];

    assert_eq!(interpret_out_l, 0x000000);
    assert_eq!(block_out_l, 0x000000);
    assert_eq!(interpret_out_l, block_out_l);
}

#[test]
fn test_jit_block_sixcomb_hf_comp() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    load_sixcomb(&mut pram, &mut xram, &mut yram);
    pram[0x0FFF] = 0x000086; // WAIT sentinel for RTS target

    xram[0x02] = 0x400000; // in_l = 0.5
    xram[0x03] = 0x400000; // in_r = 0.5

    // Run interpreter
    let mut s1 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s1.pc = 0x0040;
    s1.stack_push(0x0FFF, s1.registers[reg::SR]);
    for _ in 0..2000 {
        if s1.power_state != PowerState::Normal {
            break;
        }
        s1.execute_one(&mut jit);
    }
    let interp_out_l = xram[0x04];
    let interp_out_r = xram[0x05];

    // Reset and run block JIT
    load_sixcomb(&mut pram, &mut xram, &mut yram);
    pram[0x0FFF] = 0x000086;
    xram[0x02] = 0x400000;
    xram[0x03] = 0x400000;

    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.pc = 0x0040;
    s2.stack_push(0x0FFF, s2.registers[reg::SR]);
    s2.run(&mut jit2, 2000);
    let block_out_l = xram[0x04];
    let block_out_r = xram[0x05];

    assert_eq!(block_out_l, interp_out_l, "out_l mismatch");
    assert_eq!(block_out_r, interp_out_r, "out_r mismatch");
}

#[test]
fn test_jit_block_sixcomb_no_do() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    load_sixcomb(&mut pram, &mut xram, &mut yram);
    pram[0x0067] = 0x000086; // replace DO with WAIT
    pram[0x0068] = 0x000000;
    xram[0x02] = 0x400000;
    xram[0x03] = 0x400000;

    // Run interpreter
    let mut s1 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s1.pc = 0x0040;
    for _ in 0..100 {
        if s1.power_state != PowerState::Normal {
            break;
        }
        s1.execute_one(&mut jit);
    }
    let interp_out_l = xram[0x04];

    // Reset and run block JIT
    load_sixcomb(&mut pram, &mut xram, &mut yram);
    pram[0x0067] = 0x000086;
    pram[0x0068] = 0x000000;
    xram[0x02] = 0x400000;
    xram[0x03] = 0x400000;
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.pc = 0x0040;
    s2.run(&mut jit2, 200);
    let block_out_l = xram[0x04];

    assert_eq!(block_out_l, interp_out_l);
}

#[test]
fn test_jit_block_sixcomb_matmul_only() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    load_sixcomb(&mut pram, &mut xram, &mut yram);
    pram[0x0056] = 0x000086; // WAIT right after out_l store
    xram[0x02] = 0x400000;
    xram[0x03] = 0x400000;

    // Run interpreter
    let mut s1 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s1.pc = 0x0040;
    for _ in 0..100 {
        if s1.power_state != PowerState::Normal {
            break;
        }
        s1.execute_one(&mut jit);
    }
    let interp_out_l = xram[0x04];

    // Reset and run block JIT
    load_sixcomb(&mut pram, &mut xram, &mut yram);
    pram[0x0056] = 0x000086;
    xram[0x02] = 0x400000;
    xram[0x03] = 0x400000;
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.pc = 0x0040;
    s2.run(&mut jit2, 200);
    let block_out_l = xram[0x04];

    assert_eq!(block_out_l, interp_out_l);
}

#[test]
fn test_jit_block_clr_a_after_mac() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];

    // Mini program: first matmul phase + clr + store (from sixcomb hf_comp)
    pram[0x0040] = 0x568200; // move x:$0002,a
    pram[0x0041] = 0x562800; // move a,x:$0028
    pram[0x0042] = 0x568300; // move x:$0003,a
    pram[0x0043] = 0x562900; // move a,x:$0029
    pram[0x0044] = 0x688100; // move y:$0001,r0
    pram[0x0045] = 0x058260; // movec y:$0002,m0
    pram[0x0046] = 0x6C8300; // move y:$0003,r4
    pram[0x0047] = 0x058024; // movec x:$0000,m4
    pram[0x0048] = 0x47A100; // move x:$0021,y1
    pram[0x0049] = 0xF09813; // clr a x:(r0)+,x0 y:(r4)+,y0
    pram[0x004A] = 0xF098D2; // mac +y0,x0,a x:(r0)+,x0 y:(r4)+,y0
    pram[0x004B] = 0xF098D2;
    pram[0x004C] = 0xF098D2;
    pram[0x004D] = 0xF098D3; // macr
    pram[0x004E] = 0x21CF13; // clr a a,b
    pram[0x004F] = 0xF098D2; // mac (second phase)
    pram[0x0050] = 0xF098D2;
    pram[0x0051] = 0xF098D2;
    pram[0x0052] = 0x560400; // move a,x:$0004
    pram[0x0053] = 0x000086; // WAIT

    xram[0x0000] = 0xFFFFFF; // m4 = linear addressing
    xram[0x0002] = 0x400000; // in_l
    xram[0x0003] = 0x400000; // in_r
    xram[0x0021] = 0x7FFFFF; // L_overall
    xram[0x0022] = 0x7FFFFF; // R_overall
    yram[0x0001] = 0x000028; // r0 init
    yram[0x0002] = 0x000003; // m0 init (modulo 4)
    yram[0x0003] = 0x000052; // r4 init
    yram[0x0052] = 0x400000; // gains: reverb_in row
    yram[0x0053] = 0x400000;
    yram[0x0058] = 0x7FFFFF; // gains: out_l row
    yram[0x005D] = 0x800000;

    // Run interpreter
    let mut s1 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s1.pc = 0x0040;
    for _ in 0..100 {
        if s1.power_state != PowerState::Normal {
            break;
        }
        s1.execute_one(&mut jit);
    }
    let interp_out = xram[0x04];

    // Reset and run block JIT
    xram[0x04] = 0;
    xram[0x0002] = 0x400000;
    xram[0x0003] = 0x400000;
    let mut jit2 = JitEngine::new(PRAM_SIZE);
    let mut s2 = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s2.pc = 0x0040;
    s2.run(&mut jit2, 200);
    let block_out = xram[0x04];

    assert_eq!(block_out, interp_out, "clr-after-mac mismatch");
}
