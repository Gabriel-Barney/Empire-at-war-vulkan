#!/usr/bin/env python3
"""q2lua_reader.py - collects the Q2 Lua probe's records from a running FoC StarWarsG.exe (Steam x64 1.121).

  q2lua_reader.py run --run-dir DIR [--wait-process-sec 180] [--scan-sec 10] [--allow-unknown-exe]
  q2lua_reader.py summarize --run-dir DIR       rebuild lua_records.txt / lua_perception.tsv / lua_summary.json
  q2lua_reader.py selftest                      no game needed; ends with SELFTEST_Q2LUA OK / FAILED

Why: retail _CustomScriptMessage writes no file (docs/re/LUA_SCRIPT_OUTPUT.md), so the probe scripts
(mod\\Q2Trace: Q2_Trace_Galactic.lua, the GameScoring.lua hook and the Scripts\\AI plan hooks) keep every line as a
Lua string "Q2L1|<src>|<sid>|<seq>|<KIND>|<text>|END" (src G = galactic plot, T = GameScoring, P = AI plan hooks;
sid = the script instance).
This reader scans the game's committed private memory for them every --scan-sec seconds with
tools/probe_mod/a1_tactical/a1mem.py: OpenProcess + ReadProcessMemory only, never writes, injects or suspends.
It ends when the game exits (the Q2 trace reader closes the game at the end of the trace).

Files written into the run dir:
  lua_reader.log       REC lines in the order found, SCAN stats, EXIT line
  lua_records.txt      every distinct record, sorted by (src, sid, seq)
  lua_perception.tsv   galactic PERCEPTION records: cycle, ai, planet, one column per equation
  lua_summary.json     counts per kind, canary calls, errors, verdict per script, seq gaps per script instance
                       (a gap means records were lost: the 2026-11-03 run lost plan records to PGBase Flush_G), and
                       `plans`: per 'player plan tf' the starts, stagings, blocks, first frame, targets, stages and the
                       unit types its task force held (which plan built what)
"""
import argparse
import json
import os
import re
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))

NEEDLE = b'Q2L1|'
# a1mem.Proc.scan returns 400 bytes per hit: the scripts cap the text at 320 characters
RECORD_RE = re.compile(rb'Q2L1\|([GTP])\|([0-9A-Fa-fx]{1,20})\|([0-9]{1,6})\|([A-Z]{1,12})\|([ -~]{0,330}?)\|END')
SCRIPTS = {'G': 'galactic plot (Q2_Trace_Galactic.lua)', 'T': 'GameScoring hook',
           'P': 'AI plan hooks (Scripts\\AI: Build_Space_Forces, Build_Ground_Forces, Conquer_Pirate, Conquer_Opponent, '
                'Crush)'}


def parse_hits(hits):
    """Scan hits [(addr, bytes)] -> list of (src, sid, seq, kind, text, record) for every full record."""
    out = []
    for _addr, tail in hits:
        m = RECORD_RE.match(tail)
        if m:
            src, sid, seq, kind, text = (g.decode('ascii') for g in m.groups())
            out.append((src, sid, int(seq), kind, text, m.group(0).decode('ascii')))
    return out


def kv(text):
    """'cycle=3 ai=Rebel Q2_X=1.5' -> {'cycle': '3', 'ai': 'Rebel', 'Q2_X': '1.5'} (values hold no spaces)."""
    out = {}
    for m in re.finditer(r'(?:^| )([A-Za-z_][A-Za-z0-9_]*)=(\S*)', text):
        out[m.group(1)] = m.group(2)
    return out


def gaps(uniq):
    """Missing seq numbers per (src, sid) between 1 and the highest seq found: ['P 0x1 2-8', ...]."""
    seqs = {}
    for src, sid, seq, _kind, _text in uniq:
        seqs.setdefault((src, sid), set()).add(seq)
    out = []
    for (src, sid), have in sorted(seqs.items()):
        missing = [s for s in range(1, max(have) + 1) if s not in have]
        runs = []
        for s in missing:
            if runs and runs[-1][1] == s - 1:
                runs[-1][1] = s
            else:
                runs.append([s, s])
        if runs:
            out.append('%s %s %s' % (src, sid, ','.join('%d' % a if a == b else '%d-%d' % (a, b) for a, b in runs)))
    return out


