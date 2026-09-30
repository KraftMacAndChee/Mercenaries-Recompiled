//! Chumsky-based parser for DSP56300 assembly.

use crate::ast::*;
use crate::token::Token;
use chumsky::input::{InputRef, Stream};
use chumsky::prelude::*;
use logos::Logos;

/// Parse error with line number.
#[derive(Debug)]
pub struct ParseError {
    pub line: usize,
    pub msg: String,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "line {}: {}", self.line, self.msg)
    }
}

impl std::error::Error for ParseError {}

/// Input type for chumsky parsers: a stream of tokens.
type PI = Stream<std::vec::IntoIter<Token>>;
/// Extra type for chumsky parsers: Rich error reporting.
type E<'a> = extra::Err<Rich<'a, Token>>;

/// Parse a complete assembly source into a program.
pub fn parse(source: &str) -> Result<Program, ParseError> {
    let tokens: Vec<Token> = Token::lexer(source)
        .filter_map(|r| r.ok())
        .filter(|t| !matches!(t, Token::Comment))
        .collect();

    let stream = Stream::from_iter(tokens.clone());

    match program_parser().parse(stream).into_result() {
        Ok(mut prog) => {
            let lines: Vec<&str> = source.lines().collect();
            prog.source_lines = (0..prog.statements.len())
                .map(|i| crate::ast::SourceOrigin {
                    file: String::new(),
                    line: i + 1,
                    text: lines.get(i).unwrap_or(&"").to_string(),
                })
                .collect();
            Ok(prog)
        }
        Err(errs) => {
            let err = &errs[0];
            let start = err.span().start;
            let line = tokens[..start.min(tokens.len())]
                .iter()
                .filter(|t| matches!(t, Token::Newline | Token::NewlineCrLf))
                .count()
                + 1;
            Err(ParseError {
                line,
                msg: err.to_string(),
            })
        }
    }
}

/// Parse a register name string.
pub fn parse_register(name: &str) -> Option<Register> {
    let name_lc = name.to_ascii_lowercase();
    let name = name_lc.as_str();
    match name {
        "x0" => Some(Register::X0),
        "x1" => Some(Register::X1),
        "y0" => Some(Register::Y0),
        "y1" => Some(Register::Y1),
        "a0" => Some(Register::A0),
        "a1" => Some(Register::A1),
        "a2" => Some(Register::A2),
        "b0" => Some(Register::B0),
        "b1" => Some(Register::B1),
        "b2" => Some(Register::B2),
        "a" => Some(Register::A),
        "b" => Some(Register::B),
        "r0" => Some(Register::R(0)),
        "r1" => Some(Register::R(1)),
        "r2" => Some(Register::R(2)),
        "r3" => Some(Register::R(3)),
        "r4" => Some(Register::R(4)),
        "r5" => Some(Register::R(5)),
        "r6" => Some(Register::R(6)),
        "r7" => Some(Register::R(7)),
        "n0" => Some(Register::N(0)),
        "n1" => Some(Register::N(1)),
        "n2" => Some(Register::N(2)),
        "n3" => Some(Register::N(3)),
        "n4" => Some(Register::N(4)),
        "n5" => Some(Register::N(5)),
        "n6" => Some(Register::N(6)),
        "n7" => Some(Register::N(7)),
        "m0" => Some(Register::M(0)),
        "m1" => Some(Register::M(1)),
        "m2" => Some(Register::M(2)),
        "m3" => Some(Register::M(3)),
        "m4" => Some(Register::M(4)),
        "m5" => Some(Register::M(5)),
        "m6" => Some(Register::M(6)),
        "m7" => Some(Register::M(7)),
        "ep" => Some(Register::Ep),
        "vba" => Some(Register::Vba),
        "sc" => Some(Register::Sc),
        "sz" => Some(Register::Sz),
        "sr" => Some(Register::Sr),
        "omr" => Some(Register::Omr),
        "sp" => Some(Register::Sp),
        "ssh" => Some(Register::Ssh),
        "ssl" => Some(Register::Ssl),
        "la" => Some(Register::La),
        "lc" => Some(Register::Lc),
        "mr" => Some(Register::Mr),
        "ccr" => Some(Register::Ccr),
        "a10" => Some(Register::A10),
        "b10" => Some(Register::B10),
        "x" => Some(Register::RegX),
        "y" => Some(Register::RegY),
        "ab" => Some(Register::Ab),
        "ba" => Some(Register::Ba),
        _ => None,
    }
}

/// Parse a condition code name.
pub fn parse_cc(name: &str) -> Option<CondCode> {
    use dsp56300_core::CC_NAMES;
    let name_lc = name.to_ascii_lowercase();
    CC_NAMES
        .iter()
        .position(|&cc| cc == name_lc)
        .map(|i| match i {
            0 => CondCode::Cc,
            1 => CondCode::Ge,
            2 => CondCode::Ne,
            3 => CondCode::Pl,
            4 => CondCode::Nn,
            5 => CondCode::Ec,
            6 => CondCode::Lc,
            7 => CondCode::Gt,
            8 => CondCode::Cs,
            9 => CondCode::Lt,
            10 => CondCode::Eq,
            11 => CondCode::Mi,
            12 => CondCode::Nr,
            13 => CondCode::Es,
            14 => CondCode::Ls,
            15 => CondCode::Le,
            _ => unreachable!(),
        })
}

// ---- Expression parser (imperative precedence-climbing) ----

/// Precedence levels for binary operators (higher binds tighter).
fn binop_prec(op: BinOp) -> u8 {
    match op {
        BinOp::BitOr => 1,
        BinOp::BitAnd => 2,
        BinOp::Shl | BinOp::Shr => 3,
        BinOp::Add | BinOp::Sub => 4,
        BinOp::Mul | BinOp::Div => 5,
    }
}

/// Try to identify a binary operator at the current position (without consuming).
fn try_binop<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<BinOp> {
    let save = inp.save();
    let result = match inp.next() {
        Some(Token::Pipe) => Some(BinOp::BitOr),
        Some(Token::Ampersand) => Some(BinOp::BitAnd),
        Some(Token::Lt) => {
            if matches!(inp.next(), Some(Token::Lt)) {
                Some(BinOp::Shl)
            } else {
                None
            }
        }
        Some(Token::Gt) => {
            if matches!(inp.next(), Some(Token::Gt)) {
                Some(BinOp::Shr)
            } else {
                None
            }
        }
        Some(Token::Plus) => Some(BinOp::Add),
        Some(Token::Minus) => Some(BinOp::Sub),
        Some(Token::Star) => Some(BinOp::Mul),
        Some(Token::Slash) => Some(BinOp::Div),
        _ => None,
    };
    inp.rewind(save);
    result
}

/// Consume a binary operator (already identified by try_binop).
fn eat_binop<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>, op: BinOp) {
    inp.next();
    if matches!(op, BinOp::Shl | BinOp::Shr) {
        inp.next();
    }
}

/// Parse an expression atom.
fn parse_expr_atom<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Expr, Rich<'a, Token>> {
    let save = inp.save();
    match inp.next() {
        Some(Token::Hex(v)) => Ok(Expr::Literal(v as i64)),
        Some(Token::Dec(v)) => Ok(Expr::Literal(v as i64)),
        Some(Token::Frac(v)) => Ok(Expr::Frac(v)),
        Some(Token::Star) => Ok(Expr::CurrentPc),
        Some(Token::Ident(s)) => {
            if parse_register(&s).is_some() {
                // Don't consume registers as expression symbols -- they belong
                // to operand parsing. Rewind and fail so the caller can try
                // register parsing instead.
                inp.rewind(save);
                Err(err(inp, "expected expression"))
            } else {
                Ok(Expr::Symbol(s))
            }
        }
        Some(Token::LParen) => {
            let expr = parse_expr_prec(inp, 0)?;
            expect(inp, &Token::RParen)?;
            Ok(expr)
        }
        _ => {
            inp.rewind(save);
            Err(err(inp, "expected expression"))
        }
    }
}

/// Parse a unary expression (-, ~, or atom).
fn parse_expr_unary<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Expr, Rich<'a, Token>> {
    let save = inp.save();
    match inp.next() {
        Some(Token::Minus) => {
            let operand = parse_expr_unary(inp)?;
            if let Expr::Literal(v) = operand {
                Ok(Expr::Literal(-v))
            } else if let Expr::Frac(v) = operand {
                Ok(Expr::Frac(-v))
            } else {
                Ok(Expr::UnaryOp {
                    op: UnaryOp::Neg,
                    operand: Box::new(operand),
                })
            }
        }
        Some(Token::Tilde) => {
            let operand = parse_expr_unary(inp)?;
            Ok(Expr::UnaryOp {
                op: UnaryOp::BitNot,
                operand: Box::new(operand),
            })
        }
        _ => {
            inp.rewind(save);
            parse_expr_atom(inp)
        }
    }
}

/// Parse an expression with precedence climbing.
fn parse_expr_prec<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    min_prec: u8,
) -> Result<Expr, Rich<'a, Token>> {
    let mut lhs = parse_expr_unary(inp)?;
    while let Some(op) = try_binop(inp) {
        let prec = binop_prec(op);
        if prec < min_prec {
            break;
        }
        eat_binop(inp, op);
        let rhs = parse_expr_prec(inp, prec + 1)?;
        lhs = Expr::BinOp {
            op,
            lhs: Box::new(lhs),
            rhs: Box::new(rhs),
        };
    }
    Ok(lhs)
}

// ---- Leaf parsers (combinator style) ----

fn reg_p<'a>() -> impl Parser<'a, PI, Register, E<'a>> + Clone {
    select! { Token::Ident(s) => s }.try_map(|s, span| {
        parse_register(&s).ok_or_else(|| Rich::custom(span, "expected register"))
    })
}

fn acc_p<'a>() -> impl Parser<'a, PI, Acc, E<'a>> + Clone {
    select! {
        Token::Ident(s) if s.eq_ignore_ascii_case("a") => Acc::A,
        Token::Ident(s) if s.eq_ignore_ascii_case("b") => Acc::B,
    }
}

fn rn_p<'a>() -> impl Parser<'a, PI, u8, E<'a>> + Clone {
    select! { Token::Ident(s) => s }.try_map(|s, span| {
        s.strip_prefix('r')
            .and_then(|n| n.parse::<u8>().ok())
            .filter(|&n| n < 8)
            .ok_or_else(|| Rich::custom(span, "expected rN register"))
    })
}

fn nn_from_str(name: &str) -> Option<u8> {
    name.strip_prefix('n')
        .and_then(|s| s.parse::<u8>().ok())
        .filter(|&n| n < 8)
}

// ---- EA parser (imperative style -- complex branching) ----

