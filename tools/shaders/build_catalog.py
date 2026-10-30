#!/usr/bin/env python3
"""build_catalog.py - build the shader catalog + per-shader decompilation outputs.

Inputs (all paths are arguments, nothing is executed from the data dirs):
  --raw DIR        extracted FXOs: DIR/<game>/<meg>/DATA/ART/SHADERS/...FXO
                   (game = foc|eaw, meg = shaders|patch|patch2|64patch, case-insensitive)
  --alo GAME=JSON  output of alo_materials.py for that game (repeatable)
  --exe GAME=PATH  StarWarsG.exe, used only to test which effect names are literal strings
  --out DIR        output root; writes DIR/catalog.json, DIR/CATALOG.md and
                   DIR/decompiled/<game>/<EFFECT>/{effect.fx.txt,*.asm,*.hlsl}

Override order (later wins), per game: shaders.meg < Patch.meg < Patch2.meg < 64Patch.meg.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9sm  # noqa: E402
import fx2  # noqa: E402
import lift  # noqa: E402

MEG_ORDER = ["shaders", "patch", "patch2", "64patch"]


def collect_fxos(raw):
    games = {}
    for game in sorted(os.listdir(raw)):
        final = {}
        for meg in sorted(os.listdir(os.path.join(raw, game)), key=lambda m: MEG_ORDER.index(m.lower())):
            root = os.path.join(raw, game, meg)
            for dp, _, fs in os.walk(root):
                for f in fs:
                    if not f.upper().endswith(".FXO"):
                        continue
                    full = os.path.join(dp, f)
                    rel = os.path.relpath(full, root).replace(os.sep, "\\").upper()
                    prev = final.get(rel)
                    final[rel] = {"path": full, "meg": meg, "rel": rel,
                                  "overrides": (prev["overrides"] + [prev["meg"]]) if prev else []}
        games[game] = final
    return games


def stem(rel):
    return os.path.splitext(rel.split("\\")[-1])[0].upper()


def fmt_val(v):
    if isinstance(v, float):
        return "%.6g" % v
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, list):
        return "{" + ", ".join(fmt_val(x) for x in v) + "}"
    return str(v)


def state_label(st):
    n = st["name"]
    if st["class"] == "render":
        n = n.title().replace("_", "") if False else n
    if st["class"] in ("texstage", "sampler", "light", "lightenable", "transform") or (
            st["class"] in ("texture", "setsampler") and st["index"]):
        return "%s[%d]" % (n, st["index"])
    return n


class EffectInfo:
    def __init__(self, game, ent, outdir):
        self.game = game
        self.ent = ent
        self.data = open(ent["path"], "rb").read()
        self.eff = fx2.parse(self.data)
        self.name = stem(ent["rel"])
        self.outdir = outdir
        self.blobs = {}          # id(bytes)/hash -> info
        self.unknown_ops = collections.Counter()

    # -------------------------------------------------- shader blobs
    def blob_info(self, data, label):
        h = hashlib.sha1(data).hexdigest()[:12]
        if h in self.blobs:
            return self.blobs[h]
        sh = d3d9sm.parse(data)
        for k, v in sh.unknown_opcodes.items():
            self.unknown_ops[k] += v
        fn = re.sub(r"[^A-Za-z0-9_.-]", "_", label)
        used = {b["file"] for b in self.blobs.values()}
        base, k = fn, 2
        while fn in used:
            fn = "%s_%d" % (base, k)
            k += 1
        info = {"hash": h, "label": label, "file": fn, "profile": sh.profile,
                "instructions": sh.instruction_count(),
                "ctab": [{"name": c["name"], "set": c["regset"], "reg": c["reg"], "count": c["count"]}
                         for c in (sh.ctab["constants"] if sh.ctab else [])],
                "preshader_instructions": len(sh.preshader.instructions) if sh.preshader else 0,
                "unknown_opcodes": {str(k): v for k, v in sh.unknown_opcodes.items()},
                "end_ok": sh.ended and sh.trailing == 0}
        d = os.path.join(self.outdir, "decompiled", self.game, self.name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, fn + ".asm"), "w") as f:
            f.write("// %s :: %s  (sha1 %s)\n" % (self.name, label, h))
            f.write(sh.asm())
        with open(os.path.join(d, fn + ".hlsl"), "w") as f:
            f.write(lift.lift(sh, "%s::%s" % (self.name, label)))
        self.blobs[h] = info
        return info

    # -------------------------------------------------- state values
    def state_value(self, st):
        res = st.get("resolved")
        if res:
            k = res["kind"]
            if k == "parameter":
                return {"param": res["name"]}, "<%s>" % res["name"]
            if k == "fxlc":
                ps = d3d9sm.parse_fxlc(res["data"])
                txt = " ".join(d3d9sm.preshader_pseudo(ps, "expr"))
                return {"expr": txt}, "expr{ %s }" % txt
            if k == "shader":
                ctx = getattr(self, "_ctx", "")
                info = self.blob_info(res["data"], "%s%s" % (ctx, "vs" if st["name"] == "VertexShader" else "ps"))
                return {"shader": info["hash"], "profile": info["profile"]}, "asm(%s %s)" % (info["profile"], info["file"])
            return {"raw": k}, "<%s>" % k
        v = st["value"]
        if isinstance(v, dict) and "object" in v:
            return {"object": v["object"]}, "NULL" if st["class"] in ("vertexshader", "pixelshader") else "<object %d>" % v["object"]
        en = fx2.enum_name(st["name"], v)
        if en:
            return {"value": v, "enum": en}, en
        if st["class"] in ("vertexshader", "pixelshader") and v == 0:
            return {"value": 0}, "NULL"
        return {"value": v}, fmt_val(v)

    # -------------------------------------------------- build
    def build(self):
        e = self.eff
        par_by_name = {p["name"]: p for p in e.parameters}
        J = {"game": self.game, "effect": self.name, "path": self.ent["rel"], "meg": self.ent["meg"],
             "overrides": self.ent["overrides"], "bytes": len(self.data),
             "sha1": hashlib.sha1(self.data).hexdigest(), "alamo": {}, "parameters": [],
             "material_parameters": [], "engine_parameters": [], "textures": [], "samplers": [],
             "shader_objects": [], "techniques": [], "warnings": e.warnings}
        fx = ["// %s  (%s, from %s%s)" % (self.ent["rel"], self.game, self.ent["meg"],
                                          (", overrides " + "+".join(self.ent["overrides"])) if self.ent["overrides"] else ""),
              "// reconstructed from the compiled fx_2_0 effect by tools/shaders/fx2.py", ""]
        for p in e.parameters:
            t = p["typedef"]
            ann = {a["name"]: e.value_repr(a["value"]) for a in p["annotations"]}
            val = p["value"]
            ty = t["type"]
            entry = {"name": p["name"], "semantic": p["semantic"], "type": p["type"],
                     "elements": t["elements"], "annotations": ann}
            if t["name"].startswith("_ALAMO_"):
                J["alamo"][t["name"][7:].lower()] = e.value_repr(val)
            if ty in fx2.SAMPLER_TYPES:
                states, tex = {}, None
                svals = val if isinstance(val, list) else [val]
                for st in svals[0]["sampler_states"]:
                    jv, txt = self.state_value(st)
                    if st["name"] == "Texture":
                        tex = jv.get("param")
                    else:
                        states[st["name"]] = txt
                J["samplers"].append({"name": p["name"], "type": p["type"], "texture": tex, "states": states})
                fx.append("%s %s = sampler_state { %s };" % (
                    p["type"], p["name"], " ".join(["Texture = <%s>;" % tex] + ["%s = %s;" % kv for kv in states.items()])))
                continue
            if ty in fx2.SHADER_TYPES:
                vals = val if isinstance(val, list) else [val]
                for i, v in enumerate(vals):
                    o = e.objects.get(v["object"], {})
                    if o.get("data"):
                        lab = p["name"] + ("[%d]" % i if isinstance(val, list) else "")
                        info = self.blob_info(o["data"], lab)
                        J["shader_objects"].append({"name": lab, "hash": info["hash"], "profile": info["profile"],
                                                    "instructions": info["instructions"],
                                                    "preshader_instructions": info["preshader_instructions"]})
                        fx.append("%s %s = asm { /* %s, %d instr%s -> %s.asm */ };" % (
                            "VertexShader" if ty == 16 else "PixelShader", lab, info["profile"],
                            info["instructions"],
                            ", preshader %d" % info["preshader_instructions"] if info["preshader_instructions"] else "",
                            info["file"]))
                continue
            if ty in fx2.TEXTURE_TYPES:
                J["textures"].append({"name": p["name"], "semantic": p["semantic"], "annotations": ann})
            else:
                entry["default"] = val if not (isinstance(val, dict)) else e.value_repr(val)
            if p["semantic"]:
                J["engine_parameters"].append(p["semantic"])
            elif not t["name"].startswith("_ALAMO_") and ty not in fx2.TEXTURE_TYPES:
                J["material_parameters"].append(p["name"])
            J["parameters"].append(entry)
            anns = "".join(" <%s>" % "; ".join("%s=%s" % (k, fmt_val(v)) for k, v in ann.items())) if ann else ""
            arr = "[%d]" % t["elements"] if t["elements"] else ""
            sem = " : %s" % p["semantic"] if p["semantic"] else ""
            dv = e.value_repr(val) if isinstance(val, dict) else val
            show = "" if ty in fx2.TEXTURE_TYPES else " = %s" % (
                '"%s"' % dv if isinstance(dv, str) else fmt_val(dv))
            if t["elements"] and isinstance(val, list) and len(str(val)) > 200:
                show = " = { %d elements }" % t["elements"]
            fx.append("%s %s%s%s%s%s;" % (p["type"], p["name"], arr, sem, anns, show))
        fx.append("")
        for t in e.techniques:
            anns = {a["name"]: e.value_repr(a["value"]) for a in t["annotations"]}
            tj = {"name": t["name"], "lod": anns.get("LOD"), "annotations": anns, "passes": []}
            fx.append("technique %s%s" % (t["name"], (" <%s>" % "; ".join('%s="%s"' % kv for kv in anns.items())) if anns else ""))
            fx.append("{")
            for ps in t["passes"]:
                pj = {"name": ps["name"], "render": {}, "texstage": {}, "sampler": {}, "texture": {},
                      "material": {}, "other": {}, "vs": None, "ps": None, "expressions": {}}
                fx.append("    pass %s" % ps["name"])
                self._ctx = "%s.%s." % (t["name"], ps["name"])
                fx.append("    {")
                for st in ps["states"]:
                    jv, txt = self.state_value(st)
                    lab = state_label(st)
                    fx.append("        %s = %s;" % (lab, txt))
                    if "expr" in jv:
                        pj["expressions"][lab] = jv["expr"]
                    cls = st["class"]
                    if cls in ("vertexshader", "pixelshader"):
                        key = "vs" if cls == "vertexshader" else "ps"
                        if "param" in jv:
                            so = next((s for s in J["shader_objects"] if s["name"] == jv["param"]), None)
                            pj[key] = {"object": jv["param"], "hash": so and so["hash"],
                                       "profile": so and so["profile"],
                                       "instructions": so and so["instructions"]}
                        elif "shader" in jv:
                            info = self.blobs[jv["shader"]]
                            pj[key] = {"object": info["label"], "hash": info["hash"], "profile": info["profile"],
                                       "instructions": info["instructions"]}
                        else:
                            pj[key] = None   # fixed function
                        continue
                    bucket = {"render": "render", "texstage": "texstage", "sampler": "sampler",
                              "texture": "texture", "material": "material"}.get(cls, "other")
                    pj[bucket][lab] = jv.get("enum", jv.get("value", jv.get("param", jv.get("expr"))))
                fx.append("    }")
                tj["passes"].append(pj)
            fx.append("}")
            fx.append("")
            J["techniques"].append(tj)
        # derived: skinning palette, lights
        for p in e.parameters:
            sem = p["semantic"].upper()
            t = p["typedef"]
            if "SKIN" in sem or "SKIN" in p["name"].upper():
                if t["class"] in (2, 3) or t["elements"]:
                    J["skinning"] = {"param": p["name"], "semantic": p["semantic"], "type": p["type"],
                                     "elements": t["elements"], "rows": t.get("rows"), "columns": t.get("columns")}
        regs = [c for b in self.blobs.values() for c in b["ctab"] if J.get("skinning") and c["name"] == J["skinning"]["param"]]
        if regs:
            J["skinning"]["registers_used_max"] = max(c["count"] for c in regs)
        J["lights"] = sorted({p["semantic"] for p in e.parameters if "LIGHT" in p["semantic"].upper()})
        J["shaders"] = list(self.blobs.values())
        d = os.path.join(self.outdir, "decompiled", self.game, self.name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "effect.fx.txt"), "w") as f:
            f.write("\n".join(fx) + "\n")
        return J


def pass_key_summary(pj):
    r = pj["render"]
    out = []
    ab = r.get("ALPHABLENDENABLE")
    exprs = pj["expressions"]
    if "ALPHABLENDENABLE" in exprs:
        out.append("blend=dyn(%s)" % exprs["ALPHABLENDENABLE"].replace("expr0.x = ", "").rstrip(";"))
    elif ab in (1, True):
        out.append("blend %s/%s" % (r.get("SRCBLEND", "?"), r.get("DESTBLEND", "?")))
    elif ab in (0, False):
        out.append("opaque")
    if r.get("ALPHATESTENABLE") in (1, True):
        out.append("atest %s %s" % (r.get("ALPHAFUNC", ""), r.get("ALPHAREF", "")))
    if "ZWRITEENABLE" in r:
        out.append("zw=%s" % r["ZWRITEENABLE"])
    if "ZENABLE" in r:
        out.append("z=%s" % r["ZENABLE"])
    if "ZFUNC" in r and r["ZFUNC"] != "LESSEQUAL":
        out.append("zf=%s" % r["ZFUNC"])
    if "CULLMODE" in r:
        out.append("cull=%s" % r["CULLMODE"])
    if r.get("STENCILENABLE") in (1, True) or r.get("TWOSIDEDSTENCILMODE") in (1, True):
        out.append("stencil")
    if "COLORWRITEENABLE" in r:
        out.append("cw=%s" % r["COLORWRITEENABLE"])
    if "BLENDOP" in r and r["BLENDOP"] != "ADD":
        out.append("op=%s" % r["BLENDOP"])
    return " ".join(out)


NOT_DRAWN = {"ALDEFAULT", "DEFAULT", "MESHCOLLISION"}


def _rnd(v):
    if isinstance(v, float):
        return round(v, 5)
    if isinstance(v, list):
        return [_rnd(x) for x in v]
    return v


def alo_usage(alo_json):
    """Per-shader (upper stem) usage stats. Submeshes whose hidden flag is unknown (sources
    without it) are counted as visible unless their effect is a non-drawn one (NOT_DRAWN)."""
    d = json.load(open(alo_json))
    use = collections.defaultdict(lambda: {"submeshes": 0, "visible_submeshes": 0, "models": set(),
                                           "triangles": 0, "param_sets": collections.Counter(),
                                           "const_sets": collections.Counter(),
                                           "params_seen": collections.Counter(),
                                           "vertex_formats": collections.Counter()})
    for model, subs in d["models"].items():
        for s in subs:
            sh = s["shader"] or "<none>"
            u = use[os.path.splitext(sh)[0].upper()]
            u["submeshes"] += 1
            u["models"].add(model)
            u["vertex_formats"][s["vertex_format"]] += 1
            for k in s["params"]:
                u["params_seen"][k] += 1
            if s["hidden"] is None:
                u.setdefault("hidden_unknown", 0)
                u["hidden_unknown"] += 1
                if os.path.splitext(sh)[0].upper() in NOT_DRAWN:
                    continue
            elif s["hidden"]:
                continue
            u["visible_submeshes"] += 1
            u["triangles"] += s["triangles"] or 0
            full = json.dumps(sorted((k, _rnd(v["value"])) for k, v in s["params"].items()))
            consts = json.dumps(sorted((k, _rnd(v["value"])) for k, v in s["params"].items() if v["kind"] != "texture"))
            u["param_sets"][full] += 1
            u["const_sets"][consts] += 1
    out = {}
    for k, u in use.items():
        out[k] = {"submeshes": u["submeshes"], "visible_submeshes": u["visible_submeshes"],
                  "models": len(u["models"]), "triangles_visible": u["triangles"],
                  "unique_param_sets": len(u["param_sets"]), "unique_const_sets": len(u["const_sets"]),
                  "params_seen": dict(u["params_seen"]), "vertex_formats": dict(u["vertex_formats"]),
                  "hidden_flag_unknown": u.get("hidden_unknown", 0)}
    nomesh = sum(1 for v in d["models"].values() if not v)
    return out, len(d["models"]) - nomesh, (d.get("particles", 0) or 0) + nomesh


def exe_strings(path):
    if not path or not os.path.exists(path):
        return set()
    b = open(path, "rb").read()
    found = set(m.group(0).decode("latin-1").upper() for m in re.finditer(rb"[A-Za-z0-9_]{4,64}", b))
    return found


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True)
    ap.add_argument("--alo", action="append", default=[])
    ap.add_argument("--exe", action="append", default=[])
    ap.add_argument("--alo-combined", default=None,
                    help="merged FoC-over-EaW model set; attached to FoC effects as alo_combined")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv[1:])
    games = collect_fxos(a.raw)
    alo = {}
    alo_meta = {}
    for spec in a.alo:
        g, p = spec.split("=", 1)
        alo[g], n, parts = alo_usage(p)
        alo_meta[g] = {"models": n, "particle_systems": parts, "source": os.path.basename(p)}
    combined = {}
    if a.alo_combined:
        combined, n, parts = alo_usage(a.alo_combined)
        alo_meta["combined"] = {"models": n, "particle_systems": parts, "source": os.path.basename(a.alo_combined)}
    exes = {}
    for spec in a.exe:
        g, p = spec.split("=", 1)
        exes[g] = exe_strings(p)
    cat = {"generator": "tools/shaders/build_catalog.py", "games": {}, "alo_corpus": alo_meta}
    totals = collections.Counter()
    unknown = collections.Counter()
    for g, files in games.items():
        effs = []
        for rel in sorted(files):
            info = EffectInfo(g, files[rel], a.out)
            J = info.build()
            unknown.update(info.unknown_ops)
            J["alo"] = alo.get(g, {}).get(J["effect"])
            if combined and g == "foc":
                J["alo_combined"] = combined.get(J["effect"])
            J["exe_string_ref"] = J["effect"] in exes.get(g, set())
            for t in J["techniques"]:
                for pj in t["passes"]:
                    pj["summary"] = pass_key_summary(pj)
            effs.append(J)
            totals["effects"] += 1
            totals["techniques"] += len(J["techniques"])
            totals["passes"] += sum(len(t["passes"]) for t in J["techniques"])
            totals["shader_blobs"] += len(J["shaders"])
        # ALO shader names with no FXO
        names = {e["effect"] for e in effs}
        missing = {k: v for k, v in alo.get(g, {}).items() if k not in names}
        cat["games"][g] = {"effects": effs, "alo_shaders_without_fxo": missing}
        if combined and g == "foc":
            cat["games"][g]["combined_alo_shaders_without_fxo"] = {k: v for k, v in combined.items() if k not in names}
    cat["totals"] = dict(totals)
    cat["unknown_opcodes"] = {str(k): v for k, v in unknown.items()}
    os.makedirs(a.out, exist_ok=True)
    json.dump(cat, open(os.path.join(a.out, "catalog.json"), "w"), indent=1, default=str)
    print(json.dumps(cat["totals"]), "unknown opcodes:", cat["unknown_opcodes"] or "none")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
