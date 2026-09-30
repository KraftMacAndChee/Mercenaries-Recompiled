"""Name a target XBE's statically-linked library functions by byte-matching a
COFF .lib (which carries function name + code together -- no MAP needed).

Parses the archive's object members, pulls each function's opening bytes from
its .text, and searches for that signature in the target's .text, keeping
unique matches that land on a detected function start. Used here to test
whether RenderWare 3.7 (rwcore.lib) matches Burnout 3's RW36.
"""
import json
import struct
import sys

SIG_LEN = 12
lib_path, target_xbe, target_analysis, target_funcs, out = sys.argv[1:6]

# ---- Parse the COFF archive into object-file blobs ----
raw = open(lib_path, "rb").read()
assert raw[:8] == b"!<arch>\n", "not a COFF archive"
objs = []
i = 8
longnames = b""
while i + 60 <= len(raw):
    name = raw[i:i + 16].rstrip()
    size = int(raw[i + 48:i + 58].strip())
    data = raw[i + 60:i + 60 + size]
    if name == b"//":
        longnames = data
    elif name not in (b"/", b"/0"):
        objs.append(data)
    i += 60 + size + (size & 1)


def parse_obj(obj):
    """Yield (func_name, sig_bytes) for external code symbols in one COFF obj."""
    if len(obj) < 20:
        return
    machine, nsec, _ts, symoff, nsym = struct.unpack_from("<HHIII", obj, 0)
    if machine != 0x14C:  # IMAGE_FILE_MACHINE_I386
        return
    opt, = struct.unpack_from("<H", obj, 16)
    sec_base = 20 + opt
    sections = []  # 1-based: (name, raw_ptr, raw_size)
    for s in range(nsec):
        o = sec_base + s * 40
        sname = obj[o:o + 8].rstrip(b"\0")
        vsz, va, rsz, ra = struct.unpack_from("<IIII", obj, o + 8)
        sections.append((sname, ra, rsz))
    strtab_off = symoff + nsym * 18

    def symname(rec):
        if rec[:4] == b"\0\0\0\0":
            off, = struct.unpack_from("<I", rec, 4)
            e = obj.index(b"\0", strtab_off + off)
            return obj[strtab_off + off:e].decode("latin1")
        return rec[:8].rstrip(b"\0").decode("latin1")

    for s in range(nsym):
        rec = obj[symoff + s * 18:symoff + s * 18 + 18]
        value, secnum, typ, cls = struct.unpack_from("<IhHBx", rec[8:18].ljust(10, b"\0"))
        # 0x20 = function (msb of type); class 2 = external
        secnum, = struct.unpack_from("<h", rec, 12)
        typ, = struct.unpack_from("<H", rec, 14)
        cls = rec[16]
        value, = struct.unpack_from("<I", rec, 8)
        if cls == 2 and secnum > 0 and secnum <= len(sections):
            sname, ra, rsz = sections[secnum - 1]
            if sname != b".text":
                continue
            off = ra + value
            sig = obj[off:off + SIG_LEN]
            nm = symname(rec)
            if len(sig) == SIG_LEN and sig.count(0) <= SIG_LEN // 2:
                yield nm, sig


lib_sigs = {}  # name -> sig
for obj in objs:
    for nm, sig in parse_obj(obj):
        lib_sigs.setdefault(nm, sig)
print(f"library functions with usable signatures: {len(lib_sigs)}")

# ---- Target .text ----
a = json.load(open(target_analysis))
traw = open(target_xbe, "rb").read()
text = next(s for s in a["sections"] if s["name"] == ".text")
tva = int(text["virtual_addr"], 16)
tra = int(text["raw_addr"], 16)
tblob = traw[tra:tra + text["raw_size"]]
tf = json.load(open(target_funcs))
if isinstance(tf, dict):
    tf = tf.get("functions", list(tf.values())[0])
starts = {int(f["start"], 16) for f in tf}


def find2(blob, sig):
    out, i = [], blob.find(sig)
    while i >= 0 and len(out) < 2:
        out.append(i)
        i = blob.find(sig, i + 1)
    return out


names, uniq, amb, absent, offstart = {}, 0, 0, 0, 0
for nm, sig in lib_sigs.items():
    h = find2(tblob, sig)
    if not h:
        absent += 1
    elif len(h) > 1:
        amb += 1
    else:
        uniq += 1
        va = tva + h[0]
        if va in starts:
            names[va] = nm.lstrip("_").split("@")[0]
        else:
            offstart += 1

print(f"unique {uniq}  ambiguous {amb}  absent {absent}  off-start {offstart}")
print(f"kept {len(names)} names on function starts")
json.dump({f"{v:#010x}": n for v, n in sorted(names.items())}, open(out, "w"), indent=1)
for v, n in list(sorted(names.items()))[:15]:
    print(f"    {v:#010x}  {n}")