def plan_table(uniq):
    """Plan hook records per 'player plan tf': starts, stagings, blocks, first frame, targets and stages seen, and per
    unit type the most of it in one PBLOCK's unit list (what the task force held). Answers which plan built what."""
    out = {}
    for src, _sid, _seq, kind, text in uniq:
        if src != 'P' or kind not in ('PSTART', 'PSTAGE', 'PBLOCK'):
            continue
        d = kv(text)
        e = out.setdefault('%s %s %s' % (d.get('player', '?'), d.get('plan', '?'), d.get('tf', '?')), {
            'starts': 0, 'stagings': 0, 'blocks': 0, 'first_frame': None, 'targets': [], 'stages': [], 'units': {}})
        e[{'PSTART': 'starts', 'PSTAGE': 'stagings', 'PBLOCK': 'blocks'}[kind]] += 1
        if d.get('frame', '?') != '?':
            try:
                f = int(float(d['frame']))
                e['first_frame'] = f if e['first_frame'] is None else min(e['first_frame'], f)
            except ValueError:
                pass
        if d.get('target') and d['target'] not in e['targets']:
            e['targets'].append(d['target'])
        if kind == 'PSTAGE' and d.get('stage') and d['stage'] not in e['stages']:
            e['stages'].append(d['stage'])
        i = text.find(' units=')
        if kind == 'PBLOCK' and i >= 0:
            have = {}
            for t in text[i + 7:].split()[1:]:
                have[t] = have.get(t, 0) + 1
            for t, n in have.items():
                e['units'][t] = max(e['units'].get(t, 0), n)
    return out


def summarize(records):
    """records: iterable of (src, sid, seq, kind, text) -> (sorted list, perception rows, equations, summary dict)."""
    uniq = sorted(set(records), key=lambda r: (r[0], r[1], r[2]))
    kinds = {}
    for src, _sid, _seq, kind, _text in uniq:
        kinds.setdefault(src, {}).setdefault(kind, 0)
        kinds[src][kind] += 1
    eqs = []
    rows = []
    for src, sid, seq, kind, text in uniq:
        if src != 'G' or kind != 'PERCEPTION':
            continue
        d = kv(text)
        for k in d:
            if k not in ('cycle', 'ai', 'planet') and k not in eqs:
                eqs.append(k)
        rows.append((sid, seq, d))
    summary = {'records': len(uniq), 'kinds': kinds, 'scripts': {}, 'errors': [], 'canary': [], 'gaps': gaps(uniq),
               'plans': plan_table(uniq)}
    for src, _sid, _seq, kind, text in uniq:
        if kind == 'ERR':
            summary['errors'].append('%s %s' % (src, text))
        elif kind == 'CANARY':
            summary['canary'].append('%s %s' % (src, text))
    for src, name in SCRIPTS.items():
        k = kinds.get(src, {})
        if not k:
            verdict = 'no records (script not loaded, or records not found in memory)'
        elif src == 'G':
            if k.get('PERCEPTION'):
                verdict = 'ran: %d cycles, %d perception rows' % (k.get('CYCLE', 0), k['PERCEPTION'])
            elif k.get('ENTER') or k.get('CYCLE'):
                verdict = 'ran, but no perception rows (see errors)'
            else:
                verdict = 'loaded (LOAD/DEF), but the state machine never entered Q2_State_Trace'
        elif src == 'P':
            verdict = ('loaded: %d plan scripts; %d thread starts, %d ends, %d production failures; '
                       '%d stagings, %d blocks done, %d script exits, %d probe errors') % (
                k.get('LOAD', 0), k.get('PSTART', 0), k.get('PDONE', 0), k.get('PFAIL', 0),
                k.get('PSTAGE', 0), k.get('PBLOCK', 0), k.get('PEXIT', 0), k.get('ERR', 0))
        else:
            sampled = k.get('TSAMPLE', 0)
            verdict = 'loaded; %d samples, %d mode events, %d kills, %d built; service=%s thread=%s' % (
                sampled, k.get('TMODE', 0), k.get('TKILL', 0), k.get('TBUILT', 0),
                'yes' if k.get('SERVICE') else 'no', 'yes' if k.get('THREAD') else 'no')
        summary['scripts'][src] = {'name': name, 'verdict': verdict}
    return uniq, rows, eqs, summary


def write_outputs(run_dir, records):
    uniq, rows, eqs, summary = summarize(records)
    with open(os.path.join(run_dir, 'lua_records.txt'), 'w', encoding='ascii', newline='\n') as f:
        f.write('Q2LUARECORDS 1\n')
        for src, sid, seq, kind, text in uniq:
            f.write('%s %s %d %s %s\n' % (src, sid, seq, kind, text))
    with open(os.path.join(run_dir, 'lua_perception.tsv'), 'w', encoding='ascii', newline='\n') as f:
        f.write('\t'.join(['sid', 'seq', 'cycle', 'ai', 'planet'] + eqs) + '\n')
        for sid, seq, d in rows:
            f.write('\t'.join([sid, str(seq), d.get('cycle', ''), d.get('ai', ''), d.get('planet', '')] +
                              [d.get(e, '') for e in eqs]) + '\n')
    with open(os.path.join(run_dir, 'lua_summary.json'), 'w', encoding='ascii', newline='\n') as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write('\n')
    return summary


