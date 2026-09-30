# Subsystems, interfaces and ownership

Snapshot: 2026-09-29. Unless separately labelled, the descriptions below are **Verified from current implementation**. Source references identify implementation, not merely suggestive filenames. See [ARCHITECTURE](ARCHITECTURE.md) for the whole system and [WORKAROUNDS](WORKAROUNDS.md) for historical reasons behind unusual behavior.

## 1. Offline executable analysis and generation

**Purpose/interfaces.** [Recompile.ps1](ports/mercenaries/scripts/Recompile.ps1) drives XBE parsing, disassembly, function discovery, seed merging, `tools.recomp`, and generated patching. [translator.py](tools/recomp/translator.py) owns `BatchTranslator`/`FunctionTranslator`; [lifter.py](tools/recomp/lifter.py) translates instructions/control flow. Reviewed owner ranges keep interior switch targets and shared epilogues in the proper guest frame.

**State/lifecycle.** Python objects own decoded instructions, function databases and emitted text. [config.py](tools/recomp/config.py) also installs executable-layout globals. Discovery converges on ordered seed content/hash, with a bounded pass count; equal counts alone are not convergence. Before replacing existing generated files, the script archives their provenance. It then removes/recreates generated output and applies [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py).

**Consumers/dependencies.** Port CMake consumes generated C; runtime/manual hooks consume the ABI emitted around it. Inputs include the exact retail XBE, reviewed seeds/ranges and maintained patch logic. Generated chunks are not an independent authoritative source implementation.

**Cleanup/coupling.** Offline processes release ordinary Python state on exit. Generation and patching are not all-or-nothing filesystem transactions: the patch script can write headers before all later validation succeeds. A failed run requires inspecting the tree before building. Changes to lifting, discovery, ABI or patch matching can affect unrelated gameplay through shared guest instructions.

## 2. Guest execution ABI and manual integration

**Purpose/interfaces.** [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h) supplies registers, guest memory access, x87/SSE/MMX helpers, calls/tails and string operations. [recomp_manual.c](ports/mercenaries/src/recomp_manual.c) supplies explicit replacements, hooks, title allocation policies and implementation-header integration.

**State/ownership.** Integer registers and floating-point/SIMD state are process globals. Generated functions generally have `void(void)` signatures; guest arguments/returns live in registers/guest stack. The guest stack is independent of the native C stack. Manual helpers that re-enter guest code must preserve the correct guest context, not merely host callee-saved registers.

**Execution.** `RECOMP_ICALL` resolves manual before generated before kernel and preserves nonvolatile register values around calls. Some bridge startup dispatch resolves generated before manual and has no kernel fallback. `RECOMP_ITAIL` has different return-address behavior. The safe-call form restores saved stack state on failure. Its successful-call mismatch tracing is deliberately narrow (low-address stack collapse or the named `sub_00069550` path), not a general stack validator. The guarded invalid-address range and unknown-import behavior are compatibility policies, not a general memory-safety guarantee.

**Lifecycle/dependencies.** Main publishes the mapping and initializes `g_esp` before guest entry. Guest functions may access mapped code bytes even though native C executes them. There is no independent guest CPU instance teardown: state is process-global. Header implementation order and generated call sites are part of the interface.

## 3. Memory and allocation domains

**Files/API.** [xbox_memory_layout.c](src/kernel/xbox_memory_layout.c), [xbox_memory_layout.h](src/kernel/xbox_memory_layout.h), [kernel_memory.c](src/kernel/kernel_memory.c), [kernel_pool.c](src/kernel/kernel_pool.c), and title allocation code in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c).

**Backing and addresses.** A pagefile-backed mapping supplies normally 64 MiB of RAM; optional graphics-mod configuration selects 128 MiB before initialization. The placement logic searches for a suitable guest virtual-address arena; this does not commit four GiB of RAM. Address translation adds the guest unsigned 32-bit address to a host offset. Demand-mapped CPU aliases, the GPU aperture, RAMIN and a synthetic kernel image have separate ownership.

**Layout.** The title guest stack is based at `0x00880000` with 256 KiB; kernel data begins at `0x0087E000`; the title heap begins at `0x008C0000`. The synthetic kernel at `0x80010000` and surrounding mapping/fence behavior participate in import resolution. These addresses are ABI, not suggestions for host allocator placement.

