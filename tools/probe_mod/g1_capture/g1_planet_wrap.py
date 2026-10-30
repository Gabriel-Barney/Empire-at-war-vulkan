"""g1_planet_wrap.py - G1f t03: does the planet idle clip wrap at 239 or 240 frames?

Reads an apitrace D3D9 trace (streamed through `apitrace dump`, reusing g1_analyze's state machine read-only) and, for the
Planet.fx draws (vertex shaders with `m_world` + `UVOffset`), records per frame the first draw's m_world (4 constant rows)
and UVOffset. Output: <out>.json (series) and a text summary on stdout.

  python -I g1_planet_wrap.py extract <run-dir or trace> [--out series.json] [--max-calls N]
  python -I g1_planet_wrap.py survey rows.pkl                 rotation per (shader, scale) group: pick the planet
  python -I g1_planet_wrap.py analyse rows.pkl --scale S      steps / ticks of that group

Method (see the summary printed by `analyse`):
  1. Rotation about the spin axis, unwrapped: successive relative rotations M[i-1]^-1 M[i] of the (scale-normalised)
     3x3 part of m_world, signed against the axis of the first non-zero step.
  2. Sim ticks between frames: the clip advances `frame += fps * dt` on 30 Hz ticks with 360 deg / 239 per clip frame (ALA
     data: frame 239 duplicates frame 0), so one tick = 360/239/30 = 0.050209 deg; step / tick = ticks between frames.
  3. A 240-frame wrap holds the planet still for one clip frame (30 ticks at 30 Hz = 1 s) at the turn point; a 239 wrap does
     not. A second clock that does not stop there is needed: UVOffset (cloud scroll) if it is tick driven. The summary
     fits angle = a * UV + b before and after the turn point and reports the jump in b (in degrees and clip frames).
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys

KIT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, KIT)
import g1_analyze as A  # noqa: E402  (read only use)

MINF = 20
TOOLCHAIN = os.path.abspath(os.path.join(KIT, "..", "..", "..", "toolchain"))
APITRACE = os.path.join(TOOLCHAIN, "capture", "apitrace-14.0", "bin", "apitrace.exe")


def extract(trace, out, max_calls=0):
    st = A.TraceState(["m_world", "UVOffset", "m_worldViewProj"])
    cmd = [APITRACE, "dump", "--color=never", "--grep=" + A.DUMP_GREP, trace]
    if max_calls:
        cmd.insert(2, "--calls=0-%d" % max_calls)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                         errors="replace", bufsize=1 << 20)
    cur = None

    def flush(c):
        if c is None:
            return
        no, fn, lines = c
        text = "\n".join(lines)
        if fn.endswith("CreateVertexShader") or fn.endswith("CreatePixelShader"):
            regs = {}
            ver = None
            for ln in lines:
                m = A.REG_RE.match(ln.strip())
                if m:
                    regs[m.group(1)] = (m.group(2), int(m.group(3)), int(m.group(4)))
                vm = re.match(r"^\s*(vs|ps)_\d_\d", ln)
                if vm and ver is None:
                    ver = ln.strip()
            hm = re.search(r"ppShader = &(0x[0-9a-fA-F]+)", lines[-1])
            if hm:
                st.shaders[hm.group(1)] = {"kind": "vs" if "Vertex" in fn else "ps", "regs": regs, "version": ver}
        A.handle_call(st, no, fn, text)

    for line in p.stdout:
        line = line.rstrip("\n")
        m = A.CALL_RE.match(line)
        if m:
            flush(cur)
            cur = (int(m.group(1)), m.group(2), [line])
        elif cur is not None and line:
            cur[2].append(line)
    flush(cur)
    p.wait()
    # raw rows, deduplicated per (frame, shader, scale, translation rounded to 1 unit): the planet's submeshes share one m_world
    rows = []
    seen = set()
    for h in {h for (h, _) in st.tracked}:
        mw = st.tracked.get((h, "m_world"), [])
        uv = st.tracked.get((h, "UVOffset"), [])
        wv = st.tracked.get((h, "m_worldViewProj"), [])
        for i, (fr, v) in enumerate(mw):
            try:
                r = [float(x) for j in range(min(len(v), 4)) for x in v[j][:4]]   # 3 or 4 registers of 4 floats
            except (TypeError, ValueError, IndexError):
                continue
            if len(r) not in (12, 16):
                continue
            sc = math.sqrt(r[0] ** 2 + r[1] ** 2 + r[2] ** 2)
            tr = (r[3], r[7], r[11]) if len(r) == 12 else (r[12], r[13], r[14])
            k = (fr, h, round(sc, 2), round(tr[0]), round(tr[1]), round(tr[2]))
            if k in seen:
                continue
            seen.add(k)
            u = uv[i][1] if i < len(uv) and uv[i][0] == fr else None
            w = None
            if i < len(wv) and wv[i][0] == fr:
                try:
                    w = [float(x) for j in range(4) for x in wv[i][1][j][:4]]
                except (TypeError, ValueError, IndexError):
                    w = None
            rows.append((fr, h, r, u, w))
    rows.sort(key=lambda x: x[0])
    res = {"trace": trace, "frames": st.present, "calls": st.calls, "rows": rows}
    import pickle
    with open(out, "wb") as f:
        pickle.dump(res, f)
    print("frames %d calls %d rows %d -> %s" % (st.present, st.calls, len(rows), out))
    return


def _unused(st, trace, out):
    # one series per (shader, row scale): per frame the first draw of that key (the planet's submeshes share one m_world)
    series = {}
    for h in {h for (h, _) in st.tracked}:
        mw = st.tracked.get((h, "m_world"), [])
        uv = st.tracked.get((h, "UVOffset"), [])
        objs = {}
        ndraw = {}
        for i, (fr, v) in enumerate(mw):
            try:
                r = [[float(x) for x in v[j][:4]] for j in range(4)]
            except (TypeError, ValueError, IndexError):
                continue
            sc = math.sqrt(sum(x * x for x in r[0][:3]))
            key = "%s|%.1f" % (h, sc)
            u = uv[i][1] if i < len(uv) and uv[i][0] == fr else None
            o = objs.setdefault(key, {})
            o.setdefault(fr, ([x for row in r for x in row], u))
            ndraw[key] = ndraw.get(key, 0) + 1
        for key, o in objs.items():
            if len(o) >= MINF:
                series[key] = {"m_world": [[fr, v[0]] for fr, v in sorted(o.items())],
                               "UVOffset": [[fr, v[1]] for fr, v in sorted(o.items())],
                               "draws": ndraw[key], "frames": len(o)}
    res = {"trace": trace, "frames": st.present, "calls": st.calls, "series": series}
    with open(out, "w") as f:
        json.dump(res, f)
    print("frames %d calls %d shaders with series: %s -> %s" % (st.present, st.calls, list(series), out))


# ---------------------------------------------------------------------------------------------------- analysis
def mat3(rows):
    """3x3 rotation part of m_world from 16 floats (4 constant rows of 4); returns the matrix with the per-row scale
    removed, or None."""
    try:
        m = [[float(rows[i * 4 + j]) for j in range(3)] for i in range(3)]
    except (TypeError, ValueError, IndexError):
        return None
    out = []
    for r in m:
        n = math.sqrt(sum(x * x for x in r))
        if n < 1e-9 or n != n:
            return None
        out.append([x / n for x in r])
    return out


def mul_t(a, b):
    """a^T * b"""
    return [[sum(a[k][i] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def rotvec(m):
    tr = m[0][0] + m[1][1] + m[2][2]
    r = [(m[2][1] - m[1][2]) / 2, (m[0][2] - m[2][0]) / 2, (m[1][0] - m[0][1]) / 2]
    s = math.sqrt(sum(x * x for x in r))
    phi = math.atan2(s, (tr - 1) / 2)
    axis = [x / s for x in r] if s > 1e-12 else None
    return phi, axis


def unwrap(rows_by_frame):
    """-> [(frame, theta_deg)] unwrapped rotation about the spin axis (sign: that of the first non-zero step)."""
    frames = []
    prev = None
    theta = 0.0
    n_axis = None
    for fr, rows in rows_by_frame:
        m = mat3(rows)
        if m is None:
            continue
        if prev is not None:
            phi, axis = rotvec(mul_t(prev, m))   # prev^T m: rotation of the row-vector convention, axis sign consistent
            if axis is not None and phi > 1e-7:
                if n_axis is None:
                    n_axis = axis
                sgn = 1.0 if sum(a * b for a, b in zip(axis, n_axis)) >= 0 else -1.0
                theta += sgn * math.degrees(phi)
        frames.append((fr, theta))
        prev = m
    return frames


def fit_line(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0.0
    return a, my - a * mx


def load_groups(path):
    import pickle
    d = pickle.load(open(path, "rb"))
    groups = {}
    for fr, h, r, u, w in d["rows"]:
        sc = math.sqrt(r[0] ** 2 + r[1] ** 2 + r[2] ** 2)
        groups.setdefault((h, round(sc, 1)), []).append((fr, r, u, w))
    return d, groups


def one_per_frame(lst):
    out, last = [], None
    for e in lst:
        if e[0] != last:
            out.append(e)
            last = e[0]
    return out


def survey(path, top=25):
    """Rotation summary per (shader, scale) group, to pick the planet."""
    d, groups = load_groups(path)
    print("trace %s: %d frames, %d rows, %d groups" % (d["trace"], d["frames"], len(d["rows"]), len(groups)))
    rows = []
    for key, lst in groups.items():
        lst = one_per_frame(lst)
        if len(lst) < 100:
            continue
        th = unwrap([(fr, r) for fr, r, _, _ in lst])
        deg = [t for _, t in th]
        tx = [(r[3] if len(r) == 12 else r[12]) for _, r, _, _ in lst]
        rows.append((abs(deg[-1] - deg[0]), key, len(lst), lst[0][0], lst[-1][0], deg[-1] - deg[0], max(tx) - min(tx)))
    for t in sorted(rows, reverse=True)[:top]:
        print("scale %-9s shader %s frames %6d (%d..%d) turn %9.3f deg, x-translation spread %.2f" %
              (t[1][1], t[1][0], t[2], t[3], t[4], t[5], t[6]))


def analyse(path, key_scale=None, tick_hz=30.0, markers=None):
    d, groups = load_groups(path)
    cands = [k for k in groups if key_scale is None or abs(k[1] - key_scale) < 0.06]
    for key in sorted(cands, key=lambda k: -len(groups[k]))[:6]:
        lst = one_per_frame(groups[key])
        if len(lst) < 500:
            continue
        th = unwrap([(fr, r) for fr, r, _, _ in lst])
        deg = [t for _, t in th]
        total = deg[-1] - deg[0]
        print("\nobject %s scale %.1f: %d frames (%d..%d), total turn %.3f deg" % (key[0], key[1], len(lst), lst[0][0], lst[-1][0], total))
        steps = [abs(b - a) for a, b in zip(deg, deg[1:]) if abs(b - a) > 1e-4]
        q239 = 360.0 / 239 / tick_hz
        q240 = 360.0 / 240 / tick_hz
        hist = {}
        for x in steps:
            k = round(x / q239)
            if k and abs(x / q239 - k) < 0.15:
                hist.setdefault(k, []).append(x / k)
        print("  tick quantum if 239 clips per turn: %.6f deg, if 240: %.6f deg" % (q239, q240))
        for k in sorted(hist, key=lambda k: -len(hist[k]))[:6]:
            v = sorted(hist[k])
            print("   step = %d tick(s): %d frames, per-tick angle median %.6f deg (min %.6f max %.6f)" % (k, len(v), v[len(v) // 2], v[0], v[-1]))
        json.dump({"frames": [fr for fr, _ in th], "deg": deg}, open(path + ".%s_%.1f.json" % (key[0], key[1]), "w"))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["extract", "survey", "analyse"])
    ap.add_argument("--scale", type=float, default=None)
    ap.add_argument("path")
    ap.add_argument("--out", default="")
    ap.add_argument("--max-calls", type=int, default=0)
    ap.add_argument("--tick", type=float, default=30.0)
    a = ap.parse_args(argv)
    if a.cmd == "extract":
        trace = a.path if a.path.endswith(".trace") else os.path.join(a.path, "eaw.trace")
        out = a.out or os.path.join(os.path.dirname(trace), "analysis", "planet_series.json")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        extract(trace, out, a.max_calls)
    elif a.cmd == "survey":
        survey(a.path)
    else:
        analyse(a.path, a.scale, a.tick)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
