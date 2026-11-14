#!/usr/bin/env python3
"""cu8h_sites.py - call-site counts of a cu8h_dbg.py trace (storyb t07 / t08, cu8k t05).

  cu8h_sites.py TRACE.log [--exe <game>\\corruption\\StarWarsG.exe] [--out sites.txt] [--mode PTR]
                [--windows 0,1,2,3-29,30-128,129-255,256-1800,1801-18000] [--table 256-18000 --table 1801-18000]
                [--load-frames 2] [--chain 4] [--top 40] [--fps 30] [--quiet]

Every HIT line of the trace is one write of the synced LCG state; `steps` is the number of LCG steps it covers (one
draw = one step, except inlined code that keeps the state in a register). The call site of a hit is the first stack
entry that is not inside a draw primitive (int 0x1ffb40, float 0x1ffbb0, 0x532480, 0x5324d0, percent 0x1ffdb0): the
return address of the call into the primitive, or the PC itself for an inlined draw. Its function comes from the
exe's .pdata (cu8h_dbg_parse.Funcs), and its callers (the next --chain return addresses) tell call paths apart.

Sections of the report:
  1. modes: every game mode seen (pointer, type, stack, mode frames, hits / steps), and the battle mode (the
     tactical mode with the highest mode frame that was not current at the first hit, or --mode);
  2. seed to battle: from the last seed write before the battle mode appears, one row per run of hits with the same
     thread / mode / mode frame (cumulative steps since the seed, so the q2 reader's sample boundaries - e.g. 412 at
     battle frame 0, +919 by frame 1, +111 by frame 2 in STORY_T04_KESSEL_RUN - can be matched), up to battle mode
     frame --load-frames; then the load hits by load phase (cu8h_dbg_parse PHASES) and call site;
  3. battle windows: steps, hits and steps per frame of the battle mode per --windows range;
  4. call sites per --table range (battle mode only): steps, hits, steps per frame, frames with a draw, the commonest
     gaps between those frames, first / last mode frame, and the caller chains;
  5. formation-ctor draws (MOVEMENT sec. 4.7.6: stacks whose 2nd / 3rd entries are 40d11f, 4f6fd4): one row per
     draw with its creator (43e712 land composition, 5d5c19 space composition, 4f738a degenerate, 4f8518 split,
     4f7708 load) and the adopt caller return found deeper in the stack (sec. 4.7.2), then counts per pair.
"""
import argparse
import collections
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cu8h_dbg_parse as P  # noqa: E402

DEFAULT_WINDOWS = '0,1,2,3-29,30-128,129-255,256-1800,1801-18000'
DEFAULT_TABLES = ('256-18000', '1801-18000')
PRIM_SPAN = 0x70
CTOR_RET, FACTORY_RET = 0x40d11f, 0x4f6fd4
CREATORS = {0x43e712: 'land_composition', 0x5d5c19: 'space_composition', 0x4f738a: 'degenerate', 0x4f8518: 'split',
            0x4f7708: 'load'}
# MOVEMENT sec. 4.7.2 / 4.7.3: return addresses into the callers of the adopt 0x39bcb0 and of the team spawn helper
ADOPT_RETURNS = {0x2a1bcd: 'campaign_spawn_member', 0x2a61d5: 'team_helper_container', 0x37a369: 'garrison_leave',
                 0x38a96c: 'ability_activation', 0x395421: 'gom_postload', 0x3a1f81: 'deploy', 0x3a8e9a: 'lock_counter',
                 0x3abdf8: 'container_eject', 0x4265ce: 'stop_movement', 0x48cecb: 'special_ability',
                 0x4fd0a3: 'fleet_place_capital', 0x4fdb3d: 'fleet_place_lead', 0x4fe59d: 'fleet_place_screen',
                 0x4fefc5: 'fleet_place_transport', 0x533017: 'space_retreat', 0x5c9e62: 'fighter_hyperspace_finish',
                 0x63107f: 'landing'}
HELPER_RETURNS = {0x29ecb5: 'skirmish_company_placer', 0x2a1b51: 'campaign_spawn', 0x2a5a02: 'garrison_grouping',
                  0x2bf743: 'reinforcements', 0x2bfc2f: 'reinforcements_b', 0x3a2c80: 'replenish_wingmen',
                  0x5320d4: 'spawn_indigenous', 0x5940be: 'hangar_spawn_squadron', 0x595cf9: 'hangar_595a30'}


def is_prim(r):
    return r is not None and any(p <= r < p + PRIM_SPAN for p in P.PRIMITIVES)


