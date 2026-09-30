//! AST types for DSP56300 assembly.

/// Origin information for a source line (file and line number).
#[derive(Debug, Clone)]
pub struct SourceOrigin {
    /// Source file path (empty string for stdin/top-level).
    pub file: String,
    /// 1-based line number in the source file.
    pub line: usize,
    /// Original source text.
    pub text: String,
}

/// A complete assembly program.
pub struct Program {
    pub statements: Vec<Statement>,
    /// Per-statement source origin info (populated during parsing, maintained through include expansion).
    pub source_lines: Vec<SourceOrigin>,
}

/// A single line of assembly.
pub enum Statement {
    /// Label definition (e.g., `loop:`)
    Label(String),
    /// Instruction with optional label prefix.
    Instruction {
        label: Option<String>,
        inst: Instruction,
    },
    /// Assembler directive with optional label prefix.
    Directive {
        label: Option<String>,
        dir: Directive,
    },
    /// Empty line.
    Empty,
}

/// Assembler directives.
pub enum Directive {
    Org {
        space: MemorySpace,
        addr: Expr,
    },
    Dc(Vec<Expr>),
    Ds(Expr),
    Equ {
        name: String,
        value: Expr,
    },
    End,
    Section(String),
    EndSec,
    Xref(Vec<String>),
    Xdef(Vec<String>),
    Include(String),
    /// `psect name space:start:end` (define) or `psect name` (switch).
    Psect {
        name: String,
        range: Option<(MemorySpace, Expr, Expr)>,
    },
    /// `align n` - advance PC to next multiple of n.
    Align(Expr),
}

/// Memory space specifier.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemorySpace {
    X,
    Y,
    P,
    L,
}

/// Binary operator.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Shl,
    Shr,
    BitAnd,
    BitOr,
}

/// Unary operator.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp {
    Neg,
    BitNot,
}

/// Expression (for immediates, addresses, EQU values).
#[derive(Debug, Clone)]
pub enum Expr {
    Literal(i64),
    /// Fractional literal (Q23 value).  Distinguished from `Literal` so the
    /// encoder can use PM3 MSB-alignment when the low 16 bits are zero.
    Frac(i64),
    Symbol(String),
    CurrentPc,
    BinOp {
        op: BinOp,
        lhs: Box<Expr>,
        rhs: Box<Expr>,
    },
    UnaryOp {
        op: UnaryOp,
        operand: Box<Expr>,
    },
}

impl Expr {
    /// Try to evaluate a constant expression (no symbol references).
    /// Returns None if the expression contains symbols or CurrentPc.
    pub fn try_eval_const(&self) -> Option<i64> {
        match self {
            Expr::Literal(v) | Expr::Frac(v) => Some(*v),
            Expr::BinOp { op, lhs, rhs } => {
                let l = lhs.try_eval_const()?;
                let r = rhs.try_eval_const()?;
                Some(match op {
                    BinOp::Add => l.wrapping_add(r),
                    BinOp::Sub => l.wrapping_sub(r),
                    BinOp::Mul => l.wrapping_mul(r),
                    BinOp::Div => {
                        if r == 0 {
                            return None;
                        }
                        l / r
                    }
                    BinOp::Shl => l << (r & 63),
                    BinOp::Shr => l >> (r & 63),
                    BinOp::BitAnd => l & r,
                    BinOp::BitOr => l | r,
                })
            }
            Expr::UnaryOp { op, operand } => {
                let v = operand.try_eval_const()?;
                Some(match op {
                    UnaryOp::Neg => v.wrapping_neg(),
                    UnaryOp::BitNot => !v,
                })
            }
            Expr::Symbol(_) | Expr::CurrentPc => None,
        }
    }

    /// Evaluate a constant expression as u32. Returns None for symbols/compound.
    pub fn as_u32(&self) -> Option<u32> {
        self.try_eval_const().map(|v| v as u32)
    }

    /// Returns true if the expression can be evaluated without a symbol table.
    pub fn is_literal(&self) -> bool {
        self.try_eval_const().is_some()
    }
}

/// Register operand.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Register {
    X0,
    X1,
    Y0,
    Y1,
    A0,
    A1,
    A2,
    B0,
    B1,
    B2,
    A,
    B,
    R(u8),
    N(u8),
    M(u8),
    Ep,
    Vba,
    Sc,
    Sz,
    Sr,
    Omr,
    Sp,
    Ssh,
    Ssl,
    La,
    Lc,
    Mr,
    Ccr,
    // Composite/special registers for parallel moves
    A10,
    B10,
    RegX,
    RegY,
    Ab,
    Ba,
}