The loader intentionally does not protect `.rdata` read-only: `.rdata` end `0x003B2454` and `.data` start `0x003B2360` share host page `0x003B2000`. Protecting that page would also protect writable globals. The contrary read-only comment in `kernel_bridge.c` is stale.

**Allocation ownership.** Distinguish host malloc/VirtualAlloc storage, guest layout heap blocks, the retail title's ordinary allocation path, its existing main pool, and optional world-property/graphics pools. There are two easily confused record limits: the layout heap uses a fixed `XBOX_HEAP_MAX_ALLOCS = 16384` array, while the manual replacement CRT heap starts with 16,384 records and doubles host bookkeeping under an `SRWLOCK` up to its address-space-derived cap. Lua and a reviewed whitelist of nonphysical XMem audio allocations try the normal title path first and use the existing main pool on failure. Ownership records determine the matching free/reallocation route; a fallback pointer cannot be returned to the wrong allocator.

**Lifetime/coupling.** All guest consumers borrow the backing memory. Layout shutdown unmaps its CPU aliases/fence/kernel/base then closes the backing mapping; it does not represent a complete NV2A aperture/RAMIN teardown. Extended memory is initialized before graphics/APU and cannot be casually changed live. Extra physical capacity, bigger allocator metadata and larger title object tables solve different limits.

## 4. Kernel imports and Xbox services

**Files/API.** [kernel_bridge.c](src/kernel/kernel_bridge.c), [kernel_thunks.c](src/kernel/kernel_thunks.c), [kernel.h](src/kernel/kernel.h), and lower modules for RTL, HAL, Xbox configuration, objects, I/O and crypto.

**Role.** The bridge converts 32-bit guest arguments, layouts, callbacks and handles into native representations. The ordinal table routes to implementations. The dispatch path services pending work, accounts for the guest dummy return and stack arguments, and returns results in guest registers. Unimplemented paths can log once and return a compatibility result; this is not full kernel emulation.

**Ownership.** Bridge-side handle associations connect guest handles to host objects. Lower wrappers own their allocated host objects until matching close/release. Global thunk/scheduling state is initialized before guest entry. Kernel shutdown closes logging and synchronization state and clears tables; it is not a sweep of all potentially surviving application handles.

**Non-file services.** RTL routines operate on translated structures; HAL/configuration provides the subset of hardware/EEPROM information the title uses. Current SHA-1, RC4 and HMAC logic in [kernel_crypto.c](src/kernel/kernel_crypto.c) uses software implementations despite older BCrypt-oriented comments. Other public-key/object/I/O paths must be reviewed individually; broad module comments do not prove their completeness. Some I/O wrappers report success with zero information rather than performing a real device operation.

**Consumers/coupling.** Generated imports, manual guest calls and audio scheduling use the bridge. Host-sized structures or Windows access semantics must not leak into guest layouts unexamined.

## 5. Filesystem and installation

**Runtime files.** [kernel_path.c](src/kernel/kernel_path.c) translates guest device/drive roots to game/save directories selected by main. Roots use UTF-8-to-wide conversion; guest path remainders use the Windows code-page conversion path. Unknown paths can fall through to host interpretation with a warning: this is path compatibility, not a filesystem security sandbox.

[kernel_file.c](src/kernel/kernel_file.c) owns host file/directory operations and a bounded read cache. Eligible handles have two 8 MiB forward windows plus up to sixteen 128 KiB recent-request copies, across at most sixteen cache contexts. A single critical section protects all cache contexts and is currently held across the synchronous refill `ReadFile`; this serializes eligible cached reads across handles. Writes invalidate cached data. Close tears down file-cache/directory state and closes the host handle. The bridge separately manages guest completion/APC semantics. `GENERIC_ALL` is mapped to useful data/delete access rather than blindly requesting host ACL-ownership privileges.

`FILE_NO_INTERMEDIATE_BUFFERING` is intentionally not mapped to Win32 unbuffered I/O. It acts as the eligibility hint for this host cache because physical-sector synchronous reads caused large stalls on ordinary HDDs. This is a host-performance adaptation, not Xbox cache emulation.

