#!/usr/bin/env python3
"""q6_parse.py - report for the Q6 t15 probes from a records.log ("Q6P|<KIND>|p=<probe>|...|END", one per line).

  q6_parse.py <records.log> [--out report.txt]

Sections (numbered as roadmap Q6 t15): 1 EliminationLatch, 2 object-script first pump, 3 Perpetual STORY_ELAPSED
period, 4 load-time story resume, 5 TR land end time, 6 Select All. Each section ends with ANSWER lines and the port's
current behaviour, so the result can be compared directly.
"""
import argparse
import collections
import re
import sys

sys.dont_write_bytecode = True
RECORD_RE = re.compile(r'Q6P\|[A-Z0-9_]+\|[ -~]*?\|END')
FPS = 30.0

PORT = {
    1: ['Port (engine/galactic victory.hpp, [port] EliminationLatch): A goes on; B ends with the human eliminated at the',
        '  first victory check after the script takes Abregado Rae back (t+50 s); C ends at once (the Rebel\'s',
        '  Opponent_Controls_No_Planets passes). Literal original reading (STRATEGIC_GAPS 1.4: a player loses only when',
        '  another player\'s condition passes): A and B go on, C ends at once.'],
    2: ['Port (OBJECT_SCRIPTS 9, object_scripts.cpp): first pump at the first service after the attach, then',
        '  LastService = -(synced range_float(0, ServiceRate)) (hi = first_pump_draw_hi = ServiceRate); a later-created',
        '  object pumps again at the next service. Object scripts are on only with a mod layer ([port interim rule]).'],
    3: ['Port (STORY 8 item 11, test_q6_story_chain): a Perpetual STORY_ELAPSED T fires about every T + one story service;',
        '  T = 2 fires at frames 61 and 122 in the port test (period 61 frames).'],
    4: ['Port (STORY 8 item 9, StorySession::start_threads_at_load): the plot\'s threads are resumed once at load, after',
        '  Base_Definitions and Create_Thread("main"), before the first T9; so MAIN comes before the frame-1 STORY_ELAPSED',
        '  dispatch (TRIG Q6S_Begin0) and the Begin0 state is entered (ENTER Q6S_Begin0).'],
    5: ['Port (alamo_headless TR Blood Gulch, Empire AI vs Rebel AI, object scripts on): the Empire wins at frame 45,143',
        '  (25 min 05 s); the last Rebel command centre self-repairs under fire (AI_TACTICAL 7.7.6).'],
    6: ['Port (sim_host.cpp SelectAll, COMMANDS 2.4.1): every owned SELECTABLE object without a parent, heroes and',
        '  build-pad turrets included, the Eclipse (Not_Really_Selectable) included; base structures (DUMMY_GROUND_STRUCTURE),',
        '  star bases, defense satellites / orbital structures, proximity mines and TEAM members / docked or garrisoned',
        '  units excluded (TEAM containers expand to their members).'],
}


def parse_line(text):
    parts = text.split('|')
    kv = collections.OrderedDict()
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            kv.setdefault(k, v)
    return parts[1], kv


def load(path):
    out = []
    seen = set()
    for line in open(path, encoding='ascii', errors='replace'):
        for m in RECORD_RE.finditer(line):
            t = m.group(0)
            if t not in seen:
                seen.add(t)
                out.append(parse_line(t))
    return out


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def by(recs, probe):
    return [(k, kv) for k, kv in recs if kv.get('p') == probe]


def seq(kv):
    return int(kv.get('n', '0') or 0)


