//! dsp56300-emu: DSP56300 command-line emulator for audio processing.
//!
//! Loads a program in LOD format, then streams stereo s24le PCM through
//! the DSP via stdin/stdout.
//!
//! Example:
//!   ffmpeg -f alsa -i default -f s24le -ac 2 -ar 48000 - |
//!     dsp56300-emu examples/reverb.out |
//!     aplay -f S24_3LE -c 2 -r 48000

use std::collections::HashMap;
use std::fs;
use std::io::{self, Read, Write};
use std::path::PathBuf;

use clap::Parser;

use dsp56300_core::MemSpace;
use dsp56300_emu::core::{DspState, MemoryMap, MemoryRegion, RegionKind};
use dsp56300_emu::jit::JitEngine;

type SymbolTable = HashMap<String, u32>;

#[derive(Parser)]
#[command(
    name = "dsp56300-emu",
    version,
    about = "DSP56300 audio effect processor"
)]
struct Args {
    /// Program file in LOD format ("S AAAA WWWWWW" per word, "I AAAA name" for symbols)
    program: PathBuf,

    /// X-space address or symbol for left input sample
    #[arg(long, default_value = "in_l")]
    in_l: String,

    /// X-space address or symbol for right input sample
    #[arg(long, default_value = "in_r")]
    in_r: String,

    /// X-space address or symbol for left output sample
    #[arg(long, default_value = "out_l")]
    out_l: String,

    /// X-space address or symbol for right output sample
    #[arg(long, default_value = "out_r")]
    out_r: String,

    /// P-space entry point address or symbol
    #[arg(long, default_value = "0x0000")]
    entry: String,

    /// P-space address or symbol for per-sample routine (set PC here each sample).
    /// If a "hf_comp" symbol exists and this is not set, it is used automatically.
    #[arg(long)]
    sample_fn: Option<String>,

    /// Cycles to run per sample
    #[arg(long, default_value_t = 1000)]
    cycles: i32,

    /// X-space size in words
    #[arg(long, default_value_t = 0x10000, value_parser = parse_hex_or_dec)]
    xram_size: u32,

    /// Y-space size in words
    #[arg(long, default_value_t = 0x10000, value_parser = parse_hex_or_dec)]
    yram_size: u32,

    /// P-space size in words
    #[arg(long, default_value_t = 0x10000, value_parser = parse_hex_or_dec)]
    pram_size: u32,

    /// Run init routine then exit (for debugging)
    #[arg(long)]
    init_only: bool,

    /// Print register state after processing
    #[arg(short, long)]
    verbose: bool,

    /// Use single-step interpreter instead of JIT
    #[arg(long)]
    interpret: bool,

    /// Number of channels (1=mono, 2=stereo)
    #[arg(long, default_value_t = 2)]
    channels: u16,

    /// Trace execution (print PC/opcode at each step)
    #[arg(long)]
    trace: bool,
}

fn parse_hex_or_dec(s: &str) -> Result<u32, String> {
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u32::from_str_radix(hex, 16).map_err(|e| e.to_string())
    } else if let Some(hex) = s.strip_prefix('$') {
        u32::from_str_radix(hex, 16).map_err(|e| e.to_string())
    } else {
        s.parse::<u32>().map_err(|e| e.to_string())
    }
}

fn resolve_addr(s: &str, symbols: &SymbolTable) -> Result<u32, String> {
    match parse_hex_or_dec(s) {
        Ok(v) => Ok(v),
        Err(_) => symbols
            .get(&s.to_lowercase())
            .copied()
            .ok_or_else(|| format!("unknown symbol or invalid address: '{s}'")),
    }
}

/// Load a LOD file into DSP memory.
///
/// Recognized line formats:
/// - `S AAAA WWWWWW` - data word (space X/Y/P, hex address, hex word)
/// - `I AAAA name`   - symbol definition (hex address, name)
///
/// All other lines are ignored.
fn load_lod(dsp: &mut DspState, contents: &str) -> (usize, SymbolTable) {
    let mut count = 0;
    let mut symbols = SymbolTable::new();

    for line in contents.lines() {
        let line = line.trim();
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() != 3 {
            continue;
        }
        match parts[0] {
            "I" => {
                if let Ok(addr) = u32::from_str_radix(parts[1], 16) {
                    symbols.insert(parts[2].to_lowercase(), addr);
                }
            }
            tag => {
                let space = match tag {
                    "X" => MemSpace::X,
                    "Y" => MemSpace::Y,
                    "P" => MemSpace::P,
                    _ => continue,
                };
                let Ok(addr) = u32::from_str_radix(parts[1], 16) else {
                    continue;
                };
                let Ok(word) = u32::from_str_radix(parts[2], 16) else {
                    continue;
                };
                dsp.write_memory(space, addr, word);
                count += 1;
            }
        }
    }

    (count, symbols)
}

