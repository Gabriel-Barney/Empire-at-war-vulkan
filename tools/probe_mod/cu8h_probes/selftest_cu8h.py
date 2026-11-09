#!/usr/bin/env python3
"""selftest_cu8h.py - offline check of the cu8h probe kit (no game is started, nothing is installed).

  selftest_cu8h.py [--game DIR] [--source-zip extracted\\foc_data_text.zip] [--lua <lua50.exe>] [--keep]

1. Builds every session (galendor, hum, luke) into a temp folder: from the install when --game has one (read only),
   else from the text zip (no texts then). Every written .xml must parse; every written .lua must compile under a stock Lua 5.0.2
   (tools\\probe_mod\\out\\tools\\lua50.exe, built by ..\\common\\build_lua50.cmd on first use).
2. cu8h_dbg.py --selftest: the debugger attaches to a fake process that steps the LCG and must see every step.
3. cu8h_dbg_parse.py on a synthetic trace (seed, chunk-0x102 draws, one pick, starting forces, two weather draws;
   no exe needed, the stacks carry function starts): the verdict must be "A B C" with the replayed pick index.
4. cu8h_run.py's report on a synthetic watch log (start suppressed, draw, unsuppress, a throw and catch, holster
   450 frames after the last engaged frame): the checks must name each of them.
Ends with CU8H SELFTEST OK, or lists the failures (exit 1).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import lua50  # noqa: E402
import build_cu8h_mod as B  # noqa: E402
import cu8h_dbg_parse as P  # noqa: E402
import cu8h_run as R  # noqa: E402
import cu8h_sites as S  # noqa: E402


def synthetic_trace(path):

    lines = ['CU8HDBG 1 t=selftest state_rva=a13e24 stop_mode_frame=120', 'ATTACH pid=1 base=0 state=0',
             'ARM why=arm_now threads=1 failed=0 t=0 state=0']
    n = [0]

    def hit(state, steps, stack, mf=0, tid='1'):
        n[0] += 1
        lines.append('HIT n=%d tid=%s rip=1ffb8d state=%08x steps=%s dr6=1 mt=1 st=1 mode=1 mf=%d gf=%d stack=%s' % (
            n[0], tid, state, steps, mf, mf, ','.join('%x' % r for r in stack)))

    s = 0x0BADF00D
    hit(s, 'seed', [0x28e8a0, 0x28e890, 0x56480])
    for _ in range(20):  # chunk 0x102 construction draws (behaviour-attach stagger)
        s = P.lcg(s)
        hit(s, '1', [0x1ffb8d, 0x3a4820, 0x3a0930, 0x2b0a20, 0x35f970, 0x56480])
    v, k, s_after = P.int_draw(s, 0, 3)
    t = s
    for i in range(k):  # the pick: one int(0, 3), possibly several steps
        t = P.lcg(t)
        hit(t, '1', [0x1ffb8d, 0x277030, 0x3b8cd0, 0x56480])
    s = t
    for _ in range(5):  # skirmish starting forces
        s = P.lcg(s)
        hit(s, '1', [0x1ffb8d, 0x29f810, 0x2a4fc0], tid='2')
    for _ in range(2):  # first land tick: weather restart
        s = P.lcg(s)
        hit(s, '1', [0x1ffbd2, 0x468980, 0x468a50, 0x46a0c0, 0x3bb440], tid='2')
    lines += ['STOP why=mode_frame_120 hits=%d' % n[0], 'DETACH ok=1 err=0', 'EXIT']
    open(path, 'w').write('\n'.join(lines) + '\n')
    return v


def synthetic_watch(run_dir):
    def st(mf, **kv):
        base = {'b3b4': 0, 'b3b5': 0, 'b3b6': 0, 'b3b7': 0, 'b3b8': 0, 'b3b9': 1, 'beh_index': 2, 'bone': 7, 'drawn': 0,
                'end': 0, 'eng': 0, 'saber': '3fffff', 'start': 0}
        base.update(kv)
        return '0.0 ST id=1 type=LUKE_SKYWALKER_JEDI mf=%d gf=%d %s' % (mf, mf, ' '.join(
            '%s=%s' % x for x in sorted(base.items())))
    w = ['0.0 MODE ptr=1 type=1 mf=0 gf=0', '0.0 HERO id=1 type=LUKE_SKYWALKER_JEDI addr=1 owner=0 mf=10 gf=10',
         st(10), st(462, saber='10f8a'), st(465, saber='10f8a', drawn=1, start=465, end=470, eng=465),
         st(497, drawn=1, start=465, end=470, eng=495), st(498, drawn=1, eng=495, b3b9=0),
         st(499, drawn=1, eng=495, b3b9=0, b3b5=1),
         st(946, drawn=0, eng=495, start=945, end=950), st(951, drawn=0, eng=495)]
    open(os.path.join(run_dir, 'watch.log'), 'w').write('\n'.join(w) + '\n')
    open(os.path.join(run_dir, 'records.log'), 'w').write(
        'C8G|HSPAWN|p=hum|sid=1|n=1|f=300|role=L|type=Luke_Skywalker_Jedi|how=generic|id=1|at=0,0|END\n'
        'C8G|HP|p=hum|sid=1|n=2|f=456|L=obj:REBEL_TROOPER;|END\n')


def synthetic_story_trace(path):
    """A story-like trace for cu8h_sites.py: menu-battle draws (space mode A), the seed, galactic draws (mode G), the
    land battle (mode B): load draws at mf 0 on a load thread, 3 draws in mf 1, then 2 per frame from one call site
    and 1 every 3rd frame from another up to mf 300, plus formation-ctor draws (one with a rejected try)."""
    lines = ['CU8HDBG 1 t=selftest state_rva=a13e24 stop_mode_frame=18010', 'ATTACH pid=1 base=0 state=0',
             'ARM why=arm_file threads=1 failed=0 t=0 state=0']
    n = [0]
    st = [0x12345678]

    def hit(stack, mode, mt, mf, steps='1', tid='1', state=None):
        n[0] += 1
        st[0] = state if state is not None else P.lcg(st[0])
        lines.append('HIT n=%d tid=%s rip=%x state=%08x steps=%s dr6=1 mt=%d st=1 mode=%s mf=%d gf=%d stack=%s' % (
            n[0], tid, stack[0], st[0], steps, mt, mode, mf, mf, ','.join('%x' % r for r in stack)))

    for i in range(5):
        hit([0x1ffbd2, 0x3825b0, 0x3639d0], 'a0', 2, 1000 + i)
    hit([0x28ea14, 0x56480], 'a0', 2, 1005, steps='seed', state=0)
    for _ in range(7):
        hit([0x1ffb8d, 0x2a1eb0], 'c0', 0, 1)
    for _ in range(11):
        hit([0x1ffb8d, 0x134307, 0x3a097c, 0x2b164d, 0x3603a0], 'b0', 1, 0, tid='9')
    # one ctor draw with a rejected try: find states whose int(1, 30) value is > 29, then <= 29
    while (((P.lcg(st[0]) >> 10) & 0x7FFF) & 31) <= 29:
        st[0] = P.lcg(st[0])
    ctor = [0x1ffb8d, 0x40d11f, 0x4f6fd4, 0x4f738a, 0x43b502, 0x39bda0, 0x395421, 0x2b44a7]
    hit(ctor, 'b0', 1, 0, tid='9')    # rejected
    hit(ctor, 'b0', 1, 0, tid='9')    # accepted or not: the next draw starts after the next accepted try
    while (((st[0] >> 10) & 0x7FFF) & 31) > 29:
        hit(ctor, 'b0', 1, 0, tid='9')
    for _ in range(3):
        hit([0x1ffb8d, 0x3a94f9, 0x62d0c9, 0x62cf90], 'b0', 1, 1)
    for mf in range(2, 301):
        hit([0x1ffbd2, 0x62c8aa, 0x62d007, 0x62cdaf], 'b0', 1, mf)
        hit([0x1ffbd2, 0x62c8aa, 0x62d007, 0x62cdaf], 'b0', 1, mf)
        if mf % 3 == 0:
            hit([0x1ffb8d, 0x3a94f9, 0x62d0c9, 0x62cf90], 'b0', 1, mf)
    land = [0x1ffb8d, 0x40d11f, 0x4f6fd4, 0x43e712, 0x4f80a8, 0x426f28, 0x363c83]
    while (((P.lcg(st[0]) >> 10) & 0x7FFF) & 31) > 29:
        st[0] = P.lcg(st[0])
    hit(land, 'b0', 1, 300)
    hit(land, 'b0', 1, 300)
    lines += ['STOP why=mode_frame_300 hits=%d' % n[0], 'DETACH ok=1 err=0']
    open(path, 'w').write('\n'.join(lines) + '\n')


def synthetic_saber_watch(run_dir):
    """Luke throws at mf 462 (saber id 0x10f8a = 69514), the saber flies 3 frames and dies; the ability ends 1 frame
    later. Max travel 280 = 250 + R 20 + |aim z 17 - spawn z 7|."""
    def st(mf, saber='3fffff'):
        return ('0.0 ST id=1 type=LUKE_SKYWALKER_JEDI mf=%d gf=%d aim=150.000,0.000,17.000 b3b9=1 drawn=0 saber=%s '
                'sap_tgt=7' % (mf, mf, saber))
    w = ['0.0 MODE ptr=1 type=1 mf=0 gf=0', '0.0 HERO id=1 type=LUKE_SKYWALKER_JEDI addr=1 owner=0 mf=10 gf=10', st(10),
         st(462, '10f8a'), '0.0 SABER id=69514 type=LUKES_LIGHT_SABER addr=99 mf=462 gf=462 sap_tgt=7 aim=150,0,17',
         '0.0 TARGET id=7 type=REBEL_TROOPER mf=462 gf=462 pos=150.000,0.000,10.000']
    for k, mf in enumerate((462, 463, 464, 465)):
        w.append('0.0 SB id=69514 mf=%d gf=%d x=%.3f y=0.000 z=7.000 flags=%s trav=%.3f maxt=280.000 dmg=13.000 ptgt=7 '
                 'hx=0.000 hy=0.000 hz=0.000 d=%.3f' % (mf, mf, 8.0 * k, '42' if mf == 465 else '0', 8.0 * k,
                                                         8.0 * k))
    w += ['0.0 SBGONE id=69514 mf=466 gf=466 why=id_changed', st(467)]
    open(os.path.join(run_dir, 'watch.log'), 'w').write('\n'.join(w) + '\n')
    open(os.path.join(run_dir, 'records.log'), 'w').write('')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=B.DEFAULT_GAME)
    ap.add_argument('--source-zip', default=os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    ap.add_argument('--lua')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    fails = []
    work = tempfile.mkdtemp(prefix='cu8h_selftest_')
    try:
        have_game = os.path.isfile(os.path.join(a.game, 'corruption', 'StarWarsG.exe'))
        lua = a.lua or lua50.find_lua()
        if not lua:
            fails.append('lua50.exe missing and could not be built (see %s)' % lua50.BUILD_LOG)
        # 1. builds
        for s in B.SESSIONS:
            out = os.path.join(work, s)
            try:
                cfg = B.build(s, a.game, out, None if have_game else a.source_zip, no_text=not have_game)
            except SystemExit as e:
                fails.append('%s: build failed: %s' % (s, e))
                continue
            for rel in cfg['files']:
                p = os.path.join(out, rel)
                if rel.lower().endswith('.xml'):
                    try:
                        ET.parse(p)
                    except ET.ParseError as e:
                        fails.append('%s %s: XML does not parse: %s' % (s, rel, e))
                if rel.lower().endswith('.lua') and lua:
                    r = subprocess.run([lua, '-e', 'assert(loadfile([[%s]]))' % p], capture_output=True, text=True)
                    if r.returncode != 0:
                        fails.append('%s %s: Lua 5.0 compile failed: %s' % (s, rel, (r.stderr or r.stdout).strip()))
            print('build %-9s ok (%s)' % (s, ', '.join(cfg['files'])))
        # 2. debugger
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'cu8h_dbg.py'), '--selftest'], capture_output=True,
                           text=True, timeout=300)
        if 'CU8H DBG SELFTEST OK' not in r.stdout:
            fails.append('cu8h_dbg.py --selftest: %s' % (r.stdout[-400:] + r.stderr[-400:]).strip())
        else:
            print('cu8h_dbg.py --selftest ok')
        # 3. parser
        tpath = os.path.join(work, 'trace.log')
        v = synthetic_trace(tpath)
        text, _hits = P.report(tpath, None, os.path.join(work, 'trace_report.txt'), 4)
        if 'VERDICT t01: A B C' not in text or ('picked index %d' % v) not in text or 'WEATHER hits=2' not in text:
            fails.append('parser: unexpected report:\n' + '\n'.join(text.splitlines()[-8:]))
        else:
            print('cu8h_dbg_parse.py ok (pick %d, verdict A B C)' % v)
        # 4. watcher report
        rd = os.path.join(work, 'run')
        os.makedirs(rd)
        synthetic_watch(rd)
        text, _n = R.report(rd)
        for want in ('at first sight: b3b9=1 drawn=0', 'DRAW seen mf=465', 'saber 3fffff->10f8a at mf=462',
                     'saber 10f8a->3fffff at mf=497', 'b3b9 1->0 at mf=498', 'start-eng=450'):
            if want not in text:
                fails.append('watch report: missing "%s"' % want)
        if not any(f.startswith('watch report') for f in fails):
            print('cu8h_run.py report ok')
        # 5. call-site report (storyb / cu8k)
        spath = os.path.join(work, 'story', 'trace.log')
        os.makedirs(os.path.dirname(spath))
        synthetic_story_trace(spath)
        text = S.report(spath, None, os.path.join(work, 'story', 'sites.txt'))
        for want in ('M2 mode=b0 mt=1 st=1 mf=0..300', '<- battle mode', 'cum=7 ', 'mf   256-1800  steps=107 ',
                     'steps/frame=2.378', '62c8aa in 62c8aa', 'steps=90 ', 'frame gaps (gap:count) 1:44',
                     '1  M2 creator=degenerate adopt=gom_postload(395421)', '2  M2 creator=land_composition'):
            if want not in text:
                fails.append('sites report: missing "%s"' % want)
        if not any(f.startswith('sites report') for f in fails):
            print('cu8h_sites.py ok (load split, windows, call sites, formation-ctor draws)')
        # 6. thrown-saber report and trace summaries
        rd = os.path.join(work, 'saber')
        os.makedirs(os.path.join(rd, 'trace1-test'))
        synthetic_saber_watch(rd)
        shutil.copy(spath, os.path.join(rd, 'trace1-test', 'trace.log'))
        shutil.copy(os.path.join(work, 'story', 'sites.txt'), os.path.join(rd, 'trace1-test', 'sites.txt'))
        text, _n = R.report(rd)
        for want in ('saber id=69514 (10f8a) type=LUKES_LIGHT_SABER', 'R = max travel - 250 - |aim z - spawn z| = '
                     '280.000 - 250 - 10.000 = 20.000', 'SAP +0x2e8 set mf=462, cleared (ability end) mf=467',
                     'flagged dead mf=465', 'object gone mf=466', 'end - flagged dead = 2, end - gone = 1', '== trace trace1-test ==',
                     '2  M2 creator=land_composition'):
            if want not in text:
                fails.append('saber / trace report: missing "%s"' % want)
        if not any(f.startswith('saber / trace') for f in fails):
            print('cu8h_run.py saber and trace summaries ok')
    finally:
        if a.keep:
            print('kept', work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    if fails:
        print('CU8H SELFTEST FAILED:')
        for f in fails:
            print('  ' + f)
        return 1
    print('CU8H SELFTEST OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
