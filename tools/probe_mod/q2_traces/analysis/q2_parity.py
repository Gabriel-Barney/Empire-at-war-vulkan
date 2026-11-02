"""Q2 parity: per-metric comparison of a Q2 reference trace (original game) with a port run of the same scenario.

Standard library only. Used by `q2trace.py compare` (tools/probe_mod/q2_traces/q2trace.py); can also run alone:

    toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\q2_traces\\analysis\\q2_parity.py
        <trace.q2t | trace run dir> --port-crc <port.crc> [--port-ai-log <ai.txt>] [--port-trace <port.q2t>]
        [--lua <lua_*.txt>] [--port-factions 0=Rebel,1=Empire,5=Underworld] [--json out.json]

Both sides are reduced to the same metric set, keyed by **tick = mode frame** (the number of ticks of the mode done;
trace `F` field 4, the port's galactic frame after that many `tick_once()` calls; port CRC record t = tick t + 1):

  rng_draws       synced-RNG draws per tick. Trace: LCG steps between the states sampled at consecutive mode frames
                  (draws on global frames where the mode frame did not advance, i.e. galactic pauses, are reported
                  apart). Port: recovered from the `rng` system digest of the CRC log (StateHasher(u32 state, u64 draws),
                  stepped forward from the seed), so no engine change is needed. Tick 1 includes the galaxy build.
  fiscal_cycles   ticks where the fiscal-cycle counter increments (trace F fields). Port: only with --port-trace.
  owners          initial planet owners and every owner change (trace O owner of planet objects; port AI log
                  `<frame> owner <Planet> a -> b`); planets owned per faction every 1800 ticks (port `# t=` lines).
  fleet moves     trace: every GALACTIC_FLEET hop (depart tick, from, to, arrive tick) from positions; port: the AI's
                  `order move fleet <id> -> <Planet>` (the order tick, which is the departure tick).
  production      trace: objects that appear after the start (completion, by type and owner; fleets/particles left
                  out); port: the AI's `order build <Type> at <Planet>` (order tick). Compared by type sequence per
                  faction (tick offsets = build time, reported but not a divergence by themselves).
  object counts   per (faction, type) every 1800 ticks. Trace only unless --port-trace.
  credits         trace C lines (reader v2) and Lua `PLAYER <f> credits <x>` per cycle; port: final credits from the
                  AI-log summary (and per tick with --port-trace).
  perception      Lua `PERCEPTION <ai> <planet> <eq>=<v>` (trace only; the port has no output for it yet).

Factions name players on both sides (player indices differ): trace players by the unit types they own at the start,
port players by the AI template names and the `human` field of the AI log; override with --port-factions /
--trace-factions.
"""
import argparse
import collections
import json
import math
import os
import re
import struct
import sys

LCG_MUL, LCG_ADD = 0x41C64E6D, 0xBDF
MASK64 = (1 << 64) - 1
P1, P2, P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x27D4EB2F165667C5
SAMPLE_EVERY = 1800  # ticks between planets-owned / object-count samples (the port's "# t=" cadence, 60 s)
ORBIT_SNAP = 1e-3    # position change below this is "not moving"
NOT_PRODUCTION = ("GALACTIC_FLEET", "PARTICLES", "ART_MODEL", "GALACTIC_SQUADRON", "EXPLOSION")

# Unit-name tokens -> faction, to name the trace's player indices (the trace has no faction names).
FACTION_TOKENS = {
    "Empire": ("STORMTROOPER", "IMPERIAL", "TIE_", "E_GROUND", "EMPIRE_", "STAR_DESTROYER", "AT_ST", "AT_AT", "VEERS",
               "TARTAN", "VICTORY_DESTROYER", "ACCUSER", "SLAVE_I", "BOBA_FETT"),
    "Rebel": ("REBEL", "R_GROUND", "X-WING", "XWING", "Y-WING", "ALLIANCE", "CORELLIAN_", "MARAUDER", "T2B", "HAN_SOLO",
              "CHEWBACCA", "OBI_WAN", "MILLENNIUM", "GALLOFREE", "PLEX"),
    "Underworld": ("UNDERWORLD", "U_GROUND", "STARVIPER", "SKIPRAY", "VENGEANCE", "KEDALBE", "INTERCEPTOR4", "CRUSADER",
                   "DESTROYER_DROID", "MERC"),
}


# ---- hashing (same as q2trace.py / engine/core StateHasher) -------------------------------------------------
def _round(h, w):
    h = (h + w * P2) & MASK64
    h = ((h << 31) | (h >> 33)) & MASK64
    return (h * P1) & MASK64


def _final(h, n):
    h ^= n
    h ^= h >> 33
    h = (h * 0xFF51AFD7ED558CCD) & MASK64
    h ^= h >> 33
    h = (h * 0xC4CEB9FE1A85EC53) & MASK64
    h ^= h >> 33
    return h


def rng_digest(state, draws):
    """StateHasher digest of SimRng::hash (u32 state, u64 draw count): engine/core sim_rng.cpp."""
    b = struct.pack("<IQ", state & 0xFFFFFFFF, draws & MASK64)
    h = _round(P5, int.from_bytes(b[:8], "little"))
    h = _round(h, int.from_bytes(b[8:].ljust(8, b"\0"), "little"))
    return _final(h, 12)


def lcg(s):
    return (s * LCG_MUL + LCG_ADD) & 0xFFFFFFFF


def lcg_steps(a, b, limit):
    s = a
    for i in range(limit + 1):
        if s == b:
            return i
        s = lcg(s)
    return None


def f32(h):
    return struct.unpack("<f", struct.pack("<I", int(h, 16)))[0]


def faction_of_types(names):
    score = collections.Counter()
    for n in names:
        for fac, toks in FACTION_TOKENS.items():
            if any(t in n for t in toks):
                score[fac] += 1
    return score.most_common(1)[0][0] if score else None


def parse_map(text):
    out = {}
    for part in (text or "").split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[int(k)] = v.strip()
    return out


def empty_metrics(source):
    return {"source": source, "scenario": {}, "factions": {}, "rng_draws": None, "rng_pause_draws": None,
            "fiscal_cycles": None, "owners_initial": None, "owner_changes": None, "planets_owned": None,
            "fleet_moves": None, "fleet_hops": None, "production": None, "object_counts": None, "credits": None,
            "credits_final": None, "perception": None, "lua_owners": None, "conflicts": None, "notes": []}