**Installer.** [setup_launcher.c](ports/mercenaries/src/setup_launcher.c) owns extraction locking, worker process, progress/cancellation and staging activation. A full validation checks required files/directories, inventory counts/sizes and XBE hash. The ordinary already-installed marker path checks directory/marker presence, not the full inventory or every content hash on every launch.

The worker extracts to an owned stage, validates, writes a marker and moves it into place. It can move an old installation aside first. This is not a rollback-atomic transaction across both moves: activation failure can leave the previous installation in its backup location. Cancellation targets the owned extractor and waits for it. User saves/settings and source archives have separate lifecycles.

## 6. Scheduling, synchronization and time

**Files/API.** [kernel_bridge.c](src/kernel/kernel_bridge.c), [kernel_thread.c](src/kernel/kernel_thread.c), [kernel_sync.c](src/kernel/kernel_sync.c), main host service/tick updater, and [d3d8_frame_timing.h](src/d3d/d3d8_frame_timing.h).

**Guest scheduling.** The bridge has fixed capacities of 16 connected guest interrupts, 64 guest timers and 256 queued guest DPCs. DPC insertion rejects duplicates without replacing the first arguments; removal occurs before invoking a callback, allowing self-requeue. Delivery observes ISR/IRQL constraints and a 256-delivery safe-point budget. Host timer progression records pending work; guest-safe checkpoints execute it. Alertable guest delay can deliver queued completion APCs.

**Threads.** The bridge `PsCreateSystemThreadEx` startup path executes guest work synchronously, with special first-thread setup and later explicit context preservation. Lower kernel code also wraps native thread creation. No general preemptive multi-CPU guest scheduler is established. Native APU/tick/logging workers are real host concurrency and require their own locks/atomics.

**Timing.** Xbox performance-counter queries rescale host QPC to the title-visible 733,333,333 Hz CPU timebase, splitting whole seconds/remainder to avoid overflow. Guest clock, presentation cadence, audio sample clock and 60 Hz vblank are not one clock. FPS Cap supports 30/60/90/120/uncapped; fractional carry preserves the title's 3000-unit timebase at rates where integer truncation loses time. It is inserted at a selected main-loop path, not every use of a time value.

The AV `GUESS_FIELD` compatibility path toggles parity on each query rather than deriving wall-clock parity. This specifically prevents below-60-Hz polling from observing the same parity after two elapsed fields and stalling XMV.

**Cleanup.** Audio/tick worker joins are explicit on normal shutdown. Closing the window may terminate the process instead. A lock in one subsystem is not proof that guest globals or renderer state are thread-safe.

## 7. NV2A commands and MMIO

**Files/API.** [nv2a_core.c](src/nv2a/nv2a_core.c), [nv2a_state.h](src/nv2a/nv2a_state.h), [nv2a_mmio_hook.c](src/nv2a/nv2a_mmio_hook.c), [nv2a_regs.h](src/nv2a/nv2a_regs.h).

**Initialization/ownership.** Main calls `nv2a_hook_init` with the guest offset and RAM mapping. It creates RAMIN/aperture integration and standalone GPU state. The Windows exception path recognizes supported faulting host load/store forms, dispatches MMIO accesses and advances the saved instruction pointer. Unsupported faults continue through exception handling; they are not universally turned into zero reads.

**Runtime.** PFIFO PUT changes synchronously process a bounded pushbuffer stream: method headers, increments/nonincrements, jumps and subroutine control. Nonincrementing inline batching is restricted by method/state eligibility. Methods update NV2A state and enter PGRAPH/image-blit handling.

**Dependencies/cleanup.** GPU memory accesses borrow guest RAM. The host D3D device/PGRAPH are initialized separately. MMIO setup can log failures without returning a rich ownership object and has no matching comprehensive public teardown. Non-Windows hook branches are inactive; their existence is not equivalent hardware support.

## 8. PGRAPH and graphics resources

**Files/API.** [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c), [nv2a_image_blit.h](src/nv2a/nv2a_image_blit.h), [d3d8_resources.c](src/d3d/d3d8_resources.c), [d3d8_states.c](src/d3d/d3d8_states.c).

