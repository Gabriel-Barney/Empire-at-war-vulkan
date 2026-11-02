#!/usr/bin/env python3
"""s7s8_cleanup.py - removes everything the S7/S8 probe put into the game folder. Safe to run any number of times;
s7s8_run.py calls it at the end of every run (also after an error or Ctrl+C).

  s7s8_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\S7S8Probe is deleted only if it holds S7S8_PROBE_MARKER.txt (build_s7s8_mod.py made it).
That folder is the only thing the probe adds (the hooked free-store script, the text file with the probe's
Game_Message lines, the marker); no retail file is touched and nothing else is deleted (other probe kits' folders,
e.g. Mods\\OIAbilityProbe, are left alone).
"""
import argparse
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
MOD_NAME = 'S7S8Probe'
MARKER = 'S7S8_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    mod = os.path.join(game, 'corruption', 'Mods', MOD_NAME)
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
    ap.add_argument('--no-pause', action='store_true', help='for the .cmd wrapper: end without waiting for a key')
    a = ap.parse_args()
    rep = cleanup(a.game, a.log, a.dry_run)
    for x in rep['actions']:
        print(x)
    print('S7S8_CLEANUP %s' % ('OK' if rep['ok'] else 'INCOMPLETE'))
    return 0 if rep['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
