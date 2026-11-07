#!/usr/bin/env python3
"""a1h_cleanup.py - removes everything the a1h t09 probe put into the game folder. Safe to run any number of times;
a1h_run.py calls it at the end of every run (also after an error or Ctrl+C).

  a1h_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\A1hAssembleProbe is deleted only if it holds A1H_PROBE_MARKER.txt (build_a1h_mod.py made it).
That folder is the only thing the probe adds; no retail file is touched, nothing else is deleted. The game's own
galactic save slots are not touched either: do not save the probe campaign (a save would reference the mod's files).
"""
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAME = 'A1hAssembleProbe'
MARKER = 'A1H_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, (MOD_NAME,), MARKER, log, dry_run)


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'A1H_CLEANUP'))
