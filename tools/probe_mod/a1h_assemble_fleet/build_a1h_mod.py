#!/usr/bin/env python3
"""build_a1h_mod.py - builds the a1h t09 Assemble_Fleet probe mod (Mods\\A1hAssembleProbe) from the installed game.

  build_a1h_mod.py [--game DIR] [--out MODDIR]

Nothing from the game is stored in the repository: the shipped CampaignFiles.xml and MasterTextFile_English.dat are
read from the install (loose files / MEGs, FoC file order, through tools/probe_mod/common/ted_owners.py) and
patched:
  Data\\XML\\CampaignFiles.xml                 shipped list + A1h_Probe_Campaign.xml
  Data\\XML\\A1h_Probe_Campaign.xml            "A1H PROBE: ASSEMBLE FLEET" (human Empire, everyone else AI None)
  Data\\XML\\A1h_Story_Plots.xml, A1h_Story.xml   own story plot (one event A1H_Begin, STORY_ELAPSED 3 s)
  Data\\Scripts\\Story\\A1h_Story.lua           a1h_story.lua: the four Assemble_Fleet cases, records, verdicts
  Data\\Text\\MasterTextFile_English.dat       shipped text + TEXTS
  A1H_PROBE_MARKER.txt                        lets cleanup recognise the folder as ours
"""
import argparse
import os
import shutil
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
sys.path.insert(0, os.path.dirname(HERE))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'A1hAssembleProbe'
MARKER = 'A1H_PROBE_MARKER.txt'

# Galaxy: Tatooine (Empire home: barracks + 2 stormtrooper squads on the surface = case C), Abregado Rae (Empire
# barracks = case A), Byss (Underworld home = case B), Jabiim (Rebel home, only so that three factions hold a planet).
# No space units or stations anywhere, every non-human player AI None: nothing moves unless the story Lua does it.
CAMPAIGN = r'''<?xml version="1.0"?>
<Campaigns>
	<!-- a1h t09 probe (tools/probe_mod/a1h_assemble_fleet): the story Lua A1h_Story runs four Assemble_Fleet cases. -->
	<Campaign Name="A1h_Probe_Assemble_Empire">
		<Campaign_Set> A1h_Probe_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_A1H_PROBE_NAME </Text_ID>
		<Description_Text> TEXT_A1H_PROBE_DESC </Description_Text>
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
		<Home_Location> Underworld, Byss </Home_Location>

		<Starting_Active_Player> Empire </Starting_Active_Player>

		<Empire_Story_Name>A1h_Story_Plots.xml</Empire_Story_Name>

		<AI_Player_Control> Rebel, None </AI_Player_Control>
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
		<Starting_Forces> Empire, Tatooine, Imperial_Stormtrooper_Squad </Starting_Forces>
		<Starting_Forces> Empire, Tatooine, Imperial_Stormtrooper_Squad </Starting_Forces>
		<Starting_Forces> Empire, Abregado_Rae, E_Ground_Barracks </Starting_Forces>

		<Starting_Credits> Underworld, 0 </Starting_Credits>
		<Starting_Tech_Level> Underworld, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Underworld, 1 </Max_Tech_Level>
		<Starting_Forces> Underworld, Byss, U_Ground_Barracks </Starting_Forces>
	</Campaign>
</Campaigns>
'''

STORY_PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>A1h_Story</Lua_Script>
	<Active_Plot>A1h_Story.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''

STORY = '''<?xml version="1.0" ?>
<!-- a1h t09 probe: one event; its name is the StoryModeEvents key in A1h_Story.lua. -->
<Story>
	<Event Name="A1H_Begin">
		<Event_Type>STORY_ELAPSED</Event_Type>
		<Event_Param1>3</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_A1H_STORY_RUNNING</Reward_Param1>
		<Reward_Param2>15</Reward_Param2>
	</Event>
</Story>
'''

CASES = {
    'C': 'C (Empire starting infantry, Tatooine surface)',
    'A': 'A (Empire infantry spawned on Abregado Rae)',
    'B': 'B (Underworld AI infantry spawned on Byss)',
    'D': 'D (Empire ship + infantry spawned at Tatooine)',
}
VERDICTS = {
    'LAUNCHED': 'every unit is in the new fleet',
    'PARTIAL': 'only some units are in the fleet',
    'NOT_LAUNCHED': 'the units stayed out of the fleet',
    'NOFLEET': 'Assemble_Fleet gave no fleet',
    'NOUNITS': 'no units to assemble',
}
TEXTS = [
    ('TEXT_A1H_PROBE_NAME', 'A1H PROBE: ASSEMBLE FLEET'),
    ('TEXT_A1H_PROBE_DESC', 'a1h t09 probe. Play the EMPIRE and click nothing. After 3 s the story script assembles '
                            'fleets from ground units on four planets; wait for "A1H PROBE: done" (about 1 minute).'),
    ('TEXT_A1H_STORY_RUNNING', 'A1H PROBE: story plot loaded, the Assemble_Fleet cases start now'),
    ('TEXT_A1H_STORY_LUA', 'A1H PROBE: story Lua running'),
    ('TEXT_A1H_DONE', 'A1H PROBE: done. Take a screenshot of the galaxy map, then quit.'),
] + [('TEXT_A1H_%s_%s' % (c, v), 'A1H PROBE case %s: %s' % (d, t)) for c, d in CASES.items() for v, t in VERDICTS.items()]

CAMPAIGNFILES = 'DATA\\XML\\CAMPAIGNFILES.XML'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def story_lua():
    return open(os.path.join(HERE, 'a1h_story.lua'), encoding='ascii').read()


def build(game, out):
    gf = ted_owners.GameFiles(game)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an a1h probe folder' % out)
        shutil.rmtree(out)
    cf, src = gf.read(CAMPAIGNFILES)
    mt, src2 = gf.read(MASTERTEXT)
    if cf is None or mt is None:
        raise SystemExit('shipped CampaignFiles.xml / MasterTextFile_English.dat not found in ' + game)
    cft = cf.decode('latin-1')
    i = cft.index('</Campaign_Files>')
    cft = cft[:i] + '\t<File>A1h_Probe_Campaign.xml</File>\r\n' + cft[i:]
    write(os.path.join(out, 'Data', 'XML', 'CampaignFiles.xml'), cft.encode('latin-1'))
    write(os.path.join(out, 'Data', 'XML', 'A1h_Probe_Campaign.xml'), crlf(CAMPAIGN))
    write(os.path.join(out, 'Data', 'XML', 'A1h_Story_Plots.xml'), crlf(STORY_PLOTS))
    write(os.path.join(out, 'Data', 'XML', 'A1h_Story.xml'), crlf(STORY))
    write(os.path.join(out, 'Data', 'Scripts', 'Story', 'A1h_Story.lua'), crlf(story_lua()))
    tmp = os.path.join(out, 'mt.tmp')
    write(tmp, mt)
    keys = {k for k, _ in TEXTS}
    pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    os.remove(tmp)
    write(os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat'), dat_bytes(pairs + TEXTS))
    write(os.path.join(out, MARKER), b'a1h t09 Assemble_Fleet probe mod; removed by cleanup_a1h.cmd\r\n')
    print('built %s (CampaignFiles from %s, text from %s, %d probe strings)' % (out, src, src2, len(TEXTS)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    a = ap.parse_args()
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME))


if __name__ == '__main__':
    main()
