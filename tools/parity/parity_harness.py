#!/usr/bin/env python3
"""Q4 behavioural parity harness (roadmap group q4h): the original game's Q2 reference runs against the port.

Standard library only. Parity is checked by behaviour, not by CRC or hash equality (Gabriel, 2026-10-29): travel and build
timings, income, AI choices, battle outcomes and damage, each against a tolerance written down with its reason in
tools/parity/scenarios.json. Replaces the `q2trace.py compare` step of Q2F t07; the trace, production-queue and AI-log
parsers are reused from tools/probe_mod/q2_traces/analysis/q2_parity.py.

  parity_harness.py run [--scenario ID ...] [--port-exe alamo_headless.exe] [--refs DIR] [--out DIR]
                        [--reuse-port] [--check-baseline FILE] [--write-baseline FILE] [--commit SHA]
      Replays every scenario in the port (alamo_headless, same campaign / map, seed and tick count as the reference runs),
      reduces the reference runs and the port run to the same metrics and grades each metric. Writes
      <out>/<scenario>/report.txt and report.json (default out: tools/parity/out, ignored by git).
      Exit 0: no metric out of tolerance (or, with --check-baseline, no metric that passed in the baseline fails now);
      1: out of tolerance / regression; 2: error; 77: skipped (ALAMO_FOC_DATA or the reference runs missing).
  parity_harness.py retail [--scenario ID ...] [--refs DIR]
      The retail runs of a scenario against each other: the run-to-run spread the tolerances must allow.
  parity_harness.py list [--refs DIR]
      Scenarios, their reference runs and whether they are present on this machine.
  parity_harness.py selftest
      Small synthetic fixtures (written to a temp folder): metric extraction, tolerance grading, baseline check.

Reference runs (Q2 kit, tools/probe_mod/q2_traces/README.md) live in the kit's ignored out\\ folder. In a git worktree
that folder is empty, so the default --refs is the main checkout's copy (found through `git rev-parse
--git-common-dir`); ALAMO_Q2_TRACES overrides it.

Grading. Several retail runs of one scenario differ from each other (the galaxy uses no input, but the original's AI
reacts to wall-clock timing), so a scalar metric is graded against the retail envelope: PASS when the port value lies in
[min(retail) - tol, max(retail) + tol], tol = max(abs, rel * max(|min|, |max|)). A type mix (AI builds, fleet targets) is
graded by its overlap with the closest retail run: sum(min(a, b)) / max(sum(a), sum(b)) >= the scenario's threshold.
INFO metrics (RNG draws) are reported, never graded. N/A names the side that has no data and why.
"""
import argparse
import collections
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
KIT_REL = os.path.join("tools", "probe_mod", "q2_traces")
sys.path.insert(0, os.path.join(REPO, KIT_REL, "analysis"))
import q2_parity as q2  # noqa: E402  (tools/probe_mod/q2_traces/analysis/q2_parity.py: trace / queue / AI-log parsers)

SKIP = 77
FACTIONS = tuple(q2.FACTION_TOKENS)  # Empire, Rebel, Underworld
CATEGORY = {"travel": "timing", "build": "timing", "econ": "income", "ai": "AI choices", "outcome": "battle outcomes",
            "combat": "damage", "diag": "diagnostic"}
# Skirmish objects that are not units or structures: projectiles, team containers, props, markers, pads (the port lists pad
# contents apart), construction stand-ins, squadron containers and starbase-upgrade objects.
NOT_UNIT = ("PROJ_", "_MARKER", "BACKDROP", "ASTEROID", "NEBULA", "STARS_", "STAR_BACKDROP", "SPACE_JUNK", "DEATH_CLONE",
            "_PAD", "CONTAINER", "_UPGRADE", "MERCHANT_DOCK", "EXPLOSION", "DAMAGE_SPACE", "DAMAGE_LAND", "PARTICLE")


# ---- small helpers ---------------------------------------------------------------------------------------------------
def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def write_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=1, sort_keys=True, default=str)
        fh.write("\n")


def is_unit(name):
    n = name.upper()
    if n == "TEAM" or n.startswith("UC_"):
        return False
    return not any(t in n for t in NOT_UNIT)


def overlap(a, b):
    """Multiset overlap of two Counters: sum(min) / max(sum a, sum b); 1.0 when both are empty."""
    sa, sb = sum(a.values()), sum(b.values())
    if sa == 0 and sb == 0:
        return 1.0
    return sum(min(a[k], b[k]) for k in set(a) | set(b)) / max(sa, sb)


def assign_factions(by_owner, allowed=FACTIONS):
    """{owner: [type names]} -> {owner: faction}: each faction goes to the owner with the most of its unit-name tokens
    (neutral props such as STAR_DESTROYER_DEATH_CLONE must not claim a faction)."""
    score = {}
    for own, names in by_owner.items():
        for n in names:
            for fac, toks in q2.FACTION_TOKENS.items():
                if any(t in n.upper() for t in toks):
                    score[(fac, own)] = score.get((fac, own), 0) + 1
    out = {}
    for fac in allowed:
        cands = sorted(((s, -own) for (f, own), s in score.items() if f == fac), reverse=True)
        for s, nown in cands:
            if -nown not in out:
                out[-nown] = fac
                break
    return out


def median(xs):
    return statistics.median(xs) if xs else None