impl Register {
    /// Convert to the 6-bit register index used in opcodes (matching `dsp56300_core::reg`).
    pub fn index(&self) -> usize {
        use dsp56300_core::reg;
        match self {
            Register::X0 => reg::X0,
            Register::X1 => reg::X1,
            Register::Y0 => reg::Y0,
            Register::Y1 => reg::Y1,
            Register::A0 => reg::A0,
            Register::A1 => reg::A1,
            Register::A2 => reg::A2,
            Register::B0 => reg::B0,
            Register::B1 => reg::B1,
            Register::B2 => reg::B2,
            Register::A => reg::A,
            Register::B => reg::B,
            Register::R(n) => reg::R0 + *n as usize,
            Register::N(n) => reg::N0 + *n as usize,
            Register::M(n) => reg::M0 + *n as usize,
            Register::Ep => reg::EP,
            Register::Vba => reg::VBA,
            Register::Sc => reg::SC,
            Register::Sz => reg::SZ,
            Register::Sr => reg::SR,
            Register::Omr => reg::OMR,
            Register::Sp => reg::SP,
            Register::Ssh => reg::SSH,
            Register::Ssl => reg::SSL,
            Register::La => reg::LA,
            Register::Lc => reg::LC,
            Register::Mr | Register::Ccr => reg::SR, // MR/CCR are sub-fields of SR
            Register::A10
            | Register::B10
            | Register::RegX
            | Register::RegY
            | Register::Ab
            | Register::Ba => 0, // L-move composites, handled specially
        }
    }

    /// Returns true if this is a data ALU register (24-bit or wider).
    /// PM3 short immediate is MSB-justified for these registers.
    pub fn is_data_alu(&self) -> bool {
        matches!(
            self,
            Register::X0
                | Register::X1
                | Register::Y0
                | Register::Y1
                | Register::A0
                | Register::A1
                | Register::A2
                | Register::B0
                | Register::B1
                | Register::B2
                | Register::A
                | Register::B
        )
    }
}

/// Accumulator selector.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Acc {
    A = 0,
    B = 1,
}

/// Condition code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CondCode {
    Cc = 0,
    Ge = 1,
    Ne = 2,
    Pl = 3,
    Nn = 4,
    Ec = 5,
    Lc = 6,
    Gt = 7,
    Cs = 8,
    Lt = 9,
    Eq = 10,
    Mi = 11,
    Nr = 12,
    Es = 13,
    Ls = 14,
    Le = 15,
}

/// Effective address mode.
#[derive(Debug, Clone)]
pub enum EffectiveAddress {
    /// `(rN)-nN` -- post-decrement by N register
    PostDecN(u8),
    /// `(rN)+nN` -- post-increment by N register
    PostIncN(u8),
    /// `(rN)-` -- post-decrement
    PostDec(u8),
    /// `(rN)+` -- post-increment
    PostInc(u8),
    /// `(rN)` -- no update
    NoUpdate(u8),
    /// `(rN+nN)` -- indexed, no update
    IndexedN(u8),
    /// `$xxxx` -- absolute address (extension word)
    AbsAddr(Expr),
    /// `>$xxxx` -- force-long absolute address (always uses extension word,
    /// bypasses short-form optimizations in encoder)
    ForceLongAbsAddr(Expr),
    /// `#$xxxx` -- immediate data (extension word)
    Immediate(Expr),
    /// `-(rN)` -- pre-decrement
    PreDec(u8),
}

/// Target for bit manipulation instructions.
#[derive(Debug, Clone)]
pub enum BitTarget {
    /// `x:$ffffc5` or `x:$0023` -- memory address (pp/aa classification deferred to encoder)
    Addr { space: MemorySpace, addr: Expr },
    /// `x:(r0)+` -- effective address
    Ea {
        space: MemorySpace,
        ea: EffectiveAddress,
    },
    /// `sr` -- register
    Reg(Register),
}

/// Source for loop count (DO/DOR).
#[derive(Debug, Clone)]
pub enum LoopSource {
    Imm(Expr),
    Reg(Register),
    Aa {
        space: MemorySpace,
        addr: Expr,
    },
    Ea {
        space: MemorySpace,
        ea: EffectiveAddress,
    },
}