def prim_name(r):
    for p, name in P.PRIMITIVES.items():
        if r is not None and p <= r < p + PRIM_SPAN:
            return name
    return 'inline'


def nsteps(h):
    return int(h['steps']) if h['steps'].isdigit() else 0


def num(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def parse_ranges(spec):
    out = []
    for part in spec.split(','):
        part = part.strip()
        if not part:
            continue
        if '-' in part:
            a, b = part.split('-', 1)
            out.append((int(a), int(b)))
        else:
            out.append((int(part), int(part)))
    return out


class Site:
    def __init__(self, funcs):
        self.funcs = funcs

    def fname(self, r):
        if r is None:
            return 'm:?'
        f = self.funcs.of(r)
        return P.site_name(f) if f in P.SITES else '%x' % f

    def of(self, h, chain):
        """(call site RVA, primitive name, caller chain as a tuple of RVAs)."""
        st = h['stack']
        i = 0
        while i < len(st) and is_prim(st[i]):
            i += 1
        cs = st[i] if i < len(st) else None
        return cs, prim_name(st[0]) if st else '-', tuple(st[i + 1:i + 1 + chain])

    def label(self, cs):
        if cs is None:
            return '-'
        return '%x in %s' % (cs, self.fname(cs))


def battle_mode(hits, forced=None):
    if forced:
        return forced.lower().replace('0x', '')
    first_mode = hits[0]['mode'] if hits else None
    best = None
    for h in hits:
        if h['mt'] in ('1', '2') and h['mode'] != first_mode:
            mf = num(h['mf'])
            if mf is not None and (best is None or mf > best[0]):
                best = (mf, h['mode'])
    if best is None and hits and hits[0]['mt'] in ('1', '2'):
        return first_mode     # armed inside a battle (a move trace): that battle
    return best[1] if best else None


def report(trace, exe, out_path=None, mode=None, windows=DEFAULT_WINDOWS, tables=DEFAULT_TABLES, load_frames=2,
           chain=4, top=40, fps=30):
    funcs = P.Funcs(exe)
    hits, events = P.load(trace, funcs)
    S = Site(funcs)
    L = ['cu8h call-site report: %s' % trace,
         'exe .pdata: %d function starts (%s)' % (len(funcs.starts), exe if funcs.starts else 'NOT READ: RVAs only'),
         'hits: %d, steps: %d, seed writes: %d' % (len(hits), sum(nsteps(h) for h in hits),
                                                  sum(1 for h in hits if h['steps'] == 'seed'))]
    L.extend('  ' + e for e in events if not e.startswith('CU8HDBG'))
    if not hits:
        return finish(L, out_path)
    bm = battle_mode(hits, mode)
    # 1. modes
    L += ['', '== 1. modes (in order of first hit) ==']
    modes = collections.OrderedDict()
    for h in hits:
        m = modes.setdefault(h['mode'], {'mt': set(), 'st': set(), 'mf0': None, 'mf1': None, 'hits': 0, 'steps': 0,
                                         'n0': h['n'], 'n1': h['n']})
        m['mt'].add(h['mt'])
        m['st'].add(h['st'])
        mf = num(h['mf'])
        if mf is not None:
            m['mf0'] = mf if m['mf0'] is None else min(m['mf0'], mf)
            m['mf1'] = mf if m['mf1'] is None else max(m['mf1'], mf)
        m['hits'] += 1
        m['steps'] += nsteps(h)
        m['n1'] = h['n']
    tags = {}
    for i, (ptr, m) in enumerate(modes.items()):
        tags[ptr] = 'M%d' % i
        L.append('  M%d mode=%s mt=%s st=%s mf=%s..%s hits=%d steps=%d n%d..n%d%s' % (
            i, ptr, '/'.join(sorted(m['mt'])), '/'.join(sorted(m['st'])), m['mf0'], m['mf1'], m['hits'], m['steps'],
            m['n0'], m['n1'], '   <- battle mode' if ptr == bm else ''))
    if bm is None:
        L.append('  no battle mode found (no tactical mode other than the one current at the first hit)')
    # 2. seed to battle
    L += ['', '== 2. seed to battle (load split; rows = runs of hits with the same thread / mode / mode frame) ==']
    first_bm = next((i for i, h in enumerate(hits) if h['mode'] == bm), None) if bm else None
    seeds = [i for i, h in enumerate(hits) if h['steps'] == 'seed' and (first_bm is None or i <= first_bm)]
    start = seeds[-1] if seeds else 0
    L.append('  from n%d (%s)' % (hits[start]['n'], 'the last seed write before the battle mode' if seeds else
                                  'the first hit: no seed write seen'))
    end = len(hits)
    if bm:
        for i in range(start, len(hits)):
            h = hits[i]
            if h['mode'] == bm and num(h['mf']) is not None and num(h['mf']) > load_frames:
                end = i
                break
    cum = 0
    rows = []
    for h in hits[start:end]:
        key = (h['tid'], h['mode'], h['mf'])
        if rows and rows[-1]['key'] == key:
            r = rows[-1]
        else:
            r = {'key': key, 'h': h, 'hits': 0, 'steps': 0, 'phases': collections.Counter(), 'sites': collections.Counter()}
            rows.append(r)
        s = nsteps(h)
        cum += s
        r['hits'] += 1
        r['steps'] += s
        r['cum'] = cum
        r['last'] = h['n']
        r['phases'][h['phase']] += s or 1
        r['sites'][S.of(h, chain)[0]] += s or 1
    for r in rows:
        h = r['h']
        L.append('  n%d-%d tid=%s %s mt=%s st=%s mf=%s gf=%s hits=%d steps=%d cum=%d phases=%s sites=%s' % (
            h['n'], r['last'], h['tid'], tags.get(h['mode'], '?'), h['mt'], h['st'], h['mf'], h['gf'], r['hits'],
            r['steps'], r['cum'], ','.join('%s:%d' % kv for kv in r['phases'].most_common(4)),
            ','.join('%s:%d' % (S.fname(k), v) for k, v in r['sites'].most_common(3))))
    L += ['', '  load hits (seed .. battle mode frame %d) by phase and call site:' % load_frames]
    agg = collections.Counter()
    hc = collections.Counter()
    for h in hits[start:end]:
        cs, pr, ch = S.of(h, chain)
        k = (h['phase'], cs, pr, ch[:2])
        agg[k] += nsteps(h)
        hc[k] += 1
    for (ph, cs, pr, ch), s in sorted(agg.items(), key=lambda kv: -kv[1])[:top * 2]:
        L.append('    %-17s steps=%-6d hits=%-6d %-8s site=%-34s callers=%s' % (
            ph, s, hc[(ph, cs, pr, ch)], pr, S.label(cs), ' < '.join(S.label(c) for c in ch)))
    # 3. windows
    L += ['', '== 3. battle windows (battle mode %s, by mode frame) ==' % (tags.get(bm, '-'))]
    bh = [h for h in hits if h['mode'] == bm and num(h['mf']) is not None] if bm else []
    for a, b in parse_ranges(windows):
        w = [h for h in bh if a <= num(h['mf']) <= b]
        frames = (min(b, max(num(h['mf']) for h in bh)) - a + 1) if bh and w else 0
        st = sum(nsteps(h) for h in w)
        L.append('  mf %5d-%-5d steps=%-7d hits=%-7d frames=%-6d steps/frame=%s' % (
            a, b, st, len(w), frames, ('%.3f' % (st / frames)) if frames > 0 else '-'))
    # 4. call-site tables
    for spec in tables:
        a, b = parse_ranges(spec)[0]
        w = [h for h in bh if a <= num(h['mf']) <= b]
        if not w:
            L += ['', '== 4. call sites, battle mf %d-%d: no hits ==' % (a, b)]
            continue
        last = max(num(h['mf']) for h in w)
        nfr = min(b, last) - a + 1
        tot = sum(nsteps(h) for h in w)
        L += ['', '== 4. call sites, battle mf %d-%d (%d frames recorded up to mf %d; %d steps = %.3f per frame) ==' % (
            a, b, nfr, last, tot, tot / max(nfr, 1))]
        sites = {}
        for h in w:
            cs, pr, ch = S.of(h, chain)
            d = sites.setdefault(cs, {'steps': 0, 'hits': 0, 'frames': collections.Counter(), 'prim': collections.Counter(),
                                      'chains': collections.Counter(), 'phase': collections.Counter(), 'tid': set()})
            s = nsteps(h)
            d['steps'] += s
            d['hits'] += 1
            d['frames'][num(h['mf'])] += s
            d['prim'][pr] += 1
            d['chains'][ch] += s or 1
            d['phase'][h['phase']] += 1
            d['tid'].add(h['tid'])
        for cs, d in sorted(sites.items(), key=lambda kv: -kv[1]['steps'])[:top]:
            fr = sorted(d['frames'])
            gaps = collections.Counter(y - x for x, y in zip(fr, fr[1:]))
            per = collections.Counter(d['frames'].values())
            L.append('  %-38s steps=%-7d (%.3f/frame, %4.1f%%) hits=%-7d frames=%-6d mf %d..%d prim=%s phase=%s tid=%s' % (
                S.label(cs), d['steps'], d['steps'] / max(nfr, 1), 100.0 * d['steps'] / max(tot, 1), d['hits'], len(fr),
                fr[0], fr[-1], ','.join(d['prim']), ','.join(d['phase']), ','.join(sorted(d['tid']))))
            L.append('      frame gaps (gap:count) %s; steps per drawing frame %s' % (
                ' '.join('%d:%d' % kv for kv in gaps.most_common(5)) or '-',
                ' '.join('%d:%d' % kv for kv in sorted(per.most_common(4)))))
            for ch, s in d['chains'].most_common(4):
                L.append('      %6d  < %s' % (s, ' < '.join(S.label(c) for c in ch) or '-'))
        if len(sites) > top:
            L.append('  ... %d more call sites (--top)' % (len(sites) - top))
    # 5. formation-ctor draws
    L += ['', '== 5. formation-ctor draws (stack ..., 40d11f, 4f6fd4, creator, ...; MOVEMENT sec. 4.7.6) ==']
    # the ctor draws int(1, FPS): one LCG step per try, value ((state >> 10) & 0x7fff) & mask, rejected when > FPS - 1;
    # a rejected try continues the same draw, so hits are grouped into draws by replaying that test
    rng = fps - 1
    mask = 1
    while mask <= rng:
        mask <<= 1
    mask -= 1
    draws = []
    open_draw = False
    for h in hits:
        st = h['stack']
        if len(st) >= 4 and st[1] == CTOR_RET and st[2] == FACTORY_RET:
            accepted = h['steps'] != '1' or ((h['state'] >> 10) & 0x7FFF) & mask <= rng
            if open_draw and draws and draws[-1]['stack'] == st and draws[-1]['last'] == h['n'] - 1:
                draws[-1]['steps'] += nsteps(h)
                draws[-1]['last'] = h['n']
                open_draw = not accepted
                continue
            open_draw = not accepted
            creator = CREATORS.get(st[3], '%x' % st[3] if st[3] is not None else '?')
            adopt = next(('%s(%x)' % (ADOPT_RETURNS[r], r) for r in st[4:] if r in ADOPT_RETURNS), '-')
            helper = next(('%s(%x)' % (HELPER_RETURNS[r], r) for r in st[4:] if r in HELPER_RETURNS), '-')
            draws.append({'n': h['n'], 'last': h['n'], 'mf': h['mf'], 'gf': h['gf'], 'mode': tags.get(h['mode'], '?'),
                          'stack': st, 'steps': nsteps(h), 'creator': creator, 'adopt': adopt, 'helper': helper,
                          'tid': h['tid']})
    for d in draws:
        L.append('  n%d %s mf=%s gf=%s tid=%s steps=%d creator=%s adopt=%s helper=%s after_factory=%s' % (
            d['n'], d['mode'], d['mf'], d['gf'], d['tid'], d['steps'], d['creator'], d['adopt'], d['helper'],
            ','.join('%x' % r if r is not None else 'm' for r in d['stack'][3:8])))
    cnt = collections.Counter((d['mode'], d['creator'], d['adopt'], d['helper']) for d in draws)
    L.append('  totals (draws, not LCG steps; a draw = the hits up to the first accepted int(1, %d) try):' % fps)
    for (m, c, a, hp), k in sorted(cnt.items(), key=lambda kv: -kv[1]):
        L.append('    %5d  %s creator=%s adopt=%s helper=%s' % (k, m, c, a, hp))
    if not draws:
        L.append('    none')
    return finish(L, out_path)


def finish(L, out_path):
    text = '\n'.join(L) + '\n'
    if out_path:
        with open(out_path, 'w') as f:
            f.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--exe', default=P.DEFAULT_EXE)
    ap.add_argument('--out')
    ap.add_argument('--mode')
    ap.add_argument('--windows', default=DEFAULT_WINDOWS)
    ap.add_argument('--table', action='append')
    ap.add_argument('--load-frames', type=int, default=2)
    ap.add_argument('--chain', type=int, default=4)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--fps', type=int, default=30, help='the formation ctor draws int(1, FPS) (section 5)')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.trace)), 'sites.txt')
    text = report(a.trace, a.exe, out, a.mode, a.windows, tuple(a.table or DEFAULT_TABLES), a.load_frames, a.chain,
                  a.top, a.fps)
    if not a.quiet:
        print(text)
    print('sites report:', out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