# ---- 1 EliminationLatch ------------------------------------------------------------------------------------------
def sec1(recs):
    L = ['== 1. EliminationLatch (campaigns Q6 PROBE: ELIM A / B / C; plus TR GCMenu by eye) ==']
    runs = collections.OrderedDict()
    for k, kv in by(recs, 'elim'):
        runs.setdefault((kv.get('mode', '?'), kv.get('sid', '?')), []).append((k, kv))
    ans = {}
    if not runs:
        L.append('  no ELIM records (campaigns not played, or the plot never started).')
    for (mode, sid), rs in runs.items():
        rs.sort(key=lambda r: seq(r[1]))
        start = next((kv for k, kv in rs if k == 'START'), None)
        hbs = [kv for k, kv in rs if k == 'HB']
        done = next((kv for k, kv in rs if k == 'DONE'), None)
        give = next((kv for k, kv in rs if k == 'GIVE'), None)
        take = next((kv for k, kv in rs if k == 'TAKE'), None)
        L.append('  run %s (sid %s): %d records, %d heartbeats' % (mode, sid, len(rs), len(hbs)))
        if start:
            L.append('    START t=%s human=%s owned=%s objs=%s owners: %s' % (
                start.get('t'), start.get('human'), start.get('owned'), start.get('objs'),
                ' '.join('%s=%s' % (p, start.get(p, '?')) for p in ('Tatooine', 'Byss', 'Abregado_Rae', 'Jabiim'))))
        for name, r in (('GIVE', give), ('TAKE', take)):
            if r:
                L.append('    %s t=%s ok=%s owned after=%s Abregado_Rae=%s %s' % (
                    name, r.get('t'), r.get('ok'), r.get('owned'), r.get('Abregado_Rae'),
                    ('err=' + r.get('err')) if r.get('ok') != 'true' else ''))
        last = hbs[-1] if hbs else None
        premise = start is not None and start.get('owned') == '0'
        if done:
            v = 'RAN_TO_END'
            L.append('    DONE at dt>=170 s: the campaign did NOT end (owned=%s at the end)' % done.get('owned'))
        elif last:
            v = 'STOPPED'
            L.append('    no DONE: last heartbeat dt=%s s (t=%s). The campaign ended (or was quit) after that;'
                     ' check the note of what the screen showed.' % (last.get('dt'), last.get('t')))
        else:
            v = 'NO_HEARTBEAT'
            L.append('    no heartbeat: the plot thread never ran a loop (campaign ended at once?)')
        if not premise and start is not None:
            L.append('    PREMISE FAILED: the human owned %s planet(s) at START (Home_Location gave a planet?)'
                     % start.get('owned'))
        ans[mode] = v if premise or start is None else v + '_BUT_PREMISE_FAILED'
    L.append('  ANSWER A (own nothing all game): %s' % ans.get('A', 'NOT RUN'))
    L.append('  ANSWER B (owned once, then nothing): %s' % ans.get('B', 'NOT RUN'))
    L.append('  ANSWER C (control, opponent rule): %s' % ans.get('C', 'NOT RUN (no records: expected if it ended at once)'))
    L.append('  Reading: A RAN_TO_END and B RAN_TO_END = the original never eliminates (the latch is harmless for A but the'
             ' port ends B, which the original does not); B STOPPED a few s after TAKE = the original eliminates a player'
             ' that owned something (the latch matches); A STOPPED = the original eliminates even a player that never'
             ' owned anything (TR GCMenu would then end too: compare the TR GCMenu note).')
    L += ['  ' + x for x in PORT[1]]
    return L


