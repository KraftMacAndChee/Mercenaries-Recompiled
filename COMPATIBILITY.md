# Xbox and title compatibility model

Snapshot: 2026-09-29. This is a description of the current implementation, not a claim of complete Xbox compatibility. Evidence terms are defined in [ARCHITECTURE](ARCHITECTURE.md). Historical reasons and risky special cases are catalogued in [WORKAROUNDS](WORKAROUNDS.md).

## Classification

**Verified from current implementation.** More than one classification can apply to a path; for example a translated guest call can enter a reimplemented bridge which wraps a host API.

| Classification | Meaning here | Concrete example |
| --- | --- | --- |
| Reimplemented | Independently written host code supplies an expected operation | Guest allocation bookkeeping, kernel RTL/crypto operations, path and EEPROM services. |
| Translated | Guest representation/instructions are converted to another execution representation | x86-to-C, NV2A vertex programs/combiners-to-HLSL, texture formats/swizzle, guest structures. |
| Emulated | Explicit state reproduces hardware behavior over time | NV2A registers/PFIFO, APU voices/envelopes/interrupt state, optional DSP56300 execution. |
| Wrapped | Similar host capability is called through an adaptation boundary | Windows files/events, DXGI/D3D11 objects, XInput/SDL, XAudio2/waveOut. |
| Stubbed | Interface exists without full behavior | Selected D3D8 methods returning `E_NOTIMPL` or success without output; selected I/O/object paths. |
| Patched | A particular guest path is redirected/corrected | Lua allocation fallback, cue retirement hooks, support-menu mapper, narrow UI/mission fixes. |
| Approximated | Observable behavior is supplied without full original fidelity | Default audio speaker fold, title scheduling conventions, independent bird trajectories. |
| Intentionally ignored | Input/state is deliberately unused in a supported path | No-op parameters in selected kernel I/O wrappers; unused diagnostic work when disabled. This does not mean all ignored behavior is universally safe. |

## Executable and CPU assumptions

**Verified from current implementation.** The product requires the supported retail XBE hash and its address layout. Lifted instructions execute as native C, while the XBE's bytes remain mapped for guest access. `g_xbox_mem_offset + uint32_t_address` is the central memory relationship. Integer registers, guest stack and emulated floating-point/SIMD state preserve guest execution conventions; they are not represented by a normal C function signature.

Indirect dispatch uses reviewed manual/generated/kernel resolution. Selected invalid target ranges and missing imports return guarded compatibility results; these do not provide universal recovery from arbitrary bad pointers. Guest function discovery has reviewed range overrides because an interior branch target can share the owner's stack frame and epilogue. Treating every discovered target as a new function can corrupt execution while still compiling successfully.

**Derived.** This supports one known program rather than arbitrary Xbox binaries. Different XBE versions, whole-program concurrency, self-modifying code, or arbitrary instruction edge cases are not established by the presence of general-looking helper APIs. x87 helpers and flag reconstruction require focused tests; native floating-point similarity is not sufficient proof.

## Memory and handles

**Verified from current implementation.** Normal RAM is 64 MiB, with an optional 128 MiB configuration for expansive graphics mods. A large virtual-address aperture search is not a four-GiB physical allocation. Guest physical backing, CPU aliases, GPU VRAM aperture, RAMIN, synthetic kernel address space and host heap allocations are distinct domains. The retail `.rdata` and `.data` sections overlap host page `0x003B2000`, so the loader deliberately leaves `.rdata` writable; page-granular protection would fault legitimate `.data` initialization.

Guest handles and pointers require translation; a 64-bit Windows handle or pointer must not be truncated into a guest object without the intended mapping. Lower-level host structures also differ in packing/size. Null handling differs between explicit bridge conversion helpers and raw memory macros; callers must use the right contract.

Lua/audio fallback allocation preserves the original path first and records ownership when using the existing retail main pool. It does not grant arbitrary allocations unbounded host RAM or change physical/DMA allocation rules. Optional property-table growth is separate from optional graphics memory. The fixed 16,384-entry layout-heap record table and the separately growable manual CRT-heap record table are different allocators and limits.

## Kernel, scheduling and filesystem