# ---- trace side ------------------------------------------------------------------------------------------------
def trace_metrics(path, factions_override=None, max_tick=None):
    """Metrics of a Q2TRACE 1 file (the original game, or a port run written in the same format)."""
    m = empty_metrics(os.path.abspath(path))
    types, meta, header = {}, {}, {}
    with open(path, "r", encoding="ascii") as fh:
        for line in fh:
            if line.startswith("T "):
                p = line.split()
                types[int(p[1])] = p[4]
    objs = {}          # id -> [owner, type_name, x, y]
    seen = set()
    planets = set()
    players = {}       # index -> credits
    draws, pause_draws = {}, []
    cycles, cycle_info, cycle_windows, cycle_prev_m = [], None, [], 0
    prev_rng, prev_m = None, None
    last_cycle = None
    fleet_state = {}   # id -> (parked_pos, moving_since, from_planet)
    hops = []
    production = []
    owner_changes = []
    owners_now = {}
    owners_initial = None
    planets_owned, object_counts, credits = {}, {}, {}
    first_obj_tick = None
    new_objs = []
    planet_names, credit_samples, player_flags = {}, {}, {}
    mframe = 0
    in_header = True
    pending_sample = None

    def planet_pos():
        return {objs[i][1]: (objs[i][2], objs[i][3]) for i in planets if i in objs}

    def nearest(x, y, pp):
        best = min(((math.hypot(x - a, y - b), n) for n, (a, b) in pp.items()), default=(None, None))
        return best[1]

    def faction(idx):
        return m["factions"].get(idx, "p%d" % idx)

    def planet_of(i):
        for _ in range(6):  # object -> squadron -> fleet -> planet
            o = objs.get(i)
            if o is None:
                return None
            if i in planets:
                return o[1]
            i = o[4]
        return None

    def end_of_frame(tick):
        nonlocal owners_initial
        if not objs:
            return
        # New objects of this tick, grouped into one production batch per (owner, planet).
        batch = {}
        for tk, i in new_objs:
            o = objs.get(i)
            if o is not None:
                batch.setdefault((o[0], planet_of(i)), collections.Counter())[o[1]] += 1
        for (own, at), cnt in sorted(batch.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))):
            production.append((tick, own, at, dict(sorted(cnt.items()))))
        new_objs.clear()
        pp = planet_pos()
        own = {objs[i][1]: objs[i][0] for i in planets if i in objs}
        if owners_initial is None:
            owners_initial = dict(own)
            owners_now.update(own)
        for name, o in sorted(own.items()):
            if owners_now.get(name) != o:
                owner_changes.append((tick, name, owners_now.get(name), o))
                owners_now[name] = o
        # Fleet hops: parked -> moving -> parked somewhere else.
        live = set()
        for i, o in objs.items():
            if o[1] != "GALACTIC_FLEET":
                continue
            live.add(i)
            st = fleet_state.get(i)
            pos = (o[2], o[3])
            if st is None:
                fleet_state[i] = [pos, None, nearest(pos[0], pos[1], pp), None]
                continue
            moved = math.hypot(pos[0] - st[0][0], pos[1] - st[0][1]) > ORBIT_SNAP
            if moved and st[1] is None:
                st[1] = tick  # first tick of the hop (departure)
            elif not moved and st[1] is not None:
                to = nearest(pos[0], pos[1], pp)
                if to != st[2]:
                    hops.append((st[1], o[0], st[2], to, tick, i))
                st[1] = None
                st[2] = to
            st[0] = pos
        for i in [k for k in fleet_state if k not in live]:
            del fleet_state[i]
        for i in planets:
            if i in objs:
                planet_names[i] = objs[i][1]
        if players and (not credit_samples or tick % SAMPLE_EVERY == 0) and tick not in credit_samples:
            credit_samples[tick] = dict(sorted(players.items()))
        if tick % SAMPLE_EVERY == 0 and tick not in planets_owned:
            cnt = collections.Counter(own.values())
            planets_owned[tick] = dict(cnt)
            oc = collections.Counter((o[0], o[1]) for o in objs.values())
            object_counts[tick] = dict(oc)

    with open(path, "r", encoding="ascii") as fh:
        if fh.readline().strip() != "Q2TRACE 1":
            raise ValueError("%s: not a Q2TRACE 1 file" % path)
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
                else:
                    header[t] = p[1:]
                continue
            if t == "F":
                g, mf, rng = int(p[1]), int(p[3]), int(p[4], 16)
                if pending_sample is not None:
                    end_of_frame(pending_sample)
                if max_tick is not None and mf > max_tick:
                    pending_sample = None
                    break
                if prev_rng is not None:
                    n = lcg_steps(prev_rng, rng, 2_000_000)
                    if mf == prev_m:
                        if n:
                            pause_draws.append((g, mf, n))
                    else:
                        draws[(prev_m + 1, mf)] = n
                prev_rng, prev_m = rng, mf
                mframe = mf
                if len(p) >= 11:
                    cyc = int(p[10])
                    if cycle_info is None:
                        cycle_info = {"first_last_cycle_frame": int(p[8]), "first_next_cycle_frame": int(p[9]),
                                      "first_cycle": cyc}
                    if last_cycle is not None and cyc != last_cycle:
                        cycles.append(mf)
                        cycle_windows.append((cycle_prev_m + 1, mf))
                        if players:
                            credits[mf] = {k: v for k, v in sorted(players.items())}
                    last_cycle = cyc
                    cycle_prev_m = mf
                pending_sample = mf
            elif t == "K":
                pass  # keyframe lines overwrite the state
            elif t == "O":
                i = int(p[1])
                ent = [int(p[2]), types.get(int(p[3]), "?%s" % p[3]), f32(p[4]), f32(p[5]), int(p[13])]
                if int(p[11]) & 2:  # dead
                    objs.pop(i, None)
                    continue
                objs[i] = ent
                if i not in seen:
                    seen.add(i)
                    if first_obj_tick is None:
                        first_obj_tick = mframe
                    elif mframe > first_obj_tick and not any(x in ent[1] for x in NOT_PRODUCTION):
                        new_objs.append((mframe, i))
            elif t == "X":
                objs.pop(int(p[1]), None)
            elif t == "G":
                planets.add(int(p[1]))
            elif t == "C":
                players[int(p[1])] = f32(p[2])
                player_flags[int(p[1])] = (int(p[4]), int(p[5])) if len(p) >= 6 else (0, 0)
            elif t == "end":
                break
        if pending_sample is not None:
            end_of_frame(pending_sample)
    # Factions of the trace players (from what they own at the start).
    if factions_override:
        m["factions"] = dict(factions_override)
    else:
        by_owner = collections.defaultdict(list)
        # objects of the first sample with objects: rebuild cheaply from 'seen' is not possible, use the counts.
        first = min(object_counts) if object_counts else None
        if first is not None:
            for (own, name), n in object_counts[first].items():
                by_owner[own].extend([name] * n)
        for own, names in by_owner.items():
            f = faction_of_types(names)
            if f and f not in m["factions"].values():
                m["factions"][own] = f
    fac = faction
    m["scenario"] = {"campaign": meta.get("campaign") or meta.get("skirmish") or meta.get("label"),
                     "seed": header.get("seed", [None])[0], "ticks": mframe, "first_tick_with_objects": first_obj_tick,
                     "mod": meta.get("mod"), "lua_probe": meta.get("lua_probe"), "scenario": meta.get("scenario"),
                     "label": meta.get("label")}
    m["rng_draws"] = [(a, b, n) for (a, b), n in sorted(draws.items())]
    m["rng_pause_draws"] = pause_draws
    m["fiscal_cycles"] = {"cycle_ticks": cycles, "cycle_windows": cycle_windows, **cycle_info} if cycle_info else None
    if owners_initial is not None:
        m["owners_initial"] = {k: fac(v) for k, v in sorted(owners_initial.items())}
        m["owner_changes"] = [(tk, n, fac(a) if a is not None else None, fac(b)) for tk, n, a, b in owner_changes]
        m["planets_owned"] = {tk: {fac(k): v for k, v in sorted(d.items())} for tk, d in sorted(planets_owned.items())}
        m["fleet_hops"] = [(d, fac(o), a, b, arr) for d, o, a, b, arr, _ in hops]
        m["fleet_moves"] = [(d, fac(o), b) for d, o, a, b, arr, _ in hops]
    # C lines carry the PlayerList index. In galactic it equals the object owner id; in skirmish it does not (the human
    # is list index 0, its objects owner 3), so there the human / AI flags and the skirmish meta name the factions.
    cmap = {}
    sk = meta.get("skirmish") or ""
    hm, am = re.search(r"human (\w+)", sk), re.search(r"AI (\w+)", sk)
    if hm and am:
        ais = [k for k, (h, a) in player_flags.items() if a]
        for k, (h, a) in player_flags.items():
            if h:
                cmap[k] = hm.group(1)
        if len(ais) == 1:
            cmap[ais[0]] = am.group(1)

    def cfac(k):
        return cmap.get(k, "slot%d" % k) if cmap else fac(k)
    m["production"] = [(tk, fac(o), at, cnt) for tk, o, at, cnt in production]
    m["production_source"] = "new objects"
    m["object_counts"] = {tk: {"%s:%s" % (fac(o), n): c for (o, n), c in sorted(d.items())}
                          for tk, d in sorted(object_counts.items())} or None
    m["planet_names"] = dict(planet_names)
    m["type_names"] = types
    if credit_samples:
        m["credits_series"] = {tk: {cfac(k): v for k, v in d.items()} for tk, d in sorted(credit_samples.items())}
    if credits:
        m["credits"] = {tk: {cfac(k): v for k, v in d.items()} for tk, d in credits.items()}
    if players:
        m["credits_final"] = {cfac(k): v for k, v in sorted(players.items())}
    return m


