//! DSP56300 assembler.

pub mod ast;
pub mod encode;
pub mod parser;
pub mod token;

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use ast::*;
use encode::{EncodeError, EncodedInstruction};

/// Symbol table: maps label/EQU names to their numeric values.
pub type SymbolTable = HashMap<String, i64>;

/// Assembled output segment.
#[derive(Debug, Clone)]
pub struct Segment {
    pub space: MemorySpace,
    pub org: u32,
    pub words: Vec<u32>,
}

/// Assembler warning.
#[derive(Debug, Clone)]
pub struct AssembleWarning {
    pub line: usize,
    pub kind: WarningKind,
    pub msg: String,
}

/// Warning categories matching official Motorola assembler diagnostics.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum WarningKind {
    /// Bit number >= 24 (meaningless on 24-bit DSP).
    BitNumberOutOfRange,
    /// ALU destination register duplicated in parallel move destination.
    DuplicateDestination,
    /// PM4 X-field destination register is invalid (a10, b10, x, y).
    InvalidPm4Destination,
    /// SSH used as loop count operand (DO/DOR). Hardware restriction.
    SshAsLoopCount,
    /// SSH cannot be both source and destination in a single move.
    SshSourceAndDest,
    /// Shift count outside 0..55 range (ASL/ASR immediate).
    ShiftCountOutOfRange,
    /// Post-update will not occur because the address register is also a
    /// destination of the parallel move.
    PostUpdateOnDestination,
    /// Multi-word instruction placed at an interrupt vector address (P:$0000-$003F).
    InstructionInInterruptVector,
}

/// Per-source-line listing information (address + encoded words).
#[derive(Debug, Clone)]
pub struct ListingLine {
    /// Memory space.
    pub space: MemorySpace,
    /// Program counter at this line.
    pub addr: u32,
    /// Encoded word(s) produced by this line (0, 1, or 2 words for instructions;
    /// arbitrary length for `dc`).
    pub words: Vec<u32>,
}

/// Result of assembling a source file.
#[derive(Debug)]
pub struct AssembleResult {
    pub segments: Vec<Segment>,
    pub warnings: Vec<AssembleWarning>,
    pub symbols: SymbolTable,
    /// Per-statement listing data, indexed by statement index (0-based).
    pub listing: Vec<Option<ListingLine>>,
    /// Per-statement source origin (file, line, text). Includes are expanded inline.
    pub source_lines: Vec<ast::SourceOrigin>,
}

/// Format an assembly listing combining source lines with assembled output.
///
/// Each line shows the source file and line number, memory space address,
/// encoded words, and original source text. Included files are shown inline
/// with their origin.
pub fn format_listing(result: &AssembleResult) -> String {
    use std::fmt::Write;
    let mut out = String::new();

    // Compute column width for the "file:line" location field.
    let max_loc_len = result
        .source_lines
        .iter()
        .map(|o| {
            if o.file.is_empty() {
                format!("{}", o.line).len()
            } else {
                o.file.len() + 1 + format!("{}", o.line).len()
            }
        })
        .max()
        .unwrap_or(5);
    let loc_width = max_loc_len.max(5);

    for (idx, listing_entry) in result.listing.iter().enumerate() {
        let origin = result.source_lines.get(idx);
        let source_text = origin.map(|o| o.text.as_str()).unwrap_or("");
        let file = origin.map(|o| o.file.as_str()).unwrap_or("");
        let src_line = origin.map(|o| o.line).unwrap_or(idx + 1);

        let loc = if file.is_empty() {
            format!("{:>width$}", src_line, width = loc_width)
        } else {
            format!(
                "{:>width$}",
                format!("{}:{}", file, src_line),
                width = loc_width
            )
        };

        // Padding for continuation lines: align under the first word.
        // Format is: "{loc}  {S}:{AAAA} {WWWWWW}"
        let word_col = loc_width + 2 + 7; // loc + "  " + "S:AAAA "

        match listing_entry {
            Some(entry) if !entry.words.is_empty() => {
                let sc = space_char(entry.space);
                writeln!(
                    out,
                    "{}  {}:{:04X} {:06X}  {}",
                    loc, sc, entry.addr, entry.words[0], source_text
                )
                .unwrap();
                for w in &entry.words[1..] {
                    writeln!(out, "{:pad$}{:06X}", "", w, pad = word_col).unwrap();
                }
            }
            Some(entry) => {
                let sc = space_char(entry.space);
                writeln!(
                    out,
                    "{}  {}:{:04X}         {}",
                    loc, sc, entry.addr, source_text
                )
                .unwrap();
            }
            None => {
                writeln!(
                    out,
                    "{:>width$}                   {}",
                    loc,
                    source_text,
                    width = 0
                )
                .unwrap();
            }
        }
    }
    out
}

fn space_char(space: MemorySpace) -> char {
    match space {
        MemorySpace::X => 'X',
        MemorySpace::Y => 'Y',
        MemorySpace::P => 'P',
        MemorySpace::L => 'L',
    }
}

/// Assemble error.
#[derive(Debug)]
pub enum AssembleError {
    Parse(parser::ParseError),
    Encode { line: usize, err: EncodeError },
}

impl std::fmt::Display for AssembleError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            AssembleError::Parse(e) => write!(f, "{}", e),
            AssembleError::Encode { line, err } => write!(f, "line {}: {}", line, err),
        }
    }
}

impl std::error::Error for AssembleError {}

/// Assemble a source string into binary segments.
///
/// Uses two-pass assembly: pass 1 collects label addresses and EQU values,
/// pass 2 encodes instructions with the resolved symbol table.
pub fn assemble(source: &str) -> Result<AssembleResult, AssembleError> {
    assemble_with_include_dirs(source, &[])
}

