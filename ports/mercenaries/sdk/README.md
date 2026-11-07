# DLL plugins for Mercenaries Recompiled

Plugin Interfaces 1 and 2 are available in builds containing the plugin host.
Interface 2 adds named hook points and guest-function calls; see
[Interface 2](#interface-2-hook-points-and-guest-calls). The original
public v1.1 executable does **not** implement either. Ship a plugin only to users with
an updated runtime **and launcher**.

Asset-only mods need no manifest. Legacy proxy DLLs still use their existing
Windows loading route. Merely adding mod.ini does not convert an old DLL:
it must implement the exported entry point in this SDK.

## Player installation

Put one mod folder directly under mods, enable it in the existing selector, and
launch modded. Restart the game after changing the selection. There is no hot
loading or unloading. Do not put dependency DLLs in their own mod folders.

The selector displays Ready (manifest checks passed), Plugin (disabled),
Error (invalid manifest or unmet requirement), or DLL (legacy/dependency files
without an official plugin manifest). Click the status, or select the row and
press F1, for details. Ready does not mean the DLL has executed or gameplay
compatibility has been established. Initialization errors name the plugin.

## Folder layout

~~~text
mods/
  Example Plugin/
    mod.ini
    example.dll
    example.ini
    helper.dll          (optional dependency)
~~~

Use unique lowercase plugin IDs. Settings remain in
plugin-settings/<id>/ beside the installation, outside mod-cache.
Each plugin's other files are staged under its own runtime/plugins/<id>/ folder.
Plugins can therefore have the same settings filenames without overwriting one
another. Configuration files (.ini, .json, .cfg, .conf, .config, .toml, .yaml,
.yml, .xml) seed persistent defaults only when no saved file exists. Updated
defaults do not overwrite player settings; migrate settings explicitly if needed.
mod.ini is metadata, never a saved setting.

Read/edit settings through host->settings_directory. Read packaged resources
through host->plugin_directory. Both are absolute UTF-8 paths, valid until
shutdown. Do not assume the current working directory is your plugin folder.
Use the Windows wide-character file APIs after converting UTF-8.

Game assets (DATAxbox, movies, game_files and existing retail paths) keep the
ordinary overlay rules, even in a plugin mod. modcompatibility.ini retains its
existing global compatibility-settings behavior. A plugin's other files stay
private; a proxy that must replace an executable-adjacent DLL belongs in a
legacy mod, not a [Plugin] package. The runtime plugins directory is reserved.

## mod.ini

Save as UTF-8, optionally with a BOM. Sections and keys are case-insensitive;
IDs are lowercase and case-sensitive. Values are unquoted. Comments occupy their
own lines beginning with ; or #. Unknown/duplicate [Plugin] keys are errors.

~~~ini
[Plugin]
Id=example
Name=Example Plugin
ApiVersion=1
Library=example.dll
; Optional comma-separated IDs:
; Requires=shared-library
; Conflicts=alternative-camera
; Exclusive=player-camera
~~~

| Key | Meaning |
| --- | --- |
| Id | Required, stable 1–64 lowercase ASCII letters/digits/underscores/hyphens; no Windows device names. Changing it creates a different settings folder. |
| Name | Optional display name; defaults to Id. The selector's main list retains the folder name. |
| ApiVersion | Required, 1 or 2. The host passes this value in every `api_version` it gives the plugin. Unknown versions are rejected before initialization. |
| Library | Required relative .dll path. No absolute paths, traversal, links or device names. Directory/filename stems use letters, digits, underscores or hyphens. |
| Requires | Optional plugin IDs that must be enabled **earlier** in load order. No automatic reorder or DLL function sharing is implied. Cyclic requirements cannot satisfy this rule. |
| Conflicts | Optional IDs that must not be enabled together; checked in either direction. Declare only known incompatibilities. |
| Exclusive | Optional resource IDs that this plugin alone may claim, e.g. player-camera. Two enabled claimants are rejected. These are cooperative declarations, not enforced native memory isolation. |

All optional lists default to empty. A mod.ini without [Plugin] does not opt in.
A missing DLL, duplicate ID, wrong API, invalid path, unmet dependency, declared
conflict or exclusive-resource collision stops a modded launch with an explanation.
A disabled broken plugin does not prevent a different selection or vanilla launch.

An ordinary file overlap is not a declared plugin incompatibility. Normal game
asset overrides still follow load order. This feature does not change DSK merging
or provide a general asset-overlap detector.

## Building the example

Use Visual Studio 2022's x64 developer command prompt:

~~~bat
cmake -S ports/mercenaries/sdk/example -B build/plugin-example -A x64
cmake --build build/plugin-example --config Release
~~~

When using the standalone SDK ZIP, run from its `sdk` folder with
`cmake -S example -B build -A x64`, then `cmake --build build --config Release`.

The output directory printed by CMake contains example.dll, mod.ini and
example.ini. Copy that complete directory into mods/Example Plugin.
The example logs initialization, its first update, periodic updates and shutdown.
It checks the retail XBE header through read_guest; it does not change gameplay.
The log is plugin-settings/plugins.log. LogIntervalMs is configurable in the
persistent example.ini (250–60000 ms).

`example_hooks` is the Interface 2 example (`ApiVersion=2`). Build it the same
way (`cmake -S ports/mercenaries/sdk/example_hooks -B build/plugin-hooks-example
-A x64`) and copy its output directory into mods/Hooks Example. It subscribes to
`weapon.player_fire` and `turret.update`, and every 10 seconds reads the HUD
resolution through guest calls to the game's own functions, logging to
plugins.log. `FireAlongBarrel=1` in hooks_example.ini makes player turrets fire
along their barrels.

The example builds as C with a static CRT and includes mercenaries_plugin.h.
Third-party plugins may use C++ internally, but the entry point must have C
linkage and export exactly **MercenariesPlugin_Init**. No decorated C++ export,
executable imports, RVA scans or compiler-proxy DLL are needed.

## Lifecycle and ABI

The host calls init outside DllMain after the supported retail XBE has been
validated and mapped, before its guest entry point runs. Guest objects are not
necessarily constructed yet. Inspect object signatures before changing fields,
and wait for an update when necessary.

The host passes MercPluginHost and a zeroed MercPlugin with struct_size and
api_version filled. Check the host version/size. Fill userdata, on_update and/or
on_shutdown without changing the supplied size/version. Return 1 to accept;
return 0 with a useful NUL-terminated UTF-8 error. A valid shutdown callback
supplied before an init failure is called for cleanup.

Initialization and updates run in selected load order. Shutdown runs in reverse
order, including rollback when a later initialization fails. Successfully loaded
DLLs stay resident until normal shutdown. Start no work in DllMain; join any owned
workers during shutdown before the host unloads your DLL. Process termination
or a fatal native crash cannot guarantee cleanup.

Version 1 supports one update and one shutdown callback per plugin; dispatch to
your own internal modules if needed. It deliberately does not expose arbitrary
guest-function calls, graphics-device objects, input replacement or native detours.
Interface 2 adds documented hook points and guest-function calls (below); it still
exposes no graphics-device objects or native detours.
Future facilities need documented API versions, not guessed structure extensions.
The legacy MercenariesMod_Init/MercModHost interface used by some proxy mods is
different and is not invoked by this host.

## Update timing and guest access

Updates run on the initialization/guest owner thread at retail logical-input
checkpoints. More than one checkpoint can occur per game update. They are **not**
render-frame, vblank, fixed-rate or simulation callbacks. They can run in menus
and while simulation is paused; they may not run during blocking loads.
wall_delta_seconds is elapsed monotonic real time, not gameplay time. Throttle
periodic maintenance using time rather than counting callbacks. It is valid for
two callbacks within one millisecond to have a zero delta.

read_guest and write_guest copy bytes and return 1 on success or 0 on rejection.
They accept lower guest RAM addresses [0x00010000, 0x04000000), validate the complete
range without integer overflow, and check committed/readable/writable pages.
They reject use outside your init/update/shutdown callback, from another thread,
through another plugin's context, or with an invalid range. The API does not
expose a raw guest pointer. It does not validate your interpretation of an object,
synchronize every game worker, or prove that a write is semantically safe.

Keep callbacks short; no waits, modal UI, file scanning or expensive compilation
each update. Do not retain guest pointers or mutate native game code through
undocumented addresses. Two plugins can still intentionally change the same
guest data: coordinate with authors or declare known conflicts.

on_update returns 1 normally. Returning 0 stops further updates for that plugin
and all its dependent plugins, shows an error and logs the failure. Unrelated
plugins continue. Cleanup runs at game shutdown; DLLs are not unloaded mid-game.
Use the host logger for a detailed cause before returning 0. Do not throw
exceptions across the C ABI. Arbitrary DLL crashes cannot be safely recovered
from or sandboxed by this interface.

## Interface 2: hook points and guest calls

Set `ApiVersion=2` in mod.ini. The host then fills the interface 2 members of
`MercPluginHost` (`register_hook`, `guest_call_begin`, `guest_call`,
`guest_call_end`) and passes 2 in every `api_version`. Interface 1 plugins are
unaffected and keep receiving 1. `sdk/example_hooks` is a complete example.

### Hook points

A hook point is a named place inside the game where the host offers
documented values to subscribed plugins. Subscribe from init:

~~~c
host->register_hook(host->context, "turret.update", on_turret_update, my_state);
~~~

`register_hook` returns 0 for a name this build does not provide, so a plugin
can refuse to load or fall back. The callback receives a `MercPluginHookEvent`:
`name`, `arg_count`, `args[]` (integers, guest addresses or float bits; use
`merc_plugin_f32`/`merc_plugin_bits`), `result` and `handled`. Setting
`handled = 1` replaces the retail behaviour as described per hook. Subscribers
run in load order on the guest thread, and each sees earlier plugins' changes.
`read_guest`, `write_guest` and guest calls are valid inside the callback.
Keep callbacks short: most points run every frame.

When no plugin handles a point, the built-in Decoupled Turrets and Decoupled Helicopters options decide,
and with both off every value is the retail one. Declare
`Exclusive=` resources (for example `turret-camera`) if your plugin must not
share a point with another.

| Name | args | handled / result |
| --- | --- | --- |
| `input.vehicle_look` | 0 joystick context (0 helicopter, 2 tank, 4/9 scope, 10 mounted gun), 1 axis (0 X, 1 Y), 2 retail axis value (float), 3 pending mouse counts, 4 radians per count at the player's sensitivity (float), 5 invert Y | handled: the host consumes the mouse counts and uses `result` (float, -1..1) as the logical axis value |
| `turret_camera.view` | 0 camera matrix (guest address, PblMatrix rows X, Y, Z = -look, position), 1 turret, 2-4 barrel pivot x/y/z (float) | edit the matrix with `write_guest`; handled: skip the built-in camera change |
| `chase_camera.snap` | 0 chase-camera state, 1 vehicle | handled: skip the helicopter's snap behind the vehicle this frame |
| `chase_camera.yaw` | 0 state, 1 vehicle, 2 current yaw (float, after retail auto-reset) | handled: `result` (float) becomes the camera yaw and the pending soft reset is cancelled |
| `turret.update` | 0 turret, 1 dt (float) | before the turret consumes its traverse settings (yaw setting +0x248, pitch setting +0x22C, each -1..1 of yaw speed +0x238 / pitch speed +0x21C); handled: skip the built-in drive |
| `turret.player_aim` | 0 turret | notification: RsAiPlayer is aiming this helicopter turret for the player |
| `turret.ballistic_arc` | 0 turret | handled: `GetYawPitchDiffs` aims straight, without the shell-drop arc |
| `weapon.player_fire` | 0 site (0 vehicle weapon, 1 gunner seat) | handled: fire along the barrel instead of at the crosshair pick |
| `hud.crosshair` | 0 crosshair object, 1 crosshair mode | handled: use the dumb-target reticle (static bracket and dynamic marker) this frame |
| `hud.dumb_target_turret` | 0 turret the marker follows | handled: `result` is the turret to follow |
| `hud.dumb_target_point` | 0 axis (1 Y first, then 0 X), 1 HUD size (float), 2 retail value (float), 3 value after the built-in option (float) | handled: `result` (float, HUD pixels) is the marker target |
| `hud.dumb_target_bracket` | 0 crosshair object; 1, 2 bracket x/y in HUD pixels (float, writable) | handled: draw the bracket at args 1/2 |

Guest addresses and object offsets above refer to the supported retail XBE.
New hook points are added by the port at reviewed patch sites; names and
arguments never change meaning once published.

### Guest calls

Plugins may call translated game functions from a hook or update callback:

~~~c
uint32_t scratch, width;
if (host->guest_call_begin(host->context, 64, &scratch)) {   /* 64 bytes of guest scratch */
    host->guest_call(host->context, 0x0020F920u, 0, NULL, 0, &width); /* EAX */
    host->guest_call_end(host->context);
}
~~~

`guest_call_begin` saves the complete translated CPU state and reserves up to
4096 bytes of scratch memory below the live guest stack. Use it for sret and
out parameters (pass its guest address in `args`, read results with
`read_guest`). `guest_call` pushes `args` (args[0] is the first parameter),
sets ECX to `this_ptr` for thiscall functions, runs the function and returns
EAX. Stack cleanup by either convention is undone after each call.
`guest_call_end` restores the CPU state so the interrupted game code resumes
unchanged. Calls nest up to four deep (a hook can fire inside a call). A call
outside a callback, from another thread, or to an address that is not a
translated function is rejected. A call left open when the callback returns
is closed by the host.

Calling game code is at the plugin's risk: the host cannot know whether a
function is safe at that moment. Prefer functions the hook's own code path
already uses, and test paused, loading and menu states.

## Dependencies and troubleshooting

Build Windows **x64** DLLs. Bundle non-system dependency DLLs alongside the plugin
DLL; dependencies use that directory and Windows System32, not PATH or the
working directory. Prefer uniquely named dependency libraries: Windows may reuse
an already loaded dependency by name, so private folders alone cannot solve all
native DLL dependency collisions.

plugins.log is replaced on each plugin-enabled game launch. It records initialization,
callback failures and shutdown. Common errors:

- Missing entry point: implement/export MercenariesPlugin_Init.
- Windows error 193: wrong architecture or invalid DLL.
- Windows error 126: plugin or one of its dependency DLLs could not be found.
- Unsupported interface: install a supporting recomp build or rebuild against this SDK.
- Read/write rejected: verify the address/range, thread, callback lifetime and page access.
- Requires: enable the named plugin and place it earlier.
- Failure after a recomp update in an old proxy: fixed native offsets are not this API.

Do not package legacy Blood/proxy DLLs with a new manifest and expect conversion.
Authors must replace their bootstrap/detours with the entry point and callbacks.
Test fresh settings, repeated launches, changed load orders, disabled/vanilla runs,
paused gameplay and normal shutdown before distributing a plugin.

