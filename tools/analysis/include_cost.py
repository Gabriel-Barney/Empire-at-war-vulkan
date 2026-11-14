#!/usr/bin/env python3
"""Include-cost report over engine/ (bo3 t01): which headers are pulled into the most
translation units, and which TUs pull in the most header text.

Static scan (no compiler): follows `#include "..."` / `<...>` that resolve to a file under
engine/ (module include dirs), counts system headers by name only.

  python tools/analysis/include_cost.py [--top N] [--json out.json]
"""
import argparse, json, os, re, sys
from collections import defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ENG = os.path.join(ROOT, "engine")
INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def collect():
    srcs, hdrs, inc_dirs = [], {}, []
    for d, _, fs in os.walk(ENG):
        if "third_party" in d or os.sep + "build" in d:
            continue
        if os.path.basename(d) == "include":
            inc_dirs.append(d)
        for f in fs:
            p = os.path.join(d, f)
            if f.endswith((".cpp", ".cc")):
                srcs.append(p)
            elif f.endswith((".hpp", ".h")):
                hdrs[p] = os.path.getsize(p)
    return srcs, hdrs, inc_dirs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--json")
    a = ap.parse_args()
    srcs, hdrs, inc_dirs = collect()
    by_rel = {}
    for h in hdrs:
        for d in inc_dirs:
            if h.startswith(d + os.sep):
                by_rel[os.path.relpath(h, d).replace("\\", "/")] = h
    raw = {}

    def direct(p):
        if p not in raw:
            try:
                txt = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                txt = ""
            raw[p] = INC.findall(txt)
        return raw[p]

    def resolve(frm, kind, name):
        if kind == '"':
            c = os.path.normpath(os.path.join(os.path.dirname(frm), name))
            if c in hdrs:
                return c
        return by_rel.get(name)

    closure_cache = {}

    def closure(p):  # (project headers set, system headers set)
        if p in closure_cache:
            return closure_cache[p]
        closure_cache[p] = (set(), set())  # cycle guard
        ph, sh = set(), set()
        for kind, name in direct(p):
            r = resolve(p, kind, name)
            if r:
                ph.add(r)
                a1, b1 = closure(r)
                ph |= a1
                sh |= b1
            elif kind == "<":
                sh.add(name)
        closure_cache[p] = (ph, sh)
        return ph, sh

    hcount, scount = defaultdict(int), defaultdict(int)
    tu = []
    for s in srcs:
        ph, sh = closure(s)
        for h in ph:
            hcount[h] += 1
        for h in sh:
            scount[h] += 1
        tu.append((sum(hdrs[h] for h in ph), len(ph), os.path.getsize(s), s))
    rel = lambda p: os.path.relpath(p, ENG).replace("\\", "/")
    tot_bytes = sum(t[0] for t in tu)
    print(f"{len(srcs)} .cpp, {len(hdrs)} headers; total project-header bytes over all TUs: {tot_bytes/1e6:.1f} MB")
    print(f"\nTop {a.top} project headers by number of TUs including them (transitively):")
    for h, n in sorted(hcount.items(), key=lambda x: -x[1])[: a.top]:
        print(f"  {n:4d}  {hdrs[h]/1024:6.1f} KB  {rel(h)}")
    print(f"\nTop {a.top} headers by TUs x size (MB of header text parsed in total):")
    for h, n in sorted(hcount.items(), key=lambda x: -x[1] * hdrs[x[0]])[: a.top]:
        print(f"  {n*hdrs[h]/1e6:6.2f} MB  {n:4d} TUs  {rel(h)}")
    print(f"\nTop 10 std headers by TUs:")
    for h, n in sorted(scount.items(), key=lambda x: -x[1])[:10]:
        print(f"  {n:4d}  <{h}>")
    print(f"\nTop {a.top} TUs by transitive project-header bytes:")
    for b, n, sz, s in sorted(tu, reverse=True)[: a.top]:
        print(f"  {b/1024:7.0f} KB in {n:3d} headers, own {sz/1024:5.0f} KB  {rel(s)}")
    print(f"\nTop {a.top} largest .cpp files:")
    for b, n, sz, s in sorted(tu, key=lambda t: -t[2])[: a.top]:
        print(f"  {sz/1024:6.0f} KB  {rel(s)}")
    if a.json:
        json.dump({"tus": len(srcs), "headers": {rel(h): n for h, n in hcount.items()},
                   "std": dict(scount), "total_header_bytes": tot_bytes,
                   "tu": {rel(s): [b, n, sz] for b, n, sz, s in tu}}, open(a.json, "w"))


main()