**State/ownership.** Global PGRAPH state owns fixed caches of 32 guest color surfaces and 16 guest depth surfaces, plus textures and translated shader state, while borrowing the host device. `GuestColorSurface`/`GuestDepthSurface` contain address, format, pitch, logical/physical dimensions, host views/resources and validity/write/draw/download history. The current lookup is offset-first: an incompatible pitch/format/AA/size causes the record at that offset to be released and recreated. Thus address alone is insufficient to decide reuse/current contents, but the implementation does not maintain an unrestricted composite-key map of simultaneous same-offset layouts.

**Runtime.** Draw paths translate guest vertex/index data, render state, textures and shaders into D3D calls. Surface aliases, AA resolves, CPU writes and explicit NV062/NV09F copies require synchronization rules. Depth and stencil can independently require a depth/stencil view. Texture cache identity includes format/pitch/rectangle state; eviction releases owned host objects. Deferred work and scanout interact with flip handling.

Authentic haze is a renderer special case: a recognized four-vertex retail glow sequence is copied through one 640x480 and two 320x240 ping-pong surfaces, then copied back to the scaled target. This keeps the retail bilinear sample spacing instead of magnifying separated ghost images at higher internal resolution.

**Cleanup.** PGRAPH shutdown releases caches/surfaces and unbinds targets. External registrations distinguish host textures from borrowed pixel records; some records survive for process lifetime. `d3d8_BindExternalTexture` can change cached texture-stage COLOROP from disabled to modulate; passing null is not a symmetric restoration of every previous state.

**Coupling.** Generic D3D APIs and title draw repairs share this mutable state. A seemingly local surface heuristic can affect sky, shadows, satellite UI and movie compositing. Diagnostics must capture completed frames, not assume a discarded swapchain buffer still contains them.

## 9. Host device, presentation and shaders

**Files/API.** [d3d8_device.c](src/d3d/d3d8_device.c), [d3d8_internal.h](src/d3d/d3d8_internal.h), [d3d8_shaders.c](src/d3d/d3d8_shaders.c), [d3d8_vsh.c](src/d3d/d3d8_vsh.c), [d3d8_combiners.c](src/d3d/d3d8_combiners.c).

**Device/presentation.** DXGI Factory6 enumeration prefers high-performance hardware in OS order, skips software, and tries flip and legacy creation for a candidate before moving on. Fallback paths remain. Guest output dimensions, internal render size and host window/backbuffer are distinct. CPU-frame upload creates a source-sized texture and scales/copies it to the backbuffer; it does not upload a small source into a larger destination with an implicit full box.

`FLIP_STALL`, pending scanout service and host frame slots cooperate. `ServiceDisplayRefresh` uses refresh timing and pending-frame state; it does not repeatedly present a discarded old frame as new guest output. The message pump's `WM_QUIT` calls `ExitProcess`.

**Shaders.** Fixed-function HLSL, translated NV2A vertex programs and register-combiner pixel shaders are separate producers. [d3d8_compiler.c](src/d3d/d3d8_compiler.c) centralizes compiler selection; Wine uses the bundled native compiler path while Windows uses the normal compiler. [d3d8_shader_cache.c](src/d3d/d3d8_shader_cache.c) keys bytecode by full source/profile/version, validates its header/size/exact EOF and removes corrupt entries. Cached bytecode still requires host device object creation.

[shader_warmup.c](ports/mercenaries/src/shader_warmup.c) registers catalogs before device initialization, compiling without binding live draw state. First-use fallback remains. [d3d8_scaled_lines.c](src/d3d/d3d8_scaled_lines.c) and [d3d8_triangle_depth.c](src/d3d/d3d8_triangle_depth.c) handle primitive-specific compatibility; state restoration matters.

**Lifetime/limits.** Device wrappers use COM-like reference counting, but compiler module/cache helpers include process-lifetime state. Some D3D8-shaped methods return `E_NOTIMPL`; others return success without filling output. This is the title's renderer support layer, not a complete desktop D3D8 implementation.

## 10. APU, voices and output

**Files/API.** [apu_core.c](src/apu/apu_core.c), [apu_state.h](src/apu/apu_state.h), [apu_vp.c](src/apu/apu_vp.c), [apu_dsp.c](src/apu/apu_dsp.c), [apu_mmio_hook.c](src/apu/apu_mmio_hook.c), [apu_xaudio2.c](src/apu/apu_xaudio2.c).

