#!/usr/bin/env python3
"""build_modc_mod.py - builds the modc t13 / t14 loader probe mod from the installed game.

  build_modc_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]

Nothing from the game is stored in the repository: CampaignFiles.xml, GameObjectFiles.xml and MasterTextFile_English.dat
are read from the install (loose files / MEGs in the FoC order, through tools/probe_mod/common/ted_owners.py)
and patched. --source-zip reads the two XML lists from a text zip instead (offline check; the zip has no .dat, so pass
--no-text with it).

Mods\\ModcLoaderProbe (vanilla FoC, MODPATH=Mods\\ModcLoaderProbe):
  Data\\XML\\CampaignFiles.xml                     shipped list + Modc_Probe_Campaigns.xml + Modc_Probe_Digit_Campaigns.xml
  Data\\XML\\Modc_Probe_Campaigns.xml              MODC PROBE 1: BOM SCRIPT, MODC PROBE 2: CONTROL (normal root element)
  Data\\XML\\Modc_Probe_Digit_Campaigns.xml        MODC PROBE 3: DIGIT ROOT, root element <1Modc_Probe_Campaigns>
  Data\\XML\\Modc_Story_Plots_<Bom|Plain>.xml, Modc_Story_<Bom|Plain>.xml   one plot per campaign
  Data\\Scripts\\Story\\Modc_Probe_Bom.lua          modc_probe_story.lua with a UTF-8 BOM in front (t13)
  Data\\Scripts\\Story\\Modc_Probe_Plain.lua        modc_probe_story.lua without BOM (control; looks up the t14 units)
  Data\\XML\\GameObjectFiles.xml                   shipped list + the three probe unit files
  Data\\XML\\Modc_Probe_Units_Plain.xml            root <Modc_Probe_Units>, unit Modc_Probe_Plain_Unit (control)
  Data\\XML\\Modc_Probe_Units_Digit.xml            root <80s_Modc_Probe_Units>, unit Modc_Probe_Digit_Unit (as 1770851727's
                                                  Units_Hero_Minors_CSA_Tagge.xml: no BOM, digit root)
  Data\\XML\\Modc_Probe_Units_Bom_Digit.xml        BOM + root <181st_Modc_Probe_Units>, unit Modc_Probe_Bom_Digit_Unit (as
                                                  1770851727's Units_Hero_Empire_181st_Fighter_Wing.xml)
  Data\\Text\\MasterTextFile_English.dat           shipped text + TEXTS
  canary_*.txt                                    deleted by the probe scripts (modc_probe_story.lua)
  MODC_LOADER_PROBE_MARKER.txt
"""
import argparse
import os
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
sys.path.insert(0, os.path.dirname(HERE))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'ModcLoaderProbe'
MARKER = 'MODC_LOADER_PROBE_MARKER.txt'
BOM = b'\xef\xbb\xbf'

CAMPAIGNFILES = 'DATA\\XML\\CAMPAIGNFILES.XML'
GAMEOBJECTFILES = 'DATA\\XML\\GAMEOBJECTFILES.XML'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'

# canary file -> what its deletion proves (modc_run.py prints these)
CANARIES = {
    'canary_plain_script.txt': 'control: the plain story script ran (the probe hook works)',
    'canary_bom_script.txt': 't13: the script WITH a UTF-8 BOM ran, so the original skips the BOM',
    'canary_unit_plain_root.txt': 'control: a unit from a normal-root unit file exists',
    'canary_unit_digit_root.txt': 't14: a unit from a file whose root element is <80s_...> exists',
    'canary_unit_bom_digit_root.txt': 't14: a unit from a BOM file whose root element is <181st_...> exists',
}

# Galaxy as the Q6 probes: Tatooine, Byss, Abregado Rae, Jabiim. Human Empire, every AI None: nothing moves.
CAMPAIGN = r'''	<Campaign Name="Modc_Probe_{key}">
		<Campaign_Set> Modc_Probe_{key}_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_MODC_CAMP_{key}_NAME </Text_ID>
		<Description_Text> TEXT_MODC_CAMP_{key}_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>
		<Locations>
				Galaxy_Core_Art_Model,
				Tatooine,
				Byss,
				Abregado_Rae,
				Jabiim
		</Locations>
		<Trade_Routes>
				Byss_Tatooine,
				Abregado_Rae_Byss,
				Abregado_Rae_Jabiim
		</Trade_Routes>
		<Home_Location> Rebel, Jabiim </Home_Location>
		<Home_Location> Empire, Tatooine </Home_Location>
		<Starting_Active_Player> Empire </Starting_Active_Player>
{story}		<AI_Player_Control> Rebel, None </AI_Player_Control>
		<AI_Player_Control> Underworld, None </AI_Player_Control>
		<AI_Player_Control> Pirates, None </AI_Player_Control>
		<Human_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</Human_Victory_Conditions>
		<AI_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</AI_Victory_Conditions>
		<Markup_Filename>Empire, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Pirates, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Rebel, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Underworld, DefaultGalacticHints </Markup_Filename>
		<Supports_Custom_Settings>False</Supports_Custom_Settings>
		<Show_Completed_Tab>False</Show_Completed_Tab>
		<Starting_Credits> Rebel, 0 </Starting_Credits>
		<Starting_Tech_Level> Rebel, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Rebel, 1 </Max_Tech_Level>
		<Starting_Forces> Rebel, Jabiim, R_Ground_Barracks </Starting_Forces>
		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 1 </Max_Tech_Level>
		<Starting_Forces> Empire, Tatooine, E_Ground_Barracks </Starting_Forces>
	</Campaign>
'''