fn parse_ea<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<EffectiveAddress, Rich<'a, Token>> {
    // Pre-decrement: -(rN)
    let save = inp.save();
    if matches!(inp.next(), Some(Token::Minus)) && matches!(inp.next(), Some(Token::LParen)) {
        let n = inp.parse(rn_p())?;
        expect(inp, &Token::RParen)?;
        return Ok(EffectiveAddress::PreDec(n));
    }
    inp.rewind(save);

    // All other modes start with '('
    expect(inp, &Token::LParen)?;
    let n = inp.parse(rn_p())?;

    let save = inp.save();
    match inp.next() {
        Some(Token::Plus) => {
            // (rN+nN) -- indexed
            let save2 = inp.save();
            if let Some(Token::Ident(name)) = inp.next()
                && let Some(nn) = nn_from_str(&name)
            {
                if nn != n {
                    return Err(Rich::custom(
                        inp.span_since(save.cursor()),
                        "nN index must match rN base register in EA",
                    ));
                }
                expect(inp, &Token::RParen)?;
                return Ok(EffectiveAddress::IndexedN(nn));
            }
            inp.rewind(save2);
            Err(Rich::custom(
                inp.span_since(save.cursor()),
                "expected nN or ')' after '+'",
            ))
        }
        Some(Token::RParen) => {
            // (rN) -- check for post-modify
            let save2 = inp.save();
            match inp.next() {
                Some(Token::Plus) => {
                    // (rN)+nN or (rN)+
                    let save3 = inp.save();
                    if let Some(Token::Ident(name)) = inp.next()
                        && let Some(nn) = nn_from_str(&name)
                    {
                        if nn != n {
                            return Err(Rich::custom(
                                inp.span_since(save.cursor()),
                                "nN index must match rN base register in EA",
                            ));
                        }
                        return Ok(EffectiveAddress::PostIncN(nn));
                    }
                    inp.rewind(save3);
                    Ok(EffectiveAddress::PostInc(n))
                }
                Some(Token::Minus) => {
                    // (rN)-nN or (rN)-
                    let save3 = inp.save();
                    if let Some(Token::Ident(name)) = inp.next()
                        && let Some(nn) = nn_from_str(&name)
                    {
                        if nn != n {
                            return Err(Rich::custom(
                                inp.span_since(save.cursor()),
                                "nN index must match rN base register in EA",
                            ));
                        }
                        return Ok(EffectiveAddress::PostDecN(nn));
                    }
                    inp.rewind(save3);
                    Ok(EffectiveAddress::PostDec(n))
                }
                _ => {
                    inp.rewind(save2);
                    Ok(EffectiveAddress::NoUpdate(n))
                }
            }
        }
        _ => {
            let span = inp.span_since(save.cursor());
            inp.rewind(save);
            Err(Rich::custom(span, "expected '+', '-', or ')' in EA"))
        }
    }
}

fn expect<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>, tok: &Token) -> Result<(), Rich<'a, Token>> {
    let save = inp.save();
    match inp.next() {
        Some(ref t) if t == tok => Ok(()),
        _ => {
            let span = inp.span_since(save.cursor());
            inp.rewind(save);
            Err(Rich::custom(span, format!("expected '{}'", tok)))
        }
    }
}

fn at_eol<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
    let save = inp.save();
    let is_eol = matches!(inp.next(), None | Some(Token::Newline | Token::NewlineCrLf));
    inp.rewind(save);
    is_eol
}

fn parse_reg<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Register, Rich<'a, Token>> {
    inp.parse(reg_p())
}

fn parse_acc_op<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Acc, Rich<'a, Token>> {
    inp.parse(acc_p())
}

fn parse_expr_op<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Expr, Rich<'a, Token>> {
    parse_expr_prec(inp, 0)
}

/// Parse `#expr` or `#>expr` or `#<expr`, returning (expr, force_long).
fn parse_imm_op_with_force<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<(Expr, bool), Rich<'a, Token>> {
    expect(inp, &Token::Hash)?;
    let save = inp.save();
    let force_long = match inp.next() {
        Some(Token::Gt) => true,
        Some(Token::Lt) => false,
        _ => {
            inp.rewind(save);
            false
        }
    };
    // Character literal: #'X' -> ASCII value
    let save = inp.save();
    if let Some(Token::StringLit(s)) = inp.next()
        && s.len() == 1
    {
        return Ok((Expr::Literal(s.bytes().next().unwrap() as i64), force_long));
    }
    inp.rewind(save);
    Ok((parse_expr_op(inp)?, force_long))
}

fn parse_imm_op<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Expr, Rich<'a, Token>> {
    parse_imm_op_with_force(inp).map(|(expr, _)| expr)
}

fn expect_comma<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<(), Rich<'a, Token>> {
    expect(inp, &Token::Comma)
}

fn parse_memspace<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<MemorySpace> {
    parse_memspace_ex(inp).map(|(s, _)| s)
}

/// Like `parse_memspace`, but also returns whether a `<` force-short modifier was present.
fn parse_memspace_ex<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<(MemorySpace, bool)> {
    let save = inp.save();
    let space = match inp.next() {
        Some(Token::XMem) => MemorySpace::X,
        Some(Token::YMem) => MemorySpace::Y,
        Some(Token::PMem) => MemorySpace::P,
        Some(Token::LMem) => MemorySpace::L,
        _ => {
            inp.rewind(save);
            return None;
        }
    };
    // Check for `<` (force-short) or `<<` (force-IO-short) address modifier.
    let save2 = inp.save();
    let force_short = if matches!(inp.next(), Some(Token::Lt)) {
        let save3 = inp.save();
        if !matches!(inp.next(), Some(Token::Lt)) {
            inp.rewind(save3);
        }
        true
    } else {
        inp.rewind(save2);
        false
    };
    Some((space, force_short))
}

fn eat_force_short<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
    let save = inp.save();
    if matches!(inp.next(), Some(Token::Lt)) {
        true
    } else {
        inp.rewind(save);
        false
    }
}

/// Extract the register number from an effective address, if applicable.
fn ea_reg_num(ea: &EffectiveAddress) -> Option<u8> {
    match ea {
        EffectiveAddress::PostDecN(n)
        | EffectiveAddress::PostIncN(n)
        | EffectiveAddress::PostDec(n)
        | EffectiveAddress::PostInc(n)
        | EffectiveAddress::NoUpdate(n)
        | EffectiveAddress::IndexedN(n)
        | EffectiveAddress::PreDec(n) => Some(*n),
        EffectiveAddress::AbsAddr(_)
        | EffectiveAddress::ForceLongAbsAddr(_)
        | EffectiveAddress::Immediate(_) => None,
    }
}

fn at_ea_start<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
    let save = inp.save();
    let is_ea = matches!(inp.next(), Some(Token::LParen | Token::Minus));
    inp.rewind(save);
    is_ea
}

/// Parse an effective address that may be register-based ea OR an absolute address.
fn parse_ea_or_abs<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<EffectiveAddress, Rich<'a, Token>> {
    if at_ea_start(inp) {
        parse_ea(inp)
    } else if check_force_long_addr(inp) {
        Ok(EffectiveAddress::ForceLongAbsAddr(parse_expr_op(inp)?))
    } else {
        Ok(EffectiveAddress::AbsAddr(parse_expr_op(inp)?))
    }
}

/// Parse an EA or fall back to absolute address (no force-long support).
fn parse_ea_or_abs_simple<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<EffectiveAddress, Rich<'a, Token>> {
    if at_ea_start(inp) {
        parse_ea(inp)
    } else {
        Ok(EffectiveAddress::AbsAddr(parse_expr_op(inp)?))
    }
}

fn check_force_long_addr<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
    let save = inp.save();
    if matches!(inp.next(), Some(Token::Gt)) {
        true
    } else {
        inp.rewind(save);
        false
    }
}

fn is_hash<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
    let save = inp.save();
    let yes = matches!(inp.next(), Some(Token::Hash));
    inp.rewind(save);
    yes
}

fn peek<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<Token> {
    let save = inp.save();
    let tok = inp.next();
    inp.rewind(save);
    tok
}

/// Try to parse an R0-R7 register. Returns Some(n) if the next token is "rN",
/// consuming it; otherwise returns None without consuming anything.
fn try_parse_rn<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<u8> {
    let save = inp.save();
    if let Some(Token::Ident(s)) = inp.next()
        && let Some(Register::R(n)) = parse_register(&s)
    {
        return Some(n);
    }
    inp.rewind(save);
    None
}

/// Try to parse an sss-class register (a1/b1/x0/y0/x1/y1) followed by a comma.
/// Returns Some(register) if found, None otherwise (rewinds on failure).
fn try_parse_sss_reg<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<Register> {
    let save = inp.save();
    if let Some(Token::Ident(s)) = inp.next() {
        let reg = parse_register(&s);
        if matches!(
            reg,
            Some(
                Register::A1
                    | Register::B1
                    | Register::X0
                    | Register::Y0
                    | Register::X1
                    | Register::Y1
            )
        ) {
            // Check for comma after register to confirm this is a reg shift form
            if matches!(inp.next(), Some(Token::Comma)) {
                return reg;
            }
        }
    }
    inp.rewind(save);
    None
}

fn skip_pmem<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) {
    let save = inp.save();
    if !matches!(inp.next(), Some(Token::PMem)) {
        inp.rewind(save);
    }
}

fn err<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>, msg: &str) -> Rich<'a, Token> {
    let save = inp.save();
    Rich::custom(inp.span_since(save.cursor()), msg.to_string())
}

// ---- Bit target parser ----

fn parse_bit_target<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<BitTarget, Rich<'a, Token>> {
    if let Some(space) = parse_memspace(inp) {
        match space {
            MemorySpace::X | MemorySpace::Y => {}
            _ => return Err(err(inp, "expected x: or y: for bit target")),
        }
        if at_ea_start(inp) {
            let ea = parse_ea(inp)?;
            return Ok(BitTarget::Ea { space, ea });
        }
        // Check for immediate EA: y:#$xxxx
        {
            let save = inp.save();
            if matches!(inp.next(), Some(Token::Hash)) {
                let imm = parse_expr_op(inp)?;
                return Ok(BitTarget::Ea {
                    space,
                    ea: EffectiveAddress::Immediate(imm),
                });
            }
            inp.rewind(save);
        }
        let force_long = check_force_long_addr(inp);
        let addr = parse_expr_op(inp)?;
        if force_long {
            Ok(BitTarget::Ea {
                space,
                ea: EffectiveAddress::AbsAddr(addr),
            })
        } else {
            Ok(BitTarget::Addr { space, addr })
        }
    } else {
        let reg = parse_reg(inp)?;
        Ok(BitTarget::Reg(reg))
    }
}

// ---- Loop/rep source ----

fn parse_count_source<'a, T>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    make_imm: impl Fn(Expr) -> T,
    make_ea: impl Fn(MemorySpace, EffectiveAddress) -> T,
    make_aa: impl Fn(MemorySpace, Expr) -> T,
    make_reg: impl Fn(Register) -> T,
) -> Result<T, Rich<'a, Token>> {
    if is_hash(inp) {
        return Ok(make_imm(parse_imm_op(inp)?));
    }
    if let Some(space) = parse_memspace(inp) {
        match space {
            MemorySpace::X | MemorySpace::Y => {}
            _ => return Err(err(inp, "expected x: or y:")),
        }
        if at_ea_start(inp) {
            return Ok(make_ea(space, parse_ea(inp)?));
        }
        let force_long = check_force_long_addr(inp);
        let addr = parse_expr_op(inp)?;
        if force_long {
            return Ok(make_ea(space, EffectiveAddress::AbsAddr(addr)));
        }
        return Ok(make_aa(space, addr));
    }
    Ok(make_reg(parse_reg(inp)?))
}