# ---- 2 object scripts --------------------------------------------------------------------------------------------
def sec2(recs):
    L = ['== 2. Object-script first pump (skirmish land + space, AI-owned spawned units) ==']
    spawn = {}
    for k, kv in by(recs, 'tact'):
        if k == 'SPAWN' and kv.get('id', '-') != '-':  # object ids restart per battle: key by mode and script too
            spawn[(kv.get('mode'), kv.get('script'), kv['id'])] = (kv.get('wave'), kv.get('type'), num(kv.get('f')),
                                                                  kv.get('mode'))
    inst = collections.OrderedDict()
    for k, kv in by(recs, 'obj'):
        inst.setdefault(kv.get('sid'), []).append((k, kv))
    rows = []
    for sid, rs in inst.items():
        rs.sort(key=lambda r: seq(r[1]))
        att = next((kv for k, kv in rs if k == 'ATTACH'), {})
        pumps = [kv for k, kv in rs if k == 'PUMP']
        script = (att or (rs[0][1])).get('script', '?')
        rate = num(att.get('rate')) if att else (num(pumps[0].get('rate')) if pumps else None)
        shipped = att.get('shipped_rate', '?') if att else '?'
        obj = pumps[0].get('obj') if pumps else '?'
        pmode = pumps[0].get('mode') if pumps else '?'
        wave, otype, fspawn, mode = spawn.get((pmode, script, obj), (None, '?', None, pmode))
        f = [num(p.get('f')) for p in pumps]
        ls = [num(p.get('ls')) for p in pumps]
        t = [num(p.get('t')) for p in pumps]
        r = -ls[1] if len(ls) > 1 and ls[1] is not None and ls[1] <= 0 else None
        p3_ok = (len(ls) > 2 and ls[2] is not None and t[1] is not None and abs(ls[2] - t[1]) < 0.02)
        rows.append({'sid': sid, 'script': script, 'rate': rate, 'shipped': shipped, 'obj': obj, 'wave': wave,
                     'type': otype, 'mode': mode, 'fspawn': fspawn, 'f': f, 'ls': ls, 'r': r, 'p3_ok': p3_ok,
                     'pumps': len(pumps), 'ls1': ls[0] if ls else None, 'ls2': ls[1] if len(ls) > 1 else None})
    if not rows:
        L.append('  no object-script records (no skirmish played, or the scripts never pumped).')
    L.append('  %-30s %-6s %-5s %-8s %-4s %7s %7s %7s %10s %8s %s' % (
        'script', 'rate', 'wave', 'obj', 'mode', 'f_spawn', 'f_p1', 'f_p2', 'r=-LS@p2', 'r/rate', 'pumps / LS@p3=t@p2'))
    for x in rows:
        rr = ('%.6f' % x['r']) if x['r'] is not None else ('LS2=%s' % x['ls2'])
        ratio = ('%.3f' % (x['r'] / x['rate'])) if x['r'] is not None and x['rate'] else '-'
        fs = lambda v: '-' if v is None else '%.0f' % v  # noqa: E731
        L.append('  %-30s %-6s %-5s %-8s %-4s %7s %7s %7s %10s %8s %d / %s' % (
            x['script'][:30], x['rate'], x['wave'] or '-', x['obj'], (x['mode'] or '?')[:4], fs(x['fspawn']),
            fs(x['f'][0] if x['f'] else None), fs(x['f'][1] if len(x['f']) > 1 else None), rr, ratio, x['pumps'],
            'yes' if x['p3_ok'] else '-'))
    groups = collections.OrderedDict()
    for x in rows:
        groups.setdefault((x['script'], x['rate']), []).append(x)
    L.append('  per script:')
    for (s, rate), xs in groups.items():
        rs = [x['r'] for x in xs if x['r'] is not None]
        if rs:
            L.append('    %-30s rate %-6s n=%2d  r in [%.6f, %.6f]  max r/rate %.3f  over rate: %d' % (
                s, rate, len(rs), min(rs), max(rs), max(rs) / rate if rate else 0, sum(1 for v in rs if rate and v > rate + 1e-4)))
        else:
            L.append('    %-30s rate %-6s n=%2d  no negative LastService at pump 2 (values: %s)' % (
                s, rate, len(xs), ','.join(str(x['ls2']) for x in xs[:6])))
    allr = [(x['r'], x['rate']) for x in rows if x['r'] is not None and x['rate']]
    big = [(r, rate) for r, rate in allr if rate >= 6]
    if not allr:
        v = 'NOT_MEASURED (no instance with a negative LastService at pump 2)'
    elif all(r <= rate + 1e-4 for r, rate in allr) and any(r > 1.05 for r, _ in big):
        v = 'hi = ServiceRate (every draw within [0, ServiceRate]; draws above 1 at rate >= 6): port rule confirmed'
    elif all(r <= 1 + 1e-4 for r, _ in allr) and len(big) >= 3:
        v = 'hi = 1 (no draw above 1 even at rate 6 / 20): the port\'s first_pump_draw_hi must be 1, not ServiceRate'
    else:
        v = 'OTHER (see the table: draws above their ServiceRate, or too few samples)'
    L.append('  ANSWER bound: ' + v)
    vanilla = [x for x in rows if x['script'] in ('ObjectScript_PowerToShields', 'ObjectScript_MissileShield')]
    ran = [x for x in vanilla if x['pumps'] >= 2]
    L.append('  ANSWER vanilla objects run their Lua_Script: %s (%d of %d vanilla-rate instances pumped at least twice)' % (
        'YES' if ran else ('NO' if vanilla else 'NOT MEASURED'), len(ran), len(vanilla)))
    late = [x for x in rows if x['wave'] == '2' and len(x['f']) > 1 and x['f'][0] is not None and x['f'][1] is not None]
    if late:
        d = sorted(set(int(x['f'][1] - x['f'][0]) for x in late))
        L.append('  wave 2 (created at frame >= 2700): frames from pump 1 to pump 2 = %s (spec: the very next service)' % d)
    first = [x for x in rows if x['fspawn'] is not None and x['f'] and x['f'][0] is not None]
    if first:
        d = sorted(set(int(x['f'][0] - x['fspawn']) for x in first))
        L.append('  frames from the spawn record to pump 1 = %s (spec: first service after the attach)' % d)
    L += ['  ' + x for x in PORT[2]]
    return L


