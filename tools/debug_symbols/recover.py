"""
Recover per-function source-file attribution from a debug build's __FILE__ strings.

Debug builds keep their assert macros, and those macros bake __FILE__ into the
binary as a string literal. Each literal is referenced from the code of the
function that contains the assert, so the string cross-references in
strings.json map function addresses back to the original source file they were
compiled from.

Two passes produce the attribution:

  direct       A function references a __FILE__ string -> that function came
               from that file. Majority vote when a function references more
               than one (inlining pulls foreign asserts into a caller).

  interpolated The linker emits each object file's functions contiguously, so an
               unattributed run of functions bracketed by two functions that
               agree on the same file almost certainly belongs to that file too.
               Only fills runs where both endpoints agree, and never crosses a
               section boundary.

The result is written as a {address: name} map, the same shape tools/ghidra_naming
emits, so the recompiler picks it up from functions.json without further work.
"""

import json
import os
import re
from bisect import bisect_right
from collections import Counter, defaultdict

# A __FILE__ expansion: an optional drive, then at least one directory
# component, then the filename. Anchored at the end of the string so RCS
# keyword junk ("$Header: .../tif_aux.c,v 1.8 92/03/27 ...") does not match --
# that trailing ",v 1.8" is exactly what distinguishes a version-control
# banner from a real __FILE__.
_SOURCE_RE = re.compile(
    r"((?:[A-Za-z]:)?[\\/]?(?:[\w .+-]+[\\/])+[\w .+-]+\.(?:%s))\s*$"
)
_CODE_EXTS = "c|cc|cpp|cxx"
_HEADER_EXTS = "h|hh|hpp|hxx"

# The scanner sometimes glues a preceding string's tail onto the front of a
# literal ("-2>c:\halo\SOURCE\camera\observer.c"). Matching unanchored at the
# front handles that; these are the characters a real path never starts with.
_LEAD_JUNK = re.compile(r"^[^A-Za-z0-9_.\\/]+")


def _compile_pattern(include_headers):
    exts = _CODE_EXTS + ("|" + _HEADER_EXTS if include_headers else "")
    return re.compile(_SOURCE_RE.pattern % exts)


def _extract_path(text, pattern):
    """Return the normalized source path inside `text`, or None."""
    m = pattern.search(text)
    if not m:
        return None
    path = _LEAD_JUNK.sub("", m.group(1)).replace("\\", "/")
    return path or None


def _strip_common_root(paths):
    """Drop the shared build-machine prefix (e.g. 'c:/halo/SOURCE/')."""
    if not paths:
        return {}
    dirs = [p.rsplit("/", 1)[0] + "/" for p in paths]
    root = os.path.commonprefix([d.lower() for d in dirs])
    root = root[: root.rfind("/") + 1] if "/" in root else ""

    # When every path lives in one directory the common prefix swallows it
    # whole, leaving bare filenames and losing the subsystem the file sat in.
    # Back off one component so the immediate parent survives.
    if root and all("/" not in p[len(root):] for p in paths):
        root = root[: root.rstrip("/").rfind("/") + 1]

    n = len(root)
    return {p: (p[n:] if p.lower().startswith(root) else p) for p in paths}


def _module_name(rel_path):
    """'ai/actor_combat.c' -> 'actor_combat', as a valid C identifier.

    Source filenames are not required to be identifier-safe: Halo alone ships
    bungie_net/common/64bit_math.c, and a name starting with a digit does not
    compile.
    """
    stem = os.path.splitext(os.path.basename(rel_path))[0]
    stem = re.sub(r"[^A-Za-z0-9_]", "_", stem)
    if not stem or stem[0].isdigit():
        stem = "_" + stem
    return stem


class _FunctionIndex:
    """Address -> containing function, by binary search over sorted ranges."""

    def __init__(self, functions):
        self.funcs = sorted(functions, key=lambda f: int(f["start"], 16))
        self.starts = [int(f["start"], 16) for f in self.funcs]
        self.ends = [int(f["end"], 16) for f in self.funcs]

    def owner(self, addr):
        i = bisect_right(self.starts, addr) - 1
        if i < 0 or addr >= self.ends[i]:
            return None
        return self.funcs[i]


def _attribute_direct(index, strings, pattern):
    """Map function start address -> Counter of source paths it references."""
    votes = defaultdict(Counter)
    raw_paths = set()
    for s in strings:
        path = _extract_path(s.get("string", ""), pattern)
        if not path:
            continue
        raw_paths.add(path)
        for ref in s.get("referenced_from", []):
            fn = index.owner(int(ref, 16))
            if fn is not None:
                votes[int(fn["start"], 16)][path] += 1
    return votes, raw_paths