/// Assemble a source string, resolving `include` directives by searching
/// each directory in `include_dirs` in order.
pub fn assemble_with_include_dirs(
    source: &str,
    include_dirs: &[PathBuf],
) -> Result<AssembleResult, AssembleError> {
    let mut program = parser::parse(source).map_err(AssembleError::Parse)?;
    if !include_dirs.is_empty() {
        let base_dir = include_dirs
            .first()
            .map(|p| p.as_path())
            .unwrap_or(Path::new("."));
        expand_includes(&mut program, base_dir, include_dirs)?;
    }
    let symbols = resolve_symbols(&program)?;
    emit_segments(&program, &symbols)
}

/// Assemble a file, resolving `include` directives relative to the file's directory.
pub fn assemble_file(path: &Path) -> Result<AssembleResult, AssembleError> {
    assemble_file_with_include_dirs(path, &[])
}

/// Assemble a file with additional include search directories.
///
/// Includes are resolved first relative to the including file's directory,
/// then by searching each directory in `include_dirs` in order.
pub fn assemble_file_with_include_dirs(
    path: &Path,
    include_dirs: &[PathBuf],
) -> Result<AssembleResult, AssembleError> {
    let source = std::fs::read_to_string(path).map_err(|e| {
        AssembleError::Parse(parser::ParseError {
            line: 0,
            msg: format!("cannot read '{}': {}", path.display(), e),
        })
    })?;
    let mut program = parser::parse(&source).map_err(AssembleError::Parse)?;
    let base_dir = path.parent().unwrap_or(Path::new("."));
    expand_includes(&mut program, base_dir, include_dirs)?;
    let symbols = resolve_symbols(&program)?;
    emit_segments(&program, &symbols)
}

/// Resolve symbols by iterating pass 1 until instruction sizes converge.
///
/// The initial pass uses `instruction_size()` to estimate sizes. Then we
/// trial-encode each instruction with the resolved symbols to get the actual
/// encoded size. If any size differs, we re-run pass 1 using actual sizes and
/// iterate until stable.  This eliminates silent label corruption from
/// `instruction_size()` vs encoder disagreements.
fn resolve_symbols(program: &ast::Program) -> Result<SymbolTable, AssembleError> {
    let mut symbols = resolve_symbols_once(program, None)?;
    for _ in 0..10 {
        let refined = resolve_symbols_once(program, Some(&symbols))?;
        if refined == symbols {
            return Ok(symbols);
        }
        symbols = refined;
    }
    Ok(symbols)
}

/// Recursively expand `include` directives, splicing included file ASTs inline.
///
/// Search order: base_dir (the including file's directory), then each include_dir.
fn expand_includes(
    program: &mut ast::Program,
    base_dir: &Path,
    include_dirs: &[PathBuf],
) -> Result<(), AssembleError> {
    let mut i = 0;
    while i < program.statements.len() {
        if let Statement::Directive {
            dir: Directive::Include(ref path_str),
            ..
        } = program.statements[i]
        {
            let inc_path = resolve_include(path_str, base_dir, include_dirs).ok_or_else(|| {
                let searched: Vec<String> = std::iter::once(base_dir.to_path_buf())
                    .chain(include_dirs.iter().cloned())
                    .map(|d| d.display().to_string())
                    .collect();
                AssembleError::Parse(parser::ParseError {
                    line: i + 1,
                    msg: format!(
                        "cannot find '{}' (searched: {})",
                        path_str,
                        searched.join(", ")
                    ),
                })
            })?;
            let source = std::fs::read_to_string(&inc_path).map_err(|e| {
                AssembleError::Parse(parser::ParseError {
                    line: i + 1,
                    msg: format!("cannot include '{}': {}", inc_path.display(), e),
                })
            })?;
            let mut inc_program = parser::parse(&source).map_err(AssembleError::Parse)?;
            let inc_file = inc_path
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned();
            for origin in &mut inc_program.source_lines {
                if origin.file.is_empty() {
                    origin.file = inc_file.clone();
                }
            }
            let inc_base = inc_path.parent().unwrap_or(Path::new("."));
            expand_includes(&mut inc_program, inc_base, include_dirs)?;
            program.statements.splice(i..=i, inc_program.statements);
            program.source_lines.splice(i..=i, inc_program.source_lines);
        } else {
            i += 1;
        }
    }
    Ok(())
}

/// Find an include file by searching base_dir first, then each include_dir.
fn resolve_include(name: &str, base_dir: &Path, include_dirs: &[PathBuf]) -> Option<PathBuf> {
    let candidate = base_dir.join(name);
    if candidate.exists() {
        return Some(candidate);
    }
    for dir in include_dirs {
        let candidate = dir.join(name);
        if candidate.exists() {
            return Some(candidate);
        }
    }
    None
}

/// Per-psect state tracked during assembly.
struct PsectState {
    space: MemorySpace,
    pc: u32,
}

/// Save the current PC into the active psect (if any).
fn save_psect(psects: &mut HashMap<String, PsectState>, current: &Option<String>, pc: u32) {
    if let Some(name) = current
        && let Some(state) = psects.get_mut(name)
    {
        state.pc = pc;
    }
}