def default_refs():
    env = os.environ.get("ALAMO_Q2_TRACES")
    if env:
        return env
    here = os.path.join(REPO, KIT_REL, "out")
    # Only a folder that holds a run counts: the kit's selftest makes out\selftest in every worktree it runs in, and
    # that empty folder used to hide the main checkout's runs (the slow ctests then skipped with 77).
    if os.path.isdir(here) and any(os.path.isfile(os.path.join(here, d, "trace.q2t")) for d in os.listdir(here)):
        return here
    try:
        common = subprocess.run(["git", "-C", REPO, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=False).stdout.strip()
    except OSError:
        common = ""
    if common:
        main = os.path.join(os.path.dirname(common), KIT_REL, "out")
        if os.path.isdir(main):
            return main
    return here


def default_port_exe():
    for tree in ("build-wt-msvc", "build-msvc", "build-wt-clang", "build-clang"):
        p = os.path.join(REPO, tree, "bin", "alamo_headless.exe")
        if os.path.exists(p):
            return p
    return None


# ---- metric extraction: galactic ----------------------------------------------------------------------------------------
def galactic_credit_pass(trace_path, factions, sample_ticks):
    """Credits per faction after each frame: the income paid on each fiscal-cycle frame (credits after that frame minus
    credits after the previous sampled frame; the trace shows no spending on cycle frames) and credits at sample ticks."""
    players, before, income = {}, {}, collections.defaultdict(list)
    at = {}
    last_cycle, cycle_frame = None, None
    mf = None
    want = sorted(sample_ticks)

    def close_frame():
        if cycle_frame is not None and cycle_frame == mf:
            for i, v in players.items():
                f = factions.get(i)
                if f in FACTIONS:
                    income[f].append(v - before.get(i, v))
        for t in want:
            if mf is not None and mf <= t:
                at[t] = {factions.get(i): v for i, v in players.items() if factions.get(i) in FACTIONS}
    with open(trace_path, "r", encoding="ascii") as fh:
        for line in fh:
            if line.startswith("F "):
                p = line.split()
                if mf is not None:
                    close_frame()
                before = dict(players)
                mf = int(p[3])
                if len(p) >= 11:
                    cyc = int(p[10])
                    if last_cycle is not None and cyc != last_cycle:
                        cycle_frame = mf
                    last_cycle = cyc
            elif line.startswith("C "):
                p = line.split()
                players[int(p[1])] = q2.f32(p[2])
            elif line.strip() == "end":
                break
    if mf is not None:
        close_frame()
    return dict(income), at


def galactic_metrics(trace_path, queue_path=None, ai_log=None, sample_ticks=(4500, 9000, 13500)):
    """Metric values of one galactic run (retail trace or port state trace), {metric id: value}, plus notes."""
    m = q2.trace_metrics(trace_path)
    fac = m["factions"]
    out, notes = {}, []
    # Travel: hop duration per route (departure = first moved sample, arrival = first parked sample; same rule both sides).
    # Keyed by faction: the hop speed is the fleet's (its units' Hyperspace_Speed, the Underworld corruption bonus).
    routes = collections.defaultdict(list)
    for dep, f, a, b, arr in m["fleet_hops"] or []:
        routes["%s:%s->%s" % (f, a, b)].append(arr - dep)
    for r, ds in routes.items():
        out["travel.hop_ticks[%s]" % r] = tuple(sorted(set(ds)))
    # AI: fleet departures and their targets per faction (hops from positions on both sides).
    hops = collections.defaultdict(list)
    for dep, f, a, b, arr in m["fleet_hops"] or []:
        hops[f].append((dep, b))
    for f in FACTIONS:
        hs = hops.get(f, [])
        out["ai.fleet_departures[%s]" % f] = len(hs)
        out["ai.fleet_targets[%s]" % f] = collections.Counter(b for _, b in hs)
        if hs:
            out["ai.first_departure_tick[%s]" % f] = min(d for d, _ in hs)
    # Timing: fiscal cycles (the trace's sample window of each change; the port samples every tick).
    fc = m["fiscal_cycles"]
    if fc:
        for k, (lo, hi) in enumerate(fc["cycle_windows"], 1):
            out["econ.cycle_tick[%d]" % k] = (lo, hi) if lo != hi else hi
    # Income: per-cycle payment and credits at sample ticks.
    income, at = galactic_credit_pass(trace_path, fac, sample_ticks)
    for f, xs in income.items():
        if xs:
            out["econ.income_cycle1[%s]" % f] = xs[0]
            out["econ.income_mean[%s]" % f] = sum(xs) / len(xs)
    for t, d in sorted(at.items()):
        for f, v in d.items():
            out["econ.credits_at_%d[%s]" % (t, f)] = v
    if not income:
        notes.append("no credits in the trace (reader v1 found no player vector)")
    # Production: retail queue starts (production.q2q) or port build orders (AI log); build time per type.
    builds = None
    durations = collections.defaultdict(list)
    if queue_path and os.path.exists(queue_path):
        q2.queue_metrics(queue_path, m)
        builds = [(e["start"], e["faction"], e["type"]) for e in m["queue_entries"]]
        # An item leaves the queue when its object is created (the created-id field stays unset in the retail queues), so
        # done = the last frame listed + 1. Only items that ran uninterrupted to the end count: no restart (a stalled item
        # restarts every tick), gone before the trace ends, and listed for at least its own build frames (an item that
        # leaves earlier was cancelled).
        end = max((e["last"] for e in m["queue_entries"]), default=0)
        for e in m["queue_entries"]:
            d = e["done"] - e["last_start"]
            if e["restarts"] == 0 and (e["created"] is not None or e["last"] < end) and d >= e["frames"]:
                durations["%s@%s" % (e["type"], e["planet"])].append(d)
    elif ai_log and os.path.exists(ai_log):
        o = q2.port_metrics(None, ai_log)
        orders = [(tk, f, at_, t) for tk, f, at_, d in o["production"] for t in d]
        builds = [(tk, f, t) for tk, f, _, t in orders] + [(tk, f, t) for tk, f, _, t in o.get("starbase_orders") or []]
        # Completion = the first new object of that type and faction at that planet after the order (port state trace).
        done = [(tk, f, at_, t) for tk, f, at_, d in m["production"] for t in d]
        used = set()
        for tk, f, at_, t in sorted(orders):
            for j, (ctk, cf, cat, ct) in enumerate(done):
                if j in used or ctk < tk or cf != f or ct != t or (at_ and cat and cat != at_):
                    continue
                used.add(j)
                durations["%s@%s" % (t, cat or at_)].append(ctk - tk)
                break
    else:
        notes.append("no production data (no production.q2q and no AI log): build metrics left out")
    # The shortest completed build per type and planet (planets can carry build-time modifiers): the build time under the best modifiers, free of stalls and of port
    # orders that were dropped (those match a later completion and look longer).
    for t, ds in durations.items():
        out["build.ticks[%s]" % t] = min(ds)
    if builds is not None:
        for f in FACTIONS:
            bf = [b for b in builds if b[1] == f]
            out["ai.build_count[%s]" % f] = len(bf)
            out["ai.build_mix[%s]" % f] = collections.Counter(t for _, _, t in bf)
            if bf:
                out["ai.first_build_tick[%s]" % f] = min(tk for tk, _, _ in bf)
    # Battle outcomes: planets owned per faction at the end, captures per faction.
    if m["owners_initial"] is not None:
        own = dict(m["owners_initial"])
        caps = collections.Counter()
        for tk, n, a, b in m["owner_changes"]:
            own[n] = b
            caps[b] += 1
        cnt = collections.Counter(own.values())
        for f in FACTIONS:
            out["outcome.planets_final[%s]" % f] = cnt.get(f, 0)
            out["outcome.captures[%s]" % f] = caps.get(f, 0)
        out["outcome.owner_changes"] = len(m["owner_changes"])
    # Diagnostic: synced-RNG draws (not graded: parity is behaviour, and the draw order is an implementation detail).
    if m["rng_draws"]:
        out["diag.rng_draws_total"] = sum(n for _, _, n in m["rng_draws"] if n is not None)
    return out, notes, {"factions": fac, "ticks": m["scenario"]["ticks"], "seed": m["scenario"]["seed"],
                        "campaign": m["scenario"]["campaign"]}


# ---- metric extraction: skirmish ----------------------------------------------------------------------------------------
def skirmish_snapshot_metrics(snaps, credits, frames, out):
    """snaps: {frame: {faction: Counter(unit type)}}, credits: {frame: {faction: credits}}."""
    have = sorted(snaps)
    if not have:
        return
    use = [f for f in have if f in frames or f == have[0]]
    for f in FACTIONS:
        built, lost = collections.Counter(), 0
        prev = None
        for fr in use:
            cur = snaps[fr].get(f, collections.Counter())
            if prev is not None:
                for t in set(cur) | set(prev):
                    d = cur[t] - prev[t]
                    if d > 0:
                        built[t] += d
                    elif d < 0:
                        lost -= d
            prev = cur
        if not any(f in snaps[fr] for fr in use):
            continue
        out["ai.build_count[%s]" % f] = sum(built.values())
        out["ai.build_mix[%s]" % f] = built
        out["outcome.units_final[%s]" % f] = sum(snaps[use[-1]].get(f, collections.Counter()).values())
        out["outcome.units_lost[%s]" % f] = lost
        first = next((fr for fr in use[1:] if any(snaps[fr].get(f, collections.Counter())[t] >
                                                   snaps[use[0]].get(f, collections.Counter())[t] for t in snaps[fr].get(f, {}))),
                     None)
        if first is not None:
            out["ai.first_build_frame[%s]" % f] = first
    for fr in sorted(credits):
        if fr in frames and fr % 1800 == 0:
            for f, v in credits[fr].items():
                if f in FACTIONS:
                    out["econ.credits_at_%d[%s]" % (fr, f)] = v


def skirmish_retail_metrics(trace_path, frames):
    """Retail skirmish trace: unit snapshots, credits, damage taken (health lost) and the first shot."""
    types, meta = {}, {}
    with open(trace_path, "r", encoding="ascii") as fh:
        for line in fh:
            if line.startswith("T "):
                p = line.split()
                types[int(p[1])] = p[4]
    objs = {}  # id -> [owner, type, health]
    players, flags = {}, {}
    snaps, credits = {}, {}
    damage = collections.Counter()
    first_shot = None
    owner_fac = {}
    mf = None
    end_reason = None

    def close(frame):
        if frame is None:
            return
        if not owner_fac and objs:
            by = collections.defaultdict(list)
            for o in objs.values():
                by[o[0]].append(o[1])
            # Only the scenario's two factions (meta "human Empire idle vs AI Rebel"): neutral props carry faction tokens
            # too (SKIRMISH_MERCHANT_DOCK reads as Underworld's MERC).
            named = tuple(f for f in FACTIONS if re.search(r"\b%s\b" % f, meta.get("skirmish", ""))) or FACTIONS
            owner_fac.update(assign_factions(by, named))
        if frame in frames or frame == 1:
            s = collections.defaultdict(collections.Counter)
            for o in objs.values():
                f = owner_fac.get(o[0])
                if f and is_unit(o[1]):
                    s[f][o[1].upper()] += 1
            snaps[frame] = dict(s)
            if players:
                credits[frame] = {cfac(i): v for i, v in players.items()}

    def cfac(i):
        sk = meta.get("skirmish", "")
        hm, am = re.search(r"human (\w+)", sk), re.search(r"AI (\w+)", sk)
        h, a = flags.get(i, (0, 0))
        if h and hm:
            return hm.group(1)
        if a and am:
            return am.group(1)
        return "slot%d" % i
    with open(trace_path, "r", encoding="ascii") as fh:
        in_header = True
        for line in fh:
            p = line.split()
            if not p:
                continue
            t = p[0]
            if in_header:
                if t == "meta":
                    meta[p[1]] = " ".join(p[2:])
                elif t == "end_header":
                    in_header = False
                continue
            if t == "F":
                close(mf)
                mf = int(p[3])
            elif t == "M":
                meta[p[1]] = " ".join(p[2:])
            elif t == "O":
                i = int(p[1])
                own, name, hp = int(p[2]), types.get(int(p[3]), "?"), q2.f32(p[10])
                if int(p[11]) & 2:
                    objs.pop(i, None)
                    continue
                old = objs.get(i)
                if old is None and name.upper().startswith("PROJ_") and first_shot is None and mf:
                    first_shot = mf
                if old is not None and owner_fac.get(own) and is_unit(name) and hp < old[2]:
                    damage[owner_fac[own]] += old[2] - hp
                objs[i] = [own, name, hp]
            elif t == "X":
                objs.pop(int(p[1]), None)
            elif t == "C":
                players[int(p[1])] = q2.f32(p[2])
                flags[int(p[1])] = (int(p[4]), int(p[5])) if len(p) >= 6 else (0, 0)
            elif t == "E":
                end_reason = " ".join(p[1:])
            elif t == "end":
                break
    close(mf)
    out = {}
    skirmish_snapshot_metrics(snaps, credits, frames, out)
    for f in FACTIONS:
        if f in owner_fac.values():
            out["combat.damage_taken[%s]" % f] = damage.get(f, 0.0)
    if first_shot is not None:
        out["combat.first_shot_frame"] = first_shot
    out["outcome.battle_ended"] = 0 if end_reason in ("ticks_reached", "frames_reached") else 1
    return out, [], {"factions": owner_fac, "ticks": mf, "seed": None, "label": meta.get("label"), "end": end_reason}


STATE_RE = re.compile(r"^state t=(\d+) battle frame (\d+):(.*)$")
PLAYER_RE = re.compile(r"p(\d+) credits (-?[\d.]+) objects (\d+);")
UNITS_RE = re.compile(r"p(\d+)\{([^}]*)\}")
PAD_RE = re.compile(r"\d+:\S+?/p(\d+)=(\S+)")


def skirmish_port_metrics(ai_log, frames, ticks_wanted, run_text=""):
    """Port skirmish host AI log (alamo_headless --skirmish-ai skirmish --ai-log): state blocks every 600 battle frames."""
    snaps, credits, slot_fac = {}, {}, {}
    cur = None
    with open(ai_log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            sm = STATE_RE.match(line)
            if sm:
                cur = int(sm.group(2))
                credits[cur] = {int(a): float(b) for a, b, _ in PLAYER_RE.findall(sm.group(3))}
                snaps[cur] = collections.defaultdict(collections.Counter)
                continue
            if cur is None:
                continue
            s = line.strip()
            if s.startswith("units:"):
                for slot, body in UNITS_RE.findall(s):
                    for tok in body.split():
                        name, _, n = tok.rpartition("x")
                        if name and n.isdigit() and is_unit(name):
                            snaps[cur][int(slot)][name.upper()] += int(n)
            elif s.startswith("pads:"):
                for slot, content in PAD_RE.findall(s):
                    if is_unit(content):
                        snaps[cur][int(slot)][content.upper()] += 1
    # Slot factions from what each slot owns at the start.
    if snaps:
        first = snaps[min(snaps)]
        slot_fac.update(assign_factions({slot: list(cnt.elements()) for slot, cnt in first.items()}))
    fsnaps = {fr: {slot_fac[s]: c for s, c in d.items() if s in slot_fac} for fr, d in snaps.items()}
    fcred = {fr: {slot_fac[s]: v for s, v in d.items() if s in slot_fac} for fr, d in credits.items()}
    # Port frame 0 is the state before the first battle tick; the retail trace's frame 1 snapshot is the first one taken.
    if 0 in fsnaps and 1 not in fsnaps:
        fsnaps[1] = fsnaps.pop(0)
    out = {}
    skirmish_snapshot_metrics(fsnaps, fcred, frames, out)
    m = re.search(r"skirmish-ai \S+ workers \d+: (\d+) ticks", run_text)
    if m:
        out["outcome.battle_ended"] = 1 if int(m.group(1)) < ticks_wanted else 0
    return out, [], {"factions": slot_fac, "ticks": max(snaps) if snaps else None}


# ---- grading ------------------------------------------------------------------------------------------------------------
def family(metric_id):
    return metric_id.split("[", 1)[0]


def bracket(metric_id):
    return metric_id.split("[", 1)[1].rstrip("]") if "[" in metric_id else None


def tolerance_for(tols, metric_id):
    """The most specific tolerance entry. A key is a family prefix ("ai", "econ.credits_at", "travel.hop_ticks"),
    optionally with a bracket ("econ.credits_at[Empire]" = that family for that faction only), or an exact metric id.
    Exact id wins, then the longest matching prefix, then a bracketed key over a bare one."""
    if metric_id in tols:
        return tols[metric_id]
    fam, br = family(metric_id), bracket(metric_id)
    best, best_key = None, None
    for k, v in tols.items():
        kfam, kbr = family(k), bracket(k)
        if not fam.startswith(kfam) or (kbr is not None and kbr != br):
            continue
        key = (len(kfam), kbr is not None)
        if best_key is None or key > best_key:
            best, best_key = v, key
    return best


def scalar_values(v):
    if isinstance(v, (tuple, list)):
        return [float(x) for x in v]
    return [float(v)]


def grade(metric_id, retail_vals, port_val, tol, port_note=None, retail_note=None):
    """One metric: retail_vals = values of the reference runs that have it (None left out), port_val or None."""
    cat = CATEGORY.get(metric_id.split(".", 1)[0], "other")
    res = {"metric": metric_id, "category": cat, "status": None, "retail": None, "port": None, "tolerance": None,
           "detail": ""}
    rv = [v for v in retail_vals if v is not None]
    is_mix = any(isinstance(v, collections.Counter) for v in rv) or isinstance(port_val, collections.Counter)
    if is_mix:
        res["retail"] = [dict(sorted(v.items())) for v in rv]
        res["port"] = dict(sorted(port_val.items())) if port_val is not None else None
    else:
        res["retail"] = rv
        res["port"] = port_val
    if tol is None or tol.get("info"):
        res["status"] = "INFO"
        res["detail"] = "port %s, retail %s; not graded" % (port_val, " | ".join(str(v) for v in rv) or "-")
        return res
    if not rv or port_val is None:
        res["status"] = "N/A"
        def show(v):
            return ("%d items" % sum(v.values())) if isinstance(v, collections.Counter) else "%g" % scalar_values(v)[0]
        if not rv:
            res["detail"] = "no retail value%s; port %s" % (" (%s)" % retail_note if retail_note else "", show(port_val))
        else:
            res["detail"] = "no port value%s; retail %s" % (" (%s)" % port_note if port_note else "", ", ".join(show(v) for v in rv))
        return res
    if is_mix:
        thr = float(tol.get("min_overlap", 1.0))
        best = max(overlap(port_val, r) for r in rv)
        pair = [overlap(rv[i], rv[j]) for i in range(len(rv)) for j in range(i + 1, len(rv))]
        if tol.get("retail_floor") and pair:
            thr = min(thr, round(min(pair), 3))  # as close to a retail run as the retail runs are to each other
        res["tolerance"] = {"min_overlap": thr}
        res["score"] = round(best, 3)
        res["status"] = "PASS" if best >= thr else "FAIL"
        res["detail"] = "overlap with the closest retail run %.2f (need >= %.2f)%s; port %d, retail %s items" % (
            best, thr, "; retail runs among themselves %.2f..%.2f" % (min(pair), max(pair)) if pair else "",
            sum(port_val.values()), "/".join(str(sum(r.values())) for r in rv))
        return res
    vals = [x for v in rv for x in scalar_values(v)]
    lo, hi = min(vals), max(vals)
    a, r = float(tol.get("abs", 0.0)), float(tol.get("rel", 0.0))
    pv = scalar_values(port_val)
    nr = "%d run%s" % (len(rv), "" if len(rv) == 1 else "s")
    if tol.get("nearest"):
        # Each port value must lie within tol of some retail value (a multi-modal metric: one route, several fleet speeds).
        pool = sorted(set(vals))
        cand = []
        for p in pv:
            near = min(pool, key=lambda x: abs(x - p))
            t = max(a, r * abs(near))
            cand.append((abs(p - near) - t, p - near, t))
        _, worst, t = max(cand)
        res["tolerance"] = round(t, 3)
        res["diff"] = round(worst, 3)
        res["status"] = "PASS" if abs(worst) <= t + 1e-9 else "FAIL"
        res["detail"] = "port %s, retail %s (%s), worst %+g from the nearest retail value, tol %g" % (
            ", ".join("%g" % p for p in pv), ", ".join("%g" % x for x in pool), nr, worst, t)
        return res
    t = max(a, r * max(abs(lo), abs(hi)))
    plo, phi = min(pv), max(pv)  # a port value can be a window too (a trace with missed samples)
    diff = 0.0 if plo <= hi and phi >= lo else (plo - hi if plo > hi else phi - lo)
    res["tolerance"] = round(t, 3)
    res["diff"] = round(diff, 3)
    res["status"] = "PASS" if abs(diff) <= t + 1e-9 else "FAIL"
    rng = ("%g" % lo) if lo == hi else "%g..%g" % (lo, hi)
    ptxt = ("%g" % plo) if plo == phi else "%g..%g" % (plo, phi)
    res["detail"] = "port %s, retail %s (%s), diff %+g, tol %g" % (ptxt, rng, nr, diff, t)
    return res


def compare(scen, tols, retail_runs, port, gaps=None):
    """retail_runs: [(name, metrics)], port: metrics or None. Returns graded results sorted by category and id."""
    gaps = gaps or {}
    ids = set()
    for _, mm in retail_runs:
        ids |= set(mm)
    ids |= set(port or {})
    results = []
    for mid in sorted(ids):
        fam = family(mid)
        tol = tolerance_for(tols, mid)
        rv = [mm.get(mid) for _, mm in retail_runs]
        pv = (port or {}).get(mid)
        # A route / type seen on one side only is an AI-choice difference, already graded by the mix metrics.
        if fam in ("travel.hop_ticks", "build.ticks") and (pv is None or not any(v is not None for v in rv)):
            continue
        note = gaps.get(mid) or gaps.get(fam)
        results.append(grade(mid, rv, pv, tol, port_note=note, retail_note=note))
    order = {"timing": 0, "income": 1, "AI choices": 2, "battle outcomes": 3, "damage": 4, "diagnostic": 5}
    results.sort(key=lambda x: (order.get(x["category"], 9), x["metric"]))
    return results


def summarize(results):
    s = collections.OrderedDict()
    for r in results:
        c = s.setdefault(r["category"], collections.Counter())
        c[r["status"]] += 1
    return {k: dict(v) for k, v in s.items()}


def render(scen_id, scen, results, info, refs_used, commit):
    lines = ["Q4 parity harness: %s (%s)" % (scen_id, scen.get("title", "")),
             "  port commit: %s" % (commit or "?"),
             "  reference runs: %s" % ", ".join(refs_used),
             "  port: %s" % info.get("port_cmd", "?"), ""]
    for n in info.get("notes", []):
        lines.append("  NOTE " + n)
    w = max([len(r["metric"]) for r in results] + [10])
    cat = None
    for r in results:
        if r["category"] != cat:
            cat = r["category"]
            lines.append("")
            lines.append("== " + cat)
        lines.append("%-*s  %-5s %s" % (w, r["metric"], r["status"], r["detail"]))
    lines.append("")
    summ = summarize(results)
    lines.append("summary: " + "; ".join("%s %s" % (k, ", ".join("%s %d" % kv for kv in sorted(v.items()))) for k, v in summ.items()))
    tot = collections.Counter(r["status"] for r in results)
    lines.append("total: %s" % ", ".join("%s %d" % kv for kv in sorted(tot.items())))
    return "\n".join(lines) + "\n"


# ---- scenarios, port runs -----------------------------------------------------------------------------------------------
def load_scenarios():
    return load_json(os.path.join(HERE, "scenarios.json"))


def resolve_refs(scen, refs_dir):
    """[(run name, run dir)] of the scenario's reference runs present under refs_dir, plus the missing ones."""
    have, missing = [], []
    for r in scen["references"]:
        d = os.path.join(refs_dir, r["run"])
        if os.path.exists(os.path.join(d, "trace.q2t")):
            have.append((r["run"], d))
        else:
            missing.append(r["run"])
    return have, missing


def retail_metrics(scen, run_dir):
    trace = os.path.join(run_dir, "trace.q2t")
    if scen["kind"] == "galactic":
        return galactic_metrics(trace, os.path.join(run_dir, "production.q2q"))
    return skirmish_retail_metrics(trace, set(scen["frames"]))


def port_command(scen, exe, out_dir, ref_info):
    if scen["kind"] == "galactic":
        campaign = ref_info["campaign"]
        human = campaign.rsplit("_", 1)[-1]
        seed = int(ref_info.get("seed") or "0", 16)
        return [exe, "--economy", "--galactic-ai", "--campaign", campaign, "--human", human, "--seed", str(seed),
                "--ticks", str(ref_info["ticks"]), "--workers", str(scen.get("workers", 4)), "--quiet", "--story",
                "--crc-out", os.path.join(out_dir, "port.crc"), "--ai-log", os.path.join(out_dir, "port_ai.txt"),
                "--trace-out", os.path.join(out_dir, "port.q2t")]
    # The host logs its state on battle frame 0 and every 600 frames, frame f at host tick f + 1: one tick more than the
    # scenario so the last frame's state is written.
    return [exe, "--skirmish-ai", "skirmish", "--map", scen["map"], "--human", scen["human"], "--ticks", str(scen["ticks"] + 1),
            "--workers", str(scen.get("workers", 4)), "--quiet", "--ai-log", os.path.join(out_dir, "port_ai.txt"),
            "--crc-out", os.path.join(out_dir, "port.crc")]


def run_port(scen, exe, out_dir, ref_info, reuse):
    os.makedirs(out_dir, exist_ok=True)
    cmd = port_command(scen, exe, out_dir, ref_info)
    log = os.path.join(out_dir, "port_run.txt")
    if not (reuse and os.path.exists(log) and os.path.exists(os.path.join(out_dir, "port_ai.txt"))):
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        with open(log, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
        if r.returncode != 0:
            raise RuntimeError("port run failed (exit %d), see %s" % (r.returncode, log))
    with open(log, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    if scen["kind"] == "galactic":
        m, notes, info = galactic_metrics(os.path.join(out_dir, "port.q2t"), ai_log=os.path.join(out_dir, "port_ai.txt"))
    else:
        m, notes, info = skirmish_port_metrics(os.path.join(out_dir, "port_ai.txt"), set(scen["frames"]), scen["ticks"] + 1, text)
    info["port_cmd"] = " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd))
    return m, notes, info


def git_head():
    try:
        return subprocess.run(["git", "-C", REPO, "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True,
                              check=False).stdout.strip() or None
    except OSError:
        return None


def check_baseline(baseline, scen_id, results):
    """Metrics that passed in the baseline and fail now (a regression); PASS -> N/A is listed, not failed."""
    base = (baseline.get("scenarios") or {}).get(scen_id, {}).get("status", {})
    now = {r["metric"]: r["status"] for r in results}
    regress = sorted(k for k, v in base.items() if v == "PASS" and now.get(k) == "FAIL")
    gone = sorted(k for k, v in base.items() if v == "PASS" and now.get(k) in (None, "N/A"))
    improved = sorted(k for k, v in now.items() if v == "PASS" and base.get(k) == "FAIL")
    return regress, gone, improved


def cmd_run(a):
    scens = load_scenarios()
    refs_dir = a.refs or default_refs()
    ids = a.scenario or list(scens["scenarios"])
    exe = a.port_exe or default_port_exe()
    commit = a.commit or git_head()
    baseline = load_json(a.check_baseline) if a.check_baseline else None
    new_base = {"commit": commit, "scenarios": {}}
    if a.write_baseline and os.path.exists(a.write_baseline):
        new_base = load_json(a.write_baseline)
        new_base["commit"] = commit
        new_base.setdefault("scenarios", {})
    rc, ran = 0, 0
    for sid in ids:
        scen = scens["scenarios"].get(sid)
        if scen is None:
            print("unknown scenario %s (have %s)" % (sid, ", ".join(scens["scenarios"])))
            return 2
        have, missing = resolve_refs(scen, refs_dir)
        if not have:
            print("SKIP %s: no reference run under %s (missing %s; kit: %s)" % (sid, refs_dir, ", ".join(missing), scen["kit"]))
            continue
        if not os.environ.get("ALAMO_FOC_DATA"):
            print("SKIP %s: ALAMO_FOC_DATA not set (the port run needs the game data)" % sid)
            continue
        if not exe or not os.path.exists(exe):
            print("SKIP %s: no alamo_headless.exe (give --port-exe)" % sid)
            continue
        print("%s: %d reference run(s) from %s%s" % (sid, len(have), refs_dir,
                                                       "; missing %s" % ", ".join(missing) if missing else ""))
        retail = []
        notes = []
        ref_info = None
        for name, d in have:
            mm, nn, info = retail_metrics(scen, d)
            retail.append((name, mm))
            notes += ["%s: %s" % (name, n) for n in nn]
            ref_info = ref_info or info
        out_dir = os.path.join(a.out or os.path.join(HERE, "out"), sid)
        try:
            pm, pn, pinfo = run_port(scen, exe, out_dir, ref_info, a.reuse_port)
        except RuntimeError as e:
            print("ERROR %s: %s" % (sid, e))
            return 2
        notes += ["port: %s" % n for n in pn]
        results = compare(scen, scens["tolerances"], retail, pm, scen.get("gaps"))
        info = {"port_cmd": pinfo.get("port_cmd"), "notes": notes}
        text = render(sid, scen, results, info, [n for n, _ in have], commit)
        loo = leave_one_out(scen, scens["tolerances"], retail)
        if loo:
            text += ("retail leave-one-out (each retail run graded like the port against the other %d, same tolerances; the port "
                     "is graded against all %d, a wider envelope): %s\n" % (len(retail) - 1, len(retail), "; ".join(
                         "%s %s" % (k, ", ".join("%s %d" % kv for kv in sorted(v.items()))) for k, v in loo.items())))
        verdict = ""
        if baseline is not None:
            regress, gone, improved = check_baseline(baseline, sid, results)
            verdict = "baseline %s (commit %s): %d regressions%s; %d metrics passing in the baseline are N/A now%s; " \
                      "%d improved%s\n" % (
                          a.check_baseline, baseline.get("commit"), len(regress), " (%s)" % ", ".join(regress) if regress else "",
                          len(gone), " (%s)" % ", ".join(gone) if gone else "", len(improved),
                          " (%s): re-record with --write-baseline" % ", ".join(improved) if improved else "")
            if regress:
                rc = max(rc, 1)
        elif any(r["status"] == "FAIL" for r in results):
            rc = max(rc, 1)
        text += verdict
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "report.txt"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        write_json(os.path.join(out_dir, "report.json"), {"scenario": sid, "commit": commit, "references": [n for n, _ in have],
                                                          "missing_references": missing, "notes": notes, "results": results,
                                                          "summary": summarize(results)})
        print(text, end="")
        print("wrote %s" % os.path.join(out_dir, "report.txt"))
        new_base["scenarios"][sid] = {"references": [n for n, _ in have], "summary": summarize(results),
                                      "status": {r["metric"]: r["status"] for r in results}}
        ran += 1
    if ran == 0:
        return SKIP
    if a.write_baseline:
        write_json(a.write_baseline, new_base)
        print("wrote baseline %s" % a.write_baseline)
    return rc


def leave_one_out(scen, tols, runs):
    """Per category status counts of every retail run graded as if it were the port, against the other runs."""
    if len(runs) < 2:
        return None
    out = collections.OrderedDict()
    for i, (_, mm) in enumerate(runs):
        for r in compare(scen, tols, [x for j, x in enumerate(runs) if j != i], mm, scen.get("gaps")):
            out.setdefault(r["category"], collections.Counter())[r["status"]] += 1
    return out


def cmd_retail(a):
    scens = load_scenarios()
    refs_dir = a.refs or default_refs()
    for sid in a.scenario or list(scens["scenarios"]):
        scen = scens["scenarios"][sid]
        have, missing = resolve_refs(scen, refs_dir)
        print("== %s: %d run(s)%s" % (sid, len(have), "; missing %s" % ", ".join(missing) if missing else ""))
        runs = [(n, retail_metrics(scen, d)[0]) for n, d in have]
        if len(runs) < 2:
            print("  one run only: no spread")
            continue
        ids = sorted(set().union(*[set(m) for _, m in runs]))
        for mid in ids:
            vals = [m.get(mid) for _, m in runs]
            if any(isinstance(v, collections.Counter) for v in vals):
                vv = [v for v in vals if v is not None]
                pair = [overlap(vv[i], vv[j]) for i in range(len(vv)) for j in range(i + 1, len(vv))]
                print("  %-44s overlap %s" % (mid, " ".join("%.2f" % x for x in pair)))
            else:
                print("  %-44s %s" % (mid, " | ".join("-" if v is None else str(v) for v in vals)))
        # Leave one out: each retail run graded as if it were the port, against the other runs. A tolerance that fails
        # retail against itself is too tight (it would flag the original game's own run-to-run spread).
        tot = collections.Counter()
        for i, (name, mm) in enumerate(runs):
            others = [r for j, r in enumerate(runs) if j != i]
            res = compare(scen, scens["tolerances"], others, mm, scen.get("gaps"))
            c = collections.Counter(r["status"] for r in res)
            tot.update(c)
            print("  leave-one-out %s: %s%s" % (name, ", ".join("%s %d" % kv for kv in sorted(c.items())),
                                               "".join("\n    FAIL %s: %s" % (r["metric"], r["detail"]) for r in res
                                                       if r["status"] == "FAIL")))
        print("  leave-one-out total: %s" % ", ".join("%s %d" % kv for kv in sorted(tot.items())))
    return 0


def cmd_list(a):
    scens = load_scenarios()
    refs_dir = a.refs or default_refs()
    print("reference folder: %s" % refs_dir)
    for sid, scen in scens["scenarios"].items():
        have, missing = resolve_refs(scen, refs_dir)
        print("%s (%s): %s" % (sid, scen["kind"], scen.get("title", "")))
        for r in scen["references"]:
            print("  %-42s %s  %s" % (r["run"], "present" if r["run"] in [n for n, _ in have] else "MISSING", r.get("note", "")))
        print("  kit: %s" % scen["kit"])
    return 0


# ---- selftest -------------------------------------------------------------------------------------------------------------
def _f(x):
    import struct
    return "%08x" % struct.unpack("<I", struct.pack("<f", float(x)))[0]


def _galactic_fixture(path, hop_len=26, income=667.5, new_obj_at=40, capture_at=45, queue_path=None, start=20):
    """A tiny HotM-like trace: planets KUAT (Empire, x=0) and CORELLIA (Rebel, x=100), an Empire fleet hopping KUAT ->
    CORELLIA (departs tick 10, parks after hop_len ticks), a fiscal cycle at tick 30, a Rebel X-wing built at new_obj_at
    and CORELLIA captured by the Empire at capture_at."""
    types = {1: "KUAT", 2: "CORELLIA", 3: "STAR_DESTROYER", 4: "REBEL_X-WING_SQUADRON", 5: "GALACTIC_FLEET"}
    L = ["Q2TRACE 1", "meta scenario galactic", "meta campaign Sandbox_Fixture_Empire", "seed 00000000 00000000 0",
         "end_header", "start 0 0 0 00000000"]
    cred = {0: 1000.0, 1: 5000.0}
    planet_owner = {1: 1, 2: 0}
    fleet_x = 0.0
    for t in range(0, 60):
        cyc = 1 if t >= 30 else 0
        L.append("F %d 0 %d 00000000 0 1 0 %d %d %d" % (t, t, 0 if cyc == 0 else 30, 30 if cyc == 0 else 60, cyc))
        if t == 0:
            L.append("K %d" % t)
        if 10 <= t < 10 + hop_len:
            fleet_x = 100.0 * (t - 9) / hop_len
        elif t >= 10 + hop_len:
            fleet_x = 100.0
        if t == capture_at:
            planet_owner[2] = 1
        objs = [(1, planet_owner[1], 1, 0.0), (2, planet_owner[2], 2, 100.0), (10, 1, 3, 0.0), (11, 0, 4, 100.0),
                (20, 1, 5, fleet_x)]
        if t >= new_obj_at:
            objs.append((12, 0, 4, 100.0))
        for oid, own, ty, x in objs:
            L.append("O %d %d %d %s %s 00000000 00000000 00000000 00000000 %s 1 4194303 %d 4194303 00000000 00000000" % (
                oid, own, ty, _f(x), _f(0.0), _f(100.0), 2 if ty == 4 else 4194303))
        if t == 0:
            L += ["G 1 00000000 0 0 0 0 0 0", "G 2 00000000 0 0 0 0 0 0"]
        if t == 30:
            cred[1] += income
            cred[0] += 100.0
        for i, v in cred.items():
            L.append("C %d %s 00000000 %d %d" % (i, _f(v), 1 if i == 1 else 0, 0 if i == 1 else 1))
    L += ["T %d 0 0 %s" % (k, v) for k, v in types.items()] + ["E frames_reached", "end"]
    with open(path, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")
    if queue_path:
        Q = ["Q2QUEUES 1"]
        for t in range(start, new_obj_at + 1):
            created = 12 if t >= new_obj_at else 4194303
            Q.append("Q %d %d 2 1 1 4:0:%d:%d:%s:%s:%s:0:2:%d" % (t, t, start, new_obj_at - start, _f(1.0), _f(0.0),
                                                                 _f(300.0), created))
        Q += ["E frames_reached", "end"]
        with open(queue_path, "w", encoding="ascii", newline="\n") as fh:
            fh.write("\n".join(Q) + "\n")


def selftest():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL " + what)
    # Grading primitives.
    t = {"abs": 2}
    check(grade("travel.hop_ticks[A->B]", [26, 27], 28, t)["status"] == "PASS", "envelope: 28 within 26..27 +-2")
    tn = {"abs": 2, "nearest": True}
    check(grade("travel.hop_ticks[A->B]", [(89, 107), (107,)], (106, 90), tn)["status"] == "PASS", "nearest: 106, 90 near 107, 89")
    check(grade("travel.hop_ticks[A->B]", [(89, 107), (107,)], (98,), tn)["status"] == "FAIL", "nearest: 98 between modes fails")
    check(grade("econ.cycle_tick[1]", [1351], (1350, 1353), {"abs": 0})["status"] == "PASS", "port window overlapping")
    check(grade("travel.hop_ticks[A->B]", [26, 27], 30, t)["status"] == "FAIL", "envelope: 30 outside 26..27 +-2")
    check(grade("econ.income_mean[Empire]", [1000.0], 1015.0, {"rel": 0.02})["status"] == "PASS", "rel tolerance 2%")
    check(grade("econ.income_mean[Empire]", [1000.0], 1030.0, {"rel": 0.02})["status"] == "FAIL", "rel tolerance 2% fail")
    check(grade("econ.cycle_tick[2]", [(2699, 2701), 2701], 2700, {"abs": 0})["status"] == "PASS", "window envelope")
    check(grade("diag.rng_draws_total", [10], 99, {"info": True})["status"] == "INFO", "info metric not graded")
    check(grade("combat.damage_taken[Rebel]", [10.0], None, {"abs": 1}, port_note="no hook")["status"] == "N/A", "N/A port")
    C = collections.Counter
    check(abs(overlap(C(a=2, b=1), C(a=1, b=1, c=1)) - 2 / 3) < 1e-9, "overlap value")
    check(overlap(C(), C()) == 1.0, "overlap of empties")
    g = grade("ai.build_mix[Rebel]", [C(a=2, b=2), C(a=4)], C(a=3, b=1), {"min_overlap": 0.7})
    check(g["status"] == "PASS" and g["score"] == 0.75, "mix: best retail overlap 0.75")
    g = grade("ai.build_mix[Rebel]", [C(a=2, b=2), C(a=4)], C(b=3, c=1), {"min_overlap": 0.7, "retail_floor": True})
    check(g["status"] == "PASS" and g["tolerance"]["min_overlap"] == 0.5, "mix: threshold capped at the retail pair overlap 0.5")
    g = grade("ai.build_mix[Rebel]", [C(a=2, b=2), C(a=4)], C(c=4), {"min_overlap": 0.7, "retail_floor": True})
    check(g["status"] == "FAIL", "mix: no overlap fails")
    check(tolerance_for({"ai.build_mix": {"min_overlap": 0.5}, "ai": {"abs": 1}}, "ai.build_mix[Rebel]")["min_overlap"] == 0.5,
          "tolerance by family")
    check(tolerance_for({"econ.credits_at": {"rel": 0.1}}, "econ.credits_at_4500[Rebel]")["rel"] == 0.1, "tolerance by prefix")
    tmp = tempfile.mkdtemp(prefix="parity_selftest_")
    try:
        # Galactic: the same fixture on both sides, then a slower hop, a smaller income and a late capture on the port side.
        ret = os.path.join(tmp, "retail.q2t")
        _galactic_fixture(ret, queue_path=os.path.join(tmp, "retail.q2q"))
        rm, _, rinfo = galactic_metrics(ret, os.path.join(tmp, "retail.q2q"), sample_ticks=(59,))
        check(rm.get("travel.hop_ticks[Empire:KUAT->CORELLIA]") == (26,), "fixture hop 26 ticks (%s)" % rm.get("travel.hop_ticks[Empire:KUAT->CORELLIA]"))
        check(abs(rm.get("econ.income_cycle1[Empire]", 0) - 667.5) < 1e-3, "fixture Empire income 667.5")
        check(rm.get("econ.cycle_tick[1]") == 30, "fixture cycle at tick 30 (%s)" % rm.get("econ.cycle_tick[1]"))
        check(rm.get("build.ticks[REBEL_X-WING_SQUADRON@CORELLIA]") == 20, "fixture build: start 20, created 40 -> 20 ticks (%s)" % rm.get("build.ticks[REBEL_X-WING_SQUADRON@CORELLIA]"))
        check(rm.get("outcome.planets_final[Empire]") == 2 and rm.get("outcome.captures[Empire]") == 1, "fixture capture")
        check(rm.get("ai.fleet_departures[Empire]") == 1, "fixture one Empire departure")
        check(rinfo["factions"].get(0) == "Rebel" and rinfo["factions"].get(1) == "Empire", "fixture factions")
        port = os.path.join(tmp, "port.q2t")
        _galactic_fixture(port, hop_len=31, income=600.0, capture_at=50)
        ai = os.path.join(tmp, "port_ai.txt")
        with open(ai, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# galactic-ai campaign Sandbox_Fixture_Empire seed 0 ticks 60 human Empire\n"
                     "0 p0 templates Basic_Rebel_Default\n18 p0 order build REBEL_X-WING_SQUADRON at Corellia\n")
        pm, _, _ = galactic_metrics(port, ai_log=ai, sample_ticks=(59,))
        check(pm.get("build.ticks[REBEL_X-WING_SQUADRON@CORELLIA]") == 22, "port build: order 18 -> completion 40 (%s)" % pm.get("build.ticks[REBEL_X-WING_SQUADRON@CORELLIA]"))
        check(pm.get("ai.build_count[Rebel]") == 1, "port build orders counted")
        tols = {"travel.hop_ticks": {"abs": 2}, "build.ticks": {"abs": 3}, "econ.income_cycle1": {"rel": 0.02},
                "econ.income_mean": {"rel": 0.02}, "econ.cycle_tick": {"abs": 0}, "econ.credits_at": {"rel": 0.1},
                "ai.build_mix": {"min_overlap": 0.6}, "ai": {"rel": 0.25, "abs": 2}, "ai.fleet_targets": {"min_overlap": 0.5},
                "ai.first_departure_tick": {"abs": 30}, "ai.first_build_tick": {"abs": 30}, "outcome": {"abs": 0},
                "diag": {"info": True}}
        res = {r["metric"]: r for r in compare({}, tols, [("retail", rm)], pm)}
        check(res["travel.hop_ticks[Empire:KUAT->CORELLIA]"]["status"] == "FAIL", "slower port hop fails")
        check(res["econ.income_cycle1[Empire]"]["status"] == "FAIL", "smaller port income fails")
        check(res["build.ticks[REBEL_X-WING_SQUADRON@CORELLIA]"]["status"] == "PASS", "build 22 vs 20 within 3")
        check(res["outcome.planets_final[Empire]"]["status"] == "PASS", "same final owners pass")
        check(res["econ.cycle_tick[1]"]["status"] == "PASS", "same cycle tick passes")
        same = {r["metric"]: r for r in compare({}, tols, [("retail", rm)], rm)}
        check(all(r["status"] in ("PASS", "INFO") for r in same.values()), "a run against itself passes everything: %s" % [
            k for k, r in same.items() if r["status"] not in ("PASS", "INFO")])
        # Baseline check: PASS -> FAIL is a regression, FAIL -> PASS an improvement.
        base = {"scenarios": {"x": {"status": {"travel.hop_ticks[Empire:KUAT->CORELLIA]": "PASS", "econ.income_cycle1[Empire]": "FAIL",
                                                "outcome.planets_final[Empire]": "PASS", "travel.hop_ticks[Empire:KUAT->NABOO]": "PASS"}}}}
        reg, gone, imp = check_baseline(base, "x", list(res.values()))
        check(reg == ["travel.hop_ticks[Empire:KUAT->CORELLIA]"], "baseline regression found (%s)" % reg)
        check(gone == ["travel.hop_ticks[Empire:KUAT->NABOO]"], "baseline PASS -> missing listed (%s)" % gone)
        reg2, _, imp2 = check_baseline(base, "x", list(same.values()))
        check(not reg2 and imp2 == ["econ.income_cycle1[Empire]"], "baseline improvement found")
        # Skirmish port log parser.
        sk = os.path.join(tmp, "sk_ai.txt")
        with open(sk, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("state t=1 battle frame 0: p0 credits 6000.0 objects 17; p1 credits 6000.0 objects 13;\n"
                     "  pads: 28:Mineral_Extractor_Pad/p2\n"
                     "  units: p0{Skirmish_Empire_Star_Base_1x1 TIE_Interceptorx14 Teamx2 } p1{Skirmish_Rebel_Star_Base_1x1 X-Wingx10 }\n"
                     "state t=1801 battle frame 1800: p0 credits 6300.0 objects 17; p1 credits 900.0 objects 20;\n"
                     "  pads: 28:Mineral_Extractor_Pad/p1=Rebel_Mineral_Extractor\n"
                     "  units: p0{Skirmish_Empire_Star_Base_1x1 TIE_Interceptorx12 } p1{Skirmish_Rebel_Star_Base_1x1 X-Wingx15 Y-Wingx3 }\n")
        sm, _, sinfo = skirmish_port_metrics(sk, {1800}, 1800, "skirmish-ai skirmish workers 4: 1800 ticks (1 ms)")
        check(sinfo["factions"] == {0: "Empire", 1: "Rebel"}, "skirmish slot factions (%s)" % sinfo["factions"])
        check(sm.get("ai.build_count[Rebel]") == 9, "skirmish Rebel built 5 X-wings, 3 Y-wings, 1 extractor (%s)" % sm.get("ai.build_count[Rebel]"))
        check(sm.get("outcome.units_lost[Empire]") == 2, "skirmish Empire lost 2 interceptors")
        check(sm.get("econ.credits_at_1800[Rebel]") == 900.0, "skirmish credits at 1800")
        check(sm.get("outcome.battle_ended") == 0, "skirmish battle still running")
        # The scenario file parses and names a tolerance with a reason for every graded family it uses.
        sc = load_scenarios()
        for k, v in sc["tolerances"].items():
            check(bool(v.get("why")), "tolerance %s has a reason" % k)
        for sid, s in sc["scenarios"].items():
            check(bool(s.get("references")) and bool(s.get("kit")), "scenario %s has references and a kit" % sid)
        # No reference runs -> skip code.
        empty = os.path.join(tmp, "norefs")
        os.makedirs(empty)
        ns = argparse.Namespace(refs=empty, scenario=None, port_exe=None, commit=None, check_baseline=None,
                                write_baseline=None, out=os.path.join(tmp, "out"), reuse_port=False)
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            rc = cmd_run(ns)
        check(rc == SKIP, "no reference runs -> exit %d (got %s)" % (SKIP, rc))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("selftest %s (%d failures)" % ("OK" if not fails else "FAILED", len(fails)))
    return 0 if not fails else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--scenario", action="append", help="scenario id (repeatable; default: all)")
    r.add_argument("--port-exe", help="alamo_headless.exe (default: build-wt-msvc / build-msvc / ... bin)")
    r.add_argument("--refs", help="folder holding the Q2 run folders (default: the Q2 kit's out\\ folder, main checkout)")
    r.add_argument("--out", help="output folder (default tools/parity/out)")
    r.add_argument("--reuse-port", action="store_true", help="reuse an earlier port run in the output folder")
    r.add_argument("--check-baseline", help="fail only on metrics that passed in this baseline JSON")
    r.add_argument("--write-baseline", help="write the per-metric verdicts of this run to this JSON")
    r.add_argument("--commit", help="commit id to record (default: git HEAD)")
    t = sub.add_parser("retail")
    t.add_argument("--scenario", action="append")
    t.add_argument("--refs")
    li = sub.add_parser("list")
    li.add_argument("--refs")
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    if a.cmd == "run":
        return cmd_run(a)
    if a.cmd == "retail":
        return cmd_retail(a)
    if a.cmd == "list":
        return cmd_list(a)
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
