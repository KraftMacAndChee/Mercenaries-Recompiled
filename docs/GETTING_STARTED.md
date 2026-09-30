# Getting started with Xbox static recompilation

This guide describes analysis and integration work for a **new title**. It is not
an automatic ISO-to-playable-game recipe. For Mercenaries, use the supported
[port build guide](../ports/mercenaries/README.md); its scripts perform additional
retail-image checks, discovery passes, patches, and build steps.

## Working directory and prerequisites

Run the PowerShell commands below from the root of **this repository checkout**,
where `tools`, `src`, and `ports` are present. Keep that working directory through
the analysis steps so Python can import `tools`. Use a separate checkout when
experimenting with another title, and keep its inputs and outputs separate from
the Mercenaries generated tree.

For the repository's Windows toolchain baseline, install Python 3.12 through the
`py` launcher and Visual Studio 2022 with MSVC 14.44.35207 and Windows SDK
10.0.26100.0. Create the pinned environment using:

```powershell
& .\ports\mercenaries\scripts\Setup-Python.ps1
```

This installs the versions of Capstone, CMake, and Ninja recorded in the
[requirements file](../ports/mercenaries/requirements.txt). It does not extract a
game or create a working port. The shared runtime can be built separately:

```powershell
& .\.venv\Scripts\cmake.exe -S . -B build/toolkit -G 'Visual Studio 17 2022' -A 'x64,version=10.0.26100.0' -T 'v143,version=14.44.35207,host=x64'
& .\.venv\Scripts\cmake.exe --build build/toolkit --config Release --parallel 4
```

The root CMake project builds runtime libraries, not a game executable. Other
titles need their own integration and validation even when these libraries build.
An emulator/debugger and disassembler are useful for checking behavior and guest
state against the original executable.

## 1. Prepare and identify the executable