/// Source for REP count.
#[derive(Debug, Clone)]
pub enum RepSource {
    Imm(Expr),
    Reg(Register),
    Aa {
        space: MemorySpace,
        addr: Expr,
    },
    Ea {
        space: MemorySpace,
        ea: EffectiveAddress,
    },
}

/// A fully parsed instruction.
#[derive(Debug, Clone)]
pub enum Instruction {
    // --- Zero-operand ---
    Nop,
    Rts,
    Rti,
    Reset,
    Stop,
    Wait,
    EndDo,
    Illegal,

    // --- Single accumulator ---
    Inc(Acc),
    Dec(Acc),

    // --- Arithmetic with immediate ---
    AddImm {
        imm: Expr,
        d: Acc,
    },
    AddLong {
        imm: Expr,
        d: Acc,
    },
    SubImm {
        imm: Expr,
        d: Acc,
    },
    SubLong {
        imm: Expr,
        d: Acc,
    },
    CmpImm {
        imm: Expr,
        d: Acc,
    },
    CmpLong {
        imm: Expr,
        d: Acc,
    },
    AndImm {
        imm: Expr,
        d: Acc,
    },
    AndLong {
        imm: Expr,
        d: Acc,
    },
    OrLong {
        imm: Expr,
        d: Acc,
    },
    OrImm {
        imm: Expr,
        d: Acc,
    },
    EorImm {
        imm: Expr,
        d: Acc,
    },
    EorLong {
        imm: Expr,
        d: Acc,
    },
    AndI {
        imm: Expr,
        dest: u8,
    },
    OrI {
        imm: Expr,
        dest: u8,
    },

    // --- Shifts with immediate ---
    AslImm {
        shift: Expr,
        src: Acc,
        dst: Acc,
    },
    AsrImm {
        shift: Expr,
        src: Acc,
        dst: Acc,
    },
    LslImm {
        shift: Expr,
        dst: Acc,
    },
    LsrImm {
        shift: Expr,
        dst: Acc,
    },
    AslReg {
        shift_reg: Register,
        src: Acc,
        dst: Acc,
    },
    AsrReg {
        shift_reg: Register,
        src: Acc,
        dst: Acc,
    },
    LslReg {
        shift_reg: Register,
        dst: Acc,
    },
    LsrReg {
        shift_reg: Register,
        dst: Acc,
    },

    // --- Branches ---
    Bcc {
        cc: CondCode,
        target: Expr,
        force_long: bool,
    },
    BccRn {
        cc: CondCode,
        rn: u8,
    },
    Bra {
        target: Expr,
        force_long: bool,
    },
    BraRn {
        rn: u8,
    },
    Bsr {
        target: Expr,
        force_long: bool,
    },
    BsrRn {
        rn: u8,
    },
    Bscc {
        cc: CondCode,
        target: Expr,
        force_long: bool,
    },
    BsccRn {
        cc: CondCode,
        rn: u8,
    },
    Brkcc {
        cc: CondCode,
    },
    Jcc {
        cc: CondCode,
        target: Expr,
    },
    JccEa {
        cc: CondCode,
        ea: EffectiveAddress,
    },
    Jmp {
        target: Expr,
        force_short: bool,
    },
    JmpEa {
        ea: EffectiveAddress,
    },
    Jscc {
        cc: CondCode,
        target: Expr,
    },
    JsccEa {
        cc: CondCode,
        ea: EffectiveAddress,
    },
    Jsr {
        target: Expr,
        force_short: bool,
    },
    JsrEa {
        ea: EffectiveAddress,
    },

    // --- Bit manipulation ---
    Bchg {
        bit: Expr,
        target: BitTarget,
    },
    Bclr {
        bit: Expr,
        target: BitTarget,
    },
    Bset {
        bit: Expr,
        target: BitTarget,
    },
    Btst {
        bit: Expr,
        target: BitTarget,
    },

