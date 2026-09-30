# Compatibility workarounds and dangerous details

Snapshot: 2026-09-29. This catalog prioritizes non-obvious mechanisms in active runtime, generation and packaging paths; it does not claim every generated special case is understood. Evidence labels follow [ARCHITECTURE](ARCHITECTURE.md). History means [SOL](DEV_HISTORY_SOL.md) and/or [ASTRA](DEV_HISTORY_ASTRA.md), not a new reproduction. Source/symbol links remain useful when generated chunk numbering changes.

## Guest execution and memory

### W01 — Global registers and explicit preservation

- **WHAT:** Registers, SIMD/x87 state and guest stack encode the ABI; C signatures do not.
- **WHERE:** [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h), [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Guest routines consume implicit register arguments, including host-style callee-saved registers.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** ABI/poisoned-register investigations.
- **DEPENDENCIES:** Generated/manual/kernel boundaries and nested callbacks.
- **RISK:** Localizing state or trusting host conventions corrupts unrelated calls.
- **HISTORY:** Register and frame assumptions caused failures across subsystems; explicit context preservation remained.

### W02 — Owner ranges and shared epilogues

- **WHAT:** Reviewed ranges retain interior switch targets and shared continuations in one function frame.
- **WHERE:** [Recompile.ps1](ports/mercenaries/scripts/Recompile.ps1), [translator.py](tools/recomp/translator.py).
- **WHY:** A discovered target is not necessarily a callable function entry.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** truncated vehicle/camera/Lua/Havok/state-machine routines.
- **DEPENDENCIES:** Discovery, local control flow, dispatch and patches.
- **RISK:** Missing tails and unbalanced stack cleanup despite successful compilation.
- **HISTORY:** Shared-epilogue loss included a reported 20-byte-per-event stack leak. Function count alone did not establish correctness.

### W03 — Flags from the right predecessor

- **WHAT:** Preserve CMP/TEST semantics across intervening instructions and merged control flow.
- **WHERE:** [lifter.py](tools/recomp/lifter.py), [translator.py](tools/recomp/translator.py), [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py).
- **WHY:** A later register value may not be the value that set guest flags.
- **EVIDENCE:** **Verified from current implementation** handling; **Historically documented** causes.
- **DEPENDENCIES:** Audio status, doors, joints and many other lifted branches.
- **RISK:** Plausible but wrong results without a crash.
- **HISTORY:** Cue flags 8 versus 5 and several gameplay problems shared this translation class; they were not all independent title bugs.

### W04 — Ordered overlapping REP MOVS

- **WHAT:** Optimize safe disjoint copies but retain element order for forward overlap/propagation and MMIO width.
- **WHERE:** `XBOX_REP_MOVS` in [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h).
- **WHY:** Forward x86 string copying is not always `memmove`.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** parity work.
- **DEPENDENCIES:** Guest memory and mapped hardware transactions.
- **RISK:** Wrong overlap results or reordered hardware writes.
- **HISTORY:** Bulk copying was restricted after establishing ordered semantics.

### W05 — Different dispatch precedence

- **WHAT:** General ICALL resolves manual first; bridge startup can resolve generated first; calls and tails have different stack effects.
- **WHERE:** [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h), [kernel_bridge.c](src/kernel/kernel_bridge.c).
- **WHY:** These paths currently have distinct contracts, regardless of whether consolidation looks attractive.
- **EVIDENCE:** **Verified from current implementation**; original precedence rationale **Unknown**.
- **DEPENDENCIES:** Manual overrides, thread entry and guest return cleanup.
- **RISK:** A shared wrapper changes which implementation executes.
- **HISTORY:** Original rationale unknown. Enumerate overlap/ABI cases before unifying.

### W06 — Trace-named checkpoints are functional

- **WHAT:** `RECOMP_TRACE_RECENT` services IRQ/timer/DPC work before optional diagnostics.
- **WHERE:** [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h), bridge service functions.
- **WHY:** Pending hardware/host work needs guest-safe delivery points.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** scheduling dependence.
- **DEPENDENCIES:** Streaming, waits, ISR/DPC delivery.
- **RISK:** Removing logging-looking code creates hangs/missing completions.
- **HISTORY:** Cleanup preserved functional checkpoint order.

### W07 — Virtual arena placement and distinct aliases

- **WHAT:** Search for a usable guest address arena, back configured RAM and separately map aliases/kernel/aperture regions.
- **WHERE:** [xbox_memory_layout.c](src/kernel/xbox_memory_layout.c), [nv2a_mmio_hook.c](src/nv2a/nv2a_mmio_hook.c).
- **WHY:** Preferred host placement is not guaranteed; guest addresses must stay representable.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** startup failures.
- **DEPENDENCIES:** Address addition, VEH, CPU aliases and GPU aperture.
- **RISK:** Confusing virtual size with committed RAM or alias identity with physical index.
- **HISTORY:** Placement/zero-sentinel weakness prompted revised mapping. Current code loads executable sections too, despite older header wording.

### W08 — Grow allocation bookkeeping

- **WHAT:** Expand host allocation records under lock and refresh references after growth.
- **WHERE:** Heap tracking in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Records can exhaust before guest bytes do.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** 16,384-record exhaustion with about 2.2 MiB free.
- **DEPENDENCIES:** Alloc/free/realloc lookup and fragmentation diagnosis.
- **RISK:** Increasing RAM alone misses the limit; stale host pointers corrupt metadata.
- **HISTORY:** Bookkeeping capacity was separated from physical-memory capacity.

### W09 — Lua fallback with allocator ownership

- **WHAT:** Normal allocation first, then existing main pool; track owners and retain old blocks on failed realloc.
- **WHERE:** `sub_00178870`/main-pool helpers in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Fragmented title heap can fail table growth while the main pool has room.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** live AN HQ failure/replay.
- **DEPENDENCIES:** VM construction, guest context preservation and matching free routes.
- **RISK:** Wrong allocator frees, lost old data or recursive guest register corruption.
- **HISTORY:** A full 512-node table needed a 40,960-byte rehash; 4,526,144 main-pool bytes remained. This addressed an earlier cause than black-screen exit recovery.

### W10 — Whitelisted nonphysical XMem fallback

- **WHAT:** Only reviewed audio attributes use main-pool fallback; physical/unknown classes keep their original path.
- **WHERE:** XMem hooks in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Cue allocation can fail in one class/heap despite free memory elsewhere.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** Expo 2/Garret traces.
- **DEPENDENCIES:** Attribute whitelist, zero-fill flags and ownership records.
- **RISK:** Generalization violates DMA/alignment/free-domain expectations.
- **HISTORY:** Expo 2's 64-byte `0x6484A003` allocation failed; Garret later showed the same class. Repeated roughly 20 KiB blocks also existed in fresh runs and did not establish a leak.

### W11 — Independent default-off mod capacities

- **WHAT:** Graphics memory/pool expansion and permanent-property expansion are separate startup choices.
- **WHERE:** [mod_compatibility.c](ports/mercenaries/src/mod_compatibility.c), [recomp_world_properties.c](ports/mercenaries/src/recomp_world_properties.c), layout/generated hooks.
- **WHY:** Expansive mods exceeded separate limits.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** JE3 investigation.
- **DEPENDENCIES:** Memory banks, 8-byte records, retail `0x4434E0..0x4916E0` table range.
- **RISK:** In-place growth overlaps adjacent data; one-past pointer arithmetic must not permit writes.
- **HISTORY:** The 40,000-entry table moves to a 262,144-record pool when enabled; graphics expansion solves a different problem.

## Kernel ordering

### W12 — DPC duplicate/self-requeue rules

- **WHAT:** Preserve original duplicate arguments; dequeue before invoking; budget delivery and respect IRQL/ISR guards.
- **WHERE:** [kernel_bridge.c](src/kernel/kernel_bridge.c).
- **WHY:** Queue identity and delivery order are guest-visible.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** scheduling fixes.
- **DEPENDENCIES:** Timers, audio callbacks, saved guest context.
- **RISK:** Replaced arguments, broken self-requeue or worker-thread guest reentry.
- **HISTORY:** FIFO and retry of masked pending work were retained instead of immediate host-worker delivery.

### W13 — IRQL width and APU interrupt mapping

- **WHAT:** Use low-byte fastcall IRQL, modeled KPCR state and expected APU vector/IRQL metadata.
- **WHERE:** [kernel_bridge.c](src/kernel/kernel_bridge.c), [kernel_hal.c](src/kernel/kernel_hal.c).
- **WHY:** Wider register garbage or wrong metadata suppresses/incorrectly enables callbacks.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** CL/KPCR investigation.
- **DEPENDENCIES:** IRQ guards, APU progress and guest ISR dispatch.
- **RISK:** Silent loss of interrupt-driven work.
- **HISTORY:** The recorded bus-level-5 case used vector `0x35`, IRQL 22: `0x30 + level`, `27 - level`.

### W14 — Data rights instead of Windows ACL rights

- **WHAT:** Map guest GENERIC_ALL to useful read/write/delete access.
- **WHERE:** [kernel_file.c](src/kernel/kernel_file.c).
- **WHY:** Guest data-access intention differs from Windows ACL ownership privileges.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** access failures.
- **DEPENDENCIES:** Save creation/overwrite and installed files.
- **RISK:** Literal flag translation creates avoidable permission failures.
- **HISTORY:** Data operations were separated from host WRITE_DAC/ownership requests.

### W15 — Bulk EEPROM timezone information

- **WHAT:** Supply effective bias in the bulk layout the guest reads; keep system/file time UTC.
- **WHERE:** [kernel_xbox.c](src/kernel/kernel_xbox.c), bridge query path.
- **WHY:** Fixing an individual query does not affect a bulk `0xFF` query.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** six-hour-ahead labels.
- **DEPENDENCIES:** 96-byte layout and current timezone/DST bias.
- **RISK:** Double bias or changed UTC timestamps.
- **HISTORY:** Earlier fixed individual bias was not the active read path. Existing save labels were not rewritten.

## Graphics

### W16 — Draw families before RAMHT theories

- **WHAT:** Handle array, indexed and inline primitives, not one command form.
- **WHERE:** [nv2a_core.c](src/nv2a/nv2a_core.c), [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c).
- **WHY:** Correct binding cannot compensate for a missing draw implementation.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** first-render investigation.
- **DEPENDENCIES:** Vertex/index decoding and begin/end state.
- **RISK:** Scene-dependent missing geometry when apparently unused paths are removed.
- **HISTORY:** Early RAMHT suspicion did not explain blank output; missing DRAW_ARRAYS/indexed handling did.

### W17 — Surface address is not identity

- **WHAT:** Track layout, validity and write/draw/download history alongside guest address.
- **WHERE:** `GuestColorSurface`/`GuestDepthSurface` in [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c).
- **WHY:** Guest memory is reused and represented by several host resources.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** alias regressions.
- **DEPENDENCIES:** CPU writes, blits, sampling, depth/stencil, AA and scanout.
- **RISK:** Stale or unrelated images selected from an offset-only cache.
- **HISTORY:** Generic color/depth substitution broke sky/satellite visuals. Current explicit NV062/NV09F copies remain necessary; rejection of the heuristic did not reject all alias copies.

### W18 — Physical AA size and independent stencil

- **WHAT:** Preserve physical AA dimensions and select a DSV when either depth or stencil needs it.
- **WHERE:** [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c), [d3d8_states.c](src/d3d/d3d8_states.c).
- **WHY:** Logical color and physical zeta sizes differ; stencil works even when depth testing is disabled.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** shadow/depth work.
- **DEPENDENCIES:** Volumes, resolves, viewport and precision.
- **RISK:** Missing shadows/masks or wrong comparisons.
- **HISTORY:** ZCLAMP and stencil were investigated, but a simplified shadow oracle did not prove every hubcap artifact fixed.

### W19 — Flip/deferred-work/scanout order

- **WHAT:** FLIP_STALL and scanout service coordinate pending output, deferred work and AA resolves; refresh requires eligible new output.
- **WHERE:** [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c), [d3d8_device.c](src/d3d/d3d8_device.c), main host service.
- **WHY:** Guest flip is an ordering boundary, not an arbitrary host refresh.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** cadence/capture work.
- **DEPENDENCIES:** Frame slots, scanout validity, swapchain discard behavior.
- **RISK:** Double-present, stale/skipped output, wrong resolves or misleading captures.
- **HISTORY:** Completed-frame capture replaced reliance on an old discarded backbuffer as visual evidence.

### W20 — Source-sized CPU uploads

- **WHAT:** Upload to a texture matching source dimensions, then copy/scale to the host backbuffer.
- **WHERE:** `UploadFrame` path in [d3d8_device.c](src/d3d/d3d8_device.c).
- **WHY:** A null UpdateSubresource box covers the destination, not the available source bytes.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** driver crash.
- **DEPENDENCIES:** CPU/movie pitch, dimensions and destination scaling.
- **RISK:** Out-of-bounds reads inside the graphics driver.
- **HISTORY:** The established case used a 1920x1080 destination with a 640x480 source.

### W21 — Narrow hair/glass ordering

- **WHAT:** Select the specific skinned fringe draw, render before glass and remove only its later item while preserving begin/end structure.
- **WHERE:** [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py).
- **WHY:** Jennifer's fringe otherwise overlays transparent rear glass.
- **EVIDENCE:** **Verified from current implementation** selector; **Historically documented** 30-vertex fringe investigation.
- **DEPENDENCIES:** Draw/material/geometry identity and guest ordering.
- **RISK:** Broad transparent sorting disturbs other effects or duplicates geometry.
- **HISTORY:** A narrow correction was chosen. Coverage across all vehicles still needs visual regression; code presence alone is not visual proof.

### W22 — World aspect and UI aspect differ

- **WHAT:** Correct title projection separately from scope/menu canvas transforms.
- **WHERE:** [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), [recomp_ui.c](ports/mercenaries/src/recomp_ui.c), [menu_layout_guest.h](ports/mercenaries/src/menu_layout_guest.h), generated hooks.
- **WHY:** Correct world geometry does not imply circular overlays or proportioned text.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** vehicle/scope comparisons.
- **DEPENDENCIES:** Aspect option, local UI origins, prompt scaling and fullscreen edges.
- **RISK:** Global scaling double-corrects some paths and misses others.
- **HISTORY:** Camera reasoning used roughly 0.764 at 4:3 and 0.573 at 16:9 instead of 0.530. Scope used center 320 and exact brush/vtable/canvas identities, retaining mask half-pixel edges; menu scaling used its local origin.

### W23 — Lines and transient shader state

- **WHAT:** Accept valid two-vertex lines and restore specialized geometry/depth draw state.
- **WHERE:** [d3d8_scaled_lines.c](src/d3d/d3d8_scaled_lines.c), [d3d8_triangle_depth.c](src/d3d/d3d8_triangle_depth.c), PGRAPH consumers.
- **WHY:** Triangle assumptions and leaked geometry-shader state do not apply to later primitives.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** primitive fixes.
- **DEPENDENCIES:** Topology, shaders, raster/depth state and following draws.
- **RISK:** Missing UI lines or subsequent draws using the wrong shader.
- **HISTORY:** Cleanup retained apparently redundant state restoration.

### W24 — Narrow clears and cached-light recovery

- **WHAT:** Recover selected black-light-cache/entry-surface conditions, not clear every surface each frame.
- **WHERE:** Title hooks in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py), renderer consumers.
- **WHY:** Persistent and aliased surface content can be intentional.
- **EVIDENCE:** **Verified from current implementation** targeted hooks; **Historically documented** static-light/Haeju work.
- **DEPENDENCIES:** Exact guest state, surface ownership and transition timing.
- **RISK:** Broad resets erase valid contents or conceal the real producer/consumer fault.
- **HISTORY:** Haeju handling was scoped to gameplay entry; lighting/joint observations did not justify general scene resets.

