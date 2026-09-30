# Contributing

Start with the [Mercenaries build guide](ports/mercenaries/README.md). Use the
pinned compiler, SDK and Python dependencies when comparing build results.

## Where changes belong

- Fix instruction translation in `tools/recomp`; fix shared runtime behavior in
  the relevant `src` subsystem.
- Keep Mercenaries-specific behavior in `ports/mercenaries/src`. Durable edits to
  generated functions belong in `ports/mercenaries/scripts/Patch-Generated.py`.
- Treat `ports/mercenaries/src/recomp/gen` and `analysis` as disposable outputs.
  A direct edit there will be lost on regeneration.
- Keep test captures, logs and temporary executables under `artifacts`, not next
  to maintained sources. Do not commit game data, saves or local reference trees.

Preserve the original game's behavior unless a change deliberately offers an
option or fixes a demonstrated translation or host-integration defect. A guard
that silently returns success can hide a broken state transition; investigate
the caller's contract before adding one. Optional original-game corrections use
the [OG Bugs policy](docs/original-game-bugs.md); keep port correctness fixes
independent of that option.

## Code and documentation

Follow the surrounding file's style. Use four-space indentation in maintained
Python and C code; avoid unrelated reformatting of imported or generated code.
Prefer explicit names, bounded guest-memory access and comments that explain
invariants, address provenance or an otherwise surprising decision. Retain
third-party notices and useful unsupported-feature markers.

Keep changes focused enough to review and revert. Avoid combining a behavior
fix with a large file split. Describe what is known, what was tested and what
remains unverified. Dated investigation notes are evidence, not a substitute for
current build or user documentation.

## Naming and ownership

Use the existing convention at each boundary:

- `sub_XXXXXXXX` names identify guest addresses. Keep them stable across generated
  code, manual dispatch, patch rules, and retail fixtures.
- `recomp_*` names identify port hooks; `xbox_*` names identify shared compatibility
  APIs. D3D `dev_*` methods implement the existing device interface.
- Preserve exported names, script filenames, and textual patch/test anchors.
  Improve local names only after checking their consumers.
- Keep generator templates and port extensions distinct. Shared-looking helpers
  may have different ABI, macro, or lifetime requirements; compare those contracts
  before consolidating them.

For formatting or comment cleanup, check textual consumers and compare active C
tokens or Python ASTs as appropriate. Comments used by patchers/tests are part of
the tooling interface. Keep guest addresses, layouts, timing constants, and
diagnostic flag semantics unchanged unless a separate behavior change is intended.

## Validation

Tests live alongside the translator under `tools/recomp`. They include pure
Python checks, compiled native fixtures, and tests requiring a local retail XBE
or regenerated tree. Some older fixtures have machine-specific compiler paths;
check prerequisites before interpreting a skip or failure.

For example, after creating the project virtual environment:

```powershell
# Install the test runner separately from the pinned build dependencies.
& .\.venv\Scripts\python.exe -m pip install pytest
$env:PYTHONPATH = 'tools/recomp;.'
& .\.venv\Scripts\python.exe -X utf8 -m pytest tools/recomp/test_archive_generated_provenance.py
& .\.venv\Scripts\cmake.exe --build build/mercenaries --config Release --parallel 4
```

Run the tests relevant to the changed subsystem. For instruction or generated
patch changes, verify the retail bytes and regeneration path. Use native
fixtures for boundary conditions where possible. Source-text checks alone do
not demonstrate gameplay correctness. Record interactive validation separately
from build and automated-test results.

## Reporting and reviewing changes

Give a concrete reproduction, expected behavior, observed behavior, runtime
hash and relevant log times. Include enough evidence to distinguish a game
logic problem from translation, rendering, audio or input handling.

For a proposed change, explain the resulting behavior and the validation used.
For releases, keep validation tied to the executable hash and preserve the
content-addressed provenance archive. Player packaging is handled by
`Package-Preview.py`; source archives and player packages serve different
purposes. See [release maintenance](docs/release-maintenance.md).

## Isolated full-suite validation

From an x64 MSVC developer environment (the pinned toolset/SDK), with GCC on
PATH and pytest installed in the project virtual environment:

```powershell
& .\.venv\Scripts\python.exe -X utf8 -B tools/recomp/run_tests.py --list
& .\.venv\Scripts\python.exe -X utf8 -B tools/recomp/run_tests.py --output artifacts/test-run-new
# Restrict the run without collecting unrelated modules:
& .\.venv\Scripts\python.exe -X utf8 -B tools/recomp/run_tests.py --match reticle_proportions --output artifacts/reticles-new
```

Use a new output directory per run. The runner scans `tools/**/test_*.py` without
importing them: modules declaring test functions/methods run through pytest;
remaining modules run as standalone scripts. Each gets its own Python process,
log, and recorded exit status. Pytest modules also produce JUnit XML. Counts are
module exits, not assertion totals; inspect XML/logs for skips and collected cases.
`--jobs` defaults to 1; fixtures can share generated inputs and compiler outputs.
The runner preserves failures and timeouts rather than treating missing inputs
as successful tests. It does not supply required arguments to interactive tools.

Prerequisite groups currently coexist: source-only checks, native fixtures
requiring GCC/MSVC and the Windows SDK, retail/generated-tree checks, and tools
requiring explicit captured or live inputs. Prepare the supported retail XBE and
regenerated tree using the port guide before the broad run. Standalone C++ retail
oracles and interactive routes require their own inputs and are not discovered by
this Python runner. A single-process blanket pytest collection remains unsuitable
for fixtures that execute work or parse arguments at import.

For the dated failure inventory and recent fixture repairs, see
[maintenance validation status](docs/maintenance-validation.md).

The supported product build is Windows x64 with the pinned MSVC 14.44.35207 and
SDK 10.0.26100.0. The reusable runtime contains alternate backends; their presence
is not evidence that the Mercenaries port has a native POSIX build. Wine/Proton
runs the Windows payload and is a separate compatibility-test target.
