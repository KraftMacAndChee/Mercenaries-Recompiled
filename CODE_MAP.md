# Repository code map

Snapshot: 2026-09-29. Entries are **Verified from current implementation** unless explicitly marked otherwise. Follow symbols as well as paths: generated chunk numbers and line numbers change after regeneration. See [ARCHITECTURE](ARCHITECTURE.md) for evidence definitions.

## Start with the question

| Need to understand/change | Start here | Then follow |
| --- | --- | --- |
| Installation failure | [setup_launcher.c](ports/mercenaries/src/setup_launcher.c) validation/worker paths | Extractor exit, inventory, marker shortcut and XBE hash. |
| Runtime startup/memory mapping | [main.c](ports/mercenaries/src/main.c): `WinMain`, `host_graphics_init` | [xbox_memory_layout.c](src/kernel/xbox_memory_layout.c), exception/MMIO setup. |
| Wrong instruction result/missing call/stack drift | [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h), [lifter.py](tools/recomp/lifter.py) | [translator.py](tools/recomp/translator.py), discovery, generated body, [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py). |
| Import arguments/return/callback timing | [kernel_bridge.c](src/kernel/kernel_bridge.c) | [kernel_thunks.c](src/kernel/kernel_thunks.c), lower kernel module. |
| Guest allocation failure | [recomp_manual.c](ports/mercenaries/src/recomp_manual.c) | Layout allocator, title metadata, Lua/XMem fallback ownership. |
| Missing draw/render state/alias surface | [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c) | [nv2a_core.c](src/nv2a/nv2a_core.c), D3D resources/states/shaders. |
| FPS cap/stutter/adapter selection | [d3d8_device.c](src/d3d/d3d8_device.c) | [d3d8_frame_timing.h](src/d3d/d3d8_frame_timing.h), host service, options, guest clock patches. |
| Distorted scope/UI/world projection | [recomp_ui.c](ports/mercenaries/src/recomp_ui.c), [menu_layout_guest.h](ports/mercenaries/src/menu_layout_guest.h) | Manual camera hooks, generated sites, viewport/presentation; these are distinct transforms. |
| Silent dialogue/HQ softlock | [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), [voice_callback_queue.h](ports/mercenaries/src/voice_callback_queue.h) | Lua/voice lifecycle hooks, allocation failures, cue notifications, then APU output. |
| Audio volume/clicks/missing channels | [apu_core.c](src/apu/apu_core.c), [apu_vp.c](src/apu/apu_vp.c), [apu_dsp.c](src/apu/apu_dsp.c) | [apu_xaudio2.c](src/apu/apu_xaudio2.c), guest DSOUND/XACT commands. |
| Movie/XMV decode, cadence or volume | Generated XMV functions plus [xmv_fast_helpers.h](ports/mercenaries/src/recomp/xmv_fast_helpers.h) | [kernel_hal.c](src/kernel/kernel_hal.c) field pin, [apu_core.c](src/apu/apu_core.c) XGSCNT/output timing, movie checkpoints/volume hooks in [recomp_manual.c](ports/mercenaries/src/recomp_manual.c), and changing linear textures in PGRAPH. |
| Bindings/context/prompts | [recomp_controls.c](ports/mercenaries/src/recomp_controls.c) | [controls_mapping.h](ports/mercenaries/src/controls_mapping.h), [recomp_prompts.c](ports/mercenaries/src/recomp_prompts.c), [xinput_device.c](src/input/xinput_device.c). |
| Save path/time | [kernel_path.c](src/kernel/kernel_path.c), [kernel_file.c](src/kernel/kernel_file.c) | Bridge handles/layouts and [kernel_xbox.c](src/kernel/kernel_xbox.c) EEPROM. |
| OG Bugs scope | [recomp_original_bugs.c](ports/mercenaries/src/recomp_original_bugs.c) | [ai_boarding.h](ports/mercenaries/src/ai_boarding.h) also reads the option directly. |
| Mod capacity | [mod_compatibility.c](ports/mercenaries/src/mod_compatibility.c), [recomp_world_properties.c](ports/mercenaries/src/recomp_world_properties.c) | Memory initialization and generated pool relocation. |
| Birds/F9 features | [boids.c](ports/mercenaries/src/boids.c), [boids_guest.h](ports/mercenaries/src/boids_guest.h) | [dev_spawn.h](ports/mercenaries/src/dev_spawn.h), [boid_wings.h](ports/mercenaries/src/boid_wings.h). |
| Release/source contents | [Package-Preview.py](ports/mercenaries/scripts/Package-Preview.py) | [Archive-GeneratedProvenance.py](ports/mercenaries/scripts/Archive-GeneratedProvenance.py), [source_package_policy.py](tools/recomp/source_package_policy.py). |
| Tests/known failures | [run_tests.py](tools/recomp/run_tests.py) | [maintenance-validation.md](docs/maintenance-validation.md); CMake alone is insufficient. |

