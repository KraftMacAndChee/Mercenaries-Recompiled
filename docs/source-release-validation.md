# Initial source export validation - 2026-09-30

Validation used a fresh extraction of the curated source ZIP, its own Python
3.12 virtual environment with the pinned requirements, and the supported MSVC
14.44.35207 / Windows SDK 10.0.26100.0 toolchain. No original workspace source
or generated C was used by the isolated build. The verified retail Xbox input
was supplied from the maintainer's local extracted game directory; it is not
included in the export.

## Results

- Public dependency setup succeeded in the isolated environment.
- `Recompile.ps1 -GameDirectory <verified-retail-directory>` completed:
  **14,083/14,083 functions lifted, zero failures, 56 unresolved direct targets**.
  Those unresolved targets remain part of the existing generated fallback path;
  a successful build does not establish that every guest execution path works.
- `Build.ps1` successfully configured, compiled, and linked both the Release
  runtime and first-launch launcher from that freshly generated tree.
- Source-policy/provenance tests: **46 passed, one skipped** (creating a symbolic
  link was unavailable in the test environment).
- New exporter tests: **three passed**, covering manifest/archive byte identity,
  exclusion of local outputs, refusal to overwrite an export, and rejection of
  renamed game executables and crash captures.
- ZIP path/signature policy, reviewed-fixture digest, CRCs, per-file SHA-256
  equality, and the GitHub file-size ceiling passed. The source manifest records
  the exact payload. No payload file is ignored by the final root Git rules.
- Links in the current entry-point guides resolve inside the export. Historical
  investigation records retain citations to non-distributed local evidence.
- A limited scan for private-key headers, GitHub tokens, and AWS access-key IDs
  found no matches; this is not a comprehensive secret or provenance audit.

## Build identities

| Output | SHA-256 |
| --- | --- |
| `mercenaries_recomp.exe` | `A45B981EC29219A3532EA2F09738CBFB45E286172CBB87DDB95B438F1367C6EE` |
| `Mercenaries Recompiled.exe` | `41067E50AF8202D91BED4CC4263A4D408B31287F0686B58503E39A69A6BC05C5` |

The final packaging changes after that build affect documentation, Git ignore
rules, and source-export tooling/tests only. Maintained runtime, translator,
patch, build, and resource inputs were compared byte-for-byte with the isolated
build's source inputs before final delivery.

## Scope and limitations

The check started from an already extracted and hash-verified retail XBE. It did
not repeat ISO extraction or the top-level ISO provenance step. No gameplay,
first-launch GUI, Wine/Proton, or clean-machine installation test was performed
for this documentation/source-packaging change. The separate historical full
suite baseline remains in [maintenance validation](maintenance-validation.md)
and [stale-comment cleanup](stale-comment-cleanup.md); it was not rerun here.

The compiled executables, generated game C, extracted input, full build logs,
and test scratch outputs stay outside the public source package. Passing these
checks does not grant rights to supplied artwork/audio or establish legal
clearance. Preserve the [dependency notices](dependencies.md).