fn parse_loop_source<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<LoopSource, Rich<'a, Token>> {
    parse_count_source(
        inp,
        LoopSource::Imm,
        |space, ea| LoopSource::Ea { space, ea },
        |space, addr| LoopSource::Aa { space, addr },
        LoopSource::Reg,
    )
}

fn parse_rep_source<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<RepSource, Rich<'a, Token>> {
    parse_count_source(
        inp,
        RepSource::Imm,
        |space, ea| RepSource::Ea { space, ea },
        |space, addr| RepSource::Aa { space, addr },
        RepSource::Reg,
    )
}

// ---- Instruction helpers ----

fn parse_imm_alu_or_parallel<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnem: &str,
    make_short: fn(Expr, Acc) -> Instruction,
    make_long: fn(Expr, Acc) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    if is_hash(inp) {
        let (imm, force_long) = parse_imm_op_with_force(inp)?;
        expect_comma(inp)?;
        let d = parse_acc_op(inp)?;
        let short = !force_long && imm.as_u32().is_some_and(|v| v <= 0x3F);
        Ok(if short {
            make_short(imm, d)
        } else {
            make_long(imm, d)
        })
    } else {
        parse_parallel_from_alu(inp, mnem)
    }
}

fn parse_lsl_lsr<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnem: &str,
    make_imm: fn(Expr, Acc) -> Instruction,
    make_reg: fn(Register, Acc) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    if is_hash(inp) {
        let shift = parse_imm_op(inp)?;
        expect_comma(inp)?;
        let dst = parse_acc_op(inp)?;
        Ok(make_imm(shift, dst))
    } else if let Some(reg) = try_parse_sss_reg(inp) {
        let dst = parse_acc_op(inp)?;
        Ok(make_reg(reg, dst))
    } else {
        parse_parallel_from_alu(inp, mnem)
    }
}

fn parse_bra_bsr<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    make_rn: fn(u8) -> Instruction,
    make_label: fn(Expr, bool) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    skip_pmem(inp);
    eat_force_short(inp);
    let force_long = check_force_long_addr(inp);
    if let Some(rn) = try_parse_rn(inp) {
        Ok(make_rn(rn))
    } else {
        Ok(make_label(parse_expr_op(inp)?, force_long))
    }
}

fn parse_bcc<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    cc: CondCode,
    make_rn: fn(CondCode, u8) -> Instruction,
    make_label: fn(CondCode, Expr, bool) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    skip_pmem(inp);
    eat_force_short(inp);
    let force_long = check_force_long_addr(inp);
    if let Some(rn) = try_parse_rn(inp) {
        Ok(make_rn(cc, rn))
    } else {
        Ok(make_label(cc, parse_expr_op(inp)?, force_long))
    }
}

fn parse_jcc<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    cc: CondCode,
    make: fn(CondCode, EffectiveAddress) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    skip_pmem(inp);
    eat_force_short(inp);
    let force_long = check_force_long_addr(inp);
    if at_ea_start(inp) {
        return Ok(make(cc, parse_ea(inp)?));
    }
    let target = parse_expr_op(inp)?;
    let ea = if force_long {
        EffectiveAddress::ForceLongAbsAddr(target)
    } else {
        EffectiveAddress::AbsAddr(target)
    };
    Ok(make(cc, ea))
}

fn parse_shift<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnem: &str,
    make_imm: fn(Expr, Acc, Acc) -> Instruction,
    make_reg: fn(Register, Acc, Acc) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    if is_hash(inp) {
        let shift = parse_imm_op(inp)?;
        expect_comma(inp)?;
        let src = parse_acc_op(inp)?;
        expect_comma(inp)?;
        let dst = parse_acc_op(inp)?;
        Ok(make_imm(shift, src, dst))
    } else if let Some(reg) = try_parse_sss_reg(inp) {
        // Register form: sss_reg,S2,D (comma already consumed by try_parse_sss_reg)
        let src = parse_acc_op(inp)?;
        expect_comma(inp)?;
        let dst = parse_acc_op(inp)?;
        Ok(make_reg(reg, src, dst))
    } else {
        parse_parallel_from_alu(inp, mnem)
    }
}

fn parse_andi_ori<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    is_and: bool,
) -> Result<Instruction, Rich<'a, Token>> {
    let imm = parse_imm_op(inp)?;
    expect_comma(inp)?;
    let save = inp.save();
    let dest = match inp.next() {
        Some(Token::Ident(s)) => match s.as_str() {
            "mr" => 0u8,
            "ccr" => 1,
            "omr" | "com" => 2,
            "eom" => 3,
            _ => {
                inp.rewind(save);
                return Err(err(inp, "expected mr, ccr, omr/com, or eom"));
            }
        },
        _ => {
            inp.rewind(save);
            return Err(err(inp, "expected mr, ccr, omr/com, or eom"));
        }
    };
    if is_and {
        Ok(Instruction::AndI { imm, dest })
    } else {
        Ok(Instruction::OrI { imm, dest })
    }
}

fn parse_bit_op<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    make: fn(Expr, BitTarget) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    let bit = parse_imm_op(inp)?;
    expect_comma(inp)?;
    let target = parse_bit_target(inp)?;
    Ok(make(bit, target))
}

fn parse_bit_branch<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    make: fn(Expr, BitTarget, Expr) -> Instruction,
) -> Result<Instruction, Rich<'a, Token>> {
    let bit = parse_imm_op(inp)?;
    expect_comma(inp)?;
    let target = parse_bit_target(inp)?;
    expect_comma(inp)?;
    skip_pmem(inp);
    let addr = parse_expr_op(inp)?;
    Ok(make(bit, target, addr))
}

fn parse_jmp_or_jsr<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    make_target: impl Fn(Expr, bool) -> Instruction,
    make_ea: impl Fn(EffectiveAddress) -> Instruction,
    _name: &str,
) -> Result<Instruction, Rich<'a, Token>> {
    skip_pmem(inp);
    let force_short = eat_force_short(inp);
    let force_long = check_force_long_addr(inp);
    if at_ea_start(inp) {
        return Ok(make_ea(parse_ea(inp)?));
    }
    if force_long {
        return Ok(make_ea(EffectiveAddress::ForceLongAbsAddr(parse_expr_op(
            inp,
        )?)));
    }
    Ok(make_target(parse_expr_op(inp)?, force_short))
}

fn parse_jmp<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    parse_jmp_or_jsr(
        inp,
        |target, force_short| Instruction::Jmp {
            target,
            force_short,
        },
        |ea| Instruction::JmpEa { ea },
        "jmp",
    )
}

fn parse_jsr<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    parse_jmp_or_jsr(
        inp,
        |target, force_short| Instruction::Jsr {
            target,
            force_short,
        },
        |ea| Instruction::JsrEa { ea },
        "jsr",
    )
}

fn parse_conditional<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnem: &str,
) -> Result<Instruction, Rich<'a, Token>> {
    // debug<cc>
    if let Some(cc_str) = mnem.strip_prefix("debug")
        && let Some(cc) = parse_cc(cc_str)
    {
        return Ok(Instruction::Debugcc { cc });
    }
    // trap<cc>
    if let Some(cc_str) = mnem.strip_prefix("trap")
        && let Some(cc) = parse_cc(cc_str)
    {
        return Ok(Instruction::Trapcc { cc });
    }
    // brk<cc>
    if let Some(cc_str) = mnem.strip_prefix("brk")
        && let Some(cc) = parse_cc(cc_str)
    {
        return Ok(Instruction::Brkcc { cc });
    }
    // bs<cc>
    if let Some(cc_str) = mnem.strip_prefix("bs")
        && let Some(cc) = parse_cc(cc_str)
    {
        return parse_bcc(
            inp,
            cc,
            |cc, rn| Instruction::BsccRn { cc, rn },
            |cc, target, force_long| Instruction::Bscc {
                cc,
                target,
                force_long,
            },
        );
    }
    // b<cc>
    if let Some(cc_str) = mnem.strip_prefix('b')
        && let Some(cc) = parse_cc(cc_str)
    {
        return parse_bcc(
            inp,
            cc,
            |cc, rn| Instruction::BccRn { cc, rn },
            |cc, target, force_long| Instruction::Bcc {
                cc,
                target,
                force_long,
            },
        );
    }
    // js<cc>
    if let Some(cc_str) = mnem.strip_prefix("js")
        && let Some(cc) = parse_cc(cc_str)
    {
        return parse_jcc(inp, cc, |cc, ea| Instruction::JsccEa { cc, ea });
    }
    // j<cc>
    if let Some(cc_str) = mnem.strip_prefix('j')
        && let Some(cc) = parse_cc(cc_str)
    {
        return parse_jcc(inp, cc, |cc, ea| Instruction::JccEa { cc, ea });
    }
    // t<cc>
    if let Some(cc_str) = mnem.strip_prefix('t')
        && let Some(cc) = parse_cc(cc_str)
    {
        return parse_tcc(inp, cc);
    }
    // mpy/mac/mpyr/macr: try S,#n,D form, else parallel
    let mul_shift_mnem = match mnem {
        "mpy" => Some(MulShiftMnem::Mpy),
        "mpyr" => Some(MulShiftMnem::Mpyr),
        "mac" => Some(MulShiftMnem::Mac),
        "macr" => Some(MulShiftMnem::Macr),
        _ => None,
    };
    if let Some(msm) = mul_shift_mnem
        && let Some(inst) = try_parse_mul_shift(inp, msm)?
    {
        return Ok(inst);
    }
    parse_parallel_from_alu(inp, mnem)
}

fn parse_tcc<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    cc: CondCode,
) -> Result<Instruction, Rich<'a, Token>> {
    let first_src = parse_reg(inp)?;
    expect_comma(inp)?;
    let first_dst = parse_reg(inp)?;

    // If first pair is R registers and no second pair -> template 3 (R-only)
    if let (Register::R(src_n), Register::R(dst_n)) = (&first_src, &first_dst)
        && at_eol(inp)
    {
        return Ok(Instruction::Tcc {
            cc,
            acc: None,
            r: Some((*src_n, *dst_n)),
        });
    }

    // Template 1 or 2: first pair is accumulator/data registers
    let r = if !at_eol(inp) {
        let src2 = parse_reg(inp)?;
        expect_comma(inp)?;
        let dst2 = parse_reg(inp)?;
        let src_n = match src2 {
            Register::R(n) => n,
            _ => return Err(err(inp, "tcc second pair must be R registers")),
        };
        let dst_n = match dst2 {
            Register::R(n) => n,
            _ => return Err(err(inp, "tcc second pair must be R registers")),
        };
        Some((src_n, dst_n))
    } else {
        None
    };
    Ok(Instruction::Tcc {
        cc,
        acc: Some((first_src, first_dst)),
        r,
    })
}