fn read_s24_le(buf: &[u8]) -> u32 {
    buf[0] as u32 | (buf[1] as u32) << 8 | (buf[2] as u32) << 16
}

fn write_s24_le(w: &mut impl Write, v: u32) -> io::Result<()> {
    w.write_all(&[v as u8, (v >> 8) as u8, (v >> 16) as u8])
}

fn main() {
    let args = Args::parse();

    let contents = fs::read_to_string(&args.program).unwrap_or_else(|e| {
        eprintln!("error: cannot read '{}': {e}", args.program.display());
        std::process::exit(1);
    });

    // Set up memory
    let xram_size = args.xram_size as usize;
    let yram_size = args.yram_size as usize;
    let pram_size = args.pram_size as usize;

    let mut xram = vec![0u32; xram_size];
    let mut yram = vec![0u32; yram_size];
    let mut pram = vec![0u32; pram_size];

    let map = MemoryMap {
        x_regions: vec![MemoryRegion {
            start: 0,
            end: xram_size as u32,
            kind: RegionKind::Buffer {
                base: xram.as_mut_ptr(),
                offset: 0,
            },
        }],
        y_regions: vec![MemoryRegion {
            start: 0,
            end: yram_size as u32,
            kind: RegionKind::Buffer {
                base: yram.as_mut_ptr(),
                offset: 0,
            },
        }],
        p_regions: vec![MemoryRegion {
            start: 0,
            end: pram_size as u32,
            kind: RegionKind::Buffer {
                base: pram.as_mut_ptr(),
                offset: 0,
            },
        }],
    };

    let mut dsp = DspState::new(map);
    let mut jit = JitEngine::new(pram_size);

    let (count, symbols) = load_lod(&mut dsp, &contents);
    if args.verbose {
        eprintln!("Loaded {} words from {}", count, args.program.display());
    }

    // Resolve address arguments (numeric or symbol name)
    let resolve = |s: &str| {
        resolve_addr(s, &symbols).unwrap_or_else(|e| {
            eprintln!("error: {e}");
            std::process::exit(1);
        })
    };

    let in_l = resolve(&args.in_l);
    let in_r = resolve(&args.in_r);
    let out_l = resolve(&args.out_l);
    let out_r = resolve(&args.out_r);
    let entry = if args.entry == "0x0000" {
        // Auto-detect: prefer "hf_init" symbol over raw address 0
        symbols
            .get("hf_init")
            .copied()
            .unwrap_or_else(|| resolve(&args.entry))
    } else {
        resolve(&args.entry)
    };
    let sample_fn: Option<u32> = args
        .sample_fn
        .as_deref()
        .map(resolve)
        .or_else(|| symbols.get("hf_comp").copied());

    use dsp56300_core::reg;
    use dsp56300_emu::core::PowerState;
    const WAIT_OPCODE: u32 = 0x000086;
    let sentinel_pc = pram_size as u32 - 1;
    if sample_fn.is_some() {
        dsp.write_memory(MemSpace::P, sentinel_pc, WAIT_OPCODE);
    }

    dsp.pc = entry;

    // Run init code if sample_fn is set and different from entry.
    // Push sentinel_pc as the return address so RTS from init executes WAIT
    // and halts cleanly, then call run() for the full init sequence.
    if let Some(sample_fn) = sample_fn
        && entry != sample_fn
    {
        if args.verbose {
            eprintln!("Running init at P:${:04X}...", entry);
        }
        let sp = dsp.registers[reg::SP] as usize;
        let new_sp = sp + 1;
        dsp.stack[0][new_sp & 0xF] = sentinel_pc; // SSH = return address
        dsp.stack[1][new_sp & 0xF] = dsp.registers[reg::SR]; // SSL = status
        dsp.registers[reg::SP] = new_sp as u32;

        dsp.run(&mut jit, 10_000_000);

        if dsp.power_state != PowerState::Wait {
            eprintln!(
                "warning: init did not complete after 10M cycles, PC is at ${:04X}",
                dsp.pc
            );
        }
        dsp.power_state = PowerState::Normal;
    }

    if args.init_only {
        if args.verbose {
            print_state(&dsp);
        }
        return;
    }

    // Stream s24le PCM: stdin -> DSP -> stdout
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut reader = io::BufReader::new(stdin.lock());
    let mut writer = io::BufWriter::new(stdout.lock());

    let stereo = args.channels == 2;
    let bytes_per_frame = args.channels as usize * 3;
    let mut frame_buf = vec![0u8; bytes_per_frame];

    loop {
        match reader.read_exact(&mut frame_buf) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => {
                eprintln!("error: reading stdin: {e}");
                std::process::exit(1);
            }
        }

        let left = read_s24_le(&frame_buf);
        let right = if stereo {
            read_s24_le(&frame_buf[3..])
        } else {
            left
        };

        dsp.write_memory(MemSpace::X, in_l, left);
        dsp.write_memory(MemSpace::X, in_r, right);

        if let Some(sample_fn) = sample_fn {
            // Simulate JSR: push sentinel as return address
            dsp.pc = sample_fn;
            let sp = dsp.registers[reg::SP] as usize;
            let new_sp = sp + 1;
            dsp.stack[0][new_sp & 0xF] = sentinel_pc;
            dsp.stack[1][new_sp & 0xF] = dsp.registers[reg::SR];
            dsp.registers[reg::SP] = new_sp as u32;
            dsp.power_state = PowerState::Normal;
        }

        if args.interpret {
            for i in 0..args.cycles {
                if dsp.power_state != PowerState::Normal {
                    break;
                }
                if args.trace {
                    let pc = dsp.pc;
                    let opcode = dsp.read_memory(MemSpace::P, pc);
                    eprintln!(
                        "  step {:3}: PC=${:04X} op=${:06X} SP={}",
                        i,
                        pc,
                        opcode,
                        dsp.registers[reg::SP]
                    );
                }
                dsp.execute_one(&mut jit);
            }
        } else {
            dsp.run(&mut jit, args.cycles);
        }

        let out_l_val = dsp.read_memory(MemSpace::X, out_l);
        let out_r_val = dsp.read_memory(MemSpace::X, out_r);

        write_s24_le(&mut writer, out_l_val).unwrap();
        if stereo {
            write_s24_le(&mut writer, out_r_val).unwrap();
        }
    }

    writer.flush().unwrap();

    if args.verbose {
        print_state(&dsp);
    }
}

