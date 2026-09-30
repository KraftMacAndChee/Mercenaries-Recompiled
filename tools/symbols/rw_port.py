"""Port RenderWare function names from a PE .exe donor (with matching MAP) onto
a target XBE by byte-matching the statically-linked RenderWare library code.

Unlike XDK libraries (own sections: D3D, DSOUND...), RenderWare is linked into
.text mixed with game code, so we match within .text and keep only names that
land on a detected function start in the target. Donor is the .exe because
Xyanide's XBE is a different build from its MAP; the .exe matches the MAP and
its .text bytes are what imagebld would copy into an XBE.

IMPORTANT -- the donor and target must use the SAME RenderWare version.
Microsoft's XDK libraries are byte-stable across versions, so map_names.py port
works across XDK versions; RenderWare is NOT. Tried against Burnout 3 (RW36,
release), Xyanide (an earlier RW) matched 1 of 908 -- the versions diverge too
far for a byte signature. Check the embedded "//RenderWare/RWxxActive/" tag in
both binaries and only port between matching xx. Burnout 3 is RW36; we have no
RW36 symbol-bearing donor, so this could not name it.
"""
import json
import re
import struct
import sys

SIG_LEN = 12
# RenderWare naming: Rw=core, Rp=plugin, Rt=toolkit. Decorated forms too.
RW_RE = re.compile(r"^[_@]?((?:Rw|Rp|Rt)[A-Za-z0-9_]+)(?:@\d+)?$")
# MAP public line: " 0001:00000000  name  00400700 f i OBJ"
SYM_RE = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s", re.I)
SEC_RE = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+([0-9a-f]+)H\s+(\S+)\s+CODE", re.I)

donor_exe, donor_map, target_xbe, target_analysis, target_funcs, out = sys.argv[1:7]

# ---- Parse the PE .exe: section table (segment index -> va, file offset) ----
exe = open(donor_exe, "rb").read()
e_lfanew = struct.unpack_from("<I", exe, 0x3C)[0]
assert exe[e_lfanew:e_lfanew + 4] == b"PE\0\0"
coff = e_lfanew + 4
n_sec, = struct.unpack_from("<H", exe, coff + 2)
opt_sz, = struct.unpack_from("<H", exe, coff + 16)
opt = coff + 20
image_base, = struct.unpack_from("<I", exe, opt + 28)
pe_secs = []  # 1-based: (va, vsize, raw_off, raw_sz)
for i in range(n_sec):
    o = opt + opt_sz + i * 40
    vsz, va, rsz, ra = struct.unpack_from("<IIII", exe, o + 8)
    pe_secs.append((va, vsz, ra, rsz))

# ---- Parse the MAP: RenderWare publics -> file offset in the .exe ----
# MAP section N corresponds to PE section N (linker emitted both for this .exe).
lines = open(donor_map, errors="replace").read().splitlines()
rw_funcs = []  # (name, file_off)
for ln in lines:
    m = SYM_RE.match(ln)
    if not m:
        continue
    seg, off, name = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
    rm = RW_RE.match(name)
    if not rm or seg == 0 or seg > len(pe_secs):
        continue
    va, vsz, ra, rsz = pe_secs[seg - 1]
    if off < rsz:
        rw_funcs.append((rm.group(1), ra + off))

print(f"RenderWare publics in MAP: {len(rw_funcs)}")

# ---- Target XBE: .text bytes + detected function starts ----
a = json.load(open(target_analysis))
traw = open(target_xbe, "rb").read()
text = next(s for s in a["sections"] if s["name"] == ".text")
tva = int(text["virtual_addr"], 16)
tra = int(text["raw_addr"], 16)
trsz = text["raw_size"]
tblob = traw[tra:tra + trsz]

tf = json.load(open(target_funcs))
if isinstance(tf, dict):
    tf = tf.get("functions", list(tf.values())[0])
starts = {int(f["start"], 16) for f in tf}

# ---- Match each RW signature into the target .text, uniquely, on a start ----
def find_all(blob, sig, limit=2):
    out, i = [], blob.find(sig)
    while i >= 0 and len(out) < limit:
        out.append(i)
        i = blob.find(sig, i + 1)
    return out

names, stats = {}, {"unique": 0, "ambiguous": 0, "absent": 0, "unusable": 0, "off_start": 0}
for name, foff in rw_funcs:
    sig = exe[foff:foff + SIG_LEN]
    if len(sig) < SIG_LEN or sig.count(0) > SIG_LEN // 2:
        stats["unusable"] += 1
        continue
    hits = find_all(tblob, sig)
    if not hits:
        stats["absent"] += 1
    elif len(hits) > 1:
        stats["ambiguous"] += 1
    else:
        stats["unique"] += 1
        va = tva + hits[0]
        if va in starts:
            names[va] = name
        else:
            stats["off_start"] += 1

print(f"  unique: {stats['unique']}  ambiguous: {stats['ambiguous']}  "
      f"absent: {stats['absent']}  unusable: {stats['unusable']}  "
      f"off-start: {stats['off_start']}")
if stats["unique"]:
    print(f"  kept {len(names)} names on function starts "
          f"({100*len(names)/stats['unique']:.1f}% of unique)")
json.dump({f"{v:#010x}": n for v, n in sorted(names.items())}, open(out, "w"), indent=1)
print(f"wrote {len(names)} -> {out}")
for v, n in list(sorted(names.items()))[:12]:
    print(f"    {v:#010x}  {n}")
