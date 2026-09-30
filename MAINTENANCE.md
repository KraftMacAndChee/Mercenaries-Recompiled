# Maintenance guide

Snapshot: 2026-09-29. Read [ARCHITECTURE](ARCHITECTURE.md), [SUBSYSTEMS](SUBSYSTEMS.md) and the relevant [WORKAROUNDS](WORKAROUNDS.md) entries before editing a compatibility boundary. This guide distinguishes code-level findings from historical test results. No runtime changes, new game build or live-game validation were performed for this documentation task; the repository's aggregate test runner was rerun as described below.

## Overall assessment

**Derived from current implementation.** The project has a workable architectural center: a fixed retail guest program, explicit guest ABI/memory, host compatibility services, and narrow title hooks. It also has significant maintenance cost in the implicit contracts between those layers. The highest-risk work is not formatting or naming. It is any change that alters guest state, ordering, resource ownership, allocator domain, surface identity or callback delivery while appearing locally equivalent.

**Verified from current implementation.** Large integration files remain: the manual title layer, PGRAPH, kernel bridge, main diagnostics and generated patch machinery. Splitting them mechanically does not isolate responsibilities if shared static state and ordering remain implicit. Implementation headers can preserve a deliberate single-translation-unit boundary; moving them to `.c` files changes linkage/state semantics.

# DO NOT CASUALLY CHANGE

1. **Guest register and stack ABI.** Preserve implicit register arguments, local/global EBP handling, guest return addresses, stack cleanup and call-versus-tail behavior. Host C conventions are insufficient. See W01–W05.
2. **Function discovery owner ranges.** Interior jump-table targets can share frames/epilogues. Never delete reviewed ranges merely because discovery found the same address. See W02.
3. **Guest addresses and mappings.** Supported XBE identity, stack/heap/kernel addresses, CPU aliases, MMIO aperture and optional memory configuration are linked. Four-GiB virtual placement is not four-GiB RAM. See W07/W11.
4. **Allocation ownership.** Lua/XMem fallback blocks must return through the owning allocator. Metadata reallocation invalidates host pointers into records. Physical/DMA attributes are not ordinary heap requests. See W08–W10.
5. **Safe-point ordering.** Trace-named entry hooks service functional IRQ/timer/DPC work. Do not remove, move to audio workers or reorder them without guest-context tests. See W06/W12/W13.
6. **APU lock, halt/trap and sample-clock rules.** Worker progress, IRQ latch and guest callback execution are different stages. Do not generate fake progress to cure a wait. See W27–W29.
7. **Cue and Lua lifetime.** DoWork can invalidate cue pointers. Prepared, playing, paused and terminal are different states. Continuations require scope/owner/error checks and cancellation. See W30–W34.
8. **GPU alias/state order.** Address alone is not surface identity; CPU writes, explicit copies, depth/stencil, AA, deferred draws and scanout interact. See W17–W19.
9. **Source pitch/dimensions.** CPU uploads must not infer available source bytes from a larger destination. See W20.
10. **Targeted title selectors.** Camera/UI/hair/script fixes use exact known structures or fingerprints. Broadening them can break unrelated render paths/mods. See W21/W22/W24/W41.
11. **Time domains.** Guest fractional time, vblank, APU sample time, frame pacing and developer time controls are separate. See W39.
12. **Profile ownership.** Default control changes, existing personal bindings and graphics-option persistence are distinct operations. See W36–W38.
13. **Bird-only animation changes.** The bird track phase/rate and controller flag handling must not become a global NPC animation policy. See W42.
14. **Initialization and borrowed data lifetime.** Register warmup before device creation, choose mod memory before mapping, publish memory before guest work, and stop consumers before freeing their storage. See W46.
15. **Generation/package identity.** A successful helper test is not evidence that the generated hook exists; a built executable is not necessarily the validated one; a workspace is not a source archive. See W43/W45.
16. **Shared section page protection.** The retail `.rdata`/`.data` boundary shares page `0x003B2000`; making constants read-only also protects writable globals. See W48.
17. **Streaming-cache lock scope.** The global file-cache critical section covers synchronous refill I/O. Changing it affects invalidation/lifetime as well as throughput. See W49.
18. **Surface-cache semantics.** Current tables are offset-first fixed caches, not an unlimited address+layout map. Preserve compatibility/recreation and scanout history. See W50.
19. **Native haze dimensions.** The authentic glow filter deliberately stays at 640x480/320x240 even when the rest of rendering scales. See W51.
20. **Movie clock coupling.** XGSCNT, AV field parity, title vblank/device counters and FPS pacing are distinct. See W52.
21. **Supplementary mixer lifetime.** Final release order is not proof that `SetBufferData` or render/play/stop is synchronized. See W53.
22. **Allocator record domains.** Two record systems begin at 16,384 entries but only the manual CRT heap table grows. See W54.
23. **Controller rim calibration.** The 30,000/29,500 SDL thresholds are host-physical correction, not guest dead-zone tuning. See W55.

