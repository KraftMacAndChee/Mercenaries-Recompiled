Mercenaries Recompiled

Extract the ZIP, then run Mercenaries Recompiled.exe.
If the game files are not installed beside it, select your legally obtained
retail Mercenaries ISO (Must be the Xbox version of the game in .iso format NOT .xiso or the PS2 Version) 
and click Install. After the one-time installation,
later launches use the installed files immediately. Game data is not included.

To update an existing installation, close the game and copy the contents of
this package into that installation folder. Keep your game_files, saves,
mercenaries_recomp.ini, developer.ini and modcompatibility.ini.

New keyboard profiles use QWERTY defaults: WASD movement, mouse buttons for
fire/grenade, R reload, Space jump, E use, Q aim, Left Ctrl crouch, and Tab PDA.
Menus use WASD, Space confirm and R back. Mouse Aim starts enabled. Existing
rebinds and controller controls are preserved. To adopt the new defaults,
choose Controls > Rebind Controls > Keyboard / Mouse, select a context, and
use Reset These Binds. Only that context changes; repeat for other contexts.


Recomp Options is available from the main and pause menus. Choose Apply after
changing aspect ratio, resolution, frame rate or other options. High internal resolutions can sometimes reduce frame rate.

To enable the developer menu, set developer_menu=1 under [Developer] in developer.ini and restart.
Logging is off by default. Set logging=1 in the same section and restart when collecting diagnostics.
Set logging=0 and restart to disable it again.
Create a mods folder beside the launcher and extract each mod into its own subfolder.
The launcher then opens the mod selector: enable mods, set their order, and launch
Vanilla or Modded. Later mods win conflicts. Optional separate saves use mod-saves.
Modded disk caches are isolated from vanilla, and installed game files are not overwritten.
Read MODDING.md for supported layouts, DSK merging, native DLL compatibility, and save/cache details.
DSK mods automatically enable expanded pools within their mod profile unless overridden.
For manual capacity configuration, change the relevant values in modcompatibility.ini from 0 to 1.
Merchant of Menace mods can use up to 128 total shop entries across all categories.
The shop configuration parser now handles files beyond the stock 63 items without
overwriting engine memory. This fix is automatic; no compatibility toggle is needed.
Items still need valid support templates and their normal script unlocks.

[Recomp Options]
FPS Cap - Select 30, 60, 90, 120, or Uncapped. Higher limits depend on hardware and scene complexity.
V-Sync - Synchronizes presentation to the display refresh rate. Defaults off.
FOV - Adjust gameplay field of view from 40 to 100 degrees in one-degree steps (default 55). The value is the horizontal FOV at 4:3; widescreen correction and relative zoom are retained.
Aspect Ratio - Changes the aspect ratio of game display with support for a variety of aspect ratios
NPC Wake Distance - Changes how close the player must be to NPCs in order to activate their AI. 150 = 1.5x, 200 = 2x, 300 = 3x (Fixes T-posing)
NPC LOD - Allows you to toggle whether lower level of detail models are ever displayed during gameplay
Resolution Scale - Changes the internal resolution scaling
Anisotropic Filtering - Allows Anisotropic filtering to be toggled on and off
Display - Allows you to switch between Exclusive Fullscreen, Borderless Fullscreen, and Windowed mode
Haze - Allows for the toggling of Original Xbox Bloom. Authentic preserves the same look at all resolutions, unfiltered becomes more exagerrated at higher res. (Off will break Radiation graphical effects)
Fixed Xbox Prompts - Forces the game to display the original Xbox Prompts according to the bindings of the original game, regardless of player control method or bindings
OG Bugs - A number of bugs were patched from the original game. This allows you keep those bugs if you prefer a more faithful version of the game. Includes NPC boarding of airborne helicopters, such as the journalist in Embedded. Off requires the boarding point to be within vertical reach; On restores original horizontal-only boarding. Off also corrects the missing NW_mafia2 music cue, repeated dateline typing sounds, flat-cloud transitions, and alternate-costume Jennifer backpack reappearance after scope/vehicle visibility changes.
PS2 Upgrades - Uses the supplied PS2 player Dragunov and 20 mm autocannon firing sounds and Allied M1 tank texture. Defaults off; turn it off to restore the Xbox assets.

Apply - Applies any changes made in Recomp Options
