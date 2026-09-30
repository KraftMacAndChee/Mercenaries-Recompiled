# DSP56300 Architecture Notes

Reference knowledge derived from the DSP56300 Family Manual (Rev. 5) that has been
verified during code review rounds. This file exists to avoid re-investigating
settled questions in future reviews.

## Addressing Mode Categories (Manual Section 4.4, Table 4-1)

The DSP56300 has four addressing mode categories:

1. **Register Direct** (4.4.1): Data/control register, Rn, Mn, Nn
2. **Address Register Indirect** (4.4.2): (Rn), (Rn)+, (Rn)-, (Rn)+Nn, (Rn)-Nn, (Rn+Nn), -(Rn), (Rn+displ) - MMM 0-5, 7, and displacement sub-mode of 6
3. **PC-relative** (4.4.3): Short/long displacement, address register
4. **Special** (4.4.4): Immediate data, **absolute address** (mode 6 RRR=0), absolute short, short jump, I/O short, implicit

### Mode 6 in MMMRRR encoding

Mode 6 (MMM=110) is overloaded:
- RRR=000: **Absolute address** (Special mode) - address from extension word
- RRR=100: **Immediate data** (Special mode) - value from extension word
- Other RRR: reserved/don't-care (normalized by decoder's `norm_all`/`norm_lo`)

### Which instructions support mode 6 (absolute address)?

**"All addressing modes" / "all memory alterable"** - YES, supports mode 6. Use `emit_calc_ea_ext`:
- JMP/JSR/Jcc/JScc ea, MOVEC/MOVEM/MOVEP ea, BCLR/BSET/BTST/BCHG ea, PLOCK/PUNLOCK ea, VSL

**"Address register indirect addressing modes"** - NO, mode 6 is a Special mode, excluded. Use `emit_calc_ea`:
- DO/DOR/REP ea, JCLR/JSET/JSCLR/JSSET ea, BRCLR/BRSET/BSCLR/BSSET ea, LUA ea

The decoder's `norm_all` normalizes RRR for mode 6 but doesn't reject it. Reaching mode 6
in the second group is undefined hardware behavior - not a code bug.

## Manual Errata / Inconsistencies

- **ROR CCR table** (p.13-166): Says "C: Set if bit 47" - copy-paste from ROL. Description correctly says C = old bit 24. Code follows description.
- **MERGE operation** (p.13-108): Says `S[7-0]` but description/example use bits 11-0 (12 bits). Code matches description.
- **ENDDO** (p.13-67): Operation says `SSL(LF) -> SR` (only LF), but BRKcc says `SSL(LF,FV) -> SR`. All code paths restore both - necessary for DO FOREVER.
- **DIV operation header** (p.13-52): Says `D[39] XOR S[15]` - these are 16-bit compatibility positions. Description says bit 55/bit 23. Code uses 55/23.
- **NORMF V flag** (p.13-147): Says "Set if bit 39 is changed" - same DSP56000 40-bit holdover as DIV. The equivalent NORM instruction (p.13-146) says bit 55. Code uses 55.
- **BTST bit field width** (p.13-41): Encoding diagrams show 4-bit `bbbb` with bit 4 fixed to 0. But the instruction fields table (p.13-40) says "Bit number [0-23]" (which requires 5 bits), and the official Motorola asm56300.exe encodes `btst #23` using all 5 bits (bit 4 = 1). The diagram is wrong; the field is 5 bits (`bbbbb`) like BCHG/BCLR/BSET. Confirmed by cross-referencing with the DSP56001 manual which also uses 5 bits.

## SSH Register Access Semantics

SSH has **pop-on-read / push-on-write** side effects, but only for **move instructions** (MOVEC, MOVEM, MOVEP). The manual describes SSH pop semantics on page 13-130 (MOVEC): "If the System Stack register SSH is specified as a source operand, the Stack Pointer (SP) is post-decremented by 1 after SSH has been read."

