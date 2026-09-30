#![no_main]

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if data.len() >= 3 {
        // DSP56300 opcodes are 24-bit
        let word = (data[0] as u32) | ((data[1] as u32) << 8) | ((data[2] as u32) << 16);
        let next_word = if data.len() >= 6 {
            (data[3] as u32) | ((data[4] as u32) << 8) | ((data[5] as u32) << 16)
        } else {
            0
        };

        let _ = dsp56300_core::decode::decode(word);
        let _ = dsp56300_disasm::disassemble(0, word, next_word);
    }
});