def queue_metrics(path, m, max_tick=None):
    """Production queues from the reader's production.q2q ("Q2QUEUES 1"): one entry per queued item, keyed by
    (planet, queue, type, player, start frame, tag). Replaces the new-object production of the trace with queue starts,
    so it lines up with the port's build orders (same type names), and adds build frames / cost / completion."""
    f = m["factions"]
    names, pnames = m.get("type_names") or {}, m.get("planet_names") or {}
    entries, live = [], {}
    with open(path, "r", encoding="ascii") as fh:
        if fh.readline().strip() != "Q2QUEUES 1":
            raise ValueError("%s: not Q2QUEUES 1" % path)
        for line in fh:
            p = line.split()
            if not p or p[0] != "Q":
                continue
            mf = int(p[2])
            if max_tick is not None and mf > max_tick:
                break
            planet, queue = int(p[3]), int(p[4])
            for e in p[6:]:
                q = e.split(":")
                if len(q) < 10:
                    continue
                # One queued item = (planet, queue, type, player, tag) while it stays listed. Its start frame can
                # move (a stalled item restarts every tick), so the start is not part of the key; a gap longer than a
                # few missed samples starts a new item.
                key = (planet, queue, int(q[0]), int(q[1]), q[7])
                ent = live.get(key)
                if ent is not None and mf - ent["last"] > 12:
                    ent = None
                if ent is None:
                    ent = live[key] = {"seen": mf, "start": int(q[2]), "planet": pnames.get(planet, str(planet)),
                                       "queue": "space" if queue else "ground", "type": names.get(int(q[0]), q[0]),
                                       "player": int(q[1]), "frames": int(q[3]), "seconds": f32(q[4]),
                                       "cost": f32(q[6]), "created": None, "last": mf, "restarts": 0, "last_start": int(q[2])}
                    entries.append(ent)
                elif int(q[2]) != ent["last_start"]:
                    ent["restarts"] += 1
                    ent["last_start"] = int(q[2])
                ent["last"] = mf
                if ent["created"] is None and int(q[9]) != 0x3FFFFF:
                    ent["created"] = mf
    out = sorted(entries, key=lambda e: (e["start"], e["player"], e["planet"], e["type"]))
    for e in out:
        e["faction"] = f.get(e["player"], "p%d" % e["player"])
        e["done"] = e["created"] if e["created"] is not None else e["last"] + 1
    m["queue_entries"] = out
    m["production"] = [(e["start"], e["faction"], e["planet"], {e["type"]: 1}) for e in out]
    m["production_source"] = "queue starts"
    return m


def lua_metrics(paths, m):
    """Adds the Lua probe output (Q2LUA 1, tools/probe_mod/q2_traces/mod: CYCLE / PLAYER / PLANET / PERCEPTION lines)."""
    credits, owners, perception = {}, {}, {}
    for path in paths:
        cycle = None
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                p = line.split()
                if not p:
                    continue
                if p[0] == "CYCLE" and len(p) >= 2:
                    cycle = int(p[1])
                elif p[0] == "PLAYER" and len(p) >= 4 and cycle is not None:
                    try:
                        credits.setdefault(cycle, {})[p[1]] = float(p[3])
                    except ValueError:
                        credits.setdefault(cycle, {})[p[1]] = None
                elif p[0] == "PLANET" and len(p) >= 4 and cycle is not None:
                    owners.setdefault(cycle, {})[p[1].upper()] = p[3]
                elif p[0] == "PERCEPTION" and len(p) >= 3 and cycle is not None:
                    for kv in p[3:]:
                        if "=" in kv:
                            k, v = kv.split("=", 1)
                            try:
                                val = float(v)
                            except ValueError:
                                val = v
                            perception.setdefault(cycle, {})["%s|%s|%s" % (p[1], p[2].upper(), k)] = val
    if credits:
        m["lua_credits"] = credits
    if owners:
        m["lua_owners"] = owners
    if perception:
        m["perception"] = perception
    return m


