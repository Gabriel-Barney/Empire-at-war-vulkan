#!/usr/bin/env python3
"""Q2 reference traces: validate, summarise and convert Q2TRACE files (standard library only).

  q2trace.py validate <trace.q2t> [--json summary.json]
      Re-reads the trace, rebuilds the object state frame by frame, recomputes every digest the reader
      wrote, counts RNG draws per frame and summarises economy/ownership data. Exit 0 = consistent.
  q2trace.py to-q1 <trace.q2t> <out.crc> [--tick mode|global] [--include-torn]
      Writes an ALAMO_CRC 1 log (engine/core sim_crc.hpp) with systems ref_rng, ref_objects and parity
      parts objects, rng, so the new engine's Q1 harness can compare against the original game.
  q2trace.py compare <reference.crc> <engine.crc>
      Compares an engine CRC log against a reference log on the reference's ticks only (needs the engine to register
      the ref_rng / ref_objects systems; the port does not yet).
  q2trace.py compare <trace.q2t | run dir> (--port-exe alamo_headless.exe | --port-crc port.crc [--port-ai-log ai.txt])
                     [--port-out dir] [--lua lua_x.txt] [--json out.json] [--port-factions 0=Rebel,...]
      Per-metric behaviour-parity report of a trace against a port run of the same scenario (seed, hands-off, ticks):
      RNG draws per tick, fiscal cycles, planet owners, fleet moves, production, object counts, credits, Lua perception.
      Each divergence gets its first tick and the engine module it points at. --port-exe runs the port itself
      (galactic: alamo_headless --economy --galactic-ai --campaign <trace campaign> --human <its faction> --seed <trace
      seed> --ticks <trace ticks> --trace-out <port-out>/port.q2t; needs ALAMO_FOC_DATA / ALAMO_EAW_DATA); the port's
      state trace then feeds owners, object counts, credits and cycles per tick. Logic: analysis/q2_parity.py. Exit 1 if
      anything diverges.
  q2trace.py selftest [--reader q2reader.exe]
      Checks this file's hash/CRC code against fixed vectors (and against the reader's --selftest).
  q2trace.py selftest-fake --bin <dir with q2reader.exe and fake_game.exe> --out <dir>
      End-to-end test: runs fake_game.exe, traces it with the reader, validates the result.

Trace format: tools/probe_mod/q2_traces/README.md "Trace format".
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import zlib

MASK64 = (1 << 64) - 1
P1, P2, P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x27D4EB2F165667C5
LCG_MUL, LCG_ADD = 0x41C64E6D, 0xBDF


# ---- checksums (bit-identical to engine/core sim_crc and the reader) ---------------------------------
class StateHasher:
    def __init__(self):
        self.h = P5  # seed 0
        self.n = 0
        self.tail = b""

    @staticmethod
    def _round(h, w):
        h = (h + w * P2) & MASK64
        h = ((h << 31) | (h >> 33)) & MASK64
        return (h * P1) & MASK64

    def bytes(self, b):
        self.n += len(b)
        data = self.tail + b
        full = len(data) // 8 * 8
        for i in range(0, full, 8):
            self.h = self._round(self.h, int.from_bytes(data[i:i + 8], "little"))
        self.tail = data[full:]

    def u32(self, v):
        self.bytes(struct.pack("<I", v & 0xFFFFFFFF))

    def u64(self, v):
        self.bytes(struct.pack("<Q", v & MASK64))

    def str(self, s):
        b = s.encode()
        self.u32(len(b))
        self.bytes(b)

    def digest(self):
        h = self._round(self.h, int.from_bytes(self.tail.ljust(8, b"\0"), "little"))
        h ^= self.n
        h ^= h >> 33
        h = (h * 0xFF51AFD7ED558CCD) & MASK64
        h ^= h >> 33
        h = (h * 0xC4CEB9FE1A85EC53) & MASK64
        h ^= h >> 33
        return h


def crc32_raw(b, crc=0):
    # Reflected 0xEDB88320, init 0, no final inversion == zlib.crc32 with pre/post inversion undone.
    return (zlib.crc32(b, crc ^ 0xFFFFFFFF) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def object_crc(pos_bits, fac_bits, health_bits):
    x = 0
    for v in pos_bits + fac_bits:
        x ^= v
    b = struct.pack("<I", x)
    if f32(health_bits) != 0.0:
        b += struct.pack("<I", health_bits)
    return crc32_raw(b)


def rng_crc(s):
    return crc32_raw(struct.pack("<I", s))


def combine(names, crcs):
    h = StateHasher()
    for n, c in zip(names, crcs):
        h.str(n)
        h.u64(c)
    return h.digest()


def lcg_steps(a, b, limit):
    """Number of LCG steps from state a to state b (None if more than limit)."""
    s = a
    for i in range(limit + 1):
        if s == b:
            return i
        s = (s * LCG_MUL + LCG_ADD) & 0xFFFFFFFF
    return None


# ---- parsing ----------------------------------------------------------------------------------------
class Frame:
    __slots__ = ("g", "mtype", "mframe", "rng", "pause", "stack", "flags", "cycle", "s", "key", "lines")

    def __init__(self, parts):
        self.g, self.mtype, self.mframe = int(parts[1]), int(parts[2]), int(parts[3])
        self.rng = int(parts[4], 16)
        self.pause, self.stack, self.flags = int(parts[5]), int(parts[6]), int(parts[7])
        self.cycle = tuple(int(x) for x in parts[8:11]) if len(parts) >= 11 else None
        self.s = None
        self.key = False
        self.lines = []


def parse(path):
    meta, header = {}, {}
    frames, types, end_reason = [], {}, None
    cur = None
    with open(path, "r", encoding="ascii", newline="\n") as fh:
        first = fh.readline().rstrip("\n")
        if first != "Q2TRACE 1":
            raise ValueError("not a Q2TRACE 1 file: %r" % first)
        in_header = True
        for ln, line in enumerate(fh, 2):
            line = line.rstrip("\n")
            if not line:
                continue
            p = line.split(" ")
            tag = p[0]
            if in_header:
                if tag == "meta":
                    meta[p[1]] = " ".join(p[2:])
                elif tag == "end_header":
                    in_header = False
                else:
                    header[tag] = p[1:]
                continue
            if tag == "F":
                cur = Frame(p)
                frames.append(cur)
            elif tag == "S":
                cur.s = p[1:]
            elif tag == "K":
                cur.key = True
            elif tag == "M":
                meta[p[1]] = " ".join(p[2:])
            elif tag in ("O", "X", "G", "C", "P"):
                cur.lines.append(p)
            elif tag == "start":
                header["start"] = p[1:]
            elif tag == "T":
                types[int(p[1])] = (int(p[2], 16), int(p[3], 16), " ".join(p[4:]))
            elif tag == "E":
                end_reason = " ".join(p[1:])
            elif tag == "end":
                break
            else:
                raise ValueError("line %d: unknown tag %r" % (ln, tag))
    return meta, header, frames, types, end_reason


# ---- validate -----------------------------------------------------------------------------------------
def validate(path, json_out=None, quiet=False):
    meta, header, frames, types, end_reason = parse(path)
    errors, notes = [], []
    objs, planets, players = {}, {}, {}
    seed = int(header["seed"][0], 16) if "seed" in header else None
    draws, prev = [], None
    crc_agree = crc_total = 0
    digest_ok = digest_bad = 0
    torn = missed = light = 0
    cached_zero = 0
    credits_by_cycle, cycle_frames, owner_changes, control_samples = [], [], [], []
    last_cycle = None
    for fr in frames:
        if fr.key:
            objs, planets = {}, {}
        for p in fr.lines:
            t = p[0]
            if t == "O":
                objs[int(p[1])] = p[1:]
            elif t == "X":
                objs.pop(int(p[1]), None)
                planets.pop(int(p[1]), None)
            elif t == "G":
                planets[int(p[1])] = p[1:]
            elif t == "C":
                players[int(p[1])] = p[1:]
        if fr.flags & 1:
            torn += 1
        if prev is not None and fr.g > prev.g + 1:
            missed += fr.g - prev.g - 1
        # RNG draws between consecutive samples (frames per step may be > 1 when samples were missed).
        if prev is not None:
            n = lcg_steps(prev.rng, fr.rng, 2_000_000)
            draws.append((fr.g, fr.g - prev.g, n))
        if fr.flags & 4:  # light sample: no object read on this tick
            light += 1
        else:
            # Recompute the reader's digests from the rebuilt state.
            live = sorted((oid, o) for oid, o in objs.items() if not int(o[10]) & 2)
            h = StateHasher()
            h.u32(len(live))
            sum_calc = 0
            for oid, o in live:
                pos = [int(x, 16) for x in o[3:6]]
                fac = [int(x, 16) for x in o[6:9]]
                hp = int(o[9], 16)
                calc = object_crc(pos, fac, hp)
                if calc != int(o[15], 16):
                    errors.append("frame %d object %d: crc_calc mismatch" % (fr.g, oid))
                cached = int(o[14], 16)
                if not cached:
                    cached_zero += 1
                if cached:
                    crc_total += 1
                    crc_agree += cached == calc
                sum_calc = (sum_calc + calc) & 0xFFFFFFFF
                tinfo = types.get(int(o[2]))
                h.u32(oid)
                h.u32(int(o[1]))
                h.u32(tinfo[0] if tinfo else 0)
                for v in pos + fac + [hp]:
                    h.u32(v)
            hr = StateHasher()
            hr.u32(fr.rng)
            if fr.s is None:
                errors.append("frame %d: no S line" % fr.g)
            else:
                ok = (int(fr.s[1]) == len(live) and int(fr.s[3], 16) == sum_calc and int(fr.s[4], 16) == rng_crc(fr.rng)
                      and int(fr.s[5], 16) == hr.digest() and int(fr.s[6], 16) == h.digest())
                if ok:
                    digest_ok += 1
                else:
                    digest_bad += 1
                    if digest_bad <= 5:
                        errors.append("frame %d: S line does not match the rebuilt state" % fr.g)
        # Economy (galactic).
        if fr.cycle is not None:
            if last_cycle is not None and fr.cycle[2] != last_cycle:
                cycle_frames.append({"global_frame": fr.g, "mode_frame": fr.mframe, "cycle": fr.cycle[2],
                                     "last_cycle_frame": fr.cycle[0], "next_cycle_frame": fr.cycle[1]})
                credits_by_cycle.append({"cycle": fr.cycle[2], "credits": {i: f32(int(c[1], 16))
                                                                           for i, c in sorted(players.items())}})
            last_cycle = fr.cycle[2]
        prev = fr
    # Planet ownership changes and political-control ramps (from the rebuilt deltas, second pass).
    owner, ctrl = {}, {}
    objs = {}
    for fr in frames:
        if fr.key:
            objs = {}
        for p in fr.lines:
            if p[0] == "O":
                objs[int(p[1])] = p[1:]
            elif p[0] == "G":
                pid = int(p[1])
                o = objs.get(pid)
                own = int(o[1]) if o else None
                c = f32(int(p[2], 16))
                if pid in owner and owner[pid] != own:
                    owner_changes.append({"global_frame": fr.g, "mode_frame": fr.mframe, "planet": pid,
                                          "from": owner[pid], "to": own})
                if pid in ctrl and ctrl[pid] != c and len(control_samples) < 2000:
                    control_samples.append({"mode_frame": fr.mframe, "planet": pid, "control": c})
                owner[pid], ctrl[pid] = own, c
    seed_steps = None
    if seed is not None and "start" in header:
        seed_steps = lcg_steps(seed, int(header["start"][3], 16), 5_000_000)
    ring_written = sum(1 for fr in frames if fr.s is not None and len(fr.s) > 7
                       and int(fr.s[7][:8], 16) != 0xFFFFFFFF and int(fr.s[7], 16) != 0)
    if seed == 0 and seed_steps == 0:
        notes.append("seed slot GMM+0xE0 is 0 and the RNG state at the first sample is 0: this start path never "
                     "wrote a seed, so the synced RNG starts from 0 (deterministic)")
    if crc_total == 0 and cached_zero:
        notes.append("cached per-object CRC (+0x2C8) is always 0: the frame CRC is not computed in this session")
    if ring_written == 0:
        notes.append("FrameSynchronizer CRC ring never written (entries stay frame=-1): no frame CRC in single player")
    hist = {}
    for _, gap, n in draws:
        if gap == 1 and n is not None:
            hist[n] = hist.get(n, 0) + 1
    summary = {
        "trace": os.path.abspath(path), "meta": meta, "end_reason": end_reason,
        "frames": len(frames), "first_frame": frames[0].g if frames else None,
        "last_frame": frames[-1].g if frames else None, "missed_frames": missed, "torn_frames": torn, "light_samples": light,
        "digest_ok": digest_ok, "digest_bad": digest_bad,
        "object_crc_cached_vs_calc": {"compared": crc_total, "equal": crc_agree, "cached_zero": cached_zero},
        "seed": header.get("seed"), "draws_seed_to_first_sample": seed_steps,
        "rng_unresolved_gaps": sum(1 for d in draws if d[2] is None),
        "rng_draws_per_frame_histogram": dict(sorted(hist.items())),
        "types": len(types), "fiscal_cycles": cycle_frames, "credits_by_cycle": credits_by_cycle,
        "planet_owner_changes": owner_changes, "political_control_samples": control_samples[:200],
        "players_last": {i: {"credits": f32(int(c[1], 16)), "human": int(c[3]), "ai": int(c[4])} for i, c in sorted(players.items())},
        "frame_crc_ring_written": ring_written, "notes": notes,
        "errors": errors[:50],
    }
    if json_out:
        with open(json_out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(summary, fh, indent=1, sort_keys=True)
            fh.write("\n")
    if not quiet:
        print("trace %s: %d frames (%s..%s), missed %d, torn %d, end=%s" % (
            path, len(frames), summary["first_frame"], summary["last_frame"], missed, torn, end_reason))
        print("digests ok %d bad %d; cached object CRC equal %d/%d; seed->first sample %s draws" % (
            digest_ok, digest_bad, crc_agree, crc_total, seed_steps))
        print("draws/frame histogram: %s" % summary["rng_draws_per_frame_histogram"])
        if cycle_frames:
            print("fiscal cycles seen: %d; planet owner changes: %d" % (len(cycle_frames), len(owner_changes)))
        for n_ in notes:
            print("NOTE " + n_)
        for e in errors[:10]:
            print("ERROR " + e)
    return summary, (0 if not errors else 1)


# ---- Q1 conversion --------------------------------------------------------------------------------------
SYSTEMS = ["ref_rng", "ref_objects"]
PARITY = ["objects", "rng"]


def to_q1(path, out, tick_mode="mode", include_torn=False):
    meta, header, frames, types, end_reason = parse(path)
    lines = ["ALAMO_CRC 1", "systems %d %s" % (len(SYSTEMS), " ".join(SYSTEMS)),
             "parity %d %s" % (len(PARITY), " ".join(PARITY))]
    lines.append("# source q2trace %s" % os.path.basename(path))
    for k in sorted(meta):
        lines.append("# meta %s %s" % (k, meta[k]))
    if "seed" in header:
        lines.append("# seed %s" % " ".join(header["seed"]))
    lines.append("# tick %s (mode: mode_frame - 1 = ticks of this mode before the sample)" % tick_mode)
    last_tick = -1
    n = 0
    for fr in frames:
        if fr.s is None or (fr.flags & 1 and not include_torn):
            continue
        tick = fr.mframe - 1 if tick_mode == "mode" else fr.g
        if tick <= last_tick or tick < 0:
            continue
        crcs = [int(fr.s[5], 16), int(fr.s[6], 16)]
        parity = ((int(fr.s[3], 16) + int(fr.s[4], 16)) & 0xFFFFFFFF) ^ 0xFFFFFFFF
        lines.append("%d %016x %08x %s" % (tick, combine(SYSTEMS, crcs), parity, " ".join("%016x" % c for c in crcs)))
        last_tick = tick
        n += 1
    with open(out, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("wrote %s: %d records" % (out, n))
    return 0


def read_crc(path):
    systems, parity, recs = [], [], {}
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
                parity = p[2:]
            else:
                recs[int(p[0])] = (int(p[1], 16), int(p[2], 16), [int(x, 16) for x in p[3:]])
    return systems, parity, recs


def compare(ref_path, eng_path):
    rs, rp, rr = read_crc(ref_path)
    es, ep, er = read_crc(eng_path)
    common = [s for s in rs if s in es]
    if not common:
        print("engine log has none of the reference systems %s; comparing parity only" % rs)
    checked = 0
    for tick in sorted(rr):
        if tick not in er:
            print("DIVERGED tick %d: missing in engine log" % tick)
            return 1
        _, rpar, rc = rr[tick]
        _, epar, ec = er[tick]
        for s in common:
            if rc[rs.index(s)] != ec[es.index(s)]:
                print("DIVERGED tick %d system %s" % (tick, s))
                return 1
        if rp == ep and rpar != epar:
            print("DIVERGED tick %d parity %08x != %08x" % (tick, rpar, epar))
            return 1
        checked += 1
    print("MATCH on %d reference ticks (systems %s)" % (checked, common or "parity"))
    return 0


# ---- self tests -----------------------------------------------------------------------------------------
def selftest_vectors():
    h = StateHasher()
    out = ["hash_empty %016x" % h.digest()]
    h.u32(0x12345678)
    out.append("hash_u32 %016x" % h.digest())
    h2 = StateHasher()
    for i in range(37):
        h2.u32((i * 2654435761) & 0xFFFFFFFF)
    h2.str("REBEL_X-WING_SQUADRON")
    out.append("hash_mix %016x" % h2.digest())
    fb = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]  # noqa: E731
    pos, fac = [fb(100.5), fb(-20.25), fb(3.0)], [fb(0.0), fb(1.0), fb(0.0)]
    out.append("object_crc %08x %08x" % (object_crc(pos, fac, fb(250.0)), object_crc(pos, fac, 0)))
    out.append("rng_crc %08x" % rng_crc(0xDEADBEEF))
    out.append("name_crc %08x" % (zlib.crc32(b"STAR_DESTROYER") & 0xFFFFFFFF))
    s = 1
    for _ in range(1000):
        s = (s * LCG_MUL + LCG_ADD) & 0xFFFFFFFF
    out.append("lcg_1000 %08x" % s)
    return out


# Values printed by engine/core (alamo::StateHasher, parity_object_crc, parity_rng_crc) on 2026-10-27.
ENGINE_VECTORS = {"hash_empty": "81dd4863b7d2d7ae", "hash_u32": "cc42c7be799764e6", "hash_mix": "0d72e55ab92bd3ef",
                  "object_crc": "c41add27 f9f8db15", "rng_crc": "3b1ebf03"}


def selftest(reader=None):
    mine = selftest_vectors()
    rc = 0
    for line in mine:
        k, v = line.split(" ", 1)
        if k in ENGINE_VECTORS and ENGINE_VECTORS[k] != v:
            print("MISMATCH vs engine: %s %s != %s" % (k, v, ENGINE_VECTORS[k]))
            rc = 1
    if reader:
        theirs = subprocess.run([reader, "--selftest"], capture_output=True, text=True, check=False).stdout.split("\n")
        theirs = [t.strip() for t in theirs if t.strip()]
        if theirs != mine:
            print("MISMATCH vs reader:\n  py:     %s\n  reader: %s" % (mine, theirs))
            rc = 1
    print("selftest %s" % ("OK" if rc == 0 else "FAILED"))
    return rc


def selftest_fake(bindir, outdir):
    os.makedirs(outdir, exist_ok=True)
    rc = selftest(os.path.join(bindir, "q2reader.exe"))
    for scen, fake_mode, seed, extra in (
            ("skirmish", "space", "1234abcd", ["--ticks", "150", "--object-budget-ms", "0"]),
            ("galactic", "galactic", "0", ["--days", "4", "--day-frames", "30"])):
        fake = subprocess.Popen([os.path.join(bindir, "fake_game.exe"), fake_mode, "400", "60", seed],
                                stdout=subprocess.PIPE, text=True)
        first = fake.stdout.readline().split()
        base, pid = first[1], first[3]
        trace = os.path.join(outdir, "fake_%s.q2t" % scen)
        status = os.path.join(outdir, "fake_%s_status.json" % scen)
        cmd = [os.path.join(bindir, "q2reader.exe"), "--out", trace, "--status", status, "--scenario", scen,
               "--pid", pid, "--test-base", base, "--key-every", "50", "--wait-session-sec", "20",
               "--timeout-sec", "60", "--meta", "test=fake_game"] + extra
        r = subprocess.run(cmd, check=False)
        fake.kill()
        fake.wait()
        if r.returncode != 0:
            print("reader exit %d for %s" % (r.returncode, scen))
            rc = 1
            continue
        summary, vrc = validate(trace, os.path.join(outdir, "fake_%s_summary.json" % scen))
        _, _, frames, _, _ = parse(trace)
        # fake_game draws f % 5 numbers on frame f: check every single-frame gap.
        prev = None
        wrong = 0
        for fr in frames:
            expect = fr.g % 5 + (20 if scen == "galactic" and fr.g == 1 else 0)
            if prev is not None and fr.g == prev.g + 1 and lcg_steps(prev.rng, fr.rng, 100) != expect:
                wrong += 1
            prev = fr
        if vrc or wrong or summary["digest_bad"]:
            print("FAKE %s FAILED (validate %d, wrong draw counts %d)" % (scen, vrc, wrong))
            rc = 1
        q1 = os.path.join(outdir, "fake_%s.crc" % scen)
        to_q1(trace, q1)
        if compare(q1, q1) != 0:
            rc = 1
        if scen == "galactic" and len(summary["fiscal_cycles"]) < 3:
            print("FAKE galactic: expected fiscal cycles, got %d" % len(summary["fiscal_cycles"]))
            rc = 1
        if not summary["players_last"] or (scen == "galactic" and not summary["credits_by_cycle"][-1]["credits"]):
            print("FAKE %s: no player credits (player vector not found)" % scen)
            rc = 1
        if scen == "skirmish" and summary["light_samples"] == 0:
            print("FAKE skirmish: expected light samples with --object-budget-ms 0")
            rc = 1
        if scen == "galactic" and not any("seed" in n for n in summary["notes"]):
            print("FAKE galactic: expected the seed-0 note")
            rc = 1
    print("selftest-fake %s" % ("OK" if rc == 0 else "FAILED"))
    return rc


def parity_args(parity):
    """The parity options of analysis/q2_parity.py, minus its positional trace argument."""
    ap = argparse.ArgumentParser(add_help=False)
    parity.add_args(ap)
    out = []
    for act in ap._actions:  # noqa: SLF001 (argparse has no public accessor)
        if not act.option_strings:
            continue
        kw = {"help": act.help, "default": act.default}
        if isinstance(act, argparse._AppendAction):  # noqa: SLF001
            kw["action"] = "append"
        elif act.type is not None:
            kw["type"] = act.type
        out.append((act.option_strings, kw))
    return out


def run_port(a, trace_path, parity):
    """Runs alamo_headless for the trace's galactic scenario; returns (crc, ai_log) or None."""
    meta, header, frames, _, _ = parse(trace_path)
    if meta.get("scenario") != "galactic" or not meta.get("campaign"):
        print("--port-exe: only galactic traces can be replayed automatically (scenario %s); pass --port-crc" % meta.get("scenario"))
        return None
    if not os.environ.get("ALAMO_FOC_DATA"):
        print("--port-exe: set ALAMO_FOC_DATA (and ALAMO_EAW_DATA) to the game data folders")
        return None
    campaign = meta["campaign"]
    human = campaign.rsplit("_", 1)[-1]  # Sandbox_<map>_<Empire|Rebel|Underworld>: the human's faction
    seed = int(header.get("seed", ["0"])[0], 16)
    ticks = a.port_ticks or max(fr.mframe for fr in frames)
    os.makedirs(a.port_out, exist_ok=True)
    crc = os.path.join(a.port_out, "port.crc")
    ai = os.path.join(a.port_out, "port_ai.txt")
    q2t = os.path.join(a.port_out, "port.q2t")
    cmd = [a.port_exe, "--economy", "--galactic-ai", "--campaign", campaign, "--human", human, "--seed", str(seed),
           "--ticks", str(ticks), "--workers", str(a.port_workers), "--quiet", "--crc-out", crc, "--ai-log", ai]
    if a.port_state_trace:
        cmd += ["--trace-out", q2t]  # q2f t05: per-tick port state trace (owners, objects, credits, cycles)
    print("port run: " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    with open(os.path.join(a.port_out, "port_run.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
    if r.returncode != 0 or not os.path.exists(crc):
        print("port run failed (exit %d), see %s" % (r.returncode, os.path.join(a.port_out, "port_run.txt")))
        return None
    if a.port_state_trace and not os.path.exists(q2t):
        print("port run wrote no state trace (an alamo_headless without --trace-out?); retry with --no-port-state-trace")
        return None
    trace_out = q2t if a.port_state_trace else None
    # Credits over time: without the state trace the port prints credits only at the end of a run, so the same
    # (deterministic) run is repeated with shorter --ticks at the trace's credit sample ticks (its first C line, then
    # every 1800 ticks).
    series = {}
    if a.port_credit_samples and not trace_out:
        first_c, mf = None, 0
        with open(trace_path, "r", encoding="ascii") as fh:
            for line in fh:
                if line.startswith("F "):
                    mf = int(line.split()[3])
                elif line.startswith("C "):
                    first_c = mf
                    break
        if first_c is not None:
            samples = sorted({max(first_c, 1)} | set(range(parity.SAMPLE_EVERY, ticks + 1, parity.SAMPLE_EVERY)))
            for t in samples:
                cmd_t = [a.port_exe, "--economy", "--galactic-ai", "--campaign", campaign, "--human", human, "--seed", str(seed),
                         "--ticks", str(t), "--workers", str(a.port_workers), "--quiet"]
                rt = subprocess.run(cmd_t, capture_output=True, text=True, check=False)
                got = parity.summary_credits(rt.stdout)
                if got is not None:
                    series[t] = got
            print("port credit samples: %d runs (%s)" % (len(series), ", ".join(str(t) for t in sorted(series))))
    return crc, ai, series, trace_out


def compare_parity(a, parity):
    trace_path, _ = parity.resolve_trace(a.reference)
    run_dir = os.path.dirname(os.path.abspath(trace_path))
    if not a.port_out:
        a.port_out = os.path.join(run_dir, "parity")
    if a.port_exe:
        got = run_port(a, trace_path, parity)
        if not got:
            return 2
        a.port_crc, a.port_ai_log, port_credits, port_trace = got
        if port_trace:
            a.port_trace = port_trace
    else:
        port_credits = None
    if not a.port_crc and not a.port_ai_log and not a.port_trace:
        print("compare: give --port-exe, --port-crc / --port-ai-log or --port-trace")
        return 2
    os.makedirs(a.port_out, exist_ok=True)
    json_out = a.json or os.path.join(a.port_out, "parity.json")
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        results, rc = parity.run(a.reference, a.port_crc, a.port_ai_log, a.port_trace, a.lua, parity.parse_map(a.port_factions),
                                 parity.parse_map(a.trace_factions), json_out, a.seed, a.max_tick, port_credits=port_credits)
    text = buf.getvalue()
    print(text, end="")
    with open(os.path.join(a.port_out, "parity_report.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote %s and %s" % (os.path.join(a.port_out, "parity_report.txt"), json_out))
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validate")
    v.add_argument("trace")
    v.add_argument("--json")
    q = sub.add_parser("to-q1")
    q.add_argument("trace")
    q.add_argument("out")
    q.add_argument("--tick", choices=["mode", "global"], default="mode")
    q.add_argument("--include-torn", action="store_true")
    c = sub.add_parser("compare")
    c.add_argument("reference", help="reference.crc (CRC compare) or trace.q2t / run dir (parity report)")
    c.add_argument("engine", nargs="?", help="engine.crc (CRC compare only)")
    c.add_argument("--port-exe", help="run this alamo_headless for the port side")
    c.add_argument("--port-out", help="folder for the port run's logs and the report (default <run dir>/parity)")
    c.add_argument("--port-workers", type=int, default=4)
    c.add_argument("--port-ticks", type=int, help="port ticks (default: the trace's last mode frame)")
    c.add_argument("--no-port-credit-samples", dest="port_credit_samples", action="store_false",
                   help="skip the short port runs that sample credits over time (galactic, --port-exe)")
    c.add_argument("--no-port-state-trace", dest="port_state_trace", action="store_false",
                   help="do not ask the port run for its Q2TRACE state trace (--trace-out, q2f t05; older alamo_headless)")
    import importlib
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "analysis"))
    parity = importlib.import_module("q2_parity")
    for act in parity_args(parity):
        c.add_argument(*act[0], **act[1])
    s = sub.add_parser("selftest")
    s.add_argument("--reader")
    f = sub.add_parser("selftest-fake")
    f.add_argument("--bin", required=True)
    f.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.cmd == "validate":
        return validate(a.trace, a.json)[1]
    if a.cmd == "to-q1":
        return to_q1(a.trace, a.out, a.tick, a.include_torn)
    if a.cmd == "compare":
        if a.engine and a.reference.lower().endswith(".crc"):
            return compare(a.reference, a.engine)
        return compare_parity(a, parity)
    if a.cmd == "selftest":
        return selftest(a.reader)
    return selftest_fake(a.bin, a.out)


if __name__ == "__main__":
    sys.exit(main())
