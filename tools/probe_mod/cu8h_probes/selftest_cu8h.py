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
