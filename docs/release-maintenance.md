# Release maintenance

## Reproducible inputs

Use the port build scripts and supported retail XBE. Keep translator fixes,
manual seeds and generated patches in maintained source. Archive the generated
tree and toolchain with `Archive-GeneratedProvenance.py` before replacing a known
build. Content-addressed archives remain immutable evidence for older releases.

Source archives include maintained runtime, translator, port sources, dependency
files and documentation. Logs, local test executables, scratch backups, Python
caches and historical generated snapshots are excluded. The active generated
tree is archived separately. No extracted game data or saves belong in either
maintained source or the player payload.

## Packaging

`ports/mercenaries/scripts/Package-Preview.py` takes a completed build directory,
provenance archive, validation JSON and release notes. Use `--help` for its
arguments. Use `--public-release` for the main player release; the default keeps
the older preview package name. Both variants include `modcompatibility.ini`
with `extended_graphics_memory=0` and `expanded_world_properties=0` in `[Mods]`.
These settings are independent and read beside the executable at startup.
It checks the validated runtime hash, archive hashes, ZIP integrity,
payload hashes and clean launcher/extractor behavior before publishing the ZIP.

Record test results and limitations against the exact executable hash. Do not
carry forward a successful gameplay result as evidence for unrelated changes.
Keep source-toolchain downloads separate from the ordinary player ZIP unless
explicitly requested.

When updating an existing installation, preserve game data, saves, gameplay
settings, `modcompatibility.ini`, and an intentionally enabled `developer.ini`. New packages ship the
developer menu disabled. Do not promote a running private trace as a tested
public release merely because it compiles.

## Before source publication

Review the [dependency inventory](dependencies.md), including component licenses,
corresponding-source requirements, and supplied-resource rights. The current working tree contains extensive
port work outside the original upstream Git snapshot; a plain `git archive HEAD`
does not describe the current port. For a public source snapshot, use `Package-Source.py` and review its contents
before committing. The [source distribution guide](source-distribution.md)
describes the manifest, exclusions, and first GitHub import. Generated-tree
provenance ZIPs remain local evidence, not public source payloads.

Keep local investigation evidence available, but exclude captures, personal
paths and saves from a public source package. Large runtime and patch modules
remain candidates for focused future splits with regeneration and behavior
checks; avoid a broad restructuring during a release fix.
