// SPDX-License-Identifier: MIT

//! dsp56300-disasm: DSP56300 command-line disassembler.
//!
//! Reads a binary file or LOD text file of DSP56300 instruction words and
//! prints human-readable assembly to stdout.

use std::collections::{BTreeMap, HashMap};
use std::fmt;
use std::fs;
use std::io::{self, IsTerminal, Read, Write};
use std::path::PathBuf;

use clap::{Parser, ValueEnum};
use owo_colors::{OwoColorize, Style};

use dsp56300_disasm::{SymbolTable, disassemble_with_symbols};

#[derive(Parser)]
#[command(name = "dsp56300-disasm", version, about = "DSP56300 disassembler")]
struct Args {
    /// Input binary or LOD file (default: stdin)
    file: Option<PathBuf>,

    /// Starting PC address (hex or decimal, binary formats only)
    #[arg(short, long, default_value = "0", value_parser = parse_addr)]
    start: u32,

    /// Max number of instructions to disassemble
    #[arg(short = 'n', long)]
    count: Option<usize>,

    /// Input word format (auto-detected if omitted)
    #[arg(short, long)]
    format: Option<Format>,

    /// When to colorize output
    #[arg(short, long, default_value_t = ColorWhen::Auto)]
    color: ColorWhen,
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum Format {
    /// 4 bytes/word, little-endian u32 (pram[] array dump)
    #[value(name = "u32le")]
    U32Le,
    /// 3 bytes/word, big-endian (native DSP byte order)
    #[value(name = "u24be")]
    U24Be,
    /// 3 bytes/word, little-endian
    #[value(name = "u24le")]
    U24Le,
    /// LOD text format ("S AAAA WWWWWW" per word)
    #[value(name = "lod")]
    Lod,
}

impl fmt::Display for Format {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Format::U32Le => "u32le",
            Format::U24Be => "u24be",
            Format::U24Le => "u24le",
            Format::Lod => "lod",
        })
    }
}

impl Format {
    fn bytes_per_word(self) -> usize {
        match self {
            Format::U32Le => 4,
            Format::U24Be | Format::U24Le => 3,
            Format::Lod => unreachable!(),
        }
    }

    fn parse_word(self, bytes: &[u8]) -> u32 {
        match self {
            Format::U32Le => {
                u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) & 0x00FF_FFFF
            }
            Format::U24Be => (bytes[0] as u32) << 16 | (bytes[1] as u32) << 8 | bytes[2] as u32,
            Format::U24Le => (bytes[2] as u32) << 16 | (bytes[1] as u32) << 8 | bytes[0] as u32,
            Format::Lod => unreachable!(),
        }
    }
}

#[derive(Clone, Copy, ValueEnum)]
enum ColorWhen {
    /// Colorize if stdout is a terminal
    Auto,
    /// Always colorize
    Always,
    /// Never colorize
    Never,
}

impl fmt::Display for ColorWhen {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            ColorWhen::Auto => "auto",
            ColorWhen::Always => "always",
            ColorWhen::Never => "never",
        })
    }
}

/// Per-category `owo_colors::Style` palette.  When `enabled` is false, `paint`
/// returns the bare string so no ANSI codes appear in the output.
#[derive(Clone, Copy)]
struct Palette {
    enabled: bool,
    addr: Style,  // P:$XXXXXX label and $xxxx operand addresses - yellow
    hex: Style,   // raw hex bytes column - dimmed
    mnem: Style,  // instruction mnemonic - bold cyan
    reg: Style,   // register names - bright green
    imm: Style,   // #xxx immediates - bright yellow
    mem: Style,   // x:, y:, p:, l: memory space prefixes - magenta
    num: Style,   // decimal numbers in EA expressions - bright yellow
    label: Style, // symbol labels
}

impl Palette {
    fn color_on() -> Self {
        Self {
            enabled: true,
            addr: Style::new().yellow(),
            hex: Style::new().dimmed(),
            mnem: Style::new().bold().cyan(),
            reg: Style::new().bright_green(),
            imm: Style::new().bright_yellow(),
            mem: Style::new().magenta(),
            num: Style::new().bright_yellow(),
            label: Style::new().bright_white().bold(),
        }
    }

    fn color_off() -> Self {
        // Styles won't be used, but we still need values for the fields.
        Self {
            enabled: false,
            ..Self::color_on()
        }
    }

    fn paint(&self, s: &str, style: Style) -> String {
        if self.enabled {
            s.style(style).to_string()
        } else {
            s.to_owned()
        }
    }
}

/// Register names, longest first so prefix-matches don't fire early.
const REGISTERS: &[&str] = &[
    "ssh", "ssl", "omr", "ccr", "com", "eom", "vba", "a10", "b10", "a0", "a1", "a2", "b0", "b1",
    "b2", "x0", "x1", "y0", "y1", "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "n0", "n1", "n2",
    "n3", "n4", "n5", "n6", "n7", "m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "sp", "la", "lc",
    "sr", "mr", "sc", "sz", "ep", "ab", "ba", "a", "b", "x", "y",
];

