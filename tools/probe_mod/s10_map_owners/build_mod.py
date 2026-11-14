#!/usr/bin/env python3
"""build_mod.py - builds the S10 map-owner probe mod (Mods\\S10MapOwners) from the installed game's own files.

  build_mod.py [--game DIR] [--out MODDIR] [--no-popup] [--expected-only]

Nothing from the game is stored in the repository: the shipped GameScoring.lua, CampaignFiles.xml and
MasterTextFile_English.dat are read from the install (loose files / MEGs, FoC file order) and patched:
  Data\\Scripts\\Miscellaneous\\GameScoring.lua   shipped script + s10_hook.lua appended (logging only)
  Data\\XML\\CampaignFiles.xml                    shipped list + S10_Probe_Campaign.xml
  Data\\XML\\S10_Probe_Campaign.xml               "S10 PROBE A: TATOOINE", "S10 PROBE B: JABIIM" (Underworld)
  Data\\XML\\S10_Story_Plots_<L>.xml, S10_Story_<L>.xml   own story plot per campaign (not the Gateways story)
  Data\\Scripts\\Story\\S10_Story_<L>.lua          from s10_story.lua: spawns both sides, then invades by itself
  Data\\Text\\MasterTextFile_English.dat          shipped text + TEXTS
  S10_PROBE_MARKER.txt                            lets cleanup recognise the folder as ours
Also writes expected_objects.json (map placements of the maps.json scenarios) next to this script.
"""
import argparse
import json
import os
import shutil
import sys

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MARKER = 'S10_PROBE_MARKER.txt'

# Two campaign sets, one per invasion target, so the driver never picks a planet: the plot's Lua (s10_story.lua)
# spawns both sides and starts the invasion by itself. Human = the defender (or Underworld as fallback, see
# campaign_xml). No space units or starbases anywhere.
# The 2026-10-29 run named Story_Plots_Sandbox_27_Underworld.xml (the shipped "Gateways" story: objectives window,
# heroes) and its Starting_Forces units were not seen; the Lua spawn no longer depends on Starting_Forces.
PROBES = [
    # label, target, source, defender, defense units, map
    ('A', 'Tatooine', 'Byss', 'Empire', ['Imperial_Stormtrooper_Squad', 'Imperial_Stormtrooper_Squad'],
     '_Land_Planet_Tatooine_02'),
    ('B', 'Jabiim', 'Abregado_Rae', 'Rebel', ['Rebel_Infantry_Squad', 'Rebel_Infantry_Squad'], '_Land_Planet_Jabiim_02'),
]

CAMPAIGN = r'''
	<!-- S10 map-owner probe @@L@@, human @@HUMAN@@ (tools/probe_mod/s10_map_owners): the story Lua S10_Story_@@L@@
	     makes Underworld invade @@TARGET@@ (@@DEFENDER@@, map @@MAP@@) from @@SOURCE@@ 3 s after the start. -->
	<Campaign Name="S10_Probe_@@L@@_@@HUMAN@@">
		<Campaign_Set> S10_Probe_Set_@@L@@ </Campaign_Set>
		<Sort_Order>@@SORT@@</Sort_Order>
		<Text_ID> TEXT_S10_PROBE_NAME_@@L@@ </Text_ID>
		<Description_Text> TEXT_S10_PROBE_DESC_@@L@@ </Description_Text>
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

		<Starting_Active_Player> @@HUMAN@@ </Starting_Active_Player>

		<@@HUMAN@@_Story_Name>S10_Story_Plots_@@L@@.xml</@@HUMAN@@_Story_Name>

@@AI@@
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
		<Starting_Forces> Rebel, Jabiim, Rebel_Infantry_Squad </Starting_Forces>

		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 1 </Max_Tech_Level>
		<Starting_Forces> Empire, Tatooine, E_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Empire, Tatooine, Imperial_Stormtrooper_Squad </Starting_Forces>

		<Starting_Credits> Underworld, 1000 </Starting_Credits>
		<Starting_Tech_Level> Underworld, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Underworld, 1 </Max_Tech_Level>
		<Starting_Forces> Underworld, Byss, U_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Underworld, Abregado_Rae, U_Ground_Barracks </Starting_Forces>
	</Campaign>
'''

