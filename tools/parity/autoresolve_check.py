"""Galactic auto-resolve parity check (bkc1 t08, backlog b0027; ctest parity_autoresolve).

Reads the retail battles of autoresolve_refs.json (each battle's "grade" block), writes the battle spec the port's
alamo_galactic_autoresolve_parity reads, runs it over many seeds and grades the port's outcomes against retail:

  winner      the retail winner in >= 90% of the seeds (the winner rule is deterministic; the RNG only orders the
              survival roll);
  losses      for every combatant type in the battle: the retail count destroyed (0 when not listed) inside the port's
              5th..95th percentile band over the seeds (parity tier 2, docs/DECISIONS.md "Parity tiers");
  starbase    the retail starbase outcome (destroyed / surviving level) in >= 5% of the seeds;
  total       per side, the retail number of combatants destroyed inside the port's band (same rule as losses).

A check named in the battle's grade "known_fail" ({"check name": "backlog id"}) is reported KNOWN (with its backlog
entry) and does not fail the run, like the harness baseline; a known failure that passes now is reported FIXED so the
entry can be removed.

  python -I autoresolve_check.py run --exe <alamo_galactic_autoresolve_parity> --refs autoresolve_refs.json --out <dir>
  python -I autoresolve_check.py selftest

Exit 0 pass, 1 fail, 77 skip (the exe found no FoC data).
"""

import argparse
import json
import os
import subprocess
import sys

WINNER_MIN = 0.9
STARBASE_MIN = 0.05
BAND = (0.05, 0.95)


def write_spec(refs, path):
    lines = ["# written by autoresolve_check.py from autoresolve_refs.json"]
    for b in refs["battles"]:
        g = b.get("grade")
        if not g:
            continue
        pid = g["players"]
        lines.append("battle %s %s %d %d" % (b["id"], b["kind"], pid[b["attacker"]], pid[b["defender"]]))
        for fac, ai, playable in g["sides"]:
            lines.append("side %d %d %d" % (pid[fac], ai, playable))
        for fac, typ, n in g["combatants"]:
            lines.append("unit %d %s %d" % (pid[fac], typ, n))
        if g.get("starbase"):
            lines.append("starbase %d %s" % (pid[g["starbase"][0]], g["starbase"][1]))
        lines.append("end")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def parse_output(text):
    """-> {battle id: {seed: {"winner": p, "lost": {(p, type lower): n}, "starbase": name lower}}}"""
    out = {}
    for line in text.splitlines():
        t = line.split()
        if not t or t[0].startswith("#"):
            continue
        if t[0] == "result":
            out.setdefault(t[1], {})[int(t[2])] = {"winner": int(t[3]), "lost": {}, "starbase": None}
        elif t[0] == "lost":
            out[t[1]][int(t[2])]["lost"][(int(t[3]), t[4].lower())] = int(t[5])
        elif t[0] == "starbase":
            out[t[1]][int(t[2])]["starbase"] = t[4].lower()
    return out


def percentile(sorted_vals, q):
    if not sorted_vals:
        return 0
    k = int(round(q * (len(sorted_vals) - 1)))
    return sorted_vals[k]


def grade(refs, results):
    """-> (list of (battle, check, ok, detail), all ok)"""
    rows = []
    for b in refs["battles"]:
        g = b.get("grade")
        if not g:
            continue
        runs = results.get(b["id"], {})
        if not runs:
            rows.append((b["id"], "runs", False, "no port output"))
            continue
        pid = g["players"]
        n = len(runs)
        win = sum(1 for r in runs.values() if r["winner"] == pid[g["winner"]]) / n
        rows.append((b["id"], "winner", win >= WINNER_MIN, "%s in %.0f%% of %d seeds" % (g["winner"], 100 * win, n)))
        seen = []
        for fac, typ, _ in g["combatants"]:
            key = (pid[fac], typ.lower())
            if key in seen:
                continue
            seen.append(key)
            ref = g["losses"].get(fac, {}).get(typ, 0)
            vals = sorted(r["lost"].get(key, 0) for r in runs.values())
            lo, hi = percentile(vals, BAND[0]), percentile(vals, BAND[1])
            mean = sum(vals) / n
            rows.append((b["id"], "lost %s %s" % (fac, typ), lo <= ref <= hi,
                         "retail %d, port mean %.2f band [%d, %d]" % (ref, mean, lo, hi)))
        for fac in sorted(pid, key=lambda f: pid[f]):
            ref = sum(g["losses"].get(fac, {}).values())
            vals = sorted(sum(v for (p, _), v in r["lost"].items() if p == pid[fac]) for r in runs.values())
            lo, hi = percentile(vals, BAND[0]), percentile(vals, BAND[1])
            rows.append((b["id"], "total lost %s" % fac, lo <= ref <= hi,
                         "retail %d, port mean %.2f band [%d, %d]" % (ref, sum(vals) / n, lo, hi)))
        if g.get("starbase"):
            want = g.get("starbase_after", "destroyed").lower()
            share = sum(1 for r in runs.values() if r["starbase"] == want) / n
            rows.append((b["id"], "starbase", share >= STARBASE_MIN,
                         "retail %s, port %.0f%% of seeds" % (want, 100 * share)))
    known = {}
    for b in refs["battles"]:
        for check, entry in (b.get("grade") or {}).get("known_fail", {}).items():
            known[(b["id"], check)] = entry
    out = []
    for bid, check, ok, detail in rows:
        k = known.get((bid, check))
        status = ("FIXED" if ok else "KNOWN") if k else ("PASS" if ok else "FAIL")
        out.append((bid, check, status, detail + (" (%s)" % k if k else "")))
    return out, all(r[2] != "FAIL" for r in out)


