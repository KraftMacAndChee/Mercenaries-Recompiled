//! Integration tests for the dsp56300-asm CLI binary.

use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Output, Stdio};

const BINARY: &str = env!("CARGO_BIN_EXE_dsp56300-asm");

fn asm() -> Command {
    Command::new(BINARY)
}

/// Write assembly source to a uniquely-named temp file and return its path.
fn write_src(src: &str, name: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!("dsp56300_asm_cli_{name}.s"));
    std::fs::write(&path, src.as_bytes()).unwrap();
    path
}

/// Run `cmd`, assert it succeeded, and return stdout as raw bytes.
fn ok(cmd: &mut Command) -> Vec<u8> {
    let out: Output = cmd.output().expect("failed to run binary");
    assert!(
        out.status.success(),
        "command failed:\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr),
    );
    out.stdout
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

/// Pipe `input` on stdin, assert success, return stdout bytes.
fn ok_stdin(args: &[&str], input: &[u8]) -> Vec<u8> {
    let mut child = Command::new(BINARY)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("failed to spawn");
    child.stdin.take().unwrap().write_all(input).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(
        out.status.success(),
        "command failed:\nstderr: {}",
        String::from_utf8_lossy(&out.stderr),
    );
    out.stdout
}

// Basic correctness

#[test]
fn help_exits_zero() {
    let status = asm().arg("--help").status().unwrap();
    assert!(status.success());
}

#[test]
fn assemble_nop_u24be() {
    // nop = 0x000000 => big-endian bytes [0x00, 0x00, 0x00]
    let path = write_src("org p:$0\nnop", "nop_be");
    let bytes = ok(asm().args(["--format", "u24be"]).arg(&path));
    assert_eq!(bytes, [0x00, 0x00, 0x00]);
}

#[test]
fn assemble_nop_u32le() {
    // u32le is the default format; nop = [0x00, 0x00, 0x00, 0x00]
    let path = write_src("org p:$0\nnop", "nop_32le");
    let bytes = ok(asm().arg(&path));
    assert_eq!(bytes, [0x00, 0x00, 0x00, 0x00]);
}

#[test]
fn assemble_nop_u24le() {
    // nop = 0x000000 => little-endian bytes [0x00, 0x00, 0x00]
    let path = write_src("org p:$0\nnop", "nop_24le");
    let bytes = ok(asm().args(["--format", "u24le"]).arg(&path));
    assert_eq!(bytes, [0x00, 0x00, 0x00]);
}

#[test]
fn assemble_jmp_u24be() {
    // jmp p:$0042 = 0x0C0042 => u24be [0x0C, 0x00, 0x42]
    let path = write_src("org p:$0\njmp p:$0042", "jmp_be");
    let bytes = ok(asm().args(["--format", "u24be"]).arg(&path));
    assert_eq!(bytes, [0x0C, 0x00, 0x42]);
}

#[test]
fn assemble_jmp_u32le() {
    // jmp p:$0042 = 0x0C0042 => u32le [0x42, 0x00, 0x0C, 0x00]
    let path = write_src("org p:$0\njmp p:$0042", "jmp_32le");
    let bytes = ok(asm().arg(&path));
    assert_eq!(bytes, [0x42, 0x00, 0x0C, 0x00]);
}

#[test]
fn assemble_jmp_u24le() {
    // jmp p:$0042 = 0x0C0042 => u24le [0x42, 0x00, 0x0C]
    let path = write_src("org p:$0\njmp p:$0042", "jmp_24le");
    let bytes = ok(asm().args(["--format", "u24le"]).arg(&path));
    assert_eq!(bytes, [0x42, 0x00, 0x0C]);
}

#[test]
fn multiple_instructions_produce_multiple_words() {
    // nop (0x000000) + rts (0x00000C) = 6 bytes in u24be
    let path = write_src("org p:$0\nnop\nrts", "multi_be");
    let bytes = ok(asm().args(["--format", "u24be"]).arg(&path));
    assert_eq!(bytes, [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C]);
}

#[test]
fn output_file_flag() {
    let src_path = write_src("org p:$0\nnop", "outfile");
    let out_path = std::env::temp_dir().join("dsp56300_asm_cli_output.bin");
    ok(asm()
        .args(["--format", "u24be", "--output"])
        .arg(&out_path)
        .arg(&src_path));
    let bytes = std::fs::read(&out_path).unwrap();
    assert_eq!(bytes, [0x00, 0x00, 0x00]);
}

#[test]
fn stdin_input() {
    // Pass '-' as the file argument to read from stdin.
    let bytes = ok_stdin(&["--format", "u24be", "-"], b"org p:$0\nnop");
    assert_eq!(bytes, [0x00, 0x00, 0x00]);
}

#[test]
fn verbose_stderr_contains_segment_info() {
    let path = write_src("org p:$0\nnop\nrts", "verbose");
    let out = Command::new(BINARY)
        .args(["--format", "u24be", "--verbose"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(out.status.success());
    let stderr = String::from_utf8(out.stderr).unwrap();
    assert!(stderr.contains("segment"), "no segment count: {stderr}");
    assert!(stderr.contains("word"), "no word count: {stderr}");
    assert!(stderr.contains("P:"), "no segment address: {stderr}");
}

// Error handling

#[test]
fn missing_file_exits_nonzero() {
    let stderr = fail(asm().arg("/nonexistent/dsp56300_no_such_file.s"));
    assert!(
        stderr.contains("error"),
        "expected error message on stderr: {stderr}"
    );
}

#[test]
fn invalid_format_exits_nonzero() {
    let path = write_src("nop", "invalid_fmt");
    // Clap rejects unknown --format values with non-zero exit.
    fail(asm().args(["--format", "bogus"]).arg(&path));
}

#[test]
fn assembly_error_exits_nonzero() {
    let path = write_src("@@@ this is not valid assembly @@@", "bad_src");
    let stderr = fail(asm().arg(&path));
    assert!(!stderr.is_empty(), "expected error message on stderr");
}

#[test]
fn verbose_x_y_l_segments() {
    // Produce X, Y, and L segments so space_char's X/Y/L arms are exercised.
    let src = "org x:$0\ndc $112233\norg y:$10\ndc $445566\norg l:$20\ndc $778899";
    let path = write_src(src, "verbose_xyl");
    let out = Command::new(BINARY)
        .args(["--format", "u24be", "--verbose"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(out.status.success());
    let stderr = String::from_utf8(out.stderr).unwrap();
    assert!(stderr.contains("X:"), "missing X segment: {stderr}");
    assert!(stderr.contains("Y:"), "missing Y segment: {stderr}");
    assert!(stderr.contains("L:"), "missing L segment: {stderr}");
}

#[test]
fn empty_source_produces_no_output() {
    let path = write_src("", "empty");
    let bytes = ok(asm().args(["--format", "u24be"]).arg(&path));
    assert!(
        bytes.is_empty(),
        "expected no output for empty source: {bytes:?}"
    );
}