/// Walk statements collecting label addresses and EQU values.
///
/// When `prior_symbols` is provided, instructions are trial-encoded using those
/// symbols to determine the actual encoded size.  On the first call (no prior
/// symbols), `instruction_size()` is used as the estimate.
fn resolve_symbols_once(
    program: &ast::Program,
    prior_symbols: Option<&SymbolTable>,
) -> Result<SymbolTable, AssembleError> {
    let mut symbols = SymbolTable::new();
    let mut pc: u32 = 0;
    let mut psects: HashMap<String, PsectState> = HashMap::new();
    let mut current_psect: Option<String> = None;

    for (line_idx, stmt) in program.statements.iter().enumerate() {
        let line = line_idx + 1;
        let label = match stmt {
            Statement::Instruction { label, .. } | Statement::Directive { label, .. } => {
                label.as_deref()
            }
            Statement::Label(name) => Some(name.as_str()),
            Statement::Empty => None,
        };
        if let Some(name) = label {
            symbols.insert(name.to_string(), pc as i64);
        }

        match stmt {
            Statement::Empty | Statement::Label(_) => {}
            Statement::Directive { dir, .. } => match dir {
                Directive::Org { addr, .. } => {
                    pc = eval_at(addr, &symbols, pc, line)?;
                }
                Directive::Dc(values) => {
                    pc += values.len() as u32;
                }
                Directive::Ds(size) => {
                    let n = eval_at(size, &symbols, pc, line)?;
                    if n > 0xFFFFFF {
                        return Err(AssembleError::Encode {
                            line,
                            err: encode::EncodeError {
                                msg: "ds size out of range".into(),
                            },
                        });
                    }
                    pc += n;
                }
                Directive::End => break,
                Directive::Equ { name, value } => {
                    let v = eval_at(value, &symbols, pc, line)?;
                    symbols.insert(name.clone(), v as i64);
                }
                Directive::Psect { name, range } => {
                    save_psect(&mut psects, &current_psect, pc);
                    if let Some((space, start, _end)) = range {
                        let start_val = eval_at(start, &symbols, pc, line)?;
                        psects.insert(
                            name.clone(),
                            PsectState {
                                space: *space,
                                pc: start_val,
                            },
                        );
                        pc = start_val;
                    } else if let Some(state) = psects.get(name) {
                        pc = state.pc;
                    }
                    current_psect = Some(name.clone());
                }
                Directive::Align(alignment) => {
                    let n = eval_at(alignment, &symbols, pc, line)?;
                    if n > 0 {
                        pc = pc.div_ceil(n) * n;
                    }
                }
                Directive::Section(_)
                | Directive::EndSec
                | Directive::Xref(_)
                | Directive::Xdef(_)
                | Directive::Include(_) => {}
            },
            Statement::Instruction { inst, .. } => {
                pc += encoded_size(inst, pc, prior_symbols);
            }
        }
    }

    Ok(symbols)
}

/// Determine the size of an instruction.
///
/// When `prior_symbols` is available, trial-encodes the instruction using those
/// symbols.  The encoded result is authoritative - this eliminates any
/// `instruction_size()` vs encoder disagreement.  Falls back to the static
/// `instruction_size()` estimate if encoding fails (e.g. undefined symbol on
/// the first pass).
fn encoded_size(inst: &Instruction, pc: u32, prior_symbols: Option<&SymbolTable>) -> u32 {
    if let Some(syms) = prior_symbols
        && let Ok(enc) = encode::encode(inst, pc, syms)
    {
        return if enc.word1.is_some() { 2 } else { 1 };
    }
    instruction_size(inst)
}