Extract your own Xbox disc image with a compatible Xbox filesystem extractor.
For the examples below, place the extracted `default.xbe` and its game data under
`game_files/new-port/`. Use a new directory for a different executable revision.

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath .\game_files\new-port\default.xbe
& .\.venv\Scripts\python.exe -m tools.xbe_parser game_files/new-port/default.xbe --json game_files/new-port/title_analysis.json
```

Record the XBE hash, decoded entry point, section layout, imports, and library
versions with your port's inputs. Import counts vary by executable; they are not
a fixed count of implemented kernel APIs. The runtime parses section addresses,
but stack/heap placement and synthetic kernel data still need a title-specific
review. Do not copy another game's addresses without establishing their meaning.

## 2. Disassemble and identify functions

Pass the analysis file explicitly and put output in an isolated directory:

```powershell
& .\.venv\Scripts\python.exe -m tools.disasm game_files/new-port/default.xbe --analysis-json game_files/new-port/title_analysis.json --text-only --output artifacts/new-port/disasm -v
& .\.venv\Scripts\python.exe -m tools.func_id game_files/new-port/default.xbe --functions artifacts/new-port/disasm/functions.json --strings artifacts/new-port/disasm/strings.json --xrefs artifacts/new-port/disasm/xrefs.json --output artifacts/new-port/func-id -v
```

Disassembly produces function, label, string, cross-reference, and summary JSON
files plus assembly listings. Identification adds classifications and candidate
names. These are analysis results, not proof of complete function coverage or
correct calling conventions. A trivial function can still have an important
return value or stack effect; classification as a stub is not permission to
ignore it.

`--text-only` is an initial pass, not a completeness check. Inspect the XBE's other
sections for executable code and review function boundaries, indirect targets,
shared epilogues, and entry seeds before relying on a translation. The disassembler
supports `--extra-sections`, `--seed-functions`, and `--protected-seed-functions`;
use its `--help` and the current source for their input formats.

## 3. Generate an initial C tree

Keep every analysis input tied to the same XBE. Explicit directories prevent a
previous title's default tool output from being picked up accidentally:

```powershell
& .\.venv\Scripts\python.exe -m tools.recomp game_files/new-port/default.xbe --all --split 1000 --disasm-dir artifacts/new-port/disasm --func-id-dir artifacts/new-port/func-id --abi-dir artifacts/new-port/abi --gen-dir artifacts/new-port/gen --output-dir artifacts/new-port/recomp
```

The initially absent `abi_functions.json` produces a warning; `--abi-dir` points
to this title's own location so unrelated optional ABI data is not reused. That
warning does not establish the title's ABI correctness. Review register, stack,
flag, and calling-convention behavior before execution. Keep the binary-consistency
check enabled, and retain your own input hashes: matching filenames alone do not
establish matching content.

Split output includes `recomp_0000.c` and subsequent chunks,
`recomp_dispatch.c`, and `recomp_funcs.h`; additional generated support files
may accompany them. The initial discovery can miss functions and data-driven
entrypoints. Inspect unresolved targets and diagnostics before building.

Treat generated code as disposable. Durable fixes belong in maintained translator
code, reviewed seeds, port overrides, or a reproducible patch stage. For the
existing Mercenaries port, that stage is
[Patch-Generated.py](../ports/mercenaries/scripts/Patch-Generated.py), orchestrated
by [Recompile.ps1](../ports/mercenaries/scripts/Recompile.ps1). Do not apply its
title-specific patches to a different executable.

## 4. Integrate a host executable

Use [templates/new-game](../templates/new-game) as scaffolding, and compare it with
the maintained [Mercenaries entrypoint](../ports/mercenaries/src/main.c) and
[port CMake file](../ports/mercenaries/CMakeLists.txt). Templates contain placeholders
and incomplete hardware integration; copying them does not produce a ready-to-run
port. Resolve the template's source/header paths, placeholder entrypoint, and
required host callback definitions for your generated tree.

The dispatch type is a function-pointer **typedef**, not a variable declaration:

```c
#include <stdint.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
```

These are interface declarations only, not a startup program. Real generated and
manual dispatch implementations must supply the symbols. Use the port's ABI
header in production rather than duplicating its declarations.

Before invoking a translated entrypoint, the host integration must establish:

1. A verified executable and successful guest-memory initialization, with failures
   handled before guest access. Set `g_xbox_mem_offset` from
   `xbox_GetMemoryOffset()`; guest addresses are not ordinary host pointers.
2. Mapped guest stack storage and correctly initialized `g_esp`, register state,
   and the entry function's calling contract. Native C calls do not supply a
   guest return address or implement Xbox stack cleanup automatically.
3. Kernel services, path/save mapping, and thunk bridges using the correct guest
   layouts. Review dependencies between initialization steps for the title.
4. The graphics, MMIO/fault handling, audio, input, and event-service paths needed
   by translated code. Creating a D3D factory alone does not initialize rendering.
5. Defined resource ownership and shutdown/failure paths, including stopping host
   workers before releasing resources they access.

Link the `xboxrecomp` umbrella target plus the port's own generated/manual code
and required host integration. An external CMake project must supply a binary
directory when adding this checkout, for example:

```cmake
add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")
```

This line assumes `XBOXRECOMP_DIR` is set by the enclosing project; it is not a
complete build configuration. For the supported product, follow the port build
guide rather than reconstructing a build from this generic checklist.

## 5. Diagnose and preserve fixes

A failure is evidence to investigate, not a milestone to bypass. Capture the
executable identity, guest call/stack state, and relevant logs before changing a
callee's return value or removing a wait.

| Symptom | Investigation |
| --- | --- |
| Unknown indirect target | Check the guest pointer, function boundary, discovery seeds, and manual/generated dispatch precedence. Repair the maintained input and regenerate. |
| GPU/APU register fault | Confirm guest-to-host translation and the relevant MMIO handler. Do not replace register accesses with ordinary zero-filled pages. |
| Wait loop or silent audio | Trace the event, interrupt, callback, queue, and clock on which progress depends. Implement the missing contract rather than forcing completion. |
| Stack drift or repeated callbacks | Check the guest calling convention, return-address handling, shared epilogues, and callback re-entry. |
| Invalid object/vtable | Inspect the originating allocation and writes; a pointer guard alone does not repair corrupted state. |

Use a manual override only after establishing the original routine's contract and
the defect being corrected. Preserve guest registers, stack cleanup, outputs,
callbacks, and lifetime effects. A dummy success return can suppress an error
while leaving the caller waiting forever. Add a regression that exercises the
contract, including failure behavior, and keep the override reproducible.

Follow [Contributing](../CONTRIBUTING.md) for isolated test runs, prerequisites,
and validation records. Check both regeneration and native compilation after
translation changes. Startup success does not establish rendering, audio, saves,
mission logic, or timing parity; record interactive checks separately.

## Further reading

- [Architecture](../ARCHITECTURE.md) and [code map](../CODE_MAP.md): the current
  Mercenaries implementation and where to trace each subsystem.
- [Compatibility contracts](../COMPATIBILITY.md) and
  [workarounds](../WORKAROUNDS.md): established constraints and open questions.
- [Documentation index](README.md): current guides, validation, and historical
  evidence. Older toolkit notes describe particular earlier ports; compare their
  assumptions with current code before applying them.
