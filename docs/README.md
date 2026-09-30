# Documentation

## Build, play, and contribute

- [Building and installing Mercenaries](../ports/mercenaries/README.md): the
  supported ISO-to-port workflow, verified XBE, and pinned toolchain.
- [Default keyboard controls](DEFAULT_CONTROLS.md): new-profile bindings and
  resetting individual contexts without replacing other personal bindings.
- [Original-game bug compatibility](original-game-bugs.md): every behavior
  currently controlled by OG Bugs.
- [Contributing](../CONTRIBUTING.md): change boundaries and validation commands.
- [Dependencies and notices](dependencies.md), [release maintenance](release-maintenance.md),
  and [launcher resources](../ports/mercenaries/resources/launcher/README.md).

- [Source distribution and GitHub import](source-distribution.md): package contents,
  checksums, import commands, and repeatable source exports.
- [Initial source export validation](source-release-validation.md): isolated
  regeneration/build results and the limits of those checks.

## Maintainer reading order

These guides describe the current implementation at their recorded snapshots.
For code changes, verify the cited symbols against the working tree.

1. [Architecture](../ARCHITECTURE.md): system boundaries and the execution model.
2. [Code map](../CODE_MAP.md): files and symbols to start from for a specific problem.
3. [Subsystems](../SUBSYSTEMS.md) and [data flow](../DATA_FLOW.md): component
   responsibilities, state ownership, startup, and cross-subsystem interactions.
4. [Compatibility](../COMPATIBILITY.md) and [workarounds](../WORKAROUNDS.md): guest
   contracts, title-specific behavior, and changes that need special care.
5. [Maintenance](../MAINTENANCE.md): known debt, invariants, and regression obligations.
6. [Architecture evidence](../ARCHITECTURE_EVIDENCE.md): how current behavior was
   reconciled with historical findings and which conclusions remain uncertain.

## Validation and documentation status

- [Maintenance validation](maintenance-validation.md): the dated automated-test
  baseline and outstanding failures, with fixture-specific investigation needs.
- [Stale-comment cleanup](stale-comment-cleanup.md): the 2026-09-30 comment pass
  and full-suite rerun; all 471 module exit statuses matched the previous baseline.

Test results apply to the documented inputs and environment. A successful module
exit or build is not proof of complete gameplay or cross-platform parity.


## General toolkit reference

[Getting started](GETTING_STARTED.md) describes analysis and integration for a new
title; the [tool overview](../tools/README.md) provides additional pipeline context.
Use the Mercenaries port guide to build this game. Generic commands and templates
do not replace the supported port's discovery, patching, or validation steps.

The `formats`, `pipeline`, and `technical` directories include general reference
material and earlier-port examples. Check their assumptions against current code;
they are not the authoritative Mercenaries build or runtime specification.