/// Try to match a register name at the start of `s`.
/// Returns the length of the match, or `None`.
/// Requires a non-alphanumeric boundary after the name.
fn match_register(s: &str) -> Option<usize> {
    for &reg in REGISTERS {
        if let Some(after) = s.strip_prefix(reg) {
            let boundary = after
                .chars()
                .next()
                .map(|c| !c.is_alphanumeric() && c != '_')
                .unwrap_or(true);
            if boundary {
                return Some(reg.len());
            }
        }
    }
    None
}

/// Colorize the operand portion of a disassembled instruction.
///
/// `ops` is the text *after* the mnemonic (including the leading space when
/// operands are present). Returns a new `String` with ANSI escapes inserted
/// according to `palette`.
fn colorize_operands(ops: &str, p: &Palette) -> String {
    // Fast path: no color needed.
    if !p.enabled {
        return ops.to_owned();
    }

    let mut out = String::with_capacity(ops.len() * 2);
    let mut rest = ops;

    while !rest.is_empty() {
        if rest.starts_with(' ') {
            out.push(' ');
            rest = &rest[1..];
        } else if rest.starts_with('#') {
            // Immediate: #$xxxx or #N - consume until delimiter.
            let end = rest[1..]
                .find([',', ' ', ')'])
                .map(|n| n + 1)
                .unwrap_or(rest.len());
            out.push_str(&p.paint(&rest[..end], p.imm));
            rest = &rest[end..];
        } else if rest.starts_with('$') {
            // Hex address literal: $ followed by hex digits.
            let hex_len = rest[1..]
                .find(|c: char| !c.is_ascii_hexdigit())
                .map(|n| n + 1)
                .unwrap_or(rest.len());
            out.push_str(&p.paint(&rest[..hex_len], p.addr));
            rest = &rest[hex_len..];
        } else if let Some(ms) = ["x:", "y:", "p:", "l:"]
            .iter()
            .copied()
            .find(|&ms| rest.starts_with(ms))
        {
            out.push_str(&p.paint(ms, p.mem));
            rest = &rest[ms.len()..];
        } else if let Some(reg_len) = match_register(rest) {
            out.push_str(&p.paint(&rest[..reg_len], p.reg));
            rest = &rest[reg_len..];
        } else if rest.starts_with(|c: char| c.is_ascii_digit()) {
            // Decimal number (e.g. signed offsets in lua EA).
            let end = rest
                .find(|c: char| !c.is_ascii_digit())
                .unwrap_or(rest.len());
            out.push_str(&p.paint(&rest[..end], p.num));
            rest = &rest[end..];
        } else {
            // Punctuation (,  (  )  +  -) and anything else: pass through.
            let ch = rest.chars().next().unwrap();
            out.push(ch);
            rest = &rest[ch.len_utf8()..];
        }
    }

    out
}

fn parse_addr(s: &str) -> Result<u32, String> {
    let s = s.trim_start_matches("0x").trim_start_matches("0X");
    u32::from_str_radix(s, 16)
        .or_else(|_| s.parse::<u32>())
        .map_err(|_| format!("invalid address '{s}'; expected hex (0x1234) or decimal"))
}

/// Check if raw bytes look like LOD text (first non-empty line starts with
/// a valid LOD tag: P, X, Y, or I followed by a space).
fn looks_like_lod(data: &[u8]) -> bool {
    let text = match std::str::from_utf8(data.get(..256).unwrap_or(data)) {
        Ok(t) => t,
        Err(_) => return false,
    };
    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        return matches!(trimmed.as_bytes().first(), Some(b'P' | b'X' | b'Y' | b'I'))
            && trimmed.as_bytes().get(1) == Some(&b' ');
    }
    false
}

/// Parse LOD text into P-space words and symbols.
///
/// Symbols (I records) don't carry memory-space info, so we use a heuristic:
/// only keep symbols whose address has a P-space entry but no X/Y-space entry,
/// filtering out data-space variable names that happen to collide with P addresses.
fn parse_lod(text: &str) -> (BTreeMap<u32, u32>, HashMap<u32, Vec<String>>) {
    use std::collections::HashSet;

    let mut pwords: BTreeMap<u32, u32> = BTreeMap::new();
    let mut xy_addrs: HashSet<u32> = HashSet::new();
    let mut raw_symbols: Vec<(u32, String)> = Vec::new();

    for line in text.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() != 3 {
            continue;
        }
        match parts[0] {
            "P" => {
                if let (Ok(addr), Ok(word)) = (
                    u32::from_str_radix(parts[1], 16),
                    u32::from_str_radix(parts[2], 16),
                ) {
                    pwords.insert(addr, word);
                }
            }
            "X" | "Y" => {
                if let Ok(addr) = u32::from_str_radix(parts[1], 16) {
                    xy_addrs.insert(addr);
                }
            }
            "I" => {
                if let Ok(addr) = u32::from_str_radix(parts[1], 16) {
                    let name = parts[2];
                    if !name.starts_with("m_") {
                        raw_symbols.push((addr, name.to_string()));
                    }
                }
            }
            _ => {}
        }
    }

    // Keep symbols at P-space addresses that don't also appear in X/Y data.
    let mut symbols: HashMap<u32, Vec<String>> = HashMap::new();
    for (addr, name) in raw_symbols {
        if pwords.contains_key(&addr) && !xy_addrs.contains(&addr) {
            symbols.entry(addr).or_default().push(name);
        }
    }

    (pwords, symbols)
}