**Ownership/init.** `MCPXAPUState` owns APU registers, VP/GP/EP state, monitor buffer, timing fields, lock/conditions and worker coordination; it borrows guest RAM. The supplementary DirectSound mixer voices, active count and critical section are separate globals in `apu_core.c`. Initialization establishes VP, DSP, that global mixer, output and the frame worker, then resumes the worker through a lock/condition handshake.

**Runtime.** The APU worker operates under the APU lock, releasing it while waiting. Hardware voice processing stops when SECTL is off/trapped/halted. Without active software/test audio, idle handling waits rather than inventing monitor-clock progress. Active processing works in 32-sample units; eight units form a 256-sample output block. Worker results latch guest interrupt state for safe delivery elsewhere.

DSP56300 state/library integration exists, but guest DSP execution is opt-in in the default path. The normal fallback downmix preserves selected authored VP speaker-bin contributions; GP intermediates are not simply played as stereo. Software/movie/test sources can also feed the host mixer.

**Output/lifetime.** The monitor prefers XAudio2 and falls back to waveOut, using 48 kHz signed 16-bit stereo and owned ring-buffer copies. Borrowed voice PCM must remain alive until the voice stops. On Windows initialization calls `timeBeginPeriod(1)`; the source states that default wake granularity made 5.333 ms APU batches late and XGSCNT roughly 20 percent slow, starving the audio-mastered XMV clock. Normal shutdown calls `timeEndPeriod(1)`, requests worker exit, joins it, finalizes voice workers/output/DSP and then frees state. Do not free backing RAM first.

**Limits.** Correct cue flags do not establish correct decoded PCM or audibility. Experimental DSP/EP paths and the default mix have different fidelity claims; test them separately. The global supplementary mixer is not reset/destroyed on APU shutdown, which is another reason in-process restart is not supported.

## 11. DirectSound-shaped wrappers and title audio lifecycle

[dsound_device.c](src/audio/dsound_device.c) implements real PCM-buffer/mixer integration as well as incomplete operations. Its introductory all-stub description is stale. `DSoundBuffer` owns allocated PCM and a mixer slot; final release stops/frees the slot before freeing PCM.

That release order is not enforced by every mutation. `SetBufferData` frees the old PCM first and then writes a borrowed `APUMixerVoice *` without taking the mixer lock; allocation failure can leave the voice pointing at freed storage. `apu_mixer_get_voice`, play/stop and render are also not consistently protected by the allocation/free critical section. Treat concurrent replacement/playback as a known current hazard, not a supported ownership contract. Stream/device operations require per-method review.

The retail generated DSOUND/XACT engine remains important; title audio is not entirely routed through these wrappers. [recomp_manual.c](ports/mercenaries/src/recomp_manual.c) and generated hooks handle cue allocation, prepared-wrapper retirement, EOF packet readiness, terminal STOP recovery and HQ continuation. [voice_callback_queue.h](ports/mercenaries/src/voice_callback_queue.h) owns copied callback names, nested protected-scope state and serial cancellation. Records are unlinked before dispatch, allowing reentrancy without replaying the same node.

Fallback STOP intent records are bounded and identity-checked; the recovery writes an ordinary notification after terminal conditions, not a timer-triggered script success. Lua callback failure recovery is scoped to owner VM/scene/error and teardown. These structures rely on guest execution context and must not be moved casually to the audio worker.

## 12. XMV/movie playback

The retail XMV decoder remains primarily generated guest code. [xmv_fast_helpers.h](ports/mercenaries/src/recomp/xmv_fast_helpers.h) replaces extremely hot decoded leaf operations, while generated patches install movie diagnostics and the functional Music-category volume hook. PGRAPH handles the decoder's changing linear YUV textures and the normal CPU/movie source-sized upload path.

Movie cadence crosses multiple subsystems. XGSCNT is an APU sample counter, not a wall clock; the APU timer-period workaround keeps that clock advancing near its intended rate. HAL's query-driven alternating AV field pin lets the retail present path advance its field counter. Main's display-refresh service also increments title-specific guest state at `0x00793564` and conditionally updates `*(0x00299378) + 0x1DE8`. These mechanisms are related but not interchangeable.