/// Return the size (in words) of an instruction.
///
/// For EA-dependent instructions, checks whether the EA has an extension word.
/// For relative branches (Bcc/Bra/Bsr), conservatively returns 2 (long form).
fn instruction_size(inst: &Instruction) -> u32 {
    match inst {
        // Always 1 word
        Instruction::Nop
        | Instruction::Rts
        | Instruction::Rti
        | Instruction::Reset
        | Instruction::Stop
        | Instruction::Wait
        | Instruction::EndDo
        | Instruction::Illegal
        | Instruction::Inc(_)
        | Instruction::Dec(_)
        | Instruction::AddImm { .. }
        | Instruction::SubImm { .. }
        | Instruction::CmpImm { .. }
        | Instruction::AndImm { .. }
        | Instruction::OrImm { .. }
        | Instruction::EorImm { .. }
        | Instruction::AndI { .. }
        | Instruction::OrI { .. }
        | Instruction::AslImm { .. }
        | Instruction::AsrImm { .. }
        | Instruction::LslImm { .. }
        | Instruction::LsrImm { .. }
        | Instruction::AslReg { .. }
        | Instruction::AsrReg { .. }
        | Instruction::LslReg { .. }
        | Instruction::LsrReg { .. }
        | Instruction::Jcc { .. }
        | Instruction::Jscc { .. }
        | Instruction::Rep { .. }
        | Instruction::MovecImm { .. }
        | Instruction::MovecReg { .. }
        | Instruction::Tcc { .. }
        | Instruction::MulShift { .. }
        | Instruction::Div { .. }
        | Instruction::CmpU { .. }
        | Instruction::Norm { .. }
        | Instruction::LuaRel { .. }
        | Instruction::BccRn { .. }
        | Instruction::BraRn { .. }
        | Instruction::BsrRn { .. }
        | Instruction::BsccRn { .. }
        | Instruction::Brkcc { .. }
        | Instruction::Dmac { .. }
        | Instruction::MacSU { .. }
        | Instruction::MpySU { .. }
        | Instruction::LraRn { .. }
        | Instruction::Clb { .. }
        | Instruction::Normf { .. }
        | Instruction::Merge { .. }
        | Instruction::ExtractReg { .. }
        | Instruction::ExtractuReg { .. }
        | Instruction::InsertReg { .. }
        | Instruction::Debug
        | Instruction::Debugcc { .. }
        | Instruction::Trap
        | Instruction::Trapcc { .. }
        | Instruction::Pflush
        | Instruction::Pflushun
        | Instruction::Pfree => 1,

        // 1 word (short) or 2 words (long) -- short when force_short or literal addr < 0x1000
        Instruction::Jmp {
            target,
            force_short,
        }
        | Instruction::Jsr {
            target,
            force_short,
        } => {
            if *force_short {
                1
            } else if let Some(v) = target.as_u32() {
                if (v & 0xFFFFFF) < 0x1000 { 1 } else { 2 }
            } else {
                2
            }
        }

        // Always 2 words
        Instruction::AddLong { .. }
        | Instruction::SubLong { .. }
        | Instruction::CmpLong { .. }
        | Instruction::AndLong { .. }
        | Instruction::OrLong { .. }
        | Instruction::EorLong { .. }
        | Instruction::MpyI { .. }
        | Instruction::MpyrI { .. }
        | Instruction::MacI { .. }
        | Instruction::MacrI { .. }
        | Instruction::Do { .. }
        | Instruction::DoForever { .. }
        | Instruction::Dor { .. }
        | Instruction::DorForever { .. }
        | Instruction::Jclr { .. }
        | Instruction::Jset { .. }
        | Instruction::Jsclr { .. }
        | Instruction::Jsset { .. }
        | Instruction::Brclr { .. }
        | Instruction::Brset { .. }
        | Instruction::Bsclr { .. }
        | Instruction::Bsset { .. }
        | Instruction::MoveLongDisp { .. }
        | Instruction::Movep23Imm { .. }
        | Instruction::MovepXQqImm { .. }
        | Instruction::LraDisp { .. }
        | Instruction::ExtractImm { .. }
        | Instruction::ExtractuImm { .. }
        | Instruction::InsertImm { .. }
        | Instruction::Plockr { .. }
        | Instruction::Punlockr { .. } => 2,

        // Relative branches: conservatively 2 (long form)
        Instruction::Bcc { .. }
        | Instruction::Bra { .. }
        | Instruction::Bsr { .. }
        | Instruction::Bscc { .. } => 2,

        // 1 word + possible EA extension
        Instruction::MovecAa { .. } | Instruction::MovemAa { .. } | Instruction::Movep0 { .. } => 1,
        Instruction::MoveShortDisp { .. } => 1,

        // Jcc/Jscc EA: short form (1 word) when target is a literal < 0x1000,
        // otherwise 1 + ea extension word.  Must match encode_jcc_ea().
        Instruction::JccEa { ea, .. } | Instruction::JsccEa { ea, .. } => {
            if let EffectiveAddress::AbsAddr(expr) = ea
                && expr.as_u32().is_some_and(|v| (v & 0xFFFFFF) < 0x1000)
            {
                return 1;
            }
            1 + ea_ext_size(ea)
        }

        // EA-dependent: 1 + ea_has_ext
        Instruction::JmpEa { ea }
        | Instruction::JsrEa { ea }
        | Instruction::Lua { ea, .. }
        | Instruction::PlockEa { ea }
        | Instruction::PunlockEa { ea }
        | Instruction::Vsl { ea, .. } => 1 + ea_ext_size(ea),

        Instruction::MovecEa { ea, .. }
        | Instruction::MovemEa { ea, .. }
        | Instruction::Movep1 { ea, .. } => 1 + ea_ext_size(ea),

        Instruction::Movep23 { ea, .. } | Instruction::MovepXQq { ea, .. } => 1 + ea_ext_size(ea),

        // Bit ops: 1 + possible EA extension in target
        Instruction::Bchg { target, .. }
        | Instruction::Bclr { target, .. }
        | Instruction::Bset { target, .. }
        | Instruction::Btst { target, .. } => match target {
            BitTarget::Ea { ea, .. } => 1 + ea_ext_size(ea),
            _ => 1,
        },

        // Parallel: depends on move type
        Instruction::Parallel { pmove, .. } => 1 + pmove_ext_size(pmove),
    }
}

fn ea_ext_size(ea: &EffectiveAddress) -> u32 {
    match ea {
        EffectiveAddress::AbsAddr(_)
        | EffectiveAddress::ForceLongAbsAddr(_)
        | EffectiveAddress::Immediate(_) => 1,
        _ => 0,
    }
}

fn pmove_ext_size(pmove: &ParallelMove) -> u32 {
    match pmove {
        ParallelMove::None | ParallelMove::RegToReg { .. } => 0,
        // XImmReg and RegYImm always use EA mode 0x34 (immediate extension word)
        ParallelMove::XImmReg { .. } | ParallelMove::RegYImm { .. } => 1,
        // PM4/L absolute moves: 1-word short form when force_short or literal < $40,
        // otherwise 2-word long form with extension word.
        ParallelMove::XYAbs {
            addr, force_short, ..
        }
        | ParallelMove::LAbs {
            addr, force_short, ..
        } => {
            if *force_short {
                0
            } else if let Some(v) = addr.as_u32() {
                if (v & 0xFFFFFF) <= 0x3F { 0 } else { 1 }
            } else {
                1 // symbol/expression: assume long form
            }
        }
        ParallelMove::ImmToReg { imm, dst } => {
            // Must match the PM3 short-form check in encode_parallel().
            let is_bare_lit = matches!(imm, Expr::Literal(_));
            let is_frac = matches!(imm, Expr::Frac(_));
            let is_msb_reg = matches!(
                dst,
                Register::A
                    | Register::B
                    | Register::X0
                    | Register::X1
                    | Register::Y0
                    | Register::Y1
            );
            if let Some(v) = imm.as_u32() {
                let v24 = v & 0xFFFFFF;
                let fits = (is_bare_lit && v24 <= 0xFF)
                    || (is_msb_reg && (v24 & 0xFFFF) == 0)
                    || (is_frac && !dst.is_data_alu() && v24 <= 0xFF);
                if fits { 0 } else { 1 }
            } else {
                1 // symbol reference / expression: long form
            }
        }
        ParallelMove::EaUpdate { ea, .. } => ea_ext_size(ea),
        ParallelMove::XYMem { ea, .. }
        | ParallelMove::LMem { ea, .. }
        | ParallelMove::XReg { ea, .. }
        | ParallelMove::RegY { ea, .. }
        | ParallelMove::Pm0 { ea, .. } => ea_ext_size(ea),
        ParallelMove::LImm { .. } => 1,
        ParallelMove::XYDouble { x_ea, y_ea, .. } => ea_ext_size(x_ea).max(ea_ext_size(y_ea)),
        ParallelMove::Ifcc { .. } | ParallelMove::IfccU { .. } => 0,
    }
}