// ---- Move instructions ----

fn parse_movec<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    if is_hash(inp) {
        let (imm, force_long) = parse_imm_op_with_force(inp)?;
        expect_comma(inp)?;
        let reg = parse_reg(inp)?;
        let short = !force_long && imm.as_u32().is_some_and(|v| v <= 0xFF);
        return if short {
            Ok(Instruction::MovecImm { imm, reg })
        } else {
            Ok(Instruction::MovecEa {
                space: MemorySpace::X,
                ea: EffectiveAddress::Immediate(imm),
                reg,
                w: true,
            })
        };
    }

    if matches!(peek(inp), Some(Token::XMem | Token::YMem)) {
        let space = parse_memspace(inp).unwrap();
        if at_ea_start(inp) {
            let ea = parse_ea(inp)?;
            expect_comma(inp)?;
            return Ok(Instruction::MovecEa {
                space,
                ea,
                reg: parse_reg(inp)?,
                w: true,
            });
        }
        let force_long = check_force_long_addr(inp);
        let addr = parse_expr_op(inp)?;
        expect_comma(inp)?;
        return if force_long {
            Ok(Instruction::MovecEa {
                space,
                ea: EffectiveAddress::AbsAddr(addr),
                reg: parse_reg(inp)?,
                w: true,
            })
        } else {
            Ok(Instruction::MovecAa {
                space,
                addr,
                reg: parse_reg(inp)?,
                w: true,
            })
        };
    }

    let reg1 = parse_reg(inp)?;
    expect_comma(inp)?;

    if matches!(peek(inp), Some(Token::XMem | Token::YMem)) {
        let space = parse_memspace(inp).unwrap();
        if at_ea_start(inp) {
            let ea = parse_ea(inp)?;
            return Ok(Instruction::MovecEa {
                space,
                ea,
                reg: reg1,
                w: false,
            });
        }
        let force_long = check_force_long_addr(inp);
        let addr = parse_expr_op(inp)?;
        return if force_long {
            Ok(Instruction::MovecEa {
                space,
                ea: EffectiveAddress::AbsAddr(addr),
                reg: reg1,
                w: false,
            })
        } else {
            Ok(Instruction::MovecAa {
                space,
                addr,
                reg: reg1,
                w: false,
            })
        };
    }

    let reg2 = parse_reg(inp)?;
    Ok(Instruction::MovecReg {
        src: reg1,
        dst: reg2,
        w: true,
    })
}

fn parse_movem<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    let save = inp.save();
    if matches!(inp.next(), Some(Token::PMem)) {
        if at_ea_start(inp) {
            let ea = parse_ea(inp)?;
            expect_comma(inp)?;
            return Ok(Instruction::MovemEa {
                ea,
                reg: parse_reg(inp)?,
                w: true,
            });
        }
        let force_long = check_force_long_addr(inp);
        let addr = parse_expr_op(inp)?;
        expect_comma(inp)?;
        return if force_long {
            Ok(Instruction::MovemEa {
                ea: EffectiveAddress::AbsAddr(addr),
                reg: parse_reg(inp)?,
                w: true,
            })
        } else {
            Ok(Instruction::MovemAa {
                addr,
                reg: parse_reg(inp)?,
                w: true,
            })
        };
    }
    inp.rewind(save);

    let reg = parse_reg(inp)?;
    expect_comma(inp)?;
    skip_pmem(inp);
    if at_ea_start(inp) {
        return Ok(Instruction::MovemEa {
            ea: parse_ea(inp)?,
            reg,
            w: false,
        });
    }
    let force_long = check_force_long_addr(inp);
    let addr = parse_expr_op(inp)?;
    if force_long {
        Ok(Instruction::MovemEa {
            ea: EffectiveAddress::AbsAddr(addr),
            reg,
            w: false,
        })
    } else {
        Ok(Instruction::MovemAa {
            addr,
            reg,
            w: false,
        })
    }
}

fn parse_movep<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    // Immediate source: movep #imm,space:addr
    if is_hash(inp) {
        let imm = parse_imm_op(inp)?;
        expect_comma(inp)?;
        let space = parse_memspace(inp).ok_or_else(|| err(inp, "expected memory space"))?;
        let periph_addr = parse_expr_op(inp)?;
        // Classification (Movep23Imm vs MovepXQqImm) deferred to encoder
        return Ok(Instruction::Movep23Imm {
            periph_space: space,
            periph_addr,
            imm,
        });
    }

    if matches!(peek(inp), Some(Token::XMem | Token::YMem | Token::PMem)) {
        let space1 = parse_memspace(inp).unwrap();

        if space1 == MemorySpace::P {
            let ea = parse_ea_or_abs(inp)?;
            expect_comma(inp)?;
            let space2 = parse_memspace(inp).ok_or_else(|| err(inp, "expected memory space"))?;
            let periph_addr = parse_expr_op(inp)?;
            return Ok(Instruction::Movep1 {
                periph_space: space2,
                periph_addr,
                ea,
                w: true,
            });
        }

        // X: or Y: prefix with register-based EA: ea is first operand, periph is second
        if at_ea_start(inp) {
            let ea = parse_ea(inp)?;
            expect_comma(inp)?;
            let space2 = parse_memspace(inp).ok_or_else(|| err(inp, "expected memory space"))?;
            let periph_addr = parse_expr_op(inp)?;
            return Ok(Instruction::Movep23 {
                periph_space: space2,
                periph_addr,
                ea_space: space1,
                ea,
                w: true,
            });
        }

        // Absolute address: could be either the peripheral or the ea.
        // Parse the first address and determine role after seeing the second operand.
        let addr1 = parse_expr_op(inp)?;
        expect_comma(inp)?;

        let save = inp.save();
        if matches!(inp.next(), Some(Token::PMem)) {
            let ea = parse_ea_or_abs(inp)?;
            return Ok(Instruction::Movep1 {
                periph_space: space1,
                periph_addr: addr1,
                ea,
                w: false,
            });
        }
        inp.rewind(save);

        if matches!(peek(inp), Some(Token::XMem | Token::YMem)) {
            let space2 = parse_memspace(inp).unwrap();
            let ea_or_periph = if at_ea_start(inp) {
                parse_ea(inp)?
            } else {
                EffectiveAddress::AbsAddr(parse_expr_op(inp)?)
            };
            // Classification: addr1 is peripheral, ea_or_periph is ea (W=false)
            // OR: addr1 is ea, ea_or_periph contains the peripheral (W=true).
            // Deferred to encoder which checks address ranges.
            return Ok(Instruction::Movep23 {
                periph_space: space1,
                periph_addr: addr1,
                ea_space: space2,
                ea: ea_or_periph,
                w: false,
            });
        }

        let reg = parse_reg(inp)?;
        return Ok(Instruction::Movep0 {
            periph_space: space1,
            periph_addr: addr1,
            reg,
            w: false,
        });
    }

    // Register source: movep reg,space:addr
    let reg = parse_reg(inp)?;
    expect_comma(inp)?;
    let space = parse_memspace(inp).ok_or_else(|| err(inp, "expected memory space"))?;
    let periph_addr = parse_expr_op(inp)?;
    Ok(Instruction::Movep0 {
        periph_space: space,
        periph_addr,
        reg,
        w: true,
    })
}

fn parse_move_or_parallel<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<Instruction, Rich<'a, Token>> {
    let save = inp.save();
    match parse_move(inp) {
        Ok(inst) => Ok(inst),
        Err(_) => {
            inp.rewind(save);
            parse_parallel_from_alu(inp, "move")
        }
    }
}

fn parse_move<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    if matches!(peek(inp), Some(Token::XMem | Token::YMem)) {
        return parse_move_xy(inp);
    }
    if let Some(Token::Ident(name)) = peek(inp)
        && parse_register(&name).is_some()
    {
        return parse_move_reg_first(inp);
    }
    if at_eol(inp) {
        return Ok(Instruction::Parallel {
            alu: ParallelAlu::Move,
            pmove: ParallelMove::None,
        });
    }
    Err(err(inp, "unexpected operand for move"))
}

fn parse_signed_literal_offset<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<Expr, Rich<'a, Token>> {
    let sign = {
        let save = inp.save();
        match inp.next() {
            Some(Token::Plus) => 1i64,
            Some(Token::Minus) => -1i64,
            _ => {
                inp.rewind(save);
                return Err(err(inp, "expected + or - in offset expression"));
            }
        }
    };
    match parse_expr_op(inp)? {
        Expr::Literal(v) => Ok(Expr::Literal(sign * v)),
        _ => Err(err(inp, "offset must be literal")),
    }
}

fn make_move_xy_instr(
    space: MemorySpace,
    rn: u8,
    offset: Expr,
    reg: Register,
    w: bool,
    force_long: bool,
) -> Instruction {
    let off_i = match &offset {
        Expr::Literal(v) => *v as i32,
        _ => i32::MAX,
    };
    if !force_long && (-64..=63).contains(&off_i) && reg.index() < 16 {
        Instruction::MoveShortDisp {
            space,
            offset_reg: rn,
            offset,
            reg,
            w,
        }
    } else {
        Instruction::MoveLongDisp {
            space,
            offset_reg: rn,
            offset,
            reg,
            w,
        }
    }
}

fn parse_move_xy<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<Instruction, Rich<'a, Token>> {
    let space = parse_memspace(inp).unwrap();
    let force_long = check_force_long_addr(inp);
    expect(inp, &Token::LParen)?;
    let rn = inp.parse(rn_p())?;
    let offset = parse_signed_literal_offset(inp)?;
    expect(inp, &Token::RParen)?;
    expect_comma(inp)?;
    let reg = parse_reg(inp)?;
    Ok(make_move_xy_instr(space, rn, offset, reg, true, force_long))
}

fn parse_move_reg_first<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<Instruction, Rich<'a, Token>> {
    let reg = parse_reg(inp)?;
    expect_comma(inp)?;

    if matches!(peek(inp), Some(Token::XMem | Token::YMem)) {
        let space = parse_memspace(inp).unwrap();
        let force_long = check_force_long_addr(inp);
        expect(inp, &Token::LParen)?;
        let rn = inp.parse(rn_p())?;
        let offset = parse_signed_literal_offset(inp)?;
        expect(inp, &Token::RParen)?;
        return Ok(make_move_xy_instr(
            space, rn, offset, reg, false, force_long,
        ));
    }

    Err(err(inp, "unexpected target for move"))
}

