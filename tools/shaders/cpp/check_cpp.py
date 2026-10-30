#!/usr/bin/env python3
"""check_cpp.py - compare fx2dump (C++) against fx2.py on a set of .fxo files.
Usage: python -I check_cpp.py <fx2dump binary> <file.fxo>..."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import d3d9sm  # noqa: E402
import fx2  # noqa: E402


def py_line(path):
    e = fx2.parse(open(path, "rb").read())
    passes = sum(len(t["passes"]) for t in e.techniques)
    states = sum(len(p["states"]) for t in e.techniques for p in t["passes"])
    blobs = list(e.shader_blobs())
    ctab = sum(len((d3d9sm.parse(b).ctab or {"constants": []})["constants"]) for _, _, b in blobs)
    lods = []
    for t in e.techniques:
        lod = next((e.value_repr(a["value"]) for a in t["annotations"] if a["name"] == "LOD"), None)
        lods.append(lod or "-")
    return "%s\tparams=%d\ttechs=%d\tpasses=%d\tstates=%d\tshaders=%d\tctab=%d\tlods=%s" % (
        path, len(e.parameters), len(e.techniques), passes, states, len(blobs), ctab, ",".join(lods))


def main(argv):
    exe, files = argv[1], argv[2:]
    out = subprocess.run([exe] + files, capture_output=True, text=True).stdout.splitlines()
    bad = 0
    for f, line in zip(files, out):
        if line != py_line(f):
            bad += 1
            print("DIFF\n  cpp: %s\n  py:  %s" % (line, py_line(f)))
    print("%d files, %d differences" % (len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
