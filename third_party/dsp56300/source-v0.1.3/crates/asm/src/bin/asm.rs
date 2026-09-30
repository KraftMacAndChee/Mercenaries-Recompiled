//! dsp56300-asm: DSP56300 command-line assembler.
//!
//! Reads DSP56300 assembly source and writes binary output in several formats.

use std::fmt;
use std::fs;
use std::io::{self, Read, Write};
use std::path::PathBuf;

use clap::{Parser, ValueEnum};

use dsp56300_asm::{assemble_with_include_dirs, format_listing};

#[derive(Parser)]
#[command(name = "dsp56300-asm", version, about = "DSP56300 assembler")]
struct Args {
    /// Input assembly file (default: stdin)
    file: Option<PathBuf>,

    /// Output file (default: stdout)
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Output word format
    #[arg(short, long, default_value_t = Format::U32Le)]
    format: Format,

    /// Write listing file (source interleaved with assembled output)
    #[arg(short = 'l', long = "listing")]
    listing: Option<PathBuf>,

    /// Additional include search directories
    #[arg(short = 'I', long = "include")]
    include_dirs: Vec<PathBuf>,

    /// Print segment map to stderr
    #[arg(short, long)]
    verbose: bool,
}

#[derive(Clone, Copy, ValueEnum)]
enum Format {
    /// 4 bytes/word, little-endian u32
    #[value(name = "u32le")]
    U32Le,
    /// 3 bytes/word, big-endian (native DSP byte order)
    #[value(name = "u24be")]
    U24Be,
    /// 3 bytes/word, little-endian
    #[value(name = "u24le")]
    U24Le,
    /// Text: "S AAAA WWWWWW" per word (a56-compatible)
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
    fn encode_word(self, w: u32) -> Vec<u8> {
        match self {
            Format::U32Le => (w & 0x00FF_FFFF).to_le_bytes().to_vec(),
            Format::U24Be => vec![(w >> 16) as u8, (w >> 8) as u8, w as u8],
            Format::U24Le => vec![w as u8, (w >> 8) as u8, (w >> 16) as u8],
            Format::Lod => unreachable!("LOD format uses write_segments directly"),
        }
    }
}

fn space_char(space: dsp56300_asm::ast::MemorySpace) -> char {
    match space {
        dsp56300_asm::ast::MemorySpace::X => 'X',
        dsp56300_asm::ast::MemorySpace::Y => 'Y',
        dsp56300_asm::ast::MemorySpace::P => 'P',
        dsp56300_asm::ast::MemorySpace::L => 'L',
    }
}

fn write_segments(
    out: &mut dyn Write,
    result: &dsp56300_asm::AssembleResult,
    fmt: Format,
    verbose: bool,
) -> io::Result<()> {
    for seg in &result.segments {
        if verbose {
            eprintln!(
                "  {}:${:06X}  {} word{}",
                space_char(seg.space),
                seg.org,
                seg.words.len(),
                if seg.words.len() == 1 { "" } else { "s" },
            );
        }
        let sc = space_char(seg.space);
        for (i, &w) in seg.words.iter().enumerate() {
            if matches!(fmt, Format::Lod) {
                writeln!(out, "{} {:04X} {:06X}", sc, seg.org as usize + i, w)?;
            } else {
                out.write_all(&fmt.encode_word(w))?;
            }
        }
    }
    if matches!(fmt, Format::Lod) {
        for (name, &value) in &result.symbols {
            writeln!(out, "I {:06X} {}", value as u32, name)?;
        }
    }
    Ok(())
}

fn main() {
    let args = Args::parse();

    let source = match args.file {
        Some(ref path) if path.as_os_str() != "-" => fs::read_to_string(path).unwrap_or_else(|e| {
            eprintln!("error: cannot read '{}': {e}", path.display());
            std::process::exit(1);
        }),
        _ => {
            let mut s = String::new();
            io::stdin().read_to_string(&mut s).unwrap_or_else(|e| {
                eprintln!("error: reading stdin: {e}");
                std::process::exit(1);
            });
            s
        }
    };

    let include_dirs = if let Some(ref path) = args.file {
        let mut dirs = vec![
            path.parent()
                .unwrap_or(std::path::Path::new("."))
                .to_path_buf(),
        ];
        dirs.extend(args.include_dirs.iter().cloned());
        dirs
    } else {
        args.include_dirs.clone()
    };

    let result = assemble_with_include_dirs(&source, &include_dirs);

    let mut result = match result {
        Ok(r) => r,
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    };

    // Tag top-level source lines with the input filename.
    if let Some(ref path) = args.file
        && path.as_os_str() != "-"
    {
        let name = path
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        for origin in &mut result.source_lines {
            if origin.file.is_empty() {
                origin.file = name.clone();
            }
        }
    }

    for w in &result.warnings {
        eprintln!("warning: line {}: [{:?}] {}", w.line, w.kind, w.msg);
    }

    if args.verbose {
        let total: usize = result.segments.iter().map(|s| s.words.len()).sum();
        eprintln!(
            "{} segment{}, {} word{} total",
            result.segments.len(),
            if result.segments.len() == 1 { "" } else { "s" },
            total,
            if total == 1 { "" } else { "s" },
        );
    }

    if let Some(ref path) = args.output {
        let mut file = fs::File::create(path).unwrap_or_else(|e| {
            eprintln!("error: cannot create '{}': {e}", path.display());
            std::process::exit(1);
        });
        write_segments(&mut file, &result, args.format, args.verbose).unwrap_or_else(|e| {
            eprintln!("error: writing '{}': {e}", path.display());
            std::process::exit(1);
        });
    } else {
        let stdout = io::stdout();
        let mut out = io::BufWriter::new(stdout.lock());
        write_segments(&mut out, &result, args.format, args.verbose).unwrap_or_else(|e| {
            eprintln!("error: writing stdout: {e}");
            std::process::exit(1);
        });
    }

    if let Some(ref path) = args.listing {
        let listing = format_listing(&result);
        fs::write(path, &listing).unwrap_or_else(|e| {
            eprintln!("error: cannot write listing '{}': {e}", path.display());
            std::process::exit(1);
        });
    }
}