/// Try to parse `mpy/mac/mpyr/macr (+/-)S,#n,D` (non-parallel shift form).
/// Returns None if the second operand is not `#n`, leaving the input at its
/// original position so the parallel fallback can try.
fn try_parse_mul_shift<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnem: MulShiftMnem,
) -> Result<Option<Instruction>, Rich<'a, Token>> {
    let save = inp.save();
    let sign = parse_sign(inp);
    let Ok(src) = parse_reg(inp) else {
        inp.rewind(save);
        return Ok(None);
    };
    if !matches!(inp.next(), Some(Token::Comma)) {
        inp.rewind(save);
        return Ok(None);
    }
    // Check if next is '#' -- distinguishes S,#n,D from parallel form
    if !is_hash(inp) {
        inp.rewind(save);
        return Ok(None);
    }
    // It's the shift form: parse #n,D
    let shift = parse_imm_op(inp)?;
    expect_comma(inp)?;
    let dst = parse_acc_op(inp)?;
    Ok(Some(Instruction::MulShift {
        mnem,
        sign,
        src,
        shift,
        dst,
    }))
}

fn parse_mpyi<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    let sign = {
        let save = inp.save();
        match inp.next() {
            Some(Token::Plus) => Sign::Plus,
            Some(Token::Minus) => Sign::Minus,
            _ => {
                inp.rewind(save);
                Sign::Plus
            }
        }
    };
    let imm = parse_imm_op(inp)?;
    expect_comma(inp)?;
    let src = parse_reg(inp)?;
    expect_comma(inp)?;
    Ok(Instruction::MpyI {
        sign,
        imm,
        src,
        dst: parse_acc_op(inp)?,
    })
}

fn parse_sign<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Sign {
    let save = inp.save();
    match inp.next() {
        Some(Token::Plus) => Sign::Plus,
        Some(Token::Minus) => Sign::Minus,
        _ => {
            inp.rewind(save);
            Sign::Plus
        }
    }
}

enum ImmMulKind {
    MpyrI,
    MacI,
    MacrI,
}

fn parse_imm_mul<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    kind: ImmMulKind,
) -> Result<Instruction, Rich<'a, Token>> {
    let sign = parse_sign(inp);
    let imm = parse_imm_op(inp)?;
    expect_comma(inp)?;
    let src = parse_reg(inp)?;
    expect_comma(inp)?;
    let dst = parse_acc_op(inp)?;
    Ok(match kind {
        ImmMulKind::MpyrI => Instruction::MpyrI {
            sign,
            imm,
            src,
            dst,
        },
        ImmMulKind::MacI => Instruction::MacI {
            sign,
            imm,
            src,
            dst,
        },
        ImmMulKind::MacrI => Instruction::MacrI {
            sign,
            imm,
            src,
            dst,
        },
    })
}

fn parse_dmac<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    ss: u8,
) -> Result<Instruction, Rich<'a, Token>> {
    let sign = parse_sign(inp);
    let s1 = parse_reg(inp)?;
    expect_comma(inp)?;
    let s2 = parse_reg(inp)?;
    expect_comma(inp)?;
    let dst = parse_acc_op(inp)?;
    Ok(Instruction::Dmac {
        ss,
        sign,
        s1,
        s2,
        dst,
    })
}

fn parse_mac_mpy_su<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    is_mac: bool,
    su: bool,
) -> Result<Instruction, Rich<'a, Token>> {
    let sign = parse_sign(inp);
    let s1 = parse_reg(inp)?;
    expect_comma(inp)?;
    let s2 = parse_reg(inp)?;
    expect_comma(inp)?;
    let dst = parse_acc_op(inp)?;
    if is_mac {
        Ok(Instruction::MacSU {
            su,
            sign,
            s1,
            s2,
            dst,
        })
    } else {
        Ok(Instruction::MpySU {
            su,
            sign,
            s1,
            s2,
            dst,
        })
    }
}

fn parse_post_modify<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    base: u8,
    with_n: EffectiveAddress,
    without_n: EffectiveAddress,
) -> Result<EffectiveAddress, Rich<'a, Token>> {
    let save = inp.save();
    if let Some(Token::Ident(name)) = inp.next()
        && let Some(nn) = nn_from_str(&name)
    {
        if nn != base {
            return Err(Rich::custom(
                inp.span_since(save.cursor()),
                "nN index must match rN base register in EA",
            ));
        }
        Ok(with_n)
    } else {
        inp.rewind(save);
        Ok(without_n)
    }
}

fn parse_lua<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    expect(inp, &Token::LParen)?;
    let base = inp.parse(rn_p())?;

    let save = inp.save();
    match inp.next() {
        Some(Token::Plus) => {
            // (rN+nN) or (rN + offset)
            let save2 = inp.save();
            if let Some(Token::Ident(name)) = inp.next()
                && let Some(nn) = nn_from_str(&name)
            {
                if nn != base {
                    return Err(Rich::custom(
                        inp.span_since(save.cursor()),
                        "nN index must match rN base register in EA",
                    ));
                }
                expect(inp, &Token::RParen)?;
                expect_comma(inp)?;
                let dst = parse_reg(inp)?;
                return Ok(Instruction::Lua {
                    ea: EffectiveAddress::IndexedN(base),
                    dst,
                });
            }
            inp.rewind(save2);
            let offset = parse_expr_op(inp)?;
            expect(inp, &Token::RParen)?;
            expect_comma(inp)?;
            parse_lua_rel_dest(inp, base, offset)
        }
        Some(Token::Minus) => {
            let offset_inner = parse_expr_op(inp)?;
            let offset = match offset_inner {
                Expr::Literal(v) => Expr::Literal(-v),
                _ => return Err(err(inp, "offset must be literal")),
            };
            expect(inp, &Token::RParen)?;
            expect_comma(inp)?;
            parse_lua_rel_dest(inp, base, offset)
        }
        Some(Token::RParen) => {
            // (rN) followed by post-modify
            let ea = {
                let save2 = inp.save();
                match inp.next() {
                    Some(Token::Plus) => parse_post_modify(
                        inp,
                        base,
                        EffectiveAddress::PostIncN(base),
                        EffectiveAddress::PostInc(base),
                    )?,
                    Some(Token::Minus) => parse_post_modify(
                        inp,
                        base,
                        EffectiveAddress::PostDecN(base),
                        EffectiveAddress::PostDec(base),
                    )?,
                    _ => {
                        inp.rewind(save2);
                        EffectiveAddress::NoUpdate(base)
                    }
                }
            };
            expect_comma(inp)?;
            let dst = parse_reg(inp)?;
            Ok(Instruction::Lua { ea, dst })
        }
        _ => {
            inp.rewind(save);
            Err(err(inp, "expected ')' or '+' or '-'"))
        }
    }
}

fn parse_lua_rel_dest<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    base: u8,
    offset: Expr,
) -> Result<Instruction, Rich<'a, Token>> {
    let save = inp.save();
    if let Some(Token::Ident(name)) = inp.next() {
        if let Some(n) = name.strip_prefix('r').and_then(|s| s.parse::<u8>().ok()) {
            return Ok(Instruction::LuaRel {
                base,
                offset,
                dst_is_n: false,
                dst: n,
            });
        }
        if let Some(n) = name.strip_prefix('n').and_then(|s| s.parse::<u8>().ok()) {
            return Ok(Instruction::LuaRel {
                base,
                offset,
                dst_is_n: true,
                dst: n,
            });
        }
    }
    inp.rewind(save);
    Err(err(inp, "expected rN or nN destination"))
}

fn parse_lra<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<Instruction, Rich<'a, Token>> {
    // LRA Rn,D or LRA xxxx,D
    if let Some(rn) = try_parse_rn(inp) {
        expect_comma(inp)?;
        let dst = parse_reg(inp)?;
        Ok(Instruction::LraRn { src: rn, dst })
    } else {
        let target = parse_expr_op(inp)?;
        expect_comma(inp)?;
        let dst = parse_reg(inp)?;
        Ok(Instruction::LraDisp { target, dst })
    }
}

/// Parse EXTRACT / EXTRACTU / INSERT instructions.
/// `is_insert`: true for INSERT, false for EXTRACT/EXTRACTU.
/// `unsigned`: true for EXTRACTU (ignored for INSERT).
fn parse_extract_or_insert<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    is_insert: bool,
    unsigned: bool,
) -> Result<Instruction, Rich<'a, Token>> {
    // First operand: either #CO (immediate) or register S1
    let save = inp.save();
    let is_imm = matches!(inp.next(), Some(Token::Hash));
    if !is_imm {
        inp.rewind(save);
    }

    if is_imm {
        let co = parse_expr_op(inp)?;
        expect_comma(inp)?;
        if is_insert {
            let s2 = parse_reg(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::InsertImm { co, s2, d })
        } else if unsigned {
            let s2 = parse_acc_op(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::ExtractuImm { co, s2, d })
        } else {
            let s2 = parse_acc_op(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::ExtractImm { co, s2, d })
        }
    } else {
        let s1 = parse_reg(inp)?;
        expect_comma(inp)?;
        if is_insert {
            let s2 = parse_reg(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::InsertReg { s1, s2, d })
        } else if unsigned {
            let s2 = parse_acc_op(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::ExtractuReg { s1, s2, d })
        } else {
            let s2 = parse_acc_op(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::ExtractReg { s1, s2, d })
        }
    }
}

// ---- Parallel instruction parsing ----

