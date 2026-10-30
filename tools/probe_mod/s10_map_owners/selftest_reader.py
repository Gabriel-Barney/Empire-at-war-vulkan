#!/usr/bin/env python3
"""selftest_reader.py - end-to-end offline check of the retail output channel: bin\\s10_fake_game.exe lays out the
documented StarWarsG.exe structures with a scenario map's placements (owner = raw map owner, the "faction" rule)
plus spawned units of single-affiliation types, and opens a message box; bin\\s10reader.exe dumps the object store,
records and dismisses the box; s10_parse.py turns the reader log into the table.

  selftest_reader.py            (run build_reader.cmd first)

Cases: Polus with the hook popup; Polus without any popup (map from placements, ids from affiliation votes);
Tatooine as a campaign battle with the Underworld attacker (popup = the hook's t0 summary).
"""
import argparse
import json
import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import s10_parse  # noqa: E402

BIN = os.path.join(HERE, 'bin')
CASES = [
    # name, map, popup text ('' = none), sides (factions whose units are spawned), use popups
    ('polus_popup', '_MP_LAND_POLUS', 'S10 t0 _MP_LAND_POLUS Rebel=0:7 Empire=1:7 Neutral=3:150', ['Rebel', 'Empire'],
     True),
    ('polus_nopopup', '_MP_LAND_POLUS', '', ['Rebel', 'Empire'], False),
    ('tatooine_popup', '_LAND_PLANET_TATOOINE_02',
     'S10 t0 _LAND_PLANET_TATOOINE_02 Rebel=0:0 Empire=1:7 Neutral=3:100 Underworld=6:7', ['Empire', 'Underworld'], True),
]


def objects_file(path, mp, sides, exp, order, aff):
    lines = []
    for r in exp[mp]['objects']:
        if r['kind'] == 'marker' or r['type'].startswith('?'):
            continue
        x, y, z = r['pos']
        lines.append('%s %d %.2f %.2f %.2f' % (s10_parse.tkey(r['type']), r['owner'] if r['owner'] < 8 else 3, x, y, z))
    for k, fac in enumerate(sides):
        names = sorted(n for n, f in aff.items() if f == fac.upper() and ' ' not in n)[:7]
        for i, n in enumerate(names):
            lines.append('%s %d %.2f %.2f 0.00' % (n, order.index(fac), 100.0 + 10 * i, 500.0 + 2500 * k))
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return len(lines)


def run_case(case, exp, order, aff, out_dir):
    name, mp, popup, sides, use_popups = case
    objs = os.path.join(out_dir, 'reader_%s_objects.txt' % name)
    log = os.path.join(out_dir, 'reader_%s.log' % name)
    n = objects_file(objs, mp, sides, exp, order, aff)
    for f in (log, log + '.status.json'):
        if os.path.exists(f):
            os.remove(f)  # the reader appends
    game = subprocess.Popen([os.path.join(BIN, 's10_fake_game.exe'), objs, '5', popup], stdout=subprocess.PIPE,
                            text=True)
    head = game.stdout.readline().split()  # BASE 0x... PID n
    base, pid = head[1], head[3]
    r = subprocess.run([os.path.join(BIN, 's10reader.exe'), '--out', log, '--status', log + '.status.json',
                        '--pid', pid, '--test-base', base, '--allow-unknown-exe', '--frames', '1,60',
                        '--dismiss-popups', '--timeout-sec', '60'], capture_output=True, text=True)
    game.wait(timeout=60)
    res = s10_parse.cmd_parse(argparse.Namespace(log=log, out=os.path.join(out_dir, 'reader_%s_table.txt' % name),
                                                 tag='t0', no_popups=not use_popups, aff=aff))
    text = open(log).read()
    bad = []
    if r.returncode != 0:
        bad.append('reader exit %d' % r.returncode)
    if text.count('\nDUMP ') != 2:
        bad.append('dumps %d' % text.count('\nDUMP '))
    if popup and ('POPUP battle=1' not in text or popup not in text):
        bad.append('popup not recorded')
    if 'EXIT reason=game_exited' not in text:
        bad.append('no clean exit')
    rows = next(iter(res.values()), None) if len(res) == 1 else None
    if rows is None:
        bad.append('%d battles parsed: %s' % (len(res), list(res)))
    else:
        key = next(iter(res))
        if mp not in key and not key.startswith(('SK_POLUS', 'GC_TATOOINE')):
            bad.append('map %s' % key)
        for fac in sides:
            raw = order.index(fac)
            row = rows.get(raw)
            # a side owns map placements only where the map gives it some (Polus: one raw-0 object, Tatooine: none)
            if row and row['found'] and (row['id_eq'] != 1.0 or row['fac_eq'] != 1.0):
                bad.append('raw %d: %s' % (raw, row))
        row3 = rows.get(3)
        want3 = sum(1 for r in exp[mp]['objects'] if r['owner'] == 3 and r['kind'] != 'marker' and
                    not r['type'].startswith('?'))
        if not row3 or row3['found'] < 0.9 * want3 or row3['id_eq'] != 1.0:
            bad.append('raw 3: %s' % row3)
        table = open(os.path.join(out_dir, 'reader_%s_table.txt' % name)).read()
        for fac in sides:
            if 'player id %d = %s' % (order.index(fac), fac.upper()) not in table:
                bad.append('player id %d not identified as %s' % (order.index(fac), fac))
    print('reader case %-15s %4d objects: %s' % (name, n, 'OK' if not bad else 'FAILED ' + '; '.join(bad)))
    return not bad


def main():
    argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter).parse_args()
    for exe in ('s10reader.exe', 's10_fake_game.exe'):
        if not os.path.exists(os.path.join(BIN, exe)):
            print('missing bin\\%s: run build_reader.cmd' % exe)
            return 1
    cfg = json.load(open(os.path.join(HERE, 'maps.json')))
    exp = json.load(open(os.path.join(HERE, 'expected_objects.json')))
    aff = s10_parse.affiliations()
    out_dir = os.path.join(HERE, 'out', 'selftest')
    os.makedirs(out_dir, exist_ok=True)
    ok = all([run_case(c, exp, cfg['faction_order'], aff, out_dir) for c in CASES])
    print('READER SELFTEST OK' if ok else 'READER SELFTEST FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
