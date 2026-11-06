#!/usr/bin/env python3
"""q6_cleanup.py - removes everything the Q6 t15 probes put into the game folder. Safe to run any number of times;
q6_run.py calls it at the end of every session (also after an error or Ctrl+C).

  q6_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\Q6T15Probe and <game>\\corruption\\Mods\\Q6T15ProbeTR are deleted only if they hold
Q6T15_PROBE_MARKER.txt (build_q6_mod.py made them). They are the only things the probes add: no retail file and no
file of Thrawn's Revenge (the workshop folder is only read) is touched, nothing else is deleted. Do not save a probe
campaign (a save would reference the mod's files).
"""
import argparse
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
MOD_NAMES = ('Q6T15Probe', 'Q6T15ProbeTR')
MARKER = 'Q6T15_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    rep = {'ok': True, 'actions': []}
    for name in MOD_NAMES:
        mod = os.path.join(game, 'corruption', 'Mods', name)
        if not os.path.isdir(mod):
            rep['actions'].append('nothing to remove (%s absent)' % mod)
            continue
        if not os.path.exists(os.path.join(mod, MARKER)):
            rep['actions'].append('left %s: no %s, not made by the probe' % (mod, MARKER))
            continue
        if not dry_run:
            shutil.rmtree(mod, ignore_errors=True)
        gone = dry_run or not os.path.exists(mod)
        rep['actions'].append('%s %s' % ('would delete' if dry_run else ('deleted' if gone else 'COULD NOT DELETE'), mod))
        rep['ok'] = rep['ok'] and gone
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
    ap.add_argument('--no-pause', action='store_true', help='for the .cmd wrapper: end without waiting for a key')
    a = ap.parse_args()
    rep = cleanup(a.game, a.log, a.dry_run)
    for x in rep['actions']:
        print(x)
    print('Q6_CLEANUP %s' % ('OK' if rep['ok'] else 'INCOMPLETE'))
    return 0 if rep['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
