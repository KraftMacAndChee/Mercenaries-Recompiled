# Local localization transfer

`tools/modding/transfer_localization.py` creates a new Xbox registry archive
using matching translated strings from a locally supplied PS2 archive. The
motivating use case was a player's own PT-BR PS2 copy and own USA Xbox dump.
No translation, disc image, font atlas or game archive is distributed here.

## Inputs and matching

Supply extracted `english.dsk` files from the translated PS2 copy, unmodified
Xbox copy, and the Xbox version you intend to play (vanilla or a mod). The tool
matches the DSK asset key/type and complete named registry path. A target value
must still equal the unmodified Xbox value before it can be translated.
Changed/new mod strings and Xbox-only paths remain untouched. Printf placeholder
mismatches also remain untouched and are counted in the report.

This deliberately avoids erasing a campaign mod's changed weapon descriptions,
shop entries or mission instructions. Translate those separately with the mod
author's permission; this tool does not bundle or silently override them.

```powershell
python tools/modding/transfer_localization.py `
  --source 'D:\OwnFiles\PS2\ENGLISH.DSK' `
  --base 'D:\OwnFiles\Xbox\english.dsk' `
  --target 'D:\OwnFiles\XboxMod\english.dsk'
```

Without `--output`, this validates and reports counts/hashes only. To create a
new archive, add `--output 'D:\OwnFiles\Translation\english.dsk'`. Existing
files and inputs cannot be overwritten. Inspect the report before placing the
result in a separate `mods/My localization/DATAxbox/english.dsk` folder while
the game is closed. Load the localization after the campaign pack. Disable its
folder to return to the previous language; original files and saves stay intact.
Changing the campaign pack requires rebuilding the localization against that
pack, because the mod loader merges whole entries rather than string fields.

## Supported scope and limits

Only bounded flat DSK archives with named UTF-16 `ucfb/reg_/SCPE/BIN_` registry
leaves are supported. Unsupported grid/trailing data, malformed chunks,
duplicate paths/records and excessive nesting are refused. Unknown chunks and
untouched padding are preserved. The summary excludes text and personal paths.

Matching paths and placeholders are structural compatibility checks, not proof
of correct wording, UI fit, button glyphs, font coverage or campaign behavior.
PS2 fonts, HUD textures, executable patches and PNACH/Metamod V3 are not ported
by this operation. Xbox fonts and platform-specific prompts need separate
review. Native mod menus are independent of the guest localization archive.

The earlier local PT-BR prototype changed 6,707 strings in a Juche 2.3-based
archive and preserved four unmatched Xbox-only paths; this is dated evidence
from one user's inputs, not a promised coverage number for other translations.
The generalized tool adds stricter validation and may intentionally retain more
strings. Automated fixtures use synthetic content only. Complete campaign and
other regional builds remain unqualified.

## Contribution provenance

Implemented with OpenAI Codex, model `gpt-6.1-sol`, reasoning effort `xhigh`.
This attributes the transfer tool, fixtures and guide only; upstream game code,
community mods and translation authors retain their own attribution and rights.
