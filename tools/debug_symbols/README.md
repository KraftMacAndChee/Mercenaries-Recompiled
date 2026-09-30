# Debug Symbol Recovery (`tools.debug_symbols`)

Recovers **which original source file each function was compiled from** by mining
the `__FILE__` strings that a debug build's assert macros leave in the binary.

No disassembler, no PDB, no external database — it reads the disassembler's own
`functions.json` + `strings.json` and writes meaningful names back.

## Why this works

Nearly every game ships an assert macro shaped like:

```c
#define assert(x)  if (!(x)) _assert_failed(#x, __FILE__, __LINE__)
```

In a **debug build** those calls survive, so the binary contains one string
literal per source file that has an assert in it, and each literal is referenced
from the code of the function containing the assert. Halo's beta build carries
295 of them:

```
c:\halo\SOURCE\ai\actor_combat.c
c:\halo\SOURCE\rasterizer\xbox\rasterizer_xbox_hardware_geometry.c
c:\halo\SOURCE\physics\collision_bsp.c
```

Cross-referencing those strings gives function -> source file, for free.

Retail builds usually compile asserts out, so this tool does nothing useful on
them. It is worth hunting for a debug/beta XBE of your target before starting:
one exists for far more Xbox titles than people expect, and it is worth more than
any amount of manual reversing.

## Two passes

**direct** — a function references a `__FILE__` string, so it came from that
file. If a function references more than one (inlining drags a foreign assert
into a caller), majority vote decides; `.c` beats `.h` on a tie.

**interpolated** — the linker emits each object file's functions contiguously.
An unattributed run of functions bracketed by two functions that agree on the
same file is almost certainly from that file too. Only fills when **both**
endpoints agree, and **never across a section boundary** — different sections are
different link units, where address adjacency means nothing.

Direct evidence always overrides interpolation.

## Usage

```bash
py -3 -m tools.debug_symbols \
    --functions build/disasm/functions.json \
    --strings   build/disasm/strings.json \
    -o build/disasm/debug_symbols.json -v
```

Inspect the result, then merge it into `functions.json` so the recompiler picks
it up:

```bash
py -3 -m tools.debug_symbols --functions build/disasm/functions.json \
                             --strings   build/disasm/strings.json --apply -v
```

`--apply` keeps a one-time `functions.json.bak`, and **only renames functions
still called `sub_XXXXXXXX`** — a name already recovered by `tools.ghidra_naming`,
`tools.func_id`, or a human is never clobbered. Every attributed function also
gets `source_file` and `source_file_origin` fields regardless.

### Options

| Flag | Effect |
|------|--------|
| `--apply` | Merge names into `functions.json` in place (keeps `.bak`) |
| `--include-headers` | Also attribute from `.h`/`.hpp` (asserts in inline functions) |
| `--no-interpolate` | Direct string references only; skip the contiguity fill |

## Output

Functions are renamed `<module>__sub_<ADDR>`, e.g. `actor_combat__sub_0004A120`.
The address is kept so names stay unique and traceable back to the binary, while
the module prefix makes the generated C greppable by subsystem.

```json
{
  "summary": { "attributed_total": 4331, "coverage_percent": 49.77, ... },
  "names":   { "0x0004A120": "actor_combat__sub_0004A120" },
  "attribution": {
    "0x0004A120": { "source_file": "ai/actor_combat.c", "origin": "direct" }
  }
}
```

## Result on Halo (build 2276 `cachebeta.xbe`)

```
Source-path strings found:   295
Functions attributed direct: 2067
...of which ambiguous:       0
Functions interpolated:      2264
Attributed 4331/8702 functions (49.77%) across 295 source files
```

Coverage is concentrated exactly where it should be:

| Section | Attributed | Why |
|---------|-----------|-----|
| `.text` | 4331/7222 (60.0%) | Bungie's own code — has the asserts |
| `D3D`, `DSOUND`, `XNET`, `XPP`, `BINK`, `D3DX` | 0 | Microsoft/RAD libraries, no `c:\halo\SOURCE` asserts |

That 0% across every library section is the correctness check: those functions
are interleaved by address with attributed ones, and the section guard is what
stops interpolation from confidently mislabelling all of them.

## Testing

```bash
py -3 tools/debug_symbols/test_recover.py
```

Covers path extraction (including RCS `$Header:` banners, which look like source
paths but are not `__FILE__` — libtiff alone ships hundreds), the common-root
strip, function ownership lookup, interpolation across section boundaries, and
the guarantee that `--apply` preserves existing names.
