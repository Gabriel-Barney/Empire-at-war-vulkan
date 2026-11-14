#!/usr/bin/env python3
"""cu8h_cleanup.py - removes what the cu8h probes put into the game folder. Safe to run any number of times;
cu8h_run.py calls it at the end of every session (also after an error or Ctrl+C).

  cu8h_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\CU8HProbe is deleted only if it holds CU8H_PROBE_MARKER.txt (build_cu8h_mod.py made it).
No retail file is touched, nothing else is deleted. Never save a probe campaign (a save would reference the mod).
"""
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAMES = ('CU8HProbe',)
MARKER = 'CU8H_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, MOD_NAMES, MARKER, log, dry_run)


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'CU8H_CLEANUP'))
