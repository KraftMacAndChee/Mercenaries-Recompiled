# DSP56300 Emulator -- Known Limitations

Known deviations from the DSP56300 Family Manual (Rev. 5). Features listed
below are stored in registers but do not affect execution behavior.

---

## 1. Unimplemented SR Mode Bits

### 1.1 Sixteen-Bit Arithmetic Mode -- SR bit 17 (SA)

When SA=1, the data path switches to 16-bit operation: operand widths become
16/32/40 instead of 24/48/56, data is right-aligned in 24-bit memory words,
rounding and limiting positions shift, and instructions like MERGE/EXTRACT/INSERT
operate on different bit fields.

**Status:** Stored but never read. All arithmetic uses 24-bit data paths.

### 1.2 Sixteen-Bit Compatibility Mode -- SR bit 13 (SC)

When SC=1 (DSP56000 compatibility): MOVE operations to/from PCU registers clear
the upper 8 MSBs. The AGU clears the 8 MSBs of address calculations. The N
register sign bit moves from bit 23 to bit 15. Loop count zero causes the loop
to execute 2^16 times (instead of 2^24).

**Status:** Setting SC=1 prints a warning but does not change behavior.

### 1.3 Double-Precision Multiply Mode -- SR bit 14 (DM)

When DM=1, four specific MPY/MAC register combinations implement a 48x48->96-bit
double-precision multiply. The DSP56300 manual recommends using DMAC instead.

**Status:** Stored but never read. Affected operations always execute in
single-precision mode.

### 1.4 Cache Enable -- SR bit 19 (CE)

Controls the instruction cache. The JIT compiler provides equivalent acceleration.
Cache instructions (PFLUSH, PFLUSHUN, PFREE, PLOCK, PUNLOCK, PLOCKR, PUNLOCKR)
are NOPs with correct AGU side effects.

### 1.5 Core Priority -- SR bits 23:22 (CP)

Controls priority of core vs DMA accesses on the external bus. Stored and
preserved but no external bus to arbitrate.

---

## 2. Unimplemented OMR Bits

### 2.1 Stack Extension -- OMR bit 20 (SEN)

When SEN=1, the 16-entry hardware stack spills to data memory via the EP register.

**Status:** Stored but not implemented. Stack overflow/underflow posts
STACK_ERROR but no spill/fill occurs.

### 2.2 Stack Extension Status -- OMR bits 17-19 (EOV, EUN, WRP)

Sticky status bits for stack extension overflow, underflow, and copy events.
Never set since stack extension is not implemented.

### 2.3 Memory Switch Mode -- OMR bit 7 (MS)

When MS=1, portions of internal program RAM are remapped to X/Y data memory.
Stored but not implemented.

### 2.4 External Bus and Hardware Configuration Bits

The following OMR bits control external hardware and have no effect in the
emulator: M[D:A] (0-3), EBD (4), SD (6), CDP (8-9), BE (10), TAS (11),
BRT (12), ABE (13), APD (14), ATE (15), MSW (21-22), PEN (23).

---

## 3. Other Behavioral Gaps

### 3.1 Pipeline Interlocks Not Modeled

The hardware inserts NOP cycles for read-after-write hazards on accumulators,
SR, and AGU registers. The emulator executes all instructions atomically with
base cycle counts only. Functional correctness is not affected.

### 3.2 Instruction Cache Not Modeled

The 1K-word instruction cache is not simulated. Cache instructions execute as
NOPs or with AGU side effects only.

### 3.3 DEBUG/DEBUGcc

DEBUG/DEBUGcc are NOPs. No debug processing state is entered.

### 3.4 RESET Instruction

RESET should reset all on-chip peripherals. Treated as a 7-cycle NOP.

### 3.5 STOP/WAIT Distinction

Both set a power_state flag that stops the run loop. The distinction between
STOP (full clock halt) and WAIT (peripherals continue) is not modeled.

---

## 4. Timing Simplifications

- Cycle counts use Appendix A Table A-1 base values only (no +pru, +lab, +lim).
- Wait states for external memory access are not modeled.
- Stack extension delays (Table A-3) are not modeled.
- DMA contention is not modeled.
