#![no_main]

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Ok(s) = std::str::from_utf8(data) {
        // Multi-line: exercise the full assembler pipeline (labels, directives, etc.)
        let _ = dsp56300_asm::assemble(s);

        // Single-line: exercise individual instruction parsing
        for line in s.lines() {
            let _ = dsp56300_asm::assemble_line(line, 0);
        }
    }
});
