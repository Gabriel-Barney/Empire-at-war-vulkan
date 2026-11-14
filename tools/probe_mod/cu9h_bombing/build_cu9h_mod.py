#!/usr/bin/env python3
"""build_cu9h_mod.py - builds the cu9h bombing-run probe mod (roadmap group cu9h t02; docs/re/HUD_GAPS.md sec. 9.9)
from the installed game.

  build_cu9h_mod.py [--game DIR] [--out MODDIR] [--target tatooine|jabiim] [--interval-sec 60]
                    [--source-zip extracted\\foc_data_text.zip] [--no-text] [--config-out FILE]

One mod folder, Mods\\CU9HProbe (MODPATH=Mods\\CU9HProbe):
- Data\\XML\\CampaignFiles.xml (shipped + one line) and CU9H_Probe_Campaigns.xml: the campaign "CU9H PROBE: BOMBING RUN"
  (play the REBEL; story-driven) and the fallback "CU9H PROBE: BOMBING RUN (MANUAL)" (the same attack units start on
  Abregado Rae as Starting_Forces, no story: the driver invades by hand). Two planets: Abregado Rae (Rebel home, a barracks) and the target (Empire: a barracks and a
  stormtrooper squad; AI None everywhere). Target tatooine (default): _Land_Planet_Tatooine_02 (bright desert, open
  ground; the map the S10 probe invaded in retail); jabiim: _Land_Planet_Jabiim_02 (swamp).
- Data\\XML\\CU9H_Story_Plots.xml -> CU9H_Story.xml (event CU9H_Begin: STORY_ELAPSED 3 s, SCREEN_TEXT) and
  Data\\Scripts\\Story\\CU9H_Story.lua (cu9h_story.lua): spawns 2 Y-Wing squadrons + 2 infantry squads + a T2-B
  brigade for the Rebel on Abregado Rae, Assemble_Fleet, Move_To(target), Invade(): the land battle starts by itself
  with the Y-Wings (Ship_Class bomber, Land_Bomber_Type Y-Wing_Bombing_Run) in orbit, so the human attacker gets a
  bombing-run record (L1_S4_REMAINING sec. 3.7).
- Data\\XML\\GameConstants.xml (shipped, only Max_ / Min_Bombing_Run_Interval_Seconds changed to --interval-sec; 0 =
  leave the shipped 120 s): a shorter cooldown so the first run is ready sooner. The dial rules do not depend on it.
- Data\\Text\\MasterTextFile_English.dat (shipped + the probe strings).
Shipped files are read from the install (../common/ted_owners.py GameFiles) or, offline, from the text zip. The target's
land map must resolve in the install (a campaign battle without a map hangs retail on the load screen).
"""
import argparse
import json
import os
import re
import shutil
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'cu8g_probes'))
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
import build_cu8g_mod as g  # noqa: E402  (Source, crlf, write, texts_dat, CAMPAIGNFILES)

MOD_NAME = 'CU9HProbe'
MARKER = 'CU9H_PROBE_MARKER.txt'
DEFAULT_GAME = g.DEFAULT_GAME
GAMECONSTANTS = 'DATA\\XML\\GAMECONSTANTS.XML'
SOURCE = 'Abregado_Rae'
TARGETS = {
    'tatooine': {'planet': 'Tatooine', 'route': 'Abregado_Rae_Tatooine', 'map': '_Land_Planet_Tatooine_02'},
    'jabiim': {'planet': 'Jabiim', 'route': 'Abregado_Rae_Jabiim', 'map': '_Land_Planet_Jabiim_02'},
}
ATTACK = ['Y-Wing_Squadron', 'Y-Wing_Squadron', 'Rebel_Infantry_Squad', 'Rebel_Infantry_Squad',
          'Rebel_Light_Tank_Brigade']
DEFENSE = ['Imperial_Stormtrooper_Squad']

CAMPAIGN_FILE = '''<?xml version="1.0"?>
<Campaigns>
@@CAMPAIGNS@@
</Campaigns>
'''