fn emit_instruction(
    out: &mut impl Write,
    p: &Palette,
    pc: u32,
    opcode: u32,
    next_word: u32,
    has_next: bool,
    symbols: &SymbolTable,
) -> u32 {
    let (text, len) = disassemble_with_symbols(pc, opcode, next_word, symbols);

    let (mnem, ops) = match text.find(' ') {
        Some(pos) => (&text[..pos], &text[pos..]),
        None => (text.as_str(), ""),
    };

    let hex_col = if len == 2 && has_next {
        format!("{opcode:06X} {next_word:06X}")
    } else {
        format!("{opcode:06X}       ")
    };

    writeln!(
        out,
        "{}  {}  {}{}",
        p.paint(&format!("P:${pc:06X}"), p.addr),
        p.paint(&hex_col, p.hex),
        p.paint(mnem, p.mnem),
        colorize_operands(ops, p),
    )
    .unwrap();

    len
}

fn disassemble_lod(data: &[u8], p: &Palette, count: Option<usize>, out: &mut impl Write) {
    let text = String::from_utf8_lossy(data);
    let (pwords, label_symbols) = parse_lod(&text);

    // Build a SymbolTable (addr -> single name) for the disassembler library
    // to resolve branch/jump targets. Pick the first name per address.
    let disasm_symbols: SymbolTable = label_symbols
        .iter()
        .map(|(&addr, names)| (addr, names[0].clone()))
        .collect();

    let addrs: Vec<u32> = pwords.keys().copied().collect();
    let mut i = 0;
    let mut instr_count: usize = 0;

    while i < addrs.len() {
        if count.is_some_and(|n| instr_count >= n) {
            break;
        }

        let pc = addrs[i];

        // Print a blank line before non-contiguous regions (skip first)
        if i > 0 && addrs[i - 1] + 1 != pc {
            writeln!(out).unwrap();
        }

        // Print symbol labels at this address
        if let Some(names) = label_symbols.get(&pc) {
            for name in names {
                writeln!(out, "{}:", p.paint(name, p.label)).unwrap();
            }
        }

        let opcode = pwords[&pc];
        let next_word = addrs
            .get(i + 1)
            .filter(|&&next_addr| next_addr == pc + 1)
            .map(|&a| pwords[&a])
            .unwrap_or(0);
        let has_next = i + 1 < addrs.len() && addrs[i + 1] == pc + 1;

        let len = emit_instruction(out, p, pc, opcode, next_word, has_next, &disasm_symbols);

        i += len as usize;
        instr_count += 1;
    }
}

fn disassemble_binary(
    data: &[u8],
    fmt: Format,
    start: u32,
    p: &Palette,
    count: Option<usize>,
    out: &mut impl Write,
) {
    let bpw = fmt.bytes_per_word();
    let total_words = data.len() / bpw;
    let no_symbols = SymbolTable::new();

    let mut word_idx: usize = 0;
    let mut pc: u32 = start;
    let mut instr_count: usize = 0;

    while word_idx < total_words {
        if count.is_some_and(|n| instr_count >= n) {
            break;
        }

        let opcode = fmt.parse_word(&data[word_idx * bpw..]);
        let next_word = if word_idx + 1 < total_words {
            fmt.parse_word(&data[(word_idx + 1) * bpw..])
        } else {
            0
        };

        let len = emit_instruction(
            out,
            p,
            pc,
            opcode,
            next_word,
            word_idx + 1 < total_words,
            &no_symbols,
        );

        word_idx += len as usize;
        pc = pc.wrapping_add(len);
        instr_count += 1;
    }
}

fn main() {
    let args = Args::parse();

    let data = match args.file {
        Some(ref path) if path.as_os_str() != "-" => fs::read(path).unwrap_or_else(|e| {
            eprintln!("error: cannot read '{}': {e}", path.display());
            std::process::exit(1);
        }),
        _ => {
            let mut buf = Vec::new();
            io::stdin().read_to_end(&mut buf).unwrap_or_else(|e| {
                eprintln!("error: reading stdin: {e}");
                std::process::exit(1);
            });
            buf
        }
    };

    let use_color = match args.color {
        ColorWhen::Always => true,
        ColorWhen::Never => false,
        ColorWhen::Auto => io::stdout().is_terminal(),
    };
    let p = if use_color {
        Palette::color_on()
    } else {
        Palette::color_off()
    };

    let fmt = args.format.unwrap_or_else(|| {
        if looks_like_lod(&data) {
            Format::Lod
        } else {
            Format::U32Le
        }
    });

    let stdout = io::stdout();
    let mut out = io::BufWriter::new(stdout.lock());

    if fmt == Format::Lod {
        disassemble_lod(&data, &p, args.count, &mut out);
    } else {
        disassemble_binary(&data, fmt, args.start, &p, args.count, &mut out);
    }
}
