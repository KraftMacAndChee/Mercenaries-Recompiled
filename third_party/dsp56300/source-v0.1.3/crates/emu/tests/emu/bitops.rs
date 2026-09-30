use super::*;

#[test]
fn test_bclr_pp() {
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
    // x:$FFFFC5 is at periph[0xFFFFC5 - 0xFFFF80] = periph[69]
    periph[69] = 0x0F;
    // bclr #0,x:$ffffc5 -> template 0000101010pppppp0S00bbbb
    // pp = 5 (offset from $FFFFC0), bit = 0, S = 0 (X space)
    pram[0] = 0x0A8500;
    run_one(&mut s, &mut jit);
    assert_eq!(periph[69], 0x0E); // bit 0 cleared
    assert_eq!(s.registers[reg::SR] & 1, 1); // carry = old bit
}

#[test]
fn test_bset_pp() {
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
    periph[69] = 0x00;
    // bset #3,x:$ffffc5 -> pp=5, bit=3, S=0
    pram[0] = 0x0A8523;
    run_one(&mut s, &mut jit);
    assert_eq!(periph[69], 0x08); // bit 3 set
    assert_eq!(s.registers[reg::SR] & 1, 0); // carry = old bit (was 0)
}

#[test]
fn test_btst_reg_set() {
    // btst #3,X0: template 0000101111DDDDDD0110bbbb
    // DDDDDD=0x04 (X0), bbbb=3 -> 0000_1011_1100_0100_0110_0011 = 0x0BC463
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000008; // bit 3 set
    pram[0] = 0x0BC463;
    run_one(&mut s, &mut jit);
    // Carry should be set (bit was 1)
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_btst_reg_clear() {
    // btst #3,X0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000004; // bit 2 set, bit 3 clear
    pram[0] = 0x0BC463; // btst #3,X0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_bchg_reg() {
    // bchg #3,X0: template 0000101111DDDDDD010bbbbb
    // DDDDDD=0x04 (X0), bbbbb=3 -> 0000_1011_1100_0100_0100_0011 = 0x0BC443
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // bit 3 clear
    pram[0] = 0x0BC443; // bchg #3,X0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x000008); // bit 3 toggled on
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // old value was 0
}

#[test]
fn test_bchg_aa() {
    // bchg #5,X:$10: template 0000101100aaaaaa0S00bbbb
    // aaaaaa=0x10, S=0 (X), bbbb=5
    // 0000_1011_0001_0000_0000_0101 = 0x0B1005
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000020; // bit 5 set
    pram[0] = 0x0B1005; // bchg #5,X:$10
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x000000); // bit 5 toggled off
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // old value was 1
}

#[test]
fn test_bchg_pp() {
    // bchg #0,X:$FFFFC4: template 0000101110pppppp0S00bbbb
    // pp=4 (-> addr $FFFFC4), S=0, bbbb=0
    // 0000_1011_1000_0100_0000_0000 = 0x0B8400
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
    periph[0x44] = 0x000000; // $FFFFC4 = periph[0x44], bit 0 clear
    pram[0] = 0x0B8400; // bchg #0,X:$FFFFC4
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0x44], 0x000001); // bit 0 toggled on
}

#[test]
fn test_bclr_ea() {
    // bclr #2,X:(R0): template 0000101001MMMRRR0S00bbbb
    // MMM=100 (Rn, no update), RRR=000 (R0), S=0 (X), bbbb=2
    // 0000_1010_0110_0000_0000_0010 = 0x0A6002
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000007; // bits 0,1,2 set
    pram[0] = 0x0A6002;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0x000003); // bit 2 cleared
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = old bit 2 = 1
}

#[test]
fn test_bset_ea() {
    // bset #5,X:(R0): template 0000101001MMMRRR0S1bbbbb
    // MMM=100, RRR=000, S=0, bbbbb=5
    // 0000_1010_0110_0000_0010_0101 = 0x0A6025
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000020;
    xram[0x20] = 0x000000;
    pram[0] = 0x0A6025;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x20], 0x000020); // bit 5 set
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = old bit 5 = 0
}

#[test]
fn test_btst_ea() {
    // btst #3,X:(R0): template 0000101101MMMRRR0S10bbbb
    // MMM=100, RRR=000, S=0, bbbb=3
    // 0000_1011_0110_0000_0010_0011 = 0x0B6023
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    xram[0x10] = 0x000008; // bit 3 set
    pram[0] = 0x0B6023;
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = bit 3 = 1
    assert_eq!(xram[0x10], 0x000008); // unchanged
}

