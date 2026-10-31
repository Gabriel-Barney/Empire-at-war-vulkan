#!/usr/bin/env python3
"""a1_cleanup.py - removes everything the A1 tactical-AI probes put into the game folder. Safe to run any number of
times; a1_run.py calls it at the end of every run (also after an error or Ctrl+C).

  a1_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\A1TacticalProbe is deleted only if it holds A1_PROBE_MARKER.txt (build_a1_mod.py made it).
That folder is the only thing the probes add; no retail file is touched, nothing else is deleted. If the game is
still running, quit it first (the folder can still be deleted, but the run's own state would be unclear).
"""
import argparse
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
MOD_NAME = 'A1TacticalProbe'
MARKER = 'A1_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    corr = os.path.join(game, 'corruption')
    mod = os.path.join(corr, 'Mods', MOD_NAME)
    rep = {'ok': True, 'actions': []}
    if os.path.isdir(mod):
        if os.path.exists(os.path.join(mod, MARKER)):
            if not dry_run:
                shutil.rmtree(mod, ignore_errors=True)
            gone = dry_run or not os.path.exists(mod)
            rep['actions'].append('%s %s' % ('would delete' if dry_run else ('deleted' if gone else 'COULD NOT DELETE'),
                                             mod))
            rep['ok'] = gone
        else:
            rep['actions'].append('left %s: no %s, not made by the probe' % (mod, MARKER))
    else:
        rep['actions'].append('nothing to remove (%s absent)' % mod)
    if log:
        with open(log, 'a') as f:
            f.write('%s cleanup ok=%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), rep['ok']))
            for a in rep['actions']:
                f.write('  ' + a + '\n')
    return rep


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--log')
    ap.add_argument('--no-pause', action='store_true', help='for the .cmd wrappers: end without waiting for a key')
    a = ap.parse_args()
    rep = cleanup(a.game, a.log, a.dry_run)
    for x in rep['actions']:
        print(x)
    print('A1_CLEANUP %s' % ('OK' if rep['ok'] else 'INCOMPLETE'))
    return 0 if rep['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