/// Build an [`AssembleWarning`] with `line: 0` (caller patches the line later).
fn warn(kind: WarningKind, msg: impl Into<String>) -> AssembleWarning {
    AssembleWarning {
        line: 0,
        kind,
        msg: msg.into(),
    }
}

/// Check an instruction for conditions that warrant warnings.
fn check_warnings(inst: &Instruction) -> Vec<AssembleWarning> {
    let mut warnings = Vec::new();

    match inst {
        // Bit number >= 24 for all bit-manipulation and bit-branch instructions
        Instruction::Bchg { bit, .. }
        | Instruction::Bclr { bit, .. }
        | Instruction::Bset { bit, .. }
        | Instruction::Btst { bit, .. }
        | Instruction::Jclr { bit, .. }
        | Instruction::Jset { bit, .. }
        | Instruction::Jsclr { bit, .. }
        | Instruction::Jsset { bit, .. }
        | Instruction::Brclr { bit, .. }
        | Instruction::Brset { bit, .. }
        | Instruction::Bsclr { bit, .. }
        | Instruction::Bsset { bit, .. } => {
            if let Some(v) = bit.try_eval_const()
                && v >= 24
            {
                warnings.push(warn(
                    WarningKind::BitNumberOutOfRange,
                    format!("bit number {v} >= 24 (DSP has 24-bit words)"),
                ));
            }
        }

        // Shift count outside 0..55 for ASL/ASR immediate (56-bit accumulators)
        Instruction::AslImm { shift, .. } | Instruction::AsrImm { shift, .. } => {
            if let Some(v) = shift.try_eval_const()
                && !(0..=55).contains(&v)
            {
                warnings.push(warn(
                    WarningKind::ShiftCountOutOfRange,
                    format!("shift count {v} outside 0..55 range"),
                ));
            }
        }

        // SSH as loop count
        Instruction::Do {
            source: LoopSource::Reg(Register::Ssh),
            ..
        }
        | Instruction::Dor {
            source: LoopSource::Reg(Register::Ssh),
            ..
        } => {
            warnings.push(warn(
                WarningKind::SshAsLoopCount,
                "SSH used as loop count operand",
            ));
        }

        // SSH as both source and destination in register-to-register move
        Instruction::MovecReg { src, dst, .. }
            if *src == Register::Ssh && *dst == Register::Ssh =>
        {
            warnings.push(warn(
                WarningKind::SshSourceAndDest,
                "SSH is both source and destination",
            ));
        }

        Instruction::MulShift { shift, .. } => {
            if let Some(v) = shift.try_eval_const()
                && v >= 24
            {
                warnings.push(warn(
                    WarningKind::BitNumberOutOfRange,
                    format!("immediate value {v} >= 24 (DSP has 24-bit words)"),
                ));
            }
        }

        Instruction::MovemEa { ea, reg, w: true } => {
            if let Some(rn) = ea_post_update_reg(ea)
                && *reg == Register::R(rn)
            {
                warnings.push(warn(
                    WarningKind::PostUpdateOnDestination,
                    format!("post-update will not occur on r{rn} (also a move destination)"),
                ));
            }
        }

        Instruction::Parallel { alu, pmove } => {
            check_parallel_warnings(alu, pmove, &mut warnings);
        }

        _ => {}
    }

    warnings
}

/// Returns `Some(true)` for accumulator A, `Some(false)` for B, `None` if no ALU dest.
fn alu_dest_is_a(alu: &ParallelAlu) -> Option<bool> {
    use dsp56300_core::Accumulator;
    match alu.dest_accumulator() {
        Some(Accumulator::A) => Some(true),
        Some(Accumulator::B) => Some(false),
        None => None,
    }
}

/// Strict overlap check: only full accumulator, mantissa, and composite registers.
/// Used for LImm where the official assembler doesn't flag A0/A2/B0/B2.
fn reg_overlaps_acc_strict(reg: &Register, acc_is_a: bool) -> bool {
    if matches!(reg, Register::Ab | Register::Ba) {
        return true;
    }
    if acc_is_a {
        matches!(reg, Register::A | Register::A1 | Register::A10)
    } else {
        matches!(reg, Register::B | Register::B1 | Register::B10)
    }
}

/// Check if a register overlaps with the given accumulator (A=true, B=false).
fn reg_overlaps_acc(reg: &Register, acc_is_a: bool) -> bool {
    // AB and BA overlap with both accumulators.
    if matches!(reg, Register::Ab | Register::Ba) {
        return true;
    }
    if acc_is_a {
        matches!(
            reg,
            Register::A | Register::A0 | Register::A1 | Register::A2 | Register::A10
        )
    } else {
        matches!(
            reg,
            Register::B | Register::B0 | Register::B1 | Register::B2 | Register::B10
        )
    }
}

/// Check if a register is an invalid PM4 X-field destination (composite registers).
fn is_invalid_pm4_dest(reg: &Register) -> bool {
    matches!(
        reg,
        Register::A10 | Register::B10 | Register::RegX | Register::RegY
    )
}

