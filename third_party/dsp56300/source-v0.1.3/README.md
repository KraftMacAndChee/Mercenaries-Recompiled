Motorola DSP56300 Toolkit
=========================

[![codecov](https://codecov.io/gh/mborgerson/dsp56300/graph/badge.svg?token=EV4KQIYH1R)](https://codecov.io/gh/mborgerson/dsp56300)

A toolkit for working with the Motorola (Freescale/NXP) DSP56300 family of digital signal processors, including an assembler, disassembler, and emulator.

Crates
------

| Crate | Description |
|---|---|
| `dsp56300-core` | ISA constants, register indices, instruction decoder |
| `dsp56300-asm` | Assembler library/CLI |
| `dsp56300-disasm` | Disassembler library/CLI |
| `dsp56300-emu` | JIT emulator library/CLI |
| `dsp56300-emu-ffi` | C FFI layer; builds as a static library |

Building
--------

```sh
cargo build
cargo build --release
cargo test
```

Assembler
---------

The `dsp56300-asm` crate provides a library and CLI for assembling DSP56300 source files.

```sh
# Assemble a file (default: 4-byte little-endian words)
dsp56300-asm program.s -o program.bin

# Native DSP byte order (3 bytes/word, big-endian)
dsp56300-asm program.s -o program.bin -f u24be

# Text output (a56-compatible LOD format)
dsp56300-asm program.s -f lod

# Print segment map to stderr
dsp56300-asm program.s -o program.bin -v
```

Output formats: `u32le` (default), `u24be`, `u24le`, `lod`.

As a library:

```rust
use dsp56300_asm::assemble;

let result = assemble("org p:$0\n  nop\n  jmp p:$0\n").unwrap();
assert_eq!(result.segments[0].words, vec![0x000000, 0x0C0000]);
```

Disassembler
------------

The `dsp56300-disasm` crate provides a library and CLI for disassembling program memory into readable assembly.

```sh
# Disassemble a binary (default: 4-byte little-endian words)
dsp56300-disasm program.bin

# Native DSP byte order
dsp56300-disasm -f u24be program.bin

# Start at a specific address, limit output
dsp56300-disasm -s 0x100 -n 32 program.bin
```

Output is colorized when writing to a terminal (`--color always|never|auto`):

```
P:$000000  000000         nop
P:$000001  0C0042         jmp p:$0042
P:$000002  0A8580 001234  jclr #0,x:$ffffc5,p:$1234
```

As a library:

```rust
use dsp56300_disasm::disassemble;

let (text, len) = disassemble(0, 0x0C0042, 0);
assert_eq!(text, "jmp p:$0042");
assert_eq!(len, 1);
```

Emulator
--------

The `dsp56300-emu` crate provides a core emulator with a JIT execution engine, built with the [Cranelift](https://cranelift.dev/) compiler backend. A library and simple CLI for running an emulator are included.

```rust
use dsp56300_emu::core::{DspState, MemoryMap};
use dsp56300_emu::jit::JitEngine;

let mut pram = vec![0u32; 4096];
pram[0] = 0x0C0100; // JMP $100

let map = MemoryMap::from_pram_buffer(pram.as_mut_ptr(), pram.len() as u32);
let mut state = DspState::new(map);
let mut jit = JitEngine::new(pram.len());

state.run(&mut jit, 1000);
assert_eq!(state.pc, 0x100);
```

### From C/C++

The `dsp56300-emu-ffi` crate exposes the same functionality through a C API.

Build the static library and include the header:

```sh
cargo build --release -p dsp56300-emu-ffi --no-default-features
# produces: target/release/libdsp56300_emu_ffi.a
# header:   crates/emu-ffi/include/dsp56300.h
```

Link with `-ldsp56300_emu_ffi -ldl -lpthread -lm` (Linux) or the platform equivalent. See [`crates/emu-ffi/include/dsp56300.h`](crates/emu-ffi/include/dsp56300.h) for the full API reference and [`crates/emu-ffi/examples/quickstart.c`](crates/emu-ffi/examples/quickstart.c) for a usage example.

Examples
--------

The `examples` directory contains audio effects borrowed from the [a56](http://www.zdomain.com/a56.html) assembler source tree. The `scripts/dsp56300-run` wrapper script assembles and runs them with live audio on Linux (requires cpp, ffmpeg, and aplay).

| Example | Description |
|---|---|
| `thru.a56` | Pass-through (no processing) |
| `caltone.a56` | Sine wave calibration tone via table lookup |
| `pink.a56` | Pink noise generator |
| `reverb.a56` | Schroeder reverb |
| `sixcomb.a56` | Six-comb reverb |
| `flange.a56` | Flanging effect |
| `chorus.a56` | Chorus effect |

Run any example with live audio using the runner script:

```sh
scripts/dsp56300-run examples/reverb.a56
```

References
----------

- [DSP56300 Family Manual, Rev 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf) - referred to as "DSP56300FM" in source comments

Acknowledgements
----------------

The official Motorola DSP56300 assembler (`asm56300`), Version 6.3.15 was used for exhaustive roundtrip testing of the assembler and disassembler across the full instruction encoding space.

The DSP56300 emulator from [xemu](https://github.com/xemu-project/xemu) (originally based on DSP56001 emulation from [ARAnyM](https://aranym.github.io/), [Hatari](https://hatari.tuxfamily.org/), and later adapted for DSP56300 emulation in [XQEMU](https://github.com/xqemu/xqemu)) was used for early differential testing for the JIT emitter.
