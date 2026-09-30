#!/usr/bin/env python3
"""
Score the toolkit against XBEs built from XDK sample source.

Every other way of testing this toolkit needs a copyrighted game. This one
doesn't: point it at an Xbox XDK install and it compiles a sample, links it,
runs imagebld to produce an XBE, and then measures our disassembler against the
linker MAP from that same build. The MAP is ground truth by construction --
same build, same binary, no guessing whether the two match.

Nothing copyrighted enters the repo. The XDK, its samples, and the XBEs this
produces all stay on your disk; this file only knows how to drive them.

    py -3 tools/sdk_validate.py --xdk "G:\\...\\XBoX - Microsoft SDK 2003\\XDK"
    py -3 tools/sdk_validate.py --xdk ... --sample Tut02_Vertices
    py -3 tools/sdk_validate.py --xdk ... --list

Requires an XDK with xbox/bin/vc7/CL.Exe, xbox/bin/imagebld.exe, xbox/include
and xbox/lib. "Stripped" SDK drops (no include/lib) cannot build.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from symbols.map_names import load_sections, parse_map  # noqa: E402

# Taken from the samples' own .vcproj AdditionalDependencies. Do NOT add
# /NODEFAULTLIB or a custom /ENTRY: xapilib supplies the startup that
# references main, and without it the linker discards the entire sample as
# unreferenced, leaving an XBE of nothing but library code that scores
# identically for every sample.
DEFAULT_LIBS = ["xapilib.lib", "d3d8.lib", "d3dx8.lib", "xgraphics.lib",
                "dsound.lib", "xboxkrnl.lib"]
# Also from the .vcproj.
DEFINES = ["/DWIN32", "/D_USE_XGMATH", "/D_XBOX", "/DXBOX_SAMPLE", "/DNDEBUG"]
SAMPLE_ROOT = Path("Samples") / "Xbox"


def find_sample(xdk, name):
    root = xdk / SAMPLE_ROOT
    for p in root.rglob("*"):
        if p.is_dir() and p.name.lower() == name.lower():
            srcs = sorted(list(p.glob("*.cpp")) + list(p.glob("*.c")))
            if srcs:
                return p, srcs
    return None, []


def list_samples(xdk):
    root = xdk / SAMPLE_ROOT
    out = []
    for p in sorted(root.rglob("*")):
        if p.is_dir() and (list(p.glob("*.cpp")) or list(p.glob("*.c"))):
            out.append(p.relative_to(root).as_posix())
    return out


def build(xdk, srcs, workdir, libs, verbose=False):
    """source -> .obj -> .exe + .map -> .xbe. Returns (xbe, map) or (None, err)."""
    vc7 = xdk / "xbox" / "bin" / "vc7"
    env = dict(os.environ)
    env["INCLUDE"] = str(xdk / "xbox" / "include")
    env["LIB"] = str(xdk / "xbox" / "lib")
    env["PATH"] = f"{vc7};{xdk / 'xbox' / 'bin'};{env.get('PATH','')}"

    def run(args):
        r = subprocess.run(args, cwd=workdir, env=env, capture_output=True, text=True)
        if verbose:
            print("   ", " ".join(str(a) for a in args[:3]), "...", r.returncode)
        return r

    objs = []
    for s in srcs:
        r = run([str(vc7 / "CL.Exe"), "/c", "/nologo", "/O2"] + DEFINES + [str(s)])
        obj = workdir / (Path(s).stem + ".obj")
        if not obj.exists():
            return None, f"compile failed: {r.stdout[-400:] or r.stderr[-400:]}"
        objs.append(obj.name)

    # /SUBSYSTEM:XBOX and a MAP are both required by imagebld.
    r = run([str(vc7 / "Link.Exe"), "/nologo", "/MACHINE:I386", "/SUBSYSTEM:XBOX",
             "/FIXED:NO", "/MAP:out.map", "/OUT:out.exe"] + objs + libs)
    if not (workdir / "out.exe").exists():
        return None, f"link failed: {r.stdout[-400:] or r.stderr[-400:]}"

    # The sample's own code must actually be in the image. If the link drops it
    # as unreferenced, everything still "succeeds" and produces an XBE of pure
    # library code that scores identically for every sample -- a harness that
    # measures nothing while looking healthy. Fail loudly instead.
    mtext = (workdir / "out.map").read_text(errors="replace")
    if not any(o in mtext for o in objs):
        return None, ("link dropped the sample: no symbol in out.map comes from "
                      f"{objs}. The image is library code only.")

    r = run([str(xdk / "xbox" / "bin" / "imagebld.exe"), "/OUT:default.xbe", "out.exe"])
    if not (workdir / "default.xbe").exists():
        return None, f"imagebld failed: {r.stdout[-400:] or r.stderr[-400:]}"
    return workdir / "default.xbe", workdir / "out.map"


def score(xbe, mapf, workdir, repo):
    """Run the pipeline on the XBE and score it against its own MAP."""
    analysis = workdir / "analysis.json"
    r = subprocess.run([sys.executable, "-m", "tools.xbe_parser", str(xbe),
                        "--json", str(analysis), "--quiet"],
                       cwd=repo, capture_output=True, text=True)
    if not analysis.exists():
        return None, f"xbe_parser failed: {r.stderr[-300:]}"

    outdir = workdir / "dis"
    r = subprocess.run([sys.executable, "-m", "tools.disasm", str(xbe),
                        "--analysis-json", str(analysis), "--text-only",
                        "-o", str(outdir)], cwd=repo, capture_output=True, text=True)
    fj = outdir / "functions.json"
    if not fj.exists():
        return None, f"disasm failed: {r.stderr[-300:]}"

    secs, a = load_sections(analysis)
    syms, _entry = parse_map(mapf, secs)
    lo = int(a["sections"][0]["virtual_addr"], 16)
    hi = lo + a["sections"][0]["virtual_size"]
    truth = {v for v in syms if lo <= v < hi}

    f = json.load(open(fj))
    if isinstance(f, dict):
        f = f.get("functions", list(f.values())[0])
    det = {int(x["start"], 16) for x in f if lo <= int(x["start"], 16) < hi}
    hit = truth & det
    if not truth or not det:
        return None, "nothing to score"
    return {"truth": len(truth), "detected": len(det), "hit": len(hit),
            "recall": 100 * len(hit) / len(truth),
            "precision": 100 * len(hit) / len(det)}, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xdk", required=True, help="path to the XDK dir")
    ap.add_argument("--sample", action="append",
                    help="sample dir name (repeatable). Default: a few small ones.")
    ap.add_argument("--list", action="store_true", help="list buildable samples")
    ap.add_argument("--libs", default=",".join(DEFAULT_LIBS))
    ap.add_argument("--min-recall", type=float, default=0.0,
                    help="exit non-zero if any sample scores below this")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    xdk = Path(a.xdk)
    if not (xdk / "xbox" / "bin" / "vc7" / "CL.Exe").exists():
        sys.exit(f"no CL.Exe under {xdk}\\xbox\\bin\\vc7 -- is this a stripped SDK?")
    if not (xdk / "xbox" / "include").is_dir() or not (xdk / "xbox" / "lib").is_dir():
        sys.exit("XDK has no xbox/include or xbox/lib -- stripped SDKs cannot build")

    if a.list:
        for s in list_samples(xdk):
            print(" ", s)
        return

    repo = Path(__file__).resolve().parent.parent
    samples = a.sample or ["Tut01_CreateDevice", "Tut02_Vertices", "Tut03_Matrices"]
    libs = [x.strip() for x in a.libs.split(",") if x.strip()]

    rows, failures = [], 0
    for name in samples:
        d, srcs = find_sample(xdk, name)
        if not d:
            print(f"{name:28} SKIP (not found)")
            continue
        with tempfile.TemporaryDirectory() as td:
            wd = Path(td)
            xbe, mapf = build(xdk, srcs, wd, libs, a.verbose)
            if xbe is None:
                print(f"{name:28} BUILD FAILED: {mapf}")
                failures += 1
                continue
            res, err = score(xbe, mapf, wd, repo)
            if res is None:
                print(f"{name:28} SCORE FAILED: {err}")
                failures += 1
                continue
            rows.append((name, res))
            print(f"{name:28} recall {res['recall']:5.1f}%  precision "
                  f"{res['precision']:5.1f}%   ({res['hit']}/{res['truth']} of "
                  f"{res['detected']} detected)")
            if res["recall"] < a.min_recall:
                failures += 1

    if rows:
        r = sum(x[1]["recall"] for x in rows) / len(rows)
        p = sum(x[1]["precision"] for x in rows) / len(rows)
        print(f"\n{len(rows)} samples: mean recall {r:.1f}%, mean precision {p:.1f}%")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
