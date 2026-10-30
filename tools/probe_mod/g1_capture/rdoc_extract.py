# rdoc_extract.py - runs INSIDE RenderDoc's own Python (qrenderdoc.exe --python rdoc_extract.py).
# For every .rdc listed in the JSON config named by G1_RDOC_EXTRACT it writes <capture>.summary.json:
#   textures (id, size, format, category), render passes (attachments, load ops, clear values),
#   every draw (marker path, Vulkan cull mode / front face, depth + stencil state, blend, targets, counts),
# and PNGs: the last colour target written before present ("final"), and for the heat pass the colour target
# just before and just after the first draw that follows a 0x008080FF-style clear ("heat_before"/"heat_after").
# Vulkan values are what DXVK translated; g1_analyze.py maps them back using the self-test calibration.
import json
import os
import struct
import traceback

import renderdoc as rd

CFG = os.environ.get("G1_RDOC_EXTRACT", "")


def sdobj(o, depth=0):
    """Structured-data object -> plain python (bounded)."""
    n = o.NumChildren()
    if n == 0 or depth > 16:
        bt = o.type.basetype
        try:
            if bt == rd.SDBasic.Float:
                return o.AsDouble()
            if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger, rd.SDBasic.Enum):
                v = o.AsInt()
                s = o.AsString()
                return s if (bt == rd.SDBasic.Enum and s) else v
            if bt == rd.SDBasic.Boolean:
                return o.AsBool()
            if bt == rd.SDBasic.Resource:
                return str(o.AsResourceId())
            return o.AsString()
        except Exception:
            return None
    if o.type.basetype == rd.SDBasic.Array:
        return [sdobj(o.GetChild(i), depth + 1) for i in range(min(n, 64))]
    return {o.GetChild(i).name: sdobj(o.GetChild(i), depth + 1) for i in range(n)}


def find_key(obj, key, acc):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == key:
                acc.append(v)
            find_key(v, key, acc)
    elif isinstance(obj, list):
        for v in obj:
            find_key(v, key, acc)
    return acc


def floats_of(v):
    out = []
    if isinstance(v, dict):
        for x in v.values():
            out += floats_of(x)
    elif isinstance(v, list):
        for x in v:
            out += floats_of(x)
    else:
        try:
            out.append(float(v))
        except Exception:
            pass
    return out


def clear_colour(cv):
    """VkClearValue -> 4 floats (RenderDoc stores the union's float32 view as raw 32-bit words)."""
    vals = []
    try:
        vals = cv["color"]["uint32"]  # the float32 view does not convert; reinterpret the raw words instead
    except Exception:
        vals = floats_of(cv)[:4]
    out = []
    for v in vals[:4]:
        if isinstance(v, int):
            v = struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]
        try:
            out.append(round(float(v), 6))
        except Exception:
            out.append(None)
    return out


def enum_name(v):
    s = str(v)
    return s.split(".")[-1]


def save_png(ctrl, rid, path):
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.destType = rd.FileType.PNG
    ts.mip = 0
    ts.slice.sliceIndex = 0
    ts.alpha = rd.AlphaMapping.Discard
    ok = ctrl.SaveTexture(ts, path)
    return bool(ok.OK()) if hasattr(ok, "OK") else bool(ok)