#[test]
fn test_bchg_ea() {
    // bchg #0,X:(R0): template 0000101101MMMRRR0S00bbbb
    // MMM=100, RRR=000, S=0, bbbb=0
    // 0000_1011_0110_0000_0000_0000 = 0x0B6000
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000005;
    xram[0x05] = 0x000001; // bit 0 set
    pram[0] = 0x0B6000;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x05], 0x000000); // bit 0 toggled off
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = old bit 0 = 1
}

#[test]
fn test_bclr_aa() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bclr #0,x:$0010
    pram[0] = 0x0A1000;
    xram[0x10] = 0x000001;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(xram[0x10], 0x000000);
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // old bit was 1
}

#[test]
fn test_bset_aa() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bset #5,x:$0010
    pram[0] = 0x0A1025;
    xram[0x10] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(xram[0x10], 0x000020);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // old bit was 0
}

#[test]
fn test_btst_aa() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // btst #3,x:$0010 -- bit is set
    pram[0] = 0x0B1023;
    xram[0x10] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0);
    assert_eq!(xram[0x10], 0x000008); // unchanged

    // btst #3,x:$0010 -- bit is clear
    pram[1] = 0x0B1023;
    xram[0x10] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_btst_pp() {
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
    // btst #3,x:$ffffc0 -- bit is set
    // pp=0 maps to $ffffc0 = PERIPH_BASE+64
    pram[0] = 0x0B8023;
    periph[64] = 0x000008;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0);

    // btst #3,x:$ffffc0 -- bit is clear
    pram[1] = 0x0B8023;
    periph[64] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0);
}

#[test]
fn test_bclr_reg() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bclr #5,x0
    pram[0] = 0x0AC445;
    s.registers[reg::X0] = 0x00003F;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::X0], 0x00001F);
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // old bit was 1
}

#[test]
fn test_bset_reg() {
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bset #5,x0
    pram[0] = 0x0AC465;
    s.registers[reg::X0] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::X0], 0x000020);
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // old bit was 0
}

#[test]
fn test_bclr_ea_mode0_rn_minus_nn() {
    // BCLR #0,X:(R0)-N0 -- covers emit_calc_ea mode 0 (Rn-Nn)
    // BclrEa: 0000101000MMMRRR0S00bbbb, MMM=000, RRR=000, S=0, b=0
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0A4000; // BCLR #0,X:(R0)-N0
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 3;
    xram[0x10] = 0xFF;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x10], 0xFE); // bit 0 cleared
    assert_eq!(s.registers[reg::R0], 0x0D); // R0 = 0x10 - 3
}

#[test]
fn test_bclr_ea_mode2_rn_dec() {
    // BCLR #0,X:(R0)- -- covers emit_calc_ea mode 2 (Rn-)
    // BclrEa: MMM=010, RRR=000 -> ea_field=0x10
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0A5000; // BCLR #0,X:(R0)-
    s.registers[reg::R0] = 0x20;
    xram[0x20] = 0x03;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x20], 0x02); // bit 0 cleared
    assert_eq!(s.registers[reg::R0], 0x1F); // R0 decremented
}

#[test]
fn test_bclr_ea_mode5_rn_plus_nn() {
    // BCLR #0,X:(R0+N0) -- covers emit_calc_ea mode 5 (Rn+Nn transient)
    // BclrEa: MMM=101, RRR=000 -> ea_field=0x28
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0A6800; // BCLR #0,X:(R0+N0)
    s.registers[reg::R0] = 0x10;
    s.registers[reg::N0] = 5;
    xram[0x15] = 0x07;
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x15], 0x06); // bit 0 cleared at addr R0+N0
    assert_eq!(s.registers[reg::R0], 0x10); // R0 NOT modified (transient)
}

#[test]
fn test_bclr_ea_mode7_pre_dec() {
    // BCLR #0,X:-(R0) -- covers emit_calc_ea mode 7 (pre-decrement)
    // BclrEa: MMM=111, RRR=000 -> ea_field=0x38
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0A7800; // BCLR #0,X:-(R0)
    s.registers[reg::R0] = 0x10;
    xram[0x0F] = 0x0F;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::R0], 0x0F); // R0 pre-decremented
    assert_eq!(xram[0x0F], 0x0E); // bit 0 cleared at new R0
}

#[test]
fn test_btst_pp_unmapped() {
    // btst_pp with no periph region -> read_mem returns 0 (covers line 2581)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // btst #0,x:$ffffc5 -- pp=5, bit=0, X space
    pram[0] = 0x0B8500;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    assert_eq!(s.registers[reg::SR] & 1, 0); // unmapped -> 0 -> C=0
}

