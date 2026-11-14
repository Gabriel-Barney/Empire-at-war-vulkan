#!/usr/bin/env python3
"""cu8g_cleanup.py - removes everything the cu8g probes put into the game folder. Safe to run any number of times;
cu8g_run.py calls it at the end of every session (also after an error or Ctrl+C).

  cu8g_cleanup.py [--game DIR] [--dry-run] [--log FILE] [--no-pause]

1. <game>\\corruption\\Mods\\CU8GProbe is deleted only if it holds CU8G_PROBE_MARKER.txt (build_cu8g_mod.py made it).
2. maps-hit --map-loose only: <game>\\corruption\\Data\\Art\\Maps\\_MP_LAND_CU8G_ALDERAAN.TED is deleted only when its
   marker _MP_LAND_CU8G_ALDERAAN.TED.cu8g_probe.txt sits next to it and the file's SHA-256 matches the marker; then the
   marker, and the folders the probe created (listed in the marker) if they are empty. No retail file has that name.
No retail file is touched, nothing else is deleted. Never save a probe campaign (a save would reference the mod).
"""
import hashlib
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import probe_cleanup  # noqa: E402

DEFAULT_GAME = probe_cleanup.DEFAULT_GAME
MOD_NAMES = ('CU8GProbe',)
MARKER = 'CU8G_PROBE_MARKER.txt'
LOOSE_MAP = '_MP_LAND_CU8G_ALDERAAN.TED'
LOOSE_SUFFIX = '.cu8g_probe.txt'


def cleanup_loose(game, dry_run=False):
    rep = {'ok': True, 'actions': []}
    d = os.path.join(game, 'corruption', 'Data', 'Art', 'Maps')
    path, marker = os.path.join(d, LOOSE_MAP), os.path.join(d, LOOSE_MAP + LOOSE_SUFFIX)
    if not os.path.exists(marker):
        if os.path.exists(path):
            rep['actions'].append('left %s: no %s marker, not made by the probe' % (path, LOOSE_SUFFIX))
        return rep
    try:
        info = json.load(open(marker))
    except (OSError, ValueError) as e:
        rep['actions'].append('left %s: unreadable marker (%s)' % (path, e))
        rep['ok'] = False
        return rep
    if os.path.exists(path):
        sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if sha != info.get('sha256'):
            rep['actions'].append('left %s: content differs from the probe copy' % path)
            rep['ok'] = False
            return rep
        if not dry_run:
            os.remove(path)
        rep['actions'].append('%s %s' % ('would delete' if dry_run else 'deleted', path))
    if not dry_run:
        os.remove(marker)
    rep['actions'].append('%s %s' % ('would delete' if dry_run else 'deleted', marker))
    for c in info.get('created_dirs', []):   # deepest first (build_cu8g_mod.install_loose_map order)
        if dry_run:
            rep['actions'].append('would remove %s if empty' % c)
        elif os.path.isdir(c) and not os.listdir(c):
            os.rmdir(c)
            rep['actions'].append('removed empty %s' % c)
    if not dry_run and os.path.exists(path):
        rep['ok'] = False
        rep['actions'].append('COULD NOT DELETE ' + path)
    return rep


def cleanup(game, log=None, dry_run=False):
    rep = probe_cleanup.cleanup(game, MOD_NAMES, MARKER, None, dry_run)
    loose = cleanup_loose(game, dry_run)
    rep['actions'] += loose['actions']
    rep['ok'] = rep['ok'] and loose['ok']
    if log:
        import time
        with open(log, 'a') as f:
            f.write('%s cleanup ok=%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), rep['ok']))
            for a in rep['actions']:
                f.write('  ' + a + '\n')
    return rep


if __name__ == '__main__':
    sys.exit(probe_cleanup.run_main(__doc__, cleanup, 'CU8G_CLEANUP'))
