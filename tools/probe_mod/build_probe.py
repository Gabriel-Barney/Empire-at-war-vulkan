#!/usr/bin/env python3
"""build_probe.py (v2) - file-resolution priority probe for FoC (Steam 64-bit 1.121).

  python3 -I build_probe.py <extracted_foc_root> <mastertextfile_english.dat> <out_dir>
  python3 -I build_probe.py probeE <extracted_foc_root> <mastertextfile_english.dat> <out_dir>

Mechanism: ProbeA overrides DATA\\XML\\CAMPAIGNFILES.XML (normally in config.meg) to list
extra campaign files PROBE_G1.XML ... PROBE_G9.XML. Each is a copy of the "Gateways"
sandbox campaign set with a unique Campaign_Set/Sort_Order and a Text_ID that names the
tier that supplied the file, e.g. "PROBE G3 = C (FoC loose Data)". Every probe file is a
separate "contest": it is supplied by a chosen subset of tiers, and the Galactic
Conquest list shows which tier won each contest -- all on one screen, in one launch.

Output layout (<out_dir>):
  Mods/ProbeA, Mods/ProbeB, Mods/ProbeF   -> copy into corruption\\Mods\\
  OPTIONAL_copy_into_corruption_Data      -> tier (c): new files only, delete after
  OPTIONAL_copy_into_GameData_Data        -> tier (e): new files only, delete after
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'meg'))
import megtool  # noqa: E402

# ---------------------------------------------------------------- DAT string table
# u32 count; count x {u32 crc32(key), u32 valueChars, u32 keyBytes} sorted by crc;
# UTF-16LE values; ASCII keys.  (Verified byte-exact round trip on retail FoC english.)


def dat_read(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    recs = [struct.unpack_from('<III', d, 4 + 12 * i) for i in range(n)]
    o = 4 + 12 * n
    vals = []
    for _, vl, _ in recs:
        vals.append(d[o:o + vl * 2].decode('utf-16le'))
        o += vl * 2
    keys = []
    for _, _, kl in recs:
        keys.append(d[o:o + kl].decode('latin-1'))
        o += kl
    if o != len(d):
        raise ValueError('DAT size mismatch')
    return list(zip(keys, vals))


def dat_bytes(pairs):
    pairs = sorted(pairs, key=lambda kv: zlib.crc32(kv[0].encode('latin-1')) & 0xFFFFFFFF)
    out = bytearray(struct.pack('<I', len(pairs)))
    for k, v in pairs:
        out += struct.pack('<III', zlib.crc32(k.encode('latin-1')) & 0xFFFFFFFF, len(v), len(k))
    for _, v in pairs:
        out += v.encode('utf-16le')
    for k, _ in pairs:
        out += k.encode('latin-1')
    return bytes(out)


# ---------------------------------------------------------------- probe definition

TIERS = {
    'A': 'ProbeA loose (1st MODPATH)',
    'B': 'ProbeB loose (2nd MODPATH)',
    'C': 'FoC loose corruption\\Data',
    'D1': 'mod MEG ProbeD1 (listed 1st)',
    'D2': 'mod MEG ProbeD2 (listed 2nd)',
    'D3': 'mod MEG ProbeD3',
    'E': 'EaW loose GameData\\Data',
    'F': 'FALLBACKPATH=Mods\\ProbeF',
}

# contest -> tiers that supply a copy. Predicted winner = first in prediction order
# A > B > C > D2 > D1 > E > F  (D2 > D1: later-listed MEG wins).
CONTESTS = {
    1: ['A', 'B', 'C', 'D2', 'E'],   # expect A   (a beats everything)
    2: ['B', 'C', 'D2', 'E'],        # expect B   (b > c)
    3: ['C', 'D2', 'E'],             # expect C   (c > d)
    4: ['D2', 'E'],                  # expect D2  (d > e)
    5: ['E', 'F'],                   # expect E   (e > fallbackpath)
    6: ['D1', 'D2'],                 # expect D2  (later MEG in one MegaFiles.xml wins)
    7: ['D1'],                       # shown iff MEG lookup ignores case and '/' vs '\\'
    8: ['D3'],                       # stored lower-case/'/', crc over the RAW stored name
    9: ['F'],                        # shown iff FALLBACKPATH is searched at all
}
PREDICTED = {1: 'A', 2: 'B', 3: 'C', 4: 'D2', 5: 'E', 6: 'D2', 7: 'D1', 8: '?', 9: 'F'}

MENU_KEYS = ['TEXT_BUTTON_SINGLE_PLAYER_GAMES', 'TEXT_SINGLE_PLAYER_GAMES_MENU_TITLE',
             'TEXT_BUTTON_MULTIPLAYER', 'TEXT_MULTIPLAYER', 'TEXT_BUTTON_OPTIONS',
             'TEXT_MAIN_OPTIONS', 'TEXT_BUTTON_EXIT_GAME', 'TEXT_BUTTON_QUIT_GAME',
             'TEXT_CREDITS', 'TEXT_MAIN_MENU_TITLE_1']

GC_FILE = 'CAMPAIGNS_UNDERWORLD_GC.XML'
GATEWAYS = ('\t<Campaign Name="Sandbox_Gateways_Underworld">', '\t<Campaign Name="Sandbox_Equal_Footing_Underworld">')


def probe_key(n, t):
    return 'PROBE_G%d_%s' % (n, t)


def make_dat(pairs, tag):
    out = [(k, '[%s] %s' % (tag, v) if k in MENU_KEYS else v) for k, v in pairs]
    for n, tiers in CONTESTS.items():
        for t in tiers:
            out.append((probe_key(n, t), 'PROBE G%d = %s' % (n, t)))
            out.append((probe_key(n, t) + '_DESC',
                        'Probe contest G%d. This copy came from tier %s: %s. Do not start it.' % (n, t, TIERS[t])))
    return dat_bytes(out)


def campaign_file(gc_text, n, tier):
    """Gateways set (3 campaigns) renamed for contest n, labelled with the supplying tier."""
    start = gc_text.index(GATEWAYS[0])
    end = gc_text.index(GATEWAYS[1])
    block = gc_text[start:end]
    block = block[:block.rindex('</Campaign>') + len('</Campaign>')]
    for fac in ('Underworld', 'Rebel', 'Empire'):
        block = block.replace('"Sandbox_Gateways_%s"' % fac, '"Probe_G%d_%s"' % (n, fac))
    block = block.replace('<Campaign_Set> Sandbox_Gateways_Underworld </Campaign_Set>',
                          '<Campaign_Set> Probe_G%d_Set </Campaign_Set>' % n)
    block = block.replace('<Sort_Order>4</Sort_Order>', '<Sort_Order>%d</Sort_Order>' % (20 + n))
    block = block.replace('<Text_ID> TEXT_CAMPAIGN_SINGLE_27 </Text_ID>',
                          '<Text_ID> %s </Text_ID>' % probe_key(n, tier))
    block = block.replace('<Description_Text> TEXT_CAMPAIGN_SINGLE_DESCRIPTION_27 </Description_Text>',
                          '<Description_Text> %s_DESC </Description_Text>' % probe_key(n, tier))
    assert block.count('Probe_G%d_Set' % n) == 3 and block.count(probe_key(n, tier) + ' ') == 3
    return ('<?xml version="1.0"?>\r\n<Campaigns>\r\n\r\n' + block + '\r\n\r\n</Campaigns>\r\n').encode('latin-1')


def campaign_files_xml(orig):
    add = ''.join('\t<File>Probe_G%d.xml</File>\r\n' % n for n in CONTESTS)
    i = orig.index('</Campaign_Files>')
    return (orig[:i] + '\t<!-- probe contests -->\r\n' + add + orig[i:]).encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def main(src_root, dat_path, out):
    xml = os.path.join(src_root, 'Data', 'XML')
    gc_text = open(os.path.join(xml, GC_FILE), 'rb').read().decode('latin-1')
    cf_text = open(os.path.join(xml, 'CAMPAIGNFILES.XML'), 'rb').read().decode('latin-1')
    pairs = dat_read(dat_path)
    if dat_bytes(pairs) != open(dat_path, 'rb').read():
        raise SystemExit('DAT writer round-trip mismatch; refusing to build')

    roots = {
        'A': os.path.join(out, 'Mods', 'ProbeA', 'Data'),
        'B': os.path.join(out, 'Mods', 'ProbeB', 'Data'),
        'F': os.path.join(out, 'Mods', 'ProbeF', 'Data'),
        'C': os.path.join(out, 'OPTIONAL_copy_into_corruption_Data', 'Data'),
        'E': os.path.join(out, 'OPTIONAL_copy_into_GameData_Data', 'Data'),
    }
    megs = {'D1': [], 'D2': [], 'D3': []}
    for n, tiers in CONTESTS.items():
        for t in tiers:
            data = campaign_file(gc_text, n, t)
            if t in roots:
                write(os.path.join(roots[t], 'XML', 'Probe_G%d.xml' % n), data)
            else:
                megs[t].append((n, data))

    # Tier (a) also carries: CampaignFiles.xml override (itself a loose-over-base-MEG test),
    # the mod MegaFiles.xml, and the three MEGs.
    write(os.path.join(roots['A'], 'XML', 'CampaignFiles.xml'), campaign_files_xml(cf_text))
    write(os.path.join(roots['A'], 'Text', 'MasterTextFile_English.dat'), make_dat(pairs, 'A'))
    write(os.path.join(roots['B'], 'Text', 'MasterTextFile_English.dat'), make_dat(pairs, 'B'))

    # D1: G6 (normal name) + G7 stored as lower-case with '/' but crc of the normalised name
    d1 = []
    for n, data in megs['D1']:
        if n == 7:
            d1.append(('data/xml/probe_g7.xml', data, megtool.meg_crc('DATA\\XML\\PROBE_G7.XML')))
        else:
            d1.append(('DATA\\XML\\PROBE_G%d.XML' % n, data, None))
    write_meg(os.path.join(roots['A'], 'ProbeD1.meg'), d1)
    write_meg(os.path.join(roots['A'], 'ProbeD2.meg'),
              [('DATA\\XML\\PROBE_G%d.XML' % n, data, None) for n, data in megs['D2']])
    # D3: G8 stored lower-case with '/', crc over the RAW stored bytes (what a naive tool writes)
    raw = 'data/xml/probe_g8.xml'
    write_meg(os.path.join(roots['A'], 'ProbeD3.meg'),
              [(raw, megs['D3'][0][1], zlib.crc32(raw.encode()) & 0xFFFFFFFF)])

    # Mod MegaFiles.xml: per the RE prediction a mod's MegaFiles.xml REPLACES the base list,
    # so it repeats the retail FoC list and appends the probe MEGs (later-listed wins).
    base = ['Data\\RussianSpeech.meg', 'Data\\FrenchSpeech.meg', 'Data\\Config.meg', 'Data\\Shaders.meg',
            'Data\\Cinematics.meg', 'Data\\SpanishSpeech.meg', 'Data\\Maps.meg', 'Data\\Movies.meg',
            'Data\\Models.meg', 'Data\\ItalianSpeech.meg', 'Data\\PolishSpeech.meg', 'Data\\EnglishSpeech.meg',
            'Data\\Textures.meg', 'Data\\Music.meg', 'Data\\GermanSpeech.meg']
    lines = ''.join('\t<File> %s </File>\r\n' % f for f in base + ['Data\\ProbeD3.meg', 'Data\\ProbeD1.meg', 'Data\\ProbeD2.meg'])
    write(os.path.join(roots['A'], 'MegaFiles.xml'),
          ('<?xml version="1.0" ?>\r\n\r\n<Mega_Files>\r\n\r\n' + lines + '\r\n</Mega_Files>\r\n').encode('latin-1'))
    print('built probe v2 in', out)


def write_meg(path, items):
    """items: (stored_name, data, crc_override or None). Records sorted by the crc used."""
    names = b''.join(struct.pack('<H', len(n)) + n.encode('latin-1') for n, _, _ in items)
    data_start = 8 + len(names) + 20 * len(items)
    offs, cur = [], data_start
    for _, d, _ in items:
        offs.append(cur)
        cur += len(d)
    crcs = [c if c is not None else megtool.meg_crc(n) for n, _, c in items]
    order = sorted(range(len(items)), key=lambda i: crcs[i])
    out = bytearray(struct.pack('<II', len(items), len(items))) + names
    for r, i in enumerate(order):
        out += struct.pack('<5I', crcs[i], r, len(items[i][1]), offs[i], i)
    for _, d, _ in items:
        out += d
    write(path, bytes(out))


# ---------------------------------------------------------------- Probe E
# FALLBACKPATH chain-vs-replace (docs/re/SUBSYSTEMS.md, A3). Two GC entries:
#   E1: only in GameData\Data             -> shown "E1-GAMEDATA" iff GameData is still searched
#   E2: in FALLBACKPATH dir and GameData  -> "E2-FB" if FALLBACKPATH is searched before GameData


def gc_entry(gc_text, ident, sort_order, key):
    start = gc_text.index(GATEWAYS[0])
    block = gc_text[start:gc_text.index(GATEWAYS[1])]
    block = block[:block.rindex('</Campaign>') + len('</Campaign>')]
    for fac in ('Underworld', 'Rebel', 'Empire'):
        block = block.replace('"Sandbox_Gateways_%s"' % fac, '"%s_%s"' % (ident, fac))
    block = block.replace('<Campaign_Set> Sandbox_Gateways_Underworld </Campaign_Set>',
                          '<Campaign_Set> %s_Set </Campaign_Set>' % ident)
    block = block.replace('<Sort_Order>4</Sort_Order>', '<Sort_Order>%d</Sort_Order>' % sort_order)
    block = block.replace('<Text_ID> TEXT_CAMPAIGN_SINGLE_27 </Text_ID>', '<Text_ID> %s </Text_ID>' % key)
    block = block.replace('<Description_Text> TEXT_CAMPAIGN_SINGLE_DESCRIPTION_27 </Description_Text>',
                          '<Description_Text> %s </Description_Text>' % key)
    assert block.count('%s_Set' % ident) == 3 and block.count(' %s ' % key) == 6
    return ('<?xml version="1.0"?>\r\n<Campaigns>\r\n\r\n' + block + '\r\n\r\n</Campaigns>\r\n').encode('latin-1')


def build_probe_e(src_root, dat_path, out):
    xml = os.path.join(src_root, 'Data', 'XML')
    gc_text = open(os.path.join(xml, GC_FILE), 'rb').read().decode('latin-1')
    cf_text = open(os.path.join(xml, 'CAMPAIGNFILES.XML'), 'rb').read().decode('latin-1')
    pairs = dat_read(dat_path)
    if dat_bytes(pairs) != open(dat_path, 'rb').read():
        raise SystemExit('DAT writer round-trip mismatch; refusing to build')
    tags = {'PROBE_E1_GAMEDATA': 'PROBE E1-GAMEDATA', 'PROBE_E2_FB': 'PROBE E2-FB',
            'PROBE_E2_GAMEDATA': 'PROBE E2-GAMEDATA'}
    mod = os.path.join(out, 'Mods', 'ProbeE', 'Data')
    fb = os.path.join(out, 'Mods', 'ProbeE_FB', 'Data')
    gd = os.path.join(out, 'OPTIONAL_copy_into_GameData_Data', 'Data')
    add = '\t<!-- probe E -->\r\n\t<File>Probe_E1.xml</File>\r\n\t<File>Probe_E2.xml</File>\r\n'
    i = cf_text.index('</Campaign_Files>')
    write(os.path.join(mod, 'XML', 'CampaignFiles.xml'), (cf_text[:i] + add + cf_text[i:]).encode('latin-1'))
    write(os.path.join(mod, 'Text', 'MasterTextFile_English.dat'),
          dat_bytes([(k, '[E] %s' % v if k in MENU_KEYS else v) for k, v in pairs] + list(tags.items())))
    write(os.path.join(gd, 'XML', 'Probe_E1.xml'), gc_entry(gc_text, 'Probe_E1', 41, 'PROBE_E1_GAMEDATA'))
    write(os.path.join(gd, 'XML', 'Probe_E2.xml'), gc_entry(gc_text, 'Probe_E2', 42, 'PROBE_E2_GAMEDATA'))
    write(os.path.join(fb, 'XML', 'Probe_E2.xml'), gc_entry(gc_text, 'Probe_E2', 42, 'PROBE_E2_FB'))
    print('built probe E in', out)


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == 'probeE':
        build_probe_e(*sys.argv[2:])
        sys.exit(0)
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    main(*sys.argv[1:])
