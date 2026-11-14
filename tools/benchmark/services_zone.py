#!/usr/bin/env python3
"""SP5 t07: the services zone of a bench result - the sum of the tac.s.* zones (one per service of
TacticalBattle's services graph, sp0's step names), without
the steps that are not services (the movement and combat ticks, the stand-in orders that run instead of the tactical AI)
and without the profile-lap barriers.

    python tools/benchmark/services_zone.py RESULT.json [RESULT2.json ...]

Prints per file: the services sum (mean ms per tick, and the sum of the per-zone medians), the tick median, and the five
largest services. Compare two worker counts of one scene (e.g. tools/benchmark/stress_land4.cmd 0 / 7) measured in one
interleaved run and note the machine load beside the numbers (other lanes' builds skew them).
"""
from __future__ import annotations

import json
import sys

NOT_SERVICES = {"tac.s.move_tick", "tac.s.move_commit", "tac.s.abilities_combat", "tac.s.issue_orders"}


def services(path: str) -> tuple[float, float, dict, list]:
    d = json.load(open(path, encoding="utf-8"))
    zs = [z for z in d.get("zones", []) if z["name"].startswith("tac.s.") and z["name"] not in NOT_SERVICES]
    mean = sum(z["mean_ms"] for z in zs)
    p50 = sum(z["p50_ms"] for z in zs)
    return mean, p50, d.get("summary", {}), sorted(zs, key=lambda z: -z["mean_ms"])[:5]


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    for p in argv:
        mean, p50, summ, top = services(p)
        print(f"{p}: services mean {mean:.3f} ms / tick (sum of medians {p50:.3f} ms); tick median {summ.get('median_ms', 0):.3f} ms, "
              f"other CPU {summ.get('max_other_cpu_pct', '?')}%")
        for z in top:
            print(f"    {z['name']:<32} mean {z['mean_ms']:.3f}  p50 {z['p50_ms']:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