def report(rows):
    for bid, check, status, detail in rows:
        print("%-5s %-22s %-48s %s" % (status, bid, check, detail))
    count = lambda s: sum(1 for r in rows if r[2] == s)
    print("autoresolve parity: %d checks, %d FAIL, %d KNOWN, %d FIXED" % (len(rows), count("FAIL"), count("KNOWN"),
                                                                          count("FIXED")))


def cmd_run(a):
    with open(a.refs, encoding="utf-8") as f:
        refs = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    spec = os.path.join(a.out, "autoresolve_spec.txt")
    write_spec(refs, spec)
    p = subprocess.run([a.exe, "--spec", spec, "--seeds", str(a.seeds)], capture_output=True, text=True)
    with open(os.path.join(a.out, "autoresolve_port.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(p.stdout)
    if p.returncode == 77:
        print(p.stdout.strip())
        print("autoresolve parity: skipped (no FoC data)")
        return 77
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        print("autoresolve parity: port exe failed (%d)" % p.returncode)
        return 1
    rows, ok = grade(refs, parse_output(p.stdout))
    report(rows)
    return 0 if ok else 1


def cmd_selftest(_a):
    refs = {"battles": [{"id": "b", "kind": "space", "attacker": "R", "defender": "E", "grade": {
        "players": {"R": 0, "E": 1}, "sides": [["R", 1, 1], ["E", 0, 1]],
        "combatants": [["E", "Ship", 1], ["R", "Frig", 3]], "starbase": ["E", "SB_1"],
        "winner": "R", "losses": {"R": {"Frig": 1}, "E": {"Ship": 1}}, "starbase_after": "destroyed"}}]}
    good = "\n".join(["result b %d 0 1 -1\nlost b %d 1 Ship 1\nlost b %d 0 Frig %d\nstarbase b %d 1 destroyed"
                      % (s, s, s, 1 + (s % 3 == 0), s) for s in range(20)])
    rows, ok = grade(refs, parse_output(good))
    assert ok and all(r[2] == "PASS" for r in rows), rows
    bad = good.replace("lost b", "#")
    rows, ok = grade(refs, parse_output(bad))
    assert not ok and any(r[1] == "lost R Frig" and r[2] == "FAIL" for r in rows), rows
    refs["battles"][0]["grade"]["known_fail"] = {"lost R Frig": "b0000", "total lost R": "b0000",
                                                 "lost E Ship": "b0000", "total lost E": "b0000"}
    rows, ok = grade(refs, parse_output(bad))
    assert ok and any(r[1] == "lost R Frig" and r[2] == "KNOWN" for r in rows), rows
    rows, ok = grade(refs, parse_output(good))
    assert ok and any(r[1] == "lost R Frig" and r[2] == "FIXED" for r in rows), rows
    del refs["battles"][0]["grade"]["known_fail"]
    spec_path = os.path.join(os.environ.get("TEMP", "."), "alamo_autoresolve_selftest_spec_%d.txt" % os.getpid())
    write_spec(refs, spec_path)
    with open(spec_path, encoding="utf-8") as f:
        spec = f.read()
    os.remove(spec_path)
    assert "battle b space 0 1" in spec and "unit 0 Frig 3" in spec and "starbase 1 SB_1" in spec, spec
    print("autoresolve_check selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--refs", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--seeds", type=int, default=200)
    sub.add_parser("selftest")
    a = ap.parse_args()
    return cmd_run(a) if a.cmd == "run" else cmd_selftest(a)


if __name__ == "__main__":
    sys.exit(main())
