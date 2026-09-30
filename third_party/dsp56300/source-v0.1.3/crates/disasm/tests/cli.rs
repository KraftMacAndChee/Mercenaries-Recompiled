//! Integration tests for the dsp56300-disasm CLI binary.

use std::path::PathBuf;
use std::process::{Command, Output};

// Cargo sets this env var to the path of the compiled binary.
const BINARY: &str = env!("CARGO_BIN_EXE_dsp56300-disasm");

fn disasm() -> Command {
    Command::new(BINARY)
}

// Test-data helpers
/// Encode DSP56300 words as 3-byte big-endian (native DSP byte order).
fn u24be(words: &[u32]) -> Vec<u8> {
    words
        .iter()
        .flat_map(|&w| [(w >> 16) as u8, (w >> 8) as u8, w as u8])
        .collect()
}

/// Encode DSP56300 words as little-endian u32 (matches the pram[] array layout).
fn u32le(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|&w| w.to_le_bytes()).collect()
}

/// Write `data` to a uniquely-named temp file and return its path.
fn write_temp(data: &[u8], name: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!("dsp56300_cli_{name}.bin"));
    std::fs::write(&path, data).unwrap();
    path
}

/// Run `cmd`, assert it succeeded, and return stdout as a String.
fn ok(cmd: &mut Command) -> String {
    let out: Output = cmd.output().expect("failed to run binary");
    assert!(
        out.status.success(),
        "command failed:\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr),
    );
    String::from_utf8(out.stdout).unwrap()
}

/// Run `cmd`, assert it failed (non-zero exit), and return stderr.
fn fail(cmd: &mut Command) -> String {
    let out: Output = cmd.output().expect("failed to run binary");
    assert!(
        !out.status.success(),
        "expected non-zero exit, got success\nstdout: {}",
        String::from_utf8_lossy(&out.stdout),
    );
    String::from_utf8(out.stderr).unwrap()
}

// Basic correctness
#[test]
fn help_exits_zero() {
    let status = disasm().arg("--help").status().unwrap();
    assert!(status.success());
}

#[test]
fn nop_exact_output() {
    // 0x000000 in u24be = nop
    let path = write_temp(&u24be(&[0x000000]), "nop");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    // hex_col is always 13 chars: 6 hex + 7 padding spaces.
    // format string "{}  {}  {}{}" adds 2 spaces before and after hex_col.
    // => 9 spaces total between opcode and mnemonic.
    assert_eq!(s.trim(), "P:$000000  000000         nop");
}

#[test]
fn jmp_operands_no_color() {
    // jmp $0042 - verify mnemonic and operand appear
    let path = write_temp(&u24be(&[0x0C0042]), "jmp");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    assert!(s.contains("jmp"), "missing mnemonic: {s}");
    assert!(s.contains("$0042"), "missing address: {s}");
}

#[test]
fn add_imm_operands() {
    // add #$3f,a  (0x017F80)
    let path = write_temp(&u24be(&[0x017F80]), "add_imm");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    assert!(s.contains("add"), "missing mnemonic: {s}");
    assert!(s.contains("#$3f"), "missing immediate: {s}");
    assert!(s.contains(",a"), "missing accumulator: {s}");
}

#[test]
fn movec_imm_register() {
    // movec #$03,sr  (0x0503B9)
    let path = write_temp(&u24be(&[0x0503B9]), "movec");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    assert!(s.contains("movec"), "missing mnemonic: {s}");
    assert!(s.contains("#$03"), "missing immediate: {s}");
    assert!(s.contains("sr"), "missing register: {s}");
}

// Two-word instructions
#[test]
fn two_word_hex_on_one_line() {
    // jclr #0,x:$ffffc5,$1234 - opcode 0x0A8580 + target 0x001234
    let path = write_temp(&u24be(&[0x0A8580, 0x001234]), "jclr");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    let lines: Vec<&str> = s.lines().collect();
    // Both words consumed as one instruction -> one output line.
    assert_eq!(lines.len(), 1, "expected 1 line, got: {s}");
    assert!(
        lines[0].contains("0A8580 001234"),
        "hex not shown: {}",
        lines[0]
    );
    assert!(lines[0].contains("jclr"), "mnemonic missing: {}", lines[0]);
}

#[test]
fn two_word_pc_advances_by_two() {
    // jclr (2-word) followed by nop: PC should jump from 0 to 2.
    let path = write_temp(&u24be(&[0x0A8580, 0x001234, 0x000000]), "jclr_nop");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    let lines: Vec<&str> = s.lines().collect();
    assert_eq!(lines.len(), 2, "expected 2 lines: {s}");
    assert!(lines[0].starts_with("P:$000000"), "{}", lines[0]);
    assert!(lines[1].starts_with("P:$000002"), "{}", lines[1]);
}

