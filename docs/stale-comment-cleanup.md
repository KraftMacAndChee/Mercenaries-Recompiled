# Stale-comment audit and cleanup

Recorded 2026-09-30. This is a comment/documentation follow-up to
[DOCUMENTATION_AUDIT.md](../DOCUMENTATION_AUDIT.md), not a runtime refactor.

## Scope and method

Reviewed the documentation audit in full and checked its stale-source-comment
findings against the implementations. A keyword scan covered 850 source,
header, template, and script files under `src`, `include`, `templates`,
`ports/mercenaries/src`, and `tools`. Initial C/C++ candidates were checked in
context; follow-up hits were classified against current code rather than removed
just because they contained `TODO`, `stub`, or historical wording.

The scan is a discovery aid, not a claim that every comment in every dependency,
archived generated tree, or local build artifact has been semantically verified.
Third-party notices, generated instruction annotations, and development histories
were not rewritten. Existing unrelated working-tree changes were preserved.

## Corrected descriptions

| Area | Stale or misleading claim | Current description and source |
| --- | --- | --- |
| Guest memory | Only data is loaded; text is absent; `.rdata` is protected read-only; mapping alone supports any title. | All supported XBE section bytes are loaded; guest accesses use the mapping offset; the shared `.rdata`/`.data` page remains writable. Section loading remains bounded to the retail lower bank even with optional graphics expansion. See [layout implementation](../src/kernel/xbox_memory_layout.c), [API](../src/kernel/xbox_memory_layout.h), and [thunk bridge](../src/kernel/kernel_bridge.c). |
| DirectSound | All buffer methods are silent stubs; future buffers will each own an XAudio2 voice. | PCM buffers use the supplementary process-global mixer. Unsupported methods remain stubs. See [buffer bridge](../src/audio/dsound_device.c). |
| APU | No interrupt delivery or voice workers; native-rate/nearest-neighbor resampling; waveOut-only output; test tone programs a voice. | Guest IRQ latching, optional dependency-aware voice workers, stateful linear interpolation, XAudio2 with waveOut fallback, and a monitor-generated test tone. See [core](../src/apu/apu_core.c), [public API](../src/apu/apu.h), [VP](../src/apu/apu_vp.c), [shim](../src/apu/apu_shim.h), and [output](../src/apu/apu_xaudio2.c). |
| Cryptography | SHA-1 uses BCrypt handles; all public-key stubs return failure. | SHA-1/RC4/HMAC are software implementations. The unused Windows-only bookkeeping type remains unused. Signature checking returns true without verification; other public-key and cipher stubs are described explicitly. See [crypto](../src/kernel/kernel_crypto.c). |
| NV2A | PGRAPH/PFIFO are future-phase stubs; PUT only advances GET; failed MMIO decoding should allocate a zero page. | The synchronous pusher parses and dispatches commands. Guest RAM is shared as VRAM. An unhandled register fault stays unhandled; physical-aperture faults must not create incoherent private pages. See [state](../src/nv2a/nv2a_state.h), [dispatch](../src/nv2a/nv2a_core.c), [fault API](../src/nv2a/nv2a_mmio_hook.h), and [fault handling](../src/nv2a/nv2a_mmio_hook.c). |
| Kernel contracts | Guest critical sections map directly to Win32; replacing all XDK libraries explains interrupt/I/O stubs. | Guest critical-section exports provide no exclusion; native workers need host synchronization. Kernel bridge delivery is distinguished from lower HAL stubs, and I/O helpers are described by actual output/status behavior. See [RTL](../src/kernel/kernel_rtl.c), [HAL](../src/kernel/kernel_hal.c), and [I/O](../src/kernel/kernel_io.c). |
| Platform and runtime templates | POSIX XInput is connected through a nonexistent `input_compat` path; future SDL audio is promised; direct guest-pointer casts and stack-only arguments describe the ABI. | Stub limitations are explicit; guest accesses use the offset, arguments can use register globals and the guest stack. Port startup no longer carries new-game customization instructions. See [platform source](../src/platform/win32_compat.c), [platform header](../src/platform/win32_compat.h), [entrypoint](../ports/mercenaries/src/main.c), [port ABI](../ports/mercenaries/src/recomp/recomp_types.h), [runtime template](../templates/runtime/recomp_types.h), and [new-game template](../templates/new-game/src/main.c). |

The edits cover 23 source/template files. Only comments and whitespace on four
lines vacated by removed comments changed. No executable statements, declarations,
macros, constants, logging strings, build settings, assets, or gameplay paths were
changed. Comment lengths can change debugger/source line numbers and diagnostic
`__LINE__` values; this is not a claim of byte-identical rebuilt binaries.

[MAINTENANCE.md](../MAINTENANCE.md) and
[ARCHITECTURE_EVIDENCE.md](../ARCHITECTURE_EVIDENCE.md) now reflect the corrected
comments. The original documentation audit remains a dated record with a follow-up
resolution section; its historical source line numbers have not been silently
rewritten.

## Intentionally retained

- Real D3D method TODOs, unsupported DirectSound streams, crypto stubs, and POSIX
  SEH/audio/MMIO limitations. The cleanup does not add support for them.
- The writable shared section-page explanation, PGRAPH write-one-to-clear
  interrupt semantics, AV field/XMV timing rationale, voice-worker dependencies,
  and guest ABI/translation requirements.
- Experimental DSP/HRIR limitations and opt-in behavior. Comments describing these
  constraints are still relevant.
- Source attribution, license notices, development history, and explanations of
  demonstrated compatibility failures. Historical context alone is not staleness.
- Actual synchronization/lifetime debt. In particular, guest critical-section
  no-ops and the supplementary mixer's buffer handoff/lifetime hazards are not
  fixed by more accurate comments.

## Validation

- Compared every edited C/C++ source/header against its saved pre-edit bytes.
  After removing comments, nonblank logical lines have identical tokens, string
  and character literals, and preprocessor directives. File hashes match the
  saved manifest. All 23 files passed.
- `git diff --check` passed for the edited source and audit documents. Relative
  Markdown links in the four updated/new documentation files resolve locally.
- Ran all **471 automated test modules** using the repository `.venv` and the
  installed x64 Visual Studio developer environment: **458 successful exits and
  13 nonzero exits**. Every module's exit status exactly matches the previous
  maintenance baseline; no new failing modules were introduced. These are module
  exits, not assertion totals or a guarantee that no tests were skipped.
- The 13 existing failures remain documented in
  [Maintenance validation](maintenance-validation.md#outstanding-nonzero-modules).
  They were not hidden or repaired as part of a comment cleanup.
- Native test fixtures compiled the components they exercise. No full game
  rebuild, public package, or interactive gameplay session was performed. A
  comment-only change does not require a new visual/audio regression campaign;
  the substantive uncertainties in the earlier audit still need their own tests.

Reproduction: from an x64 Visual Studio developer shell, run the repository
Python interpreter with `tools/recomp/run_tests.py --output <new-artifact-directory>
--jobs 1 --timeout 180`. The exact command and per-module logs are retained in the
local validation artifacts.

Local before-images, file hashes, exact comment diffs, scan inventories, and test
logs are under `artifacts/stale-comments-20260930/`. These are ignored local
engineering artifacts, not public source-package inputs.