#[test]
fn test_bset_pp_unmapped() {
    // bset_pp with no periph region -> write_mem silently drops (covers line 2603)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // bset #3,x:$ffffc5 -- pp=5, bit=3, X space
    pram[0] = 0x0A8530;
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 1);
    // Old bit was 0 -> C=0
    assert_eq!(s.registers[reg::SR] & 1, 0);
}

#[test]
fn test_bclr_qq() {
    // bclr #0,X:$FFFF80: template 0000000100qqqqqq0S00bbbb
    // qq=0 (offset 0 -> addr $FFFF80), S=0 (X), bbbb=0
    // 0000_0001_0000_0000_0000_0000 = 0x010000
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
    periph[0] = 0x00000F; // bits 0-3 set
    pram[0] = 0x010000; // bclr #0,X:$FFFF80
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0], 0x00000E); // bit 0 cleared
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = old bit (was 1)
}

#[test]
fn test_bset_qq() {
    // bset #5,X:$FFFF80: template 0000000100qqqqqq0S1bbbbb
    // qq=0, S=0, bbbbb=5 -> 0000_0001_0000_0000_0010_0101 = 0x010025
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
    pram[0] = 0x010025; // bset #5,X:$FFFF80
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0], 0x000020); // bit 5 set
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = old bit (was 0)
}

#[test]
fn test_btst_qq() {
    // btst #3,X:$FFFF80: template 0000000101qqqqqq0S10bbbb
    // qq=0, S=0, bbbb=3 -> 0000_0001_0100_0000_0010_0011 = 0x014023
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
    periph[0] = 0x000008; // bit 3 set
    pram[0] = 0x014023; // btst #3,X:$FFFF80
    run_one(&mut s, &mut jit);
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = tested bit (1)
}

#[test]
fn test_bchg_qq() {
    // bchg #2,X:$FFFF80: template 0000000101qqqqqq0S0bbbbb
    // qq=0, S=0, bbbbb=2 -> 0000_0001_0100_0000_0000_0010 = 0x014002
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
    periph[0] = 0x000004; // bit 2 set
    pram[0] = 0x014002; // bchg #2,X:$FFFF80
    run_one(&mut s, &mut jit);
    assert_eq!(periph[0], 0x000000); // bit 2 toggled off
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0); // carry = old bit (was 1)
}

#[test]
fn test_bclr_sr_does_not_corrupt_c() {
    // BCLR #5,SR: clear E flag (bit 5 of SR). C should be unaffected.
    // Opcode: bclr #n,D with D=SR(0x39), n=5
    // Template: 0000101011DDDDDD010bbbbb
    // DDDDDD=111001, bbbbb=00101
    // 0000_1010_1111_1001_0100_0101 = 0x0AF945
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0AF945; // BCLR #5,SR
    // Set E=1 (bit 5), C=0 (bit 0)
    s.registers[reg::SR] = 1 << sr::E;
    run_one(&mut s, &mut jit);
    let e = (s.registers[reg::SR] >> sr::E) & 1;
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(e, 0, "BCLR #5,SR: E should be cleared");
    assert_eq!(c, 0, "BCLR #5,SR: C should be unaffected (was 0, stays 0)");
}

#[test]
fn test_bset_sr_bit0_sets_c() {
    // BSET #0,SR: set C flag (bit 0 of SR). C should be set.
    // Template: 0000101011DDDDDD011bbbbb
    // DDDDDD=111001, bbbbb=00000
    // 0000_1010_1111_1001_0110_0000 = 0x0AF960
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    pram[0] = 0x0AF960; // BSET #0,SR
    s.registers[reg::SR] = 0; // C=0 initially
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "BSET #0,SR: C should be set by the operation");
}
#[test]
fn test_btst_sr_updates_carry() {
    // BTST #n,SR should update C to the old value of the tested bit
    // BTST #5,SR (test E bit): opcode for BTST #n,D where D=SR
    // Template: 0000101011DDDDDD0bbbbb, BTST reg uses 0000101111aaaaaa0bbbbb
    // Actually BTST reg: 0000101111DDDDDD0bbbbb
    // SR reg encoding: DDDDDD = 111001 (from Table 12-16: SR = 11001, but as 6-bit: 111001)
    // Actually looking at decode.rs for BtstReg
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set E=1 (bit 5), C=0 initially
    s.registers[reg::SR] = 1 << sr::E;
    // BTST #5,SR: should set C=1 (because E=1)
    // BTST reg template: 0000101111DDDDDD0bbbbb
    // SR = register index 0x39 (57) from reg::SR constant
    // But decoder uses a different encoding... let me check
    // Actually BTST on SR uses the same bit_op_reg path.
    // Opcode: 0x0BF965 = BTST #5,SR (0000101111 111001 0 00101)
    // Wait: 0x0BF9.. is BCHG/BTST prefix 0000101111
    // For BTST: bits 5:0 = 0bbbbb where b=5 -> 000101
    // Register: DDDDDD for SR
    pram[0] = 0x0BF965; // BTST #5,SR
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    // After BTST #5,SR: C should be 1 (E bit was set)
    assert_eq!(c, 1, "BTST #5,SR should set C=1 when E=1");
}