### W25 — Compiler selection and cache identity

- **WHAT:** Centralize compiler selection, use bundled native compiler on Wine, validate source/profile/version-keyed bytecode and warm without binding live state.
- **WHERE:** [d3d8_compiler.c](src/d3d/d3d8_compiler.c), [d3d8_shader_cache.c](src/d3d/d3d8_shader_cache.c), [shader_warmup.c](ports/mercenaries/src/shader_warmup.c).
- **WHY:** Compiler availability and first-use compilation affect compatibility/stutter.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** platform/performance work.
- **DEPENDENCIES:** Early registration, cache validation and first-use fallback.
- **RISK:** Wrong shader identity, malformed cache reads or warmup state leakage.
- **HISTORY:** The compiler wrapper changed native fixture linkage requirements. Cache identity is not claimed to include a driver fingerprint.

## Audio and HQ transitions

### W26 — Final nonempty packet readiness

- **WHAT:** Retain decoded final packet data and clear pending state at the proper point before Process handling.
- **WHERE:** Audio stream hooks in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c) and generated patches.
- **WHY:** EOF means no more input, not no already-decoded samples.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** packet analysis.
- **DEPENDENCIES:** Status, callbacks and buffer lifetime.
- **RISK:** Dropped tails, permanent pending status or early completion.
- **HISTORY:** Packet/completion order was corrected rather than masking it with forced script callbacks.

