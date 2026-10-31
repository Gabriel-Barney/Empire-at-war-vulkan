#!/usr/bin/env python3
"""build_a1_mod.py - builds the A1 tactical-AI probe mod (Mods\\A1TacticalProbe) from the installed game's own files.

  build_a1_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-campaign]
  build_a1_mod.py --caps-only OUT.json [--game DIR]

One mod folder serves both probes (one MODPATH; nothing from the game is stored in the repository):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   free-store timing probe (A1 t02): the shipped script, unchanged,
                                                       + freestore_timing\\fs_hook.lua appended (logging wrappers only)
  landing-point probe (A1 t04) campaign, reused from tools\\probe_mod\\s10_map_owners\\build_mod.py (proven in the
  2026-10-29 S10 run 20261029-074311 to start the land battle by itself): "S10 PROBE A: TATOOINE" (play the EMPIRE;
  the AI Underworld invades Tatooine 3 s after the start) and "S10 PROBE B: JABIIM" (play the REBELS):
  Data\\XML\\CampaignFiles.xml, S10_Probe_Campaign.xml, S10_Story_Plots_<A|B>.xml, S10_Story_<A|B>.xml,
  Data\\Scripts\\Story\\S10_Story_<A|B>.lua, Data\\Text\\MasterTextFile_English.dat. The S10 GameScoring.lua hook is
  NOT included (this probe reads ownership from memory), so skirmish scoring runs the shipped script.
  A1_PROBE_MARKER.txt                                  lets cleanup recognise the folder as ours.
--caps-only writes {TYPE_NAME: Additional_Population_Capacity} (non-zero values, Variant_Of_Existing_Type
resolved) from the install's MARKERS.XML / PROPS_STORY.XML, the land types that carry one (a1_reader --caps).
"""
import argparse
import json
import os
import re
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
S10 = os.path.join(os.path.dirname(HERE), 's10_map_owners')
sys.path.insert(0, HERE)
sys.path.insert(0, S10)
sys.path.insert(0, os.path.dirname(HERE))
import ted_owners  # noqa: E402

MOD_NAME = 'A1TacticalProbe'
MARKER = 'A1_PROBE_MARKER.txt'
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
CAP_FILES = ('DATA\\XML\\MARKERS.XML', 'DATA\\XML\\PROPS_STORY.XML')
HOOK = os.path.join(HERE, 'freestore_timing', 'fs_hook.lua')


def read_shipped(game, name, source_zip=None):
    """(bytes, source label) of a shipped Data file: the install in FoC order, or the extracted text zip."""
    if source_zip:
        z = zipfile.ZipFile(source_zip)
        want = name.upper().replace('/', '\\')
        for n in z.namelist():
            if n.upper().replace('/', '\\') == want:
                return z.read(n), source_zip + ':' + n
        return None, None
    return ted_owners.GameFiles(game).read(name)


def freestore_text(game, source_zip=None):
    data, src = read_shipped(game, FREESTORE, source_zip)
    if data is None:
        raise SystemExit('shipped BusyTacticalFreeStore.lua not found (game %s, zip %s)' % (game, source_zip))
    text = data.decode('latin-1').replace('\r\n', '\n')
    hook = open(HOOK, encoding='ascii').read()
    return (text.rstrip('\n') + '\n\n' + hook).replace('\n', '\r\n').encode('latin-1'), src


def caps_table(game, source_zip=None):
    types = {}
    for name in CAP_FILES:
        data, _ = read_shipped(game, name, source_zip)
        if data is None:
            continue
        text = data.decode('latin-1')
        for m in re.finditer(r'<(\w+)\s+Name\s*=\s*"([^"]+)"[^>]*>(.*?)</\1>', text, re.S):
            body = m.group(3)
            cap = re.search(r'<Additional_Population_Capacity>\s*([-\d.]+)\s*<', body)
            var = re.search(r'<Variant_Of_Existing_Type>\s*([^<\s]+)\s*<', body)
            types[m.group(2).upper()] = (float(cap.group(1)) if cap else None, var.group(1).upper() if var else None)

    def value(t, depth=0):
        cap, var = types.get(t, (None, None))
        if cap is not None or var is None or depth > 8:
            return cap
        return value(var, depth + 1)

    out = {}
    for t in types:
        v = value(t)
        if v:
            out[t] = int(v) if v == int(v) else v
    return out


def build_campaign(game, out):
    """The S10 campaign vehicle (s10_map_owners/build_mod.py) into out, without its GameScoring hook and without
    rewriting s10's tracked expected_objects.json."""
    import build_mod as s10_build  # noqa: E402 - s10_map_owners/build_mod.py

    def expected_from_file(g):
        with open(os.path.join(S10, 'expected_objects.json')) as f:
            return ted_owners.GameFiles(g), json.load(f)

    s10_build.build_expected = expected_from_file
    s10_build.build(game, out, False)
    os.remove(os.path.join(out, 'Data', 'Scripts', 'Miscellaneous', 'GameScoring.lua'))
    os.remove(os.path.join(out, s10_build.MARKER))


def build(game, out, source_zip=None, campaign=True):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an A1 probe folder' % out)
        shutil.rmtree(out)
    if campaign:
        build_campaign(game, out)
    fs, src = freestore_text(game, source_zip)
    p = os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(fs)
    with open(os.path.join(out, MARKER), 'wb') as f:
        f.write(b'A1 tactical-AI probe mod (tools/probe_mod/a1_tactical); removed by cleanup_a1.cmd\r\n')
    print('built %s (BusyTacticalFreeStore.lua from %s, campaign=%s)' % (out, src, campaign))
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    ap.add_argument('--source-zip', help='read the shipped free-store script from this zip (offline check)')
    ap.add_argument('--no-campaign', action='store_true')
    ap.add_argument('--caps-only', metavar='OUT_JSON')
    a = ap.parse_args()
    if a.caps_only:
        caps = caps_table(a.game, a.source_zip)
        with open(a.caps_only, 'w') as f:
            json.dump(caps, f, indent=1, sort_keys=True)
        print('wrote %s (%d types)' % (a.caps_only, len(caps)))
        return
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.source_zip, not a.no_campaign)


if __name__ == '__main__':
    main()
