# Maintenance validation status

Recorded 2026-09-29. This follow-up addresses validation debt identified by the
maintainer review. It changes test fixtures and documentation only; production
source, gameplay behavior, assets, packaging, and build configuration are unchanged.
The earlier audit and cleanup reports remain historical records.

## Completed repairs

Eight previously failing modules now pass without removing their substantive checks:

| Fixture | Repair |
| --- | --- |
| [Developer settings](../tools/recomp/test_developer_ini_native.py) | Include the production settings parser in the extracted fixture. |
| [Frame timing](../tools/recomp/test_frame_timing_native.py) | Supply the missing SDK presentation constant. |
| [CPU presentation](../tools/recomp/test_cpu_frame_present_integration.py) | Link the production shader compiler wrapper and use its actual interface. |
| [Polygon offset](../tools/recomp/test_polygon_offset_native.py) | Link the production shader compiler with a fixture diagnostic sink. |
| [Guest depth precision](../tools/recomp/test_guest_depth_precision_native.py) | Link the same production compiler; retain WARP rendering and numerical checks. |
| [Renderer creation recovery](../tools/recomp/test_renderer_creation_recovery_native.py) | Include the real graphics-memory-size helper with retail memory configuration. |
| [PPD import](../tools/recomp/test_dev_import_ppd_native.py) | Include the production world-property implementation and explicitly model retail mode. Unexpected mod-pool allocation remains an assertion failure. |
| [Music preview](../tools/recomp/test_music_preview_native.py) | Include the audio-source snapshot helper and assert its separate event count while retaining music throttling and read-only checks. |

[CPU upload](../tools/recomp/test_cpu_frame_upload_native.py) and
[dossier intel](../tools/recomp/test_datapod_intel_native.py) now execute through
explicit `main()` entry points, rather than compiling native code during import.
AST comparison confirmed their original execution statements and embedded C were
preserved. The dossier fixture additionally returns its harness for explicit reuse
by the [NG+ regression](../tools/recomp/test_datapod_ngplus_native.py), which still
runs the dossier baseline and its existing negative controls.

A new [import-safety regression](../tools/recomp/test_native_fixture_import_safety.py)
checks that discovery ignores unrelated command-line arguments and neither creates
a temporary fixture directory nor invokes native subprocess work.

## Validation evidence

- The isolated runner attempted 471 modules. The full run exposed the NG+ fixture's
  reliance on import-time execution; its explicit harness reuse was then repaired
  and that module rerun successfully. Combining the full run with this recorded
  retry gives **458 successful module exits and 13 nonzero exits**.
- The previous baseline had 449 successful exits and 21 nonzero exits among 470
  modules. Eight old failures are resolved, one new regression module passes,
  and no new failing modules remain after the NG+ follow-up.
- Counts describe module exits, not assertion totals or proof of complete coverage.
  Inspect each module's log/JUnit XML for skips and actual collected cases.
- Syntax validation checked 602 Python files without failures.
- Saved-input hashes confirm no production source or build-input edits in this
  batch. Existing game and launcher executable hashes also remain unchanged.
- No fresh product build, player package, or interactive gameplay test was performed
  in this batch. Native fixtures compile the production components they exercise.

Local evidence is under `artifacts/maintenance-debt-20260929`: `all-tests`,
`ngplus-explicit`, `import-safety-final`, `final-results.json`, `summary.json`,
`scope-validation.json`, and `import-guard-equivalence.json`. Generated logs and
backups belong in local artifacts, not the public source distribution.

## Outstanding nonzero modules

A failing fixture is not automatically a product defect. Preserve its intended
contract before repairing extraction boundaries, dependencies, or expectations.

| Module | Required investigation |
| --- | --- |
| [Subdue diagnostic](../tools/diagnostics/test_subdue_retail_diagnostic.py) | Stale `PATCH_EXE` import and command-file write path. Replace the obsolete mechanism with tested write safeguards, not just deletion of the import. |
| [Collision dispatch](../tools/recomp/test_collision_dispatch_state_guard.py) | Compare generated dispatch behavior with the expected guard; an exact source-string assertion fails. |
| [Frame-pacing deadline](../tools/recomp/test_d3d8_frame_pacing_deadline.py) | Establish deadline behavior under the current frame-slot state; replace obsolete spelling checks only with equivalent coverage. |
| [Developer spawn request](../tools/recomp/test_dev_spawn_request_native.py) | Model dependencies and side effects of newer parser branches, including guest context and audio requests. |
| [Gameplay draw selector](../tools/recomp/test_gameplay_draw_base_selector.py) | Verify the surface-alias selection contract behind a missing textual match. |
| [Global pointer watch](../tools/recomp/test_global_pointer_watch.py) | Verify the diagnostic-output contract before updating the old format expectation. |
| [Environment cache](../tools/recomp/test_pgraph_environment_cache_native.py) | Distinguish intentional environment reads from violations of the intended caching contract. |
| [Preview logging](../tools/recomp/test_preview_log_native.py) | Resolve host-window dependencies in the extracted procedure while preserving F8/F9 behavior checks. |
| [Prompts](../tools/recomp/test_prompts_native.py) | Trace the failing native binding/resource hash assertion; treat this as a possible behavior defect. |
| [Roadblock trace cache](../tools/recomp/test_roadblock_trace_cache_native.py) | Bound the intended trace-cache check and account for additional environment switches. |
| [Setup launcher](../tools/recomp/test_setup_launcher_native.py) | Supply explicit ISO prerequisites, disposable output directories, and the supported generator/toolchain. |
| [Shared epilogues](../tools/recomp/test_shared_epilogues_native.py) | Check retail bytes and ABI behavior before replacing generated-body expectations. |
| [Visual/audio options](../tools/recomp/test_visual_audio_options_native.py) | Include FPS-cap loading and model INI writes with assertions, rather than suppressing their side effects. |

## Next maintenance priorities

1. Resolve the remaining fixture failures with explicit prerequisites and behavioral
   checks. Keep native assertion failures distinct from harness compilation failures.
2. Strengthen generated-patch ordering and failure-recovery tests before changing
   patch infrastructure.
3. Use those checks to support narrowly scoped responsibility/ownership improvements
   from the maintainer review. Avoid broad runtime restructuring until contracts
   for guest memory, rendering, audio, timing, and lifetimes are covered.

This batch reduces validation debt; it does not resolve every architectural finding
or certify the remaining failures as harmless. Any later production changes need
subsystem-specific regression testing and appropriate interactive game coverage.
