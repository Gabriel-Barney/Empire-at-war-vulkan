"""g1_analyze.py - extracts the G1 facts from a finished capture run folder (run with the project venv, -I).

Inputs found in --run-dir:
  eaw.trace                apitrace D3D9 trace  -> streamed through `apitrace dump`, D3D9 state machine:
                           CULLMODE (device default + value at every draw), stencil state at stencil draws,
                           clears (heat 0x008080FF), render-target textures/surfaces and formats, StretchRects,
                           per-shader CTAB registers (from the dump's disassembly) and the value of `Colorization`
                           (and D3DRS_TEXTUREFACTOR for fixed-function passes) at every draw that uses it,
                           per-frame m_world series for planet candidates (shaders with UVOffset) -> steps per turn.
                           Heat pass: d3dretrace snapshots just before / after the first SceneHeat-like draw,
                           then the measured image shift (the 0x80 bias + half-texel term).
  *.summary.json           RenderDoc extraction (rdoc_extract.py): Vulkan cull/front-face, stencil, clears,
                           heat before/after PNGs (shift measured the same way), final image colour stats.
  shots\\*.png, *_d3d9.log DXVK run: listed, DXVK log summarised (draw counts are in the HUD screenshots).
Outputs: <run>\\analysis\\facts.json and report.md.

G1f additions (2026-10-30):
  --vb-draws A-B   the vertex-buffer bytes read by the draws in call range A-B (apitrace --blobs), decoded for the 36-byte
                   lit layout (colour / UV / position ranges per draw) -> analysis\\vb_draws_A-B.json
                   (--vs-filter TEXT: only draws whose 'vs | ps' description contains TEXT; --facts PATH)
  --compare NAME REF.png PORT.png [--regions "n=x0,y0,x1,y1;..."]   per-region colour statistics + side-by-side PNG
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import zlib
from collections import Counter, defaultdict

KIT = os.path.dirname(os.path.abspath(__file__))
CALIBRATION = os.path.join(KIT, "selftest", "calibration.json")

D3DCULL = {"1": "D3DCULL_NONE", "2": "D3DCULL_CW", "3": "D3DCULL_CCW"}
CALL_RE = re.compile(r"^(\d+) ([A-Za-z0-9_:]+)\((.*)$")
REG_RE = re.compile(r"^//\s+(\w+)\s+([cbsi])(\d+)\s+(\d+)\s*$")


# ---------------------------------------------------------------------------------------------- PNG + images
def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos, idat, w = 8, b"", 0
    h = ct = bd = 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    if bd != 8 or ct not in (0, 2, 6):
        raise ValueError("unsupported PNG (bit depth %d, colour type %d)" % (bd, ct))
    bpp = {0: 1, 2: 3, 6: 4}[ct]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    rgb = []
    for r in rows:
        if bpp == 1:
            rgb.append([(v, v, v) for v in r])
        else:
            rgb.append([(r[x], r[x + 1], r[x + 2]) for x in range(0, stride, bpp)])
    return w, h, rgb


def colour_stats(path):
    w, h, px = read_png(path)
    def mean(region):
        n = 0
        s = [0, 0, 0]
        for y0, y1, x0, x1 in region:
            for y in range(y0, y1):
                for x in range(x0, x1):
                    p = px[y][x]
                    s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; n += 1
        return [round(v / max(n, 1), 2) for v in s]
    full = mean([(0, h, 0, w)])
    centre = mean([(h // 3, 2 * h // 3, w // 3, 2 * w // 3)])
    lit = [0, 0, 0]
    n = 0
    for row in px:
        for p in row:
            if p[0] + p[1] + p[2] > 60:
                lit[0] += p[0]; lit[1] += p[1]; lit[2] += p[2]; n += 1
    lit = [round(v / max(n, 1), 2) for v in lit]
    def ratio(m):
        return round(m[2] / max((m[0] + m[1]) / 2.0, 1e-6), 4)
    return {"size": [w, h], "mean_rgb": full, "centre_mean_rgb": centre, "lit_mean_rgb": lit, "lit_pixels": n,
            "blue_ratio_full": ratio(full), "blue_ratio_lit": ratio(lit)}


def luma_crop(path, size=192):
    w, h, px = read_png(path)
    s = min(size, w - 16, h - 16)
    x0, y0 = (w - s) // 2, (h - s) // 2
    return [[0.299 * px[y][x][0] + 0.587 * px[y][x][1] + 0.114 * px[y][x][2] for x in range(x0 - 8, x0 + s + 8)]
            for y in range(y0 - 8, y0 + s + 8)], s


def sample(img, x, y):
    x0, y0 = int(x // 1), int(y // 1)
    fx, fy = x - x0, y - y0
    r0, r1 = img[y0], img[y0 + 1]
    return (r0[x0] * (1 - fx) + r0[x0 + 1] * fx) * (1 - fy) + (r1[x0] * (1 - fx) + r1[x0 + 1] * fx) * fy


def measure_shift(before_png, after_png):
    """Shift (dx, dy) in pixels such that after(x, y) ~ before(x + dx, y + dy), i.e. the sampling offset the
    full-screen pass applied. Integer search -4..4, then 0.05-pixel refinement per axis (bilinear)."""
    a, s = luma_crop(before_png)
    b, _ = luma_crop(after_png)

    def ssd(dx, dy, step=3):
        e = 0.0
        for y in range(8, 8 + s, step):
            for x in range(8, 8 + s, step):
                d = b[y][x] - sample(a, x + dx, y + dy)
                e += d * d
        return e

    best = min(((ssd(dx, dy), dx, dy) for dy in range(-4, 5) for dx in range(-4, 5)))
    _, bx, by = best
    fx = min(((ssd(bx + k / 20.0, by), bx + k / 20.0) for k in range(-10, 11)))[1]
    fy = min(((ssd(fx, by + k / 20.0), by + k / 20.0) for k in range(-10, 11)))[1]
    flat = ssd(0, 0) < 1e-6 and ssd(1, 0) < 1e-6
    return {"dx_px": round(fx, 2), "dy_px": round(fy, 2), "residual": round(ssd(fx, fy) / max(1, (s // 3) ** 2), 4),
            "image_is_flat": flat}


# ---------------------------------------------------------------------------------------------- apitrace
class TraceState:
    def __init__(self, track_names):
        self.track = track_names
        self.shaders = {}            # handle -> {"kind","regs":{name:(set,idx,size)},"version"}
        self.vs = self.ps = None
        self.rs = {}
        self.vsc = {}
        self.psc = {}
        self.frame = 0
        self.calls = 0
        self.draws = 0
        self.first_cull = None
        self.cull_sets = Counter()
        self.cull_at_draw = Counter()
        self.cull_by_ps = defaultdict(Counter)
        self.stencil_draws = Counter()
        self.clears = Counter()
        self.heat = []
        self.rt_textures = Counter()
        self.surfaces = Counter()
        self.stretch = 0
        self.colorization = Counter()
        self.colorization_examples = []
        self.texfactor = Counter()
        self.tracked = defaultdict(list)   # (shader handle, name) -> [(frame, values)]
        self.marker = "UVOffset"           # a draw is tracked only when its vs / ps declares this register (g1f t07: planet = "CloudScrollRate")
        self.pending_heat = None
        self.present = 0
        # added 2026-10-29 (G1 t02/t04)
        self.focus = None                  # (first_frame, last_frame) or None = every frame
        self.seq_frames = set()
        self.seq = {}                      # frame -> [token, ...] (pass / clear order)
        self.frame_end = {}                # frame -> EndScene call (snapshot point for the final image)
        self.desc = {}                     # shader handle -> "ps_2_0#n[reg,reg,...]"
        self.decls = {}                    # vertex declaration handle -> usages
        self.decl = None
        self.fvf = None
        self.cull_by_shader = defaultdict(Counter)
        self.cull_focus = Counter()
        self.stencil_focus = Counter()
        self.colorization_focus = Counter()
        self.colorization_by_shader = defaultdict(Counter)
        self.ff_draws = Counter()
        self.colour_input = Counter()      # draws whose vertex layout carries COLOR0, by shader / FF
        self.rt_sets = []
        self.ds_sets = []
        self.tex_desc = {}                 # texture handle -> "WxH FMT USAGE"
        self.surf_of = {}                  # surface handle -> what it belongs to
        self.stretch_pairs = Counter()
        self.first_frame_with = {}

    def in_focus(self):
        return self.focus is None or self.focus[0] <= self.frame <= self.focus[1]

    def tok(self, t):
        if self.frame in self.seq_frames:
            q = self.seq.setdefault(self.frame, [])
            if t == "draw" and q and q[-1].startswith("draw"):
                n = int(q[-1][5:]) if q[-1].startswith("drawx") else 1
                q[-1] = "drawx%d" % (n + 1)
            else:
                q.append(t)

    def regs_of(self, h):
        return self.shaders.get(h, {}).get("regs", {})


def parse_floats(s):
    out = []
    for x in s.split(","):
        x = x.strip()
        try:
            out.append(float(x))
        except ValueError:  # NaN / inf spellings keep their register slot
            out.append(float("nan"))
    return out


def arg(text, name):
    m = re.search(r"\b" + name + r" = ([^,)]+)", text)
    return m.group(1).strip() if m else None


def handle_call(st, no, fn, text):
    st.calls += 1
    short = fn.split("::")[-1]
    if short in ("CreateDevice", "CreateDeviceEx", "Reset", "ResetEx"):
        st.rs = {}
    elif short == "SetRenderState":
        state, val = arg(text, "State"), arg(text, "Value")
        st.rs[state] = val
        if state == "D3DRS_CULLMODE":
            st.cull_sets[val] += 1
            if st.first_cull is None:
                st.first_cull = {"call": no, "value": val, "draws_before": st.draws}
    elif short == "SetFVF":
        st.fvf = arg(text, "FVF")
        st.decl = None
    elif short == "SetVertexDeclaration":
        st.decl = arg(text, "pDecl")
    elif short == "CreateVertexDeclaration":
        m = re.search(r"ppDecl = &(0x[0-9a-fA-F]+)", text)
        if m:
            st.decls[m.group(1)] = re.findall(r"Usage = D3DDECLUSAGE_(\w+), UsageIndex = (\d+)", text)
    elif short == "SetRenderTarget":
        surf = arg(text, "pRenderTarget")
        st.rt_sets.append({"call": no, "frame": st.frame, "index": arg(text, "RenderTargetIndex"), "surface": surf,
                           "of": st.surf_of.get(surf)})
        st.tok("RT%s=%s" % (arg(text, "RenderTargetIndex"), st.surf_of.get(surf, surf)))
    elif short == "SetDepthStencilSurface":
        st.ds_sets.append({"call": no, "frame": st.frame, "surface": arg(text, "pNewZStencil")})
        st.tok("DS=%s" % arg(text, "pNewZStencil"))
    elif short == "GetSurfaceLevel":
        m = re.search(r"ppSurfaceLevel = &(0x[0-9a-fA-F]+)", text)
        t = re.search(r"this = (0x[0-9a-fA-F]+)", text)
        if m and t:
            st.surf_of[m.group(1)] = "%s(%s) L%s" % (t.group(1), st.tex_desc.get(t.group(1), "?"), arg(text, "Level"))
    elif short == "EndScene":
        st.frame_end[st.frame] = no
    elif short in ("SetVertexShader", "SetPixelShader"):
        h = arg(text, "pShader")
        if short == "SetVertexShader":
            st.vs = h
        else:
            st.ps = h
    elif short in ("SetVertexShaderConstantF", "SetPixelShaderConstantF"):
        start = int(arg(text, "StartRegister") or 0)
        m = re.search(r"pConstantData = \{([^}]*)\}", text)
        vals = parse_floats(m.group(1)) if m else []
        bank = st.vsc if short.startswith("SetVertex") else st.psc
        for i in range(0, len(vals) // 4):
            bank[start + i] = tuple(vals[4 * i:4 * i + 4])
    elif short == "Clear":
        col = int(arg(text, "Color") or 0)
        st.clears["0x%08X %s" % (col, arg(text, "Flags"))] += 1
        st.tok("Clear(%s, 0x%08X%s)" % ((arg(text, "Flags") or "").replace("D3DCLEAR_", ""), col,
                                        ", rect" if "pRects = &" in text else ""))
        if col & 0x00FFFFFF == 0x008080FF and (col >> 24) == 0:
            # scene_call: the last draw before the StretchRect that copied the scene (the back buffer then holds
            # the undistorted scene; after the clear it holds the 0x80 distortion buffer, not the scene)
            st.pending_heat = {"frame": st.frame, "clear_call": no, "stretch_call": None, "draw_call": None,
                               "scene_call": getattr(st, "draw_before_stretch", None)}
    elif short == "StretchRect":
        st.stretch += 1
        src, dst = arg(text, "pSourceSurface"), arg(text, "pDestSurface")
        st.stretch_pairs["%s -> %s %s" % (st.surf_of.get(src, src), st.surf_of.get(dst, dst), arg(text, "Filter"))] += 1
        st.tok("StretchRect(%s -> %s)" % (st.surf_of.get(src, src), st.surf_of.get(dst, dst)))
        st.draw_before_stretch = getattr(st, "last_draw", None)
        if st.pending_heat and st.pending_heat["stretch_call"] is None:
            st.pending_heat["stretch_call"] = no
    elif short in ("CreateTexture", "CreateRenderTarget", "CreateDepthStencilSurface", "CreateCubeTexture"):
        usage = arg(text, "Usage") or ""
        if short != "CreateTexture" or "RENDERTARGET" in usage or "DEPTHSTENCIL" in usage:
            d = "%s %sx%s %s %s" % (short, arg(text, "Width") or arg(text, "EdgeLength"), arg(text, "Height"),
                                     arg(text, "Format"), usage)
            st.rt_textures[d] += 1
            m = re.search(r"pp\w+ = &(0x[0-9a-fA-F]+)", text)
            if m:
                st.tex_desc[m.group(1)] = d.split(" ", 1)[1]
                if short in ("CreateRenderTarget", "CreateDepthStencilSurface"):
                    st.surf_of[m.group(1)] = d
    elif short == "Present" or short == "PresentEx":
        st.tok("Present")
        st.frame += 1
        st.present += 1
    elif short.startswith("Draw"):
        st.draws += 1
        foc = st.in_focus()
        st.tok("draw")
        cull = st.rs.get("D3DRS_CULLMODE", "(default D3DCULL_CCW: never set)")
        st.cull_at_draw[cull] += 1
        st.cull_by_ps[str(st.ps)][cull] += 1
        sh = "%s | %s" % (st.desc.get(st.vs, "FF-vs" if st.vs in (None, "NULL") else st.vs),
                          st.desc.get(st.ps, "FF-ps" if st.ps in (None, "NULL") else st.ps))
        if foc:
            st.cull_focus[cull] += 1
            st.cull_by_shader[sh][cull] += 1
            usages = st.decls.get(st.decl) if st.decl not in (None, "NULL") else None
            has_col = (("COLOR", "0") in usages) if usages is not None else ("DIFFUSE" in (st.fvf or ""))
            if has_col:
                st.colour_input["%s colorvertex=%s lighting=%s diffusesrc=%s" % (
                    sh, st.rs.get("D3DRS_COLORVERTEX"), st.rs.get("D3DRS_LIGHTING"),
                    st.rs.get("D3DRS_DIFFUSEMATERIALSOURCE"))] += 1
            if st.ps in (None, "NULL"):
                st.ff_draws["fvf=%s decl=%s lighting=%s colorvertex=%s diffusesrc=%s ambientsrc=%s blend=%s/%s" % (
                    st.fvf if st.decl in (None, "NULL") else "-", "-" if st.decl in (None, "NULL") else "yes",
                    st.rs.get("D3DRS_LIGHTING"), st.rs.get("D3DRS_COLORVERTEX"), st.rs.get("D3DRS_DIFFUSEMATERIALSOURCE"),
                    st.rs.get("D3DRS_AMBIENTMATERIALSOURCE"), st.rs.get("D3DRS_SRCBLEND"), st.rs.get("D3DRS_DESTBLEND"))] += 1
        if st.rs.get("D3DRS_STENCILENABLE") == "TRUE":
            key = tuple((k, st.rs.get(k)) for k in ("D3DRS_STENCILFUNC", "D3DRS_STENCILREF", "D3DRS_STENCILMASK",
                                                    "D3DRS_STENCILWRITEMASK", "D3DRS_STENCILPASS", "D3DRS_STENCILFAIL",
                                                    "D3DRS_STENCILZFAIL", "D3DRS_TWOSIDEDSTENCILMODE", "D3DRS_CCW_STENCILPASS",
                                                    "D3DRS_CCW_STENCILZFAIL", "D3DRS_SRCBLEND", "D3DRS_DESTBLEND",
                                                    "D3DRS_ALPHABLENDENABLE", "D3DRS_COLORWRITEENABLE", "D3DRS_ZFUNC"))
            st.stencil_draws[key] += 1
            if foc:
                st.stencil_focus[key] += 1
        for bank, h in ((st.psc, st.ps), (st.vsc, st.vs)):
            regs = st.regs_of(h)
            if "Colorization" in regs:
                rs, ri, _ = regs["Colorization"]
                v = bank.get(ri)
                st.colorization[str(v)] += 1
                if foc:
                    st.colorization_focus[str(v)] += 1
                    st.colorization_by_shader[st.desc.get(h, h)][str(v)] += 1
                st.first_frame_with.setdefault("Colorization", st.frame)
                if len(st.colorization_examples) < 20 and v is not None:
                    st.colorization_examples.append({"call": no, "frame": st.frame, "shader": h, "register": "c%d" % ri,
                                                     "value": v})
            for name in st.track:
                if name in regs and (st.marker in st.regs_of(st.ps) or st.marker in st.regs_of(st.vs)):
                    rs, ri, size = regs[name]
                    st.tracked[(h, name)].append((st.frame, [bank.get(ri + k) for k in range(min(size, 4))]))
        if st.ps is None or not st.regs_of(st.ps):
            tf = st.rs.get("D3DRS_TEXTUREFACTOR")
            if tf is not None:
                st.texfactor[tf] += 1
        st.last_draw = no
        ph = st.pending_heat
        if ph and ph["stretch_call"] is not None and "TRIANGLESTRIP" in text and "PrimitiveCount = 2" in text:
            ph["draw_call"] = no
            st.heat.append(ph)
            st.pending_heat = None


# Calls the state machine reads. Everything else (texture stage / sampler states, SetTexture, memcpy, Lock, lights,
# AddRef / Release ...) is left out of the dump with --grep, which roughly halves the text apitrace prints.
DUMP_GREP = (r"(SetRenderState|ShaderConstantF|SetVertexShader|SetPixelShader|Create\w+|Clear|StretchRect|Present"
             r"|EndScene|Draw\w+|SetRenderTarget|SetDepthStencilSurface|GetSurfaceLevel|SetFVF|SetVertexDeclaration|Reset\w*)$")


def analyse_trace(trace, apitrace, d3dretrace, out_dir, track, max_calls, snap, focus=None, seq_frames=None):
    st = TraceState(track)
    st.focus = focus
    st.seq_frames = set(seq_frames or [])
    cmd = [apitrace, "dump", "--color=never", "--grep=" + DUMP_GREP, trace]
    if max_calls:
        cmd.insert(2, "--calls=0-%d" % max_calls)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                         errors="replace", bufsize=1 << 20)
    cur = None          # (no, fn, [lines])
    def flush(c):
        if c is None:
            return
        no, fn, lines = c
        text = "\n".join(lines)
        if fn.endswith("CreateVertexShader") or fn.endswith("CreatePixelShader"):
            regs = {}
            ver = None
            for ln in lines:
                m = REG_RE.match(ln.strip())
                if m:
                    regs[m.group(1)] = (m.group(2), int(m.group(3)), int(m.group(4)))
                vm = re.match(r"^\s*(vs|ps)_\d_\d", ln)
                if vm and ver is None:
                    ver = ln.strip()
            hm = re.search(r"ppShader = &(0x[0-9a-fA-F]+)", lines[-1])
            if hm:
                st.shaders[hm.group(1)] = {"kind": "vs" if "Vertex" in fn else "ps", "regs": regs, "version": ver}
                names = sorted((n for n in regs if regs[n][0] == "c"), key=lambda k: regs[k][1])
                st.desc[hm.group(1)] = "%s#%d[%s]" % ((ver or "?").split()[0], len(st.shaders), ",".join(names)[:120])
        handle_call(st, no, fn, text)
    for line in p.stdout:
        line = line.rstrip("\n")
        m = CALL_RE.match(line)
        if m:
            flush(cur)
            cur = (int(m.group(1)), m.group(2), [line])
        elif cur is not None and line:
            cur[2].append(line)
    flush(cur)
    p.wait()

    facts = {
        "trace": trace, "calls": st.calls, "frames": st.present, "draws": st.draws,
        "shaders": len(st.shaders),
        "cull": {"first_set": st.first_cull, "sets": dict(st.cull_sets), "at_draw": dict(st.cull_at_draw),
                 "note": "D3DRS_CULLMODE default is D3DCULL_CCW; 'first_set' with draws_before 0 is the engine's device default"},
        "stencil_states_at_draw": [{"state": dict(k), "draws": n} for k, n in st.stencil_draws.most_common(20)],
        "clears": dict(st.clears.most_common(30)),
        "render_targets": dict(st.rt_textures),
        "stretch_rects": st.stretch,
        "colorization_values_at_draw": dict(st.colorization.most_common(30)),
        "colorization_examples": st.colorization_examples,
        "texturefactor_at_ff_draw": dict(st.texfactor.most_common(20)),
        "heat_passes_found": len(st.heat), "heat_first": st.heat[:3],
        "planet_candidates": {},
        "focus_frames": list(st.focus) if st.focus else None,
        "focus": {
            "cull_at_draw": dict(st.cull_focus),
            "cull_by_shader": {k: dict(v) for k, v in sorted(st.cull_by_shader.items(), key=lambda kv: -sum(kv[1].values()))[:60]},
            "stencil_states_at_draw": [{"state": dict(k), "draws": n} for k, n in st.stencil_focus.most_common(20)],
            "colorization_values_at_draw": dict(st.colorization_focus.most_common(30)),
            "colorization_by_shader": {k: dict(v.most_common(8)) for k, v in st.colorization_by_shader.items()},
            "fixed_function_draws": dict(st.ff_draws.most_common(30)),
            "draws_with_vertex_colour": dict(st.colour_input.most_common(40)),
        },
        "set_render_target": st.rt_sets[:50], "set_depth_stencil": st.ds_sets[:50],
        "stretch_rect_pairs": dict(st.stretch_pairs.most_common(20)),
        "frame_sequences": {str(k): v for k, v in sorted(st.seq.items())},
        "frame_end_calls": {str(k): st.frame_end.get(k) for k in sorted(st.seq_frames)},
        "first_frame_with": st.first_frame_with,
        "shaders_desc": st.desc,
    }
    for (h, name), series in st.tracked.items():
        per_frame = {}
        for fr, v in series:
            per_frame.setdefault(fr, v)
        vals = [v for _, v in sorted(per_frame.items()) if v and v[0] is not None]
        angles = []
        for v in vals:
            r0 = v[0]
            import math
            angles.append(math.atan2(r0[1], r0[0]))
        distinct = sorted(set(round(x, 6) for x in angles))
        steps = None
        if len(angles) > 2:
            import math
            uw = [angles[0]]
            for x in angles[1:]:
                d = x - uw[-1]
                while d > math.pi:
                    d -= 2 * math.pi
                while d < -math.pi:
                    d += 2 * math.pi
                uw.append(uw[-1] + d)
            incs = [b - a for a, b in zip(uw, uw[1:]) if abs(b - a) > 1e-7]
            if incs:
                med = sorted(abs(x) for x in incs)[len(incs) // 2]
                steps = round(2 * math.pi / med, 2) if med else None
            facts["planet_candidates"]["%s %s" % (h, name)] = {
                "frames": len(per_frame), "distinct_angles": len(distinct), "total_turn_deg": round(math.degrees(uw[-1] - uw[0]), 3),
                "median_step_deg": round(math.degrees(med), 5) if incs else None, "steps_per_turn": steps}
    # final images of the sequence frames (EndScene: after the UI, before Present) through d3dretrace
    ends = [st.frame_end[f] for f in sorted(st.seq_frames) if f in st.frame_end]
    if snap and ends and d3dretrace and os.path.isfile(d3dretrace):
        pre = os.path.join(out_dir, "final_")
        r = subprocess.run([d3dretrace, "-b", "--call-nos", "--snapshot=" + ",".join(str(c) for c in ends),
                            "--snapshot-prefix=" + pre, trace], capture_output=True, text=True)
        facts["final_images"] = {}
        for c in ends:
            pth = pre + "%010d.png" % c
            if os.path.isfile(pth):
                try:
                    facts["final_images"][str(c)] = dict(colour_stats(pth), png=pth)
                except Exception as e:
                    facts["final_images"][str(c)] = {"png": pth, "error": repr(e)}
            else:
                facts["final_images"][str(c)] = {"error": "no snapshot", "stderr": r.stderr[-800:]}
    # heat snapshots through d3dretrace
    if snap and st.heat and d3dretrace and os.path.isfile(d3dretrace):
        h = st.heat[len(st.heat) // 2]
        pre = os.path.join(out_dir, "heat_")
        before = h.get("scene_call") or (h["draw_call"] - 1)
        r = subprocess.run([d3dretrace, "-b", "--call-nos", "--snapshot=%d,%d" % (before, h["draw_call"]),
                            "--snapshot-prefix=" + pre, trace],
                           capture_output=True, text=True)
        b = pre + "%010d.png" % before
        a = pre + "%010d.png" % h["draw_call"]
        if os.path.isfile(a) and os.path.isfile(b):
            facts["heat_shift_apitrace"] = dict(measure_shift(b, a), before=b, after=a, draw_call=h["draw_call"])
        else:
            facts["heat_shift_apitrace"] = {"error": "d3dretrace produced no snapshots", "stderr": r.stderr[-2000:]}
    return facts


# ---------------------------------------------------------------------------------------------- RenderDoc
def analyse_rdoc(summary_path, calib):
    s = json.load(open(summary_path, encoding="utf-8"))
    out = {"summary": summary_path, "draws": len(s.get("draws", []))}
    cull = Counter("%s/front_ccw=%s" % (d["cull"], d["front_ccw"]) for d in s.get("draws", []))
    out["vulkan_cull_at_draw"] = dict(cull)
    if calib:
        m = {"%s/front_ccw=%s" % (v["cull"], v["front_ccw"]): k for k, v in calib.get("dxvk_cull_map", {}).items()}
        out["d3d9_cull_at_draw (via self-test calibration)"] = {m.get(k, "?" + k): n for k, n in cull.items()}
    st = Counter(json.dumps({"front": d["front"], "back": d["back"]}, sort_keys=True) for d in s.get("draws", []) if d["stencil"])
    out["stencil_states"] = [{"state": json.loads(k), "draws": n} for k, n in st.most_common(10)]
    out["clears"] = [{"eid": p["eid"], "values": p["clear_values"], "markers": p["markers"][-1:]} for p in s.get("passes", [])]
    out["render_targets"] = dict(Counter("%dx%d %s" % (t["w"], t["h"], t["format"]) for t in s.get("textures", [])
                                         if "ColorTarget" in t["category"] or "DepthTarget" in t["category"]))
    imgs = s.get("images", {})
    if "final" in imgs:
        out["final_colour"] = dict(colour_stats(imgs["final"]["path"]), png=imgs["final"]["path"])
    if "heat_before" in imgs and "heat_after" in imgs:
        out["heat_shift"] = dict(measure_shift(imgs["heat_before"]["path"], imgs["heat_after"]["path"]),
                                 before=imgs["heat_before"]["path"], after=imgs["heat_after"]["path"])
    return out


# ---------------------------------------------------------------------------------------------- vertex buffers (G1f t02)
# `apitrace dump --blobs` writes every memcpy blob of the selected calls to blob_call<N>.bin in the current directory.
# A D3D9 vertex buffer is filled as  Lock (call L) / memcpy (call L+1, blob) / Unlock (call L+2),  so the bytes a draw
# reads are the blobs of the write locks after the latest creation of that handle (handles are reused: match by the
# creation call, not by the handle alone).
VB_GREP = r"(CreateVertexBuffer|VertexBuffer9::Lock)$"
VB_CREATE_RE = re.compile(r"^(\d+) IDirect3DDevice9::CreateVertexBuffer\(.*Length = (\d+), Usage = ([^,]*), FVF = (.*?), "
                          r"Pool = (\w+), ppVertexBuffer = &(0x[0-9a-fA-F]+)")
VB_LOCK_RE = re.compile(r"^(\d+) IDirect3DVertexBuffer9::Lock\(this = (0x[0-9a-fA-F]+), OffsetToLock = (\d+), "
                        r"SizeToLock = (\d+).*Flags = ([^)]*)\)")
DRAW_GREP = (r"(SetStreamSource|SetFVF|SetVertexDeclaration|SetVertexShader|SetPixelShader|SetTexture|SetRenderState"
             r"|Draw\w+|Present)$")


def build_vb_index(trace, apitrace):
    """handle -> {"creates": [(call, length, usage, fvf, pool)], "locks": [(call, offset, size, flags)]} in call order."""
    p = subprocess.Popen([apitrace, "dump", "--color=never", "--grep=" + VB_GREP, trace], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace", bufsize=1 << 20)
    idx = defaultdict(lambda: {"creates": [], "locks": []})
    for line in p.stdout:
        m = VB_CREATE_RE.match(line)
        if m:
            idx[m.group(6)]["creates"].append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4), m.group(5)))
            continue
        m = VB_LOCK_RE.match(line)
        if m:
            idx[m.group(2)]["locks"].append((int(m.group(1)), int(m.group(3)), int(m.group(4)), m.group(5)))
    p.wait()
    return dict(idx)


def vb_contents_calls(idx, handle, draw_call):
    """(creation, writes) of the buffer a draw at `draw_call` reads: the latest creation of `handle` before the draw and
    its write locks [(lock_call, offset, size, flags)] (READONLY and zero-size locks are reads, not writes)."""
    e = idx.get(handle)
    if not e:
        return None, []
    cs = [c for c in e["creates"] if c[0] < draw_call]
    if not cs:
        return None, []
    c = cs[-1]
    return c, [lk for lk in e["locks"] if c[0] < lk[0] < draw_call and "READONLY" not in lk[3] and lk[2] != 0]


def fetch_blobs(trace, apitrace, calls, out_dir):
    """One `apitrace dump --blobs --calls=...` pass; returns {call: path} of the blob files it wrote."""
    os.makedirs(out_dir, exist_ok=True)
    cs = ",".join(str(c) for c in sorted(set(calls)))
    subprocess.run([apitrace, "dump", "--color=never", "--blobs", "--calls=" + cs, trace], cwd=out_dir,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    got = {}
    for c in set(calls):
        pth = os.path.join(out_dir, "blob_call%d.bin" % c)
        if os.path.isfile(pth):
            got[c] = pth
    return got


def decode_lit36(data, first=0, count=None):
    """36-byte lit vertices (D3DFVF_XYZ | NORMAL | DIFFUSE | TEX1) -> [(x, y, z, nx, ny, nz, argb, u, v)]."""
    n = len(data) // 36
    count = n - first if count is None else count
    return [struct.unpack_from("<3f3fI2f", data, (first + i) * 36) for i in range(max(0, min(count, n - first)))]


def summarise_lit36(verts):
    if not verts:
        return {"vertices": 0}
    cols = Counter(v[6] for v in verts)
    us = [v[7] for v in verts]
    vs = [v[8] for v in verts]
    return {"vertices": len(verts),
            "colour_argb_counts": {"0x%08X" % k: n for k, n in cols.most_common(12)},
            "alpha_values": sorted({v[6] >> 24 for v in verts}),
            "u_range": [round(min(us), 5), round(max(us), 5)], "v_range": [round(min(vs), 5), round(max(vs), 5)],
            "u_frac_distinct": sorted({round(u % 1.0, 3) for u in us})[:16],
            "v_frac_distinct": sorted({round(v % 1.0, 3) for v in vs})[:16],
            "z_range": [round(min(v[2] for v in verts), 3), round(max(v[2] for v in verts), 3)]}


def analyse_vb_draws(trace, apitrace, out_dir, call_range, facts_path=None, vs_filter=None, max_draws=64):
    """Draws in `call_range` ("A-B"): the vertex-buffer bytes each one reads, decoded when the layout is the 36-byte lit
    one. Needs the trace's VB index (one pass over the trace, cached in out_dir\\vb_index.json) and one blob pass."""
    os.makedirs(out_dir, exist_ok=True)
    cache = os.path.join(out_dir, "vb_index.json")
    if os.path.isfile(cache):
        idx = json.load(open(cache, encoding="utf-8"))
    else:
        idx = build_vb_index(trace, apitrace)
        json.dump(idx, open(cache, "w", encoding="utf-8"))
    desc = {}
    if facts_path and os.path.isfile(facts_path):
        try:
            f = json.load(open(facts_path, encoding="utf-8"))
            desc = next(iter(f.get("apitrace", {}).values())).get("shaders_desc", {})
        except Exception:
            desc = {}
    p = subprocess.run([apitrace, "dump", "--color=never", "--calls=" + call_range, "--grep=" + DRAW_GREP, trace],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    cur = {"stream": None, "stride": None, "off": 0, "vs": None, "ps": None, "fvf": None, "tex0": None, "bl": {}}
    draws = []
    for line in p.stdout.split("\n"):
        m = CALL_RE.match(line)
        if not m:
            continue
        no, short, text = int(m.group(1)), m.group(2).split("::")[-1], line
        if short == "SetStreamSource" and "StreamNumber = 0" in text:
            cur["stream"], cur["stride"], cur["off"] = arg(text, "pStreamData"), int(arg(text, "Stride") or 0), int(arg(text, "OffsetInBytes") or 0)
        elif short == "SetVertexShader":
            cur["vs"] = arg(text, "pShader")
        elif short == "SetPixelShader":
            cur["ps"] = arg(text, "pShader")
        elif short == "SetFVF":
            cur["fvf"] = re.search(r"FVF = (.*?)\)", text).group(1)
        elif short == "SetTexture" and "Stage = 0," in text:
            cur["tex0"] = arg(text, "pTexture")
        elif short == "SetRenderState" and arg(text, "State") in ("D3DRS_SRCBLEND", "D3DRS_DESTBLEND"):
            cur["bl"][arg(text, "State")[8:]] = arg(text, "Value")
        elif short.startswith("Draw") and short != "DrawPrimitiveUP":
            sh = "%s | %s" % (desc.get(cur["vs"], cur["vs"]), desc.get(cur["ps"], cur["ps"]))
            if vs_filter and vs_filter not in sh:
                continue
            draws.append({"call": no, "fn": short, "stream": cur["stream"], "stride": cur["stride"], "offset": cur["off"],
                          "fvf": cur["fvf"], "shaders": sh, "tex0": cur["tex0"], "blend": dict(cur["bl"]),
                          "base_vertex": int(arg(text, "BaseVertexIndex") or 0), "min_vertex": int(arg(text, "MinVertexIndex") or 0),
                          "num_vertices": int(arg(text, "NumVertices") or 0) or None,
                          "start_vertex": int(arg(text, "StartVertex") or 0)})
    draws = draws[:max_draws]
    need = {}
    for d in draws:
        c, writes = vb_contents_calls(idx, d["stream"], d["call"])
        d["vb_create"] = c
        d["vb_writes"] = writes[-4:]
        for lk in d["vb_writes"]:
            need[lk[0] + 1] = lk
    blobs = fetch_blobs(trace, apitrace, list(need), os.path.join(out_dir, "blobs")) if need else {}
    out = []
    for d in draws:
        r = dict(d)
        c = d["vb_create"]
        if c and d["vb_writes"] and all((lk[0] + 1) in blobs for lk in d["vb_writes"]):
            buf = bytearray(c[1])            # later writes overwrite earlier ones at their offset
            for lk in d["vb_writes"]:
                b = open(blobs[lk[0] + 1], "rb").read()
                buf[lk[1]:lk[1] + len(b)] = b
            r["vb_blob_calls"] = [lk[0] + 1 for lk in d["vb_writes"]]
            if d["stride"] == 36 and "NORMAL" in (c[3] or "") and "DIFFUSE" in (c[3] or "") and "TEX1" in (c[3] or ""):
                first = d["offset"] // 36 + d["base_vertex"] + d["min_vertex"]
                nv = d["num_vertices"] or (len(buf) // 36 - first)
                r["lit36"] = summarise_lit36(decode_lit36(bytes(buf), first, nv))
        out.append(r)
    return out


# ---------------------------------------------------------------------------------------------- image comparison (G1f t04)
def write_png_rgb(path, w, h, rows):
    """rows: h byte strings of w * 3 RGB bytes."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def region_metrics(px, x0, y0, x1, y1, bright_thr=150):
    """Colour statistics of px[y][x] (rows of (r, g, b)) over a rectangle."""
    n = 0
    s = [0.0, 0.0, 0.0]
    lum = []
    sat = 0.0
    bright = 0
    cls = Counter()
    for y in range(y0, y1):
        row = px[y]
        for x in range(x0, x1):
            r, g, b = row[x]
            n += 1
            s[0] += r
            s[1] += g
            s[2] += b
            l = 0.299 * r + 0.587 * g + 0.114 * b
            lum.append(l)
            mx, mn = max(r, g, b), min(r, g, b)
            sat += (mx - mn) / mx if mx else 0.0
            if l >= bright_thr:
                bright += 1
            if mx < 30:
                cls["dark"] += 1
            elif g > r + 6 and g >= b:
                cls["green"] += 1
            elif r > g > b and r - b > 20:
                cls["brown"] += 1
            elif b > r and b >= g:
                cls["blue"] += 1
            else:
                cls["other"] += 1
    lum.sort()

    def pct(p):
        return round(lum[min(len(lum) - 1, int(p * len(lum)))], 2) if lum else None
    n = max(n, 1)
    return {"pixels": n, "mean_rgb": [round(v / n, 2) for v in s], "luma_mean": round(sum(lum) / n, 2),
            "luma_p5_p50_p95_p99": [pct(0.05), pct(0.5), pct(0.95), pct(0.99)], "saturation_mean": round(sat / n, 4),
            "bright_pixels_per_mpx": round(bright * 1e6 / n, 1),
            "class_fraction": {k: round(v / n, 4) for k, v in sorted(cls.items())}}


def compare_pair(ref_png, port_png, regions, out_prefix=None):
    """regions: {name: (x0, y0, x1, y1)} in pixels (both images must be the same size)."""
    rw, rh, rpx = read_png(ref_png)
    pw, ph, ppx = read_png(port_png)
    res = {"ref": ref_png, "port": port_png, "ref_size": [rw, rh], "port_size": [pw, ph], "regions": {}}
    for name, (x0, y0, x1, y1) in regions.items():
        a, b = region_metrics(rpx, x0, y0, x1, y1), region_metrics(ppx, x0, y0, x1, y1)
        res["regions"][name] = {"rect": [x0, y0, x1, y1], "ref": a, "port": b,
                                "luma_ratio_port_over_ref": round(b["luma_mean"] / max(a["luma_mean"], 1e-6), 3)}
    if out_prefix:
        def half(px, w, h):
            return [bytes(c for x in range(0, w - 1, 2) for c in px[y][x]) for y in range(0, h - 1, 2)]
        ra, pa = half(rpx, rw, rh), half(ppx, pw, ph)
        hh = min(len(ra), len(pa))
        write_png_rgb(out_prefix + "_side_by_side.png", len(ra[0]) // 3 + len(pa[0]) // 3, hh, [ra[i] + pa[i] for i in range(hh)])
        res["side_by_side"] = out_prefix + "_side_by_side.png"
    return res


def parse_regions(spec):
    """'name=x0,y0,x1,y1;name2=...' -> dict"""
    out = {}
    for part in spec.split(";"):
        if part.strip():
            k, v = part.split("=")
            out[k.strip()] = tuple(int(t) for t in v.split(","))
    return out


# ---------------------------------------------------------------------------------------------- report
def md(facts):
    L = ["# G1 capture analysis", "", "Run: `%s`" % facts["run_dir"], ""]
    for name, f in facts.get("apitrace", {}).items():
        L += ["## apitrace `%s`" % name, "", "- calls %d, frames %d, draws %d, shaders %d" % (f["calls"], f["frames"], f["draws"], f["shaders"])]
        L += ["- **Engine cull mode**: first SetRenderState(CULLMODE) = `%s`" % json.dumps(f["cull"]["first_set"]),
              "- CULLMODE at draw: `%s`" % json.dumps(f["cull"]["at_draw"])]
        L += ["- **Stencil states at draw** (top): "] + ["  - %d draws: `%s`" % (x["draws"], json.dumps(x["state"])) for x in f["stencil_states_at_draw"][:6]]
        L += ["- **Colorization at draw**: `%s`" % json.dumps(f["colorization_values_at_draw"]),
              "- TEXTUREFACTOR at fixed-function draws: `%s`" % json.dumps(f["texturefactor_at_ff_draw"]),
              "- Clears: `%s`" % json.dumps(f["clears"]),
              "- Render targets: `%s`" % json.dumps(f["render_targets"]),
              "- Heat passes found: %d; first: `%s`" % (f["heat_passes_found"], json.dumps(f["heat_first"][:1]))]
        if "heat_shift_apitrace" in f:
            L += ["- **Heat pass image shift** (after vs before SceneHeat): `%s`" % json.dumps(f["heat_shift_apitrace"])]
        fo = f.get("focus") or {}
        if f.get("focus_frames"):
            L += ["", "### Focus frames %s" % f["focus_frames"],
                  "- CULLMODE at draw: `%s`" % json.dumps(fo.get("cull_at_draw")),
                  "- Colorization at draw: `%s`" % json.dumps(fo.get("colorization_values_at_draw"))]
            L += ["- Colorization by shader:"] + ["  - `%s`: `%s`" % (k, json.dumps(v)) for k, v in fo.get("colorization_by_shader", {}).items()]
            L += ["- Stencil states at draw:"] + ["  - %d draws: `%s`" % (x["draws"], json.dumps(x["state"])) for x in fo.get("stencil_states_at_draw", [])[:6]]
            L += ["- Fixed-function draws: `%s`" % json.dumps(fo.get("fixed_function_draws"))]
        L += ["- CULLMODE by shader (focus, top 25):"] + ["  - `%s`: `%s`" % (k, json.dumps(v)) for k, v in list(fo.get("cull_by_shader", {}).items())[:25]]
        L += ["- Draws whose vertex layout carries COLOR0 (focus): `%s`" % json.dumps(fo.get("draws_with_vertex_colour"))]
        L += ["- SetRenderTarget calls: `%s`" % json.dumps(f.get("set_render_target")),
              "- StretchRect pairs: `%s`" % json.dumps(f.get("stretch_rect_pairs"))]
        for fr, sq in (f.get("frame_sequences") or {}).items():
            L += ["- Frame %s sequence: `%s`" % (fr, " ; ".join(sq))]
        for c, v in (f.get("final_images") or {}).items():
            L += ["- Final image at call %s: `%s`" % (c, json.dumps(v))]
        if f["planet_candidates"]:
            L += ["- Planet candidates (m_world per frame, shaders with UVOffset): `%s`" % json.dumps(f["planet_candidates"])]
        L.append("")
    for name, r in facts.get("renderdoc", {}).items():
        L += ["## RenderDoc `%s`" % name, ""]
        for k, v in r.items():
            L.append("- %s: `%s`" % (k, json.dumps(v)))
        L.append("")
    if facts.get("dxvk"):
        L += ["## DXVK", ""] + ["- %s: `%s`" % (k, json.dumps(v)) for k, v in facts["dxvk"].items()] + [""]
    return "\n".join(L) + "\n"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--apitrace", default="")
    ap.add_argument("--d3dretrace", default="")
    ap.add_argument("--track", nargs="*", default=["m_world"])
    ap.add_argument("--max-calls", type=int, default=0, help="only the first N calls of a trace (0 = all)")
    ap.add_argument("--no-snapshots", action="store_true")
    ap.add_argument("--focus-frames", default="", help="A-B: per-draw stats also for these frames only (e.g. the battle)")
    ap.add_argument("--seq-frames", default="", help="comma list of frames whose pass/clear sequence is recorded and "
                                                    "whose final image is snapshotted (default: 3 frames of the focus range)")
    ap.add_argument("--vb-draws", default="", help="A-B: decode the vertex-buffer contents read by the draws in this call range")
    ap.add_argument("--vs-filter", default="", help="--vb-draws: only draws whose 'vs | ps' description contains this text")
    ap.add_argument("--facts", default="", help="--vb-draws: facts.json with the shader descriptions (default analysisacts.json)")
    ap.add_argument("--max-draws", type=int, default=64)
    ap.add_argument("--compare", nargs=3, metavar=("NAME", "REF_PNG", "PORT_PNG"), help="colour statistics of two images + side-by-side PNG")
    ap.add_argument("--regions", default="whole=0,0,1920,650", help="--compare: 'name=x0,y0,x1,y1;...'")
    a = ap.parse_args(argv)
    if a.compare:
        name, ref, port = a.compare
        out = os.path.join(os.path.abspath(a.run_dir), "analysis")
        os.makedirs(out, exist_ok=True)
        res = compare_pair(ref, port, parse_regions(a.regions), os.path.join(out, "compare_" + name))
        json.dump(res, open(os.path.join(out, "compare_%s.json" % name), "w", encoding="utf-8"), indent=2)
        print(json.dumps(res, indent=2))
        return 0
    rd = os.path.abspath(a.run_dir)
    if a.vb_draws:
        out = os.path.join(rd, "analysis")
        trace = os.path.join(rd, "eaw.trace")
        res = analyse_vb_draws(trace, a.apitrace, out, a.vb_draws, a.facts or os.path.join(out, "facts.json"), a.vs_filter or None, a.max_draws)
        pth = os.path.join(out, "vb_draws_%s.json" % a.vb_draws)
        json.dump(res, open(pth, "w", encoding="utf-8"), indent=2)
        print("%d draws written to %s" % (len(res), pth))
        return 0
    out = os.path.join(rd, "analysis")
    os.makedirs(out, exist_ok=True)
    calib = json.load(open(CALIBRATION)) if os.path.isfile(CALIBRATION) else None
    facts = {"run_dir": rd, "apitrace": {}, "renderdoc": {}, "dxvk": {}, "errors": []}
    for n in sorted(os.listdir(rd)):
        p = os.path.join(rd, n)
        try:
            if n.endswith(".trace") and a.apitrace:
                focus = tuple(int(x) for x in a.focus_frames.split("-")) if a.focus_frames else None
                seq = [int(x) for x in a.seq_frames.split(",") if x] if a.seq_frames else (
                    [focus[0] + (focus[1] - focus[0]) * k // 4 for k in (1, 2, 3)] if focus else [])
                facts["apitrace"][n] = analyse_trace(p, a.apitrace, a.d3dretrace, out, a.track, a.max_calls,
                                                     not a.no_snapshots, focus, seq)
            elif n.endswith(".summary.json"):
                facts["renderdoc"][n] = analyse_rdoc(p, calib)
            elif n.endswith("_d3d9.log"):
                txt = open(p, encoding="utf-8", errors="replace").read()
                facts["dxvk"][n] = {"dxvk_version": (re.findall(r"DXVK: (\S+)", txt) or [None])[0],
                                    "device": (re.findall(r"Found device: (.+)", txt) or [None])[0],
                                    "format_remaps": sorted(set(re.findall(r"D3D9: (VK_FORMAT_\S+ -> VK_FORMAT_\S+)", txt))),
                                    "errors": len(re.findall(r"^err:", txt, re.M)), "warnings": len(re.findall(r"^warn:", txt, re.M))}
        except Exception as e:  # keep going: one bad input must not hide the others
            facts["errors"].append("%s: %r" % (n, e))
    shots = os.path.join(rd, "shots")
    if os.path.isdir(shots):
        facts["dxvk"]["screenshots"] = sorted(os.listdir(shots))
    json.dump(facts, open(os.path.join(out, "facts.json"), "w", encoding="utf-8"), indent=2)
    open(os.path.join(out, "report.md"), "w", encoding="utf-8").write(md(facts))
    print("analysis written to " + out)
    return 1 if facts["errors"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