fn check_parallel_warnings(
    alu: &ParallelAlu,
    pmove: &ParallelMove,
    warnings: &mut Vec<AssembleWarning>,
) {
    let acc_is_a = alu_dest_is_a(alu);

    // Check duplicate destination: ALU dest accumulator vs PM dest register.
    // For LImm and logical/shift ALU ops, the official assembler only warns for
    // full/mantissa/composite registers (A/B/A1/B1/A10/B10/AB/BA), not for
    // extension sub-registers (A0/A2/B0/B2). Logical/shift ops only write A1/B1.
    let is_limm = matches!(pmove, ParallelMove::LImm { .. });
    let alu_is_logical_or_shift = alu.is_logical_or_shift();
    if let Some(is_a) = acc_is_a {
        let mut pm_dsts: Vec<&Register> = Vec::new();
        match pmove {
            ParallelMove::RegToReg { dst, .. } => pm_dsts.push(dst),
            ParallelMove::ImmToReg { dst, .. } => pm_dsts.push(dst),
            ParallelMove::XYMem {
                reg, write: true, ..
            } => pm_dsts.push(reg),
            ParallelMove::XYAbs {
                reg, write: true, ..
            } => pm_dsts.push(reg),
            ParallelMove::LMem {
                reg, write: true, ..
            } => pm_dsts.push(reg),
            ParallelMove::LAbs {
                reg, write: true, ..
            } => pm_dsts.push(reg),
            ParallelMove::LImm { reg, .. } => pm_dsts.push(reg),
            ParallelMove::XReg {
                x_reg,
                d2,
                write: true,
                ..
            } => {
                pm_dsts.push(x_reg);
                pm_dsts.push(d2);
            }
            ParallelMove::XImmReg { x_reg, d2, .. } => {
                pm_dsts.push(x_reg);
                pm_dsts.push(d2);
            }
            ParallelMove::RegY {
                d1, y_reg, write, ..
            } => {
                pm_dsts.push(d1);
                if *write {
                    pm_dsts.push(y_reg);
                }
            }
            ParallelMove::RegYImm { d1, y_reg, .. } => {
                pm_dsts.push(d1);
                pm_dsts.push(y_reg);
            }
            ParallelMove::XYDouble {
                x_reg,
                y_reg,
                x_write,
                y_write,
                ..
            } => {
                if *x_write {
                    pm_dsts.push(x_reg);
                }
                if *y_write {
                    pm_dsts.push(y_reg);
                }
            }
            ParallelMove::Pm0 { acc, ea, .. }
                // The official assembler only flags DuplicateDestination for Pm0
                // with simple EA modes (PostInc, PostDec, NoUpdate, PostIncN).
                // Complex modes (PostDecN, PreDec, Disp, AbsAddr) are silently accepted.
                if is_simple_ea(ea) => {
                    pm_dsts.push(acc);
                }
            _ => {}
        }
        for dst in &pm_dsts {
            let overlaps = if is_limm || alu_is_logical_or_shift {
                reg_overlaps_acc_strict(dst, is_a)
            } else {
                reg_overlaps_acc(dst, is_a)
            };
            if overlaps {
                warnings.push(warn(
                    WarningKind::DuplicateDestination,
                    "ALU destination duplicated in parallel move",
                ));
                break;
            }
        }
    }

    // XYDouble: both X and Y fields writing to the same register is a duplicate dest,
    // even when the ALU op has no destination (e.g. cmp/tst).
    if let ParallelMove::XYDouble {
        x_reg,
        y_reg,
        x_write: true,
        y_write: true,
        ..
    } = pmove
        && x_reg == y_reg
    {
        let already = warnings
            .iter()
            .any(|w| w.kind == WarningKind::DuplicateDestination);
        if !already {
            warnings.push(warn(
                WarningKind::DuplicateDestination,
                "X and Y move destinations are the same register",
            ));
        }
    }

    if let ParallelMove::RegToReg { src, dst } = pmove
        && *src == Register::Ssh
        && *dst == Register::Ssh
    {
        warnings.push(warn(
            WarningKind::SshSourceAndDest,
            "SSH is both source and destination",
        ));
    }

    // Check invalid PM4 destination registers.
    // For LImm, only warn when the invalid dest register does NOT overlap
    // the ALU dest accumulator (official classifies that as DuplicateDestination).
    match pmove {
        ParallelMove::XYMem {
            reg, write: true, ..
        }
        | ParallelMove::XYAbs {
            reg, write: true, ..
        } if is_invalid_pm4_dest(reg) => {
            warnings.push(warn(
                WarningKind::InvalidPm4Destination,
                "invalid PM4 destination register",
            ));
        }
        ParallelMove::LImm { reg, .. } => {
            let overlaps_alu = acc_is_a
                .map(|is_a| reg_overlaps_acc(reg, is_a))
                .unwrap_or(false);
            if is_invalid_pm4_dest(reg) && !overlaps_alu {
                warnings.push(warn(
                    WarningKind::InvalidPm4Destination,
                    "invalid PM4 destination register",
                ));
            }
        }
        _ => {}
    }

    check_post_update_on_dest(pmove, warnings);
}

/// The official assembler only checks DuplicateDestination for simple EA modes.
fn is_simple_ea(ea: &EffectiveAddress) -> bool {
    matches!(
        ea,
        EffectiveAddress::PostInc(_)
            | EffectiveAddress::PostDec(_)
            | EffectiveAddress::NoUpdate(_)
            | EffectiveAddress::PostIncN(_)
    )
}

fn is_vector_prohibited_reg(reg: &Register) -> bool {
    matches!(
        reg,
        Register::La | Register::Lc | Register::Sp | Register::Sr | Register::Ssh | Register::Ssl
    )
}