# ---- 3 / 4 story -------------------------------------------------------------------------------------------------
def story_runs(recs):
    runs = collections.OrderedDict()
    for k, kv in by(recs, 'story'):
        runs.setdefault(kv.get('sid'), []).append((k, kv))
    for rs in runs.values():
        rs.sort(key=lambda r: seq(r[1]))
    return runs


def sec3(recs):
    L = ['== 3. Perpetual STORY_ELAPSED period (campaign Q6 PROBE: STORY TIMING) ==']
    runs = story_runs(recs)
    if not runs:
        L.append('  no story records.')
    expect = {'Q6S_Cycle2': 2.0, 'Q6S_Cycle1': 1.0, 'Q6S_Cycle07': 0.7, 'Q6S_Cycle40': 40.0, 'Q6S_Once2': 2.0}
    answer = None
    for sid, rs in runs.items():
        fires = collections.OrderedDict()
        for k, kv in rs:
            if k == 'TRIG':
                fires.setdefault(kv.get('event'), []).append((num(kv.get('f')), num(kv.get('t'))))
        L.append('  run sid %s:' % sid)
        for ev in ('Q6S_Begin0', 'Q6S_Once2', 'Q6S_Begin3', 'Q6S_Cycle07', 'Q6S_Cycle1', 'Q6S_Cycle2', 'Q6S_Cycle40'):
            fl = fires.get(ev, [])
            if not fl:
                L.append('    %-12s never fired' % ev)
                continue
            fr = [f for f, _ in fl if f is not None]
            per = [int(b - a) for a, b in zip(fr, fr[1:])]
            cnt = collections.Counter(per)
            T = expect.get(ev)
            L.append('    %-12s fires=%-3d first f=%s t=%s  periods (frames) %s%s' % (
                ev, len(fl), '%.0f' % fr[0] if fr else '?', fl[0][1], dict(sorted(cnt.items())),
                ('  T*30=%.0f' % (T * FPS)) if T and ev != 'Q6S_Once2' else ''))
            if ev == 'Q6S_Cycle2':
                L.append('                 first frames: %s' % [int(f) for f in fr[:6]])
                if per:
                    answer = 'Cycle2 period %s frames (%s); first fires %s' % (
                        cnt.most_common(1)[0][0], dict(sorted(cnt.items())), [int(f) for f in fr[:3]])
    L.append('  ANSWER: ' + (answer or 'NOT MEASURED'))
    L.append('  Reading: period = T*30 + 1 (61 for T = 2) = the port rule (accumulation restarts at the service after the'
             ' fire); period = T*30 (60) = the timer does not lose a service; frames relative to Once2 show the offset of'
             ' the first fire.')
    L += ['  ' + x for x in PORT[3]]
    return L


