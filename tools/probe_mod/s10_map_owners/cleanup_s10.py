#!/usr/bin/env python3
"""cleanup_s10.py - removes everything the S10 map-owner probe put into the game folder. Safe to run any number of
times; run_s10.py calls it at the end of every run.

  cleanup_s10.py [--game DIR] [--dry-run]

  * <game>\\corruption\\Mods\\S10MapOwners is deleted only if it holds S10_PROBE_MARKER.txt (i.e. build_mod.py made it);
  * every _S10MapOwners*.txt the game wrote (game folder, Mods, user folders) is MOVED into out\\<run>\\ (never deleted).
No retail file is touched: the probe only adds the mod folder, and the game itself writes the log.
"""
import argparse
import datetime
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
MARKER = 'S10_PROBE_MARKER.txt'
LOG_NAME = 's10mapowners'


def log_roots(game):
    roots = [(os.path.join(game, 'corruption'), 4), (os.path.join(game, 'GameData'), 2), (game, 1)]
    for env in ('USERPROFILE', 'APPDATA', 'LOCALAPPDATA'):
        v = os.environ.get(env)
        if v:
            roots.append((v, 3 if env != 'USERPROFILE' else 2))
    docs = os.path.join(os.environ.get('USERPROFILE', ''), 'Documents')
    if os.path.isdir(docs):
        roots.append((docs, 4))
    return roots


def find_logs(game, since=None):
    found = []
    for root, depth in log_roots(game):
        if not os.path.isdir(root):
            continue
        base = root.rstrip('\\/').count(os.sep)
        for d, dirs, files in os.walk(root):
            if d.count(os.sep) - base >= depth:
                dirs[:] = []
            dirs[:] = [x for x in dirs if not x.startswith('.') and x.lower() not in ('node_modules', 'temp', 'cache')]
            for fn in files:
                if LOG_NAME in fn.lower() and fn.lower().endswith('.txt'):
                    p = os.path.join(d, fn)
                    if HERE.lower() in p.lower():
                        continue  # our own out\ folder
                    try:
                        if since is None or os.path.getmtime(p) >= since:
                            found.append(p)
                    except OSError:
                        pass
    return sorted(set(found), key=lambda p: -os.path.getsize(p))


def collect_logs(game, run_dir, since=None, prefix=''):
    """Moves the game-written logs into run_dir; returns the new paths (largest first)."""
    out = []
    for p in find_logs(game, since):
        dst = os.path.join(run_dir, prefix + os.path.basename(p))
        n = 1
        while os.path.exists(dst):
            dst = os.path.join(run_dir, '%s%d_%s' % (prefix, n, os.path.basename(p)))
            n += 1
        shutil.move(p, dst)
        with open(os.path.join(run_dir, 'cleanup.log'), 'a') as f:
            f.write('moved %s -> %s\n' % (p, dst))
        out.append(dst)
    return out


def cleanup(game, run_dir, dry_run=False):
    os.makedirs(run_dir, exist_ok=True)
    rep = {'ok': True, 'actions': []}
    mod = os.path.join(game, 'corruption', 'Mods', 'S10MapOwners')
    if not dry_run:  # first: a log the game wrote inside the mod folder must survive the delete
        rep['actions'] += ['moved log -> ' + p for p in collect_logs(game, run_dir, prefix='leftover_')]
    if os.path.isdir(mod):
        if os.path.exists(os.path.join(mod, MARKER)):
            rep['actions'].append('delete ' + mod)
            if not dry_run:
                shutil.rmtree(mod)
        else:
            rep['actions'].append('KEPT %s (no %s: not made by build_mod.py)' % (mod, MARKER))
            rep['ok'] = False
    if dry_run:
        rep['actions'] += ['would move ' + p for p in find_logs(game)]
    if not dry_run and os.path.isdir(mod):
        rep['ok'] = False
    with open(os.path.join(run_dir, 'cleanup.log'), 'a') as f:
        f.write('%s cleanup%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), ' (dry run)' if dry_run else ''))
        for x in rep['actions'] or ['nothing to do']:
            f.write('  ' + x + '\n')
        f.write('CLEANUP_%s\n' % ('OK' if rep['ok'] else 'INCOMPLETE'))
    return rep


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    run_dir = os.path.join(HERE, 'out', 'cleanup-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    rep = cleanup(a.game, run_dir, a.dry_run)
    for x in rep['actions'] or ['nothing to do']:
        print(x)
    print('CLEANUP_OK' if rep['ok'] else 'CLEANUP_INCOMPLETE', '(log in %s)' % run_dir)
    return 0 if rep['ok'] else 40


if __name__ == '__main__':
    sys.exit(main())