## Established invariants and ownership review

**Verified from current implementation.** Normal RAM is 64 MiB; optional graphics expansion chooses 128 MiB before initialization. Title stack base is `0x00880000`, size 256 KiB; kernel data is `0x0087E000`; heap begins `0x008C0000`. The native executable's large host stack is separate. Synthetic kernel and MMIO regions need their own mappings. All are tied to the supported XBE layout. The `.rdata`/`.data` overlap on host page `0x003B2000` is also part of that exact-image contract and prevents page-granular read-only protection.

The shared guest CPU state is process-global. Guest callback queues and fallback allocation lists are not automatically thread-safe because an APU worker or host event API exists elsewhere. DPC/timer delivery is deferred to guest-safe points. A host callback or worker may produce pending state; it must not arbitrarily execute lifted game code.

Ownership review must distinguish: mapped guest RAM; the fixed layout-heap record table; the separately growable manual CRT-heap records; guest allocator domains; borrowed PCM; output-ring copies; guest resource headers; host COM resources; borrowed pixel records; copied continuation names; guest-to-host handles. A function called `release` in one layer may not own the other layer's bytes.

The supplementary DirectSound mixer is global rather than embedded in `MCPXAPUState`. Its allocation/free critical section does not protect all mutation/render access. Until repaired and tested, do not mutate buffer storage concurrently with playback and do not claim in-process audio restart support.

**Derived.** In-process restart is not a supported assumption. Normal-return teardown exists, but WM_QUIT uses ExitProcess, MMIO registration lacks comprehensive symmetric teardown, and process-lifetime cache/registration records remain. Adding restart would require an explicit ownership/reset audit first.

## Highest-impact improvements without redesign

These are recommendations, not changes made in this task.

| Priority | Improvement | Why it helps | Constraint before implementation |
| --- | --- | --- | --- |
| 1 | Resolve the remaining automated nonzero modules while preserving their contracts | Establishes a trustworthy baseline and distinguishes fixture breakage from real behavior defects | Keep negative controls and native assertions; do not weaken checks merely to get green. |
| 2 | Add generated-integration and patch-failure tests | Helper correctness previously coexisted with missing production hooks; partial writes can produce mixed generations | Assert actual emitted call sites, counts/order and unchanged outputs on staged failure. |
| 3 | Record small boundary contracts beside existing interfaces | Makes register/stack, callback, allocator and borrowed-resource rules visible without changing architecture | First verify each rule against callers and tests; do not invent ownership. |
| 4 | Stage patch output and commit only after validation | Reduces recovery ambiguity after late patch failure | Golden generated-output/ABI parity and failure-injection tests first; retain provenance archive ordering. |
| 5 | Consolidate exact duplicated constants/predicates only after parity checks | Reduces drifting addresses/selectors across helpers and patch scripts | Preserve widths, signedness, evaluation order, build-time versus runtime identity. |
| 6 | Separate diagnostic presentation from functional checkpoints behind stable interfaces | Prevents accidental removal of scheduler behavior during logging cleanup | Prove scheduling/trace-off behavior first; preserve environment cache semantics. |
| 7 | Introduce explicit registration/ownership documentation before narrower file extraction | Makes large files navigable and future extraction safer | Preserve single-TU static state until tests establish a replacement contract. |
| 8 | Add release smoke checks for clean-machine prerequisites and exact tested payload | Prevents dependency/packaging defects being misdiagnosed as bad ISOs | Test launcher, extractor, compiler support and source exclusion separately from gameplay. |

Do not start with general render sorting, global allocator replacement, a new guest scheduler, wholesale C++ conversion or generic timeout recovery. Those would redesign working compatibility behavior.

## Current maintainability findings

### Monolithic integration and hidden contracts

**Verified from current implementation.** Manual helpers, generated patches and implementation headers share guest register/layout assumptions. PGRAPH combines decode-facing state, resource identity, conversion, synchronization and presentation. The bridge combines dispatch with callback scheduling and structure conversion.

