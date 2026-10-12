# Non-destructive mods

Create `mods` beside `Mercenaries Recompiled.exe`. Extract each mod into its own immediate subfolder. ZIP files are not loaded directly. With any mod folders present, launching the game opens the mod selector. New mods start disabled; the selector remembers enabled mods, order, and the separate-save setting.

Check the mods to enable, use the arrows to change their order, and choose **Launch Modded**. Higher numbers load later and win conflicts. **Launch Vanilla** ignores the selection and uses the original game files, saves, and disk cache. With no enabled mods, Launch Modded also starts vanilla. Neither button installs files over the original game data.

Mouse-wheel and scrollbar navigation accommodate long lists. Keyboard controls: Up/Down selects a row; Ctrl+Up/Down moves it; Space enables/disables it; Tab moves between the list, launch buttons, and separate-save checkbox; Enter activates the focused item; Escape closes the selector.

## Optional direct-launch shortcuts

After choosing mods in the selector, a shortcut can set
`MERCENARIES_AUTOLAUNCH_MODE=modded` for that launcher process to reuse the saved
selection without opening the selector. `vanilla` launches the original game
and normal saves. Both routes use the existing preparation, compatibility
checks and lifetime lock; the variable does not enable newly discovered mods.
An empty mod selection still launches vanilla.

For a `PLAY.cmd` beside `Mercenaries Recompiled.exe`:

```bat
@echo off
setlocal
set "MERCENARIES_AUTOLAUNCH_MODE=modded"
if /i "%~1"=="vanilla" set "MERCENARIES_AUTOLAUNCH_MODE=vanilla"
if /i "%~1"=="selector" set "MERCENARIES_AUTOLAUNCH_MODE="
start "" /d "%~dp0" "%~dp0Mercenaries Recompiled.exe"
endlocal
```

Run `PLAY.cmd selector` to change the selection and `PLAY.cmd vanilla` to play
without mods. The values are case-insensitive. An unset, empty, unrecognized or
oversized value retains the normal selector behavior. Prefer `setlocal` over a
system-wide environment variable so ordinary launches keep their existing UI.
No selection or save is rewritten by the shortcut itself.

## Folder layouts

A pack of Xbox DSK files can use this layout:

```text
mods/
  JE3/
    assets.dsk
    english.dsk
    locked.dsk
    streamed.dsk
```

Bare DSK filenames map into the game's `DATAxbox` folder. Mods can also mirror the game-data tree explicitly:

```text
mods/
  My skin and additions/
    game_files/
      DATAxbox/
        assets.dsk
        new-folder/
          additional-file.dat
```

`game_files/mercenaries-retail/`, `mercenaries-retail/`, and direct `DATAxbox/` or `movies/` trees are also accepted. Explicit `game_files/` is the unambiguous choice for a new guest file outside known game folders. Added files become visible to game filesystem lookups and directory enumeration; the game still needs an asset or script reference to use them. Loose textures are not automatically inserted into DSK archives.

## Legacy DLL mods

Windows proxy DLL mods retain their installation-relative layout:

```text
mods/
  Camera/
    D3DCOMPILER_47.dll
    d3dcompiler_47_native.dll
    modloader/
      camera/
        camera.ini
```

Files not identified as game data go into a private runtime directory for that mod selection. This allows Windows to load replacement DLLs before the game starts. Mods are not arbitrary DLL injection: a DLL still needs to be imported by the executable, forwarded by a proxy, or loaded by the mod itself. Two mods replacing the same DLL use the later file; they do not automatically chain hooks. Executable and `default.xbe` replacement are not supported by this static recompilation.

Native mods must support the current executable version. In testing, Camera 1.0 DLL loaded and wrote its log, but reported `INCOMPATIBLE EXE: no camera hooks installed`. The loader cannot repair version-specific hooks inside a third-party DLL. Native DLLs execute normal Windows code; the overlay is not a sandbox for their own file operations.

## Official DLL plugins

Updated launcher/runtime builds support an optional `[Plugin]` section in
`mod.ini` and a documented `MercenariesPlugin_Init` entry point. The original
public v1.1 does not provide this interface. Asset-only mods do not need a
manifest; existing proxy DLL mods keep the legacy behavior above.

The selector shows plugin status and details (click the status or press F1).
Required plugins must appear earlier in load order. Declared conflicts and
exclusive-resource collisions block the modded launch with an explanation.
Ordinary game-asset overlaps still follow the existing load-order rules.

Official plugins receive initialization, update and shutdown callbacks, checked
guest-memory copies, logging, and a private settings directory. Their settings
live in `plugin-settings/<id>/` beside the installation and survive mod-cache
retirement. Plugin IDs remain stable when folders are renamed. This is separate
from the legacy `mod-settings/<native-selection>/` mechanism described below.

See the [plugin author guide and SDK](../ports/mercenaries/sdk/README.md) for the
complete `mod.ini` schema, API contract, limitations and buildable example.
Existing DLLs must be adapted by their authors to use this interface; a manifest
alone cannot convert them or make conflicting hooks compatible.