fn parse_parallel_from_alu<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnemonic: &str,
) -> Result<Instruction, Rich<'a, Token>> {
    use ParallelAlu::*;
    use dsp56300_core::{Accumulator, reg};

    let save_pos = inp.save();

    /// Try to consume an accumulator token (a/b), returning `Accumulator` and the `reg` index.
    fn try_acc<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<Accumulator> {
        let save = inp.save();
        match inp.next() {
            Some(Token::Ident(s)) if s.eq_ignore_ascii_case("a") => Some(Accumulator::A),
            Some(Token::Ident(s)) if s.eq_ignore_ascii_case("b") => Some(Accumulator::B),
            _ => {
                inp.rewind(save);
                None
            }
        }
    }

    /// Try to consume an ALU source: accumulator ("a"/"b"), XY pair ("x"/"y"), or
    /// single register ("x0"/"y0"/"x1"/"y1"). Returns `(kind, index)`.
    #[derive(Debug)]
    enum AluSrc {
        Acc(Accumulator),
        XYPair { hi: usize, lo: usize },
        Reg(usize),
    }

    fn try_alu_src<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<AluSrc> {
        let save = inp.save();
        match inp.next() {
            Some(Token::Ident(s)) => match s.to_ascii_lowercase().as_str() {
                "a" => Some(AluSrc::Acc(Accumulator::A)),
                "b" => Some(AluSrc::Acc(Accumulator::B)),
                "x" => Some(AluSrc::XYPair {
                    hi: reg::X1,
                    lo: reg::X0,
                }),
                "y" => Some(AluSrc::XYPair {
                    hi: reg::Y1,
                    lo: reg::Y0,
                }),
                "x0" => Some(AluSrc::Reg(reg::X0)),
                "x1" => Some(AluSrc::Reg(reg::X1)),
                "y0" => Some(AluSrc::Reg(reg::Y0)),
                "y1" => Some(AluSrc::Reg(reg::Y1)),
                _ => {
                    inp.rewind(save);
                    None
                }
            },
            _ => {
                inp.rewind(save);
                None
            }
        }
    }

    fn try_comma<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
        let save = inp.save();
        if matches!(inp.next(), Some(Token::Comma)) {
            true
        } else {
            inp.rewind(save);
            false
        }
    }

    /// Try to consume a JJ register (x0/y0/x1/y1).
    fn try_jj_reg<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Option<usize> {
        let save = inp.save();
        match inp.next() {
            Some(Token::Ident(s)) => match s.as_str() {
                "x0" => Some(reg::X0),
                "x1" => Some(reg::X1),
                "y0" => Some(reg::Y0),
                "y1" => Some(reg::Y1),
                _ => {
                    inp.rewind(save);
                    None
                }
            },
            _ => {
                inp.rewind(save);
                None
            }
        }
    }

    let alu = match mnemonic {
        // Zero operands
        "move" => Move,

        // Unary: mnemonic acc
        "tst" | "rnd" | "clr" | "not" | "asr" | "lsr" | "abs" | "ror" | "asl" | "lsl" | "neg"
        | "rol" => {
            let d = try_acc(inp).ok_or_else(|| {
                Rich::custom(inp.span_since(save_pos.cursor()), "expected accumulator")
            })?;
            match mnemonic {
                "tst" => Tst { d },
                "rnd" => Rnd { d },
                "clr" => Clr { d },
                "not" => Not { d },
                "asr" => Asr { d },
                "lsr" => Lsr { d },
                "abs" => Abs { d },
                "ror" => Ror { d },
                "asl" => Asl { d },
                "lsl" => Lsl { d },
                "neg" => Neg { d },
                "rol" => Rol { d },
                _ => unreachable!(),
            }
        }

        // Binary: mnemonic src,d -- source type determines variant
        "tfr" | "add" | "sub" | "cmp" | "cmpm" | "addr" | "subr" | "addl" | "subl" | "adc"
        | "sbc" | "or" | "eor" | "and" | "max" | "maxm" => {
            let src = try_alu_src(inp).ok_or_else(|| {
                Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected ALU source operand",
                )
            })?;
            if !try_comma(inp) {
                return Err(Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected ','",
                ));
            }
            let d = try_acc(inp).ok_or_else(|| {
                Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected accumulator destination",
                )
            })?;

            match src {
                AluSrc::Acc(src) => match mnemonic {
                    "tfr" => TfrAcc { src, d },
                    "add" => AddAcc { src, d },
                    "sub" => SubAcc { src, d },
                    "cmp" => CmpAcc { src, d },
                    "cmpm" => CmpmAcc { src, d },
                    "addr" => Addr { src, d },
                    "subr" => Subr { src, d },
                    "addl" => Addl { src, d },
                    "subl" => Subl { src, d },
                    "max" => Max,
                    "maxm" => Maxm,
                    _ => {
                        return Err(Rich::custom(
                            inp.span_since(save_pos.cursor()),
                            format!("'{mnemonic}' does not accept accumulator source"),
                        ));
                    }
                },
                AluSrc::XYPair { hi, lo } => match mnemonic {
                    "add" => AddXY { hi, lo, d },
                    "sub" => SubXY { hi, lo, d },
                    "adc" => Adc { hi, lo, d },
                    "sbc" => Sbc { hi, lo, d },
                    _ => {
                        return Err(Rich::custom(
                            inp.span_since(save_pos.cursor()),
                            format!("'{mnemonic}' does not accept x/y pair source"),
                        ));
                    }
                },
                AluSrc::Reg(src) => match mnemonic {
                    "add" => AddReg { src, d },
                    "tfr" => TfrReg { src, d },
                    "or" => Or { src, d },
                    "eor" => Eor { src, d },
                    "sub" => SubReg { src, d },
                    "cmp" => CmpReg { src, d },
                    "and" => And { src, d },
                    "cmpm" => CmpmReg { src, d },
                    _ => {
                        return Err(Rich::custom(
                            inp.span_since(save_pos.cursor()),
                            format!("'{mnemonic}' does not accept register source"),
                        ));
                    }
                },
            }
        }

        // Multiply: mnemonic [+/-]s1,s2,d
        "mpy" | "mpyr" | "mac" | "macr" => {
            // Optional sign prefix (default +)
            let save_sign = inp.save();
            let negate = match inp.next() {
                Some(Token::Minus) => true,
                Some(Token::Plus) => false,
                _ => {
                    inp.rewind(save_sign);
                    false
                }
            };

            let Some(s1) = try_jj_reg(inp) else {
                return Err(Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected source register (x0/y0/x1/y1)",
                ));
            };
            if !try_comma(inp) {
                return Err(Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected ','",
                ));
            }
            let Some(s2) = try_jj_reg(inp) else {
                return Err(Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected source register (x0/y0/x1/y1)",
                ));
            };
            if !try_comma(inp) {
                return Err(Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected ','",
                ));
            }
            let Some(d) = try_acc(inp) else {
                return Err(Rich::custom(
                    inp.span_since(save_pos.cursor()),
                    "expected accumulator destination",
                ));
            };

            // Try canonical order first, then commuted
            let make_mpy = |s1, s2| match mnemonic {
                "mpy" => Mpy { negate, s1, s2, d },
                "mpyr" => Mpyr { negate, s1, s2, d },
                "mac" => Mac { negate, s1, s2, d },
                "macr" => Macr { negate, s1, s2, d },
                _ => unreachable!(),
            };

            let alu = make_mpy(s1, s2);
            if alu.encode().is_some() {
                alu
            } else {
                let commuted = make_mpy(s2, s1);
                if commuted.encode().is_some() {
                    commuted
                } else {
                    return Err(Rich::custom(
                        inp.span_since(save_pos.cursor()),
                        "invalid multiply register pair",
                    ));
                }
            }
        }

        _ => {
            return Err(Rich::custom(
                inp.span_since(save_pos.cursor()),
                format!("unknown parallel ALU mnemonic: '{mnemonic}'"),
            ));
        }
    };

    let pmove = if at_eol(inp) {
        ParallelMove::None
    } else {
        parse_parallel_move(inp)?
    };

    Ok(Instruction::Parallel { alu, pmove })
}

fn parse_parallel_move<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<ParallelMove, Rich<'a, Token>> {
    // #imm,reg or XImmReg
    if is_hash(inp) {
        let (imm, force_long) = parse_imm_op_with_force(inp)?;
        expect_comma(inp)?;
        let dst = parse_reg(inp)?;
        if !at_eol(inp) {
            let s2 = parse_reg(inp)?;
            expect_comma(inp)?;
            let d2 = parse_reg(inp)?;
            return Ok(ParallelMove::XImmReg {
                imm,
                x_reg: dst,
                s2,
                d2,
            });
        }
        // L-move composite registers => LImm (24-bit immediate)
        if matches!(
            dst,
            Register::A10
                | Register::B10
                | Register::RegX
                | Register::RegY
                | Register::Ab
                | Register::Ba
        ) {
            return Ok(ParallelMove::LImm { imm, reg: dst });
        }
        // Force-long (#>): use XYMem with Immediate EA for 24-bit extension word
        if force_long {
            return Ok(ParallelMove::XYMem {
                space: MemorySpace::X,
                ea: EffectiveAddress::Immediate(imm),
                reg: dst,
                write: true,
            });
        }
        return Ok(ParallelMove::ImmToReg { imm, dst });
    }

    // Memory space first: x:/y:/l:
    if matches!(peek(inp), Some(Token::XMem | Token::YMem | Token::LMem)) {
        return parse_parallel_mem_first(inp);
    }

    // EA update: (ea),rN or bare (ea)
    if at_ea_start(inp) {
        let ea = parse_ea(inp)?;
        let dst_n = if !at_eol(inp) && matches!(peek(inp), Some(Token::Comma)) {
            expect_comma(inp)?;
            let dst = parse_reg(inp)?;
            match dst {
                Register::R(n) => n,
                _ => return Err(err(inp, "EA update destination must be R register")),
            }
        } else {
            // Bare (ea) form -- infer register from EA
            ea_reg_num(&ea).ok_or_else(|| err(inp, "cannot infer register from EA"))?
        };
        return Ok(ParallelMove::EaUpdate { ea, dst: dst_n });
    }

    // IFcc / IFcc.U: `if{cc}` or `if{cc}.u`
    if let Some(Token::Ident(name)) = peek(inp)
        && let Some(cc_str) = name.strip_prefix("if")
        && let Some(cc) = parse_cc(cc_str)
    {
        inp.next(); // consume the ifcc ident
        let save = inp.save();
        if matches!(inp.next(), Some(Token::Dot)) {
            if let Some(Token::Ident(u)) = inp.next()
                && u == "u"
            {
                return Ok(ParallelMove::IfccU { cc });
            }
            inp.rewind(save);
        } else {
            inp.rewind(save);
        }
        return Ok(ParallelMove::Ifcc { cc });
    }

    // Register source
    if let Some(Token::Ident(name)) = peek(inp)
        && let Some(reg) = parse_register(&name)
    {
        inp.next(); // consume the ident
        expect_comma(inp)?;

        // reg,x:/y:/l:
        if matches!(peek(inp), Some(Token::XMem | Token::YMem | Token::LMem)) {
            let (space, force_short) = parse_memspace_ex(inp).unwrap();
            if !force_short && at_ea_start(inp) {
                let ea = parse_ea(inp)?;
                if !at_eol(inp) {
                    return parse_dual_move_second(inp, reg, space, ea);
                }
                return if space == MemorySpace::L {
                    Ok(ParallelMove::LMem {
                        ea,
                        reg,
                        write: false,
                    })
                } else {
                    Ok(ParallelMove::XYMem {
                        space,
                        ea,
                        reg,
                        write: false,
                    })
                };
            }
            let addr = parse_expr_op(inp)?;
            if !at_eol(inp) {
                // Dual move: treat absolute address as EA AbsAddr
                let ea = EffectiveAddress::AbsAddr(addr);
                return parse_dual_move_second(inp, reg, space, ea);
            }
            return if space == MemorySpace::L {
                Ok(ParallelMove::LAbs {
                    addr,
                    reg,
                    write: false,
                    force_short,
                })
            } else {
                Ok(ParallelMove::XYAbs {
                    space,
                    addr,
                    reg,
                    write: false,
                    force_short,
                })
            };
        }

        // reg,reg -- could be RegToReg or start of Pm1 Class II
        let dst = parse_reg(inp)?;
        if !at_eol(inp) {
            // Pm1 Class II
            if matches!(peek(inp), Some(Token::YMem)) {
                parse_memspace(inp);
                let y_ea = parse_ea_or_abs_simple(inp)?;
                expect_comma(inp)?;
                let d2 = parse_reg(inp)?;
                return Ok(ParallelMove::RegY {
                    s1: reg,
                    d1: dst,
                    ea: y_ea,
                    y_reg: d2,
                    write: true,
                });
            }
            if is_hash(inp) {
                let imm = parse_imm_op(inp)?;
                expect_comma(inp)?;
                let d2 = parse_reg(inp)?;
                return Ok(ParallelMove::RegYImm {
                    s1: reg,
                    d1: dst,
                    imm,
                    y_reg: d2,
                });
            }
            // s1,d1 d2,y:(ea)
            let d2 = parse_reg(inp)?;
            expect_comma(inp)?;
            if !matches!(peek(inp), Some(Token::YMem)) {
                return Err(err(inp, "expected y: in Pm1 Class II move"));
            }
            parse_memspace(inp);
            let y_ea = parse_ea_or_abs_simple(inp)?;
            return Ok(ParallelMove::RegY {
                s1: reg,
                d1: dst,
                ea: y_ea,
                y_reg: d2,
                write: false,
            });
        }
        return Ok(ParallelMove::RegToReg { src: reg, dst });
    }

    Err(err(inp, "unexpected token in parallel move"))
}