### W27 — Idle/trapped APU does not invent time

- **WHAT:** Gate hardware progress on SECTL, wait while idle and separately permit software/test audio.
- **WHERE:** Frame worker in [apu_core.c](src/apu/apu_core.c).
- **WHY:** Manufactured 32-sample progress can complete work that hardware never performed.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** scheduling work.
- **DEPENDENCIES:** Envelopes, sample clock, interrupts and output.
- **RISK:** Silent time advancement that looks successful to guest code.
- **HISTORY:** Host pacing was separated from actual hardware progress.

### W28 — Default speaker fold versus optional DSP

- **WHAT:** Default output folds selected VP bins; DSP/EP execution is separately opt-in.
- **WHERE:** [apu_dsp.c](src/apu/apu_dsp.c), [apu_vp.c](src/apu/apu_vp.c).
- **WHY:** Authored speaker contributions must survive; GP intermediates are not final stereo.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** mixing/DSP experiments.
- **DEPENDENCIES:** Voice routing, clipping/conversion and monitor path.
- **RISK:** Silent sources, wrong gain or effects noise.
- **HISTORY:** Experimental DSP/HRIR paths did not replace the default. It uses front bins 0/1 and 6/7, center 2, LFE 3, rear 4/5 and 8/9; center/rear weight is about 0.7071 and LFE 0.5.