STORY_PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>S10_Story_@@L@@</Lua_Script>
	<Active_Plot>S10_Story_@@L@@.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''

STORY = '''<?xml version="1.0" ?>
<!-- S10 map-owner probe @@L@@: one event; its name is the StoryModeEvents key in S10_Story_@@L@@.lua. -->
<Story>
	<Event Name="S10_Begin">
		<Event_Type>STORY_ELAPSED</Event_Type>
		<Event_Param1>3</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_S10_STORY_RUNNING</Reward_Param1>
		<Reward_Param2>15</Reward_Param2>
	</Event>
</Story>
'''

TEXTS = [
    ('TEXT_S10_PROBE_NAME_A', 'S10 PROBE A: TATOOINE'),
    ('TEXT_S10_PROBE_DESC_A', 'Map-owner probe A. Play the EMPIRE. Underworld invades Tatooine from Byss by itself '
                              'after 3 s; fight the land battle, give no orders, wait 60 s.'),
    ('TEXT_S10_PROBE_NAME_B', 'S10 PROBE B: JABIIM'),
    ('TEXT_S10_PROBE_DESC_B', 'Map-owner probe B. Play the REBELS. Underworld invades Jabiim from Abregado Rae by '
                              'itself after 3 s; fight the land battle, give no orders, wait 60 s.'),
    ('TEXT_S10_STORY_RUNNING', 'S10 PROBE: story plot loaded, the invasion starts now'),
    ('TEXT_S10_STORY_LUA', 'S10 PROBE: story Lua running'),
    ('TEXT_S10_HOOK_ALIVE', 'S10 PROBE: GameScoring hook running in this battle'),
]


AI_PLAYERS = {'Empire': 'BasicEmpire', 'Rebel': 'BasicRebel', 'Underworld': 'None'}  # Underworld: driven by the story Lua


def subst(text, probe, sort=0, human='Underworld'):
    label, target, source, defender, defense, mp = probe
    ai = '\n'.join('\t\t<AI_Player_Control> %s, %s </AI_Player_Control>' % (f, v) for f, v in AI_PLAYERS.items()
                   if f != human)
    for k, v in (('@@HUMAN@@', human), ('@@AI@@', ai), ('@@L@@', label), ('@@LABEL@@', label), ('@@TARGET@@', target),
                 ('@@SOURCE@@', source),
                 ('@@DEFENDER@@', defender), ('@@MAP@@', mp), ('@@SORT@@', str(sort)),
                 ('@@DEFENSE@@', ', '.join('"%s"' % u for u in defense))):
        text = text.replace(k, v)
    assert '@@' not in text, text
    return text


def campaign_xml():
    """Per probe one campaign set with two campaigns: the defender as the human (preferred: the AI-controlled Underworld
    fleet invades the human's planet, the pattern of the shipped Story_Campaign_Underworld.lua) and Underworld as the
    human (fallback: the human's own fleet gets Invade() from the script; if no battle starts the driver invades)."""
    out = []
    for i, p in enumerate(PROBES):
        for human in (p[3], 'Underworld'):
            out.append(subst(CAMPAIGN, p, i, human))
    return '<?xml version="1.0"?>\n<Campaigns>\n' + ''.join(out) + '\n</Campaigns>\n'


def story_lua(probe):
    return subst(open(os.path.join(HERE, 's10_story.lua'), encoding='ascii').read(), probe)


GAMESCORING = 'DATA\\SCRIPTS\\MISCELLANEOUS\\GAMESCORING.LUA'
CAMPAIGNFILES = 'DATA\\XML\\CAMPAIGNFILES.XML'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'


def probe_types(expected):
    """Type names worth a Find_All_Objects_Of_Type(name) query: every non-neutral-owned placement and every
    reinforcement / capture point of the scenario maps (cross-check of the per-player enumeration)."""
    names = set()
    for m in expected.values():
        for r in m['objects']:
            if r['type'].startswith('?') or r['kind'] == 'marker':
                continue  # unresolved CRC, or a marker
            if r['owner'] != 3 or r['kind'] in ('reinforcement_point', 'capture_point'):
                names.add(r['type'])
    return sorted(names, key=str.upper)


