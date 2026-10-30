#!/usr/bin/env python3
"""crosscheck_vkd3d.py - compare d3d9sm.py's disassembly against vkd3d-shader's
d3dbc -> d3d-asm output, instruction by instruction (mnemonic + operand registers).

Usage: python -I crosscheck_vkd3d.py <vkc-binary> <blob dir> [report.json]

<vkc-binary> is the tiny vkd3d-shader driver built by build_vkd3d.sh
("vkc d3dbc asm in.bin out.txt").
"""
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d9sm  # noqa: E402

_SKIP = ("vs_", "ps_", "//")


def _norm_line(line):
    line = line.strip()
    if not line or line.startswith(_SKIP):
        return None
    line = line.replace(" = ", ", ")          # vkd3d prints "def c0 = 1, 2, 3, 4"
    line = line.lstrip("+")
    op, _, rest = line.partition(" ")
    op = re.sub(r"(_sat|_pp|_centroid|_x2|_x4|_x8|_d2|_d4|_d8)+$", "", op)
    if op.startswith("dcl"):
        op = "dcl"
    if op == "texcrd":
        op = "texcoord"
    if op == "texld":
        op = "tex"
    # registers only (drop swizzles / masks / literal formatting)
    regs = re.findall(r"\b(oPos|oFog|oPts|oD\d+|oT\d+|oC\d+|oDepth|[rvctsibp]\d+)\b", rest)
    return op, tuple(regs)


def _mine(blob):
    sh = d3d9sm.parse(blob)
    out = []
    for ins in sh.instructions:
        n = _norm_line(sh.fmt_ins(ins))
        if n:
            out.append(n)
    return sh, out


def _theirs(vkc, path):
    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tf:
        tmp = tf.name
    try:
        r = subprocess.run([vkc, "d3dbc", "asm", path, tmp], capture_output=True, text=True,
                           timeout=60)
        if r.returncode != 0:
            return None, r.stderr.strip()
        txt = open(tmp, encoding="latin-1").read()
    finally:
        os.unlink(tmp)
    out = [n for n in (_norm_line(l) for l in txt.splitlines()) if n]
    return out, r.stderr.strip()


def main(argv):
    vkc, d = argv[1], argv[2]
    rep = {"blobs": 0, "match": 0, "mismatch": [], "vkd3d_failed": [], "unknown_opcodes": {},
           "profiles": {}}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".bin"):
            continue
        p = os.path.join(d, fn)
        blob = open(p, "rb").read()
        sh, mine = _mine(blob)
        rep["blobs"] += 1
        rep["profiles"][sh.profile] = rep["profiles"].get(sh.profile, 0) + 1
        for k, v in sh.unknown_opcodes.items():
            rep["unknown_opcodes"][str(k)] = rep["unknown_opcodes"].get(str(k), 0) + v
        with tempfile.NamedTemporaryFile(suffix=".spv", delete=False) as tf:
            spv = tf.name
        r = subprocess.run([vkc, "d3dbc", "spirv", p, spv], capture_output=True, text=True, timeout=60)
        os.unlink(spv)
        if r.returncode == 0:
            rep["spirv_ok"] = rep.get("spirv_ok", 0) + 1
        else:
            msg = re.sub(r"^<anonymous>:\d+:\d+: ", "", (r.stderr.strip().splitlines() or ["?"])[0])
            rep.setdefault("spirv_fail", {}).setdefault(msg, []).append(fn)
        theirs, err = _theirs(vkc, p)
        if theirs is None:
            rep["vkd3d_failed"].append({"blob": fn, "err": err[:300]})
            continue
        rep["instructions_compared"] = rep.get("instructions_compared", 0) + min(len(mine), len(theirs))
        if mine == theirs:
            rep["match"] += 1
        else:
            first = next((i for i, (a, b) in enumerate(zip(mine, theirs)) if a != b),
                         min(len(mine), len(theirs)))
            rep["mismatch"].append({"blob": fn, "profile": sh.profile, "mine": len(mine),
                                    "vkd3d": len(theirs), "first_diff": first,
                                    "mine_at": mine[first] if first < len(mine) else None,
                                    "vkd3d_at": theirs[first] if first < len(theirs) else None})
    print("blobs %d, identical %d, mismatched %d, vkd3d failed %d, unknown opcodes %s" % (
        rep["blobs"], rep["match"], len(rep["mismatch"]), len(rep["vkd3d_failed"]),
        rep["unknown_opcodes"] or "none"))
    print("vkd3d d3dbc->SPIR-V: %d ok, failures: %s" % (rep.get("spirv_ok", 0), {k: len(v) for k, v in rep.get("spirv_fail", {}).items()}))
    for m in rep["mismatch"][:20]:
        print("  MISMATCH", m)
    for m in rep["vkd3d_failed"][:10]:
        print("  VKD3D-FAIL", m)
    if len(argv) > 3:
        json.dump(rep, open(argv[3], "w"), indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