### W29 — Bounded multipass voice dependencies

- **WHAT:** Revisit eligible voice dependencies without rewriting guest link structures to force order.
- **WHERE:** [apu_vp.c](src/apu/apu_vp.c).
- **WHY:** Linked producer/consumer voices may need more than a naive single pass.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** ordering work.
- **DEPENDENCIES:** Voice links, bins and bounded frame work.
- **RISK:** Link mutation changes guest state; unbounded retry stalls output; one pass loses contributions.
- **HISTORY:** Bounded lookahead/multipass handling was preferred to link repair.

### W30 — Protected-scope voice continuations

- **WHAT:** Copy/queue continuation names, defer through protected work, cancel matching error/teardown scopes and unlink before dispatch.
- **WHERE:** [voice_callback_queue.h](ports/mercenaries/src/voice_callback_queue.h), manual helpers and generated Pcall/Deinit hooks.
- **WHY:** Immediate/failed voice completion can request a function before the script defines it.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** HQ work.
- **DEPENDENCIES:** Depth/serial, VM owner and actual generated insertion sites.
- **RISK:** Reentrancy, stale VM use, duplicate callbacks or permanent waiting.
- **HISTORY:** Helper tests once passed while generated hooks were absent. Test both helper and integration sites.

### W31 — Exact HQ error recovery and chair cleanup

