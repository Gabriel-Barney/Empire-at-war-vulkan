"""Render a glTF produced by alo2gltf to PNG with matplotlib (sanity check, no GPU needed).

Supports the subset alo2gltf emits: node TRS/matrix hierarchies, float/u16 accessors, triangle
lists, and rigid skins whose POSITIONs are already in model space (mesh node under the root).

    python -I render_gltf.py in.gltf out.png [--max-tris 250000] [--title T] [--frame F]
"""
import argparse
import base64
import json
import math
import os
import struct

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402

COMP = {5126: ("f", 4), 5123: ("H", 2), 5125: ("I", 4), 5121: ("B", 1)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def load(path):
    if path.lower().endswith(".glb"):
        data = open(path, "rb").read()
        jlen = struct.unpack_from("<I", data, 12)[0]
        gl = json.loads(data[20:20 + jlen])
        blen = struct.unpack_from("<I", data, 20 + jlen)[0]
        buf = data[28 + jlen:28 + jlen + blen]
    else:
        gl = json.load(open(path))
        uri = gl["buffers"][0]["uri"]
        if uri.startswith("data:"):
            buf = base64.b64decode(uri.split(",", 1)[1])
        else:
            buf = open(os.path.join(os.path.dirname(path), os.path.basename(uri)), "rb").read()
    return gl, buf


def accessor(gl, buf, i):
    a = gl["accessors"][i]
    v = gl["bufferViews"][a["bufferView"]]
    fmt, size = COMP[a["componentType"]]
    n = NCOMP[a["type"]]
    off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
    arr = np.frombuffer(buf, dtype=np.dtype("<" + fmt), count=a["count"] * n, offset=off)
    return arr.reshape(a["count"], n) if n > 1 else arr


def quat_mat(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def local_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=float).reshape(4, 4).T
    m = np.eye(4)
    s = np.diag(node.get("scale", [1, 1, 1]))
    m[:3, :3] = quat_mat(node.get("rotation", [0, 0, 0, 1])) @ s
    m[:3, 3] = node.get("translation", [0, 0, 0])
    return m


def apply_animation(gl, buf, frame):
    """Overwrite node TRS with the sampled values of animation 0 at keyframe index `frame`."""
    if frame is None or not gl.get("animations"):
        return
    anim = gl["animations"][0]
    for ch in anim["channels"]:
        smp = anim["samplers"][ch["sampler"]]
        out = accessor(gl, buf, smp["output"])
        f = min(frame, len(out) - 1)
        gl["nodes"][ch["target"]["node"]][ch["target"]["path"]] = [float(x) for x in out[f]]
        gl["nodes"][ch["target"]["node"]].pop("matrix", None)


def collect(gl, buf):
    tris = []
    nodes = gl["nodes"]
    globals_ = {}

    def visit(i, parent):
        g = parent @ local_matrix(nodes[i])
        globals_[i] = g
        for c in nodes[i].get("children", []):
            visit(c, g)

    for r in gl["scenes"][gl.get("scene", 0)]["nodes"]:
        visit(r, np.eye(4))
    for i, n in enumerate(nodes):
        if "mesh" not in n:
            continue
        for p in gl["meshes"][n["mesh"]]["primitives"]:
            pos = accessor(gl, buf, p["attributes"]["POSITION"]).astype(float)
            idx = accessor(gl, buf, p["indices"]).astype(np.int64).reshape(-1, 3)
            if "skin" in n and "JOINTS_0" in p["attributes"]:
                skin = gl["skins"][n["skin"]]
                ibm = accessor(gl, buf, skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
                jm = np.stack([globals_[j] @ ibm[k] for k, j in enumerate(skin["joints"])])
                joints = accessor(gl, buf, p["attributes"]["JOINTS_0"]).astype(np.int64)
                wts = accessor(gl, buf, p["attributes"]["WEIGHTS_0"]).astype(float)
                ph = np.concatenate([pos, np.ones((len(pos), 1))], 1)
                pw = np.zeros((len(pos), 3))
                for k in range(4):
                    m = jm[joints[:, k]]
                    pw += wts[:, k:k + 1] * np.einsum("nij,nj->ni", m, ph)[:, :3]
            else:
                m = globals_[i]
                pw = pos @ m[:3, :3].T + m[:3, 3]
            tris.append(pw[idx])
    return np.concatenate(tris) if tris else np.zeros((0, 3, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gltf")
    ap.add_argument("png")
    ap.add_argument("--max-tris", type=int, default=250000)
    ap.add_argument("--title", default=None)
    ap.add_argument("--frame", type=int, default=None, help="pose with animation 0 at this keyframe")
    args = ap.parse_args()
    gl, buf = load(args.gltf)
    apply_animation(gl, buf, args.frame)
    T = collect(gl, buf)
    ntri = len(T)
    if ntri > args.max_tris:
        rng = np.random.default_rng(0)
        T = T[rng.choice(ntri, args.max_tris, replace=False)]
    # glTF is Y-up; plot with Z-up for matplotlib: (x, -z, y)
    T = T[:, :, [0, 2, 1]] * np.array([1, -1, 1])
    fn = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    ln = np.linalg.norm(fn, axis=1, keepdims=True)
    fn = np.divide(fn, ln, out=np.zeros_like(fn), where=ln > 0)
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    ctr, rad = (lo + hi) / 2, (hi - lo).max() / 2

    views = [(25, -60, "3/4"), (90, -90, "top"), (0, -90, "side")]
    fig = plt.figure(figsize=(18, 6.4), dpi=100)
    for k, (elev, azim, label) in enumerate(views):
        ax = fig.add_subplot(1, 3, k + 1, projection="3d")
        e, a = math.radians(elev), math.radians(azim)
        light = np.array([math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)])
        light = 0.7 * light + 0.3 * np.array([0.3, 0.2, 1.0])
        light /= np.linalg.norm(light)
        shade = 0.25 + 0.75 * np.abs(fn @ light)
        col = np.stack([shade * 0.55, shade * 0.65, shade * 0.8, np.ones_like(shade)], 1)
        pc = Poly3DCollection(T, facecolors=col, edgecolors="none", linewidths=0)
        ax.add_collection3d(pc)
        for setlim, c in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), ctr):
            setlim(c - rad, c + rad)
        ax.set_box_aspect((1, 1, 1))
        ax.view_init(elev=elev, azim=azim)
        ax.set_axis_off()
        ax.set_title(label)
    sub = f"  ({ntri} triangles" + (f", animation frame {args.frame}" if args.frame is not None else "") + ")"
    fig.suptitle((args.title or os.path.basename(args.gltf)) + sub)
    fig.tight_layout()
    fig.savefig(args.png)
    print("wrote", args.png, ntri, "triangles")


if __name__ == "__main__":
    main()
