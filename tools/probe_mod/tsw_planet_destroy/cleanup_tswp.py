#!/usr/bin/env python3
"""cleanup_tswp.py - removes everything the TSW planet-destroy probe put into the game folder. Safe to run any number
of times; run_tswp.py calls it at the end of every run.

  cleanup_tswp.py [--game DIR] [--dry-run]

<game>\\corruption\\Mods\\TswPlanetProbe is deleted only if it holds TSWP_PROBE_MARKER.txt (i.e. build_mod.py made it).
No retail file is touched: the probe only adds that mod folder.
"""
import argparse
import os
import shutil
import sys

sys.dont_write_bytecode = True
DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
MARKER = 'TSWP_PROBE_MARKER.txt'
MOD_NAME = 'TswPlanetProbe'


def cleanup(game, dry_run=False):
    mod = os.path.join(game, 'corruption', 'Mods', MOD_NAME)
    if not os.path.isdir(mod):
        return True, 'nothing to do (%s absent)' % mod
    if not os.path.exists(os.path.join(mod, MARKER)):
        return False, 'KEPT %s (no %s: not made by build_mod.py)' % (mod, MARKER)
    if dry_run:
        return True, 'would delete ' + mod
    shutil.rmtree(mod)
    return not os.path.isdir(mod), 'deleted ' + mod


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    ok, msg = cleanup(a.game, a.dry_run)
    print(msg)
    print('CLEANUP_OK' if ok else 'CLEANUP_INCOMPLETE')
    return 0 if ok else 40


if __name__ == '__main__':
    sys.exit(main())