- **WHAT:** Recover a confirmed Lua error for the matching EnterBriefing owner/VM/scene after construction and safe scope handling; use authored exit/chair cleanup.
- **WHERE:** HQ helpers in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), generated hooks.
- **WHY:** Black screen with movement can be incomplete transition state, not CPU freeze or just alpha.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** live HQ debugging.
- **DEPENDENCIES:** Scene ownership, callback/event cancellation and seated camera state.
- **RISK:** Broad fade resets/timeouts bypass valid scenes; omitted chair cleanup leaves bad player/camera state.
- **HISTORY:** Chair cleanup followed stale seated state. Why one earlier recovery failed in the allocation case remained unresolved. Exterior guard lock and interior black screen are distinct.

### W32 — Terminal guard wait reconciliation

- **WHAT:** Reconcile an exact registered PreBriefing wait against managed terminal cue state at a safe checkpoint.
- **WHERE:** Guard/audio helpers in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Immediate STOP/Finished can suppress the expected callback.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** silent-guard investigation.
- **DEPENDENCIES:** Registered identity, wrapper/cue state and notification ordering.
- **RISK:** Equating absent low-level voice with completion skips valid dialogue or duplicates scripts.
- **HISTORY:** Disappearance alone was rejected: healthy queued playback also contains voice-free intervals.

### W33 — Failed STOP subscription intent

- **WHAT:** Keep bounded descriptors and emit ordinary notification only after terminal flags, empty children and wrapper/handle/bank identity agree.
- **WHERE:** Notification recovery in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Subscription allocation can fail after Play succeeds.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** ignored 120-byte allocation failure.
- **DEPENDENCIES:** 128-entry intent table, 24-byte descriptors, bounded output queue and terminal evidence.
- **RISK:** Timer-based completion or reused identity advances the wrong script.
- **HISTORY:** Tests included a 50-event queue and negative identity cases. Recovery produces a notification, not direct scene success.

### W34 — Prepared cue retirement and post-DoWork state

- **WHAT:** Clear eligible waiting/prepared wrapper references before free, preserve playing/paused cases and re-evaluate after DoWork.
- **WHERE:** Cue lifecycle in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), generated retirement hook.
- **WHY:** Nested work can invalidate cached pointers/state.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** independently reproduced lifecycle gap.
- **DEPENDENCIES:** Wrapper pool/state, list ownership and hook order.
- **RISK:** Use-after-free, removing active cues or retrying stale state.
- **HISTORY:** Dump 22972 lacked guest memory needed to prove the exact originating corruption. The fixed mechanism is better established than attribution to that user's crash.

### W35 — Authored music release rather than arbitrary crossfade

- **WHAT:** Preserve guest envelope/state behavior instead of adding a generic host crossfade.
- **WHERE:** [apu_vp.c](src/apu/apu_vp.c), cue-flag translation/manual integration.
- **WHY:** Authored release data already defines transition behavior.
- **EVIDENCE:** **Verified from current implementation** envelope handling; **Historically documented** 9/3-second releases and 843/281 groups of 16 APU frames.
- **DEPENDENCIES:** Cue flags, 32-sample/48 kHz progression and stop semantics.
- **RISK:** Masking another defect or changing intended music.
- **HISTORY:** Envelope tests worked before a later flag fix, so the broad abrupt-transition report was not conclusively explained.

## Input, options and content

### W36 — Per-held-input rebinding gate

- **WHAT:** Snapshot held inputs, release-gate each independently, use separate analog release/activation thresholds and retain F10 cancellation.
- **WHERE:** [recomp_controls.c](ports/mercenaries/src/recomp_controls.c).
- **WHY:** Requiring all inputs neutral lets unrelated held/drifting controls block every new key.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** only-F10-working reports.
- **DEPENDENCIES:** Capture snapshot, edge detection and approximately 0.25/0.65 thresholds.
- **RISK:** Capturing the opening click, permanent blocking or drift-selected binds.
- **HISTORY:** Global-neutral gating was replaced; prompt switching likewise uses activity edges, not device presence.

### W37 — Defaults versus personal profiles

- **WHAT:** QWERTY defaults include on-foot/vehicles, preserving explicit existing binds and legacy missing-key behavior.
- **WHERE:** [recomp_controls.c](ports/mercenaries/src/recomp_controls.c), [controls_mapping.h](ports/mercenaries/src/controls_mapping.h).
- **WHY:** New defaults must not overwrite personal settings.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** explicit user requirement.
- **DEPENDENCIES:** Profile/version selection, context reset, prompt resolver and mouse aim.
- **RISK:** Silent control changes or mismatched prompts.
- **HISTORY:** Helicopter/car/tank were explicitly included after follow-up; new-profile mouse aim and legacy behavior remained distinct.

### W38 — HVT award versus support selection/addition