# ---- port side -----------------------------------------------------------------------------------------------
def read_crc_columns(path):
    systems, recs = [], []
    with open(path, "r", encoding="ascii") as fh:
        if fh.readline().strip() != "ALAMO_CRC 1":
            raise ValueError("%s: not ALAMO_CRC 1" % path)
        for line in fh:
            p = line.split()
            if not p or p[0] == "#":
                continue
            if p[0] == "systems":
                systems = p[2:]
            elif p[0] == "parity":
                continue
            elif p[0][0].isdigit():
                recs.append((int(p[0]), [int(x, 16) for x in p[3:]]))
    return systems, recs


def port_tactical_start(crc_path):
    """First CRC record with a tactical mode on the stack (the `spatial` digest hashes `top_tactical() != nullptr`
    first, so it leaves its galactic-only value when the battle is pushed). None if the run never goes tactical."""
    systems, recs = read_crc_columns(crc_path)
    if "spatial" not in systems or not recs:
        return None
    k = systems.index("spatial")
    base = recs[0][1][k]
    for tick, cols in recs:
        if cols[k] != base:
            return tick
    return None


def port_rng_draws(crc_path, seed=0, limit=500_000, offset=0):
    """Per-tick synced-RNG draws from the port CRC log's `rng` system digest (state, draw count). `offset` = the record
    that becomes mode frame 1 (a battle pushed mid-run); earlier records are summed into tick 1's count."""
    systems, recs = read_crc_columns(crc_path)
    if "rng" not in systems:
        return None, "port CRC log has no rng system"
    k_rng = systems.index("rng")
    s, d = seed & 0xFFFFFFFF, 0
    out = []
    for tick, cols in recs:
        want = cols[k_rng]
        ss, k = s, 0
        while k <= limit and rng_digest(ss, d + k) != want:
            ss = lcg(ss)
            k += 1
        if k > limit:
            return out, "rng digest of record %d not reached within %d draws (seed %d)" % (tick, limit, seed)
        out.append((tick + 1, tick + 1, k))  # record t = after tick t -> mode frame t + 1
        s, d = ss, d + k
    if offset:
        pre = sum(n for a, _, n in out if a <= offset)
        out = [(offset + 1 - offset, offset + 1 - offset, pre + next((n for a, _, n in out if a == offset + 1), 0))] + [
            (a - offset, a - offset, n) for a, _, n in out if a > offset + 1]
    return out, None


AI_LINE = re.compile(r"^(\d+) p(\d+) (.*)$")


