#!/usr/bin/env python3
"""r1_cleanup.py - removes what the R1 hardpoint probe put into the game folder. Safe to run any number of times;
r1_run.py calls it at the start and at the end of every run (also after an error or Ctrl+C).

  r1_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\R1HardpointProbe is deleted only if it holds R1_PROBE_MARKER.txt (build_r1_mod.py made it).
That folder is the only thing the probe installs; no retail file is touched and nothing else is deleted.
"""
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAMES = ('R1HardpointProbe',)
MARKER = 'R1_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, MOD_NAMES, MARKER, log, dry_run)


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'R1_CLEANUP'))