def read_log_records(log_path):
    recs = []
    if not os.path.isfile(log_path):
        return recs
    with open(log_path, encoding='ascii', errors='replace') as f:
        for line in f:
            i = line.find(' rec=')
            if i < 0:
                continue
            for src, sid, seq, kind, text, _rec in parse_hits([(0, line[i + 5:].rstrip('\n').encode('ascii', 'replace'))]):
                recs.append((src, sid, seq, kind, text))
    return recs


class Log:
    def __init__(self, path):
        self.f = open(path, 'a', encoding='ascii', errors='replace', newline='\n')
        self.lock = threading.Lock()
        self.t0 = time.time()

    def __call__(self, s):
        with self.lock:
            self.f.write('%8.2f %s\n' % (time.time() - self.t0, s))
            self.f.flush()


def run(a):
    import a1mem  # noqa: E402  (ctypes, Windows only)
    log_path = os.path.join(a.run_dir, 'lua_reader.log')
    log = Log(log_path)
    log('Q2LUAREADER 1 scan_sec=%s' % a.scan_sec)
    t0 = time.time()
    p = None
    while p is None and time.time() - t0 < a.wait_process_sec:
        for pid in a1mem.find_pids():
            try:
                q = a1mem.Proc(pid)
            except OSError as e:
                log('OPEN_FAILED pid=%d error=%s' % (pid, e))
                continue
            if q.pe_identity() or a.allow_unknown_exe:
                p = q
                break
            q.close()
        if p is None:
            time.sleep(1.0)
    if p is None:
        log('EXIT reason=no_game scans=0 records=0')
        write_outputs(a.run_dir, [])
        return 2
    log('GAME pid=%d base=%s identity=%s' % (p.pid, hex(p.base), 'ok' if p.pe_identity() else 'unknown'))
    seen = set()
    records = []
    scans = 0
    reason = 'game_exited'
    try:
        while True:
            alive = p.alive()
            ts = time.time()
            try:
                hits, st = p.scan([NEEDLE])
            except OSError as e:
                log('SCAN_ERROR %s' % e)
                hits, st = {NEEDLE: []}, {}
            scans += 1
            new = 0
            for src, sid, seq, kind, text, rec in parse_hits(hits[NEEDLE]):
                if rec in seen:
                    continue
                seen.add(rec)
                records.append((src, sid, seq, kind, text))
                new += 1
                log('REC rec=%s' % rec)
            if scans % 10 == 1 or new:
                log('SCAN n=%d mb=%.0f sec=%.2f new=%d total=%d' % (
                    scans, st.get('bytes', 0) / 1e6, time.time() - ts, new, len(records)))
            if not alive:
                break
            # wait in short steps so the last scan follows the game's exit closely
            end = time.time() + a.scan_sec
            while time.time() < end and p.alive():
                time.sleep(0.25)
    except KeyboardInterrupt:
        reason = 'interrupted'
    log('EXIT reason=%s scans=%d records=%d' % (reason, scans, len(records)))
    s = write_outputs(a.run_dir, records)
    for src, v in sorted(s['scripts'].items()):
        log('VERDICT %s %s' % (src, v['verdict']))
    log('GAPS %s' % ('; '.join(s['gaps']) if s['gaps'] else 'none'))
    return 0