# key -> (story script or None, menu name, description)
CAMPAIGNS = {
    'Bom': ('Modc_Probe_Bom', 'MODC PROBE 1: BOM SCRIPT',
            'modc t13. Play the EMPIRE, click nothing for 15 seconds, note the lines on screen, quit (do not save).'),
    'Plain': ('Modc_Probe_Plain', 'MODC PROBE 2: CONTROL',
              'modc control and t14 units. Play the EMPIRE, click nothing for 15 seconds, note the lines on screen, '
              'quit (do not save).'),
    'Digit': (None, 'MODC PROBE 3: DIGIT ROOT',
              'modc t14. This entry is listed only if the game accepts a root element named <1Modc_Probe_Campaigns>. '
              'Do not start it.'),
    'Empty': (None, 'MODC PROBE 4: NO PLANETS',
              'modc t14. A campaign without Locations, Trade_Routes or forces (as the menu rows of workshop mod '
              '1770851727). Note whether it is listed. Do not start it.'),
}

# A campaign with only menu fields, as Campaigns_Dummies.xml of workshop mod 1770851727.
EMPTY_CAMPAIGN = r'''	<Campaign Name="Modc_Probe_Empty">
		<Campaign_Set> Modc_Probe_Empty_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_MODC_CAMP_Empty_NAME </Text_ID>
		<Description_Text> TEXT_MODC_CAMP_Empty_DESC </Description_Text>
		<Starting_Active_Player> Hostile </Starting_Active_Player>
	</Campaign>
'''

PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>{lua}</Lua_Script>
	<Active_Plot>Modc_Story_{key}.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''

STORY = '''<?xml version="1.0" ?>
<!-- modc loader probe (tools/probe_mod/modc_loader) -->
<Story>
	<Event Name="Modc_Begin">
		<Event_Type>STORY_ELAPSED</Event_Type>
		<Event_Param1>2</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_MODC_{key_upper}_PLOT</Reward_Param1>
		<Reward_Param2>15</Reward_Param2>
	</Event>
</Story>
'''

# file -> (root element, unit name, BOM)
UNIT_FILES = {
    'Modc_Probe_Units_Plain.xml': ('Modc_Probe_Units', 'Modc_Probe_Plain_Unit', False),
    'Modc_Probe_Units_Digit.xml': ('80s_Modc_Probe_Units', 'Modc_Probe_Digit_Unit', False),
    'Modc_Probe_Units_Bom_Digit.xml': ('181st_Modc_Probe_Units', 'Modc_Probe_Bom_Digit_Unit', True),
}

UNIT = '''<?xml version="1.0"?>
<{root}>

<!-- modc loader probe (tools/probe_mod/modc_loader): one TIE fighter variant -->
	<UniqueUnit Name="{name}">
		<Variant_Of_Existing_Type>TIE_Fighter</Variant_Of_Existing_Type>
	</UniqueUnit>

</{root}>
'''

TEXTS = [
    ('TEXT_MODC_BOM_PLOT', 'MODC PROBE: plot 1 loaded (story XML event, no Lua)'),
    ('TEXT_MODC_PLAIN_PLOT', 'MODC PROBE: plot 2 loaded (story XML event, no Lua)'),
    ('TEXT_MODC_BOM_RAN', 'MODC PROBE: the BOM script RAN'),
    ('TEXT_MODC_PLAIN_RAN', 'MODC PROBE: the control script ran'),
    ('TEXT_MODC_UNIT_PLAIN', 'MODC PROBE: unit from the normal-root file exists'),
    ('TEXT_MODC_UNIT_DIGIT', 'MODC PROBE: unit from the <80s_...> root file EXISTS'),
    ('TEXT_MODC_UNIT_BOM_DIGIT', 'MODC PROBE: unit from the BOM <181st_...> root file EXISTS'),
    ('TEXT_MODC_DONE', 'MODC PROBE: script done. Quit to the main menu (do not save).'),
] + [('TEXT_MODC_CAMP_%s_NAME' % k, v[1]) for k, v in CAMPAIGNS.items()] + \
    [('TEXT_MODC_CAMP_%s_DESC' % k, v[2]) for k, v in CAMPAIGNS.items()]