- **WHAT:** Recognize HVT screen `0xC739FD0F` and support submodes; selection allows movement cancellation while addition is modal.
- **WHERE:** [recomp_controls.c](ports/mercenaries/src/recomp_controls.c), manual input hooks.
- **WHY:** A modal HVT award can still use default joy context.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** gamepad-success/KB+M-failure report.
- **DEPENDENCIES:** Screen identity, submode 1/2, current packet/context and held-fire barrier.
- **RISK:** Broad modal handling breaks movement cancellation; broad confirm mapping fires through menus.
- **HISTORY:** Ordinary mission support-add testing missed the different HVT path.

### W39 — Fractional guest time and distinct presentation time

- **WHAT:** Carry fractional 3000-unit increments at a selected main-loop path; expose 30/60/90/120/uncapped while keeping separate vblank/host pacing.
- **WHERE:** Generated clock hook, [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), [recomp_options.c](ports/mercenaries/src/recomp_options.c), D3D timing.
- **WHY:** High-rate integer truncation loses game time.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** helicopter/FPS evaluation.
- **DEPENDENCIES:** Exact hook, cap serialization and frame slots.
- **RISK:** Global carry double-corrects time; changing vblank with the cap affects other systems.
- **HISTORY:** 144/240 were not retained. One high-end 4K benchmark and a narrow C17 damping correction do not prove every physics path rate-independent.

### W40 — OG Bugs exceeds the central enum

- **WHAT:** Gate Mafia music, dateline typing, cloud transition and helicopter boarding height.
- **WHERE:** [recomp_original_bugs.c](ports/mercenaries/src/recomp_original_bugs.c), [ai_boarding.h](ports/mercenaries/src/ai_boarding.h).
- **WHY:** Selectable original quirks coexist with fixes.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** Embedded journalist report.
- **DEPENDENCIES:** Option, cue identity, helicopter vtable and finite vertical separation around 1.1 units.
- **RISK:** Enum-only review misses boarding; gating every compatibility fix disables unrelated repairs.
- **HISTORY:** Boarding was initially thought recomp-specific, then identified as original behavior. WMD remains separate; Embedded filming/dialogue duration was not established resolved.

### W41 — Fingerprinted scripts and stable optional content

- **WHAT:** Match script length/hash before appending WMD correction; identify supported PS2 banks/art and retain stable backing while selecting additions through the option.
- **WHERE:** [mission_script_fixes.c](ports/mercenaries/src/mission_script_fixes.c), [ps2_upgrades.c](ports/mercenaries/src/ps2_upgrades.c), [ps2_retail_mix.h](ports/mercenaries/src/ps2_retail_mix.h).
- **WHY:** Avoid rewriting mods or treating arbitrary data as known layouts.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** independent retail mixing derivation.
- **DEPENDENCIES:** Fingerprints, caller-owned script buffer and persistent bank/pixels.
- **RISK:** Broken mods or stale guest/host references after early free.
- **HISTORY:** Mixing was checked against the supplied PS2 disc while retaining its behavior. Asset supply/technical identity does not establish redistribution rights.

### W42 — Independent birds, bird-only animation fixes

- **WHAT:** Fixed-step motion uses individual goals/outbound-return behavior; wing policy alternates flapping and raised-wing gliding, restoring saved rates.
- **WHERE:** [boids.c](ports/mercenaries/src/boids.c), [boids_guest.h](ports/mercenaries/src/boids_guest.h), [boid_wings.h](ports/mercenaries/src/boid_wings.h).
- **WHY:** Initial replacement looked uniformly circling and could freeze wings down or flap continuously.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** visual feedback/revisions.
- **DEPENDENCIES:** Bird actor/controller identity, track phase/rate, 30 Hz stepping and actor lifecycle.
- **RISK:** General animation flag changes affect all NPCs; wrong phase creates down-wing gliding.
- **HISTORY:** Raised phase near 0.25 and down near 0.75 were established for this track; excursion and flap/glide timing were retuned. Exact former trajectories are not claimed.

## Build, diagnostics and lifetime

### W43 — Partial generated-patch writes

- **WHAT:** Headers can be written before every later patch validates.
- **WHERE:** [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py).
- **WHY:** Current staged machinery is not a filesystem transaction.
- **EVIDENCE:** **Verified from current implementation**; original ordering rationale **Unknown**.
- **DEPENDENCIES:** ABI headers and generated C must agree.
- **RISK:** Building after failure combines old/new pieces.
- **HISTORY:** Original rationale unknown. Staging/failure tests should precede transactional refactoring; none was performed here.

### W44 — Diagnostics can be capped, cached or mutating

- **WHAT:** Bound log queue/files/warnings, cache some environment settings and separate deliberate state/failure injection tools.
- **WHERE:** [preview_log.c](src/kernel/preview_log.c), [main.c](ports/mercenaries/src/main.c), [tools/diagnostics](tools/diagnostics), manual probes.
- **WHY:** Full tracing changes timing/volume; fault injection is not observation.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** missed audio lines/trace windows.
- **DEPENDENCIES:** Startup flags, executable identity, active trace and warning limits.
- **RISK:** Missing log lines mistaken for absent failures; injected behavior mistaken for natural reproduction.
- **HISTORY:** An audio warning cap hid later failures; an HQ entry occurred after a short trace window closed. Confirm process/build and trace coverage first.