def hook_text(types, popup):
    src = open(os.path.join(HERE, 's10_hook.lua'), encoding='ascii').read()
    lines = ''.join('\n\t"%s",' % t for t in types)
    src = src.replace('S10_TYPES = { --@@TYPES@@\n}', 'S10_TYPES = {' + lines + '\n}')
    src = src.replace('S10_POPUP = true --@@POPUP@@', 'S10_POPUP = %s' % ('true' if popup else 'false'))
    assert '@@' not in src
    return src


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def build_expected(game):
    gf, types = ted_owners.GameFiles(game), ted_owners.load_types()
    cfg = json.load(open(os.path.join(HERE, 'maps.json')))
    exp = ted_owners.expected(gf, types, sorted({s['map'] for s in cfg['scenarios']}))
    with open(os.path.join(HERE, 'expected_objects.json'), 'w') as f:
        f.write('{\n' + ',\n'.join('%s: {"source": %s, "objects": [\n%s]}' % (
            json.dumps(m), json.dumps(d['source']), ',\n'.join(json.dumps(r, separators=(',', ':')) for r in d['objects']))
            for m, d in exp.items()) + '\n}\n')
    return gf, exp


def build(game, out, popup):
    gf, exp = build_expected(game)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an S10 probe folder' % out)
        shutil.rmtree(out)
    gs, src = gf.read(GAMESCORING)
    cf, src2 = gf.read(CAMPAIGNFILES)
    if gs is None or cf is None:
        raise SystemExit('shipped GameScoring.lua / CampaignFiles.xml not found in ' + game)
    text = gs.decode('latin-1').replace('\r\n', '\n')
    write(os.path.join(out, 'Data', 'Scripts', 'Miscellaneous', 'GameScoring.lua'),
          (text + '\n' + hook_text(probe_types(exp), popup)).replace('\n', '\r\n').encode('latin-1'))
    cft = cf.decode('latin-1')
    i = cft.index('</Campaign_Files>')
    cft = cft[:i] + '\t<File>S10_Probe_Campaign.xml</File>\r\n' + cft[i:]
    write(os.path.join(out, 'Data', 'XML', 'CampaignFiles.xml'), cft.encode('latin-1'))
    write(os.path.join(out, 'Data', 'XML', 'S10_Probe_Campaign.xml'), crlf(campaign_xml()))
    for p in PROBES:
        write(os.path.join(out, 'Data', 'XML', 'S10_Story_Plots_%s.xml' % p[0]), crlf(subst(STORY_PLOTS, p)))
        write(os.path.join(out, 'Data', 'XML', 'S10_Story_%s.xml' % p[0]), crlf(subst(STORY, p)))
        write(os.path.join(out, 'Data', 'Scripts', 'Story', 'S10_Story_%s.lua' % p[0]), crlf(story_lua(p)))
    mt, src3 = gf.read(MASTERTEXT)
    if mt is None:
        raise SystemExit('shipped MasterTextFile_English.dat not found in ' + game)
    tmp = os.path.join(out, 'mt.tmp')
    write(tmp, mt)
    keys = {k for k, _ in TEXTS}
    pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    os.remove(tmp)
    write(os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat'), dat_bytes(pairs + TEXTS))
    write(os.path.join(out, MARKER), b'S10 map-owner probe mod; removed by cleanup_s10.cmd\r\n')
    print('built %s (GameScoring from %s, CampaignFiles from %s, text from %s, %d probe types, popup=%s)' % (
        out, src, src2, src3, len(probe_types(exp)), popup))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\S10MapOwners)')
    ap.add_argument('--no-popup', action='store_true', help='no _MessagePopup from the GameScoring hook')
    ap.add_argument('--popup', action='store_true', help=argparse.SUPPRESS)  # old flag; popups are the default now
    ap.add_argument('--expected-only', action='store_true')
    a = ap.parse_args()
    if a.expected_only:
        build_expected(a.game)
        print('wrote', os.path.join(HERE, 'expected_objects.json'))
        return
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', 'S10MapOwners'), not a.no_popup)


if __name__ == '__main__':
    main()