## DSK conflicts and additions

When multiple selected mods supply the same retail DSK, the loader compares each archive against the installed vanilla archive and combines changed entries. Entries are identified by their asset key and type. Unchanged vanilla entries in a later archive do not erase an earlier mod's unrelated changes. Later changed entries win; added entries accumulate; omitted vanilla entries count as deletions. Merging happens at the whole-entry level, not individual properties within an entry. Mods made for different base archives or with incompatible asset dependencies may still conflict.

The second DSK header word is an optional grid-data offset, not a format version.
Different values alone do not block a merge or indicate overlapping asset edits.
For flat archives, the merger writes a zero grid offset and rebuilds the ordinary
asset directory. An optional zero-filled trailing sector (including its final
cache-signature word) is supported. Actual grid indexes or other unsupported
trailing data are rejected with the archive path and reason rather than discarded.
This does not establish gameplay compatibility between mods.

A single replacement archive is read unchanged. A new archive with no retail counterpart uses whole-file precedence. Disabling a mod removes its additions from the next launch's file view and archive composition. DSK packs automatically enable the existing expanded graphics-memory and world-property pools in their runtime profile. A mod supplying its own `modcompatibility.ini` can override those settings. Vanilla settings stay unchanged.

## Saves, caches, and settings

- **Use separate saves** enabled: modded play uses `mod-saves/mercenaries`.
- Disabled: modded play uses the normal `saves/mercenaries`. Gameplay can then update those saves normally.
- All mod selections using separate saves share that separate save collection; it is not one collection per mod.
- Saves are not copied or converted automatically. To copy progress deliberately, close the game and copy the desired `UserData`/`TitleData` contents into the equivalent separate-save folders. Do not copy disk caches.
- Xbox disk caches and system-partition backing files for modded play live under `mod-cache/<fingerprint>/storage`, regardless of save selection. Vanilla uses its original cache. No vanilla cache is deleted.
- The fingerprint includes the game executable, installed retail data, selected mod order and contents, and relevant native libraries. Changed data or load order selects a different cache. This prevents cached mod assets from leaking into vanilla or another selection.
- Game controls and Recomp Options still use the installation's `mercenaries_recomp.ini`. Developer logging uses its `developer.ini`. Native mods still see their expected paths in the profile's `runtime` directory. Configuration files (`.ini`, `.json`, `.cfg`, `.conf`, `.config`, `.toml`, `.yaml`, `.yml`, `.xml`) are retained in `mod-settings/<native-selection>/`, separately from disposable caches. The settings identity follows the ordered native-mod folder names, so archive edits and game updates do not discard settings. Different native-mod combinations have separate settings. Rename a native-mod folder and it receives a new settings identity. Binary/custom settings formats are not automatically preserved.

Close the game before editing mods. The first launch hashes the installed data and prepares any required merged archives; the selector stays responsive while preparing. Native runtime files and caches consume additional disk space. An unchanged fingerprint reuses the current profile. Changing the selection or its inputs retires older profiles before creating the replacement; launching vanilla also retires mod profiles. Close the running game before preparing another launch. Configuration files are linked to persistent storage where supported; atomically replaced files and files on filesystems without hard-link support are synchronized on the next preparation or retirement. Unedited configuration defaults update with the mod; edited settings are retained. Older loader profiles have their configuration files rescued into `mod-settings/legacy/<old-fingerprint>/` for manual recovery because their native-mod identities were not recorded. Do not manually delete a cache containing unsynchronized settings; use the launcher to retire it. Native logs and other runtime output are disposable. Back up any unsupported custom settings files before switching configurations. Never remove `saves` or `mod-saves` to clear a cache.

Keep the installation path reasonably short: the runtime currently uses Win32 `MAX_PATH` buffers, and deeply nested mods may be rejected. Mod folders must use ordinary directories, not links or junctions.

## Implementation and distribution

`mod_loader.cpp` resolves the selected files, composes archives, and prepares the launch environment. `mod_selector.cpp` implements the launcher UI. `src/kernel/mod_overlay.c` maps guest reads to real source files; small directory placeholders provide a merged enumeration view without copying the retail data. The guest cannot write through this overlay. `kernel_path.c` routes mod caches separately from savegames.

Local mods, profiles, and saves are excluded from source exports. Launcher artwork is embedded in the executable. Distribute the new launcher and game executable together; no supplied example mod or retail game data is bundled in the build.


## Optional gameplay extensions

Mods can opt into faction-aware support, impact-stun ammunition, extra grenade
storage, NPC voice-set overrides, ExOps follow/wait interactions, throwable C4,
per-mission permanent contract failure, and latest-save permadeath through
`modcompatibility.ini`. See [the gameplay extension guide](MOD_EXTENSIONS.md) and
[the JE3 configuration example](examples/je3-modcompatibility.ini). Permadeath uses
an explicit `save_profile` in that INI; folder names never enable it or determine
which profile is tracked.