**Derived harm:** a local edit can cross several contracts without the type system showing it. Start by documenting entry/exit invariants and testing them, then extract only demonstrably separable responsibilities. Large size alone is not proof that any specific branch is unnecessary.

### Patch staging and source-text tests

**Verified from current implementation.** Patch processing writes some headers before all later validations. Many tests match generated source fragments; others compile extracted production helpers.

**Derived harm:** failed generation can leave mixed state, and a harmless spelling change can fail tests while a missing live hook escapes isolated helper coverage. Retain useful structural assertions, but add behavior and integration checks instead of replacing every test with looser regexes.

### Ownership and cleanup asymmetry

**Verified from current implementation.** Graphics registrations, compiler state and some optional assets retain process-lifetime storage; normal return and window-close teardown differ. The PS2 texture-registration failure path can retain a guest allocation before falling back to the original resource.

**Derived harm:** maintainers cannot safely infer whether an apparent leak is required stable storage, a process-lifetime shortcut or an actual failure-path leak. Document owner/borrower pairs and failure cleanup before changing lifetime. Do not declare the optional-art failure path harmless without exercising it.

### Stub and error reporting inconsistency

**Verified from current implementation.** Some D3D-shaped APIs return E_NOTIMPL, some return success with untouched outputs; selected kernel I/O stubs return success with zero information. Startup mixes fatal and continuation behavior. Missing kernel routes may warn once and return zero.

**Derived harm:** a successful return can be mistaken for supported functionality, and caller assumptions vary. Maintain a per-method coverage table and add output/status tests before normalizing behavior. Changing a return value can itself change gameplay control flow.

### Comment accuracy and platform expectations

**Updated 2026-09-30.** The authorized comment-only follow-up corrected the memory-loader, shared-page/thunk protection, DirectSound, software crypto, APU resampling/worker/output, and NV2A dispatch descriptions against their implementations. See [Stale-comment cleanup](docs/stale-comment-cleanup.md). This resolves the stale source-comment findings from the documentation audit; it does not change the implementations or their limitations.

**Remaining requirements:** DSP execution is opt-in; native POSIX branches do not provide Windows-path MMIO/audio parity. Guest critical-section exports provide no mutual exclusion, and the supplementary mixer ownership/lifetime risks above remain open. Preserve comments explaining these limits and the compatibility contracts rather than treating stub success as full support.

### Coupled settings and context

**Verified from current implementation.** Input mapping, prompt activity, support-screen identity, pending/applied options, renderer callbacks and guest clock hooks cross module boundaries. OG boarding reads the toggle outside the central enum.

**Derived harm:** searching only the obvious option enum or mapper misses consumers. Maintain caller maps and tests for live transitions, legacy INI profiles and independently gated feature paths.

## Validation status and commands

**Freshly rerun for this audit.** Read-only discovery found 471 modules: 248 pytest modules and 223 script modules. A serial run with the repository's configured `.venv` produced 455 successful exits and 16 nonzero exits outside a Visual Studio developer shell. The three additional failures were native D3D fixture builds; rerunning exactly those modules under the installed x64 Visual Studio developer environment made all three pass. The normalized result is therefore **458 successful module exits and 13 nonzero exits**, reproducing the result recorded in [Maintenance validation](docs/maintenance-validation.md). The earlier documented baseline was 449/470 with 21 nonzero exits. These are module exits, not assertion counts; skips and prerequisite failures require log/JUnit inspection. This audit did not rebuild the game or perform interactive play.

The 13 outstanding areas are subdue diagnostics, collision dispatch, frame-pacing deadline, developer spawn request, gameplay draw selector, global pointer watch, PGRAPH environment cache, preview logging, prompts, roadblock trace cache, setup launcher, shared epilogues and visual/audio options. Prompts includes a native binding/resource assertion; do not classify every failure as obsolete fixture text.

**Verified from current implementation.** The isolated runner's interfaces are:

```powershell
# Read-only discovery; does not import/execute test modules.
python tools/recomp/run_tests.py --list
python tools/recomp/run_tests.py --match audio --list

# Execution: choose a NEW output directory. Tests may compile native fixtures.
python tools/recomp/run_tests.py --match audio --output artifacts/validation-audio-new --jobs 1 --timeout 180
python tools/recomp/run_tests.py --output artifacts/validation-all-new --jobs 1 --timeout 180

# Product build using already generated supported input.
powershell -ExecutionPolicy Bypass -File ports/mercenaries/scripts/Build.ps1
```

