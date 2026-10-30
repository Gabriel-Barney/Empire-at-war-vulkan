#!/usr/bin/env python3
"""alo_materials.py - scan ALO models for material chunks (0x400/0x10100) and report
which effect (.fx) each submesh uses, with its parameter set, vertex format and size.

Usage: python -I alo_materials.py <out.json> <dir-or-file>...

Chunk format (see docs/formats/SPEC.md): u32 id, u32 size (bit 31 = has children).
Material parameter chunks 0x10102..0x10106 hold mini-chunks: u8 id, u8 size, payload
(01 = name, 02 = value).
"""
import json
import os
import struct
import sys


def chunks(b, off, end):
    while off + 8 <= end:
        cid, size = struct.unpack_from("<II", b, off)
        container = bool(size & 0x80000000)
        size &= 0x7FFFFFFF
        yield cid, container, off + 8, off + 8 + size
        off += 8 + size


def minis(b, off, end):
    out = {}
    while off + 2 <= end:
        mid, sz = b[off], b[off + 1]
        out[mid] = b[off + 2: off + 2 + sz]
        off += 2 + sz
    return out


def cstr(x):
    return x.split(b"\0", 1)[0].decode("latin-1")


def parse_material(b, s, e):
    mat = {"shader": None, "params": {}}
    for cid, cont, cs, ce in chunks(b, s, e):
        if cid == 0x10101:
            mat["shader"] = cstr(b[cs:ce])
        elif 0x10102 <= cid <= 0x10106:
            m = minis(b, cs, ce)
            name = cstr(m.get(1, b""))
            v = m.get(2, b"")
            if cid == 0x10102:
                val = struct.unpack("<i", v[:4])[0] if len(v) >= 4 else None
                kind = "int"
            elif cid == 0x10103:
                val = round(struct.unpack("<f", v[:4])[0], 6) if len(v) >= 4 else None
                kind = "float"
            elif cid == 0x10104:
                val = [round(x, 6) for x in struct.unpack("<3f", v[:12])] if len(v) >= 12 else None
                kind = "float3"
            elif cid == 0x10106:
                val = [round(x, 6) for x in struct.unpack("<4f", v[:16])] if len(v) >= 16 else None
                kind = "float4"
            else:
                val = cstr(v)
                kind = "texture"
            mat["params"][name] = {"kind": kind, "value": val}
    return mat


def scan_model(path):
    b = open(path, "rb").read()
    out = []
    if len(b) < 8:
        return out, "empty"
    first = struct.unpack_from("<I", b, 0)[0]
    if first == 0x900:
        return out, "particle"
    for cid, cont, s, e in chunks(b, 0, len(b)):
        if cid != 0x400 or not cont:
            continue
        mesh_name, hidden, coll = None, 0, 0
        pending = None
        for c2, cont2, s2, e2 in chunks(b, s, e):
            if c2 == 0x401:
                mesh_name = cstr(b[s2:e2])
            elif c2 == 0x402:
                hidden, coll = struct.unpack_from("<II", b, s2 + 32)
            elif c2 == 0x10100:
                pending = parse_material(b, s2, e2)
            elif c2 == 0x10000:
                vc = pc = 0
                vfmt = None
                for c3, _, s3, e3 in chunks(b, s2, e2):
                    if c3 == 0x10001:
                        vc, pc = struct.unpack_from("<II", b, s3)
                    elif c3 == 0x10002:
                        vfmt = cstr(b[s3:e3])
                sub = dict(pending or {"shader": None, "params": {}})
                sub.update({"mesh": mesh_name, "hidden": hidden, "collision": coll,
                            "vertices": vc, "triangles": pc, "vertex_format": vfmt})
                out.append(sub)
                pending = None
    return out, "model"


def main(argv):
    outp = argv[1]
    files = []
    for a in argv[2:]:
        if os.path.isdir(a):
            for root, _, fs in os.walk(a):
                files += [os.path.join(root, f) for f in fs if f.upper().endswith(".ALO")]
        else:
            files.append(a)
    # later arguments override earlier ones by basename (patch megs after base)
    by_name = {}
    for f in files:
        by_name[os.path.basename(f).upper()] = f
    res = {"models": {}, "particles": 0, "errors": []}
    for name, f in sorted(by_name.items()):
        try:
            subs, kind = scan_model(f)
        except Exception as ex:  # noqa: BLE001
            res["errors"].append([name, repr(ex)])
            continue
        if kind == "particle":
            res["particles"] += 1
            continue
        res["models"][name] = subs
    json.dump(res, open(outp, "w"), indent=0)
    print("models %d, particle systems %d, submeshes %d, errors %d" % (
        len(res["models"]), res["particles"], sum(len(v) for v in res["models"].values()),
        len(res["errors"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