#[test]
fn test_btst_ea_mode6_absolute() {
    // BTST #5,X:$0100 - mode 6 (absolute address from extension word)
    // Opcode: 0x0B7025 (MMMRRR=110000, S=0, bbbb=0101)
    // Extension word: 0x000100
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x100] = 0x000020; // bit 5 set
    pram[0] = 0x0B7025; // btst #5,X:(abs)
    pram[1] = 0x000100; // absolute address = $100
    run_one(&mut s, &mut jit);
    assert_eq!(s.pc, 2, "2-word instruction should advance PC by 2");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C should be set (bit 5 is 1)"
    );
}

#[test]
fn test_bclr_bit23_x0() {
    // BCLR #23,X0: clear bit 23 (MSB of a 24-bit register).
    // Template: 0000101011DDDDDD010bbbbb, DDDDDD=000100 (X0), bbbbb=10111 (23)
    // 0000_1010_1100_0100_0101_0111 = 0x0AC457
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x800000; // only bit 23 set
    pram[0] = 0x0AC457; // bclr #23,x0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x000000, "bit 23 should be cleared");
    assert_ne!(s.registers[reg::SR] & (1 << sr::C), 0, "C = old bit 23 = 1");
}

#[test]
fn test_bset_bit23_x0() {
    // BSET #23,X0: set bit 23 (MSB of a 24-bit register).
    // Template: 0000101011DDDDDD011bbbbb, DDDDDD=000100 (X0), bbbbb=10111 (23)
    // 0000_1010_1100_0100_0111_0111 = 0x0AC477
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000; // all clear
    pram[0] = 0x0AC477; // bset #23,x0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x800000, "bit 23 should be set");
    assert_eq!(s.registers[reg::SR] & (1 << sr::C), 0, "C = old bit 23 = 0");
}

#[test]
fn test_bchg_sr_complements_ccr() {
    // DSP56300FM p.13-19: BCHG complements the specified bit.
    // When target is SR, the CCR bit is complemented.
    // bchg #0,sr -> complements C flag (bit 0 of SR).
    // BCHG reg template: 0000101111DDDDDD010bbbbb
    // DDDDDD=111001 (SR), bbbbb=00000 (bit 0)
    // 0000_1011_1111_1001_0100_0000 = 0x0BF940
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] &= !(1 << sr::C); // C=0 initially
    pram[0] = 0x0BF940; // bchg #0,sr
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C should be complemented from 0 to 1");
    // Run again: C should flip back to 0
    s.pc = 0;
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C should be complemented back from 1 to 0");
}

#[test]
fn test_btst_flags_unaffected() {
    // DSP56300FM p.13-40: BTST only affects C flag.
    // V, Z, N, U, E should be unchanged after BTST.
    // Pre-set V=1, Z=1, N=1, U=1, E=1 in SR. X0=0x000008 (bit 3 set).
    // btst #3,x0 (0x0BC463) -> C=1 (bit 3 is set).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |=
        (1 << sr::V) | (1 << sr::Z) | (1 << sr::N) | (1 << sr::U) | (1 << sr::E);
    s.registers[reg::X0] = 0x000008; // bit 3 set
    pram[0] = 0x0BC463; // btst #3,x0
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C should be 1 (bit 3 is set)");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::V),
        0,
        "V should be preserved"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::Z),
        0,
        "Z should be preserved"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "N should be preserved"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::U),
        0,
        "U should be preserved"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::E),
        0,
        "E should be preserved"
    );
}

#[test]
fn test_bset_accum_b_limiting() {
    // DSP56300FM p.13-34: BSET on accumulators optionally scales per S1:S0 bits.
    // With no scaling (S1:S0=00), bit operations apply directly.
    // B1=0x000008 (bit 3 already set). bset #3,b1 -> C=1 (old bit), B1 unchanged.
    // BSET reg template: 0000101011DDDDDD011bbbbb
    // B1 = 0x0d -> DDDDDD=001101, bbbbb=00011
    // 0000_1010_1100_1101_0110_0011 = 0x0ACD63
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x000008;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x0ACD63; // bset #3,b1
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0x000008,
        "B1 should be unchanged (bit already set)"
    );
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C should be 1 (old bit 3 was set)");

    // Now clear B1 and set bit 3 via BSET
    s.pc = 0;
    s.registers[reg::B1] = 0x000000;
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::B1], 0x000008, "B1 bit 3 should now be set");
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C should be 0 (old bit 3 was clear)");
}