def _resolve_votes(votes):
    """Majority vote per function; .c beats .h on a tie."""
    resolved, ambiguous = {}, 0
    for addr, counter in votes.items():
        if len(counter) > 1:
            ambiguous += 1
        best = max(
            counter.items(),
            key=lambda kv: (kv[1], not kv[0].lower().endswith((".h", ".hpp"))),
        )
        resolved[addr] = best[0]
    return resolved, ambiguous


def _interpolate(index, direct):
    """Fill unattributed runs bracketed by two functions agreeing on one file."""
    filled = {}
    known = [(a, i) for i, a in enumerate(index.starts) if a in direct]
    for (addr_a, i), (addr_b, j) in zip(known, known[1:]):
        if j - i < 2 or direct[addr_a] != direct[addr_b]:
            continue
        # Never bridge a gap that crosses sections -- different sections are
        # different link units and contiguity says nothing across them.
        if index.funcs[i].get("section") != index.funcs[j].get("section"):
            continue
        for k in range(i + 1, j):
            fn = index.funcs[k]
            if fn.get("section") != index.funcs[i].get("section"):
                continue
            filled[index.starts[k]] = direct[addr_a]
    return filled


def run(
    functions_path,
    strings_path,
    output_path=None,
    apply_to=None,
    include_headers=False,
    interpolate=True,
    verbose=False,
):
    pattern = _compile_pattern(include_headers)

    with open(functions_path, "r", encoding="utf-8") as fh:
        functions = json.load(fh)
    with open(strings_path, "r", encoding="utf-8") as fh:
        strings = json.load(fh)

    index = _FunctionIndex(functions)
    votes, raw_paths = _attribute_direct(index, strings, pattern)
    direct, ambiguous = _resolve_votes(votes)

    if verbose:
        print(f"  Source-path strings found:   {len(raw_paths)}")
        print(f"  Functions attributed direct: {len(direct)}")
        print(f"  ...of which ambiguous:       {ambiguous}")

    filled = _interpolate(index, direct) if interpolate else {}
    if verbose and interpolate:
        print(f"  Functions interpolated:      {len(filled)}")

    combined = dict(filled)
    combined.update(direct)  # direct evidence always wins over interpolation

    rel = _strip_common_root(sorted(raw_paths))
    names, attribution = {}, {}
    for addr, path in combined.items():
        rel_path = rel.get(path, path)
        key = f"0x{addr:08X}"
        names[key] = f"{_module_name(rel_path)}__sub_{addr:08X}"
        attribution[key] = {
            "source_file": rel_path,
            "origin": "direct" if addr in direct else "interpolated",
        }

    total = len(functions)
    summary = {
        "total_functions": total,
        "source_paths": len(raw_paths),
        "attributed_direct": len(direct),
        "attributed_interpolated": len(filled),
        "attributed_total": len(combined),
        "ambiguous": ambiguous,
        "coverage_percent": round(100.0 * len(combined) / total, 2) if total else 0.0,
    }

    if output_path:
        with open(output_path, "w", encoding="utf-8") as fh:
            json.dump(
                {"summary": summary, "names": names, "attribution": attribution},
                fh,
                indent=2,
            )
        if verbose:
            print(f"  Wrote {output_path}")

    if apply_to:
        _apply(apply_to, names, attribution, verbose)

    return summary


def _apply(functions_path, names, attribution, verbose):
    """Merge recovered names into functions.json, keeping a .bak once."""
    with open(functions_path, "r", encoding="utf-8") as fh:
        functions = json.load(fh)

    backup = functions_path + ".bak"
    if not os.path.exists(backup):
        with open(backup, "w", encoding="utf-8") as fh:
            json.dump(functions, fh, indent=2)

    renamed = 0
    for fn in functions:
        key = f"0x{int(fn['start'], 16):08X}"
        if key not in names:
            continue
        # Only replace placeholder names -- never clobber a name that some
        # other pass (ghidra_naming, func_id, a human) already recovered.
        if re.fullmatch(r"sub_[0-9A-Fa-f]+", fn.get("name", "")):
            fn["name"] = names[key]
            renamed += 1
        fn["source_file"] = attribution[key]["source_file"]
        fn["source_file_origin"] = attribution[key]["origin"]

    with open(functions_path, "w", encoding="utf-8") as fh:
        json.dump(functions, fh, indent=2)

    if verbose:
        print(f"  Applied to {functions_path}: {renamed} renamed, backup at {backup}")