fn parse_parallel_mem_first<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<ParallelMove, Rich<'a, Token>> {
    let (space, force_short) = parse_memspace_ex(inp).unwrap();

    if !force_short && at_ea_start(inp) {
        let ea = parse_ea(inp)?;
        expect_comma(inp)?;
        let reg = parse_reg(inp)?;
        if !at_eol(inp) {
            return parse_dual_move_after_read(inp, space, ea, reg);
        }
        return if space == MemorySpace::L {
            Ok(ParallelMove::LMem {
                ea,
                reg,
                write: true,
            })
        } else {
            Ok(ParallelMove::XYMem {
                space,
                ea,
                reg,
                write: true,
            })
        };
    }

    // x:$xxxx,reg or x:<addr,reg
    let addr = parse_expr_op(inp)?;
    expect_comma(inp)?;
    let reg = parse_reg(inp)?;
    if !at_eol(inp) {
        // Dual move: treat absolute address as EA AbsAddr
        let ea = EffectiveAddress::AbsAddr(addr);
        return parse_dual_move_after_read(inp, space, ea, reg);
    }
    if space == MemorySpace::L {
        Ok(ParallelMove::LAbs {
            addr,
            reg,
            write: true,
            force_short,
        })
    } else {
        Ok(ParallelMove::XYAbs {
            space,
            addr,
            reg,
            write: true,
            force_short,
        })
    }
}

fn parse_dual_move_second<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    x_reg: Register,
    x_space: MemorySpace,
    x_ea: EffectiveAddress,
) -> Result<ParallelMove, Rich<'a, Token>> {
    // After "reg,space:(ea)" we expect:
    // y:(ea),reg -- XYDouble
    // s2,d2 -- XReg/Pm0
    // reg,y:(ea) -- XYDouble write Y
    parse_dual_move_rest(inp, x_reg, x_space, x_ea, false)
}

fn parse_dual_move_after_read<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    x_space: MemorySpace,
    x_ea: EffectiveAddress,
    x_reg: Register,
) -> Result<ParallelMove, Rich<'a, Token>> {
    parse_dual_move_rest(inp, x_reg, x_space, x_ea, true)
}

/// Shared implementation for dual-move second-half parsing.
/// `x_write`: true when the X move is a read (mem->reg), false when it's a write (reg->mem).
fn parse_dual_move_rest<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    x_reg: Register,
    x_space: MemorySpace,
    x_ea: EffectiveAddress,
    x_write: bool,
) -> Result<ParallelMove, Rich<'a, Token>> {
    if matches!(peek(inp), Some(Token::YMem)) {
        parse_memspace(inp);
        let y_ea = parse_ea_or_abs_simple(inp)?;
        expect_comma(inp)?;
        let y_reg = parse_reg(inp)?;
        return Ok(ParallelMove::XYDouble {
            x_ea,
            x_reg,
            y_ea,
            y_reg,
            x_write,
            y_write: true,
        });
    }

    let s2 = parse_reg(inp)?;
    expect_comma(inp)?;

    if matches!(peek(inp), Some(Token::YMem)) {
        parse_memspace(inp);
        let y_ea = parse_ea_or_abs_simple(inp)?;
        return Ok(ParallelMove::XYDouble {
            x_ea,
            x_reg,
            y_ea,
            y_reg: s2,
            x_write,
            y_write: false,
        });
    }

    let d2 = parse_reg(inp)?;

    // Pm0: acc,space:(ea) data_reg,acc (only in reg-first / write form)
    if !x_write && x_reg == d2 && (x_reg == Register::A || x_reg == Register::B) {
        return Ok(ParallelMove::Pm0 {
            acc: x_reg,
            space: x_space,
            ea: x_ea,
            data_reg: s2,
        });
    }

    if x_space == MemorySpace::X {
        return Ok(ParallelMove::XReg {
            ea: x_ea,
            x_reg,
            s2,
            d2,
            write: x_write,
        });
    }
    Err(err(inp, "unexpected dual move after Y memory read"))
}

// ---- Top-level program parser ----

fn program_parser<'a>() -> impl Parser<'a, PI, Program, E<'a>> {
    custom(|inp| {
        let mut statements = Vec::new();
        consume_newlines(inp, &mut statements, true);
        loop {
            if at_eof(inp) {
                break;
            }
            let stmt = parse_statement(inp)?;
            statements.push(stmt);
            consume_newlines(inp, &mut statements, false);
        }
        Ok(Program {
            statements,
            source_lines: Vec::new(),
        })
    })
}

fn at_eof<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> bool {
    let save = inp.save();
    let is_eof = inp.next().is_none();
    inp.rewind(save);
    is_eof
}

/// Consume newlines between statements, emitting `Empty` for blank lines.
///
/// The first newline is the line terminator for the preceding statement (or
/// a leading blank before any statements). Each additional consecutive newline
/// represents a blank source line and gets an `Empty` statement so that
/// statement indices stay aligned with source line numbers.
fn consume_newlines<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    statements: &mut Vec<Statement>,
    is_leading: bool,
) {
    let mut count = 0u32;
    loop {
        let save = inp.save();
        match inp.next() {
            Some(Token::Newline | Token::NewlineCrLf) => {
                count += 1;
            }
            _ => {
                inp.rewind(save);
                break;
            }
        }
    }
    // The first newline terminates the preceding statement's line (or is
    // the first blank line at the start of the file).
    let empties = if is_leading {
        count
    } else {
        count.saturating_sub(1)
    };
    for _ in 0..empties {
        statements.push(Statement::Empty);
    }
}

fn parse_statement<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<Statement, Rich<'a, Token>> {
    if at_eol(inp) {
        return Ok(Statement::Empty);
    }

    let mut label = None;
    let save = inp.save();
    if let Some(Token::Ident(name)) = inp.next() {
        let save2 = inp.save();
        if matches!(inp.next(), Some(Token::Colon)) {
            label = Some(name.clone());
            if at_eol(inp) {
                return Ok(Statement::Label(name));
            }
        } else {
            inp.rewind(save2);
            let save_before = inp.save();
            match parse_directive_or_instruction(inp, &name, None) {
                Ok(stmt) => return Ok(stmt),
                Err(first_err) => {
                    inp.rewind(save_before);
                    label = Some(name);
                    if at_eol(inp) {
                        return Ok(Statement::Label(label.unwrap()));
                    }
                    let save3 = inp.save();
                    if let Some(Token::Ident(mnem)) = inp.next() {
                        match parse_directive_or_instruction(inp, &mnem, label) {
                            Ok(stmt) => return Ok(stmt),
                            // Label reinterpretation also failed; prefer the
                            // original error since it refers to the real mnemonic.
                            Err(_) => return Err(first_err),
                        }
                    }
                    inp.rewind(save3);
                    return Err(first_err);
                }
            }
        }
    } else {
        inp.rewind(save);
    }

    if at_eol(inp) {
        return Ok(if let Some(l) = label {
            Statement::Label(l)
        } else {
            Statement::Empty
        });
    }

    let save = inp.save();
    if let Some(Token::Ident(mnemonic)) = inp.next() {
        return parse_directive_or_instruction(inp, &mnemonic, label);
    }
    inp.rewind(save);
    Err(err(inp, "expected mnemonic or directive"))
}

fn parse_directive_or_instruction<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnemonic: &str,
    mut label: Option<String>,
) -> Result<Statement, Rich<'a, Token>> {
    let mnemonic_lc = mnemonic.to_ascii_lowercase();
    let mnemonic = mnemonic_lc.as_str();
    match mnemonic {
        "org" => {
            let space =
                parse_memspace(inp).ok_or_else(|| err(inp, "expected memory space after ORG"))?;
            let addr = parse_expr_op(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Org { space, addr },
            })
        }
        "dc" => {
            let mut values = vec![parse_expr_op(inp)?];
            loop {
                let save = inp.save();
                if matches!(inp.next(), Some(Token::Comma)) {
                    values.push(parse_expr_op(inp)?);
                } else {
                    inp.rewind(save);
                    break;
                }
            }
            Ok(Statement::Directive {
                label,
                dir: Directive::Dc(values),
            })
        }
        "ds" => {
            let size = parse_expr_op(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Ds(size),
            })
        }
        "equ" => {
            let name = label
                .take()
                .ok_or_else(|| err(inp, "equ requires a label"))?;
            let value = parse_expr_op(inp)?;
            Ok(Statement::Directive {
                label: None,
                dir: Directive::Equ { name, value },
            })
        }
        "end" => Ok(Statement::Directive {
            label,
            dir: Directive::End,
        }),
        "section" => {
            let name = parse_ident(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Section(name),
            })
        }
        "endsec" => Ok(Statement::Directive {
            label,
            dir: Directive::EndSec,
        }),
        "xref" => {
            let names = parse_ident_list(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Xref(names),
            })
        }
        "xdef" | "global" => {
            let names = parse_ident_list(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Xdef(names),
            })
        }
        "include" => {
            let path = parse_string_lit(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Include(path),
            })
        }
        "psect" => {
            let name = parse_ident(inp)?;
            // Check for optional range: space:start:end
            let range = {
                let save = inp.save();
                if let Some(space) = parse_memspace(inp) {
                    let start = parse_expr_op(inp)?;
                    // Expect ':' separator before end address
                    if matches!(inp.peek(), Some(Token::Colon)) {
                        inp.next(); // consume ':'
                        let end = parse_expr_op(inp)?;
                        Some((space, start, end))
                    } else {
                        // space:start without :end - treat as space:start only
                        Some((space, start, Expr::Literal(0)))
                    }
                } else {
                    inp.rewind(save);
                    None
                }
            };
            Ok(Statement::Directive {
                label,
                dir: Directive::Psect { name, range },
            })
        }
        "align" => {
            let alignment = parse_expr_op(inp)?;
            Ok(Statement::Directive {
                label,
                dir: Directive::Align(alignment),
            })
        }
        _ => {
            let inst = parse_instruction_with_mnemonic(inp, mnemonic)?;
            Ok(Statement::Instruction { label, inst })
        }
    }
}

fn parse_ident<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<String, Rich<'a, Token>> {
    let save = inp.save();
    match inp.next() {
        Some(Token::Ident(s)) => Ok(s),
        _ => {
            inp.rewind(save);
            Err(err(inp, "expected identifier"))
        }
    }
}