## Directory roles

| Location | Role |
| --- | --- |
| [CMakeLists.txt](CMakeLists.txt), [include/xbox/xboxrecomp.h](include/xbox/xboxrecomp.h) | Runtime build aggregation and umbrella interface. |
| [src/kernel](src/kernel) | Guest bridge, memory, paths/files, synchronization, HAL/EEPROM, crypto, logging. |
| [src/nv2a](src/nv2a) | GPU state, MMIO, pushbuffer interpreter and host PGRAPH. Replay/demo assets are not automatically title runtime inputs. |
| [src/d3d](src/d3d) | D3D8-shaped C API on D3D11; resources/states, shaders/cache, presentation. |
| [src/apu](src/apu), [src/audio](src/audio) | APU processing/output and supplementary DirectSound wrappers respectively. |
| [src/input](src/input), [src/platform](src/platform) | Controller backends/activity and platform wrappers. |
| [ports/mercenaries](ports/mercenaries) | Product build, generated integration, title policies, launcher, resources/data/assets. |
| [tools/xbe_parser](tools/xbe_parser), [tools/disasm](tools/disasm), [tools/func_id](tools/func_id), [tools/recomp](tools/recomp) | Offline analysis, discovery, lifting, validation, fixtures and oracles. |
| [tools/diagnostics](tools/diagnostics) | Development capture/replay/inspection; some tools mutate a live process. |
| [tools/debug](tools/debug), [tools/debug_symbols](tools/debug_symbols), [tools/symbols](tools/symbols), [tools/ghidra_naming](tools/ghidra_naming) | Debugging/name evidence tooling, not a second runtime. |
| [tools/linux](tools/linux), [tools/xmv](tools/xmv) | Platform support and movie analysis. |
| [templates](templates) | Recompilation templates, distinct from the title ABI header actually compiled. |
| [third_party](third_party), [src/licenses](src/licenses) | Dependencies, notices and source inventories; root licensing does not supersede component terms. |
| [docs](docs) | Runtime evidence, maintenance/release procedures and validation records. |
| Local artifacts/build/generated/extracted-data trees | Evidence and products, not a public-source inclusion policy. |

## Build/generation entry points

- [Build-From-Iso.ps1](ports/mercenaries/scripts/Build-From-Iso.ps1): extraction, generation, build and provenance orchestration.
- [Recompile.ps1](ports/mercenaries/scripts/Recompile.ps1): supported-XBE check, iterative seed/discovery passes, archival before replacement, reviewed function ranges/manual entries, lifting and patching.
- [__main__.py](tools/recomp/__main__.py): translator CLI. `BatchTranslator` coordinates functions; `FunctionTranslator` handles individual translations. [config.py](tools/recomp/config.py) installs executable-specific globals; concurrent unrelated inputs in one Python process are not an established interface.
- [Patch-Generated.py](ports/mercenaries/scripts/Patch-Generated.py): ordered `GeneratedPatch` operations and larger transformations. A behavioral build stage, not formatting.
- [Build.ps1](ports/mercenaries/scripts/Build.ps1), [port CMake](ports/mercenaries/CMakeLists.txt): Windows toolchain configuration and native target assembly.
- [Generate-Shader-Warmup.py](ports/mercenaries/scripts/Generate-Shader-Warmup.py), [Dev-Vehicle-Catalog.cmake](ports/mercenaries/scripts/Dev-Vehicle-Catalog.cmake), [Mission-Script-Fixes.cmake](ports/mercenaries/scripts/Mission-Script-Fixes.cmake): reviewed input-to-compiled catalog/script generation.
- [Archive-GeneratedProvenance.py](ports/mercenaries/scripts/Archive-GeneratedProvenance.py): generated evidence and maintained source archives.
- [Package-Preview.py](ports/mercenaries/scripts/Package-Preview.py): validated player payload, optional separate source delivery and package verification.

## Important structures and interfaces

