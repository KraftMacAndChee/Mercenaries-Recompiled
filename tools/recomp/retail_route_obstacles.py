"""Retail-only obstacle evidence for project-selected diagnostic detours.

These detours remain project choices, not original road nodes. Reading their
current values verifies their constraints; it does not fabricate their origin.
"""
from pathlib import Path
import hashlib
import itertools
import struct
from tools.diagnostics.inspect_retail_templates import dsk_records,string_table,hash_string
from tools.diagnostics.inspect_retail_script import chunks
from tools.diagnostics.inspect_retail_model_bounds import model_records

MODELS={"dmz_fence","global_roadblock01","dmz_lowwall01","dmz_lowwall02",
        "dmz_lowwallcorner01","global_guardrail","allies_hq"}
WORLDS={"sw","swn","swn_allies0_enc_roadblock"}

def obstacles(data):
    data=Path(data)
    raw=(data/"assets.dsk").read_bytes()
    if hashlib.sha256(raw).hexdigest()!="8b8ee254a245490ea4cda54ccea05b7a7a51afc41125aaf3cf5fd8ec512d1610":
        raise ValueError("Unsupported retail world archive")
    rows=[]
    for record,key,kind,payload in dsk_records(data/"assets.dsk"):
        if kind!=0x37A3E893:continue
        for tag,world in chunks(dict(chunks(payload))[b"ucfb"]):
            if tag!=b"wrld":continue
            fields=list(chunks(world));one=dict(fields)
            name=one[b"NAME"].rstrip(b"\0").decode("ascii")
            if name not in WORLDS:continue
            table=string_table(one[b"TABL"]);ordinal=0
            for tag,body in fields:
                if tag!=b"inst":continue
                parts=dict(chunks(body));model,obj=struct.unpack_from("<II",parts[b"PROP"])
                model=table[model];matrix=struct.unpack("<12f",parts[b"XFRM"])
                if model.lower() in MODELS:
                    rows.append(dict(record=record,world=name,instance=ordinal,model=model,matrix=matrix))
                ordinal+=1
    models=model_records(data,{hash_string(r["model"]) for r in rows})
    for row in rows:
        model=models[hash_string(row["model"])];matrix=row["matrix"]
        # Enclose both compiled boxes. XFRM stores three basis rows and position.
        corners=[]
        for box in model["boxes"]:
            for point in itertools.product(*[(box[j],box[j+3]) for j in range(3)]):
                corners.append([matrix[9+i]+sum(point[j]*matrix[j*3+i] for j in range(3)) for i in range(3)])
        row["bounds"]=[min(p[i] for p in corners) for i in range(3)]+[max(p[i] for p in corners) for i in range(3)]
        row["model_archive"]=model["archive"];row["model_record"]=model["record_index"]
        row["model_info_sha256"]=hashlib.sha256(model["info"]).hexdigest()
    return rows
