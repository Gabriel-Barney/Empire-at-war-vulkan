#!/usr/bin/env python3
"""cu8h_dbg_parse.py - report for a cu8h_dbg.py trace (cu8h t01; CU8G_T02_ENV_PICK_RE sec. 3 / sec. 7 P1).

  cu8h_dbg_parse.py TRACE.log [--exe <game>\\corruption\\StarWarsG.exe] [--out report.txt] [--n-env 4] [--all-runs]

Every HIT line is one write of the synced LCG state. Its stack (RVAs) is mapped to function starts with the exe's own
.pdata table (read from the file; the draw primitives 0x1ffb40 / 0x1ffbb0 / 0x532480 / 0x5324d0 / 0x1ffdb0 are leaf
functions without an entry, so their callers are the first frames with one). Each hit gets a phase from the functions
on its stack (first match in PHASES order), and a site (the function of the first return address outside the
primitives). Consecutive hits with the same thread, phase and site are printed as one run.

Checks (sec. 7 P1 expectations):
  A  no ENV_PICK hit before the last CHUNK102 hit (and at least one CHUNK102 hit);
  B  exactly one ENV_PICK draw per load (one int draw: one or more LCG steps from one call), and its index for
     n = --n-env (int(0, n-1): r15 & mask, rejection loop as 0x1ffb40);
  C  post-load phases (SKIRMISH_FORCES / CAMPAIGN_POSTLOAD / SKYDOME / ...) only after the pick;
  D  WEATHER draws on the first land tick: their count (2 expected when the picked record is a rain one).
"""
import argparse
import bisect
import os
import struct
import sys

sys.dont_write_bytecode = True
DEFAULT_EXE = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption\StarWarsG.exe'

PRIMITIVES = {0x1ffb40: 'int', 0x1ffbb0: 'float', 0x532480: 'float_b', 0x5324d0: 'float01', 0x1ffdb0: 'percent'}
# (phase, function starts any of which on the stack gives the phase) - first match wins
PHASES = [
    ('ENV_PICK', (0x277030,)),
    ('WEATHER', (0x468a50, 0x468980, 0x46a0c0)),
    ('SKYDOME', (0x3bc1c0, 0x4d98c0)),
    ('CHUNK102', (0x2b0a20, 0x35f970)),
    ('SKIRMISH_FORCES', (0x2a4fc0,)),
    ('CAMPAIGN_POSTLOAD', (0x2a1eb0,)),
    ('ANIM_VARIANT', (0x2aafb0,)),
    ('COMBINE', (0x29dc30,)),
    ('POSTLOAD_START', (0x3b7af0, 0x4d75a0)),
    ('START_GAME', (0x28e890,)),
    ('LAND_LOAD_OTHER', (0x3b8cd0,)),
    ('SPACE_LOAD_OTHER', (0x4d80b0,)),
    ('LOAD_MAP_OTHER', (0x56480,)),
    ('LAND_TICK', (0x3bb440,)),
    ('COMMON_TICK', (0x3639d0,)),
]
SITES = {0x381a90: 'hardpoint_ctor', 0x37c050: 'sap_stagger', 0x3a4820: 'behaviour_attach', 0x378d40: 'spawn_table',
         0x29f810: 'create_object', 0x3989a0: 'init_from_type', 0x3a0930: 'object_finish', 0x277030: 'env_pick',
         0x468a50: 'weather_restart', 0x468980: 'weather_draw', 0x28e890: 'start_game', 0x2a4fc0: 'skirmish_forces',
         0x2a1eb0: 'campaign_postload', 0x501050: 'anim_variant', 0x3a92f0: 'anim_variant2', 0x527dd0: 'ai_data',
         0x59d270: 'land_tick_59d270'}