// --start flag
#[test]
fn start_flag_shifts_pc_labels() {
    let path = write_temp(&u24be(&[0x000000, 0x000000]), "start");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never", "--start", "0x100"])
        .arg(&path));
    let lines: Vec<&str> = s.lines().collect();
    assert_eq!(lines.len(), 2);
    assert!(lines[0].starts_with("P:$000100"), "{}", lines[0]);
    assert!(lines[1].starts_with("P:$000101"), "{}", lines[1]);
}

#[test]
fn start_flag_bare_integer_is_hex() {
    // parse_addr tries hex first: "256" -> 0x256, not decimal 256.
    let path = write_temp(&u24be(&[0x000000]), "start_bare");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never", "--start", "256"])
        .arg(&path));
    assert!(s.contains("P:$000256"), "expected PC $000256: {s}");
}

// --count flag
#[test]
fn count_limits_output() {
    let path = write_temp(&u24be(&[0x000000; 10]), "count");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never", "--count", "3"])
        .arg(&path));
    assert_eq!(s.lines().count(), 3, "expected 3 lines: {s}");
}

#[test]
fn count_zero_produces_no_output() {
    let path = write_temp(&u24be(&[0x000000; 5]), "count_zero");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never", "--count", "0"])
        .arg(&path));
    assert_eq!(s.trim(), "", "expected empty output: {s}");
}

// --format flag
#[test]
fn format_u32le_default() {
    // Same nop as u32le (little-endian 4-byte): 0x00_00_00_00
    let path = write_temp(&u32le(&[0x000000]), "u32le");
    // No --format needed; u32le is the default.
    let s = ok(disasm().args(["--color", "never"]).arg(&path));
    assert!(s.trim().ends_with("nop"), "expected nop: {s}");
}

#[test]
fn format_u24le() {
    // nop as 3-byte little-endian: bytes [0x00, 0x00, 0x00]
    let path = write_temp(&[0x00, 0x00, 0x00], "u24le_nop");
    let s = ok(disasm()
        .args(["--format", "u24le", "--color", "never"])
        .arg(&path));
    assert!(s.trim().ends_with("nop"), "expected nop: {s}");
}

#[test]
fn format_u24be_vs_u32le_agree() {
    // Encode jmp $0042 in both formats and check same mnemonic+operands.
    let path_be = write_temp(&u24be(&[0x0C0042]), "agree_be");
    let path_le = write_temp(&u32le(&[0x0C0042]), "agree_le");

    let s_be = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path_be));
    let s_le = ok(disasm()
        .args(["--format", "u32le", "--color", "never"])
        .arg(&path_le));

    // Both should produce the same disassembly text after the hex column.
    let strip = |s: &str| s.split("  ").last().unwrap_or("").trim().to_owned();
    assert_eq!(strip(&s_be), strip(&s_le));
}

// --color flag
#[test]
fn color_never_has_no_ansi() {
    let path = write_temp(&u24be(&[0x0C0042]), "no_ansi");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    assert!(!s.contains('\x1b'), "unexpected ANSI codes: {s:?}");
}

#[test]
fn color_always_has_ansi() {
    let path = write_temp(&u24be(&[0x0C0042]), "ansi");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "always"])
        .arg(&path));
    assert!(s.contains('\x1b'), "expected ANSI codes in output: {s:?}");
}

#[test]
fn color_always_contains_mnemonic_and_operand() {
    // Even with ANSI codes interspersed, the text should be present.
    let path = write_temp(&u24be(&[0x017F80]), "ansi_content");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "always"])
        .arg(&path));
    // Strip ANSI: \x1b[...m sequences
    let plain: String = strip_ansi(&s);
    assert!(plain.contains("add"), "missing mnemonic: {plain}");
    assert!(plain.contains("#$3f"), "missing immediate: {plain}");
    assert!(plain.contains(",a"), "missing register: {plain}");
}

/// Naive ANSI escape stripper for test assertions.
fn strip_ansi(s: &str) -> String {
    let mut out = String::new();
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\x1b' {
            // Skip until 'm' (end of CSI sequence).
            for ch in chars.by_ref() {
                if ch == 'm' {
                    break;
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

// Error handling
#[test]
fn missing_file_exits_nonzero() {
    let stderr =
        fail(disasm().args(["--color", "never", "/nonexistent/dsp56300_no_such_file.bin"]));
    assert!(
        stderr.contains("error"),
        "expected error message on stderr: {stderr}"
    );
}

#[test]
fn invalid_format_exits_nonzero() {
    let path = write_temp(&[], "invalid_fmt");
    // Clap will reject an unknown --format value with exit code 2.
    fail(disasm().args(["--format", "bogus"]).arg(&path));
}

#[test]
fn empty_file_produces_no_output() {
    let path = write_temp(&[], "empty");
    let s = ok(disasm()
        .args(["--format", "u24be", "--color", "never"])
        .arg(&path));
    assert_eq!(s.trim(), "", "expected no output for empty file: {s}");
}