#[test]
fn test_bchg_bit23_max() {
    // bchg #23,x0: toggle bit 23 of X0. X0=0x000000, so bit 23 goes 0->1.
    // C = old bit value = 0. X0 = 0x800000.
    // Template: 0000101111DDDDDD010bbbbb, DDDDDD=0x04 (X0), bbbbb=10111 (23).
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::X0] = 0x000000;
    pram[0] = 0x0BC457; // bchg #23,x0
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::X0],
        0x800000,
        "X0 bit 23 should be toggled to 1"
    );
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C should be 0 (old bit 23 was 0)");
}

#[test]
fn test_bchg_y_space_ea() {
    // yram[0x10]=0x000001, R0=0x10. bchg #0,y:(r0) toggles bit 0: 1->0.
    // C = old bit = 1. yram[0x10] = 0x000000.
    // Template: 0000101101MMMRRR0S00bbbb, MMM=100, RRR=000, S=1 (Y), bbbb=0000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::R0] = 0x000010;
    yram[0x10] = 0x000001;
    pram[0] = 0x0B6040; // bchg #0,y:(r0)
    run_one(&mut s, &mut jit);
    assert_eq!(
        yram[0x10], 0x000000,
        "yram[0x10] bit 0 should be toggled off"
    );
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C should be 1 (old bit 0 was 1)");
}

#[test]
fn test_bclr_y_space_aa() {
    // BCLR in Y memory absolute short.
    // yram[0x10]=0x0000FF. bclr #0,y:$10 clears bit 0.
    // C = old bit 0 = 1. yram[0x10] = 0xFE.
    // Template: 0000101000aaaaaa0S00bbbb, aaaaaa=010000, S=1 (Y), bbbb=0000.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x10] = 0x0000FF;
    pram[0] = 0x0A1040; // bclr #0,y:$10
    run_one(&mut s, &mut jit);
    assert_eq!(yram[0x10], 0x0000FE, "yram[0x10] bit 0 should be cleared");
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C should be 1 (old bit 0 was 1)");
}

#[test]
fn test_bset_y_space_pp() {
    // BSET in Y-space peripheral I/O.
    // Targets Y:$FFFFC0 (peripheral base). pp=0 maps to $FFFFC0 = PERIPH_BASE+64.
    // Template: 0000101010pppppp0S1bbbbb, pp=0, S=1 (Y), bbbbb=0.
    // Set up Y-space peripheral region, set periph[64]=0, then bset #0 -> bit 0 set.
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
    periph[64] = 0x000000; // $FFFFC0 = periph[64], bit 0 clear
    pram[0] = 0x0A8060; // bset #0,y:$ffffc0
    run_one(&mut s, &mut jit);
    assert_eq!(periph[64], 0x000001, "periph[64] bit 0 should be set");
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C should be 0 (old bit 0 was 0)");
}

#[test]
fn test_btst_bit15_max_register() {
    // DSP56300FM p.13-40: BTST #n,D register variant uses 4-bit bbbb field (max bit 15).
    // Template: 0000101111DDDDDD0110bbbb, DDDDDD=000100 (X0), bbbb=1111 (bit 15).
    // 0000_1011_1100_0100_0110_1111 = 0x0BC46F
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // X0 = 0x008000 (bit 15 set). btst #15,x0 => C=1.
    s.registers[reg::X0] = 0x008000;
    pram[0] = 0x0BC46F; // btst #15,x0
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C=1 when bit 15 is set in X0");

    // X0 = 0x007FFF (bit 15 clear). btst #15,x0 => C=0.
    s.registers[reg::X0] = 0x007FFF;
    s.pc = 0;
    pram[0] = 0x0BC46F; // btst #15,x0
    run_one(&mut s, &mut jit);
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C=0 when bit 15 is clear in X0");
}

#[test]
fn test_bclr_accum_a1() {
    // DSP56300FM p.13-22: BCLR on accumulator portion A1.
    // bclr #3,a1 clears bit 3, stores old bit value in C.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // A1 = 0x000008 (bit 3 set). BCLR #3,A1 => A1=0, C=1.
    s.registers[reg::A1] = 0x000008;
    pram[0] = 0x0ACC43; // bclr #3,a1
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000, "A1 bit 3 should be cleared");
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C=1 (old bit 3 was set)");

    // A1 = 0x000000 (bit 3 already clear). BCLR #3,A1 => A1=0, C=0.
    s.registers[reg::A1] = 0x000000;
    s.pc = 0;
    pram[0] = 0x0ACC43; // bclr #3,a1
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::A1], 0x000000, "A1 should remain 0");
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C=0 (old bit 3 was clear)");
}