def extract(path, out_dir):
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    if not res.OK():
        return {"capture": path, "error": "open failed: %s" % res}
    if cap.LocalReplaySupport() != rd.ReplaySupport.Supported:
        return {"capture": path, "error": "replay not supported here"}
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if not res.OK():
        return {"capture": path, "error": "replay failed: %s" % res}
    sf = ctrl.GetStructuredFile()
    base = os.path.join(out_dir, os.path.splitext(os.path.basename(path))[0])
    summary = {"capture": path, "api": enum_name(ctrl.GetAPIProperties().pipelineType), "textures": [], "passes": [],
               "draws": [], "images": {}}
    for t in ctrl.GetTextures():
        summary["textures"].append({"id": str(t.resourceId), "w": t.width, "h": t.height, "format": t.format.Name(),
                                    "category": str(t.creationFlags)})

    flat = []

    def walk(acts, markers):
        for a in acts:
            name = a.GetName(sf)
            flat.append((a, markers))
            if len(a.children):
                walk(a.children, markers + [name])

    walk(ctrl.GetRootActions(), [])

    heat_clear_eid = None
    heat_draw = None
    last_colour = None
    last_colour_eid = None
    for a, markers in flat:
        name = a.GetName(sf)
        if a.flags & rd.ActionFlags.BeginPass and "Clear" in name:
            clears = []
            ch = None
            if len(a.events):
                ch = sdobj(sf.chunks[a.events[-1].chunkIndex])
                for cv in find_key(ch, "clearValue", []):
                    clears.append(clear_colour(cv))
            summary["passes"].append({"eid": a.eventId, "name": name, "markers": markers, "clear_values": clears, "chunk": ch})
            for cv in clears:
                if len(cv) >= 4 and None not in cv[:4] and abs(cv[0] - 0.502) < 0.01 and abs(cv[1] - 0.502) < 0.01 and cv[2] > 0.99 and cv[3] < 0.01:
                    heat_clear_eid = a.eventId
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        ctrl.SetFrameEvent(a.eventId, True)
        vk = ctrl.GetVulkanPipelineState()
        gp = ctrl.GetPipelineState()
        ras = vk.rasterizer
        ds = vk.depthStencil

        def face(f):
            return {"func": enum_name(f.function), "pass": enum_name(f.passOperation), "fail": enum_name(f.failOperation),
                    "zfail": enum_name(f.depthFailOperation), "ref": f.reference, "cmask": f.compareMask,
                    "wmask": f.writeMask}

        blends = []
        for b in vk.colorBlend.blends:
            blends.append({"enable": b.enabled, "src": enum_name(b.colorBlend.source), "dst": enum_name(b.colorBlend.destination),
                           "op": enum_name(b.colorBlend.operation), "write": b.writeMask})
        outs = [str(o.resource) for o in gp.GetOutputTargets() if str(o.resource) != "ResourceId::0"]
        d = {"eid": a.eventId, "markers": markers[-3:], "num_indices": a.numIndices, "num_instances": a.numInstances,
             "cull": enum_name(ras.cullMode), "front_ccw": bool(ras.frontCCW), "fill": enum_name(ras.fillMode),
             "depth_bias": ras.depthBias, "slope_bias": ras.slopeScaledDepthBias,
             "depth_test": ds.depthTestEnable, "depth_write": ds.depthWriteEnable, "depth_func": enum_name(ds.depthFunction),
             "stencil": ds.stencilTestEnable, "front": face(ds.frontFace), "back": face(ds.backFace),
             "blend": blends, "outputs": outs}
        summary["draws"].append(d)
        in_blit = any(("Blit" in m) or ("blit" in m) for m in markers)
        if outs and not in_blit:
            last_colour = outs[0]
            last_colour_eid = a.eventId
        if heat_clear_eid is not None and heat_draw is None and a.eventId > heat_clear_eid and not in_blit and outs:
            heat_draw = (a.eventId, outs[0])
    images = summary["images"]
    if last_colour is not None:
        ctrl.SetFrameEvent(last_colour_eid, True)
        rid = [t.resourceId for t in ctrl.GetTextures() if str(t.resourceId) == last_colour][0]
        p = base + "_final.png"
        if save_png(ctrl, rid, p):
            images["final"] = {"path": p, "eid": last_colour_eid, "texture": last_colour}
    if heat_draw is not None:
        eid, tex = heat_draw
        rid = [t.resourceId for t in ctrl.GetTextures() if str(t.resourceId) == tex][0]
        prev = max(e for e in [x.eventId for x, _ in flat] if e < eid)
        ctrl.SetFrameEvent(prev, True)
        p0 = base + "_heat_before.png"
        ctrl.SetFrameEvent(eid, True)
        p1 = base + "_heat_after.png"
        ctrl.SetFrameEvent(prev, True)
        ok0 = save_png(ctrl, rid, p0)
        ctrl.SetFrameEvent(eid, True)
        ok1 = save_png(ctrl, rid, p1)
        if ok0 and ok1:
            images["heat_before"] = {"path": p0, "eid": prev, "texture": tex}
            images["heat_after"] = {"path": p1, "eid": eid, "texture": tex}
        summary["heat_clear_eid"] = heat_clear_eid
    ctrl.Shutdown()
    cap.Shutdown()
    with open(base + ".summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    return {"capture": path, "summary": base + ".summary.json", "draws": len(summary["draws"]), "images": list(images)}


def main():
    cfg = json.load(open(CFG, "r", encoding="utf-8-sig"))
    results = []
    for c in cfg["captures"]:
        try:
            results.append(extract(c, cfg["out_dir"]))
        except Exception:
            results.append({"capture": c, "error": traceback.format_exc()})
    with open(os.path.join(cfg["out_dir"], "rdoc_extract_status.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
    return 0 if all("error" not in r for r in results) else 1


code = 2
try:
    code = main()
except Exception:
    try:
        with open(CFG + ".error.txt", "w") as f:
            f.write(traceback.format_exc())
    except Exception:
        pass
os._exit(code)