### W45 — Source guard and validated-build identity

- **WHAT:** Explicit source policy rejects forbidden development/data/archive/link content with reviewed hash-bound fixture exceptions; package ties runtime to validation/provenance hashes.
- **WHERE:** [source_package_policy.py](tools/recomp/source_package_policy.py), [Archive-GeneratedProvenance.py](ports/mercenaries/scripts/Archive-GeneratedProvenance.py), [Package-Preview.py](ports/mercenaries/scripts/Package-Preview.py).
- **WHY:** Workspace contents are broader than distributable inputs.
- **EVIDENCE:** **Verified from current implementation**; **Historically documented** archive separation findings.
- **DEPENDENCIES:** Path/content rules, fixture hashes and validation identity.
- **RISK:** Publishing whole workspace/Git/evidence, or assuming new clean archives sanitize old ones.
- **HISTORY:** An older archive containing a retired SDK header passed a guard. Guards do not certify licensing or erase history.

### W46 — ExitProcess versus normal teardown

- **WHAT:** Guest return performs cleanup, but WM_QUIT calls ExitProcess; some registrations/caches last for the process.
- **WHERE:** [main.c](ports/mercenaries/src/main.c), [d3d8_device.c](src/d3d/d3d8_device.c), MMIO/renderer registration.
- **WHY:** **Derived:** practical lifetime model is one game per process.
- **EVIDENCE:** **Verified from current implementation** paths; comprehensive restart support **Unknown**.
- **DEPENDENCIES:** Worker joins, borrowed RAM/pixels, COM and exception handlers.
- **RISK:** Assuming graceful unwinding or in-process restart safety.
- **HISTORY:** Original rationale unknown. This does not classify every retained allocation as harmless.

### W47 — Installer marker shortcut and staged moves

- **WHAT:** Normal validation trusts a nonempty marker; full installation validation checks inventory/XBE; activation can first move old data aside.
- **WHERE:** [setup_launcher.c](ports/mercenaries/src/setup_launcher.c).
- **WHY:** Ordinary launch and extraction verification are separate; avoiding repeated expensive work is **Derived**.
- **EVIDENCE:** **Verified from current implementation** control flow.
- **DEPENDENCIES:** Marker after validation, owned stage/lock/cancel state and filesystem moves.
- **RISK:** Mistaking launch check for full integrity validation or two moves for atomic rollback.
- **HISTORY:** Original rationale unknown for every shortcut. Extraction exit code alone cannot distinguish malformed media from tool/runtime failure.

### W48 — Shared `.rdata`/`.data` host page remains writable

- **WHAT:** Do not apply read-only protection to the retail `.rdata` range.
- **WHERE:** [xbox_memory_layout.c](src/kernel/xbox_memory_layout.c).
- **WHY:** `.rdata` ends at `0x003B2454` and `.data` begins at `0x003B2360`; both share 4 KiB page `0x003B2000`. `VirtualProtect` would also make the first writable globals read-only.
- **EVIDENCE:** **Verified from current implementation** and its explicit fault explanation.
- **DEPENDENCIES:** Supported-XBE section layout and page-granular host protection.
- **RISK:** A generic “protect constants” cleanup faults title initialization. The read-only statement near thunk installation in `kernel_bridge.c` is stale.
- **HISTORY:** The source records the observed initialization fault; the exact first historical faulting guest write was not re-run during this audit.

### W49 — Streaming-file cache serializes refills

- **WHAT:** Treat Xbox `FILE_NO_INTERMEDIATE_BUFFERING` as eligibility for buffered host read-ahead/recent-read caching; protect all cache contexts with one critical section.
- **WHERE:** [kernel_file.c](src/kernel/kernel_file.c).
- **WHY:** Win32 unbuffered sector I/O caused long synchronous HDD stalls for interleaved media-bank reads.
- **EVIDENCE:** **Verified from current implementation**; HDD rationale is explicit in source and **Historically documented**.
- **DEPENDENCIES:** Sixteen contexts, two 8 MiB forward windows and sixteen 128 KiB recent blocks per active cached handle.
- **RISK:** The lock is held across refill `ReadFile`, so eligible reads on different handles serialize. Moving I/O outside the lock requires a real lifetime/invalidation design, not only a lock-scope reduction.
- **HISTORY:** A bounded recent tier was added after two large windows thrashed on recurring separated reads.

### W50 — Surface cache is offset-first, not an unlimited composite-key map

- **WHAT:** Reuse a same-offset color surface only when pitch/format/AA and physical size are compatible; otherwise release and recreate that offset's record.
- **WHERE:** [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c).
- **WHY:** Address alone cannot establish compatible host backing, while clip narrowing must preserve a larger compatible image.
- **EVIDENCE:** **Verified from current implementation**.
- **DEPENDENCIES:** Fixed 32-color/16-depth caches, write/download serials, aliases and scanout fallback.
- **RISK:** Describing this as a general address+layout map suggests simultaneous same-offset variants that the implementation does not actually retain.
- **HISTORY:** Pointer-only and address-only reasoning caused feedback/alias regressions; the exact original rationale for every cache limit remains **Unknown**.

