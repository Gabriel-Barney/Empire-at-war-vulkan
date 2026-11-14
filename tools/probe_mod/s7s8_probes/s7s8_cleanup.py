#!/usr/bin/env python3
"""s7s8_cleanup.py - removes everything the S7/S8 probe put into the game folder. Safe to run any number of times;
s7s8_run.py calls it at the end of every run (also after an error or Ctrl+C).

  s7s8_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

<game>\\corruption\\Mods\\S7S8Probe is deleted only if it holds S7S8_PROBE_MARKER.txt (build_s7s8_mod.py made it).
That folder is the only thing the probe adds (the hooked free-store script, the text file with the probe's
Game_Message lines, the marker); no retail file is touched and nothing else is deleted (other probe kits' folders,
e.g. Mods\\OIAbilityProbe, are left alone).
"""
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAME = 'S7S8Probe'
MARKER = 'S7S8_PROBE_MARKER.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, (MOD_NAME,), MARKER, log, dry_run)


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'S7S8_CLEANUP'))