Use the repository's configured Python environment and required native tools; `python` above denotes that interpreter. Read individual prerequisites before running native/retail/live-process tests. Do not raise parallelism blindly: some tests use shared native/device/environment resources. Regeneration additionally needs the supported retail input and runs a write-producing pipeline; it is not necessary for every documentation change.

## Regression matrix after meaningful code changes

| Changed boundary | Automated evidence | Manual/integration coverage |
| --- | --- | --- |
| Lifter/discovery/ABI | Retail-byte oracles, poisoned registers, stack balance, shared epilogues, generated parity | Startup, load/save, mission transitions, physics/collision, dialogue. |
| Memory/allocators | Failure injection, ownership/free/realloc, record growth, alias/mapping negative cases | Long sessions, repeated HQ entries, high asset pressure, normal and mod configurations. |
| Kernel scheduling/files | DPC duplicate/self-requeue, masked IRQ retry, alertable completions, path/access/cache invalidation and concurrent-handle serialization | Streaming, save/load, focus/window changes and ordinary-user permissions. |
| GPU resources/depth | Captured command replay, WARP/numerical tests, format/alias/copy negatives | Shadows/hubcaps, sky/terrain, satellite, transparent glass/hair, movie transitions. |
| Aspect/UI | Transform/selector assertions at several ratios/resolutions | Circular scope, text/prompt proportions, fullscreen edges and world wheel shapes. |
| Audio/lifecycle | PCM/envelope/packet tests, allocator failure, cue retirement, STOP identity/queue bounds, actual generated hooks, and concurrent `SetBufferData`/render ownership | Expo 2 smoke/extraction, Garret/HQ dialogue, guard lock, interior fade, music transitions and long sessions. |
| Input/options | Legacy/new INIs, held-key/axis capture, pad-less KB+M, context transitions and prompt resolver | On foot/car/tank/helicopter, HVT addition, support movement, controller switching. |
| Timing/FPS/XMV | Fractional accumulation, deadlines, no double presentation, wait service behavior, AV field toggling and XGSCNT/movie scheduler tests | Helicopter/vehicle/physics routes and movie natural/skip playback at 30/60/90/120/uncapped, several refresh rates and CPU/GPU loads. |
| Birds/developer tools | Lifecycle, movement/phase/rate, guest identity and request-scope tests | Independent flight, outbound return, raised-wing glide; verify ordinary NPC animation unchanged. |
| Packaging/platform | Source policy, member hashes, launcher/extractor/clean prerequisites | Clean Windows install and supported Wine/Proton paths; native POSIX requires its own qualification. |

## Evidence collection discipline

Use the exact running executable hash, build configuration, settings, save/scene and log session. F8 marks a report but does not capture a frame. Check whether a trace was still active and whether warning limits suppressed later events. Keep injected failures separate from natural failures. Copy saves for destructive developer tests; mission selection can change progression.

For audio, record allocation/stream/cue/notification/PCM/output stages separately. For graphics, capture completed frame data and enough state to identify the producer/consumer surface. For crashes, guest memory and object identities often matter more than the top host stack frame. For provenance, keep original evidence honest and distinguish present input independence from historical development claims.

## Remaining unknowns and manual obligations

- Full high-FPS physics/script parity across all missions and hardware is unproved.
- Original-game parity for the hubcap-shadow artifact remains unresolved; the simplified oracle is narrower evidence.
- The affected 3 of Clubs save did not establish a universal capture/PDA defect; do not fabricate progress as a repair.
- Every silent dialogue report is not proven to have the same cause, even with common allocation evidence.
- Exact original corruption in the HQ crash dump was not recoverable from missing guest memory.
- Music-transition fidelity and optional DSP/effects behavior require listening/capture comparisons.
- The supplementary DirectSound mixer's unsynchronized borrowed PCM mutation is a source-visible hazard; whether normal game call ordering always avoids it is unknown.
- The rationale for every hard-coded movie/vblank address and every diagnostic movie checkpoint is not independently established beyond the supported retail layout and recorded debugging history.
- Jennifer glass layering and other narrow graphics fixes still need broad vehicle/scene regression.
- Birds intentionally use an approximate replacement trajectory model.
- Native POSIX parity, in-process restart and complete success-stub coverage are not established.
- Clean current source packaging does not certify old archives, legal rights or a different historical provenance.

This guide recommends conservative improvements. It does not authorize turning compatibility uncertainty into a broad cleanup or claiming failed/unperformed validation passed.