fn parse_ident_list<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
) -> Result<Vec<String>, Rich<'a, Token>> {
    let mut names = vec![parse_ident(inp)?];
    loop {
        let save = inp.save();
        if matches!(inp.next(), Some(Token::Comma)) {
            names.push(parse_ident(inp)?);
        } else {
            inp.rewind(save);
            break;
        }
    }
    Ok(names)
}

fn parse_string_lit<'a>(inp: &mut InputRef<'a, '_, PI, E<'a>>) -> Result<String, Rich<'a, Token>> {
    let save = inp.save();
    match inp.next() {
        Some(Token::StringLit(s)) => Ok(s),
        _ => {
            inp.rewind(save);
            Err(err(inp, "expected string literal"))
        }
    }
}

fn parse_instruction_with_mnemonic<'a>(
    inp: &mut InputRef<'a, '_, PI, E<'a>>,
    mnem: &str,
) -> Result<Instruction, Rich<'a, Token>> {
    match mnem {
        "nop" => Ok(Instruction::Nop),
        "rts" => Ok(Instruction::Rts),
        "rti" => Ok(Instruction::Rti),
        "reset" => Ok(Instruction::Reset),
        "stop" => Ok(Instruction::Stop),
        "wait" => Ok(Instruction::Wait),
        "enddo" => Ok(Instruction::EndDo),
        "ill" | "illegal" => Ok(Instruction::Illegal),
        "inc" => Ok(Instruction::Inc(parse_acc_op(inp)?)),
        "dec" => Ok(Instruction::Dec(parse_acc_op(inp)?)),
        "add" => parse_imm_alu_or_parallel(
            inp,
            "add",
            |imm, d| Instruction::AddImm { imm, d },
            |imm, d| Instruction::AddLong { imm, d },
        ),
        "sub" => parse_imm_alu_or_parallel(
            inp,
            "sub",
            |imm, d| Instruction::SubImm { imm, d },
            |imm, d| Instruction::SubLong { imm, d },
        ),
        "cmp" => parse_imm_alu_or_parallel(
            inp,
            "cmp",
            |imm, d| Instruction::CmpImm { imm, d },
            |imm, d| Instruction::CmpLong { imm, d },
        ),
        "and" => parse_imm_alu_or_parallel(
            inp,
            "and",
            |imm, d| Instruction::AndImm { imm, d },
            |imm, d| Instruction::AndLong { imm, d },
        ),
        "or" => parse_imm_alu_or_parallel(
            inp,
            "or",
            |imm, d| Instruction::OrImm { imm, d },
            |imm, d| Instruction::OrLong { imm, d },
        ),
        "eor" => parse_imm_alu_or_parallel(
            inp,
            "eor",
            |imm, d| Instruction::EorImm { imm, d },
            |imm, d| Instruction::EorLong { imm, d },
        ),
        "andi" => parse_andi_ori(inp, true),
        "ori" => parse_andi_ori(inp, false),
        "asl" => parse_shift(
            inp,
            "asl",
            |shift, src, dst| Instruction::AslImm { shift, src, dst },
            |shift_reg, src, dst| Instruction::AslReg {
                shift_reg,
                src,
                dst,
            },
        ),
        "asr" => parse_shift(
            inp,
            "asr",
            |shift, src, dst| Instruction::AsrImm { shift, src, dst },
            |shift_reg, src, dst| Instruction::AsrReg {
                shift_reg,
                src,
                dst,
            },
        ),
        "lsl" => parse_lsl_lsr(
            inp,
            "lsl",
            |shift, dst| Instruction::LslImm { shift, dst },
            |shift_reg, dst| Instruction::LslReg { shift_reg, dst },
        ),
        "lsr" => parse_lsl_lsr(
            inp,
            "lsr",
            |shift, dst| Instruction::LsrImm { shift, dst },
            |shift_reg, dst| Instruction::LsrReg { shift_reg, dst },
        ),
        "jmp" => parse_jmp(inp),
        "jsr" => parse_jsr(inp),
        "bra" => parse_bra_bsr(
            inp,
            |rn| Instruction::BraRn { rn },
            |target, force_long| Instruction::Bra { target, force_long },
        ),
        "bsr" => parse_bra_bsr(
            inp,
            |rn| Instruction::BsrRn { rn },
            |target, force_long| Instruction::Bsr { target, force_long },
        ),
        "bchg" => parse_bit_op(inp, |bit, target| Instruction::Bchg { bit, target }),
        "bclr" => parse_bit_op(inp, |bit, target| Instruction::Bclr { bit, target }),
        "bset" => parse_bit_op(inp, |bit, target| Instruction::Bset { bit, target }),
        "btst" => parse_bit_op(inp, |bit, target| Instruction::Btst { bit, target }),
        "jclr" => parse_bit_branch(inp, |bit, target, addr| Instruction::Jclr {
            bit,
            target,
            addr,
        }),
        "jset" => parse_bit_branch(inp, |bit, target, addr| Instruction::Jset {
            bit,
            target,
            addr,
        }),
        "jsclr" => parse_bit_branch(inp, |bit, target, addr| Instruction::Jsclr {
            bit,
            target,
            addr,
        }),
        "jsset" => parse_bit_branch(inp, |bit, target, addr| Instruction::Jsset {
            bit,
            target,
            addr,
        }),
        "brclr" => parse_bit_branch(inp, |bit, target, addr| Instruction::Brclr {
            bit,
            target,
            addr,
        }),
        "brset" => parse_bit_branch(inp, |bit, target, addr| Instruction::Brset {
            bit,
            target,
            addr,
        }),
        "bsclr" => parse_bit_branch(inp, |bit, target, addr| Instruction::Bsclr {
            bit,
            target,
            addr,
        }),
        "bsset" => parse_bit_branch(inp, |bit, target, addr| Instruction::Bsset {
            bit,
            target,
            addr,
        }),
        "do" => {
            if matches!(inp.peek(), Some(Token::Ident(s)) if s.eq_ignore_ascii_case("forever")) {
                inp.skip();
                expect_comma(inp)?;
                skip_pmem(inp);
                Ok(Instruction::DoForever {
                    end_addr: parse_expr_op(inp)?,
                })
            } else {
                let source = parse_loop_source(inp)?;
                expect_comma(inp)?;
                skip_pmem(inp);
                Ok(Instruction::Do {
                    source,
                    end_addr: parse_expr_op(inp)?,
                })
            }
        }
        "dor" => {
            if matches!(inp.peek(), Some(Token::Ident(s)) if s.eq_ignore_ascii_case("forever")) {
                inp.skip();
                expect_comma(inp)?;
                skip_pmem(inp);
                Ok(Instruction::DorForever {
                    end_addr: parse_expr_op(inp)?,
                })
            } else {
                let source = parse_loop_source(inp)?;
                expect_comma(inp)?;
                skip_pmem(inp);
                Ok(Instruction::Dor {
                    source,
                    end_addr: parse_expr_op(inp)?,
                })
            }
        }
        "rep" => Ok(Instruction::Rep {
            source: parse_rep_source(inp)?,
        }),
        "movec" => parse_movec(inp),
        "movem" => parse_movem(inp),
        "movep" => parse_movep(inp),
        "move" => parse_move_or_parallel(inp),
        "mpyi" => parse_mpyi(inp),
        "mpyri" => parse_imm_mul(inp, ImmMulKind::MpyrI),
        "maci" => parse_imm_mul(inp, ImmMulKind::MacI),
        "macri" => parse_imm_mul(inp, ImmMulKind::MacrI),
        "dmacss" => parse_dmac(inp, 0),
        "dmacsu" => parse_dmac(inp, 2),
        "dmacuu" => parse_dmac(inp, 3),
        "macsu" => parse_mac_mpy_su(inp, true, true),
        "macuu" => parse_mac_mpy_su(inp, true, false),
        "mpysu" => parse_mac_mpy_su(inp, false, true),
        "mpyuu" => parse_mac_mpy_su(inp, false, false),
        "div" => {
            let src = parse_reg(inp)?;
            expect_comma(inp)?;
            Ok(Instruction::Div {
                src,
                dst: parse_acc_op(inp)?,
            })
        }
        "cmpu" => {
            let src = parse_reg(inp)?;
            expect_comma(inp)?;
            Ok(Instruction::CmpU {
                src,
                dst: parse_acc_op(inp)?,
            })
        }
        "norm" => {
            let src = parse_reg(inp)?;
            let src_n = match src {
                Register::R(n) => n,
                _ => return Err(err(inp, "norm source must be R register")),
            };
            expect_comma(inp)?;
            Ok(Instruction::Norm {
                src: src_n,
                dst: parse_acc_op(inp)?,
            })
        }
        "lua" => parse_lua(inp),
        "lra" => parse_lra(inp),
        "clb" => {
            let s = parse_acc_op(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::Clb { s, d })
        }
        "normf" => {
            let src = parse_reg(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::Normf { src, d })
        }
        "merge" => {
            let src = parse_reg(inp)?;
            expect_comma(inp)?;
            let d = parse_acc_op(inp)?;
            Ok(Instruction::Merge { src, d })
        }
        "extract" => parse_extract_or_insert(inp, false, false),
        "extractu" => parse_extract_or_insert(inp, false, true),
        "insert" => parse_extract_or_insert(inp, true, false),
        "vsl" => {
            let s = parse_acc_op(inp)?;
            expect_comma(inp)?;
            let i_val = parse_expr_op(inp)?;
            let i_bit = match i_val {
                Expr::Literal(0) => 0u8,
                Expr::Literal(1) => 1u8,
                _ => return Err(err(inp, "vsl bit must be 0 or 1")),
            };
            expect_comma(inp)?;
            // Skip optional l: prefix (tokenized as LMem)
            let save = inp.save();
            if !matches!(inp.next(), Some(Token::LMem)) {
                inp.rewind(save);
            }
            let ea = parse_ea_or_abs(inp)?;
            Ok(Instruction::Vsl { s, i_bit, ea })
        }
        "debug" => Ok(Instruction::Debug),
        "trap" => Ok(Instruction::Trap),
        "pflush" => Ok(Instruction::Pflush),
        "pflushun" => Ok(Instruction::Pflushun),
        "pfree" => Ok(Instruction::Pfree),
        "plock" => {
            let ea = if at_ea_start(inp) {
                parse_ea(inp)?
            } else {
                EffectiveAddress::AbsAddr(parse_expr_op(inp)?)
            };
            Ok(Instruction::PlockEa { ea })
        }
        "plockr" => {
            skip_pmem(inp);
            Ok(Instruction::Plockr {
                target: parse_expr_op(inp)?,
            })
        }
        "punlock" => {
            let ea = if at_ea_start(inp) {
                parse_ea(inp)?
            } else {
                EffectiveAddress::AbsAddr(parse_expr_op(inp)?)
            };
            Ok(Instruction::PunlockEa { ea })
        }
        "punlockr" => {
            skip_pmem(inp);
            Ok(Instruction::Punlockr {
                target: parse_expr_op(inp)?,
            })
        }
        other => parse_conditional(inp, other),
    }
}