# one campaign; @@NAME@@ / @@SET@@ / @@SORT@@ / @@TEXT@@ / @@STORY@@ / @@FORCES@@ / @@NOTE@@ per campaign
CAMPAIGN = r'''	<!-- cu9h t02 bombing-run probe (tools/probe_mod/cu9h_bombing): @@NOTE@@ -->
	<Campaign Name="@@NAME@@">
		<Campaign_Set> @@SET@@ </Campaign_Set>
		<Sort_Order>@@SORT@@</Sort_Order>
		<Text_ID> TEXT_@@TEXT@@_NAME </Text_ID>
		<Description_Text> TEXT_@@TEXT@@_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>
		<Locations>
				Galaxy_Core_Art_Model,
				Abregado_Rae,
				@@TARGET@@
		</Locations>
		<Trade_Routes>
				@@ROUTE@@
		</Trade_Routes>
		<Home_Location> Rebel, Abregado_Rae </Home_Location>
		<Home_Location> Empire, @@TARGET@@ </Home_Location>
		<Starting_Active_Player> Rebel </Starting_Active_Player>
@@STORY@@		<AI_Player_Control> Empire, None </AI_Player_Control>
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
		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 1 </Max_Tech_Level>
		<Starting_Forces> Rebel, Abregado_Rae, R_Ground_Barracks </Starting_Forces>
@@FORCES@@		<Starting_Forces> Empire, @@TARGET@@, E_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Empire, @@TARGET@@, Imperial_Stormtrooper_Squad </Starting_Forces>
	</Campaign>
'''

STORY_PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>CU9H_Story</Lua_Script>
	<Active_Plot>CU9H_Story.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''

STORY = '''<?xml version="1.0" ?>
<!-- cu9h t02 probe: one event; its name is the StoryModeEvents key in CU9H_Story.lua. -->
<Story>
	<Event Name="CU9H_Begin">
		<Event_Type>STORY_ELAPSED</Event_Type>
		<Event_Param1>3</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_CU9H_STORY_RUNNING</Reward_Param1>
		<Reward_Param2>15</Reward_Param2>
	</Event>
</Story>
'''


def texts(target):
    return [
        ('TEXT_CU9H_CAMP_NAME', 'CU9H PROBE: BOMBING RUN'),
        ('TEXT_CU9H_CAMP_DESC', 'cu9h t02 bombing-run probe. Play the REBEL. Click nothing on the galaxy: after 3 s your '
                                'fleet (2 Y-Wing squadrons and ground units) flies to %s and invades by itself. '
                                'Choose Begin (never auto-resolve). Never save.' % target),
        ('TEXT_CU9H_MANUAL_NAME', 'CU9H PROBE: BOMBING RUN (MANUAL)'),
        ('TEXT_CU9H_MANUAL_DESC', 'cu9h t02 fallback. Play the REBEL. Drag the ground units on Abregado Rae into the '
                                  'Y-Wing fleet, move it to %s, drop a ground unit on the surface, Begin. Never '
                                  'save.' % target),
        ('TEXT_CU9H_STORY_RUNNING', 'CU9H PROBE: story plot loaded; the invasion of %s starts now. Click nothing.' % target),
        ('TEXT_CU9H_LUA_MOVING', 'CU9H PROBE: fleet assembled, flying to %s.' % target),
        ('TEXT_CU9H_LUA_INVADE', 'CU9H PROBE: invasion ordered. Choose Begin.'),
        ('TEXT_CU9H_LUA_FAIL', 'CU9H PROBE: story script failed. Invade by hand (README).'),
        ('TEXT_CU9H_LUA_NOT_HUMAN', 'CU9H PROBE: the Rebel is not the human player. Restart and pick the REBEL.'),
    ]


def campaigns(tg):
    """Two campaign sets: the story-driven one (the default path) and a manual fallback with the same forces as
    Starting_Forces and no story (the driver invades by hand when the story does not start the battle)."""
    out = []
    for name, cset, sort, text, story, forces, note in (
            ('CU9H_Probe_Bombing', 'CU9H_Probe_Bombing_Set', 0, 'CU9H_CAMP',
             '\t\t<Rebel_Story_Name>CU9H_Story_Plots.xml</Rebel_Story_Name>\n', [],
             'the story Lua CU9H_Story makes the human Rebel invade %s 3 s after the start' % tg['planet']),
            ('CU9H_Probe_Bombing_Manual', 'CU9H_Probe_Bombing_Manual_Set', 1, 'CU9H_MANUAL', '', ATTACK,
             'manual fallback: the attack units start on Abregado_Rae, no story; invade %s by hand' % tg['planet'])):
        f = ''.join('\t\t<Starting_Forces> Rebel, Abregado_Rae, %s </Starting_Forces>\n' % u for u in forces)
        c = CAMPAIGN
        for k, v in (('@@NAME@@', name), ('@@SET@@', cset), ('@@SORT@@', str(sort)), ('@@TEXT@@', text),
                     ('@@STORY@@', story), ('@@FORCES@@', f), ('@@NOTE@@', note), ('@@TARGET@@', tg['planet']),
                     ('@@ROUTE@@', tg['route'])):
            c = c.replace(k, v)
        out.append(c)
    text = CAMPAIGN_FILE.replace('@@CAMPAIGNS@@', ''.join(out).rstrip('\n'))
    if '@@' in text:
        raise SystemExit('campaign template: unreplaced placeholder')
    return text


