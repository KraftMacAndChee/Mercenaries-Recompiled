# dsp56300 v0.1.3 MSVC x64 provenance

This directory contains the reproducible, project-local DSP56300 emulator used
by the shared MCPX APU runtime. It is not title-generated output.

- Upstream: https://github.com/mborgerson/dsp56300
- Release/tag: `v0.1.3`
- License: MIT (preserved as `LICENSE`)
- Upstream source archive: `../dsp56300-v0.1.3-source.tar.gz`
- Source archive SHA-256: `B1EFAC0EE05D3E1BDD6B336717149C19F86F9510FB7B7775AFC223B6BABF37E8`
- Rust toolchain: `1.96.0-x86_64-pc-windows-msvc` (as pinned by upstream `rust-toolchain.toml`)
- Build directory: `../source-v0.1.3`
- Build command: `cargo build --locked --release --target x86_64-pc-windows-msvc -p dsp56300-emu-ffi`
- Library SHA-256: `73C8C0373BFCB6A6EF97DCE70CD979AE0897D07441A1C0A552BF8C87A05AE625`
- Header SHA-256: `56CC51CF3F72616102077DF74B4A220D9D6717D7FD0F5F5A261703954DD8EB6C`
- License SHA-256: `73F2274B38A798D9FD1B08956BCD8691C0CF7549AFE6704B9337DF9F2E14CB57`
- Project runtime patches include the DSP56300 extended system stack (OMR SEN), MCPX program-memory aliases, and an explicit 16-bit MCPX R/N/M address-register profile. The generic profile remains 24-bit.
- Patched source SHA-256 `crates/emu/src/core.rs`: `01CD693E217C34C323557432D5E152EBF30962CF4541A01D1A93AF89CDE59EF5`
- Patched source SHA-256 `crates/emu/src/emit/mod.rs`: `1A18200D0C1F5E15B2D2D70ACAE8D1D837AC13B4F0FF91BA53A4B709646857FD`
- Patched source SHA-256 `crates/emu/src/emit/regs.rs`: `BDBAD4EE9BC5A16BA544C7AC527618849B543410309B7510A9C2954122C92441`
- Patched source SHA-256 `crates/emu/src/emit/agu.rs`: `FE915E1FB7693057EFB4D5928569DC28D577314952423A0BD14D19B29159D5BA`
- Patched source SHA-256 `crates/emu/src/emit/alu.rs`: `D411E9E2FCC842E3A903074C06917140960D9829C6F783C2D0BB73546D1BDA87`
- Patched source SHA-256 `crates/emu/src/jit.rs`: `322A0D2B7473ACE0AAAECE9108C5692FA965A76A791BA82360116CFC0684AA03`
- Patched source SHA-256 `crates/emu-ffi/src/lib.rs`: `420B514230B6C30E5854722A8396A9968BABC3C5857A9713DCA0F5C0B13DB888`

The MSVC static library requires the Windows import libraries `ws2_32`,
`ntdll`, `userenv`, `bcrypt`, and `advapi32`; the APU CMake target declares
those dependencies. A clean MSVC link-and-run probe is preserved under
`artifacts/test-runs/dsp56300-msvc-link-probe`.

The upstream GNU release artifact was also evaluated, but it references GNU
unwind symbols and therefore cannot be linked into this MSVC runtime. The
library here was rebuilt from the pinned source instead of adding a
compatibility shim.