**Verified from current implementation.** Kernel imports enter an ordinal bridge which decodes arguments, handles stack cleanup and selects compatibility implementations. The bridge services IRQ/timer/DPC work at controlled guest execution points. The APU worker can make an interrupt pending without directly invoking a guest ISR. Duplicate DPC insertion preserves the queued record; self-requeue is possible because the record is removed before callback execution.

The bridge's synchronous guest-thread startup and lower native thread wrappers coexist. This is not a faithful general Xbox scheduler. Windows events/waits and host QPC are wrapped where useful, with service calls integrated into waits. IRQL and interrupt metadata are explicitly modeled where the title relies on them.

Guest paths map to selected game/save roots. Unknown path fallback is permitted, so path translation is not a security isolation boundary. Reads are performed through host file APIs and cached where eligible, while guest APC completion is separately scheduled. Eligible cache refills hold one global cache critical section across synchronous `ReadFile`, so different cached handles can serialize. Xbox `FILE_NO_INTERMEDIATE_BUFFERING` is treated as a caching hint instead of Win32 sector-I/O semantics. `GENERIC_ALL` is adapted to data/delete operations instead of demanding unrelated Windows security-owner rights.

EEPROM/configuration behavior returns the layout the guest actually asks for. Save timestamps depend on effective local timezone information in the bulk query; system/file timestamps remain UTC. Old save labels are not silently rewritten by this adaptation.

**Unknown.** Complete parity for every unused kernel ordinal, object-manager operation, security behavior, device I/O control and cryptographic public-key path is not established. Some I/O stubs report successful zero-length work. Do not infer support from an exported function name or introductory module comment.

## Graphics

**Verified from current implementation.** Generated retail rendering can emit NV2A pushbuffers and touch mapped hardware registers. The runtime decodes supported MMIO and PFIFO commands, maintains GPU state, then translates PGRAPH work through a D3D8-shaped C API to D3D11. That API is also usable by host/title helpers; it is not simply a wrapper around a real Xbox D3D device.

Texture swizzle/format conversion, guest shader tokens, fixed-function behavior, register combiners, fog, alpha tests, depth/stencil and primitive handling are realized with host resources/states/shaders. Surface reuse/current-content decisions include address, compatible layout and history, but the actual surface tables are offset-first fixed caches (32 color and 16 depth records): an incompatible same-offset layout replaces the record rather than forming an unlimited composite-key map. Explicit copies, AA resolves and CPU writes can change which host image represents guest memory.

Important differences:

- Host render resolution and output/window resolution can differ from guest logical dimensions.
- Guest AA may require a physical depth size different from logical color size.
- A depth/stencil view may be required for stencil even if depth testing is disabled.
- Some Xbox rasterization/depth/line semantics need narrow host adjustments.
- A flip is part of command/scanout ordering, not merely `SwapChain->Present` at an arbitrary time.
- CPU frames upload using source dimensions and are then scaled; destination dimensions cannot determine source read size.
- World projection correction, UI canvas correction and final presentation scaling are distinct operations.
- Authentic haze is a fixed retail-resolution filter path using one 640x480 and two 320x240 surfaces; this special case prevents internal-resolution scaling from separating the authored blur samples.

**Verified from current implementation.** `GetDeviceCaps`, `GetDisplayMode` and `GetCreationParameters` in the inspected host device implementation include success returns without populating the expected outputs. Other methods return `E_NOTIMPL`. These are compatibility coverage limits, not a promise to arbitrary D3D8 clients.

**Historically documented.** Generic color/depth alias experiments caused sky/satellite regressions and were rejected. Current explicit copy handling remains intentional. The hubcap/shadow report was not conclusively fixed by the historical investigation; a simplified mask oracle did not prove original-game visual parity.

## Audio

**Verified from current implementation.** Audio spans generated retail DSOUND/XACT code, title lifecycle fixes, APU register/voice processing, optional DSP execution, supplementary software voices and host output. It is inaccurate to characterize the entire system as DirectSound stubs or as always executing the retail DSP program.

The default VP path folds selected speaker bins into stereo; full DSP and EP execution have separate opt-in gates. Core/library allocation does not imply that path is active. APU halt/trap/idle states gate hardware progress. Host output owns queued PCM buffers and prefers XAudio2 with waveOut fallback. `MCPXAPUState` owns the hardware/monitor worker state, not the separate global DirectSound software-mixer slots.