fn print_state(dsp: &DspState) {
    use dsp56300_core::reg;
    eprintln!("--- DSP State ---");
    eprintln!("  PC: ${:06X}", dsp.pc);
    eprintln!(
        "  SR: ${:06X}  OMR: ${:06X}  SP: {}",
        dsp.registers[reg::SR],
        dsp.registers[reg::OMR],
        dsp.registers[reg::SP]
    );
    eprintln!(
        "  A:  ${:02X}:{:06X}:{:06X}",
        dsp.registers[reg::A2] & 0xFF,
        dsp.registers[reg::A1],
        dsp.registers[reg::A0]
    );
    eprintln!(
        "  B:  ${:02X}:{:06X}:{:06X}",
        dsp.registers[reg::B2] & 0xFF,
        dsp.registers[reg::B1],
        dsp.registers[reg::B0]
    );
    eprintln!(
        "  X:  ${:06X}:{:06X}  Y: ${:06X}:{:06X}",
        dsp.registers[reg::X1],
        dsp.registers[reg::X0],
        dsp.registers[reg::Y1],
        dsp.registers[reg::Y0]
    );
    eprintln!(
        "  R0: ${:04X}  R1: ${:04X}  R2: ${:04X}  R3: ${:04X}",
        dsp.registers[reg::R0],
        dsp.registers[reg::R1],
        dsp.registers[reg::R2],
        dsp.registers[reg::R3]
    );
    eprintln!(
        "  N0: ${:04X}  N1: ${:04X}  N2: ${:04X}  N3: ${:04X}",
        dsp.registers[reg::N0],
        dsp.registers[reg::N1],
        dsp.registers[reg::N2],
        dsp.registers[reg::N3]
    );
    eprintln!(
        "  M0: ${:04X}  M1: ${:04X}  M2: ${:04X}  M3: ${:04X}",
        dsp.registers[reg::M0],
        dsp.registers[reg::M1],
        dsp.registers[reg::M2],
        dsp.registers[reg::M3]
    );
}