The exact rationale for every movie checkpoint/trace hook is historical or diagnostic. Do not infer that every checkpoint is functional. The hot helpers, volume application, field behavior, sample clock and renderer texture path are functional current behavior.

## 13. Input, bindings, prompts and options

**Backends.** [xinput_device.c](src/input/xinput_device.c) combines XInput, SDL/DualSense and the title mapper. Activity detection occurs before remapping. Controller presence alone does not define prompt mode. Backend merging handles overlapping devices/stale axes; keyboard mapping also works without a connected pad.

PlayStation/SDL sticks receive only an outer-rim host calibration: magnitude 30,000 enters and 29,500 releases a latch that scales to the Xbox signed-16-bit rim. Values below that physical rim—and therefore guest-authored dead zones/thresholds—are preserved. Per-thread environment lookups use a fixed 64-entry launch-configuration cache; changes after a name has been cached are not observed.

**Title mapping.** [recomp_controls.c](ports/mercenaries/src/recomp_controls.c) loads executable-relative INI bindings, preserves existing explicit profiles, maps real gameplay/menu contexts, and accumulates raw mouse input through the window path. [controls_mapping.h](ports/mercenaries/src/controls_mapping.h) provides binding resolution used by [recomp_prompts.c](ports/mercenaries/src/recomp_prompts.c). Rebinding snapshots held inputs and waits for their release individually; an unrelated held input does not permanently block all new keys.

**Contexts.** Support selection, support-item addition and the HVT award screen are not one context. Selection permits movement cancellation; modal addition captures confirmation while suppressing inherited held-fire activation. A gamepad working where KB+M fails can identify a mapper/context gap rather than a broken guest menu.

**Options/state.** [recomp_options.c](ports/mercenaries/src/recomp_options.c) separates pending/applied state and persists its own keys. [recomp_options_menu.c](ports/mercenaries/src/recomp_options_menu.c) exposes the menu; callbacks apply graphics changes. FPS choices are indexed `[30,60,90,120,0]`, with legacy 60-FPS compatibility handling. Binding ownership is separate; saving video options must not reset user controls. Some world/object-distance behavior is latched rather than immediately reconstructing the world.

## 14. Title policies, optional content and developer features

**OG Bugs/scripts.** [recomp_original_bugs.c](ports/mercenaries/src/recomp_original_bugs.c) gates music selection, dateline typing and cloud transition fixes; [ai_boarding.h](ports/mercenaries/src/ai_boarding.h) independently gates helicopter boarding-height behavior. [mission_script_fixes.c](ports/mercenaries/src/mission_script_fixes.c) adds a WMD-inspector correction only to a length/hash-matched retail script; changed scripts are left alone. The caller owns the new buffer. This fix is not automatically part of OG Bugs.

**PS2 additions.** [ps2_upgrades.c](ports/mercenaries/src/ps2_upgrades.c) identifies supported bank/texture inputs, appends supported content and selects additions through the option. Stable guest allocations and registered host pixel data have deliberately long lifetimes. A bank may remain extended when the option is off; selection and resource existence are different. The artwork replacement's failure path deserves ownership review: an allocated guest block can be retained when later registration fails. Exact parity/legal redistribution are separate evidence questions.

**Mods.** [mod_compatibility.c](ports/mercenaries/src/mod_compatibility.c) reads two default-off startup choices. Graphics expansion changes backing/pool capacity; [recomp_world_properties.c](ports/mercenaries/src/recomp_world_properties.c) independently expands the permanent property table. The latter translates the retail 40,000-entry range to 262,144 records, preserving one-past pointer arithmetic but rejecting writes beyond capacity.

**Developer UI.** [dev_menu.c](ports/mercenaries/src/dev_menu.c), [dev_overlay.c](ports/mercenaries/src/dev_overlay.c), [free_cam.c](ports/mercenaries/src/free_cam.c) and guest adapters separate host requests from execution inside the title's world update. F9/F11 are settings-gated. Missions/spawning can mutate campaign/world state; diagnostic names do not imply read-only behavior. Free-camera position also affects streaming and input/time policy.