def sec4(recs):
    L = ['== 4. Load-time story resume (order of the first records of the plot script) ==']
    runs = story_runs(recs)
    answer = None
    for sid, rs in runs.items():
        L.append('  run sid %s (record order = execution order inside the plot\'s Lua state):' % sid)
        shown = 0
        for k, kv in rs:
            if k in ('TOP', 'DEFS', 'MAIN', 'PUMP', 'ENTER') or (k == 'TRIG' and kv.get('event') in ('Q6S_Begin0', 'Q6S_Begin3', 'Q6S_Once2')):
                if k == 'TRIG' and kv.get('k') != '1':
                    continue
                if k == 'PUMP' and int(kv.get('k', '99')) > 4:
                    continue
                L.append('    n=%-4s f=%-6s %-6s %s' % (kv.get('n'), kv.get('f'), k, ' '.join(
                    '%s=%s' % (a, b) for a, b in kv.items() if a not in ('p', 'sid', 'n', 'f'))))
                shown += 1
                if shown > 22:
                    break
        main = next((kv for k, kv in rs if k == 'MAIN'), None)
        b0 = next((kv for k, kv in rs if k == 'TRIG' and kv.get('event') == 'Q6S_Begin0'), None)
        e0 = next((kv for k, kv in rs if k == 'ENTER' and kv.get('event') == 'Q6S_Begin0'), None)
        if main and b0:
            if seq(main) < seq(b0):
                answer = ('MAIN (f=%s) ran before the Begin0 fire (f=%s): the threads were resumed before the first ELAPSED'
                          ' dispatch' % (main.get('f'), b0.get('f')))
            else:
                answer = ('Begin0 fired (f=%s, cur=%s next=%s) BEFORE main\'s first resume (f=%s): no load-time resume'
                          % (b0.get('f'), b0.get('cur'), b0.get('next'), main.get('f')))
            answer += '; Begin0 state %s' % ('entered (f=%s)' % e0.get('f') if e0 else 'NEVER entered (event dropped)')
        elif main:
            answer = 'MAIN at f=%s, Begin0 never fired' % main.get('f')
    L.append('  ANSWER: ' + (answer or 'NOT MEASURED'))
    L += ['  ' + x for x in PORT[4]]
    return L


