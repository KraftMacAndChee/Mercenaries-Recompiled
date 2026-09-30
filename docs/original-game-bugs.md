# Original-game bug compatibility

Recomp Options includes **OG Bugs**. It defaults to **On**, preserving the
original behavior of the bugs covered by this setting. **Off** enables the
listed optional corrections. Choose Apply to activate a change; Back without
applying discards it. The setting is saved beside the runtime as
`[RecompOptions] OGBugs=1` or `OGBugs=0` in `mercenaries_recomp.ini`.

This setting does not disable translation, rendering or host-integration fixes.
It is not a claim to reproduce every original bug. Its current scope is the four
corrections below. Three use the central fix enum; helicopter boarding reads
the applied option directly in `ai_boarding.h`.

## NW Mafia 2 music cue

The mission requests `mafia.Main_loop`, which does not resolve to a cue config.
With OG Bugs Off, the optional correction resolves that exact request to
`music.RU_explore`, the Mafia exploration cue playing before the broken request.
The earlier substitution used `music.Main_loop`, which is the main-menu theme;
it has been replaced at the user's request. This selects the existing exploration
cue, not the adaptive exploration/action controller or a new soundtrack.

Other requests, including the main-menu and mission-success themes, are unchanged.
Resolution occurs before managed-handle matching and config lookup, allowing a
playing cue to be reused. Applying Off can recover subsequent requests in an
already silent mission without reloading. Applying On restores the unresolved
retail request. The original author's intended replacement remains unconfirmed.
Native tests verify resolution, handle reuse and preservation mode.

See the [investigation record](runtime/nw-mafia2-music-2026-09-19.md) for the
captured failure and the limits of original-game comparison.

## Repeated mission-title ticking

The retail dateline setter restarts the title animation but retains the count of
characters already sounded. Reopening the same briefing, or displaying a shorter
title, consequently skips its `hud.text_blips` cue. Retail
Xbox instructions contain this behavior.

With OG Bugs Off, starting a valid new dateline also clears that sound counter.
The original animation rate, cue and timeout remain unchanged. This applies to
all titles using the shared dateline setter, including HQ briefing re-entry.
Changing the option affects the next title; it does not restart a current one.
OG Bugs On preserves the retained-counter behavior.

A native regression executes the lifted retail setter and update routines. It
reproduces the original silence and validates repeated and shorter titles at
30/60/120 FPS, early reopening, invalid text and restoration of preservation
mode. The Release candidate builds; an in-game listening check remains pending.

## Cloud layer at weather boundaries

The original flat-sky renderer can discard the outgoing cloud texture as soon
as the player enters a clear region, before the weather fade finishes. This was
reproduced at the western Nampo docks and traced to retail
instructions. With OG Bugs Off, transitions between sky_clouds and an empty
layer fade cloud coverage using the weather's original duration. Turning back
mid-transition starts from the visible coverage. Other cloud texture names pass
through. OG Bugs On preserves the immediate switch; applying a change affects
the next sky update and may itself visibly change coverage.

Native transition tests and private in-game crossings in both directions pass.
See [the Nampo investigation](runtime/terrain-sky-2026-09-21.md).

## Helicopter boarding height, including Embedded

With OG Bugs On, normal NPC helicopter boarding preserves the retail
horizontal-only proximity check. This includes the Embedded journalist boarding
an overhead helicopter even when its dock is far above him.

With OG Bugs Off, the helicopter dock must also be within **1.1 metres vertically**
of the rider. The existing horizontal checks still apply. This affects both normal
NPC boarding paths and driver/passenger seats; it leaves scripted immediate
transfers, player hijacks, and non-helicopter boarding behavior unchanged.

The applied setting is read on each boarding attempt, including an NPC already
waiting. Applying On removes the additional height restriction on the next attempt;
with Off, bringing the dock within reach allows boarding. This is not a change to
the journalist's filming duration or dialogue timing.

The [boarding implementation](../ports/mercenaries/src/ai_boarding.h) gates this
behavior directly through `recomp_options_og_bugs()`, outside the three-entry
central enum. The [native regression](../tools/recomp/test_ai_boarding_native.py)
checks both paths, height and horizontal distance, driver/passenger seats,
ordinary vehicles, live option changes, waiting then landing, and guest-register
preservation. This establishes those tested contracts, not every in-game scene.

## Inspector death tracking is always enabled

WMD Inspectors death tracking is a mission-correctness fix and is **not controlled
by OG Bugs**. It applies with either setting when the supported retail mission
loads. Restart/reload a mission already running an older build to receive it.
The explosive-only report was not established as original-game behavior.

See [the investigation and gameplay tests](runtime/wmd-inspector-deaths-2026-09-20.md).

## Adding another correction

Add a named entry to `recomp_original_bug_fix` in
`ports/mercenaries/src/recomp_original_bugs.h`, then explicitly allow it in
`recomp_original_bug_fix_enabled`. Unknown entries are disabled. Place each
behavior change at the narrowest relevant call site and gate it through this
policy; generated edits must also be represented in `Patch-Generated.py`.
The existing boarding-height gate is an exception to enum-based registration;
include direct option consumers when auditing the setting's full scope.

Document original evidence, the intended departure, application timing and
remaining uncertainty here. Test both preservation and corrected behavior,
including unrelated paths. Do not move existing fixes under this setting
without reviewing whether they correct the original game or the port.