LOAD_PHASES = ('CHUNK102', 'ENV_PICK', 'START_GAME', 'LAND_LOAD_OTHER', 'SPACE_LOAD_OTHER', 'LOAD_MAP_OTHER')
POST_PHASES = ('SKIRMISH_FORCES', 'CAMPAIGN_POSTLOAD', 'SKYDOME', 'ANIM_VARIANT', 'COMBINE', 'POSTLOAD_START')


class Funcs:
    """Function starts from the exe's .pdata (the file, so the report does not need the game running)."""

    def __init__(self, exe):
        self.starts, self.begins, self.ends, self.roots = [], [], [], []
        if not exe or not os.path.isfile(exe):
            return
        b = open(exe, 'rb').read()
        o = struct.unpack_from('<I', b, 0x3C)[0]
        ns = struct.unpack_from('<H', b, o + 6)[0]
        so = struct.unpack_from('<H', b, o + 20)[0]
        opt = o + 24
        pd_rva, pd_size = struct.unpack_from('<II', b, opt + 112 + 3 * 8)
        secs = [struct.unpack_from('<IIII', b, opt + so + 40 * i + 8) for i in range(ns)]

        def off(rva):
            for vs, va, _rs, rp in secs:
                if va <= rva < va + vs:
                    return rp + rva - va
            return None
        p = off(pd_rva)
        if p is None:
            return
        ents = [struct.unpack_from('<III', b, p + 12 * i) for i in range(pd_size // 12)]

        def root(e, depth=0):
            # chained unwind info (UNW_FLAG_CHAININFO): the entry is a split part of the function its chain names
            u = off(e[2] & ~1) if e[2] else None
            if u is None or depth > 8:
                return e[0]
            flags, count = b[u] >> 3, b[u + 2]
            if not flags & 0x4:
                return e[0]
            c = u + 4 + 2 * ((count + 1) & ~1)
            return root(struct.unpack_from('<III', b, c), depth + 1)
        ents.sort()
        self.begins = [e[0] for e in ents]
        self.ends = [e[1] for e in ents]
        self.roots = [root(e) for e in ents]
        self.starts = sorted(set(self.roots))

    def of(self, rva):
        """Start of the function holding rva (split parts resolve to their parent); rva itself when unknown."""
        if not self.starts:
            return rva
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and rva < self.ends[i]:
            return self.roots[i]
        return rva


def parse_kv(line):
    d = {}
    for tok in line.split()[1:]:
        if '=' in tok:
            k, v = tok.split('=', 1)
            d[k] = v
    return d


def rva_of(tok):
    return None if tok.startswith('m:') else int(tok, 16)


def lcg(s):
    return (s * 0x41C64E6D + 0xBDF) & 0xFFFFFFFF


def int_draw(state_before, lo, hi):
    """0x1ffb40 int(lo, hi): (value, steps, state after) from the state before the call."""
    if lo == hi:
        return lo, 0, state_before
    rng = min(hi - lo, 0x7FFE)
    mask = 1
    while mask <= rng:
        mask <<= 1
    mask -= 1
    s, k = state_before, 0
    while True:
        s = lcg(s)
        k += 1
        v = ((s >> 10) & 0x7FFF) & mask
        if v <= rng:
            return lo + v, k, s


def load(path, funcs):
    hits, events = [], []
    for line in open(path, encoding='ascii', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('HIT '):
            d = parse_kv(line)
            stack = [rva_of(t) for t in d.get('stack', '').split(',') if t]
            fstack = [funcs.of(r) if r is not None else None for r in stack]
            phase = 'OTHER'
            for name, fs in PHASES:
                if any(f in fs for f in fstack if f is not None):
                    phase = name
                    break
            site = None
            for r, f in zip(stack[1:], fstack[1:]):
                if r is None:
                    continue
                if f in PRIMITIVES or r in PRIMITIVES:
                    continue
                site = f
                break
            prim = None
            for r in stack[:2]:
                if r is not None:
                    for p0, name in PRIMITIVES.items():
                        if p0 <= r < p0 + 0x70:
                            prim = name
            h = {'n': int(d['n']), 'tid': d['tid'], 'state': int(d['state'], 16), 'steps': d['steps'],
                 'mt': d.get('mt'), 'st': d.get('st'), 'mode': d.get('mode'), 'mf': d.get('mf'), 'gf': d.get('gf'),
                 'phase': phase, 'site': site, 'prim': prim, 'rip': d.get('rip'), 'stack': stack, 'fstack': fstack}
            if phase == 'OTHER' and site is not None:
                h['phase'] = 'OTHER'
            hits.append(h)
        elif line.split(' ', 1)[0] in ('ATTACH', 'ARM', 'DISARM', 'STOP', 'DETACH', 'EXIT', 'CU8HDBG', 'NO_GAME',
                                       'EMERGENCY_FAIL', 'DRAIN_STEP', 'CTX_FAIL', 'BREAK_FAILED'):
            events.append(line)
    return hits, events


def site_name(f):
    if f is None:
        return '-'
    return '%s(%x)' % (SITES[f], f) if f in SITES else '%x' % f


def runs(hits):
    out = []
    for h in hits:
        key = (h['tid'], h['phase'], h['site'], h['prim'])
        if out and out[-1]['key'] == key:
            r = out[-1]
            r['last'] = h
            r['count'] += 1
            r['steps'] += int(h['steps']) if h['steps'].isdigit() else 0
            r['seed'] = r['seed'] or h['steps'] == 'seed'
        else:
            out.append({'key': key, 'first': h, 'last': h, 'count': 1,
                        'steps': int(h['steps']) if h['steps'].isdigit() else 0, 'seed': h['steps'] == 'seed'})
    return out


def report(path, exe, out_path=None, n_env=4, all_runs=False):
    funcs = Funcs(exe)
    hits, events = load(path, funcs)
    L = []
    L.append('cu8h t01 synced-draw trace report: %s' % path)
    L.append('exe .pdata: %d function starts (%s)' % (len(funcs.starts), exe if funcs.starts else 'NOT READ'))
    L.extend('  ' + e for e in events)
    L.append('hits: %d' % len(hits))
    if not hits:
        text = '\n'.join(L) + '\n'
        if out_path:
            open(out_path, 'w').write(text)
        return text, hits
    rs = runs(hits)
    L.append('')
    L.append('== runs (consecutive hits with the same thread / phase / site / primitive) ==')
    shown = 0
    for r in rs:
        f, la = r['first'], r['last']
        if not all_runs and shown >= 400:
            L.append('  ... %d more runs (--all-runs)' % (len(rs) - shown))
            break
        shown += 1
        L.append('  n=%d-%d tid=%s %-17s site=%-28s prim=%-8s hits=%d steps=%d%s mt=%s st=%s mf=%s gf=%s' % (
            f['n'], la['n'], f['tid'], f['phase'], site_name(f['site']), f['prim'] or '-', r['count'], r['steps'],
            ' SEED' if r['seed'] else '', f['mt'], f['st'], f['mf'], f['gf']))
    # per-phase totals
    L.append('')
    L.append('== phase totals ==')
    tot = {}
    for h in hits:
        t = tot.setdefault(h['phase'], [0, 0, h['n'], h['n']])
        t[0] += 1
        t[1] += int(h['steps']) if h['steps'].isdigit() else 0
        t[3] = h['n']
    for k, (c, s, a, b) in sorted(tot.items(), key=lambda kv: kv[1][2]):
        L.append('  %-17s hits=%d steps=%d first=n%d last=n%d' % (k, c, s, a, b))
    # checks
    L.append('')
    L.append('== checks (CU8G_T02_ENV_PICK_RE sec. 7 P1) ==')
    seeds = [h for h in hits if h['steps'] == 'seed']
    L.append('seed writes: %s' % (', '.join('n%d %s site=%s' % (h['n'], h['phase'], site_name(h['site']))
                                            for h in seeds) or 'none'))
    picks = [h for h in hits if h['phase'] == 'ENV_PICK']
    c102 = [h for h in hits if h['phase'] == 'CHUNK102']
    verdict = []
    if not c102:
        L.append('A: no CHUNK102 hit (no object draws seen in the map load) -> INCONCLUSIVE')
        verdict.append('A?')
    elif not picks:
        L.append('A: CHUNK102 hits n%d..n%d, no ENV_PICK hit' % (c102[0]['n'], c102[-1]['n']))
        verdict.append('A?')
    else:
        early = [p for p in picks if p['n'] < c102[-1]['n']]
        L.append('A: CHUNK102 hits n%d..n%d (%d hits), first ENV_PICK n%d -> %s' % (
            c102[0]['n'], c102[-1]['n'], len(c102), picks[0]['n'], 'OK (pick after every chunk-0x102 draw)'
            if not early else 'DIFFERS (%d pick hits before the last chunk-0x102 hit)' % len(early)))
        verdict.append('A' if not early else 'A!')
    if picks:
        # group pick hits into calls: consecutive hits; one int call can take several LCG steps
        prev_state = None
        for h in hits:
            if h['n'] == picks[0]['n']:
                break
            prev_state = h['state']
        steps = sum(int(p['steps']) for p in picks if p['steps'].isdigit())
        contiguous = picks[-1]['n'] - picks[0]['n'] + 1 == len(picks)
        if prev_state is not None:
            v, k, s_after = int_draw(prev_state, 0, n_env - 1)
            match = s_after == picks[-1]['state'] and k == steps
            L.append('B: ENV_PICK hits=%d steps=%d contiguous=%s; int(0, %d) from the state before = %d (%d step(s)) '
                     '-> %s' % (len(picks), steps, contiguous, n_env - 1, v, k,
                                'OK: one int draw, picked index %d' % v if match and contiguous else
                                'DIFFERS: not one int(0, %d) draw' % (n_env - 1)))
            verdict.append('B' if match and contiguous else 'B!')
        else:
            L.append('B: ENV_PICK is the first hit; no state before it to replay')
            verdict.append('B?')
        post = [h for h in hits if h['phase'] in POST_PHASES]
        early_post = [h for h in post if h['n'] < picks[0]['n']]
        L.append('C: post-load hits=%d (%s), before the pick: %d -> %s' % (
            len(post), ', '.join(sorted({h['phase'] for h in post})) or 'none', len(early_post),
            'OK' if not early_post else 'DIFFERS'))
        verdict.append('C' if not early_post else 'C!')
        load_after = [h for h in hits if h['phase'] in ('CHUNK102',) and h['n'] > picks[-1]['n']]
        if load_after:
            L.append('   note: %d CHUNK102 hits after the pick (another load?) first n%d' % (
                len(load_after), load_after[0]['n']))
    wx = [h for h in hits if h['phase'] == 'WEATHER']
    if wx:
        first_mf = wx[0]['mf']
        same = [h for h in wx if h['mf'] == first_mf and h['mode'] == wx[0]['mode']]
        L.append('D: WEATHER hits=%d; in the first weather frame (mf=%s): %d hit(s), %d step(s), sites %s' % (
            len(wx), first_mf, len(same), sum(int(h['steps']) for h in same if h['steps'].isdigit()),
            ', '.join(sorted({site_name(h['site']) for h in same}))))
    else:
        L.append('D: no WEATHER hit (expected when the picked record has no weather scenario, or space)')
    L.append('VERDICT t01: %s' % ' '.join(verdict))
    text = '\n'.join(L) + '\n'
    if out_path:
        with open(out_path, 'w') as f:
            f.write(text)
    return text, hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--out')
    ap.add_argument('--n-env', type=int, default=4)
    ap.add_argument('--all-runs', action='store_true')
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.trace)), 'report.txt')
    text, _ = report(a.trace, a.exe, out, a.n_env, a.all_runs)
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
