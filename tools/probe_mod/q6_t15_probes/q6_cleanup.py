#!/usr/bin/env python3
"""q6_cleanup.py - removes everything the Q6 t15 probes put into the game folder. Safe to run any number of times;
q6_run.py calls it at the end of every session (also after an error or Ctrl+C).

  q6_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\Q6T15Probe and <game>\\corruption\\Mods\\Q6T15ProbeTR are deleted only if they hold
Q6T15_PROBE_MARKER.txt (build_q6_mod.py made them). They are the only things the probes add: no retail file and no
file of Thrawn's Revenge (the workshop folder is only read) is touched, nothing else is deleted. Do not save a probe
campaign (a save would reference the mod's files).
"""
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAMES = ('Q6T15Probe', 'Q6T15ProbeTR')
MARKER = 'Q6T15_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, MOD_NAMES, MARKER, log, dry_run)


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'Q6_CLEANUP'))