**Approximated / Verified from current implementation.** Default downmix intentionally supplies useful authored speaker contributions without claiming full hardware DSP/effects parity. DSP experiments must be tested independently from the supported default. Cue-ready, packet-ready, playing, paused, terminal, notification-delivered and audible are separate states.

**Patched / Verified from current implementation.** Title hooks preserve queued final packets, retire only eligible prepared wrappers, handle selected allocation failures, defer protected-scope Lua continuations and reconstruct failed STOP subscriptions only after identity-checked terminal evidence. None is a general “dialogue has been silent too long, mark it complete” timer.

**Incorrect if treated as a safety guarantee.** Final `DSoundBuffer::Release` stops/frees its mixer slot before freeing PCM, but `SetBufferData` frees/replaces PCM without equivalent locking or stop synchronization. Mixer render/play/stop and borrowed voice mutation are not consistently covered by the allocation/free critical section. Concurrent mutation can race and allocation failure can retain a dangling mixer pointer.

**Unknown.** The original player crash's exact corrupting write was not captured in its dump; a related prepared-cue lifecycle fault was independently established. Likewise, the historical abrupt-music-transition complaint was not universally proved resolved merely by fixing flags or observing an envelope in a fixture.

## Movie/XMV timing

**Verified from current implementation.** The retail decoder is generated guest code with patched hot leaf helpers. Its cadence depends on at least three distinct mechanisms: APU XGSCNT sample progress, a query-toggled AV field parity bit, and title-specific vblank/device-field counters updated by the host display service. Windows APU initialization requests a 1 ms multimedia timer period because coarse wakes made the sample counter/movie clock run slow. These are compatibility couplings, not a single generic host clock.

## Input, UI and time

**Verified from current implementation.** Host input maps to the title's guest input contexts; keyboard/mouse is not just a virtual gamepad with one global binding table. The current context and actual support/HVT screen determine mapping. Prompt activity and binding resolution are shared with display code so labels track actions. Existing personal rebinds remain distinct from default-profile evolution. SDL/PlayStation input applies only an outer-rim calibration (30,000 enter, 29,500 release) so the controller can reach the Xbox radial maximum without changing guest-authored thresholds below the rim.

FPS cap choices are 30, 60, 90, 120 and uncapped. Guest clock fractional carry, host frame slots and display refresh cooperate, while vblank remains separately modeled. **Historically documented:** high-FPS evaluation motivated these choices, but testing on a particular machine and route is not proof that every physics/script path is frame-rate-independent.

**Patched / Verified from current implementation.** Scope/menu fixes select particular UI objects/materials and transform around the appropriate origin. The world camera correction changes a separate projection parameter. Broadly scaling all HUD coordinates could double-correct prompts or break fullscreen mask edges.

## Optional original behavior and content

**Verified from current implementation.** OG Bugs currently affects the Mafia music selection fix, dateline typing sound, cloud transition behavior and helicopter boarding-height check. The latter is directly gated in `ai_boarding.h`, outside the three-item central enum. Do not use enum cardinality as the feature count. WMD-inspector correction is separately fingerprint-selected; it is not automatically OG-toggle content.

PS2 Upgrades uses supported bank/art inputs with independent option-driven selection. Retaining a supplied asset is not proof of its redistribution rights. The independent birds implementation preserves the developer feature but uses its own flocking/goal/gliding model. **Historically documented:** exact former trajectories were not retained, and the user accepted that imperfect recreation after feedback-driven corrections.

## Host and distribution assumptions

**Verified from current implementation.** The main product is Windows x64. Wine/Proton uses that executable and bundled compiler support; native POSIX source branches are not evidence of equivalent graphics/audio/MMIO support. The installer uses a supported data/XBE inventory and a completed-install marker; marker fast-path validation is intentionally less exhaustive than extraction validation.

A public source ZIP excludes retail game copies and development-only areas through explicit policy. Generated evidence archives, extracted game data, local captures and the full workspace are different artifacts. Content/path guards and byte hashes establish limited packaging properties, not a comprehensive licensing or historical-provenance certification.