def selftest(_a):
    fails = []

    def check(cond, what):
        print(('  ok    ' if cond else '  FAIL  ') + what)
        if not cond:
            fails.append(what)

    rec = b'Q2L1|G|0x1a2b|7|PERCEPTION|cycle=3 ai=Rebel planet=Nal_Hutta Q2_Space_Total=12.5 Q2_Target_Player=1|END'
    hits = [(0x1000, rec + b'\x00garbage'), (0x2000, rec), (0x3000, b'Q2L1|" .. src .. "|'),
            (0x4000, b'Q2L1|T|0x9|1|LOAD|GameScoring probe chunk ran|END\x00'),
            (0x5000, b'Q2L1|T|0x9|2|CANARY|file=Mods\\Q2Trace\\Q2Trace_Canary_Scoring.txt ok=true err=nil|END'),
            (0x6000, b'Q2L1|G|0x1a2b|8|PERC'),  # cut record (chunk end): ignored
            (0x7000, b'Q2L1|G|0x1a2b|9|ERR|EvaluatePerception Q2_Target_Player [string "?"]: bad|END'),
            (0x8000, b'Q2L1|P|0x77|2|PSTART|t=3.4 frame=102 plan=Build_Space_Forces player=REBEL target=? units=0|END'),
            (0x9000, b'Q2L1|P|0x78|1|PSTAGE|t=18 frame=540 plan=Conquer_Opponent player=REBEL target=Shola tf=GroundForce'
                     b' n=1 via=AssembleForce strongest=nil stage=Mon_Calamari stagestr=x spend=1 unalloc=0 cr=900'
                     b' count=0 units=0|END'),
            (0xa000, b'Q2L1|P|0x78|2|PBLOCK|t=19 frame=559 plan=Conquer_Opponent player=REBEL target=Shola tf=GroundForce'
                     b' n=1 k=1 ret=boolean spend=1 unalloc=0 cr=900 count=3 units=3 REBEL_INFANTRY_SQUAD'
                     b' REBEL_INFANTRY_SQUAD REBEL_LIGHT_TANK_BRIGADE|END')]
    p = parse_hits(hits)
    check(len(p) == 8, 'parse: 8 full records, source text and cut record ignored (%d)' % len(p))
    recs = [r[:5] for r in p]
    uniq, rows, eqs, s = summarize(recs)
    check(len(uniq) == 7, 'dedup: 7 distinct records')
    check(s['scripts']['P']['verdict'].startswith('loaded: 0 plan scripts; 1 thread starts') and
          '1 stagings, 1 blocks done' in s['scripts']['P']['verdict'], 'plan hook verdict')
    co = s['plans'].get('REBEL Conquer_Opponent GroundForce', {})
    check(co.get('stagings') == 1 and co.get('blocks') == 1 and co.get('first_frame') == 540 and
          co.get('stages') == ['Mon_Calamari'] and co.get('targets') == ['Shola'] and
          co.get('units') == {'REBEL_INFANTRY_SQUAD': 2, 'REBEL_LIGHT_TANK_BRIGADE': 1},
          'plan table: stage, target and units per player / plan / task force %s' % co)
    check(rows and rows[0][2].get('planet') == 'Nal_Hutta' and rows[0][2].get('Q2_Space_Total') == '12.5',
          'perception row parsed')
    check(eqs == ['Q2_Space_Total', 'Q2_Target_Player'], 'equation columns in first-seen order')
    check(s['scripts']['G']['verdict'].startswith('ran'), 'galactic verdict ran')
    check(s['scripts']['T']['verdict'].startswith('loaded'), 'scoring verdict loaded')
    check(len(s['canary']) == 1 and len(s['errors']) == 1, 'canary and error collected')
    check(s['gaps'] == ['G 0x1a2b 1-6,8', 'P 0x77 1'], 'seq gaps reported per instance %s' % s['gaps'])
    _u, _r, _e, s0 = summarize([])
    check(s0['scripts']['G']['verdict'].startswith('no records'), 'empty run verdict')
    out = os.path.join(HERE, 'out', 'selftest', 'q2lua')
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'lua_reader.log'), 'w', encoding='ascii', newline='\n') as f:
        for r in p:
            f.write('    1.00 REC rec=%s\n' % r[5])
    back = read_log_records(os.path.join(out, 'lua_reader.log'))
    check(sorted(set(back)) == sorted(set(recs)), 'records read back from lua_reader.log')
    s2 = write_outputs(out, back)
    with open(os.path.join(out, 'lua_perception.tsv'), encoding='ascii') as f:
        tsv = f.read().splitlines()
    check(len(tsv) == 2 and tsv[1].split('\t')[4] == 'Nal_Hutta', 'lua_perception.tsv written')
    check(s2['records'] == 7 and 'REBEL Conquer_Opponent GroundForce' in s2['plans'], 'lua_summary.json written')
    print('SELFTEST_Q2LUA ' + ('FAILED (%d)' % len(fails) if fails else 'OK'))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--run-dir', required=True)
    r.add_argument('--wait-process-sec', type=float, default=180)
    r.add_argument('--scan-sec', type=float, default=10.0)
    r.add_argument('--allow-unknown-exe', action='store_true')
    s = sub.add_parser('summarize')
    s.add_argument('--run-dir', required=True)
    sub.add_parser('selftest')
    a = ap.parse_args()
    if a.cmd == 'run':
        return run(a)
    if a.cmd == 'summarize':
        s = write_outputs(a.run_dir, read_log_records(os.path.join(a.run_dir, 'lua_reader.log')))
        print(json.dumps(s['scripts'], indent=2, sort_keys=True))
        return 0
    return selftest(a)


if __name__ == '__main__':
    sys.exit(main())