# ---- 5 TR land -----------------------------------------------------------------------------------------------------
def mmss(frames):
    s = frames / FPS
    return '%d:%04.1f' % (int(s // 60), s - 60 * int(s // 60))


def sec5(recs):
    L = ['== 5. TR land end time (Blood Gulch, TR session) ==']
    rs = sorted(by(recs, 'trland'), key=lambda r: (r[1].get('sid'), seq(r[1])))
    if not rs:
        L.append('  no TR land records (TR session not played, or the overlay mod did not load: then use the stopwatch'
                 ' notes).')
        L.append('  ANSWER: NOT MEASURED')
        L += ['  ' + x for x in PORT[5]]
        return L
    for k, kv in rs:
        if k in ('ARM', 'SKIP', 'STRUCTS', 'BASEGONE', 'CCGONE'):
            L.append('  f=%-7s %-8s %s' % (kv.get('f'), k, ' '.join('%s=%s' % (a, b) for a, b in kv.items()
                                                                if a not in ('p', 'sid', 'n', 'f'))))
    gone = [(num(kv.get('f')), kv.get('type')) for k, kv in rs if k == 'GONE']
    L.append('  enemy structures destroyed: %d' % len(gone))
    for f, t in gone:
        L.append('    f=%-7.0f (%s) %s' % (f, mmss(f), t))
    samples = [kv for k, kv in rs if k == 'S']
    base = next((num(kv.get('f')) for k, kv in rs if k == 'BASEGONE'), None)
    ccg = next((num(kv.get('f')) for k, kv in rs if k == 'CCGONE'), None)
    last = num(samples[-1].get('f')) if samples else None
    if base is not None:
        L.append('  CC samples after the base was gone (every ~30 s):')
        nxt = base
        for kv in samples:
            f = num(kv.get('f'))
            if f is not None and f >= nxt:
                nxt = f + 900
                L.append('    f=%-7.0f (%s) %s' % (f, mmss(f), kv.get('cc1', '-')))
    if base is not None and ccg is not None:
        ans = 'base gone at f=%.0f (%s), last CC gone at f=%.0f (%s): the CC survived %.0f frames (%s)' % (
            base, mmss(base), ccg, mmss(ccg), ccg - base, mmss(ccg - base))
    elif base is not None:
        ans = 'base gone at f=%.0f (%s); the CC was still alive at the last sample f=%s (%s)' % (
            base, mmss(base), '%.0f' % last if last else '?', mmss(last) if last else '?')
    else:
        ans = 'the base was never reduced to the CC (last sample f=%s)' % ('%.0f' % last if last else '?')
    L.append('  ANSWER: ' + ans)
    L.append('  Sample resolution: the hook runs only inside TR\'s own free-store calls (On_Unit_Service every 2 s per idle'
             ' unit, FreeStoreService every 20 s), so frames are "at or before, within 2-20 s".')
    L += ['  ' + x for x in PORT[5]]
    return L


# ---- 6 Select All ----------------------------------------------------------------------------------------------------
def doc_expect(tname):
    t = tname.upper()
    if t.startswith('E_GROUND_') or t.startswith('R_GROUND_') or t.startswith('U_GROUND_') or 'STAR_BASE' in t:
        return 'OUT (base structure / star base)'
    if 'DEFENSE_SATELLITE' in t and not t.startswith('UC_'):
        return 'OUT (orbital structure)'
    if 'PROXIMITY_MINE' in t or 'REMOTE_BOMB' in t:
        return 'OUT (mine)'
    return 'IN unless it has a parent (TEAM member / docked / garrisoned)'


def sec6(recs):
    L = ['== 6. Select All look (Ctrl+A in the land and the space skirmish) ==']
    rs = by(recs, 'tact')
    hs = [kv for k, kv in rs if k == 'HSPAWN']
    if not hs:
        L.append('  no HSPAWN records (skirmish not played).')
    for kv in hs:
        L.append('  %-5s spawned for you: %-32s id=%-6s how=%-10s spec %s' % (
            kv.get('mode'), kv.get('type'), kv.get('id'), kv.get('how'), kv.get('expect')))
    owns = collections.OrderedDict()
    for k, kv in rs:
        if k == 'OWN':
            owns.setdefault((kv.get('mode'), kv.get('tag')), []).extend(x for x in kv.get('list', '').split(';') if x)
    for (mode, tag), items in owns.items():
        L.append('  %s, %s: you own %d objects (type:id:parent type):' % (mode, tag, len(items)))
        for it in items:
            parts = it.split(':')
            tn = parts[0]
            parent = parts[2] if len(parts) > 2 else '-'
            exp = doc_expect(tn) if parent == '-' else 'OUT (has parent %s; its container is selected instead)' % parent
            L.append('    %-42s -> spec %s' % (it, exp))
    L.append('  ANSWER: from the Ctrl+A screenshots (no Lua query reads a selection). Compare each object above with the'
             ' selection brackets / the selected-units panel in the screenshots.')
    L += ['  ' + x for x in PORT[6]]
    return L


def report(path, out=None):
    recs = load(path)
    L = ['Q6 t15 probes report (%d records from %s)' % (len(recs), path), '']
    kinds = collections.Counter((kv.get('p'), k) for k, kv in recs)
    L.append('records per probe/kind: ' + ', '.join('%s/%s=%d' % (p, k, n) for (p, k), n in sorted(kinds.items(),
                                                                                                 key=lambda x: str(x))))
    for f in (sec1, sec2, sec3, sec4, sec5, sec6):
        L.append('')
        L += f(recs)
    text = '\n'.join(L) + '\n'
    if out:
        with open(out, 'w') as fh:
            fh.write(text)
    return text, recs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('records')
    ap.add_argument('--out')
    a = ap.parse_args()
    text, _ = report(a.records, a.out)
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
