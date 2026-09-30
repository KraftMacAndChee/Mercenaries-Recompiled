//! Lexer tokens for DSP56300 assembly, using logos.

use logos::Logos;
use std::fmt;

/// A single token produced by the lexer.
///
/// Tokens are designed to lex the output format of the disassembler, plus
/// typical assembler source conventions (labels, directives, comments).
#[derive(Logos, Debug, Clone, PartialEq, Eq, Hash)]
#[logos(skip r"[ \t]+")]
pub enum Token {
    // --- Memory space prefixes (must come before identifiers) ---
    #[token("x:", ignore(case))]
    XMem,
    #[token("y:", ignore(case))]
    YMem,
    #[token("p:", ignore(case))]
    PMem,
    #[token("l:", ignore(case))]
    LMem,

    // --- Hex literal: $xxxx ---
    #[regex(r"\$[0-9a-fA-F]+", lex_hex)]
    Hex(u32),

    // --- Fractional literal: 0.5, .4375 (Q23 fixed-point) ---
    #[regex(r"[0-9]*\.[0-9]+", lex_frac)]
    Frac(i64),

    // --- Decimal literal ---
    #[regex(r"[0-9]+", lex_dec)]
    Dec(u32),

    // --- Punctuation ---
    #[token("#")]
    Hash,
    #[token(",")]
    Comma,
    #[token("(")]
    LParen,
    #[token(")")]
    RParen,
    #[token("+")]
    Plus,
    #[token("-")]
    Minus,
    #[token(":")]
    Colon,
    #[token(">")]
    Gt,
    #[token("<")]
    Lt,
    #[token("*")]
    Star,
    #[token("|")]
    Pipe,
    #[token("&")]
    Ampersand,
    #[token("~")]
    Tilde,
    #[token("/")]
    Slash,
    #[token(".")]
    Dot,

    // --- String literal: 'path' or "path" ---
    #[regex(r"'[^']*'", lex_string)]
    #[regex(r#""[^"]*""#, lex_string)]
    StringLit(String),

    // --- Newline ---
    #[token("\n")]
    Newline,
    #[token("\r\n")]
    NewlineCrLf,

    // --- Comment (;...) ---
    #[regex(r";[^\n]*", allow_greedy = true)]
    Comment,

    // --- Identifier (label, mnemonic, register name) ---
    // Matched case-insensitively, stored as lowercase.
    #[regex(r"[a-zA-Z_][a-zA-Z0-9_]*", lex_ident)]
    Ident(String),
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Token::XMem => write!(f, "x:"),
            Token::YMem => write!(f, "y:"),
            Token::PMem => write!(f, "p:"),
            Token::LMem => write!(f, "l:"),
            Token::Hex(v) => write!(f, "${:x}", v),
            Token::Frac(v) => write!(f, "<frac:{}>", v),
            Token::Dec(v) => write!(f, "{}", v),
            Token::Hash => write!(f, "#"),
            Token::Comma => write!(f, ","),
            Token::LParen => write!(f, "("),
            Token::RParen => write!(f, ")"),
            Token::Plus => write!(f, "+"),
            Token::Minus => write!(f, "-"),
            Token::Colon => write!(f, ":"),
            Token::Gt => write!(f, ">"),
            Token::Lt => write!(f, "<"),
            Token::Star => write!(f, "*"),
            Token::Pipe => write!(f, "|"),
            Token::Ampersand => write!(f, "&"),
            Token::Tilde => write!(f, "~"),
            Token::Slash => write!(f, "/"),
            Token::Dot => write!(f, "."),
            Token::StringLit(s) => write!(f, "'{}'", s),
            Token::Newline | Token::NewlineCrLf => write!(f, "\\n"),
            Token::Comment => write!(f, ";..."),
            Token::Ident(s) => write!(f, "{}", s),
        }
    }
}

fn lex_hex(lex: &mut logos::Lexer<Token>) -> Option<u32> {
    u32::from_str_radix(&lex.slice()[1..], 16).ok()
}

fn lex_dec(lex: &mut logos::Lexer<Token>) -> Option<u32> {
    lex.slice().parse().ok()
}

fn lex_frac(lex: &mut logos::Lexer<Token>) -> Option<i64> {
    let val: f64 = lex.slice().parse().ok()?;
    Some((val * (1u64 << 23) as f64).round() as i64)
}

fn lex_string(lex: &mut logos::Lexer<Token>) -> String {
    let s = lex.slice();
    s[1..s.len() - 1].to_string()
}

fn lex_ident(lex: &mut logos::Lexer<Token>) -> String {
    lex.slice().to_string()
}

/// Wrapper around the logos lexer providing peek/next/expect operations.
pub struct TokenStream<'src> {
    tokens: Vec<(Token, std::ops::Range<usize>)>,
    pos: usize,
    source: &'src str,
    line: usize,
}

impl<'src> TokenStream<'src> {
    /// Lex the source and create a token stream. Skips comments and newlines
    /// are preserved as tokens.
    pub fn new(source: &'src str) -> Self {
        let lexer = Token::lexer(source);
        let tokens: Vec<_> = lexer
            .spanned()
            .filter_map(|(result, span)| match result {
                Ok(tok) => {
                    if matches!(tok, Token::Comment) {
                        None
                    } else {
                        Some((tok, span))
                    }
                }
                Err(()) => None, // skip unrecognized bytes
            })
            .collect();
        Self {
            tokens,
            pos: 0,
            source,
            line: 1,
        }
    }

    /// Peek at the current token without consuming it.
    pub fn peek(&self) -> Option<&Token> {
        self.tokens.get(self.pos).map(|(t, _)| t)
    }

    /// Consume and return the current token.
    #[allow(clippy::should_implement_trait)]
    pub fn next(&mut self) -> Option<Token> {
        if let Some((tok, _)) = self.tokens.get(self.pos) {
            let tok = tok.clone();
            if matches!(tok, Token::Newline | Token::NewlineCrLf) {
                self.line += 1;
            }
            self.pos += 1;
            Some(tok)
        } else {
            None
        }
    }

    /// Consume the current token if it matches `expected`, returning true.
    pub fn eat(&mut self, expected: &Token) -> bool {
        if self.peek() == Some(expected) {
            self.next();
            true
        } else {
            false
        }
    }

    /// Return the current line number (1-based).
    pub fn line(&self) -> usize {
        self.line
    }

    /// Return the source text.
    pub fn source(&self) -> &'src str {
        self.source
    }

    /// Check if we're at the end of input or at a newline (end of statement).
    pub fn at_eol(&self) -> bool {
        matches!(
            self.peek(),
            None | Some(Token::Newline | Token::NewlineCrLf)
        )
    }

    /// Skip past newline(s), returning true if any were consumed.
    pub fn skip_newlines(&mut self) -> bool {
        let mut skipped = false;
        while matches!(self.peek(), Some(Token::Newline | Token::NewlineCrLf)) {
            self.next();
            skipped = true;
        }
        skipped
    }

    /// Return the current position in the token stream.
    pub fn pos(&self) -> usize {
        self.pos
    }

    /// Restore the position in the token stream (for backtracking).
    pub fn set_pos(&mut self, pos: usize) {
        self.pos = pos;
    }

    /// Consume and return an Ident token, or return None.
    pub fn eat_ident(&mut self) -> Option<String> {
        if let Some(Token::Ident(_)) = self.peek()
            && let Some(Token::Ident(s)) = self.next()
        {
            return Some(s);
        }
        None
    }
}
