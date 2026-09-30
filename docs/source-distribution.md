# Source distribution and GitHub import

This guide describes the initial public source snapshot prepared on 2026-09-30.
It is a curated export of the maintained working tree, not an export of the
local Git database and not a playable release. Existing copyright, dependency
notices, and historical development records remain applicable.

## Included and excluded material

Included: the runtime and port source, generator and patch tooling, tests and
reviewed fixtures, documentation, pinned binary dependencies and their notices,
DSP56300 and Lua source, and the maintainer-supplied individual artwork/audio
resources used by the build. See [dependencies](dependencies.md) for license and
source provenance details. Some optional diagnostic tools need an explicitly
provided capture, emulator, or compiler; these are not normal runtime inputs.

Excluded: `.git`, development-only reference trees, local audit backups and
scratch evidence, disc images, extracted Xbox/PS2 data, saves, dumps, generated
game C and analysis, Python environments, and build outputs. No original Xbox
or PS2 executable, full sound bank, or complete game copy is included. Normal
builds need the user's verified Xbox retail ISO and the documented public tools.

Use the current [build guide](../ports/mercenaries/README.md) and
[documentation index](README.md) for supported workflows. Optional visual
resources have separate rights; see the
[source boundary](runtime/source-distribution-boundary.md).

## Package identity

`SOURCE_MANIFEST.json` at the archive root lists every exported payload file,
its byte size, and SHA-256. It excludes itself to avoid a circular checksum.
The adjacent `SHA256SUMS.txt` identifies the complete ZIP. Extract the ZIP into
an empty directory before checking or importing it. Never populate the export
by copying the original workspace wholesale.

The file hashes identify the source snapshot. They do not certify asset rights,
absence of every possible secret, or gameplay correctness. The source policy
checks known game/archive signatures and prohibited paths; it is a safeguard,
not a substitute for reviewing newly added files.

## First GitHub import

Use the supplied clean `repository` folder or a fresh extraction of the ZIP.
Do not run these commands in the original development workspace.

```powershell
git init -b main
git add .
git status --short
git diff --cached --stat
git commit -m "Initial source distribution"
```

Create an **empty** GitHub repository (do not initialize another README or
license). Then replace the URL below with your repository URL:

```powershell
git remote add origin https://github.com/YOUR-ACCOUNT/YOUR-REPOSITORY.git
git push -u origin main
```

The initial commit records an exported snapshot, not the full historical Git
history. Keep that distinction in the repository description and release notes.
The included files fit below GitHub's 100 MiB per-file limit; Git LFS is not
required for this snapshot. Future large captures and game data do not belong
in Git or LFS for this repository.

`.github` contains bug-report and pull-request templates. No automated build
workflow is installed: a full game build requires user-owned retail inputs that
must not be uploaded to a public CI service. Source-only validation may be added
separately. Contributor tests and prerequisite groups are documented in
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Preparing another source export

After reviewing the changes, from the repository root:

```powershell
& .\.venv\Scripts\python.exe -X utf8 -B ports/mercenaries/scripts/Package-Source.py --output artifacts/source-export-new
```

Choose a new destination. The script refuses to overwrite an existing output,
collects maintained source roots, rejects links and prohibited content, writes a
fresh file manifest, and emits a clean directory plus a verified ZIP and hash.
Its root-document allowlist and exclusions are explicit. Review additions to
those roots before publishing; the exporter does not infer licensing or whether
an unfamiliar diagnostic file should be public.

Keep generated-tree provenance archives private. They are different from this
public source package and may contain translated game code. For player-release
packaging use the [release maintenance guide](release-maintenance.md).