/// Check if an instruction is prohibited in the interrupt vector table (P:$0000-$00FF).
fn is_prohibited_in_interrupt_vector(inst: &Instruction) -> bool {
    match inst {
        Instruction::Do { .. }
        | Instruction::DoForever { .. }
        | Instruction::Dor { .. }
        | Instruction::DorForever { .. }
        | Instruction::Rep { .. } => true,

        // MOVEM: prohibited when involving a control register (either direction)
        Instruction::MovemEa { reg, .. } | Instruction::MovemAa { reg, .. } => {
            is_vector_prohibited_reg(reg)
        }

        Instruction::MovecReg { dst, w: true, .. } => is_vector_prohibited_reg(dst),
        Instruction::MovecAa { reg, w: true, .. } => is_vector_prohibited_reg(reg),
        Instruction::MovecEa { reg, w: true, .. } => is_vector_prohibited_reg(reg),
        Instruction::MovecImm { reg, .. } => is_vector_prohibited_reg(reg),

        // w=false: peripheral->register, so register is being written
        Instruction::Movep0 { reg, w: false, .. } => is_vector_prohibited_reg(reg),

        Instruction::MoveLongDisp { reg, w: true, .. } => is_vector_prohibited_reg(reg),

        Instruction::Bchg {
            target: BitTarget::Reg(reg),
            ..
        }
        | Instruction::Bclr {
            target: BitTarget::Reg(reg),
            ..
        }
        | Instruction::Bset {
            target: BitTarget::Reg(reg),
            ..
        } => is_vector_prohibited_reg(reg),

        _ => false,
    }
}

/// Extract the R register number from an EA if it uses a post-update mode.
fn ea_post_update_reg(ea: &EffectiveAddress) -> Option<u8> {
    match ea {
        EffectiveAddress::PostDec(n)
        | EffectiveAddress::PostInc(n)
        | EffectiveAddress::PostDecN(n)
        | EffectiveAddress::PostIncN(n) => Some(*n),
        _ => None,
    }
}

/// Check if any PM destination register is Rn matching the EA's post-update register.
fn check_post_update_on_dest(pmove: &ParallelMove, warnings: &mut Vec<AssembleWarning>) {
    // Collect (EA, destination registers) pairs from the parallel move
    let mut ea_dest_pairs: Vec<(&EffectiveAddress, Vec<&Register>)> = Vec::new();

    match pmove {
        ParallelMove::XYMem {
            ea,
            reg,
            write: true,
            ..
        } => {
            ea_dest_pairs.push((ea, vec![reg]));
        }
        ParallelMove::LMem {
            ea,
            reg,
            write: true,
            ..
        } => {
            ea_dest_pairs.push((ea, vec![reg]));
        }
        ParallelMove::XReg {
            ea,
            x_reg,
            d2,
            write: true,
            ..
        } => {
            ea_dest_pairs.push((ea, vec![x_reg, d2]));
        }
        ParallelMove::RegY {
            ea,
            d1,
            y_reg,
            write: true,
            ..
        } => {
            ea_dest_pairs.push((ea, vec![d1, y_reg]));
        }
        ParallelMove::XYDouble {
            x_ea,
            x_reg,
            y_ea,
            y_reg,
            x_write,
            y_write,
        } => {
            if *x_write {
                ea_dest_pairs.push((x_ea, vec![x_reg]));
            }
            if *y_write {
                ea_dest_pairs.push((y_ea, vec![y_reg]));
            }
        }
        ParallelMove::Pm0 { ea, data_reg, .. } => {
            ea_dest_pairs.push((ea, vec![data_reg]));
        }
        _ => {}
    }

    for (ea, dsts) in &ea_dest_pairs {
        if let Some(rn) = ea_post_update_reg(ea) {
            for dst in dsts {
                if let Register::R(n) = dst
                    && *n == rn
                {
                    warnings.push(warn(
                        WarningKind::PostUpdateOnDestination,
                        format!("post-update will not occur on r{rn} (also a move destination)"),
                    ));
                    return;
                }
            }
        }
    }
}