| Structure/interface | Definition/use | Meaning |
| --- | --- | --- |
| Register globals, memory macros, `RECOMP_ICALL`, `RECOMP_ITAIL`, `XBOX_REP_MOVS` | [recomp_types.h](ports/mercenaries/src/recomp/recomp_types.h) | Guest ABI/instruction helpers. |
| Thunk table/bridge dispatcher | [kernel_thunks.c](src/kernel/kernel_thunks.c), [kernel_bridge.c](src/kernel/kernel_bridge.c) | Ordinal routes plus argument/handle/layout adaptation. |
| Allocation/mapping records | [xbox_memory_layout.c](src/kernel/xbox_memory_layout.c), [recomp_manual.c](ports/mercenaries/src/recomp_manual.c) | Backing, aliases, title metadata and fallback ownership are separate. The layout heap has a fixed 16,384-record table; the manual CRT heap starts at 16,384 records and grows under an `SRWLOCK`. |
| `NV2AState`, PGRAPH state | [nv2a_state.h](src/nv2a/nv2a_state.h), [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c) | GPU register/command state and its host realization. |
| `GuestColorSurface`, `GuestDepthSurface` | [nv2a_pgraph_d3d11.c](src/nv2a/nv2a_pgraph_d3d11.c) | Address/format/pitch, host objects, validity, dimensions and synchronization history. |
| D3D8-shaped COM-style C wrappers | [d3d8_xbox.h](src/d3d/d3d8_xbox.h), [d3d8_internal.h](src/d3d/d3d8_internal.h) | Host renderer interface, not complete desktop D3D8. |
| `MCPXAPUState` | [apu_state.h](src/apu/apu_state.h) | APU registers, VP/GP/EP state, monitor buffer, locks and worker coordination. It does not own the global supplementary software mixer. |
| `APUMixerVoice` | [apu.h](src/apu/apu.h), [apu_core.c](src/apu/apu_core.c) and audio consumers | Global supplementary-mixer slot borrowing PCM from an owner; access is not consistently locked. |
| `DSoundBuffer` | [dsound_device.c](src/audio/dsound_device.c) | Refcounted PCM owner. Release stops/frees the slot first, but `SetBufferData` currently replaces storage without equivalent synchronization. |
| Applied/pending option records | [recomp_options.c](ports/mercenaries/src/recomp_options.c) | Persisted/applied configuration versus staged menu changes. |
| Boid world/wing state | [boids.h](ports/mercenaries/src/boids.h), [boid_wings.h](ports/mercenaries/src/boid_wings.h) | Host motion/animation policy; guest adapter owns actor references. |

## Title integration files that are easy to miss

Many title `.h` files contain implementation with internal linkage and are included into `recomp_manual.c` in meaningful order. Moving them into separate translation units can change static state, macro visibility and cached environment settings.

| Files | Role |
| --- | --- |
| [voice_callback_queue.h](ports/mercenaries/src/voice_callback_queue.h) | Owns copied continuation names and protected-scope/serial cancellation. |
| [stream_admission.h](ports/mercenaries/src/stream_admission.h) | Title stream admission; follow manual/generated consumers before changing policy. |
| [ai_boarding.h](ports/mercenaries/src/ai_boarding.h), [carry_pickup_guest.h](ports/mercenaries/src/carry_pickup_guest.h), [cargo_helicopters.h](ports/mercenaries/src/cargo_helicopters.h) | Narrow interactions using guest object layouts. |
| [turret_camera_roll.h](ports/mercenaries/src/turret_camera_roll.h), [winch_camera_preview.h](ports/mercenaries/src/winch_camera_preview.h), [freecam_streaming.h](ports/mercenaries/src/freecam_streaming.h) | Camera integration/diagnostics, distinct from general render matrices. |
| [dev_player_actor.h](ports/mercenaries/src/dev_player_actor.h), [dev_spawn.h](ports/mercenaries/src/dev_spawn.h), [dev_mission_guest.h](ports/mercenaries/src/dev_mission_guest.h), [dev_battle_guest.h](ports/mercenaries/src/dev_battle_guest.h) | Guest-safe execution of host developer requests. Player lookup is shared. |
| [dev_menu.c](ports/mercenaries/src/dev_menu.c), [dev_overlay.c](ports/mercenaries/src/dev_overlay.c), [free_cam.c](ports/mercenaries/src/free_cam.c), [dev_missions.c](ports/mercenaries/src/dev_missions.c) | Host UI, request and camera side. |
| [ps2_upgrades.c](ports/mercenaries/src/ps2_upgrades.c), [ps2_retail_mix.h](ports/mercenaries/src/ps2_retail_mix.h), [audio_bank_fixes.c](ports/mercenaries/src/audio_bank_fixes.c) | Optional bank/art additions and targeted sound-bank corrections. |
| [mission_script_fixes.c](ports/mercenaries/src/mission_script_fixes.c), [wmd_inspectors.lua](ports/mercenaries/src/wmd_inspectors.lua) | Fingerprint-selected project correction appended to the matching retail script. |
| [chapter_return.c](ports/mercenaries/src/chapter_return.c), [shop_config.c](ports/mercenaries/src/shop_config.c) | Title progression-return coordination and shop configuration. |
| [shader_warmup.c](ports/mercenaries/src/shader_warmup.c) | Catalog-based precompilation; first-use compilation remains. |

## Validation navigation

[run_tests.py](tools/recomp/run_tests.py) discovers `tools/**/test_*.py` by AST and runs isolated subprocesses, choosing pytest or direct script execution. Native fixtures, retail oracles, source-spelling checks, WARP rendering and live-game tests establish different things. [MAINTENANCE](MAINTENANCE.md) describes which evidence is needed for each subsystem. A developer utility or fixture can explain a decision without being a runtime/build dependency.