def port_metrics(crc_path=None, ai_log=None, port_trace=None, factions_override=None, seed=0, max_tick=None, tactical=False):
    if port_trace:
        m = trace_metrics(port_trace, factions_override, max_tick)
        m["source"] = os.path.abspath(port_trace)
    else:
        m = empty_metrics(os.path.abspath(crc_path or ai_log or "?"))
    if crc_path and m["rng_draws"] is None:
        offset = 0
        if tactical:
            start = port_tactical_start(crc_path)
            if start is None:
                m["notes"].append("port run never pushed a tactical mode; RNG ticks are the run's own")
            else:
                offset = start
                m["notes"].append("port battle starts at CRC record %d: port tick 1 = that record; the draws of the %d "
                                  "records before it (galaxy, fleet hop, battle setup) are counted into tick 1" % (start, start))
                m["scenario"]["tactical_start_record"] = start
        draws, err = port_rng_draws(crc_path, seed, offset=offset)
        m["rng_draws"] = [x for x in draws if max_tick is None or x[1] <= max_tick] if draws else draws
        m["rng_pause_draws"] = []
        if err:
            m["notes"].append(err)
    if not ai_log:
        return m
    fac_template, human_idx, human_fac = {}, None, None
    owner_changes, moves, builds, owned, conflicts = [], [], [], {}, []
    summary = None
    with open(ai_log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("# galactic-ai campaign"):
                p = line.split()
                if "campaign" in p:
                    m["scenario"]["campaign"] = p[p.index("campaign") + 1]
                if "seed" in p:
                    m["scenario"]["seed"] = p[p.index("seed") + 1]
                if "ticks" in p:
                    m["scenario"]["ticks"] = int(p[p.index("ticks") + 1])
                if "human" in p:
                    human_fac = p[p.index("human") + 1]
                continue
            if line.startswith("# galactic-ai ") and "planets:" in line:
                summary = line
                hm = re.search(r"human p(\d+)", line)
                if hm:
                    human_idx = int(hm.group(1))
                continue
            if line.startswith("# t="):
                tm = re.match(r"# t=(\d+)s planets((?: \d+=\d+)+)", line)
                if tm:
                    tick = int(tm.group(1)) * 30
                    owned[tick] = {int(a): int(b) for a, b in (kv.split("=") for kv in tm.group(2).split())}
                continue
            if line.startswith("conflict "):
                cm = re.match(r"conflict tick (\d+) gframe (\d+) (\S+) (\S+) (\S+) vs (\S+): (.*)$", line)
                if cm:
                    conflicts.append((int(cm.group(1)), cm.group(4).upper(), cm.group(5), cm.group(6), cm.group(7)))
                continue
            om = re.match(r"^(\d+) owner (\S+) (-?\d+) -> (-?\d+)$", line)
            if om:
                owner_changes.append((int(om.group(1)), om.group(2).upper(), int(om.group(3)), int(om.group(4))))
                continue
            am = AI_LINE.match(line)
            if not am:
                continue
            frame, pl, text = int(am.group(1)), int(am.group(2)), am.group(3)
            if text.startswith("templates "):
                fac_template[pl] = text.split()[1]
            elif text.startswith("order move fleet "):
                moves.append((frame, pl, text.split("-> ")[-1].upper()))
            elif text.startswith("order build "):
                w = text.split()
                builds.append((frame, pl, w[2].upper(), w[4].upper() if len(w) > 4 else None))
            elif text.startswith("human ") or " human p" in text:
                pass
    # Factions of the port players.
    facs = {}
    for pl, tmpl in fac_template.items():
        f = next((k for k in FACTION_TOKENS if k.lower() in tmpl.lower()), None)
        if f:
            facs[pl] = f
    if human_idx is not None and human_fac:
        facs[human_idx] = human_fac
    if factions_override:
        facs.update(factions_override)
    m["factions"] = facs

    def fac(i):
        return facs.get(i, "p%d" % i)

    def keep(tk):
        return max_tick is None or tk <= max_tick
    m["owner_changes"] = [(tk, n, fac(a), fac(b)) for tk, n, a, b in owner_changes if keep(tk)]
    m["planets_owned"] = {tk: {fac(k): v for k, v in sorted(d.items()) if v} for tk, d in sorted(owned.items()) if keep(tk)}
    m["fleet_moves"] = [(tk, fac(pl), to) for tk, pl, to in moves if keep(tk)]
    # Starbase levels are not sync-store objects in the original's galactic mode (the vanilla trace has no *STAR_BASE*
    # type at all), so the trace cannot see them: kept apart from the compared production.
    m["production"] = [(tk, fac(pl), at, {t: 1}) for tk, pl, t, at in builds if keep(tk) and "STAR_BASE" not in t]
    m["starbase_orders"] = [(tk, fac(pl), at, t) for tk, pl, t, at in builds if keep(tk) and "STAR_BASE" in t]
    m["production_orders"] = True
    m["conflicts"] = [c for c in conflicts if keep(c[0])]
    if summary:
        pm = re.search(r"planets:((?: \d+=\d+/-?[\d.]+)+)", summary)
        if pm:
            m["credits_final"] = {fac(int(a)): float(c) for a, b, c in
                                  (re.match(r"(\d+)=(\d+)/(-?[\d.]+)", kv).groups() for kv in pm.group(1).split())
                                  if fac(int(a)) in FACTION_TOKENS}
        m["scenario"]["summary"] = summary[2:]
    return m


# ---- comparison ------------------------------------------------------------------------------------------------
class Result:
    def __init__(self, metric, status, first_tick=None, detail="", module=None):
        self.metric, self.status, self.first_tick, self.detail, self.module = metric, status, first_tick, detail, module

    def as_dict(self):
        return {"metric": self.metric, "status": self.status, "first_tick": self.first_tick, "detail": self.detail,
                "module": self.module}


def cmp_rng(ref, port):
    if ref["rng_draws"] is None or port["rng_draws"] is None:
        return Result("rng_draws", "n/a", detail="missing on the %s side" % ("trace" if ref["rng_draws"] is None else "port"))
    pd = {a: n for a, b, n in port["rng_draws"]}
    port_last = max(pd) if pd else 0
    checked = equal = 0
    first = None
    first_detail = ""
    span_bad = 0
    rows = []
    ref_total = port_total = 0
    for a, b, n in ref["rng_draws"]:
        if b > port_last or n is None:
            continue
        pn = sum(pd.get(t, 0) for t in range(a, b + 1))
        checked += 1
        ref_total += n
        port_total += pn
        if pn == n:
            equal += 1
        else:
            if a != b:
                span_bad += 1
            if first is None:
                first = a
                first_detail = "tick %s: trace %d draws, port %d" % (a if a == b else "%d..%d" % (a, b), n, pn)
        if len(rows) < 12:
            rows.append("%s:%d/%d" % (a if a == b else "%d-%d" % (a, b), n, pn))
    pause = sum(n for _, _, n in ref.get("rng_pause_draws") or [])
    hist_r = collections.Counter(n for a, b, n in ref["rng_draws"] if a == b and b <= port_last)
    hist_p = collections.Counter(n for a, b, n in port["rng_draws"])
    detail = ("%d/%d ticks equal; total draws trace %d, port %d (+%d trace draws on paused frames); "
              "first ticks (trace/port) %s; most common per-tick counts trace %s, port %s" % (
                  equal, checked, ref_total, port_total, pause, " ".join(rows),
                  hist_r.most_common(4), hist_p.most_common(4)))
    if checked == 0:
        return Result("rng_draws", "n/a", detail="no tick on both sides; " + detail)
    if first is None:
        return Result("rng_draws", "match", detail=detail)
    return Result("rng_draws", "DIVERGES", first, first_detail + "; " + detail, "engine/galactic + engine/ai (draw order)")


PORT_FIRST_CYCLE_FRAME = 0   # engine/galactic: the frame-0 cycle is bookkeeping only (GalacticSim::start, q2f t02), first payment 1350
PORT_CYCLE_PERIOD = 1350     # econ::fiscal_period_frames(45 s)


def cycle_rule_check(c, first=PORT_FIRST_CYCLE_FRAME, period=PORT_CYCLE_PERIOD):
    """The trace against the port's cycle rule (cycle on sim frame first + k*period; the mode frame seen after it is one
    more). Each trace change is a window (previous sample + 1 .. sample) because samples can be missed."""
    bad = []
    if c.get("first_next_cycle_frame") != first + period or c.get("first_last_cycle_frame") != first:
        bad.append("first sample: last %s next %s (port rule %d / %d)" % (c.get("first_last_cycle_frame"),
                                                                         c.get("first_next_cycle_frame"), first, first + period))
    for k, (lo, hi) in enumerate(c.get("cycle_windows") or [], 1):
        want = first + k * period + 1
        if not lo <= want <= hi:
            bad.append("cycle %d seen in ticks %d..%d, port rule %d" % (k + 1, lo, hi, want))
    return bad


def cmp_cycles(ref, port):
    if ref["fiscal_cycles"] and not port["fiscal_cycles"]:
        c = ref["fiscal_cycles"]
        bad = cycle_rule_check(c)
        d = ("port run has no cycle output; trace vs the port's rule (empty cycle on frame 0, then a paid one every %d): %s; trace "
             "cycle ticks %s" % (PORT_CYCLE_PERIOD, "; ".join(bad) if bad else "all %d cycles agree" % (len(c["cycle_ticks"]) + 1),
                                 c["cycle_ticks"][:12]))
        return Result("fiscal_cycles", "DIVERGES" if bad else "rule-match", None, d,
                      "engine/galactic (economy)" if bad else "engine/apps (headless: no cycle output)")
    if not ref["fiscal_cycles"] or not port["fiscal_cycles"]:
        side = "trace" if not ref["fiscal_cycles"] else "port"
        d = "missing on the %s side" % side
        if ref["fiscal_cycles"]:
            c = ref["fiscal_cycles"]
            d += "; trace: first next_cycle_frame %s, cycle ticks %s" % (c.get("first_next_cycle_frame"), c["cycle_ticks"][:12])
        return Result("fiscal_cycles", "n/a", detail=d, module="engine/apps (headless: no cycle output)" if side == "port" else None)
    r, p = ref["fiscal_cycles"]["cycle_ticks"], port["fiscal_cycles"]["cycle_ticks"]
    n = min(len(r), len(p))
    for i in range(n):
        if r[i] != p[i]:
            return Result("fiscal_cycles", "DIVERGES", min(r[i], p[i]), "cycle %d: trace tick %d, port %d" % (i + 1, r[i], p[i]),
                          "engine/galactic (economy)")
    return Result("fiscal_cycles", "match", detail="%d cycle ticks equal %s" % (n, r[:6]))


def cmp_owners(ref, port):
    out = []
    if ref["owners_initial"] and port["owners_initial"]:
        diff = {k: (v, port["owners_initial"].get(k)) for k, v in ref["owners_initial"].items() if port["owners_initial"].get(k) != v}
        out.append(Result("owners_initial", "DIVERGES" if diff else "match", 0 if diff else None,
                          str(diff) if diff else "%d planets" % len(ref["owners_initial"]), "engine/galactic (campaign setup)" if diff else None))
    elif ref["owners_initial"]:
        out.append(Result("owners_initial", "n/a", detail="port: no initial owners in the AI log (trace: %s)" % ref["owners_initial"]))
    if ref["owner_changes"] is None or port["owner_changes"] is None:
        out.append(Result("owner_changes", "n/a", detail="missing on one side"))
    else:
        r, p = ref["owner_changes"], port["owner_changes"]
        first, det = None, ""
        for i in range(max(len(r), len(p))):
            a = r[i] if i < len(r) else None
            b = p[i] if i < len(p) else None
            if a is None or b is None or a[1:] != b[1:] or abs(a[0] - b[0]) > 30:
                first = min(x[0] for x in (a, b) if x)
                det = "change %d: trace %s, port %s" % (i + 1, a, b)
                break
        if first is None:
            out.append(Result("owner_changes", "match", detail="%d changes" % len(r)))
        else:
            out.append(Result("owner_changes", "DIVERGES", first, det + "; trace %d changes, port %d" % (len(r), len(p)),
                              "engine/ai (galactic AI) + engine/apps/sim (conflicts)"))
    if ref["planets_owned"] and port["planets_owned"]:
        first, det, n = None, "", 0
        for tk in sorted(set(ref["planets_owned"]) & set(port["planets_owned"])):
            n += 1
            a = {k: v for k, v in ref["planets_owned"][tk].items() if v and k in FACTION_TOKENS}
            b = {k: v for k, v in port["planets_owned"][tk].items() if v and k in FACTION_TOKENS}
            if a != b and first is None:
                first, det = tk, "tick %d: trace %s, port %s" % (tk, a, b)
        out.append(Result("planets_owned", "DIVERGES" if first is not None else "match", first,
                          det or "%d samples equal (every %d ticks)" % (n, SAMPLE_EVERY),
                          "engine/ai (galactic AI)" if first is not None else None))
    return out


def per_faction(seq, idx_fac=1):
    d = collections.defaultdict(list)
    for x in seq:
        d[x[idx_fac]].append(x)
    return d


def cmp_fleet(ref, port):
    if ref["fleet_moves"] is None or port["fleet_moves"] is None:
        return [Result("fleet_moves", "n/a", detail="missing on one side")]
    out = []
    rf, pf = per_faction(ref["fleet_moves"]), per_faction(port["fleet_moves"])
    for fac in sorted(set(rf) | set(pf)):
        if fac not in FACTION_TOKENS:
            continue
        r, p = rf.get(fac, []), pf.get(fac, [])
        first, det = None, ""
        for i in range(max(len(r), len(p))):
            a = r[i] if i < len(r) else None
            b = p[i] if i < len(p) else None
            if a is None or b is None or a[2] != b[2] or abs(a[0] - b[0]) > 30:
                first = min(x[0] for x in (a, b) if x)
                det = "move %d: trace %s, port %s" % (i + 1, a and "tick %d -> %s" % (a[0], a[2]), b and "tick %d -> %s" % (b[0], b[2]))
                break
        rt = collections.Counter(x[2] for x in r)
        pt = collections.Counter(x[2] for x in p)
        summ = "trace %d departures (by target %s), port %d (%s)" % (len(r), dict(rt.most_common(5)), len(p), dict(pt.most_common(5)))
        out.append(Result("fleet_moves[%s]" % fac, "DIVERGES" if first is not None else "match", first,
                          (det + "; " if det else "") + summ, "engine/ai (galactic AI plans)" if first is not None else None))
    if ref.get("fleet_hops"):
        hops = ref["fleet_hops"]
        durs = collections.Counter((h[2], h[3], h[4] - h[0]) for h in hops)
        out.append(Result("fleet_hops (trace info)", "info", detail="%d hops; e.g. %s" % (
            len(hops), ", ".join("%s->%s %d ticks" % k for k, _ in durs.most_common(6)))))
    return out


def cmp_production(ref, port):
    """Production batches per faction: (tick, faction, planet, {type: n}). The trace sees completions (new objects, by
    member type), the port AI log sees build orders (by company type), so the batch sequence is compared by planet and the
    tick of the first batch; type names are listed, not matched."""
    if ref["production"] is None or port["production"] is None:
        d = "missing on the %s side" % ("trace" if ref["production"] is None else "port")
        if ref["production"]:
            d += "; trace (%s) %d events, first: %s" % (ref.get("production_source"), len(ref["production"]), "; ".join(
                "tick %d %s %s" % (x[0], x[1], ",".join("%s x%d" % kv for kv in x[3].items())) for x in ref["production"][:6]))
        return [Result("production", "n/a", detail=d)]
    out = []
    what = "build orders" if port.get("production_orders") else "batches"
    pprod = list(port["production"])
    if ref.get("production_source") == "queue starts" and port.get("starbase_orders"):
        # The queues show starbase upgrades, so the port's starbase orders are compared too.
        pprod = sorted(pprod + [(tk, f, at, {t: 1}) for tk, f, at, t in port["starbase_orders"]], key=lambda x: x[0])
    rf, pf = per_faction(ref["production"]), per_faction(pprod)

    def fmt(x):
        return x and "tick %d at %s %s" % (x[0], x[2], ",".join("%s%s" % (t, "" if n == 1 else " x%d" % n) for t, n in x[3].items()))
    for fac in sorted(set(rf) | set(pf)):
        if fac not in FACTION_TOKENS:
            continue
        r, p = rf.get(fac, []), pf.get(fac, [])
        first, det = None, ""
        for i in range(max(len(r), len(p))):
            a = r[i] if i < len(r) else None
            b = p[i] if i < len(p) else None
            if a is None or b is None or a[2] != b[2] or (ref.get("production_source") == "queue starts" and set(a[3]) != set(b[3])):
                first = min(x[0] for x in (a, b) if x)
                det = "item %d: trace %s; port %s" % (i + 1, fmt(a), fmt(b))
                break
        rt, pt = collections.Counter(), collections.Counter()
        for x in r:
            rt.update(x[3])
        for x in p:
            pt.update(x[3])
        summ = "trace %d %s %s; port %d %s %s" % (len(r), ref.get("production_source", "batches"), dict(rt.most_common(6)), len(p),
                                                  what, dict(pt.most_common(6)))
        out.append(Result("production[%s]" % fac, "DIVERGES" if first is not None else "match", first,
                          (det + "; " if det else "") + summ, "engine/ai (galactic AI build plans) + engine/galactic (production)"
                          if first is not None else None))
    return out


def cmp_object_counts(ref, port):
    if not ref["object_counts"] or not port["object_counts"]:
        info = ""
        if ref["object_counts"]:
            last = max(ref["object_counts"])
            tot = collections.Counter()
            for k, v in ref["object_counts"][last].items():
                tot[k.split(":")[0]] += v
            info = "; trace at tick %d: %s objects by faction" % (last, dict(tot))
        return Result("object_counts", "n/a", detail="missing on the port side (needs a port state trace)" + info,
                      module="engine/apps (headless: no object dump)")
    first, det = None, ""
    for tk in sorted(set(ref["object_counts"]) & set(port["object_counts"])):
        a, b = ref["object_counts"][tk], port["object_counts"][tk]
        if a != b:
            diff = {k: (a.get(k, 0), b.get(k, 0)) for k in sorted(set(a) | set(b)) if a.get(k, 0) != b.get(k, 0)}
            first, det = tk, "tick %d: (trace, port) %s" % (tk, dict(list(diff.items())[:8]))
            break
    return Result("object_counts", "DIVERGES" if first is not None else "match", first, det, "engine/galactic" if first is not None else None)


def cmp_credits(ref, port):
    out = []
    rc = ref.get("credits") or {}
    lc = ref.get("lua_credits") or {}
    if not rc and not lc and not ref.get("credits_final"):
        out.append(Result("credits", "n/a", detail="the trace has no credits (reader v1 found no player vector; no Lua probe); "
                          "port final: %s" % port.get("credits_final")))
        return out
    if rc and port.get("credits"):
        first, det = None, ""
        for tk in sorted(set(rc) & set(port["credits"])):
            a, b = rc[tk], port["credits"][tk]
            bad = {f: (a.get(f), b.get(f)) for f in a if f in FACTION_TOKENS and b.get(f) is not None and abs(a[f] - b[f]) > 0.5}
            if bad:
                first, det = tk, "cycle tick %d: (trace, port) %s" % (tk, bad)
                break
        out.append(Result("credits_per_cycle", "DIVERGES" if first is not None else "match", first, det,
                          "engine/galactic (economy)" if first is not None else None))
    elif (rc or lc) and not (ref.get("credits_series") and port.get("credits_series")):
        src = rc or lc
        out.append(Result("credits_per_cycle", "n/a", detail="port has no per-cycle credits (needs a port state trace); "
                          "trace first cycles: %s" % dict(list(sorted(src.items()))[:3]), module="engine/apps (headless)"))
    series = ref.get("credits_series")
    if series and not port.get("credits_series"):
        pts = sorted(series.items())
        show = [pts[0]] + pts[1:][-3:] if len(pts) > 4 else pts
        out.append(Result("credits_series", "n/a", detail="port has no credits over time; trace %s" % "; ".join(
            "tick %d %s" % (tk, {f: round(v, 1) for f, v in d.items() if f in FACTION_TOKENS}) for tk, d in show)))
    elif series and port.get("credits_series"):
        first, det, n = None, "", 0
        for tk in sorted(set(series) & set(port["credits_series"])):
            a, b = series[tk], port["credits_series"][tk]
            n += 1
            bad = {f: (round(a[f], 1), round(b[f], 1)) for f in a if f in FACTION_TOKENS and b.get(f) is not None and abs(a[f] - b[f]) > 0.5}
            if bad and first is None:
                first, det = tk, "tick %d: (trace, port) %s" % (tk, bad)
        out.append(Result("credits_series", "DIVERGES" if first is not None else "match", first, det or "%d samples equal" % n,
                          "engine/galactic (economy)" if first is not None else None))
    if ref.get("credits_final") and port.get("credits_final"):
        a, b = ref["credits_final"], port["credits_final"]
        bad = {f: (a.get(f), b.get(f)) for f in a if f in FACTION_TOKENS and b.get(f) is not None and abs(a[f] - b[f]) > 0.5}
        out.append(Result("credits_final", "DIVERGES" if bad else "match", ref["scenario"].get("ticks") if bad else None,
                          str(bad or a), "engine/galactic (economy) + engine/ai" if bad else None))
    return out


def cmp_lua(ref, port):
    out = []
    if ref.get("lua_owners"):
        cyc = ref["lua_owners"]
        if port.get("lua_owners"):
            first = None
            for c in sorted(set(cyc) & set(port["lua_owners"])):
                if cyc[c] != port["lua_owners"][c]:
                    first = c
                    break
            out.append(Result("lua_owners", "DIVERGES" if first is not None else "match",
                              None if first is None else first * 1350, "cycle %s" % first, "engine/ai" if first is not None else None))
        else:
            out.append(Result("lua_owners", "n/a", detail="port has no per-cycle owners; trace has %d cycles" % len(cyc)))
    if ref.get("perception"):
        out.append(Result("perception", "n/a", detail="trace has %d cycles x %d values; the port has no perception dump yet" % (
            len(ref["perception"]), len(next(iter(ref["perception"].values())))), module="engine/ai (perception equations)"))
    return out


def compare_metrics(ref, port):
    res = [cmp_rng(ref, port), cmp_cycles(ref, port)]
    res += cmp_owners(ref, port)
    res += cmp_fleet(ref, port)
    res += cmp_production(ref, port)
    res.append(cmp_object_counts(ref, port))
    res += cmp_credits(ref, port)
    res += cmp_lua(ref, port)
    if ref.get("queue_entries"):
        q = ref["queue_entries"]
        by_type = {}
        for e in q:
            by_type.setdefault(e["type"], set()).add((e["frames"], e["cost"]))
        stalled = [e for e in q if e["restarts"] > 30]
        res.append(Result("production queues (trace info)", "info", detail="%d queued items; build frames / cost by type: %s%s" % (
            len(q), "; ".join("%s %s" % (t, sorted(v)[:3]) for t, v in sorted(by_type.items())),
            "; stalled items (start frame reset every tick): %s" % "; ".join(
                "%s %s@%s ticks %d-%d (%d restarts)" % (e["faction"], e["type"], e["planet"], e["seen"], e["last"], e["restarts"])
                for e in stalled[:6]) if stalled else "")))
    if port.get("starbase_orders") and ref.get("production_source") != "queue starts":
        res.append(Result("starbase orders (port info)", "info", detail="not visible in the trace (no starbase objects): %s" % (
            ", ".join("%s %d %s@%s" % (f, tk, t, at) for tk, f, at, t in port["starbase_orders"][:8]))))
    if port.get("conflicts") is not None:
        c = port["conflicts"]
        res.append(Result("conflicts (port info)", "info", detail="%d conflicts; first %s" % (len(c), c[:2])))
    return res


def report(ref, port, results):
    lines = ["Q2 parity report",
             "  trace: %s (%s, seed %s, %s ticks)" % (ref["source"], ref["scenario"].get("campaign"), ref["scenario"].get("seed"),
                                                   ref["scenario"].get("ticks")),
             "  port:  %s (%s, seed %s, %s ticks)" % (port["source"], port["scenario"].get("campaign"), port["scenario"].get("seed"),
                                                   port["scenario"].get("ticks")),
             "  factions: trace %s; port %s" % (ref["factions"], port["factions"]),
             "  tick = mode frame (ticks of the mode done); port CRC record t = tick t+1", ""]
    for n in ref["notes"] + port["notes"]:
        lines.append("  NOTE " + n)
    w = max(len(r.metric) for r in results)
    for r in results:
        ft = "" if r.first_tick is None else " first tick %s" % r.first_tick
        mod = "" if not r.module else " [%s]" % r.module
        lines.append("%-*s  %-8s%s%s" % (w, r.metric, r.status, ft, mod))
        if r.detail:
            lines.append("%-*s    %s" % (w, "", r.detail))
    div = [r for r in results if r.status == "DIVERGES"]
    lines.append("")
    lines.append("%d metrics compared: %d match, %d match the port's rule only, %d diverge, %d n/a" % (
        sum(1 for r in results if r.status in ("match", "DIVERGES", "rule-match")), sum(1 for r in results if r.status == "match"),
        sum(1 for r in results if r.status == "rule-match"), len(div), sum(1 for r in results if r.status == "n/a")))
    return "\n".join(lines)


SUMMARY_CREDITS = re.compile(r"planets:((?: \d+=\d+/-?[\d.]+)+)")


def summary_credits(text):
    """{player index: credits} from an alamo_headless galactic-ai summary line ('planets: 0=4/8338 1=4/13668 ...')."""
    mm = SUMMARY_CREDITS.search(text)
    if not mm:
        return None
    return {int(a): float(c) for a, c in re.findall(r"(\d+)=\d+/(-?[\d.]+)", mm.group(1))}


def resolve_trace(path):
    """A run folder or a trace file -> (trace.q2t, [lua_*.txt])."""
    if not os.path.isdir(path) and os.path.basename(path).lower() == "trace.q2t":
        path = os.path.dirname(os.path.abspath(path))
    if os.path.isdir(path):
        lua = sorted(os.path.join(path, f) for f in os.listdir(path) if f.lower().startswith("lua_") and f.lower().endswith(".txt"))
        return os.path.join(path, "trace.q2t"), lua
    return path, []


def run(trace, port_crc=None, port_ai_log=None, port_trace=None, lua=None, port_factions=None, trace_factions=None,
        json_out=None, seed=None, max_tick=None, quiet=False, port_credits=None):
    tpath, lua_auto = resolve_trace(trace)
    ref = trace_metrics(tpath, trace_factions)
    qpath = os.path.join(os.path.dirname(os.path.abspath(tpath)), "production.q2q")
    if os.path.exists(qpath):
        queue_metrics(qpath, ref)
    lua_files = list(lua or []) + [f for f in lua_auto if f not in (lua or [])]
    if lua_files:
        lua_metrics(lua_files, ref)
    if seed is None:
        seed = int(ref["scenario"].get("seed") or "0", 16)
    tick_cap = max_tick if max_tick is not None else ref["scenario"].get("ticks")
    tactical = ref["scenario"].get("scenario") == "skirmish"
    port = port_metrics(port_crc, port_ai_log, port_trace, port_factions, seed, tick_cap, tactical)
    if port_credits:  # {tick: {player index: credits}} from short port runs (q2trace.py compare --port-exe)
        port["credits_series"] = {tk: {port["factions"].get(i, "p%d" % i): v for i, v in d.items()}
                                  for tk, d in sorted(port_credits.items())}
    results = compare_metrics(ref, port)
    text = report(ref, port, results)
    if not quiet:
        print(text)
    if json_out:
        with open(json_out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"trace": ref["scenario"], "port": port["scenario"], "trace_factions": ref["factions"],
                       "port_factions": port["factions"], "results": [r.as_dict() for r in results],
                       "trace_fleet_hops": ref.get("fleet_hops"), "trace_production": ref.get("production"),
                       "port_production_orders": port.get("production_orders"), "trace_owner_changes": ref.get("owner_changes"),
                       "trace_fiscal_cycles": ref.get("fiscal_cycles"), "trace_rng_first": (ref["rng_draws"] or [])[:40],
                       "port_rng_first": (port["rng_draws"] or [])[:40]}, fh, indent=1, sort_keys=True, default=str)
            fh.write("\n")
    return results, (1 if any(r.status == "DIVERGES" for r in results) else 0)