fn emit_segments(
    program: &ast::Program,
    symbols: &SymbolTable,
) -> Result<AssembleResult, AssembleError> {
    let mut segments = Vec::new();
    let mut warnings = Vec::new();
    let mut listing: Vec<Option<ListingLine>> = vec![None; program.statements.len()];
    let mut current_space = MemorySpace::P;
    let mut pc: u32 = 0;
    let mut current_words = Vec::new();
    let mut current_org: u32 = 0;

    // Psect tracking: each psect saves its (space, org, words, pc) when switched away.
    let mut psects: HashMap<String, PsectState> = HashMap::new();
    let mut psect_words: HashMap<String, (u32, Vec<u32>)> = HashMap::new(); // name -> (org, words)
    let mut current_psect: Option<String> = None;

    let flush = |segments: &mut Vec<Segment>,
                 current_words: &mut Vec<u32>,
                 current_space: MemorySpace,
                 current_org: u32| {
        if !current_words.is_empty() {
            segments.push(Segment {
                space: current_space,
                org: current_org,
                words: std::mem::take(current_words),
            });
        }
    };

    for (line_idx, stmt) in program.statements.iter().enumerate() {
        let line = line_idx + 1;
        match stmt {
            Statement::Empty | Statement::Label(_) => {}
            Statement::Directive { dir, .. } => match dir {
                Directive::Org { space, addr } => {
                    flush(
                        &mut segments,
                        &mut current_words,
                        current_space,
                        current_org,
                    );
                    current_space = *space;
                    pc = eval_at(addr, symbols, pc, line)?;
                    current_org = pc;
                    listing[line_idx] = Some(ListingLine {
                        space: current_space,
                        addr: pc,
                        words: vec![],
                    });
                }
                Directive::Dc(values) => {
                    let start_pc = pc;
                    let mut dc_words = Vec::new();
                    for val in values {
                        let v = eval_at(val, symbols, pc, line)?;
                        let w = v & 0xFFFFFF;
                        current_words.push(w);
                        dc_words.push(w);
                        pc += 1;
                    }
                    listing[line_idx] = Some(ListingLine {
                        space: current_space,
                        addr: start_pc,
                        words: dc_words,
                    });
                }
                Directive::Ds(size) => {
                    let start_pc = pc;
                    let n = eval_at(size, symbols, pc, line)?;
                    if n > 0xFFFFFF {
                        return Err(AssembleError::Encode {
                            line,
                            err: encode::EncodeError {
                                msg: "ds size out of range".into(),
                            },
                        });
                    }
                    current_words.resize(current_words.len() + n as usize, 0u32);
                    pc += n;
                    listing[line_idx] = Some(ListingLine {
                        space: current_space,
                        addr: start_pc,
                        words: vec![],
                    });
                }
                Directive::End => break,
                Directive::Psect { name, range } => {
                    if let Some(ref cur_name) = current_psect {
                        save_psect(&mut psects, &current_psect, pc);
                        psect_words.insert(
                            cur_name.clone(),
                            (current_org, std::mem::take(&mut current_words)),
                        );
                    } else {
                        flush(
                            &mut segments,
                            &mut current_words,
                            current_space,
                            current_org,
                        );
                    }

                    if let Some((space, start, _end)) = range {
                        let start_val = eval_at(start, symbols, pc, line)?;
                        psects.insert(
                            name.clone(),
                            PsectState {
                                space: *space,
                                pc: start_val,
                            },
                        );
                        current_space = *space;
                        pc = start_val;
                        current_org = start_val;
                    } else if let Some(state) = psects.get(name) {
                        current_space = state.space;
                        pc = state.pc;
                        if let Some((org, words)) = psect_words.remove(name) {
                            current_org = org;
                            current_words = words;
                        } else {
                            current_org = pc;
                        }
                    }
                    current_psect = Some(name.clone());
                }
                Directive::Align(alignment) => {
                    let n = eval_at(alignment, symbols, pc, line)?;
                    if n > 0 {
                        let aligned = pc.div_ceil(n) * n;
                        if aligned != pc {
                            flush(
                                &mut segments,
                                &mut current_words,
                                current_space,
                                current_org,
                            );
                            pc = aligned;
                            current_org = pc;
                        }
                    }
                }
                Directive::Equ { .. }
                | Directive::Section(_)
                | Directive::EndSec
                | Directive::Xref(_)
                | Directive::Xdef(_)
                | Directive::Include(_) => {}
            },
            Statement::Instruction { inst, .. } => {
                warnings.extend(check_warnings(inst).into_iter().map(|mut w| {
                    w.line = line;
                    w
                }));
                let encoded = encode::encode(inst, pc, symbols)
                    .map_err(|err| AssembleError::Encode { line, err })?;
                let inst_len = if encoded.word1.is_some() { 2u32 } else { 1 };
                // Interrupt vector table: P:$0000-$00FF (256 words, 128 two-word slots).
                // The DSP56300 family has a 256-word IVT, unlike the DSP56000's 64-word IVT.
                if current_space == MemorySpace::P && pc < 0x100 {
                    // A 2-word instruction at an odd address spans into the next vector slot.
                    if inst_len == 2 && (pc & 1) != 0 {
                        warnings.push(AssembleWarning {
                            line,
                            kind: WarningKind::InstructionInInterruptVector,
                            msg: format!(
                                "2-word instruction at P:${:04X} spans interrupt vector boundary",
                                pc
                            ),
                        });
                    }
                    if is_prohibited_in_interrupt_vector(inst) {
                        warnings.push(AssembleWarning {
                            line,
                            kind: WarningKind::InstructionInInterruptVector,
                            msg: format!(
                                "instruction prohibited in interrupt vector table (P:${:04X})",
                                pc
                            ),
                        });
                    }
                }
                let start_pc = pc;
                let mut inst_words = vec![encoded.word0];
                current_words.push(encoded.word0);
                pc += 1;
                if let Some(w1) = encoded.word1 {
                    inst_words.push(w1);
                    current_words.push(w1);
                    pc += 1;
                }
                listing[line_idx] = Some(ListingLine {
                    space: current_space,
                    addr: start_pc,
                    words: inst_words,
                });
            }
        }
    }

    flush(
        &mut segments,
        &mut current_words,
        current_space,
        current_org,
    );

    for (name, (org, words)) in psect_words {
        if !words.is_empty()
            && let Some(state) = psects.get(&name)
        {
            segments.push(Segment {
                space: state.space,
                org,
                words,
            });
        }
    }

    Ok(AssembleResult {
        segments,
        warnings,
        symbols: symbols.clone(),
        listing,
        source_lines: program.source_lines.clone(),
    })
}

/// Evaluate an expression, mapping errors to [`AssembleError::Encode`] at the given line.
fn eval_at(expr: &Expr, sym: &SymbolTable, pc: u32, line: usize) -> Result<u32, AssembleError> {
    encode::eval(expr, sym, pc).map_err(|err| AssembleError::Encode { line, err })
}

/// Assemble a single instruction line, returning the encoded words.
/// Useful for testing.
pub fn assemble_line(line: &str, pc: u32) -> Result<EncodedInstruction, AssembleError> {
    let program = parser::parse(line).map_err(AssembleError::Parse)?;
    for stmt in &program.statements {
        if let Statement::Instruction { inst, .. } = stmt {
            let symbols = SymbolTable::new();
            return encode::encode(inst, pc, &symbols)
                .map_err(|err| AssembleError::Encode { line: 1, err });
        }
    }
    Err(AssembleError::Parse(parser::ParseError {
        line: 1,
        msg: "no instruction found".to_string(),
    }))
}