class Source:
    """Shipped files from the install (GameFiles) or from a text zip (offline check, no .dat inside)."""

    def __init__(self, game, source_zip=None):
        self.zip = zipfile.ZipFile(source_zip) if source_zip else None
        self.names = {n.upper().replace('/', '\\'): n for n in self.zip.namelist()} if self.zip else None
        self.gf = None if self.zip else ted_owners.GameFiles(game)

    def read(self, name):
        if self.zip:
            n = self.names.get(name.upper())
            return (self.zip.read(n), 'zip:' + n) if n else (None, None)
        return self.gf.read(name)


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def add_to_list(data, close_tag, files):
    """Inserts <File> lines before the list's closing tag."""
    t = data.decode('latin-1')
    i = t.rindex(close_tag)
    return (t[:i] + ''.join('\t<File>%s</File>\r\n' % f for f in files) + t[i:]).encode('latin-1')


def prepare(out):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a modc loader probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER),
          b'modc t13/t14 loader probe mod (tools/probe_mod/modc_loader); removed by cleanup_modc.cmd\r\n')


def build(game, out, source_zip=None, no_text=False):
    src = Source(game, source_zip)
    prepare(out)
    xml = os.path.join(out, 'Data', 'XML')
    cf, cf_src = src.read(CAMPAIGNFILES)
    gof, gof_src = src.read(GAMEOBJECTFILES)
    if cf is None or gof is None:
        raise SystemExit('shipped CampaignFiles.xml / GameObjectFiles.xml not found')
    write(os.path.join(xml, 'CampaignFiles.xml'),
          add_to_list(cf, '</Campaign_Files>', ['Modc_Probe_Campaigns.xml', 'Modc_Probe_Digit_Campaigns.xml']))
    write(os.path.join(xml, 'GameObjectFiles.xml'), add_to_list(gof, '</', list(UNIT_FILES)))
    normal, digit = [], []
    for key, (lua, _, _) in CAMPAIGNS.items():
        story = ''
        if lua:
            story = '\t\t<Empire_Story_Name>Modc_Story_Plots_%s.xml</Empire_Story_Name>\n' % key
            write(os.path.join(xml, 'Modc_Story_Plots_%s.xml' % key), crlf(PLOTS.format(lua=lua, key=key)))
            write(os.path.join(xml, 'Modc_Story_%s.xml' % key), crlf(STORY.format(key_upper=key.upper())))
        if key == 'Empty':
            normal.append(EMPTY_CAMPAIGN)
        else:
            (digit if key == 'Digit' else normal).append(CAMPAIGN.format(key=key, story=story))
    write(os.path.join(xml, 'Modc_Probe_Campaigns.xml'),
          crlf('<?xml version="1.0"?>\n<Campaigns>\n\t<!-- modc loader probe (tools/probe_mod/modc_loader) -->\n%s'
               '</Campaigns>\n' % ''.join(normal)))
    write(os.path.join(xml, 'Modc_Probe_Digit_Campaigns.xml'),
          crlf('<?xml version="1.0"?>\n<1Modc_Probe_Campaigns>\n\t<!-- modc loader probe: root element name starts '
               'with a digit -->\n%s</1Modc_Probe_Campaigns>\n' % ''.join(digit)))
    for f, (root, name, bom) in UNIT_FILES.items():
        write(os.path.join(xml, f), (BOM if bom else b'') + crlf(UNIT.format(root=root, name=name)))
    story_lua = open(os.path.join(HERE, 'modc_probe_story.lua'), encoding='ascii').read()
    sdir = os.path.join(out, 'Data', 'Scripts', 'Story')
    write(os.path.join(sdir, 'Modc_Probe_Bom.lua'), BOM + crlf(story_lua.replace('@@KIND@@', 'BOM')))
    write(os.path.join(sdir, 'Modc_Probe_Plain.lua'), crlf(story_lua.replace('@@KIND@@', 'PLAIN')))
    for c in CANARIES:
        write(os.path.join(out, c), b'modc loader probe canary: the probe script deletes this file when it runs\r\n')
    n_text = 0
    if not no_text:
        mt, _ = src.read(MASTERTEXT)
        if mt is None:
            raise SystemExit('shipped MasterTextFile_English.dat not found (use --no-text for an offline check)')
        tmp = os.path.join(out, 'mt.tmp')
        write(tmp, mt)
        keys = {k for k, _ in TEXTS}
        pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
        os.remove(tmp)
        write(os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat'), dat_bytes(pairs + TEXTS))
        n_text = len(TEXTS)
    print('built %s (CampaignFiles from %s, GameObjectFiles from %s, %d probe strings)' % (out, cf_src, gof_src, n_text))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    a = ap.parse_args()
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.source_zip, a.no_text)


if __name__ == '__main__':
    main()