def add_args(ap):
    ap.add_argument("trace", help="trace.q2t or its run folder (lua_*.txt there are read too)")
    ap.add_argument("--port-crc", help="port CRC log (alamo_headless --crc-out)")
    ap.add_argument("--port-ai-log", help="port AI log (alamo_headless --ai-log)")
    ap.add_argument("--port-trace", help="port run written as Q2TRACE 1 (when the port can emit one)")
    ap.add_argument("--lua", action="append", help="Lua probe output (lua_*.txt); repeatable")
    ap.add_argument("--port-factions", help="port player index -> faction, e.g. 0=Rebel,1=Empire,5=Underworld")
    ap.add_argument("--trace-factions", help="trace player index -> faction, e.g. 0=Rebel,1=Empire,6=Underworld")
    ap.add_argument("--seed", type=lambda s: int(s, 0), help="port session seed (default: the trace's seed line)")
    ap.add_argument("--max-tick", type=int, help="compare up to this tick (default: the trace's last mode frame)")
    ap.add_argument("--json", help="write the results as JSON")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_args(ap)
    a = ap.parse_args(argv)
    _, rc = run(a.trace, a.port_crc, a.port_ai_log, a.port_trace, a.lua, parse_map(a.port_factions), parse_map(a.trace_factions),
                a.json, a.seed, a.max_tick)
    return rc


if __name__ == "__main__":
    sys.exit(main())