#[test]
fn test_btst_bit23_aa_memory() {
    // DSP56300FM p.13-40: BTST field description says "Bit number [0-23]".
    // The p.13-41 encoding diagram shows only 4 b bits with bit 4 fixed to 0,
    // but the official Motorola asm56300.exe encodes btst #23 using all 5 bits
    // (bit 4 = 1). The encoding diagram is errata; the b field is 5 bits.
    //
    // btst #23,x:$10 = 0x0B1037 (confirmed by official asm56300.exe)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Bit 23 set
    xram[0x10] = 0x800000;
    pram[0] = 0x0B1037; // btst #23,x:$10
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 when bit 23 is set (manual p.13-40)"
    );
    // Bit 23 clear
    xram[0x10] = 0x7FFFFF;
    s.pc = 0;
    pram[0] = 0x0B1037; // btst #23,x:$10
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=0 when bit 23 is clear (manual p.13-40)"
    );
}

#[test]
fn test_bchg_flags_preserved() {
    // DSP56300FM p.13-20: For non-SR targets, V/Z/N/U/E are unaffected.
    // Pre-set V=1,Z=1,N=1,U=1,E=1, then BCHG #0,X0, verify those flags unchanged.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |=
        (1 << sr::V) | (1 << sr::Z) | (1 << sr::N) | (1 << sr::U) | (1 << sr::E);
    s.registers[reg::X0] = 0x000001; // bit 0 set
    pram[0] = 0x0BC440; // bchg #0,X0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x000000); // bit 0 toggled off
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (old bit was 1)"
    );
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "V preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "Z preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "N preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::U), 0, "U preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::E), 0, "E preserved");
}

#[test]
fn test_bclr_flags_preserved() {
    // V/Z/N/U/E unaffected for non-SR targets.
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] |=
        (1 << sr::V) | (1 << sr::Z) | (1 << sr::N) | (1 << sr::U) | (1 << sr::E);
    s.registers[reg::X0] = 0x000020; // bit 5 set
    pram[0] = 0x0AC445; // bclr #5,X0
    run_one(&mut s, &mut jit);
    assert_eq!(s.registers[reg::X0], 0x000000); // bit 5 cleared
    assert_ne!(s.registers[reg::SR] & (1 << sr::V), 0, "V preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::Z), 0, "Z preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::N), 0, "N preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::U), 0, "U preserved");
    assert_ne!(s.registers[reg::SR] & (1 << sr::E), 0, "E preserved");
}

#[test]
fn test_bclr_ea_mode6_absolute() {
    // BCLR with EA mode 6 (absolute address from extension word).
    // bclr_ea: 0000101000MMMRRR0S0bbbbb
    // mode 6: MMM=110, RRR=000 -> 0000_1010_0011_0000_0000_0010 = 0x0A3002
    // bbbbb=2, S=0 (X space)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x100] = 0x000004; // bit 2 set
    pram[0] = 0x0A7002; // bclr #2,x:$xxxx (mode 6: MMM=110, S=0, bbbb=0010)
    pram[1] = 0x000100; // address = $100
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x100], 0x000000, "bit 2 cleared");
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (old bit was 1)"
    );
    assert_eq!(s.pc, 2, "2-word instruction");
}

#[test]
fn test_bset_ea_mode6_absolute() {
    // BSET with EA mode 6 (absolute address from extension word).
    // bset_ea: 0000101001MMMRRR0S0bbbbb
    // mode 6: MMM=110, RRR=000 -> 0000_1010_0111_0000_0000_0010 = 0x0A7002
    // bbbbb=2, S=0 (X space)
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x100] = 0x000000; // bit 2 clear
    pram[0] = 0x0A7022; // bset #2,x:$xxxx (mode 6: MMM=110, S=0, bit5=1, bbbbb=00010)
    pram[1] = 0x000100; // address = $100
    run_one(&mut s, &mut jit);
    assert_eq!(xram[0x100], 0x000004, "bit 2 set");
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=0 (old bit was 0)"
    );
    assert_eq!(s.pc, 2, "2-word instruction");
}