def prepare(out):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a cu9h probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    g.write(os.path.join(out, MARKER), b'cu9h probe mod (tools/probe_mod/cu9h_bombing); removed by cleanup_cu9h.cmd\r\n')


def lua_list(names):
    return ', '.join('"%s"' % n for n in names)


def story_lua(target):
    t = open(os.path.join(HERE, 'cu9h_story.lua'), encoding='ascii').read()
    for k, v in (('@@TARGET@@', target), ('@@SOURCE@@', SOURCE), ('@@ATTACK@@', lua_list(ATTACK)),
                 ('@@DEFENSE@@', lua_list(DEFENSE))):
        t = t.replace(k, v)
    if '@@' in t:
        raise SystemExit('cu9h_story.lua: unreplaced placeholder')
    return g.crlf(t)


def patch_constants(data, seconds):
    text = data.decode('latin-1')
    for tag in ('Max_Bombing_Run_Interval_Seconds', 'Min_Bombing_Run_Interval_Seconds'):
        text, n = re.subn(r'<%s>[^<]*</%s>' % (tag, tag), '<%s>%.1f</%s>' % (tag, float(seconds), tag), text)
        if n != 1:
            raise SystemExit('GameConstants.xml: expected one <%s>, found %d' % (tag, n))
    return text.encode('latin-1')


def build(game, out, target='tatooine', interval_sec=60, source_zip=None, no_text=False):
    if target not in TARGETS:
        raise SystemExit('unknown target ' + target)
    tg = TARGETS[target]
    src = g.Source(game, source_zip)
    map_note = 'not checked (offline build)'
    if not src.zip:
        data, where = src.read('DATA\\ART\\MAPS\\%s.TED' % tg['map'].upper())
        if data is None:
            raise SystemExit('the land map %s.ted of %s is not in the install: a campaign battle without a map hangs '
                             'retail' % (tg['map'], tg['planet']))
        map_note = '%s.ted found (%s, %d bytes)' % (tg['map'], where, len(data))
    prepare(out)
    files = []

    def put(rel, data):
        g.write(os.path.join(out, rel), data)
        files.append(rel)

    cf = src.need(g.CAMPAIGNFILES)[0].decode('latin-1')
    i = cf.index('</Campaign_Files>')
    put('Data\\XML\\CampaignFiles.xml', (cf[:i] + '\t<File>CU9H_Probe_Campaigns.xml</File>\r\n' + cf[i:]).encode('latin-1'))
    put('Data\\XML\\CU9H_Probe_Campaigns.xml', g.crlf(campaigns(tg)))
    put('Data\\XML\\CU9H_Story_Plots.xml', g.crlf(STORY_PLOTS))
    put('Data\\XML\\CU9H_Story.xml', g.crlf(STORY))
    put('Data\\Scripts\\Story\\CU9H_Story.lua', story_lua(tg['planet']))
    if interval_sec:
        put('Data\\XML\\GameConstants.xml', patch_constants(src.need(GAMECONSTANTS)[0], interval_sec))
    if not no_text:
        put('Data\\Text\\MasterTextFile_English.dat', g.texts_dat(src, texts(tg['planet'])))
    return {'mod': MOD_NAME, 'target': target, 'planet': tg['planet'], 'source': SOURCE, 'map': tg['map'],
            'map_check': map_note, 'interval_sec': interval_sec or 120, 'attack': ATTACK, 'defense': DEFENSE,
            'files': files}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--target', choices=sorted(TARGETS), default='tatooine')
    ap.add_argument('--interval-sec', type=int, default=60)
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--config-out')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.game, out, a.target, a.interval_sec, a.source_zip, a.no_text)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s: %s; map %s' % (out, ', '.join(cfg['files']), cfg['map_check']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