Non-move instructions that access SSH as a register operand (BCLR/BSET/BCHG/BTST #n,SSH; JCLR/JSET/JSCLR/JSSET #n,SSH,addr; etc.) must use plain register access (`load_reg`/`store_reg`) **without** pop/push side effects. Using `read_reg_for_move`/`write_reg_for_move` for these instructions corrupts SP.

## QQQQ Register Encoding (Table 12-16, Encoding 4)

All 16 values (0x0-0xF) are valid. Used by DMAC, MPY(su,uu), MAC(su,uu). The full mapping:

| QQQQ | S1,S2 | QQQQ | S1,S2 |
|------|-------|------|-------|
| 0000 | X0,X0 | 1000 | X1,X1 |
| 0001 | Y0,Y0 | 1001 | Y1,Y1 |
| 0010 | X1,X0 | 1010 | X0,X1 |
| 0011 | Y1,Y0 | 1011 | Y0,Y1 |
| 0100 | X0,Y1 | 1100 | Y1,X0 |
| 0101 | Y0,X0 | 1101 | X0,Y0 |
| 0110 | X1,Y0 | 1110 | Y0,X1 |
| 0111 | Y1,X1 | 1111 | X1,Y1 |

## NORMF V Flag: ASL vs ASR Paths

NORMF shifts the accumulator left (ASL) or right (ASR) based on the sign of the source operand. The V flag ("Set if bit 55 is changed during the shift") only applies to the **ASL (left shift) path**. During ASR, bit 55 is the sign bit and is always replicated - it can never change. V must be hardcoded to 0 for the ASR path.

The same uniformity check used for multi-bit ASL (checking whether bits [55:(55-n)] are all-same) is correct for the left-shift path, but must NOT be applied to the right-shift path.

## Parallel Move Duplicate Destinations

The manual (p.13-120) explicitly states: "duplicate destinations are not allowed within the same instruction." This means the ALU-wins-on-conflict question (when both the ALU and parallel move write to the same register) is **undefined behavior** for illegal instruction encodings. Our pm_0 has a defensive check; other move types do not, since the input is architecturally invalid.

## Long Interrupt SR Clearing (Section 2.3.2.5)

When a long interrupt is formed, the following SR bits are cleared: LF, S1, S0, I1, I0, **and SA** (Section 2.3.2.5). The SA bit (bit 17) is easy to miss since SA mode is otherwise unimplemented, but the clear must still happen to maintain correct SR state across interrupt boundaries. **FV is NOT cleared** - it is not listed in the manual's enumeration of cleared bits.

## Hardware Stack Overflow Behavior

When SP is at 15 and a push occurs, SP wraps to 0 (P[3:0] = 0000) with SE bit set, and a STACK_ERROR exception is posted. **The manual does not explicitly state whether the push data is written to stack slot 0 or silently dropped** (Section 5.4.3.1, Table 5-2).

The implementation treats the hardware stack as a circular buffer that overwrites slot 0 on overflow. This is unverified against real hardware but is a reasonable interpretation: since P[3:0] wraps to 0 and SP "always points to the top of stack" (manual p.5-19), writing to the pointed-to location is consistent.

Programs that overflow the hardware stack without stack extension (SEN=0) are already in an error state - the STACK_ERROR interrupt fires and the data at slot 0 is stale regardless.

## RND V Flag: Positive-Addend Overflow Formula

The RND instruction adds a positive rounding constant to the accumulator. The standard overflow formula `V = (sign_A XOR sign_R) AND (sign_B XOR sign_R)` simplifies to `V = (sign_A XOR sign_R) AND sign_R` when sign_B = 0 (positive constant). This means overflow is only possible in the positive-to-negative direction - adding a positive value to a negative value cannot overflow (it moves toward zero). A simple `sign_A XOR sign_R` (any sign change) is incorrect because it false-positives when a small negative value rounds to zero/positive.

## Bit-Test-and-Branch SSH Pop Semantics

The manual distinguishes between two groups of bit-test instructions regarding SSH pop:

- **BRCLR/BRSET/BSCLR/BSSET** (bit-test-and-branch/subroutine): the manual explicitly states SSH is popped when it is the source register (p.13-26, 13-29, 13-32, 13-35).
- **JCLR/JSET/JSCLR/JSSET** (bit-test-and-jump): no mention of SSH pop in their manual pages.
- **BCLR/BSET/BCHG/BTST** (pure bit operations): no SSH pop (these are not branches).

The `read_bit_test_operand` function takes a `pop_ssh` parameter to distinguish the Branch/BranchSub variants (which pop) from the Jump/JumpSub variants (which don't).

## DO FOREVER Cycle Count

DO FOREVER = 4 cycles per Table A-1, distinct from all other DO variants (5 cycles). DOR FOREVER timing is not separately listed in Table A-1.
