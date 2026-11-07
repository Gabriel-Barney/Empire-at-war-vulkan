#!/usr/bin/env python3
"""a1_cleanup.py - removes everything the A1 tactical-AI probes put into the game folder. Safe to run any number of
times; a1_run.py calls it at the end of every run (also after an error or Ctrl+C).

  a1_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\A1TacticalProbe is deleted only if it holds A1_PROBE_MARKER.txt (build_a1_mod.py made it).
That folder is the only thing the probes add; no retail file is touched, nothing else is deleted. If the game is
still running, quit it first (the folder can still be deleted, but the run's own state would be unclear).
"""
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAME = 'A1TacticalProbe'
MARKER = 'A1_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, (MOD_NAME,), MARKER, log, dry_run)


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'A1_CLEANUP'))