    // --- Bit branch ---
    Jclr {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Jset {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Jsclr {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Jsset {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Brclr {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Brset {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Bsclr {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },
    Bsset {
        bit: Expr,
        target: BitTarget,
        addr: Expr,
    },

    // --- Loop ---
    Do {
        source: LoopSource,
        end_addr: Expr,
    },
    DoForever {
        end_addr: Expr,
    },
    Dor {
        source: LoopSource,
        end_addr: Expr,
    },
    DorForever {
        end_addr: Expr,
    },
    Rep {
        source: RepSource,
    },

    // --- Move (non-parallel) ---
    MovecReg {
        src: Register,
        dst: Register,
        w: bool,
    },
    MovecAa {
        space: MemorySpace,
        addr: Expr,
        reg: Register,
        w: bool,
    },
    MovecEa {
        space: MemorySpace,
        ea: EffectiveAddress,
        reg: Register,
        w: bool,
    },
    MovecImm {
        imm: Expr,
        reg: Register,
    },
    MovemEa {
        ea: EffectiveAddress,
        reg: Register,
        w: bool,
    },
    MovemAa {
        addr: Expr,
        reg: Register,
        w: bool,
    },
    Movep23 {
        periph_space: MemorySpace,
        periph_addr: Expr,
        ea_space: MemorySpace,
        ea: EffectiveAddress,
        w: bool,
    },
    Movep23Imm {
        periph_space: MemorySpace,
        periph_addr: Expr,
        imm: Expr,
    },
    MovepXQq {
        periph_addr: Expr,
        ea_space: MemorySpace,
        ea: EffectiveAddress,
        w: bool,
    },
    MovepXQqImm {
        periph_addr: Expr,
        imm: Expr,
    },
    Movep1 {
        periph_space: MemorySpace,
        periph_addr: Expr,
        ea: EffectiveAddress,
        w: bool,
    },
    Movep0 {
        periph_space: MemorySpace,
        periph_addr: Expr,
        reg: Register,
        w: bool,
    },
    MoveLongDisp {
        space: MemorySpace,
        offset_reg: u8,
        offset: Expr,
        reg: Register,
        w: bool,
    },
    MoveShortDisp {
        space: MemorySpace,
        offset_reg: u8,
        offset: Expr,
        reg: Register,
        w: bool,
    },

    // --- Multiply ---
    /// mpy/mpyr/mac/macr (+/-)S,#n,D
    MulShift {
        mnem: MulShiftMnem,
        sign: Sign,
        src: Register,
        shift: Expr,
        dst: Acc,
    },
    MpyI {
        sign: Sign,
        imm: Expr,
        src: Register,
        dst: Acc,
    },
    MpyrI {
        sign: Sign,
        imm: Expr,
        src: Register,
        dst: Acc,
    },
    MacI {
        sign: Sign,
        imm: Expr,
        src: Register,
        dst: Acc,
    },
    MacrI {
        sign: Sign,
        imm: Expr,
        src: Register,
        dst: Acc,
    },
    Dmac {
        ss: u8,
        sign: Sign,
        s1: Register,
        s2: Register,
        dst: Acc,
    },
    MacSU {
        su: bool,
        sign: Sign,
        s1: Register,
        s2: Register,
        dst: Acc,
    },
    MpySU {
        su: bool,
        sign: Sign,
        s1: Register,
        s2: Register,
        dst: Acc,
    },
    Div {
        src: Register,
        dst: Acc,
    },
    CmpU {
        src: Register,
        dst: Acc,
    },
    Norm {
        src: u8,
        dst: Acc,
    },

    // --- Address ---
    Lua {
        ea: EffectiveAddress,
        dst: Register,
    },
    LuaRel {
        base: u8,
        offset: Expr,
        dst_is_n: bool,
        dst: u8,
    },
    LraRn {
        src: u8,
        dst: Register,
    },
    LraDisp {
        target: Expr,
        dst: Register,
    },

    // --- Tier 3: Specialized ---
    Clb {
        s: Acc,
        d: Acc,
    },
    Normf {
        src: Register,
        d: Acc,
    },
    Merge {
        src: Register,
        d: Acc,
    },
    ExtractReg {
        s1: Register,
        s2: Acc,
        d: Acc,
    },
    ExtractImm {
        co: Expr,
        s2: Acc,
        d: Acc,
    },
    ExtractuReg {
        s1: Register,
        s2: Acc,
        d: Acc,
    },
    ExtractuImm {
        co: Expr,
        s2: Acc,
        d: Acc,
    },
    InsertReg {
        s1: Register,
        s2: Register,
        d: Acc,
    },
    InsertImm {
        co: Expr,
        s2: Register,
        d: Acc,
    },
    Vsl {
        s: Acc,
        i_bit: u8,
        ea: EffectiveAddress,
    },
    Debug,
    Debugcc {
        cc: CondCode,
    },
    Trap,
    Trapcc {
        cc: CondCode,
    },
    Pflush,
    Pflushun,
    Pfree,
    PlockEa {
        ea: EffectiveAddress,
    },
    Plockr {
        target: Expr,
    },
    PunlockEa {
        ea: EffectiveAddress,
    },
    Punlockr {
        target: Expr,
    },

    // --- Transfer conditional ---
    Tcc {
        cc: CondCode,
        acc: Option<(Register, Register)>,
        r: Option<(u8, u8)>,
    },

    // --- Parallel ALU + move ---
    Parallel {
        alu: ParallelAlu,
        pmove: ParallelMove,
    },
}

/// Sign for mpy/mac operands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sign {
    Plus,
    Minus,
}

/// Mnemonic for MulShift (S,#n,D) instructions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MulShiftMnem {
    Mpy,
    Mpyr,
    Mac,
    Macr,
}

/// Re-export core's structured ParallelAlu enum.
pub use dsp56300_core::ParallelAlu;

/// Parallel data move portion.
#[derive(Debug, Clone)]
pub enum ParallelMove {
    /// No parallel move (Pm0 with no data move, or Pm2 NOP).
    None,
    /// Register to register: `a,b` (Pm2).
    RegToReg { src: Register, dst: Register },
    /// EA update: `(r0)+,r0` (Pm2).
    EaUpdate { ea: EffectiveAddress, dst: u8 },
    /// Immediate to register: `#$xx,r0` (Pm2/Pm3).
    ImmToReg { imm: Expr, dst: Register },
    /// X or Y memory read/write: `x:(r0)+,x0` or `a,x:(r0)+` (Pm4).
    XYMem {
        space: MemorySpace,
        ea: EffectiveAddress,
        reg: Register,
        write: bool,
    },
    /// X or Y memory with absolute address: `x:$xxxx,a` or `a,x:$xxxx` (Pm4).
    XYAbs {
        space: MemorySpace,
        addr: Expr,
        reg: Register,
        write: bool,
        /// True when `<` prefix was used (force short aa addressing, 1 word).
        force_short: bool,
    },
    /// L memory move (Pm4 with L: prefix).
    LMem {
        ea: EffectiveAddress,
        reg: Register,
        write: bool,
    },
    /// L memory with absolute address.
    LAbs {
        addr: Expr,
        reg: Register,
        write: bool,
        /// True when `<` prefix was used (force short aa addressing, 1 word).
        force_short: bool,
    },
    /// L memory immediate.
    LImm { imm: Expr, reg: Register },
    /// Class I X move + register: `x:(r0)+,x0 a,y0` (Pm1 X space).
    XReg {
        ea: EffectiveAddress,
        x_reg: Register,
        s2: Register,
        d2: Register,
        write: bool,
    },
    /// Class I X immediate + register: `#$xx,x0 a,y0` (Pm1 X space).
    XImmReg {
        imm: Expr,
        x_reg: Register,
        s2: Register,
        d2: Register,
    },
    /// Class II register + Y move: `a,x0 y:(r0)+,y0` (Pm1 Y space).
    RegY {
        s1: Register,
        d1: Register,
        ea: EffectiveAddress,
        y_reg: Register,
        write: bool,
    },
    /// Class II register + Y immediate: `a,x0 #$xx,y0` (Pm1 Y space).
    RegYImm {
        s1: Register,
        d1: Register,
        imm: Expr,
        y_reg: Register,
    },
    /// Dual X:Y move (Pm8): `x:(r0)+,x0 y:(r4)+,y0`.
    XYDouble {
        x_ea: EffectiveAddress,
        x_reg: Register,
        y_ea: EffectiveAddress,
        y_reg: Register,
        x_write: bool,
        y_write: bool,
    },
    /// Pm0 form: `a,x:(r0)+ x0,a`
    Pm0 {
        acc: Register,
        space: MemorySpace,
        ea: EffectiveAddress,
        data_reg: Register,
    },
    /// IFcc: conditional execution (CCR unchanged).
    Ifcc { cc: CondCode },
    /// IFcc.U: conditional execution (CCR updated when condition true).
    IfccU { cc: CondCode },
}