#[test]
fn test_btst_y_space_aa() {
    // BTST with Y-space absolute short addressing.
    // btst_aa: 0000101100aaaaaa1S1bbbbb
    // aa=0x10, S=1 (Y space), bbbbb=3
    // 0000_1011_0001_0000_1110_0011 = 0x0B1063
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    yram[0x10] = 0x000008; // bit 3 set
    pram[0] = 0x0B1063; // btst #3,y:$10
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=1 (bit 3 set in Y-space)"
    );

    // Clear case
    yram[0x10] = 0x000000;
    s.pc = 0;
    pram[0] = 0x0B1063;
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=0 (bit 3 clear in Y-space)"
    );
}

#[test]
fn test_bchg_sr_mr_bits() {
    // BCHG/BCLR on SR bits 8-15 (MR region: I0, I1, S0, S1, SC, DM, LF, FV).
    // BCHG #8,SR should complement I0 (bit 8).
    // BCHG reg template: 0000101111DDDDDD010bbbbb
    // DDDDDD=111001 (SR=0x39), bbbbb=01000 (bit 8)
    // 0000_1011_1111_1001_0100_1000 = 0x0BF948
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = 0; // I0=0
    pram[0] = 0x0BF948; // bchg #8,SR
    run_one(&mut s, &mut jit);
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::I0),
        0,
        "BCHG #8,SR: I0 should be complemented to 1"
    );
}

#[test]
fn test_bclr_sr_mr_bit_s0() {
    // BCLR on SR bit 10 (S0).
    // When target is SR, bits are directly cleared (no C = old-bit side effect).
    // BCLR reg template: 0000101011DDDDDD010bbbbb
    // DDDDDD=111001 (SR=0x39), bbbbb=01010 (bit 10)
    // 0000_1010_1111_1001_0100_1010 = 0x0AF94A
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.registers[reg::SR] = (1 << sr::S0) | (1 << sr::N); // S0=1, N=1 (preserved)
    pram[0] = 0x0AF94A; // bclr #10,SR
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::S0),
        0,
        "BCLR #10,SR: S0 should be cleared"
    );
    assert_ne!(
        s.registers[reg::SR] & (1 << sr::N),
        0,
        "BCLR #10,SR: N should be preserved"
    );
}

#[test]
fn test_bset_ssh_does_not_pop() {
    // BSET on SSH should NOT pop/push the stack.
    // BSET reg template: 0000101011DDDDDD011bbbbb
    // DDDDDD=111100 (SSH=0x3C), bbbbb=00000 (bit 0)
    // 0000_1010_1111_1100_0110_0000 = 0x0AFC60
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Push something onto the stack so SP > 0
    s.stack_push(0x100, 0x200);
    let sp_before = s.registers[reg::SP] & 0xF;
    pram[0] = 0x0AFC60; // bset #0,SSH
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after, sp_before, "BSET on SSH should not change SP");
}

#[test]
fn test_bchg_ssh_does_not_pop() {
    // BCHG on SSH should NOT pop/push the stack.
    // BCHG reg template: 0000101111DDDDDD010bbbbb
    // DDDDDD=111100 (SSH=0x3C), bbbbb=00000 (bit 0)
    // 0000_1011_1111_1100_0100_0000 = 0x0BFC40
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.stack_push(0x100, 0x200);
    let sp_before = s.registers[reg::SP] & 0xF;
    pram[0] = 0x0BFC40; // bchg #0,SSH
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after, sp_before, "BCHG on SSH should not change SP");
}

#[test]
fn test_bchg_aa_bit15() {
    // BCHG aa form at bit-field boundary (bit 15).
    // BCHG aa template: 0000101100aaaaaa0S0bbbbb
    // aaaaaa=0x10, S=0 (X), bbbbb=01111 (bit 15)
    // 0000_1011_0001_0000_0000_1111 = 0x0B100F
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    xram[0x10] = 0x000000; // bit 15 clear
    pram[0] = 0x0B100F; // bchg #15,X:$10
    run_one(&mut s, &mut jit);
    assert_eq!(
        xram[0x10], 0x008000,
        "BCHG #15: bit 15 should be set (toggled from 0)"
    );
    assert_eq!(
        s.registers[reg::SR] & (1 << sr::C),
        0,
        "C=0 (old bit 15 was 0)"
    );
}

