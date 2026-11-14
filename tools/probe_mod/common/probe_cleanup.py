#!/usr/bin/env python3
"""probe_cleanup.py - the cleanup every mod-installing probe kit shares (cu18 t01, 2026-11-07; it was copied into
eight <kit>_cleanup.py scripts).

<game>\\corruption\\Mods\\<name> is deleted only if it holds the kit's marker file (the kit's builder writes it), so a
folder the probe did not make is never touched. That folder is the only thing these probes add; no retail file is
touched and nothing else is deleted. Each kit's <kit>_cleanup.py names its folder(s) and marker and calls cleanup();
its main() is run_main() with the kit's docstring and result tag.
"""
import argparse
import os
import shutil
import time

DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'


def cleanup(game, mod_names, marker, log=None, dry_run=False):
    """Delete each <game>\\corruption\\Mods\\<name> that holds `marker`. Returns {'ok': bool, 'actions': [str]}; with
    `log`, appends a time-stamped summary to that file."""
    rep = {'ok': True, 'actions': []}
    for name in mod_names:
        mod = os.path.join(game, 'corruption', 'Mods', name)
        if not os.path.isdir(mod):
            rep['actions'].append('nothing to remove (%s absent)' % mod)
            continue
        if not os.path.exists(os.path.join(mod, marker)):
            rep['actions'].append('left %s: no %s, not made by the probe' % (mod, marker))
            continue
        if not dry_run:
            shutil.rmtree(mod, ignore_errors=True)
        gone = dry_run or not os.path.exists(mod)
        rep['actions'].append('%s %s' % ('would delete' if dry_run else ('deleted' if gone else 'COULD NOT DELETE'), mod))
        rep['ok'] = rep['ok'] and gone
    if log:
        with open(log, 'a') as f:
            f.write('%s cleanup ok=%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), rep['ok']))
            for a in rep['actions']:
                f.write('  ' + a + '\n')
    return rep


def arg_parser(doc):
    ap = argparse.ArgumentParser(description=doc, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--log')
    ap.add_argument('--no-pause', action='store_true', help='for the .cmd wrapper: end without waiting for a key')
    return ap


def run_main(doc, kit_cleanup, tag):
    """The command line of a kit's <kit>_cleanup.py: prints the actions and '<tag> OK|INCOMPLETE'; exit 0 when ok."""
    a = arg_parser(doc).parse_args()
    rep = kit_cleanup(a.game, a.log, a.dry_run)
    for x in rep['actions']:
        print(x)
    print('%s %s' % (tag, 'OK' if rep['ok'] else 'INCOMPLETE'))
    return 0 if rep['ok'] else 1
