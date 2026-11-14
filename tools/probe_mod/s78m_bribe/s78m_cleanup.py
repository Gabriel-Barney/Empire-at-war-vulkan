#!/usr/bin/env python3
r"""s78m_cleanup.py - removes what the s78m t06 bribe probe put into the game folder and restores the player's profile.
Safe to run any number of times; s78m_run.py does the same at the end of every run (also after an error or Ctrl+C).

  s78m_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--run-dir DIR] [--no-profile] [--no-pause]

* <game>\corruption\Mods\S78MBribeProbe is deleted only if it holds S78M_PROBE_MARKER.txt (build_s78m_mod.py made
  it). That folder is the only thing the probe adds; no retail file is touched, other kits' Mods folders are left.
* Profile: the game rewrites its options (registry HKCU\Software\Petroglyph\StarWars FOC) and may write autosaves /
  the last-game recording under Saved Games. s78m_run.py snapshots both before the game starts
  (..\q2_traces\q2_profile.py, files in out\<run>\profile_backup\). This script restores that snapshot, verifies it,
  and moves files the run created into out\<run>\profile_new\ (nothing is deleted). --run-dir picks the run
  (default: out\latest.txt). A snapshot already restored (profile_restore_done.txt exists) is not applied again, so
  an old backup never overwrites newer settings. Skipped while StarWarsG.exe is running (the game would write its
  options again on exit).
"""
import argparse
import os
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'q2_traces'))
import probe_cleanup  # noqa: E402
from probe_run import running  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAME = 'S78MBribeProbe'
MARKER = 'S78M_PROBE_MARKER.txt'
DONE = 'profile_restore_done.txt'


def cleanup(game, log=None, dry_run=False):
    return probe_cleanup.cleanup(game, (MOD_NAME,), MARKER, log, dry_run)


def restore_profile(run_dir, dry_run=False):
    """Returns (ok, message)."""
    if not run_dir or not os.path.isdir(run_dir):
        return True, 'no run folder: no profile to restore'
    if not os.path.exists(os.path.join(run_dir, 'profile_backup', 'profile.json')):
        return True, 'run %s has no profile snapshot' % run_dir
    if os.path.exists(os.path.join(run_dir, DONE)):
        return True, 'profile of %s was already restored (%s)' % (run_dir, DONE)
    if running('StarWarsG.exe'):
        return False, 'StarWarsG.exe is running: close it and run this again'
    import q2_profile
    if dry_run:
        snap = q2_profile._load(run_dir)
        return True, 'would restore; differences now: %s' % (q2_profile.diff(snap) or 'none')
    rc = q2_profile.restore(run_dir)
    if rc == 0:
        with open(os.path.join(run_dir, DONE), 'w') as f:      # q2_profile writes no file when nothing changed
            f.write('profile restored and verified ' + time.strftime('%Y-%m-%d %H:%M:%S') + '\n')
    return rc == 0, 'profile restore %s' % ('verified' if rc == 0 else 'NOT VERIFIED, see profile_restore.json')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--log')
    ap.add_argument('--run-dir')
    ap.add_argument('--no-profile', action='store_true')
    ap.add_argument('--no-pause', action='store_true', help='for the .cmd wrapper: end without waiting for a key')
    a = ap.parse_args()
    rep = cleanup(a.game, a.log, a.dry_run)
    for x in rep['actions']:
        print(x)
    ok = rep['ok']
    if not a.no_profile:
        run_dir = a.run_dir
        if not run_dir:
            latest = os.path.join(HERE, 'out', 'latest.txt')
            if os.path.exists(latest):
                run_dir = open(latest).read().strip()
        pok, msg = restore_profile(run_dir, a.dry_run)
        print(msg)
        ok = ok and pok
    print('S78M_CLEANUP %s' % ('OK' if ok else 'INCOMPLETE'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
