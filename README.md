# Mercenaries Recompiled

DISCLAIMER: This project utilized a hybrid-workflow and iterative development.
	    It was built with the assistance of Codex 5.6 Sol and Astra!

A native Windows port of **Mercenaries: Playground of Destruction**, built with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). The project translates the
original Xbox executable into C and supplies compatibility layers for graphics,
audio, input, memory, and kernel services.

**You must supply your own supported retail Xbox copy of the game.** This source
repository does not include a disc image, extracted game-data tree, or generated
game executable. A PS2 disc is not a substitute for the Xbox build input.

[Build guide](ports/mercenaries/README.md) ·
[Documentation](docs/README.md) ·
[Architecture](ARCHITECTURE.md) ·
[Contributing](CONTRIBUTING.md) ·
[Dependencies and licenses](docs/dependencies.md)

## What is in this repository?

This is the maintained source, translation tooling, tests, documentation, and
pinned dependencies for the Windows x64 port. Features include keyboard/mouse
and controller support, configurable controls, resolution and aspect-ratio
options, an FPS cap, and selectable original-game bug compatibility. See the
[default controls](docs/DEFAULT_CONTROLS.md) and
[OG Bugs behavior](docs/original-game-bugs.md) for details.

The source archive is a developer package. It must be built before it can be
played. A player release is a separate package containing the launcher, native
runtime, dependencies, and first-launch instructions.

Windows x64 is the supported native build. Wine/Proton runs the Windows payload
as a separate compatibility path. The reusable toolkit's POSIX/OpenGL code does
not imply a supported native Linux or macOS Mercenaries build.

## Build from source

Install these prerequisites:

- Python **3.12**, available through the Windows `py` launcher.
- Visual Studio **2022**, with the C++ desktop build tools, MSVC
  **14.44.35207**, and Windows SDK **10.0.26100.0**.
- PowerShell 7 or Windows PowerShell 5.1.
- Your legally obtained **USA retail Xbox Mercenaries ISO**.

From the repository root in PowerShell:

```powershell
& .\ports\mercenaries\scripts\Build-From-Iso.ps1 -IsoPath 'D:\Games\Mercenaries.iso'
```

The example ISO path is a placeholder. Setup creates `.venv` and installs the
pinned Python dependencies; this step needs access to the Python package index
or an appropriate local cache. The extractor and required runtime libraries are
bundled with their notices. Visual Studio and the Windows SDK must already be
installed.

The script extracts and verifies the retail input, generates and patches the
translated C, builds the runtime and launcher, and writes build provenance.
The supported `default.xbe` SHA-256 is:

```text
AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7
```

Default output:

```text
build/mercenaries/bin/Release/
    Mercenaries Recompiled.exe    # First-launch setup and launcher
    mercenaries_recomp.exe        # Internal game runtime
```

Keep the DLLs and supporting directories beside those executables. Start
`Mercenaries Recompiled.exe` and select the supported ISO when prompted.
See the [port guide](ports/mercenaries/README.md) for the installation contract
and [release maintenance](docs/release-maintenance.md) for player packaging.
A root-only CMake build produces toolkit libraries, not the game executable.

Do not edit `ports/mercenaries/src/recomp/gen` to make permanent fixes. Its
contents are regenerated; durable patches belong in the maintained generator,
patch script, or port integration code.

## Playing a prebuilt release

Extract the entire player package and run `Mercenaries Recompiled.exe`. The
launcher extracts and verifies your ISO on first launch; it does **not** compile
the game on your computer. Keep the package's DLLs, `tools`, and `compat`
directories together. Follow its `README-FIRST-LAUNCH.txt` for the instructions
appropriate to that build. This source package does not contain those playable
executables.

## Reporting a problem

Use the repository's Issues tab. Include the exact build/version or executable
hash, Windows version, CPU/GPU/RAM, resolution, control device, reproduction
steps, and expected versus observed behavior. Mention any mods or changed
settings. A screenshot or a short recording can help with rendering issues.

For a runtime log, set `logging=1` under `[Developer]` in the player package's
`developer.ini`, restart, and reproduce the problem. Press **F8** while the game
has focus to mark the event in `preview-logs`. Logging is off by default. The
optional F9 developer menu separately requires `developer_menu=1` and a restart.

Review logs before attaching them; they can contain local paths. Do not upload
disc images, extracted game data, or memory dumps to a public issue. Arrange a
private channel if a maintainer needs a save or dump.

## Contributing and validation

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing guest-memory behavior,
generated-code interfaces, rendering, audio, threading, or timing. Compatibility
code can have ordering and lifetime requirements that ordinary refactoring
would accidentally change.

Tests are under `tools/`. Some use only maintained source, others compile native
fixtures, and others require a verified retail input, generated code, or a
specific capture. The isolated runner lists and selects tests without importing
unrelated modules:

```powershell
& .\.venv\Scripts\python.exe -m pip install pytest
& .\.venv\Scripts\python.exe -X utf8 -B tools/recomp/run_tests.py --list
& .\.venv\Scripts\python.exe -X utf8 -B tools/recomp/run_tests.py --match source_package_policy --output artifacts/source-policy-check
```

Use a new output directory for each run. Follow the contributor guide for the
compiler environment and full-suite prerequisites. See
[validation status](docs/maintenance-validation.md) and
[the latest comment-cleanup baseline](docs/stale-comment-cleanup.md) for known
failures. The recorded baseline has **458 successful module exits out of 471**;
it is not an all-green test suite or a gameplay guarantee.

## Finding your way around

| Path | Responsibility |
| --- | --- |
| `ports/mercenaries/src` | Game integration, controls, options, launcher, manual overrides |
| `ports/mercenaries/scripts` | Extraction, generation patches, builds, packaging |
| `ports/mercenaries/resources` | Embedded launcher resources and optional upgrade assets |
| `src`, `include` | Shared Xbox runtime and interfaces |
| `tools/recomp`, `tools/disasm`, `tools/func_id`, `tools/xbe_parser` | Translation pipeline and regression tests |
| `tools/diagnostics`, `tools/debug` | Optional investigation utilities |
| `templates` | Starting points for other Xbox ports |
| `docs` | Current guides and dated investigation records |
| `third_party`, `ports/mercenaries/third_party` | Dependencies, corresponding sources where bundled, notices |

Start with [ARCHITECTURE.md](ARCHITECTURE.md) and [CODE_MAP.md](CODE_MAP.md).
The [documentation index](docs/README.md) gives the full maintainer reading order.
Historical records explain decisions and failed approaches; their conclusions
may have been superseded. Local evidence archives referenced by those records
are not shipped. See [source distribution](docs/source-distribution.md) for the
export boundary, manifest, and instructions for importing this snapshot into Git.

## Credits and licensing

Mercenaries Recompiled is by Trenton Turner (KraftMacAndChee), building on
sp00nz's xboxrecomp toolkit and work from the Xbox emulation and modding
communities. This is an unofficial fan project, not an official game release.

The root [MIT license](LICENSE) retains the toolkit's copyright notice. It does
not replace imported components' licenses. xemu-derived code carries per-file
LGPL notices; SDL2, DSP56300, Lua, fonts, and other dependencies retain their own
licenses. The bundled Windows SDK shader compiler has separate redistribution
terms. Consult the [dependency inventory](docs/dependencies.md) and accompanying
notice files before redistributing a modified package.

Maintainer-supplied artwork, controller prompts, and optional upgrade resources
are retained. Their inclusion does not grant new rights to those assets under
the root MIT license. Mercenaries and its game assets belong to their respective
owners. This export preserves attribution and development records.