### W51 — Authentic haze keeps retail filter resolution

- **WHAT:** Recognize the retail four-vertex glow sequence and route it through one 640x480 input plus two 320x240 ping-pong surfaces before copying to the scaled target.
- **WHERE:** [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c), [recomp_options.c](ports/mercenaries/src/recomp_options.c).
- **WHY:** Scaling the three bilinear samples with internal resolution separates them into visible ghost images.
- **EVIDENCE:** **Verified from current implementation**; visual motivation is explicit in source and **Historically documented**.
- **DEPENDENCIES:** Haze mode 1, exact source/target dimensions, draw shape, surface serial and later state restoration.
- **RISK:** Generalizing this path to unrelated post-processing or changing its dimensions breaks the recognized retail filter contract.
- **HISTORY:** Haze, fog, flare alpha and water were investigated as separate systems; this is not a global blur rule.

### W52 — Movie cadence depends on APU, AV field and title counters

- **WHAT:** Keep XGSCNT sample progress, query-toggled AV field parity, display-refresh guest counters, generated XMV decode and PGRAPH texture updates coordinated without merging them into one clock.
- **WHERE:** [apu_core.c](src/apu/apu_core.c), [kernel_hal.c](src/kernel/kernel_hal.c), [main.c](ports/mercenaries/src/main.c), [xmv_fast_helpers.h](ports/mercenaries/src/recomp/xmv_fast_helpers.h).
- **WHY:** Coarse Windows timer wakes slowed the audio-mastered movie clock; wall-clock field parity can repeat when polled below 60 Hz and stall frame release.
- **EVIDENCE:** **Verified from current implementation** and explicit source comments; broader movie-debug narrative is **Historically documented**.
- **DEPENDENCIES:** `timeBeginPeriod(1)`/normal `timeEndPeriod(1)`, AV `GUESS_FIELD`, guest `0x00793564`, device pointer `0x00299378`, field offset `0x1DE8`, and supported-XBE layout.
- **RISK:** Tying these counters to FPS Cap, milliseconds or host refresh alone produces freeze/catch-up or 16.67x clock errors.
- **HISTORY:** Most movie checkpoints are diagnostics. Their existence is not proof that each checkpoint is a functional requirement.

### W53 — Supplementary DirectSound mixer synchronization is incomplete

- **WHAT:** Final buffer release stops/frees a mixer slot before freeing PCM, but `SetBufferData` and mixer render/play/stop do not implement equivalent locking.
- **WHERE:** [dsound_device.c](src/audio/dsound_device.c), [apu_core.c](src/apu/apu_core.c).
- **WHY:** **Unknown.** This is a current implementation limitation, not a justified compatibility behavior.
- **EVIDENCE:** **Verified from current implementation**.
- **DEPENDENCIES:** `APUMixerVoice` borrows `DSoundBuffer` PCM; allocation/free alone use `g_mixer_cs`.
- **RISK:** Concurrent replacement can race render; replacement-allocation failure can leave the voice pointing at freed PCM. Do not cite final-release order as proof all mutation is safe.
- **HISTORY:** No conversation evidence established that this race is benign or intentionally unreachable.

### W54 — Two 16,384-record allocator facts are not the same limit

- **WHAT:** Distinguish the fixed layout-heap record array from the manual replacement CRT heap's growable host record table.
- **WHERE:** [xbox_memory_layout.c](src/kernel/xbox_memory_layout.c), [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).
- **WHY:** Both begin with the number 16,384 but have different owners, address arenas, growth behavior and locking.
- **EVIDENCE:** **Verified from current implementation**.
- **DEPENDENCIES:** Layout heap/free coalescing versus the manual 1 MiB CRT arena and `SRWLOCK`.
- **RISK:** Applying the “grow records” rationale to the fixed layout allocator hides a real capacity limit; applying fixed-table assumptions to the manual heap recreates false OOM.
- **HISTORY:** The manual table was made growable after metadata exhaustion could masquerade as guest-memory exhaustion.

### W55 — PlayStation outer-rim calibration preserves guest thresholds

- **WHAT:** Enter outer-rim normalization at magnitude 30,000, release at 29,500, and scale only that latched physical rim to the Xbox signed-16-bit radial maximum.
- **WHERE:** [xinput_device.c](src/input/xinput_device.c).
- **WHY:** Measured DualSense range stopped near 30,200 and jittered at the edge, preventing reliable full Xbox deflection.
- **EVIDENCE:** **Verified from current implementation**; measured-device rationale is explicit source commentary.
- **DEPENDENCIES:** SDL PlayStation path, per-port/per-stick latch and pre-mapping prompt activity.
- **RISK:** Applying a broad rescale/dead-zone rewrite changes authored movement thresholds and prompt/activity behavior.
- **HISTORY:** This is a host-controller correction; exact behavior of every PlayStation-compatible device remains **Unknown**.
