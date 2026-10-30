#!/usr/bin/env python3
"""merge_alo.py - combine ALO material scans into one alo_materials.py-format JSON.

  python -I merge_alo.py <out.json> <input>...

Inputs are applied in order and later ones shadow earlier ones by ALO basename (load-order:
list the lowest-priority archive first). An input is either
  * an alo_materials.py output ({"models": {NAME: [submesh...]}}), or
  * an assets-stream export ({"source", "alo_count", "shader_usage", "errors",
    "alo": {PATH: {"meshes": [{"name", "materials": [{"shader", "params"}]}]}}}).
    These carry no per-mesh hidden flag or vertex counts: `hidden` is set to null.
"""
import json
import os
import sys

KIND = {"int": "int", "float": "float", "float3": "float3", "float4": "float4", "texture": "texture"}


def load(path):
    d = json.load(open(path))
    if "models" in d:
        return d["models"], d.get("particles", 0)
    models = {}
    for p, m in d["alo"].items():
        name = os.path.basename(p.replace("\\", "/")).upper()
        subs = []
        for mesh in m.get("meshes", []):
            for mat in mesh.get("materials", []):
                params = {k: {"kind": KIND.get(v.get("type"), v.get("type")), "value": v.get("value")}
                          for k, v in (mat.get("params") or {}).items()}
                subs.append({"shader": mat.get("shader"), "params": params, "mesh": mesh.get("name"),
                             "hidden": None, "collision": None, "vertices": None, "triangles": None,
                             "vertex_format": None})
        if subs or m.get("meshes") is not None:
            models[name] = subs
    return models, None


def main(argv):
    out, ins = argv[1], argv[2:]
    models, parts, src = {}, 0, []
    for p in ins:
        m, pc = load(p)
        shadowed = len(set(m) & set(models))
        models.update(m)
        parts += pc or 0
        src.append({"input": os.path.basename(p), "models": len(m), "shadowed_earlier": shadowed})
    json.dump({"models": models, "particles": parts, "sources": src, "errors": []}, open(out, "w"))
    print("merged %d models from %s" % (len(models), src))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