**Birds.** [boids.c](ports/mercenaries/src/boids.c) owns the independent fixed-step movement model; [boids_guest.h](ports/mercenaries/src/boids_guest.h) tracks guest actors and rendering/animation integration. Creation is explicit; reload/province changes do not silently respawn the flock. [boid_wings.h](ports/mercenaries/src/boid_wings.h) controls flap/glide phases and restores saved track rates. Bird-specific flags must remain scoped to bird actors, not all NPCs. **Historically documented:** this is an accepted approximate replacement, not exact reproduction of the former flock trajectories.

## 15. Diagnostics, validation and packaging

[preview_log.c](src/kernel/preview_log.c) owns bounded asynchronous preview logging, rotation and recent-history/summary behavior. Logging is default-off; F8 records a marker, not a screenshot. Crash dumps, one-time warnings, verbose traces and targeted fault injection have different semantics. Process-lifetime environment caching can make a setting change ineffective after initialization.

[run_tests.py](tools/recomp/run_tests.py) discovers without importing tests, creates fresh output directories, isolates subprocesses and records timeout/nonzero results. Tests can compile production fragments, compare generated text, run retail oracles or render with host APIs; none alone proves the whole game.

[Package-Preview.py](ports/mercenaries/scripts/Package-Preview.py) checks the executable hash against validation evidence, verifies provenance archives, verifies maintained source policy, stages an explicit player payload, checks ZIP CRC/member hashes, runs clean launcher/extractor checks and only then renames the partial ZIP. It does not itself establish gameplay correctness. [source_package_policy.py](tools/recomp/source_package_policy.py) rejects forbidden paths/types/magic and unsafe links, with tightly identified fixture exceptions; it is not a proof of complete provenance or licensing.

## 16. Build dependencies and dependency boundaries

**Verified from current implementation.** [Build.ps1](ports/mercenaries/scripts/Build.ps1) selects Visual Studio 17 2022, x64, Windows SDK 10.0.26100.0 and MSVC 14.44.35207. It expects CMake in the repository Python environment and already generated `recomp_dispatch.c`. The port uses C for most runtime code, C++ for selected launcher art, and Windows resources; a C++ compiler alone does not recreate retail inputs or generated code.

| Dependency | Active role | Boundary/lifetime |
| --- | --- | --- |
| xemu-derived maintained APU/NV2A adapters | Compiled runtime hardware compatibility | Local source is compiled directly; an external emulator process is not the normal execution path. Preserve per-file notices and [inventory](src/licenses/xemu-source-inventory.json). |
| DSP56300 v0.1.3 MSVC x64 library and patched source | Linked DSP implementation/FFI | MSVC build requires the pinned headers/library; execution is separately gated. Source/Cargo.lock support rebuilds, but transitive Rust packages are not all vendored offline. |
| SDL2 2.32.10 | Controller support and platform branches | Runtime DLL is explicitly staged; inclusion does not imply all native POSIX branches work. |
| xdvdfs 0.8.3 | User ISO extraction | Child tool used by installer/setup, not guest gameplay execution. Missing runtime prerequisites can fail before ISO contents are processed. |
| Windows SDK shader compiler | HLSL compilation, including Wine support | Central wrapper selects/retains the compiler module; package includes the compatibility DLL/notices. |
| Windows APIs and D3D11/DXGI | Window/device/files/events/clocks/audio services | Host facilities adapted through compatibility code, not a complete Xbox OS. |
| Python/Capstone/CMake/Ninja build dependencies | Analysis, generation, configuration and tests | Version inputs live in [requirements.txt](ports/mercenaries/requirements.txt); not player gameplay dependencies. |
| Lua 5.0.3 | Host-side test/reference interpreter | Not the game's runtime Lua engine; the retail runtime remains lifted guest code. |
| Launcher fonts/art and optional prompt/PS2 assets | Compiled resources and optional title content | Resource presence and user supply are separate from proof of redistribution rights. |

[Dependency inventory](docs/dependencies.md) records notices and exact local locations. Its pinned public xemu comparison revision is not asserted to be the historical import commit. A maintained-source package must include appropriate source/notices, not just copy binary dependencies and assume the root license covers them.
