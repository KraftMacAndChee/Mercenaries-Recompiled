#!/usr/bin/env python3
import argparse, math, struct
from pathlib import Path

def snorm16(v):
    return max(-1.0, v / 32767.0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vertices", type=Path)
    ap.add_argument("constants", type=Path)
    ap.add_argument("--stride", type=int, default=20,
                    help="captured interleaved vertex stride (default: 20)")
    args = ap.parse_args()
    vb = args.vertices.read_bytes()
    cb = args.constants.read_bytes()
    stride = args.stride
    if stride < 6 or len(vb) % stride or len(cb) != 3072:
        raise SystemExit("unexpected capture layout")
    c = [struct.unpack_from("<4f", cb, i * 16) for i in range(192)]
    rel = [0,1,2,3,4,5,6,17,18,23,24,25,26,27,30,31]
    for i in rel:
        print(f"c{i:02d} = " + " ".join(f"{x:.9g}" for x in c[i]))
    rows = []
    for i in range(len(vb) // stride):
        off = i * stride
        raw_pos = struct.unpack_from("<3h", vb, off)
        p = tuple(snorm16(x) for x in raw_pos) + (1.0,)
        obj = tuple(c[30][j] * p[j] + c[31][j] for j in range(4))
        cam = tuple(sum(obj[j] * c[k][j] for j in range(4)) for k in (23,24,25))
        z = cam[2]
        q = (cam[0]*c[26][0]+z*c[27][0], cam[1]*c[26][1]+z*c[27][1], z*c[26][2]+c[27][2])
        rcp = min(max(1.0/z, 5.42101e-20), 1.884467e19)
        screen = tuple(x * rcp for x in q)
        raw = vb[off:off+stride]
        rows.append(dict(i=i, obj=obj, cam=cam, screen=screen, raw=raw))
    for key in ("obj","cam","screen"):
        for axis, label in enumerate("xyz"):
            vals = [r[key][axis] for r in rows]
            print(f"{key}.{label} = [{min(vals):.9g}, {max(vals):.9g}]")
    triangles = []
    for i in range(2, len(rows)):
        a,b,d = rows[i-2:i+1]
        ax,ay = a["screen"][:2]; bx,by=b["screen"][:2]; dx,dy=d["screen"][:2]
        area = abs((bx-ax)*(dy-ay)-(by-ay)*(dx-ax))/2
        triangles.append((area,i-2,i-1,i,a["raw"]==b["raw"],b["raw"]==d["raw"],a["raw"]==d["raw"]))
    print("top triangle areas:")
    for row in sorted(triangles, reverse=True)[:30]:
        print("  area=%10.3f verts=%3d,%3d,%3d dup=%s/%s/%s" % row)
    print("adjacent exact duplicates:")
    print("  " + " ".join(f"{i-1}-{i}" for i in range(1,len(rows)) if rows[i-1]["raw"] == rows[i]["raw"]))
    nondeg = [t for t in triangles if not any(t[4:]) and t[0] > 0.0001]
    print(f"triangles={len(triangles)} nondegenerate={len(nondeg)} max_area={max(t[0] for t in triangles):.3f}")

if __name__ == "__main__":
    main()