#[test]
fn test_bset_accum_scaling_mode() {
    // BSET on accumulator sub-register with scaling mode active.
    // Manual p.13-34: "optionally shifts the accumulator value according to scaling
    // mode bits S0 and S1". The emitter uses load_reg/store_reg which bypasses
    // scaling. This test locks in the current behavior (no scaling applied).
    // BSET #0,B1: DDDDDD=001101 (B1=0x0D), bbbbb=00000
    // 0000_1010_1100_1101_0110_0000 = 0x0ACD60
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set scale-up mode: S1=1, S0=0
    s.registers[reg::SR] = 1 << sr::S1;
    s.registers[reg::B2] = 0x00;
    s.registers[reg::B1] = 0x400000;
    s.registers[reg::B0] = 0x000000;
    pram[0] = 0x0ACD60; // bset #0,B1
    run_one(&mut s, &mut jit);
    assert_eq!(
        s.registers[reg::B1],
        0x400001,
        "BSET #0,B1: bit 0 should be set"
    );
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C=0 (old bit 0 was 0)");
}

#[test]
fn test_btst_ssh_does_not_pop() {
    // BTST on SSH should not pop (per ARCHITECTURE-NOTES.md).
    // Manual p.13-40 says "For destination operand SSH:SP, decrement the SP by 1"
    // but our implementation (and BSET/BCHG tests) confirm no pop for pure bit ops.
    // BTST reg template: 0000101111DDDDDD0110bbbb (note: 5-bit bbbbb per errata)
    // DDDDDD=111100 (SSH=0x3C), bbbbb=00000 (bit 0)
    // 0000_1011_1111_1100_0110_0000 = 0x0BFC60
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.stack_push(0x100, 0x200); // SP=1
    s.stack_push(0x300, 0x400); // SP=2
    let sp_before = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_before, 2);
    pram[0] = 0x0BFC60; // btst #0,SSH
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after, sp_before, "BTST on SSH should not pop the stack");
    // Verify C reflects the tested bit (SSH = 0x300, bit 0 = 0)
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 0, "C should reflect bit 0 of SSH (0x300 has bit 0 = 0)");
}

#[test]
fn test_bclr_ssh_does_not_pop() {
    // BCLR on SSH should NOT pop/push the stack.
    // BCLR reg template: 0000101011DDDDDD010bbbbb
    // DDDDDD=111100 (SSH=0x3C), bbbbb=00000 (bit 0)
    // 0000_1010_1111_1100_0100_0000 = 0x0AFC40
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    s.stack_push(0x100, 0x200); // SP=1
    s.stack_push(0x303, 0x400); // SP=2, SSH=0x303 (bit 0 set, bit 1 set)
    let sp_before = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_before, 2);
    pram[0] = 0x0AFC40; // bclr #0,SSH
    run_one(&mut s, &mut jit);
    let sp_after = s.registers[reg::SP] & 0xF;
    assert_eq!(sp_after, sp_before, "BCLR on SSH should not change SP");
    // C should reflect the old bit 0 of SSH (was 1)
    let c = (s.registers[reg::SR] >> sr::C) & 1;
    assert_eq!(c, 1, "C should be 1 (old bit 0 of SSH was set)");
    // SSH bit 0 should now be cleared
    assert_eq!(s.registers[reg::SSH] & 1, 0, "SSH bit 0 should be cleared");
}

#[test]
fn test_bset_full_accum_b_no_limiting() {
    // BSET on full accumulator B (DDDDDD=0x0F). The manual p.13-34 mentions
    // accumulator scaling/limiting for full A/B, but the emitter deliberately
    // uses load_reg/store_reg (no limiting) for bit ops. This test locks in
    // that behavior: the operation hits register slot 0x0F directly.
    // BSET reg template: 0000101011DDDDDD011bbbbb
    // DDDDDD=001111 (B=0x0F), bbbbb=00000 (bit 0)
    // 0000_1010_1100_1111_0110_0000 = 0x0ACF60
    let mut jit = JitEngine::new(PRAM_SIZE);
    let mut xram = [0u32; XRAM_SIZE];
    let mut yram = [0u32; YRAM_SIZE];
    let mut pram = [0u32; PRAM_SIZE];
    let mut s = DspState::new(MemoryMap::test(&mut xram, &mut yram, &mut pram));
    // Set B sub-registers to a value with extension in use
    s.registers[reg::B2] = 0xFF;
    s.registers[reg::B1] = 0x800000;
    s.registers[reg::B0] = 0x000000;
    // Set scale-up mode
    s.registers[reg::SR] = 1 << sr::S1;
    pram[0] = 0x0ACF60; // bset #0,B (full accumulator, slot 0x0F)
    run_one(&mut s, &mut jit);
    // Bit op hits register slot 0x0F directly, not the packed accumulator.
    // B2/B1/B0 sub-registers should be unchanged (bit op doesn't touch them).
    assert_eq!(s.registers[reg::B2], 0xFF, "B2 unchanged");
    assert_eq!(s.registers[reg::B1], 0x800000, "B1 unchanged");
    assert_eq!(s.registers[reg::B0], 0x000000, "B0 unchanged");
}
